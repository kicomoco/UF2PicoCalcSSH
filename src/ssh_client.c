#include "ssh_client.h"
#include "network.h"
#include "storage.h"
#include "terminal.h"
#include "wolfssh/ssh.h"
#include "wolfssl/wolfcrypt/asn_public.h"
#include "wolfssl/wolfcrypt/ecc.h"
#include "wolfssl/wolfcrypt/rsa.h"
#include "wolfssl/wolfcrypt/sha256.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef SSH_HOST_TEST
#include <time.h>
#else
#include "pico/stdlib.h"
#endif

static WOLFSSH_CTX *ctx;
static WOLFSSH *session;
static ssh_profile_t profile;
static char password[128], message[128] = "Not connected";
static host_check_fn verify_host;
static enum { SSH_IDLE, SSH_TCP, SSH_HANDSHAKE, SSH_RESIZE, SSH_OPEN } state;
static uint8_t *private_key, *public_key;
static word32 private_size, public_size, key_type_size;
static const byte *key_type;
static uint32_t deadline;
static uint8_t tx[2048];
static unsigned tx_size;
static bool started, key_attempted, password_attempted;

static uint32_t now_ms(void) {
#ifdef SSH_HOST_TEST
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint32_t)(t.tv_sec * 1000 + t.tv_nsec / 1000000);
#else
    return to_ms_since_boot(get_absolute_time());
#endif
}

static void release_keys(void) {
    if (private_key) { secure_clear(private_key, private_size); free(private_key); }
    free(public_key);
    private_key = public_key = NULL;
    private_size = public_size = 0;
}

void ssh_stop(void) {
    if (session) wolfSSH_free(session);
    if (ctx) wolfSSH_CTX_free(ctx);
    session = NULL;
    ctx = NULL;
    release_keys();
    transport_close();
    secure_clear(password, sizeof(password));
    secure_clear(tx, sizeof(tx));
    tx_size = 0;
    state = SSH_IDLE;
}

static bool fail(const char *text, int code) {
    ssh_stop();
    snprintf(message, sizeof(message), "%s (%d)", text, code);
    return false;
}

static int io_read(WOLFSSH *ssh, void *data, word32 size, void *arg) {
    (void)ssh; (void)arg;
    int n = transport_read(data, size);
    return n > 0 ? n : n == 0 ? WS_CBIO_ERR_WANT_READ : WS_CBIO_ERR_CONN_CLOSE;
}

static int io_write(WOLFSSH *ssh, void *data, word32 size, void *arg) {
    (void)ssh; (void)arg;
    int n = transport_write(data, size);
    return n > 0 ? n : n == 0 ? WS_CBIO_ERR_WANT_WRITE : WS_CBIO_ERR_CONN_CLOSE;
}

static int host_check(const byte *key, word32 size, void *arg) {
    (void)arg;
    uint8_t hash[32];
    uint16_t port;
    if (wc_Sha256Hash(key, size, hash) != 0 || !parse_port(profile.port, &port)) return -1;
    return verify_host && verify_host(profile.host, port, hash) ? 0 : -1;
}

static int authenticate(byte type, WS_UserAuthData *data, void *arg) {
    (void)arg;
    if (type == WOLFSSH_USERAUTH_PUBLICKEY && private_key) {
        if (key_attempted) return WOLFSSH_USERAUTH_FAILURE;
        key_attempted = true;
        data->sf.publicKey.publicKeyType = key_type;
        data->sf.publicKey.publicKeyTypeSz = key_type_size;
        data->sf.publicKey.publicKey = public_key;
        data->sf.publicKey.publicKeySz = public_size;
        data->sf.publicKey.privateKey = private_key;
        data->sf.publicKey.privateKeySz = private_size;
        return WOLFSSH_USERAUTH_SUCCESS;
    }
    if (type == WOLFSSH_USERAUTH_PASSWORD) {
        if (private_key && !key_attempted) return WOLFSSH_USERAUTH_FAILURE;
        if (password_attempted) return WOLFSSH_USERAUTH_FAILURE;
        password_attempted = true;
        data->sf.password.password = (byte *)password;
        data->sf.password.passwordSz = (word32)strlen(password);
        return WOLFSSH_USERAUTH_SUCCESS;
    }
    return WOLFSSH_USERAUTH_INVALID_AUTHTYPE;
}

static void put_string(uint8_t *buffer, unsigned *index, const void *value, unsigned size) {
    buffer[(*index)++] = (uint8_t)(size >> 24);
    buffer[(*index)++] = (uint8_t)(size >> 16);
    buffer[(*index)++] = (uint8_t)(size >> 8);
    buffer[(*index)++] = (uint8_t)size;
    memcpy(buffer + *index, value, size);
    *index += size;
}

static bool load_key(const char *path) {
    uint8_t *file;
    size_t size;
    if (!storage_read_key(path, &file, &size)) {
        snprintf(message, sizeof(message), "%s", storage_error());
        return false;
    }
    int format = strstr((char *)file, "-----BEGIN ") ? WOLFSSH_FORMAT_PEM : WOLFSSH_FORMAT_ASN1;
    int r = wolfSSH_ReadKey_buffer(file, (word32)size, format, &private_key,
                                   &private_size, &key_type, &key_type_size, NULL);
    secure_clear(file, size);
    free(file);
    if (r != WS_SUCCESS) return fail("Key must be unencrypted RSA/EC PEM or DER", r);
    public_key = malloc(1100);
    if (!public_key) return fail("No memory for public key", -1);
    unsigned index = 0;
    word32 offset = 0;
    if (!strcmp((const char *)key_type, "ssh-rsa")) {
        RsaKey rsa;
        r = wc_InitRsaKey(&rsa, NULL);
        if (r) return fail("RSA initialization failed", r);
        r = wc_RsaPrivateKeyDecode(private_key, &offset, &rsa, private_size);
        byte e[9], n[513];
        word32 e_size = sizeof(e) - 1, n_size = sizeof(n) - 1;
        if (!r) r = wc_RsaFlattenPublicKey(&rsa, e + 1, &e_size, n + 1, &n_size);
        wc_FreeRsaKey(&rsa);
        if (r) return fail("RSA key decode failed (max 4096 bits)", r);
        put_string(public_key, &index, "ssh-rsa", 7);
        e[0] = 0;
        bool e_padding = (e[1] & 0x80) != 0;
        put_string(public_key, &index, e + (e_padding ? 0 : 1), e_size + (e_padding ? 1 : 0));
        n[0] = 0;
        bool padding = (n[1] & 0x80) != 0;
        put_string(public_key, &index, n + (padding ? 0 : 1), n_size + (padding ? 1 : 0));
    } else if (!strcmp((const char *)key_type, "ecdsa-sha2-nistp256")) {
        ecc_key ecc;
        r = wc_ecc_init(&ecc);
        if (r) return fail("EC initialization failed", r);
        r = wc_EccPrivateKeyDecode(private_key, &offset, &ecc, private_size);
        byte q[65];
        word32 q_size = sizeof(q);
        if (!r) r = wc_ecc_export_x963(&ecc, q, &q_size);
        wc_ecc_free(&ecc);
        if (r) return fail("EC key must use NIST P-256", r);
        put_string(public_key, &index, key_type, key_type_size);
        put_string(public_key, &index, "nistp256", 8);
        put_string(public_key, &index, q, q_size);
    } else return fail("Only RSA and P-256 PEM/DER private keys supported", -1);
    public_size = index;
    return true;
}

bool ssh_start(const ssh_profile_t *p, const char *pw, host_check_fn verify) {
    ssh_stop();
    if (!valid_ssh(p)) return fail("Host, port 1..65535 and username required", -1);
    if (!wifi_ready()) return fail("Connect WiFi first", -1);
    if (strlen(pw) >= sizeof(password)) return fail("Password too long", -1);
    if (!started) {
        int r = wolfSSH_Init();
        if (r) return fail("SSH initialization failed", r);
        started = true;
    }
    profile = *p;
    key_attempted = false;
    password_attempted = false;
    memcpy(password, pw, strlen(pw) + 1);
    verify_host = verify;
    if (p->key_path[0] && !load_key(p->key_path)) { ssh_stop(); return false; }
    ctx = wolfSSH_CTX_new(WOLFSSH_ENDPOINT_CLIENT, NULL);
    if (!ctx) return fail("No memory for SSH context", -1);
    wolfSSH_SetIORecv(ctx, io_read);
    wolfSSH_SetIOSend(ctx, io_write);
    wolfSSH_SetUserAuth(ctx, authenticate);
    wolfSSH_CTX_SetPublicKeyCheck(ctx, host_check);
    /* No obsolete SHA-1 key exchange, host signatures or CBC ciphers. */
    if (wolfSSH_CTX_SetAlgoListKex(ctx, "curve25519-sha256,ecdh-sha2-nistp256") ||
        wolfSSH_CTX_SetAlgoListKey(ctx, "ssh-ed25519,ecdsa-sha2-nistp256,rsa-sha2-512,rsa-sha2-256") ||
        wolfSSH_CTX_SetAlgoListCipher(ctx, "aes128-ctr,aes256-ctr") ||
        wolfSSH_CTX_SetAlgoListMac(ctx, "hmac-sha2-256") ||
        wolfSSH_CTX_SetWindowPacketSize(ctx, 32768, 16384))
        return fail("SSH algorithm configuration failed", -1);
    session = wolfSSH_new(ctx);
    if (!session) return fail("No memory for SSH session", -1);
    if (wolfSSH_SetUsername(session, p->username) ||
        wolfSSH_SetChannelType(session, WOLFSSH_SESSION_TERMINAL, NULL, 0))
        return fail("SSH session configuration failed", -1);
    uint16_t port;
    if (!parse_port(p->port, &port)) return fail("Invalid port", -1);
    if (!transport_connect(p->host, port)) return fail(transport_error(), -1);
    state = SSH_TCP;
    snprintf(message, sizeof(message), "Resolving host / connecting TCP...");
    return true;
}

bool ssh_active(void) { return state != SSH_IDLE; }
bool ssh_connected(void) { return state == SSH_OPEN; }
const char *ssh_status(void) { return message; }

static bool retry(int error) {
    return error == WS_WANT_READ || error == WS_WANT_WRITE || error == WS_REKEYING;
}

void ssh_poll(void) {
    if (state == SSH_IDLE) return;
    if (transport_failed() || !wifi_ready()) {
        fail(transport_failed() ? transport_error() : "WiFi disconnected", -1);
        return;
    }
    if (state == SSH_TCP) {
        if (!transport_ready()) return;
        state = SSH_HANDSHAKE;
        deadline = now_ms() + 90000;
        snprintf(message, sizeof(message), "SSH handshake / authentication...");
    }
    if (state == SSH_HANDSHAKE) {
        int r = wolfSSH_connect(session);
        if (r == WS_SUCCESS) {
            state = SSH_RESIZE;
            secure_clear(password, sizeof(password));
            release_keys();
        } else {
            int error = wolfSSH_get_error(session);
            if (!retry(error)) fail("SSH handshake/authentication failed", error);
            else if ((int32_t)(now_ms() - deadline) >= 0) fail("SSH handshake timed out", error);
            return;
        }
    }
    if (state == SSH_RESIZE) {
        int r = wolfSSH_ChangeTerminalSize(session, TERM_COLS, TERM_ROWS, 318, 288);
        if (r == WS_SUCCESS) {
            state = SSH_OPEN;
            snprintf(message, sizeof(message), "Connected");
        } else {
            int error = wolfSSH_get_error(session);
            if (!retry(error)) fail("Terminal size request failed", error);
            else if ((int32_t)(now_ms() - deadline) >= 0) fail("Terminal size request timed out", error);
        }
    }
    if (state == SSH_OPEN && tx_size) {
        int n = wolfSSH_stream_send(session, tx, tx_size);
        if (n > 0) {
            memmove(tx, tx + n, tx_size - (unsigned)n);
            tx_size -= (unsigned)n;
        } else {
            int error = wolfSSH_get_error(session);
            if (!retry(error)) fail("SSH send failed", error);
        }
    }
}

bool ssh_send(const void *data, unsigned size) {
    if (!ssh_connected() || size > sizeof(tx) - tx_size) return false;
    memcpy(tx + tx_size, data, size);
    tx_size += size;
    return true;
}

int ssh_read(void *data, unsigned size) {
    if (!ssh_connected()) return 0;
    int n = wolfSSH_stream_read(session, data, size);
    if (n > 0) return n;
    int error = wolfSSH_get_error(session);
    if (error == WS_EXTDATA) {
        n = wolfSSH_extended_data_read(session, data, size);
        if (n >= 0) return n;
        error = wolfSSH_get_error(session);
    }
    if (retry(error)) return 0;
    if (error == WS_EOF || error == WS_CHANNEL_CLOSED) fail("Remote session closed", error);
    else fail("SSH receive failed", error);
    return -1;
}

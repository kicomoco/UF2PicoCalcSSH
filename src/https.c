#include "https.h"
#include "ca_roots.h"
#include "network.h"
#include "pico/stdlib.h"
#include <wolfssl/wolfcrypt/settings.h>
#include <wolfssl/ssl.h>
#include <wolfssl/error-ssl.h>
#include <wolfssl/wolfcrypt/error-crypt.h>
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#ifndef APP_VERSION
#define APP_VERSION "dev"
#endif

#define MAX_REDIRECTS 5
#define IDLE_TIMEOUT_MS 20000

static enum { S_IDLE, S_CONNECT, S_HANDSHAKE, S_REQUEST, S_HEADERS, S_BODY, S_DONE, S_FAILED } state;
static enum { CH_SIZE, CH_DATA, CH_DATA_END, CH_TRAILER } chunk_state;
static WOLFSSL_CTX *ctx;
static WOLFSSL *ssl;
static char url[HTTPS_URL_LEN], location[HTTPS_URL_LEN], host[128];
static const char *path;
static uint16_t port;
static char request[HTTPS_URL_LEN + 256];
static size_t request_len;
static char line[HTTPS_URL_LEN + 64];
static size_t line_len;
static bool line_overflow, status_seen, chunked, have_length;
static int status_code;
static size_t content_length, received, max_body, chunk_left;
static unsigned redirects;
static https_sink_fn sink;
static void *sink_ctx;
static char error[96];
static uint32_t last_activity;

static uint32_t now_ms(void) { return to_ms_since_boot(get_absolute_time()); }

static int io_recv(WOLFSSL *s, char *buf, int size, void *c) {
    (void)s; (void)c;
    int n = transport_read(buf, (unsigned)size);
    if (n > 0) return n;
    return n == 0 ? WOLFSSL_CBIO_ERR_WANT_READ : WOLFSSL_CBIO_ERR_CONN_CLOSE;
}

static int io_send(WOLFSSL *s, char *buf, int size, void *c) {
    (void)s; (void)c;
    int n = transport_write(buf, (unsigned)size);
    if (n > 0) return n;
    return n == 0 ? WOLFSSL_CBIO_ERR_WANT_WRITE : WOLFSSL_CBIO_ERR_CONN_CLOSE;
}

/* Without an RTC the clock may be behind real time, so a certificate that is
 * "not yet valid" is accepted; signatures, expiry and host name are still enforced. */
static int verify_cb(int preverify, WOLFSSL_X509_STORE_CTX *store) {
    if (!preverify && store && store->error == ASN_BEFORE_DATE_E) return 1;
    return preverify;
}

static bool init_ctx(void) {
    if (ctx) return true;
    if (wolfSSL_Init() != WOLFSSL_SUCCESS) return false;
    ctx = wolfSSL_CTX_new(wolfSSLv23_client_method());
    if (!ctx) return false;
    wolfSSL_CTX_SetIORecv(ctx, io_recv);
    wolfSSL_CTX_SetIOSend(ctx, io_send);
    wolfSSL_CTX_set_verify(ctx, WOLFSSL_VERIFY_PEER, verify_cb);
    unsigned loaded = 0;
    for (unsigned i = 0; i < ca_root_count; i++)
        if (wolfSSL_CTX_load_verify_buffer(ctx, ca_roots[i].der, (long)ca_roots[i].size,
                                           WOLFSSL_FILETYPE_ASN1) == WOLFSSL_SUCCESS) loaded++;
    if (loaded != ca_root_count) {
        wolfSSL_CTX_free(ctx);
        ctx = NULL;
        return false;
    }
    return true;
}

static void close_connection(void) {
    if (ssl) {
        wolfSSL_free(ssl);
        ssl = NULL;
    }
    transport_close();
}

static bool fail(const char *message) {
    close_connection();
    snprintf(error, sizeof(error), "%s", message);
    state = S_FAILED;
    return false;
}

static bool fail_tls(int code) {
    const char *reason;
    switch (code) {
    case ASN_NO_SIGNER_E: reason = "certificate not trusted"; break;
    case ASN_AFTER_DATE_E: reason = "certificate expired"; break;
    case DOMAIN_NAME_MISMATCH: reason = "certificate name mismatch"; break;
    case ASN_SIG_CONFIRM_E: reason = "bad certificate signature"; break;
    case SOCKET_PEER_CLOSED_E: case SOCKET_ERROR_E: reason = "connection closed"; break;
    default: reason = "TLS error"; break;
    }
    char message[sizeof(error)];
    snprintf(message, sizeof(message), "HTTPS %s (%d)", reason, code);
    return fail(transport_failed() ? transport_error() : message);
}

static bool parse_url(void) {
    if (strncmp(url, "https://", 8)) return false;
    const char *h = url + 8;
    size_t n = strcspn(h, "/:?#");
    if (!n || n >= sizeof(host)) return false;
    for (size_t i = 0; i < n; i++)
        if (!isalnum((unsigned char)h[i]) && h[i] != '-' && h[i] != '.') return false;
    memcpy(host, h, n);
    host[n] = 0;
    const char *p = h + n;
    port = 443;
    if (*p == ':') {
        unsigned value = 0;
        for (p++; isdigit((unsigned char)*p) && value <= 65535; p++) value = value * 10 + (unsigned)(*p - '0');
        if (!value || value > 65535) return false;
        port = (uint16_t)value;
    }
    if (!*p) path = "/";
    else if (*p == '/') path = p;
    else return false;
    for (const char *c = path; *c; c++)
        if ((unsigned char)*c <= ' ' || *c == 0x7f) return false;
    return true;
}

static bool start(void) {
    close_connection();
    if (!parse_url()) return fail("Bad or unsupported download URL");
    status_seen = chunked = have_length = line_overflow = false;
    status_code = 0;
    content_length = received = line_len = 0;
    chunk_state = CH_SIZE;
    location[0] = 0;
    char host_header[sizeof(host) + 8];
    if (port == 443) snprintf(host_header, sizeof(host_header), "%s", host);
    else snprintf(host_header, sizeof(host_header), "%s:%u", host, port);
    int n = snprintf(request, sizeof(request),
                     "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: PicoCalcSSH/" APP_VERSION
                     "\r\nAccept: */*\r\nConnection: close\r\n\r\n", path, host_header);
    if (n < 0 || (size_t)n >= sizeof(request)) return fail("Download URL too long");
    request_len = (size_t)n;
    last_activity = now_ms();
    if (!transport_connect(host, port)) return fail(transport_error());
    state = S_CONNECT;
    return true;
}

static void finish(void) {
    close_connection();
    state = S_DONE;
}

static bool deliver(const uint8_t *data, size_t n) {
    if (!n) return true;
    if (n > max_body - received) return fail("Download larger than expected");
    if (!sink(sink_ctx, data, n)) {
        if (state != S_FAILED) fail("Download stopped");
        return false;
    }
    received += n;
    return true;
}

static bool headers_done(void) {
    if (status_code >= 300 && status_code < 400 && status_code != 304) {
        if (!location[0]) return fail("Redirect without a location");
        if (++redirects > MAX_REDIRECTS) return fail("Too many redirects");
        if (location[0] == '/') {
            char base[sizeof(host) + 16];
            snprintf(base, sizeof(base), "https://%s", host);
            size_t base_len = strlen(base), location_len = strlen(location);
            if (base_len + location_len >= sizeof(url)) return fail("Redirect URL too long");
            memcpy(url, base, base_len);
            memcpy(url + base_len, location, location_len + 1);
        } else snprintf(url, sizeof(url), "%s", location);
        return start();
    }
    if (status_code != 200) {
        char message[48];
        snprintf(message, sizeof(message), "Server returned HTTP %d", status_code);
        return fail(message);
    }
    if (chunked) have_length = false;
    if (have_length && content_length > max_body) return fail("Download larger than expected");
    state = S_BODY;
    line_len = 0;
    if (have_length && !content_length) finish();
    return true;
}

static bool header_line(void) {
    if (!status_seen) {
        if (line_overflow || strncmp(line, "HTTP/1.", 7) || line_len < 12 || line[8] != ' ')
            return fail("Bad HTTP response");
        status_code = (line[9] - '0') * 100 + (line[10] - '0') * 10 + (line[11] - '0');
        status_seen = true;
        return true;
    }
    if (!line_len) return headers_done();
    char *colon = strchr(line, ':');
    if (!colon) return true;
    *colon = 0;
    char *value = colon + 1;
    while (*value == ' ' || *value == '\t') value++;
    size_t length = strlen(value);
    while (length && (value[length - 1] == ' ' || value[length - 1] == '\t')) value[--length] = 0;
    if (!strcasecmp(line, "location")) {
        if (line_overflow) return fail("Redirect URL too long");
        snprintf(location, sizeof(location), "%s", value);
    } else if (!strcasecmp(line, "content-length")) {
        size_t v = 0;
        if (!*value) return fail("Bad content length");
        for (char *c = value; *c; c++) {
            if (!isdigit((unsigned char)*c) || v > (SIZE_MAX - 9) / 10) return fail("Bad content length");
            v = v * 10 + (size_t)(*c - '0');
        }
        content_length = v;
        have_length = true;
    } else if (!strcasecmp(line, "transfer-encoding")) {
        chunked = !strcasecmp(value, "chunked");
        if (!chunked) return fail("Unsupported transfer encoding");
    }
    return true;
}

/* Collects a CRLF-terminated line; returns true when one is complete. */
static bool take_line(uint8_t c) {
    if (c == '\n') {
        if (line_len && line[line_len - 1] == '\r') line_len--;
        line[line_len] = 0;
        return true;
    }
    if (line_len < sizeof(line) - 1) line[line_len++] = (char)c;
    else line_overflow = true;
    return false;
}

static bool chunked_body(const uint8_t *d, size_t n) {
    while (n && state == S_BODY) {
        if (chunk_state == CH_DATA) {
            size_t m = n < chunk_left ? n : chunk_left;
            if (!deliver(d, m)) return false;
            d += m; n -= m; chunk_left -= m;
            if (!chunk_left) chunk_state = CH_DATA_END;
            continue;
        }
        uint8_t c = *d++;
        n--;
        if (chunk_state == CH_DATA_END) {
            if (c == '\n') chunk_state = CH_SIZE;
            else if (c != '\r') return fail("Bad chunked response");
            continue;
        }
        if (!take_line(c)) continue;
        if (line_overflow) return fail("Bad chunked response");
        if (chunk_state == CH_TRAILER) {
            if (!line_len) finish();
        } else {
            size_t size = 0;
            const char *p = line;
            if (!isxdigit((unsigned char)*p)) return fail("Bad chunked response");
            for (; isxdigit((unsigned char)*p); p++) {
                if (size > (SIZE_MAX >> 4)) return fail("Bad chunked response");
                size = size * 16 + (size_t)(isdigit((unsigned char)*p) ? *p - '0' : (tolower((unsigned char)*p) - 'a' + 10));
            }
            chunk_left = size;
            chunk_state = size ? CH_DATA : CH_TRAILER;
        }
        line_len = 0;
    }
    return state != S_FAILED;
}

static bool consume(const uint8_t *d, size_t n) {
    while (n && state == S_HEADERS) {
        uint8_t c = *d++;
        n--;
        if (!take_line(c)) continue;
        if (!header_line()) return false;
        line_len = 0;
        line_overflow = false;
    }
    if (!n || state != S_BODY) return state != S_FAILED;
    if (chunked) return chunked_body(d, n);
    if (have_length && n > content_length - received) n = content_length - received;
    if (!deliver(d, n)) return false;
    if (have_length && received == content_length) finish();
    return true;
}

bool https_get(const char *target, size_t limit, https_sink_fn body_sink, void *context) {
    https_cancel();
    if (!init_ctx()) return fail("TLS setup failed");
    if (strlen(target) >= sizeof(url)) return fail("Download URL too long");
    snprintf(url, sizeof(url), "%s", target);
    redirects = 0;
    max_body = limit;
    sink = body_sink;
    sink_ctx = context;
    return start();
}

void https_cancel(void) {
    close_connection();
    state = S_IDLE;
}

const char *https_error(void) { return error; }
size_t https_received(void) { return received; }
size_t https_length(void) { return have_length ? content_length : 0; }

https_state_t https_poll(void) {
    if (state == S_IDLE) return HTTPS_IDLE;
    if (state == S_DONE) return HTTPS_DONE;
    if (state == S_FAILED) return HTTPS_FAILED;
    if (transport_failed()) { fail(transport_error()); return HTTPS_FAILED; }
    if (!wifi_ready()) { fail("WiFi disconnected"); return HTTPS_FAILED; }
    if ((int32_t)(now_ms() - last_activity) > IDLE_TIMEOUT_MS) {
        fail("Update server timed out");
        return HTTPS_FAILED;
    }
    if (state == S_CONNECT) {
        if (!transport_ready()) return HTTPS_BUSY;
        ssl = wolfSSL_new(ctx);
        if (!ssl) { fail("Not enough memory for TLS"); return HTTPS_FAILED; }
        if (wolfSSL_UseSNI(ssl, WOLFSSL_SNI_HOST_NAME, host, (unsigned short)strlen(host)) != WOLFSSL_SUCCESS ||
            wolfSSL_check_domain_name(ssl, host) != WOLFSSL_SUCCESS) {
            fail("TLS setup failed");
            return HTTPS_FAILED;
        }
        state = S_HANDSHAKE;
        last_activity = now_ms();
    }
    if (state == S_HANDSHAKE) {
        int r = wolfSSL_connect(ssl);
        if (r != WOLFSSL_SUCCESS) {
            int e = wolfSSL_get_error(ssl, r);
            if (e == WOLFSSL_ERROR_WANT_READ || e == WOLFSSL_ERROR_WANT_WRITE) return HTTPS_BUSY;
            fail_tls(e);
            return HTTPS_FAILED;
        }
        state = S_REQUEST;
        last_activity = now_ms();
    }
    if (state == S_REQUEST) {
        int r = wolfSSL_write(ssl, request, (int)request_len);
        if (r <= 0) {
            int e = wolfSSL_get_error(ssl, r);
            if (e == WOLFSSL_ERROR_WANT_READ || e == WOLFSSL_ERROR_WANT_WRITE) return HTTPS_BUSY;
            fail_tls(e);
            return HTTPS_FAILED;
        }
        state = S_HEADERS;
        last_activity = now_ms();
    }
    static uint8_t buffer[2048];
    for (int i = 0; i < 16 && (state == S_HEADERS || state == S_BODY); i++) {
        int r = wolfSSL_read(ssl, buffer, sizeof(buffer));
        if (r > 0) {
            last_activity = now_ms();
            if (!consume(buffer, (size_t)r)) break;
            continue;
        }
        int e = wolfSSL_get_error(ssl, r);
        if (e == WOLFSSL_ERROR_WANT_READ || e == WOLFSSL_ERROR_WANT_WRITE) break;
        if (e == WOLFSSL_ERROR_ZERO_RETURN || e == SOCKET_PEER_CLOSED_E) {
            if (state == S_BODY && !have_length && !chunked) finish();
            else fail("Connection closed before the download finished");
        } else fail_tls(e);
        break;
    }
    return state == S_DONE ? HTTPS_DONE : state == S_FAILED ? HTTPS_FAILED :
           state == S_IDLE ? HTTPS_IDLE : HTTPS_BUSY;
}

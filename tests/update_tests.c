#include "update_core.h"
#include "updater.h"
#include "https.h"
#include "system.h"
#include "ff.h"
#include "diskio.h"
#include <wolfssl/wolfcrypt/settings.h>
#include <wolfssl/wolfcrypt/sha256.h>
#include <wolfssl/wolfcrypt/hash.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- RAM disk ---- */
#define SECTORS 16384
static uint8_t disk[SECTORS * 512u];
static bool fail_writes;
DSTATUS disk_initialize(BYTE drive) { return drive ? STA_NOINIT : 0; }
DSTATUS disk_status(BYTE drive) { return drive ? STA_NOINIT : 0; }
DRESULT disk_read(BYTE drive, BYTE *buffer, LBA_t sector, UINT count) {
    if (drive || sector >= SECTORS || count > SECTORS - sector) return RES_PARERR;
    memcpy(buffer, disk + sector * 512u, count * 512u);
    return RES_OK;
}
DRESULT disk_write(BYTE drive, const BYTE *buffer, LBA_t sector, UINT count) {
    if (fail_writes) return RES_ERROR;
    if (drive || sector >= SECTORS || count > SECTORS - sector) return RES_PARERR;
    memcpy(disk + sector * 512u, buffer, count * 512u);
    return RES_OK;
}
DRESULT disk_ioctl(BYTE drive, BYTE command, void *buffer) {
    if (drive) return RES_PARERR;
    switch (command) {
    case CTRL_SYNC: return fail_writes ? RES_ERROR : RES_OK;
    case GET_SECTOR_COUNT: *(LBA_t *)buffer = SECTORS; return RES_OK;
    case GET_SECTOR_SIZE: *(WORD *)buffer = 512; return RES_OK;
    case GET_BLOCK_SIZE: *(DWORD *)buffer = 1; return RES_OK;
    default: return RES_PARERR;
    }
}

/* ---- running image ---- */
#define IMAGE_SIZE 5000u
static uint8_t running[(IMAGE_SIZE + 255) & ~255u];
const uint8_t *system_image(size_t *size) {
    *size = sizeof(running);
    return running;
}
void system_reboot_to_loader(void) { abort(); }

/* ---- HTTPS mock: serves one response body, a few bytes per poll ---- */
static const uint8_t *serve_data;
static size_t serve_size, served, serve_limit;
static https_sink_fn serve_sink;
static bool serve_fail, serve_active, serve_done, serve_failed;
static char last_url[HTTPS_URL_LEN], https_err[64];
static const uint8_t *next_data;
static size_t next_size;
static bool next_fail;
static void respond(const void *data, size_t size, bool fail) {
    next_data = data;
    next_size = size;
    next_fail = fail;
}
bool https_get(const char *url, size_t max_body, https_sink_fn sink, void *ctx) {
    (void)ctx;
    snprintf(last_url, sizeof(last_url), "%s", url);
    serve_data = next_data;
    serve_size = next_size;
    serve_fail = next_fail;
    serve_limit = max_body;
    serve_sink = sink;
    served = 0;
    serve_active = true;
    serve_done = serve_failed = false;
    https_err[0] = 0;
    return true;
}
https_state_t https_poll(void) {
    if (!serve_active) return serve_done ? HTTPS_DONE : serve_failed ? HTTPS_FAILED : HTTPS_IDLE;
    if (serve_fail) {
        snprintf(https_err, sizeof(https_err), "Server returned HTTP 404");
        serve_active = false;
        serve_failed = true;
        return HTTPS_FAILED;
    }
    size_t n = serve_size - served < 777 ? serve_size - served : 777;
    if (served + n > serve_limit || (n && !serve_sink(NULL, serve_data + served, n))) {
        snprintf(https_err, sizeof(https_err), served + n > serve_limit ? "Download larger than expected"
                                                                       : "Download stopped");
        serve_active = false;
        serve_failed = true;
        return HTTPS_FAILED;
    }
    served += n;
    if (served < serve_size) return HTTPS_BUSY;
    serve_active = false;
    serve_done = true;
    return HTTPS_DONE;
}
void https_cancel(void) { serve_active = serve_done = serve_failed = false; }
const char *https_error(void) { return https_err; }
size_t https_received(void) { return served; }
size_t https_length(void) { return serve_size; }

/* ---- helpers ---- */
static void put32(uint8_t *b, uint32_t v) {
    b[0] = (uint8_t)v;
    b[1] = (uint8_t)(v >> 8);
    b[2] = (uint8_t)(v >> 16);
    b[3] = (uint8_t)(v >> 24);
}
static void block_header(uint8_t *b, uint32_t flags, uint32_t address, uint32_t number, uint32_t total,
                         uint32_t family) {
    memset(b, 0, 512);
    put32(b, 0x0A324655u);
    put32(b + 4, 0x9E5D5157u);
    put32(b + 8, flags);
    put32(b + 12, address);
    put32(b + 16, 256);
    put32(b + 20, number);
    put32(b + 24, total);
    put32(b + 28, family);
    put32(b + 508, 0x0AB16F30u);
}
/* picotool layout: RP2350 compatibility marker first, then the application pages. */
static size_t make_uf2(uint8_t *out, const uint8_t *image, size_t size) {
    uint32_t pages = (uint32_t)((size + 255) / 256);
    block_header(out, 0xA000, 0x10FFFF00u, 0, 2, 0xE48BFF57u);
    for (uint32_t i = 0; i < pages; i++) {
        uint8_t *b = out + 512u * (i + 1);
        block_header(b, 0x2000, UF2_APP_BASE + i * 256u, i, pages, 0xE48BFF59u);
        size_t take = size - i * 256u < 256 ? size - i * 256u : 256;
        memcpy(b + 32, image + i * 256u, take);
    }
    return 512u * (pages + 1);
}
static void write_file(const char *path, const void *data, size_t size) {
    FIL f;
    UINT n;
    assert(f_open(&f, path, FA_WRITE | FA_CREATE_ALWAYS) == FR_OK);
    assert(f_write(&f, data, (UINT)size, &n) == FR_OK && n == size);
    assert(f_close(&f) == FR_OK);
}
static size_t read_file(const char *path, uint8_t *data, size_t size) {
    FIL f;
    UINT n;
    if (f_open(&f, path, FA_READ) != FR_OK) return (size_t)-1;
    assert(f_read(&f, data, (UINT)size, &n) == FR_OK);
    f_close(&f);
    return n;
}
static bool exists(const char *path) { return f_stat(path, NULL) == FR_OK; }
static update_state_t run(void) {
    update_state_t s;
    for (unsigned i = 0; i < 200000 && updater_busy(); i++) s = updater_poll();
    s = updater_state();
    return s;
}
static void sha_hex(const uint8_t *data, size_t size, char hex[65]) {
    uint8_t digest[32];
    assert(wc_Sha256Hash(data, (word32)size, digest) == 0);
    for (int i = 0; i < 32; i++) snprintf(hex + i * 2, 3, "%02x", digest[i]);
}
static FATFS fs;
static void format(void) {
    uint8_t work[4096];
    MKFS_PARM options = {.fmt = FM_FAT | FM_SFD, .n_fat = 1};
    f_mount(NULL, "", 0);
    assert(f_mkfs("", &options, work, sizeof(work)) == FR_OK);
    assert(f_mount(&fs, "", 1) == FR_OK);
}

static void core_tests(void) {
    assert(update_is_newer("1.0.1", "1.0.0"));
    assert(update_is_newer("1.10.0", "1.9.99"));
    assert(update_is_newer("2.0.0", "1.99.99"));
    assert(!update_is_newer("1.0.0", "1.0.0"));
    assert(!update_is_newer("1.0.0", "1.0.1"));
    assert(update_is_newer("1.0.0", "1.0.0-dev"));
    assert(update_is_newer("1.0.0", "dev"));
    assert(!update_is_newer("1.0", "0.0.1"));
    assert(!update_is_newer("1.0.0-rc1", "0.0.1"));
    assert(!update_is_newer("1.0.0.0", "0.0.1"));

    uint8_t hash[32];
    char name[UPDATE_NAME_LEN], version[UPDATE_VERSION_LEN];
    const char *sums = "0000000000000000000000000000000000000000000000000000000000000000  README.txt\r\n"
                       "B53C40442E64748D6200DE1E360F179BAB2136C85F71DDDC3E7A99433B0A7A4D *picocalc_ssh-v1.2.30.uf2\r\n";
    assert(update_parse_sums(sums, hash, name, version));
    assert(!strcmp(name, "picocalc_ssh-v1.2.30.uf2") && !strcmp(version, "1.2.30"));
    assert(hash[0] == 0xb5 && hash[31] == 0x4d);
    assert(!update_parse_sums("zz", hash, name, version));
    assert(!update_parse_sums("b53c40442e64748d6200de1e360f179bab2136c85f71dddc3e7a99433b0a7a4d  picocalc_ssh-vX.uf2",
                              hash, name, version));
    assert(!update_parse_sums("b53c40442e64748d6200de1e360f179bab2136c85f71dddc3e7a99433b0a7a4  picocalc_ssh-v1.0.0.uf2",
                              hash, name, version));

    static uint8_t uf2[40 * 512], image[4000];
    for (size_t i = 0; i < sizeof(image); i++) image[i] = (uint8_t)(i * 7);
    size_t size = make_uf2(uf2, image, sizeof(image));
    uf2_check_t check;
    uf2_check_init(&check);
    for (size_t at = 0; at < size; at += 512) assert(uf2_check_block(&check, uf2 + at));
    assert(uf2_check_finish(&check) && check.app_blocks == 16 && check.markers == 1);

    uf2_check_init(&check);
    for (size_t at = 0; at < size - 512; at += 512) assert(uf2_check_block(&check, uf2 + at));
    assert(!uf2_check_finish(&check) && strstr(check.error, "Incomplete"));

    uf2_check_init(&check);
    assert(uf2_check_block(&check, uf2));
    assert(!uf2_check_block(&check, uf2 + 1024) && strstr(check.error, "order"));

    uf2_check_init(&check);
    assert(uf2_check_block(&check, uf2) && !uf2_check_block(&check, uf2) && strstr(check.error, "marker"));

    uint8_t bad[512];
    memcpy(bad, uf2 + 1024, 512); /* block 1 rewrites page 0 */
    put32(bad + 12, UF2_APP_BASE);
    uf2_check_init(&check);
    assert(uf2_check_block(&check, uf2 + 512) && !uf2_check_block(&check, bad) && strstr(check.error, "Duplicate"));

    memcpy(bad, uf2 + 512, 512);
    bad[508] ^= 1;
    uf2_check_init(&check);
    assert(!uf2_check_block(&check, bad) && strstr(check.error, "magic"));
    memcpy(bad, uf2 + 512, 512);
    put32(bad + 28, 0xE48BFF56u); /* RP2040 family */
    uf2_check_init(&check);
    assert(!uf2_check_block(&check, bad) && strstr(check.error, "Pico 2"));
    memcpy(bad, uf2 + 512, 512);
    put32(bad + 12, 0x10400000u);
    uf2_check_init(&check);
    assert(!uf2_check_block(&check, bad) && strstr(check.error, "Address"));
    memcpy(bad, uf2 + 1024, 512);
    put32(bad + 20, 0);
    uf2_check_init(&check);
    assert(uf2_check_block(&check, bad) && !uf2_check_finish(&check) && strstr(check.error, "Incomplete"));

    assert(uf2_block_matches(uf2, image, sizeof(image)) == UF2_MATCH_OTHER);
    assert(uf2_block_matches(uf2 + 512, image, sizeof(image)) == UF2_MATCH_SAME);
    memcpy(bad, uf2 + 512, 512);
    bad[32 + 0x20] ^= 0xff; /* proginfo area is ignored */
    bad[32 + 0x2B] ^= 0xff;
    assert(uf2_block_matches(bad, image, sizeof(image)) == UF2_MATCH_SAME);
    bad[32 + 0x2C] ^= 0xff;
    assert(uf2_block_matches(bad, image, sizeof(image)) == UF2_MATCH_DIFFERENT);
    assert(uf2_block_matches(uf2 + 512 * 16, image, 15 * 256) == UF2_MATCH_DIFFERENT);
    bad[0] = 0;
    assert(uf2_block_matches(bad, image, sizeof(image)) == UF2_MATCH_INVALID);
    puts("update core tests passed");
}

static uint8_t current_uf2[32 * 512], new_uf2[32 * 512], other_uf2[32 * 512], readback[64 * 512];
static size_t current_size, new_size, other_size;
static char sums_text[256];

static void prepare_release(const char *version, const uint8_t *data, size_t size) {
    char hex[65];
    sha_hex(data, size, hex);
    snprintf(sums_text, sizeof(sums_text), "%s  picocalc_ssh-v%s.uf2\n", hex, version);
    respond(sums_text, strlen(sums_text), false);
}

static void check_available(const char *current, const char *expected_target, bool expected_found) {
    assert(updater_check(current));
    assert(updater_state() == UPDATE_CHECKING && updater_busy());
    assert(!strcmp(last_url, "https://github.com/kicomoco/UF2PicoCalcSSH/releases/latest/download/SHA256SUMS"));
    respond(new_uf2, new_size, false);
    assert(run() == UPDATE_AVAILABLE);
    assert(!strcmp(updater_version(), "1.0.7"));
    assert(!strcmp(updater_target(), expected_target));
    assert(updater_target_found() == expected_found);
}

static void flow_tests(void) {
    static uint8_t new_image[7000], other_image[IMAGE_SIZE];
    for (size_t i = 0; i < IMAGE_SIZE; i++) running[i] = (uint8_t)(i * 13 + 1);
    for (size_t i = 0; i < sizeof(new_image); i++) new_image[i] = (uint8_t)(i * 5 + 3);
    memcpy(other_image, running, IMAGE_SIZE);
    other_image[4000] ^= 0x55;
    current_size = make_uf2(current_uf2, running, IMAGE_SIZE);
    current_uf2[512 + 32 + 0x24] ^= 0xA5; /* loader-patched proginfo differs from flash */
    new_size = make_uf2(new_uf2, new_image, sizeof(new_image));
    other_size = make_uf2(other_uf2, other_image, IMAGE_SIZE);

    /* Locate the launched file among similar apps, then install over it. */
    format();
    assert(f_mkdir("/pico2-apps") == FR_OK && f_mkdir("/pico2-apps/tools") == FR_OK);
    write_file("/pico2-apps/aaa-other.uf2", other_uf2, other_size);
    write_file("/pico2-apps/readme.txt", "hello", 5);
    write_file("/pico2-apps/short.uf2", current_uf2, current_size - 512);
    /* Sparse look-alike: same first/last pages, a middle page omitted but numbering kept valid. */
    static uint8_t sparse[32 * 512];
    size_t sparse_size = current_size - 512;
    memcpy(sparse, current_uf2, 512 * 4);
    memcpy(sparse + 512 * 4, current_uf2 + 512 * 5, current_size - 512 * 5);
    uint32_t sparse_total = (uint32_t)(sparse_size / 512 - 1);
    for (uint32_t i = 0; i < sparse_total; i++) {
        put32(sparse + 512 * (i + 1) + 20, i);
        put32(sparse + 512 * (i + 1) + 24, sparse_total);
    }
    write_file("/pico2-apps/abc-sparse.uf2", sparse, sparse_size);
    write_file("/pico2-apps/tools/SSH Client.UF2", current_uf2, current_size);
    write_file("/ssh-root.uf2", current_uf2, current_size);
    prepare_release("1.0.7", new_uf2, new_size);
    check_available("1.0.6", "/pico2-apps/tools/SSH Client.UF2", true);
    assert(updater_install() && updater_state() == UPDATE_DOWNLOADING);
    assert(!strcmp(last_url, "https://github.com/kicomoco/UF2PicoCalcSSH/releases/download/v1.0.7/picocalc_ssh-v1.0.7.uf2"));
    assert(run() == UPDATE_DONE);
    assert(read_file("/pico2-apps/tools/SSH Client.UF2", readback, sizeof(readback)) == new_size);
    assert(!memcmp(readback, new_uf2, new_size));
    assert(!exists("/pico2-apps/tools/SSH Client.UF2.new") && !exists("/pico2-apps/tools/SSH Client.UF2.bak"));
    assert(read_file("/pico2-apps/aaa-other.uf2", readback, sizeof(readback)) == other_size);
    assert(updater_done_bytes() == new_size && updater_total_bytes() == new_size);
    puts("update: launched file located and replaced");

    /* Same or older release. */
    prepare_release("1.0.7", new_uf2, new_size);
    assert(updater_check("1.0.7") && run() == UPDATE_CURRENT);
    prepare_release("1.0.7", new_uf2, new_size);
    assert(updater_check("1.1.0") && run() == UPDATE_CURRENT);

    /* Launched file not found: install to the default loader path, creating the folder. */
    format();
    write_file("/other.uf2", current_uf2, current_size);
    prepare_release("1.0.7", new_uf2, new_size);
    check_available("1.0.0-dev", UPDATE_DEFAULT_FILE, false);
    assert(updater_install() && run() == UPDATE_DONE);
    assert(read_file(UPDATE_DEFAULT_FILE, readback, sizeof(readback)) == new_size && !memcmp(readback, new_uf2, new_size));

    /* Checksum mismatch keeps the current app and removes the partial download. */
    format();
    assert(f_mkdir("/pico2-apps") == FR_OK);
    write_file("/pico2-apps/ssh.uf2", current_uf2, current_size);
    prepare_release("1.0.7", new_uf2, new_size);
    sums_text[0] = sums_text[0] == '0' ? '1' : '0';
    check_available("1.0.6", "/pico2-apps/ssh.uf2", true);
    assert(updater_install() && run() == UPDATE_FAILED);
    assert(strstr(updater_error(), "checksum"));
    assert(read_file("/pico2-apps/ssh.uf2", readback, sizeof(readback)) == current_size);
    assert(!memcmp(readback, current_uf2, current_size) && !exists("/pico2-apps/ssh.uf2.new"));

    /* Invalid UF2 content is rejected while streaming. */
    static uint8_t corrupt[32 * 512];
    memcpy(corrupt, new_uf2, new_size);
    put32(corrupt + 512 * 5 + 20, 9);
    prepare_release("1.0.7", corrupt, new_size);
    check_available("1.0.6", "/pico2-apps/ssh.uf2", true);
    respond(corrupt, new_size, false);
    assert(updater_install() && run() == UPDATE_FAILED);
    assert(strstr(updater_error(), "order"));
    assert(read_file("/pico2-apps/ssh.uf2", readback, sizeof(readback)) == current_size);
    assert(!exists("/pico2-apps/ssh.uf2.new"));

    /* Truncated download. */
    prepare_release("1.0.7", new_uf2, new_size - 100);
    check_available("1.0.6", "/pico2-apps/ssh.uf2", true);
    respond(new_uf2, new_size - 100, false);
    assert(updater_install() && run() == UPDATE_FAILED && strstr(updater_error(), "Incomplete"));
    assert(!exists("/pico2-apps/ssh.uf2.new"));

    /* Cancelling a download removes the partial file. */
    prepare_release("1.0.7", new_uf2, new_size);
    check_available("1.0.6", "/pico2-apps/ssh.uf2", true);
    assert(updater_install());
    for (int i = 0; i < 3; i++) updater_poll();
    assert(updater_state() == UPDATE_DOWNLOADING && updater_done_bytes() > 0);
    updater_cancel();
    assert(updater_state() == UPDATE_IDLE && !updater_busy() && !exists("/pico2-apps/ssh.uf2.new"));
    assert(read_file("/pico2-apps/ssh.uf2", readback, sizeof(readback)) == current_size);

    /* A stale backup from an interrupted earlier update is replaced. */
    write_file("/pico2-apps/ssh.uf2.bak", "old", 3);
    prepare_release("1.0.7", new_uf2, new_size);
    check_available("1.0.6", "/pico2-apps/ssh.uf2", true);
    assert(updater_install() && run() == UPDATE_DONE && !exists("/pico2-apps/ssh.uf2.bak"));

    /* Network and SD failures. */
    respond(NULL, 0, true);
    assert(updater_check("1.0.6") && run() == UPDATE_FAILED && strstr(updater_error(), "HTTP 404"));
    respond("no release here\n", 16, false);
    assert(updater_check("1.0.6") && run() == UPDATE_FAILED && strstr(updater_error(), "no PicoCalc SSH UF2"));
    format();
    prepare_release("1.0.7", new_uf2, new_size);
    check_available("1.0.6", UPDATE_DEFAULT_FILE, false);
    fail_writes = true;
    assert(updater_install() && run() == UPDATE_FAILED && strstr(updater_error(), "SD"));
    fail_writes = false;
    puts("update flow tests passed");
}

int main(void) {
    core_tests();
    flow_tests();
    return 0;
}

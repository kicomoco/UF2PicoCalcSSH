#include "updater.h"
#include "ff.h"
#include "https.h"
#include "system.h"
#include "update_core.h"
#include <wolfssl/wolfcrypt/settings.h>
#include <wolfssl/wolfcrypt/sha256.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#ifndef UPDATE_REPO
#define UPDATE_REPO "kicomoco/UF2PicoCalcSSH"
#endif
#define RELEASES "https://github.com/" UPDATE_REPO "/releases"
#define PATH_LEN 300
#define CHUNK 4096u
#define MAX_DIRS 8
#define LOCATE_BLOCKS_PER_POLL 8u

static update_state_t state;
static char error[96], sink_error[96];
static char current[UPDATE_VERSION_LEN], version[UPDATE_VERSION_LEN], asset[UPDATE_NAME_LEN];
static char target[PATH_LEN], temp[PATH_LEN + 4], backup[PATH_LEN + 4];
static bool found;
static uint8_t expected[32];
static uint8_t buffer[CHUNK], block[UF2_BLOCK];
static size_t buffered, block_fill, done_bytes, total_bytes;
static char sums[1024];
static size_t sums_len;
static FIL file;
static bool file_open;
static wc_Sha256 sha;
static uf2_check_t check;

/* Launch-file search over /pico2-apps and its immediate subdirectories (the loader menu). */
static char dirs[MAX_DIRS][PATH_LEN];
static unsigned dir_count, dir_index;
static DIR dir;
static bool dir_open;
static char candidate[PATH_LEN];
static uint32_t candidate_pages;
static uf2_check_t candidate_check;
static const uint8_t *image;
static size_t image_size;

static enum { VERIFY_TEMP, VERIFY_TARGET } verify_phase;

static update_state_t failed(const char *message) {
    if (state != UPDATE_FAILED) snprintf(error, sizeof(error), "%s", message);
    state = UPDATE_FAILED;
    return state;
}

static update_state_t failed_fs(const char *message, FRESULT r) {
    char text[96];
    snprintf(text, sizeof(text), "%s (SD error %d)", message, (int)r);
    return failed(text);
}

static void close_file(void) {
    if (file_open) f_close(&file);
    file_open = false;
}

static void close_dir(void) {
    if (dir_open) f_closedir(&dir);
    dir_open = false;
}

static void stop_work(void) {
    https_cancel();
    close_file();
    close_dir();
    buffered = block_fill = 0;
}

static bool has_uf2_suffix(const char *name) {
    size_t n = strlen(name);
    return n > 4 && !strcasecmp(name + n - 4, ".uf2");
}

static bool join(char *out, size_t size, const char *a, const char *b) {
    int n = snprintf(out, size, "%s/%s", a, b);
    return n > 0 && (size_t)n < size;
}

/* ---- checking ---- */

static bool sums_sink(void *ctx, const uint8_t *data, size_t size) {
    (void)ctx;
    if (size > sizeof(sums) - 1 - sums_len) return false;
    memcpy(sums + sums_len, data, size);
    sums_len += size;
    sums[sums_len] = 0;
    return true;
}

bool updater_check(const char *current_version) {
    if (updater_busy()) return false;
    stop_work();
    error[0] = version[0] = asset[0] = target[0] = 0;
    found = false;
    done_bytes = total_bytes = 0;
    snprintf(current, sizeof(current), "%s", current_version ? current_version : "");
    sums_len = 0;
    sums[0] = 0;
    state = UPDATE_CHECKING;
    if (!https_get(RELEASES "/latest/download/SHA256SUMS", sizeof(sums) - 1, sums_sink, NULL))
        failed(https_error());
    return true;
}

static void locate_start(void) {
    image = system_image(&image_size);
    snprintf(dirs[0], sizeof(dirs[0]), "%s", UPDATE_DEFAULT_DIR);
    dir_count = 1;
    dir_index = 0;
    candidate[0] = 0;
    state = UPDATE_LOCATING;
}

static update_state_t poll_checking(void) {
    https_state_t s = https_poll();
    if (s == HTTPS_BUSY) return state;
    if (s != HTTPS_DONE) return failed(https_error());
    if (!update_parse_sums(sums, expected, asset, version)) return failed("Release has no PicoCalc SSH UF2");
    if (!update_is_newer(version, current)) {
        state = UPDATE_CURRENT;
        return state;
    }
    locate_start();
    return state;
}

/* ---- locating the launched file ---- */

static void locate_finish(void) {
    close_file();
    close_dir();
    if (!found) snprintf(target, sizeof(target), "%s", UPDATE_DEFAULT_FILE);
    state = UPDATE_AVAILABLE;
}

/* Reads a few blocks of the open candidate; returns true once the candidate is decided. */
static bool locate_compare(void) {
    for (unsigned i = 0; i < LOCATE_BLOCKS_PER_POLL; i++) {
        UINT n = 0;
        FRESULT r = f_read(&file, block, UF2_BLOCK, &n);
        if (r != FR_OK || (n && n != UF2_BLOCK)) {
            close_file();
            return true;
        }
        if (!n) {
            close_file();
            /* A valid UF2 (no duplicate pages) whose pages match every page of the running image. */
            if (uf2_check_finish(&candidate_check) && candidate_pages == image_size / 256u) {
                snprintf(target, sizeof(target), "%s", candidate);
                found = true;
            }
            return true;
        }
        uf2_match_t m = uf2_block_matches(block, image, image_size);
        if (m == UF2_MATCH_DIFFERENT || m == UF2_MATCH_INVALID || !uf2_check_block(&candidate_check, block)) {
            close_file();
            return true;
        }
        if (m == UF2_MATCH_SAME) candidate_pages++;
    }
    return false;
}

static update_state_t poll_locating(void) {
    if (file_open) {
        if (locate_compare() && found) locate_finish();
        return state;
    }
    if (!dir_open) {
        if (dir_index >= dir_count) {
            locate_finish();
            return state;
        }
        if (f_opendir(&dir, dirs[dir_index]) != FR_OK) {
            dir_index++;
            return state;
        }
        dir_open = true;
    }
    FILINFO info;
    if (f_readdir(&dir, &info) != FR_OK || !info.fname[0]) {
        close_dir();
        dir_index++;
        return state;
    }
    if (info.fname[0] == '.' || info.fattrib & (AM_HID | AM_SYS)) return state;
    if (info.fattrib & AM_DIR) {
        char sub[PATH_LEN];
        if (dir_index == 0 && dir_count < MAX_DIRS && join(sub, sizeof(sub), dirs[0], info.fname))
            memcpy(dirs[dir_count++], sub, sizeof(sub));
        return state;
    }
    if (!has_uf2_suffix(info.fname) || info.fsize < UF2_BLOCK || info.fsize % UF2_BLOCK) return state;
    if (!join(candidate, sizeof(candidate), dirs[dir_index], info.fname)) return state;
    if (f_open(&file, candidate, FA_READ) != FR_OK) return state;
    file_open = true;
    candidate_pages = 0;
    uf2_check_init(&candidate_check);
    return state;
}

/* ---- downloading ---- */

static bool sink_fail(const char *message) {
    if (!sink_error[0]) snprintf(sink_error, sizeof(sink_error), "%s", message);
    return false;
}

static bool flush(void) {
    if (!buffered) return true;
    UINT n = 0;
    FRESULT r = f_write(&file, buffer, (UINT)buffered, &n);
    if (r != FR_OK || n != buffered) {
        char text[64];
        snprintf(text, sizeof(text), "SD card write failed (%d)", r != FR_OK ? (int)r : (int)FR_DENIED);
        return sink_fail(text);
    }
    buffered = 0;
    return true;
}

static bool download_sink(void *ctx, const uint8_t *data, size_t size) {
    (void)ctx;
    if (wc_Sha256Update(&sha, data, (word32)size) != 0) return sink_fail("SHA-256 failed");
    done_bytes += size;
    for (size_t i = 0; i < size;) {
        size_t take = UF2_BLOCK - block_fill < size - i ? UF2_BLOCK - block_fill : size - i;
        memcpy(block + block_fill, data + i, take);
        block_fill += take;
        i += take;
        if (block_fill == UF2_BLOCK) {
            block_fill = 0;
            if (!uf2_check_block(&check, block)) return sink_fail(check.error);
        }
    }
    for (size_t i = 0; i < size;) {
        size_t take = CHUNK - buffered < size - i ? CHUNK - buffered : size - i;
        memcpy(buffer + buffered, data + i, take);
        buffered += take;
        i += take;
        if (buffered == CHUNK && !flush()) return false;
    }
    return true;
}

static void remove_temp(void) {
    close_file();
    if (temp[0]) f_unlink(temp);
}

bool updater_install(void) {
    if (state != UPDATE_AVAILABLE) return false;
    snprintf(temp, sizeof(temp), "%s.new", target);
    snprintf(backup, sizeof(backup), "%s.bak", target);
    if (!found) {
        FRESULT r = f_mkdir(UPDATE_DEFAULT_DIR);
        if (r != FR_OK && r != FR_EXIST) {
            failed_fs("Cannot create " UPDATE_DEFAULT_DIR, r);
            return true;
        }
    }
    FRESULT r = f_open(&file, temp, FA_WRITE | FA_CREATE_ALWAYS);
    if (r != FR_OK) {
        failed_fs("Cannot create download file", r);
        return true;
    }
    file_open = true;
    buffered = block_fill = done_bytes = total_bytes = 0;
    sink_error[0] = 0;
    uf2_check_init(&check);
    if (wc_InitSha256(&sha) != 0) {
        remove_temp();
        failed("SHA-256 failed");
        return true;
    }
    char url[256];
    snprintf(url, sizeof(url), RELEASES "/download/v%s/%s", version, asset);
    state = UPDATE_DOWNLOADING;
    if (!https_get(url, UF2_MAX_FILE, download_sink, NULL)) {
        remove_temp();
        failed(https_error());
    }
    return true;
}

static bool open_verify(const char *path) {
    FRESULT r = f_open(&file, path, FA_READ);
    if (r != FR_OK) {
        failed_fs("Cannot reopen update file", r);
        return false;
    }
    file_open = true;
    done_bytes = 0;
    total_bytes = f_size(&file);
    uf2_check_init(&check);
    if (wc_InitSha256(&sha) != 0) {
        close_file();
        failed("SHA-256 failed");
        return false;
    }
    state = verify_phase == VERIFY_TEMP ? UPDATE_VERIFYING : UPDATE_INSTALLING;
    return true;
}

static update_state_t poll_downloading(void) {
    total_bytes = https_length();
    https_state_t s = https_poll();
    if (s == HTTPS_BUSY) return state;
    if (s != HTTPS_DONE) {
        remove_temp();
        return failed(sink_error[0] ? sink_error : https_error());
    }
    uint8_t digest[32];
    bool ok = flush();
    FRESULT r = f_sync(&file);
    close_file();
    if (!ok) {
        f_unlink(temp);
        return failed(sink_error);
    }
    if (r != FR_OK) {
        f_unlink(temp);
        return failed_fs("SD card write failed", r);
    }
    if (block_fill || !uf2_check_finish(&check)) {
        f_unlink(temp);
        return failed(block_fill ? "Incomplete UF2 download" : check.error);
    }
    if (wc_Sha256Final(&sha, digest) != 0 || memcmp(digest, expected, sizeof(digest))) {
        f_unlink(temp);
        return failed("Download checksum mismatch");
    }
    verify_phase = VERIFY_TEMP;
    if (!open_verify(temp)) f_unlink(temp);
    return state;
}

/* ---- verifying and installing ---- */

static void restore_backup(void) {
    f_unlink(target);
    f_rename(backup, target);
}

static update_state_t swap_files(void) {
    FRESULT r = f_unlink(backup);
    if (r != FR_OK && r != FR_NO_FILE) {
        f_unlink(temp);
        return failed_fs("Cannot remove old backup", r);
    }
    bool had_target = f_stat(target, NULL) == FR_OK;
    if (had_target && (r = f_rename(target, backup)) != FR_OK) {
        f_unlink(temp);
        return failed_fs("Cannot back up current app", r);
    }
    if ((r = f_rename(temp, target)) != FR_OK) {
        if (had_target) f_rename(backup, target);
        f_unlink(temp);
        return failed_fs("Cannot replace app file", r);
    }
    verify_phase = VERIFY_TARGET;
    if (!open_verify(target)) {
        if (had_target) restore_backup();
    }
    return state;
}

static update_state_t poll_verifying(void) {
    UINT n = 0;
    FRESULT r = f_read(&file, buffer, CHUNK, &n);
    bool bad = r != FR_OK || n % UF2_BLOCK || wc_Sha256Update(&sha, buffer, n) != 0;
    for (UINT i = 0; !bad && i < n; i += UF2_BLOCK) bad = !uf2_check_block(&check, buffer + i);
    done_bytes += n;
    if (!bad && n == CHUNK) return state;
    close_file();
    uint8_t digest[32];
    bad = bad || !uf2_check_finish(&check) || wc_Sha256Final(&sha, digest) != 0 ||
          memcmp(digest, expected, sizeof(digest));
    if (verify_phase == VERIFY_TEMP) {
        if (bad) {
            f_unlink(temp);
            return failed("Saved update failed verification");
        }
        return swap_files();
    }
    if (bad) {
        restore_backup();
        return failed("Installed app failed verification; restored");
    }
    f_unlink(backup);
    state = UPDATE_DONE;
    return state;
}

update_state_t updater_poll(void) {
    switch (state) {
    case UPDATE_CHECKING: return poll_checking();
    case UPDATE_LOCATING: return poll_locating();
    case UPDATE_DOWNLOADING: return poll_downloading();
    case UPDATE_VERIFYING:
    case UPDATE_INSTALLING: return poll_verifying();
    default: return state;
    }
}

void updater_cancel(void) {
    if (state == UPDATE_VERIFYING || state == UPDATE_INSTALLING) return;
    bool downloading = state == UPDATE_DOWNLOADING;
    stop_work();
    if (downloading) f_unlink(temp);
    state = UPDATE_IDLE;
}

update_state_t updater_state(void) { return state; }
bool updater_busy(void) {
    return state == UPDATE_CHECKING || state == UPDATE_LOCATING || state == UPDATE_DOWNLOADING ||
           state == UPDATE_VERIFYING || state == UPDATE_INSTALLING;
}
const char *updater_error(void) { return error; }
const char *updater_version(void) { return version; }
const char *updater_target(void) { return target; }
bool updater_target_found(void) { return found; }
size_t updater_done_bytes(void) { return done_bytes; }
size_t updater_total_bytes(void) { return total_bytes; }

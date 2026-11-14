#include "storage.h"
#include "ff.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static FATFS filesystem;
static char error[96];
static bool mounted, writable, recovered;
typedef struct {
    char magic[8];
    uint32_t size, checksum;
} file_header_t;
static const char *config_path = "/sshclient/settings.bin";
static const char *backup_path = "/sshclient/settings.bak";
static const char *temp_path = "/sshclient/settings.tmp";
static const char *prefs_path = "/sshclient/prefs.bin";
static const char *prefs_temp_path = "/sshclient/prefs.tmp";

static uint32_t checksum(const void *data, size_t size) {
    const uint8_t *p = data;
    uint32_t crc = 0xffffffffu;
    while (size--) {
        crc ^= *p++;
        for (int i = 0; i < 8; i++) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1)));
    }
    return ~crc;
}

static bool fail(const char *operation, FRESULT result) {
    snprintf(error, sizeof(error), "%s (SD error %d)", operation, (int)result);
    return false;
}

const char *storage_error(void) { return error; }

static FRESULT load_blob(const char *path, const char *magic, void *data, size_t size) {
    FIL f;
    UINT n = 0;
    FRESULT r = f_open(&f, path, FA_READ);
    if (r != FR_OK) return r;
    file_header_t header;
    if (f_size(&f) != sizeof(header) + size) {
        f_close(&f);
        return FR_INVALID_OBJECT;
    }
    r = f_read(&f, &header, sizeof(header), &n);
    if (r == FR_OK && (n != sizeof(header) || memcmp(header.magic, magic, 8) ||
                       header.size != size)) r = FR_INVALID_OBJECT;
    if (r == FR_OK) r = f_read(&f, data, (UINT)size, &n);
    FRESULT close_result = f_close(&f);
    if (r != FR_OK) return r;
    if (close_result != FR_OK) return close_result;
    return n == size && checksum(data, size) == header.checksum ? FR_OK : FR_INVALID_OBJECT;
}

static FRESULT load(const char *path, settings_t *s) {
    FRESULT r = load_blob(path, "PCSSH01", s, sizeof(*s));
    return r == FR_OK && !settings_valid(s) ? FR_INVALID_OBJECT : r;
}

static FRESULT write_blob(const char *path, const char *magic, const void *data, size_t size) {
    FIL f;
    UINT n;
    FRESULT r = f_open(&f, path, FA_WRITE | FA_CREATE_ALWAYS);
    if (r != FR_OK) return r;
    file_header_t header = {.size = (uint32_t)size, .checksum = checksum(data, size)};
    memcpy(header.magic, magic, 8);
    r = f_write(&f, &header, sizeof(header), &n);
    if (r == FR_OK && n != sizeof(header)) r = FR_DISK_ERR;
    if (r == FR_OK) r = f_write(&f, data, (UINT)size, &n);
    if (r == FR_OK && n != size) r = FR_DISK_ERR;
    if (r == FR_OK) r = f_sync(&f);
    FRESULT closed = f_close(&f);
    return r == FR_OK ? closed : r;
}

bool storage_init(settings_t *s) {
    mounted = writable = recovered = false;
    memset(s, 0, sizeof(*s));
    s->version = 1;
    FRESULT r = f_mount(&filesystem, "", 1);
    if (r != FR_OK) return fail("Cannot mount SD card", r);
    r = f_mkdir("/sshclient");
    if (r != FR_OK && r != FR_EXIST) return fail("Cannot create /sshclient", r);
    mounted = writable = true;
    r = load(config_path, s);
    if (r == FR_OK) return true;
    FRESULT backup = load(backup_path, s);
    if (backup == FR_OK) {
        recovered = true;
        snprintf(error, sizeof(error), "Recovered backup; save to repair settings");
        return false;
    }
    memset(s, 0, sizeof(*s));
    s->version = 1;
    if (r == FR_NO_FILE && backup == FR_NO_FILE) return true;
    writable = false;
    return fail("Settings damaged; restore/remove files on PC", r);
}

bool storage_save(const settings_t *s) {
    if (!writable) {
        snprintf(error, sizeof(error), "SD storage unavailable; nothing saved");
        return false;
    }
    if (!settings_valid(s)) return fail("Invalid settings; not saved", FR_INVALID_PARAMETER);
    FRESULT r = write_blob(temp_path, "PCSSH01", s, sizeof(*s));
    if (r != FR_OK) return fail("Cannot write settings", r);
    /* Preserve the previous complete file until the new one has been synced. */
    FILINFO info;
    r = f_stat(config_path, &info);
    if (recovered) {
        if (r == FR_OK) r = f_unlink(config_path);
        if (r != FR_OK && r != FR_NO_FILE) return fail("Cannot replace damaged settings", r);
    } else if (r == FR_OK) {
        FRESULT removed = f_unlink(backup_path);
        if (removed != FR_OK && removed != FR_NO_FILE) return fail("Cannot rotate backup", removed);
        r = f_rename(config_path, backup_path);
        if (r != FR_OK) return fail("Cannot back up settings", r);
    } else if (r != FR_NO_FILE) return fail("Cannot inspect settings", r);
    r = f_rename(temp_path, config_path);
    if (r != FR_OK) return fail("Cannot install settings", r);
    /* Backups must not retain credentials after a Forget/Delete operation. */
    r = f_unlink(backup_path);
    if (r != FR_OK && r != FR_NO_FILE) return fail("Saved, but cannot remove old credentials", r);
    recovered = false;
    error[0] = 0;
    return true;
}

bool storage_load_prefs(prefs_t *p) {
    if (!mounted) {
        prefs_defaults(p);
        return true;
    }
    FRESULT r = load_blob(prefs_path, "PCPRF01", p, sizeof(*p));
    /* A power loss between unlink and rename leaves only the synced temp file. */
    bool primary_missing = r == FR_NO_FILE;
    if (primary_missing) r = load_blob(prefs_temp_path, "PCPRF01", p, sizeof(*p));
    if (r == FR_OK && prefs_valid(p)) return true;
    prefs_defaults(p);
    if (primary_missing) return true;
    return fail("Display settings damaged; using defaults", r == FR_OK ? FR_INVALID_OBJECT : r);
}

bool storage_save_prefs(const prefs_t *p) {
    if (!mounted) {
        snprintf(error, sizeof(error), "SD storage unavailable; settings not saved");
        return false;
    }
    if (!prefs_valid(p)) return fail("Invalid display settings; not saved", FR_INVALID_PARAMETER);
    FRESULT r = write_blob(prefs_temp_path, "PCPRF01", p, sizeof(*p));
    if (r != FR_OK) return fail("Cannot write display settings", r);
    r = f_unlink(prefs_path);
    if (r != FR_OK && r != FR_NO_FILE) return fail("Cannot replace display settings", r);
    r = f_rename(prefs_temp_path, prefs_path);
    if (r != FR_OK) return fail("Cannot install display settings", r);
    return true;
}

bool storage_read_key(const char *path, uint8_t **data, size_t *size) {
    *data = NULL;
    *size = 0;
    FIL f;
    FRESULT r = f_open(&f, path, FA_READ);
    if (r != FR_OK) return fail("Cannot open private key", r);
    FSIZE_t length = f_size(&f);
    if (!length || length > 16384) {
        f_close(&f);
        return fail("Key must be 1..16384 bytes", FR_INVALID_PARAMETER);
    }
    uint8_t *buffer = malloc((size_t)length + 1);
    if (!buffer) {
        f_close(&f);
        return fail("Not enough memory for key", FR_NOT_ENOUGH_CORE);
    }
    UINT n = 0;
    r = f_read(&f, buffer, (UINT)length, &n);
    FRESULT closed = f_close(&f);
    if (r == FR_OK) r = closed;
    if (r != FR_OK || n != length) {
        secure_clear(buffer, (size_t)length);
        free(buffer);
        return fail("Cannot read private key", r == FR_OK ? FR_DISK_ERR : r);
    }
    buffer[length] = 0;
    *data = buffer;
    *size = (size_t)length;
    return true;
}

bool storage_list(const char *path, file_entry_t *entries, unsigned capacity,
                  unsigned *count, unsigned skip, bool *more) {
    DIR dir;
    FILINFO info;
    *count = 0;
    *more = false;
    FRESULT r = f_opendir(&dir, path);
    if (r != FR_OK) return fail("Cannot open directory", r);
    while ((r = f_readdir(&dir, &info)) == FR_OK && info.fname[0]) {
        if (info.fattrib & (AM_HID | AM_SYS)) continue;
        if (!strcmp(info.fname, ".") || !strcmp(info.fname, "..")) continue;
        if (skip) { skip--; continue; }
        if (*count == capacity) { *more = true; break; }
        file_entry_t *e = &entries[(*count)++];
        snprintf(e->name, sizeof(e->name), "%s", info.fname);
        e->directory = (info.fattrib & AM_DIR) != 0;
    }
    FRESULT closed = f_closedir(&dir);
    if (r == FR_OK) r = closed;
    return r == FR_OK || fail("Cannot read directory", r);
}

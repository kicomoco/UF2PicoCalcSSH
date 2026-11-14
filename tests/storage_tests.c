#include "model.h"
#include "storage.h"
#include "ff.h"
#include "diskio.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SECTORS 32768
static uint8_t disk[SECTORS * 512u];
static bool fail_writes;
static settings_t settings, reloaded;
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
static void write_file(const char *path, const void *data, unsigned size) {
    FIL f;
    UINT n;
    assert(f_open(&f, path, FA_WRITE | FA_CREATE_ALWAYS) == FR_OK);
    assert(f_write(&f, data, size, &n) == FR_OK && n == size);
    assert(f_close(&f) == FR_OK);
}
int main(void) {
    FATFS fs;
    assert(f_mount(&fs, "", 0) == FR_OK);
    uint8_t work[4096];
    MKFS_PARM options = {.fmt = FM_FAT | FM_SFD, .n_fat = 1};
    assert(f_mkfs("", &options, work, sizeof(work)) == FR_OK);
    assert(f_mount(NULL, "", 0) == FR_OK);
    assert(storage_init(&settings) && settings.version == 1 && settings.wifi_count == 0);
    wifi_profile_t wifi = {.ssid = "home", .password = "password"};
    assert(wifi_upsert(&settings, &wifi) == 0);
    strcpy(wifi.ssid, "office");
    assert(wifi_upsert(&settings, &wifi) == 1);
    ssh_profile_t ssh = {.host = "host.example", .port = "2222", .username = "user", .key_path = "/keys/id.pem"};
    assert(ssh_upsert(&settings, &ssh) == 0);
    assert(storage_save(&settings));
    assert(storage_init(&reloaded));
    assert(!memcmp(&settings, &reloaded, sizeof(settings)));
    assert(reloaded.version == 1); /* Original settings load without assuming a last connection. */
    assert(wifi_remember_connection(&settings, &wifi));
    assert(storage_save(&settings));
    assert(storage_init(&reloaded));
    assert(reloaded.version == 2 && !strcmp(reloaded.wifi[0].ssid, "office"));
    assert(!memcmp(&settings, &reloaded, sizeof(settings)));
    strcpy(wifi.ssid, "home");
    assert(wifi_remember_connection(&settings, &wifi));
    assert(storage_save(&settings));
    wifi_remove(&settings, 0);
    ssh_remove(&settings, 0);
    assert(storage_save(&settings));
    assert(storage_init(&reloaded));
    assert(reloaded.version == 1 && reloaded.wifi_count == 1 && !strcmp(reloaded.wifi[0].ssid, "office"));
    assert(reloaded.ssh_count == 0 && !reloaded.wifi[1].password[0]);
    FILINFO info;
    assert(f_stat("/sshclient/settings.bak", &info) == FR_NO_FILE);
    /* A completed backup survives a failed replacement and can be recovered. */
    assert(f_rename("/sshclient/settings.bin", "/sshclient/settings.bak") == FR_OK);
    assert(!storage_init(&reloaded) && strstr(storage_error(), "Recovered"));
    assert(reloaded.wifi_count == 1);
    assert(storage_save(&reloaded));
    assert(storage_init(&reloaded));
    assert(f_stat("/sshclient/settings.bak", &info) == FR_NO_FILE);
    /* Detect corruption in the payload, not just malformed counts/header. */
    FIL f;
    UINT n;
    assert(f_open(&f, "/sshclient/settings.bin", FA_WRITE) == FR_OK);
    assert(f_lseek(&f, 64) == FR_OK);
    const uint8_t corrupt = 0xff;
    assert(f_write(&f, &corrupt, 1, &n) == FR_OK && n == 1);
    assert(f_close(&f) == FR_OK);
    assert(!storage_init(&reloaded));
    assert(!storage_save(&settings)); /* Never overwrite damaged settings silently. */
    assert(f_unlink("/sshclient/settings.bin") == FR_OK);
    assert(storage_init(&reloaded));
    assert(storage_save(&settings));
    fail_writes = true;
    assert(!storage_save(&settings));
    fail_writes = false;
    assert(storage_init(&reloaded) && reloaded.wifi_count == 1);
    /* Device preferences are separate, CRC-protected and fall back to safe defaults. */
    prefs_t prefs, loaded;
    assert(storage_load_prefs(&loaded) && loaded.screen_brightness == 30 &&
           loaded.keyboard_brightness == KBD_LIGHT_UNCHANGED && loaded.auto_join && loaded.auto_ssh);
    prefs_defaults(&prefs);
    prefs.screen_brightness = 70;
    prefs.keyboard_brightness = 0;
    prefs.auto_ssh = 0;
    assert(storage_save_prefs(&prefs) && storage_save_prefs(&prefs));
    assert(storage_load_prefs(&loaded) && !memcmp(&prefs, &loaded, sizeof(prefs)));
    assert(storage_init(&reloaded) && reloaded.wifi_count == 1); /* Profiles are untouched. */
    prefs.screen_brightness = 0;
    assert(!storage_save_prefs(&prefs));
    prefs.screen_brightness = 70;
    fail_writes = true;
    assert(!storage_save_prefs(&prefs));
    fail_writes = false;
    assert(storage_load_prefs(&loaded) && loaded.screen_brightness == 70);
    /* Interrupted after removing the old file: the synced temp copy is used. */
    assert(f_unlink("/sshclient/prefs.tmp") == FR_OK); /* Stale temp from the failed write is ignored. */
    assert(f_rename("/sshclient/prefs.bin", "/sshclient/prefs.tmp") == FR_OK);
    assert(storage_load_prefs(&loaded) && loaded.screen_brightness == 70);
    assert(storage_save_prefs(&prefs) && f_stat("/sshclient/prefs.tmp", &info) == FR_NO_FILE);
    assert(f_open(&f, "/sshclient/prefs.bin", FA_WRITE) == FR_OK);
    assert(f_lseek(&f, 16) == FR_OK);
    assert(f_write(&f, &corrupt, 1, &n) == FR_OK && n == 1);
    assert(f_close(&f) == FR_OK);
    assert(!storage_load_prefs(&loaded) && strstr(storage_error(), "defaults"));
    assert(loaded.screen_brightness == 30);
    assert(storage_save_prefs(&prefs) && storage_load_prefs(&loaded) && loaded.screen_brightness == 70);
    assert(f_mkdir("/keys") == FR_OK);
    write_file("/keys/id.pem", "example test file", 17);
    uint8_t *data = NULL;
    size_t size;
    assert(storage_read_key("/keys/id.pem", &data, &size) && size == 17);
    assert(!memcmp(data, "example test file", 17));
    free(data);
    assert(!storage_read_key("/keys/missing.pem", &data, &size) && !data);
    write_file("/keys/empty.pem", "", 0);
    assert(!storage_read_key("/keys/empty.pem", &data, &size));
    write_file("/keys/third.pem", "test", 4);
    file_entry_t entries[2];
    unsigned count;
    bool more;
    assert(storage_list("/keys", entries, 2, &count, 0, &more) && count == 2 && more);
    assert(storage_list("/keys", entries, 2, &count, 2, &more) && count == 1 && !more);
    assert(!storage_list("/missing", entries, 2, &count, 0, &more));
    puts("Real FAT persistence, deletion, backup recovery, corruption, write errors and key browsing passed.");
    return 0;
}

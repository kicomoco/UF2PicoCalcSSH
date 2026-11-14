#ifndef UPDATER_H
#define UPDATER_H
#include <stdbool.h>
#include <stddef.h>

typedef enum {
    UPDATE_IDLE,
    UPDATE_CHECKING,    /* fetching the latest release's SHA256SUMS */
    UPDATE_LOCATING,    /* finding the SD file this app was launched from */
    UPDATE_AVAILABLE,   /* newer release found; waiting for updater_install() */
    UPDATE_CURRENT,     /* already running the latest release */
    UPDATE_DOWNLOADING, /* streaming the UF2 to <target>.new */
    UPDATE_VERIFYING,   /* re-reading the download from the SD card */
    UPDATE_INSTALLING,  /* replacing the target and verifying it */
    UPDATE_DONE,        /* target replaced and verified */
    UPDATE_FAILED,
} update_state_t;

#define UPDATE_DEFAULT_DIR "/pico2-apps"
#define UPDATE_DEFAULT_FILE UPDATE_DEFAULT_DIR "/picocalc_ssh.uf2"

bool updater_check(const char *current_version);
bool updater_install(void);
/* Abandons a check or download (removing partial files); ignored while installing. */
void updater_cancel(void);
update_state_t updater_poll(void);
update_state_t updater_state(void);
bool updater_busy(void);
const char *updater_error(void);
const char *updater_version(void);
/* SD path that will be (or was) replaced, and whether it was positively identified. */
const char *updater_target(void);
bool updater_target_found(void);
size_t updater_done_bytes(void);
size_t updater_total_bytes(void);
#endif

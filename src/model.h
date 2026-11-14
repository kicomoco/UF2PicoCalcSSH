#ifndef MODEL_H
#define MODEL_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MAX_WIFI 16
#define MAX_PROFILES 24
#define MAX_HOSTS 32
#define KEY_PATH_LEN 256
typedef struct {
    char ssid[33];
    char password[65];
} wifi_profile_t;
typedef struct {
    char host[128];
    char port[6];
    char username[64];
    char key_path[KEY_PATH_LEN];
} ssh_profile_t;
typedef struct {
    char host[128];
    uint16_t port;
    uint8_t fingerprint[32];
} known_host_t;
typedef struct {
    uint32_t version;
    uint32_t wifi_count, ssh_count, host_count;
    wifi_profile_t wifi[MAX_WIFI];
    ssh_profile_t ssh[MAX_PROFILES];
    known_host_t hosts[MAX_HOSTS];
} settings_t;
#define KBD_LIGHT_UNCHANGED 255
typedef struct {
    uint32_t version;
    uint8_t screen_brightness;   /* percent, 10..100 in steps of 10 */
    uint8_t keyboard_brightness; /* percent 0..100, or KBD_LIGHT_UNCHANGED */
    uint8_t auto_join;           /* scan and join strongest known WiFi at startup */
    uint8_t auto_ssh;            /* open SSH page 3 s after WiFi connects */
} prefs_t;
void prefs_defaults(prefs_t *prefs);
bool prefs_valid(const prefs_t *prefs);
uint8_t backlight_level(unsigned percent);
void secure_clear(void *data, size_t size);
bool parse_port(const char *text, uint16_t *port);
bool valid_wifi(const wifi_profile_t *profile);
bool valid_ssh(const ssh_profile_t *profile);
bool settings_valid(const settings_t *settings);
int wifi_upsert(settings_t *settings, const wifi_profile_t *profile);
bool wifi_remember_connection(settings_t *settings, const wifi_profile_t *profile);
int ssh_upsert(settings_t *settings, const ssh_profile_t *profile);
/* Overwrites the profile at index (upserts when index < 0); returns its new index or -1. */
int ssh_replace(settings_t *settings, int index, const ssh_profile_t *profile);
void wifi_remove(settings_t *settings, unsigned index);
void ssh_remove(settings_t *settings, unsigned index);
void host_remove(settings_t *settings, unsigned index);
int host_find(const settings_t *settings, const char *host, uint16_t port);
#endif

#include "model.h"
#include <string.h>

void prefs_defaults(prefs_t *p) {
    memset(p, 0, sizeof(*p));
    p->version = 1;
    p->screen_brightness = 30;
    p->keyboard_brightness = KBD_LIGHT_UNCHANGED;
    p->auto_join = p->auto_ssh = 1;
}

bool prefs_valid(const prefs_t *p) {
    return p->version == 1 && p->screen_brightness >= 10 && p->screen_brightness <= 100 &&
           p->screen_brightness % 10 == 0 &&
           (p->keyboard_brightness == KBD_LIGHT_UNCHANGED ||
            (p->keyboard_brightness <= 100 && p->keyboard_brightness % 10 == 0)) &&
           p->auto_join <= 1 && p->auto_ssh <= 1;
}

/* The PicoCalc keyboard controller accepts backlight values 0..240. */
uint8_t backlight_level(unsigned percent) {
    return (uint8_t)((percent > 100 ? 100 : percent) * 240 / 100);
}

void secure_clear(void *data, size_t size) {
    volatile unsigned char *p = data;
    while (size--) *p++ = 0;
}

bool parse_port(const char *s, uint16_t *port) {
    unsigned value = 0;
    if (!s || !*s) return false;
    for (unsigned i = 0; s[i]; i++) {
        if (i >= 5 || s[i] < '0' || s[i] > '9') return false;
        value = value * 10 + (unsigned)(s[i] - '0');
    }
    if (!value || value > 65535) return false;
    *port = (uint16_t)value;
    return true;
}

static bool terminated(const char *s, size_t n) {
    return memchr(s, 0, n) != NULL;
}

bool valid_wifi(const wifi_profile_t *p) {
    if (!terminated(p->ssid, sizeof(p->ssid)) || !p->ssid[0] ||
        !terminated(p->password, sizeof(p->password))) return false;
    size_t n = strlen(p->password);
    if (n == 64) {
        for (size_t i = 0; i < n; i++) {
            char c = p->password[i];
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
                  (c >= 'A' && c <= 'F'))) return false;
        }
        return true;
    }
    return n == 0 || (n >= 8 && n <= 63);
}

bool valid_ssh(const ssh_profile_t *p) {
    uint16_t port;
    if (!terminated(p->host, sizeof(p->host)) || !p->host[0] ||
        !terminated(p->port, sizeof(p->port)) || !parse_port(p->port, &port) ||
        !terminated(p->username, sizeof(p->username)) || !p->username[0] ||
        !terminated(p->key_path, sizeof(p->key_path))) return false;
    for (const char *s = p->host; *s; s++)
        if ((unsigned char)*s <= 32 || (unsigned char)*s >= 127) return false;
    return true;
}

bool settings_valid(const settings_t *s) {
    if ((s->version != 1 && s->version != 2) || s->wifi_count > MAX_WIFI ||
        s->ssh_count > MAX_PROFILES || s->host_count > MAX_HOSTS) return false;
    for (unsigned i = 0; i < s->wifi_count; i++)
        if (!valid_wifi(&s->wifi[i])) return false;
    for (unsigned i = 0; i < s->ssh_count; i++)
        if (!valid_ssh(&s->ssh[i])) return false;
    for (unsigned i = 0; i < s->host_count; i++)
        if (!terminated(s->hosts[i].host, sizeof(s->hosts[i].host)) ||
            !s->hosts[i].host[0] || !s->hosts[i].port) return false;
    return true;
}

bool wifi_remember_connection(settings_t *s, const wifi_profile_t *p) {
    int index = wifi_upsert(s, p);
    if (index < 0) return false;
    wifi_profile_t connected = s->wifi[index];
    memmove(&s->wifi[1], &s->wifi[0], (size_t)index * sizeof(s->wifi[0]));
    s->wifi[0] = connected;
    secure_clear(&connected, sizeof(connected));
    /* Version 2 preserves the original layout; slot zero is the last successful network. */
    s->version = 2;
    return true;
}

int wifi_upsert(settings_t *s, const wifi_profile_t *p) {
    if (!valid_wifi(p)) return -1;
    unsigned i;
    for (i = 0; i < s->wifi_count; i++)
        if (!strcmp(s->wifi[i].ssid, p->ssid)) break;
    if (i == MAX_WIFI) return -1;
    s->wifi[i] = *p;
    if (i == s->wifi_count) s->wifi_count++;
    return (int)i;
}

int ssh_upsert(settings_t *s, const ssh_profile_t *p) {
    if (!valid_ssh(p)) return -1;
    unsigned i;
    for (i = 0; i < s->ssh_count; i++)
        if (!strcmp(s->ssh[i].host, p->host) &&
            !strcmp(s->ssh[i].port, p->port) &&
            !strcmp(s->ssh[i].username, p->username)) break;
    if (i == MAX_PROFILES) return -1;
    s->ssh[i] = *p;
    if (i == s->ssh_count) s->ssh_count++;
    return (int)i;
}

int ssh_replace(settings_t *s, int index, const ssh_profile_t *p) {
    if (index < 0 || (unsigned)index >= s->ssh_count) return ssh_upsert(s, p);
    if (!valid_ssh(p)) return -1;
    /* If the edit now matches another profile, merge them rather than keep a duplicate. */
    for (unsigned i = 0; i < s->ssh_count; i++)
        if ((int)i != index && !strcmp(s->ssh[i].host, p->host) &&
            !strcmp(s->ssh[i].port, p->port) && !strcmp(s->ssh[i].username, p->username)) {
            ssh_remove(s, i);
            if ((int)i < index) index--;
            break;
        }
    s->ssh[index] = *p;
    return index;
}

#define REMOVE_BODY(member, count) \
    if (index >= s->count) return; \
    memmove(&s->member[index], &s->member[index + 1], \
            (s->count - index - 1) * sizeof(s->member[0])); \
    secure_clear(&s->member[--s->count], sizeof(s->member[0]))

void wifi_remove(settings_t *s, unsigned index) {
    if (index == 0 && s->wifi_count) s->version = 1;
    REMOVE_BODY(wifi, wifi_count);
}
void ssh_remove(settings_t *s, unsigned index) { REMOVE_BODY(ssh, ssh_count); }
void host_remove(settings_t *s, unsigned index) { REMOVE_BODY(hosts, host_count); }

int host_find(const settings_t *s, const char *host, uint16_t port) {
    for (unsigned i = 0; i < s->host_count; i++)
        if (s->hosts[i].port == port && !strcmp(s->hosts[i].host, host)) return (int)i;
    return -1;
}

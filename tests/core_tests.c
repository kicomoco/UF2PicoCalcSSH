#include "model.h"
#include "terminal.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static settings_t settings;
static terminal_t t;
static char response[64];
static bool reply(const void *p, unsigned n) {
    assert(n < sizeof(response));
    memcpy(response, p, n);
    response[n] = 0;
    return true;
}
static void feed(const char *s) { terminal_feed(&t, (const uint8_t *)s, strlen(s), reply); }
static void profiles(void) {
    uint16_t port = 0;
    assert(parse_port("22", &port) && port == 22);
    assert(parse_port("65535", &port) && port == 65535);
    assert(!parse_port("0", &port) && !parse_port("65536", &port));
    assert(!parse_port("-1", &port) && !parse_port("22x", &port));
    assert(!parse_port("", &port) && !parse_port("999999999", &port));
    settings.version = 1;
    wifi_profile_t w = {.ssid = "home", .password = "password"};
    assert(wifi_upsert(&settings, &w) == 0);
    strcpy(w.password, "newpassword");
    assert(wifi_upsert(&settings, &w) == 0 && settings.wifi_count == 1);
    strcpy(w.ssid, "office");
    assert(wifi_upsert(&settings, &w) == 1);
    assert(!strcmp(settings.wifi[0].ssid, "home"));
    assert(settings.version == 1);
    assert(wifi_remember_connection(&settings, &w));
    assert(settings.version == 2 && settings_valid(&settings));
    assert(!strcmp(settings.wifi[0].ssid, "office"));
    assert(!strcmp(settings.wifi[1].ssid, "home"));
    assert(wifi_remember_connection(&settings, &w) && settings.wifi_count == 2);
    wifi_remove(&settings, 1);
    assert(settings.version == 2 && settings.wifi_count == 1);
    strcpy(w.ssid, "home");
    assert(wifi_remember_connection(&settings, &w) && settings.wifi_count == 2);
    assert(!strcmp(settings.wifi[0].ssid, "home"));
    strcpy(w.password, "short");
    assert(!valid_wifi(&w));
    w.password[0] = 0;
    assert(valid_wifi(&w));
    memset(w.password, 'a', 64); w.password[64] = 0;
    assert(valid_wifi(&w));
    w.password[0] = 'z';
    assert(!valid_wifi(&w));
    wifi_remove(&settings, 0);
    assert(settings.wifi_count == 1 && !strcmp(settings.wifi[0].ssid, "office"));
    assert(settings.version == 1); /* Forgetting the last connection must not select another. */
    assert(settings.wifi[1].password[0] == 0);
    w.password[0] = 0;
    for (unsigned i = 1; i < MAX_WIFI; i++) {
        snprintf(w.ssid, sizeof(w.ssid), "network%u", i);
        assert(wifi_upsert(&settings, &w) == (int)i);
    }
    strcpy(w.ssid, "full");
    assert(wifi_upsert(&settings, &w) == -1);
    assert(!wifi_remember_connection(&settings, &w) && settings.version == 1);
    strcpy(w.ssid, "network7");
    assert(wifi_remember_connection(&settings, &w) && settings.wifi_count == MAX_WIFI);
    assert(!strcmp(settings.wifi[0].ssid, "network7"));
    wifi_remove(&settings, MAX_WIFI);
    assert(settings.version == 2 && settings.wifi_count == MAX_WIFI);
    ssh_profile_t p = {.host = "example.com", .port = "22", .username = "alice", .key_path = "/keys/id.pem"};
    assert(ssh_upsert(&settings, &p) == 0);
    strcpy(p.key_path, "/keys/new.pem");
    assert(ssh_upsert(&settings, &p) == 0 && settings.ssh_count == 1);
    strcpy(p.port, "2222");
    assert(ssh_upsert(&settings, &p) == 1 && settings.ssh_count == 2);
    strcpy(p.username, "bob");
    assert(ssh_upsert(&settings, &p) == 2);
    ssh_remove(&settings, 1);
    assert(settings.ssh_count == 2 && !strcmp(settings.ssh[1].username, "bob"));
    assert(settings_valid(&settings));
    memset(settings.ssh[0].host, 'x', sizeof(settings.ssh[0].host));
    assert(!settings_valid(&settings));
    memset(&settings, 0, sizeof(settings));
    settings.version = 1;
    strcpy(settings.hosts[0].host, "example.com");
    settings.hosts[0].port = 22;
    settings.host_count = 1;
    assert(host_find(&settings, "example.com", 22) == 0);
    assert(host_find(&settings, "example.com", 2222) == -1);
    host_remove(&settings, 0);
    assert(settings.host_count == 0 && !settings.hosts[0].port);
    settings.version = 2;
    assert(settings_valid(&settings));
    settings.version = 3;
    assert(!settings_valid(&settings));
}
static void terminal_tests(void) {
    terminal_init(&t);
    feed("abc\r\nxyz");
    assert(t.cells[0][0].ch == 'a' && t.cells[1][0].ch == 'x');
    feed("\033[2J\033[H");
    assert(t.cells[1][0].ch == ' ');
    feed("\033[31mR\033[0mN");
    assert(t.cells[0][0].fg != t.cells[0][1].fg);
    feed("\033[24;53HZQ");
    assert(t.cells[22][52].ch == 'Z' && t.cells[23][0].ch == 'Q');
    feed("\033[Habc\033[2D\033[P");
    assert(t.cells[0][1].ch == 'c');
    feed("\033[H\033[6n");
    assert(!strcmp(response, "\033[1;1R"));
    feed("\033[999;999H");
    assert(t.x == TERM_COLS - 1 && t.y == TERM_ROWS - 1);
    feed("\033[Hkeep\033[?1049hother\033[?1049l");
    assert(t.cells[0][0].ch == 'k' && t.cells[0][3].ch == 'p');
    feed("\033[?25l"); assert(!t.cursor);
    feed("\033[?25h"); assert(t.cursor);
    feed("\033]0;title\033"); feed("\\\033[HZ");
    assert(t.cells[0][0].ch == 'Z');
    feed("\033[H\xc3"); feed("\xa9");
    assert(t.cells[0][0].ch == 0xe9);
    feed("\033[2;4r\033[?6h\033[H");
    assert(t.y == 1);
    feed("\033[?6l\033[r");
    assert(t.top == 0 && t.bottom == TERM_ROWS - 1);
    feed("\033[38;5;196m\033[48;2;0;255;0mX");
    uint8_t bytes[16];
    assert(terminal_key(&t, (key_event_t){KEY_UP, 0}, bytes) == 3 && !memcmp(bytes, "\033[A", 3));
    feed("\033[?1h");
    assert(terminal_key(&t, (key_event_t){KEY_UP, 0}, bytes) == 3 && !memcmp(bytes, "\033OA", 3));
    assert(terminal_key(&t, (key_event_t){'c', MOD_CTRL}, bytes) == 1 && bytes[0] == 3);
    assert(terminal_key(&t, (key_event_t){KEY_F10, 0}, bytes) == 5 && !memcmp(bytes, "\033[21~", 5));
    /* Exercise arbitrary, split escape streams under the sanitizers. */
    uint32_t random = 1234;
    for (unsigned i = 0; i < 200000; i++) {
        random = random * 1664525u + 1013904223u;
        uint8_t byte = (uint8_t)(random >> 24);
        terminal_feed(&t, &byte, 1, reply);
        assert(t.x >= 0 && t.x < TERM_COLS && t.y >= 0 && t.y < TERM_ROWS);
    }
}
int main(void) {
    profiles();
    terminal_tests();
    prefs_t prefs;
    prefs_defaults(&prefs);
    assert(prefs_valid(&prefs) && backlight_level(prefs.screen_brightness) == 72);
    assert(backlight_level(100) == 240 && backlight_level(0) == 0 && backlight_level(250) == 240);
    prefs.screen_brightness = 0; assert(!prefs_valid(&prefs));
    prefs.screen_brightness = 35; assert(!prefs_valid(&prefs));
    prefs.screen_brightness = 100; prefs.keyboard_brightness = 110; assert(!prefs_valid(&prefs));
    prefs.keyboard_brightness = 0; prefs.auto_join = 2; assert(!prefs_valid(&prefs));
    prefs.auto_join = 0; prefs.version = 2; assert(!prefs_valid(&prefs));
    {
        settings_t e;
        memset(&e, 0, sizeof(e)); e.version = 1;
        ssh_profile_t x = {.host = "a", .port = "22", .username = "u"}, y = {.host = "b", .port = "22", .username = "v"};
        assert(ssh_replace(&e, -1, &x) == 0 && ssh_replace(&e, -1, &y) == 1);
        ssh_profile_t z = {.host = "c", .port = "2200", .username = "w"};
        assert(ssh_replace(&e, 0, &z) == 0 && e.ssh_count == 2 && !strcmp(e.ssh[0].host, "c"));
        assert(ssh_replace(&e, 1, &z) == 0 && e.ssh_count == 1); /* Merged with the duplicate. */
        assert(ssh_replace(&e, 0, &y) == 0 && ssh_replace(&e, -1, &x) == 1);
        assert(ssh_replace(&e, 0, &x) == 0 && e.ssh_count == 1 && !strcmp(e.ssh[0].host, "a"));
        z.host[0] = 0;
        assert(ssh_replace(&e, 0, &z) < 0 && !strcmp(e.ssh[0].host, "a"));
        assert(ssh_replace(&e, 7, &y) == 1 && e.ssh_count == 2); /* Stale index: upsert. */
    }
    puts("Profile, WiFi gating, host key and terminal tests passed.");
    return 0;
}

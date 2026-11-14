#include "app.h"
#include "model.h"
#include "network.h"
#include "ssh_client.h"
#include "storage.h"
#include "system.h"
#include "updater.h"
#include "ui/gfx.h"
#include "hw/keyboard.h"
#include "pico/stdlib.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static settings_t disk;
static wifi_profile_t joined;
static ssh_profile_t connected_profile;
static unsigned saves, connects, joins;
static bool online, joining, active, established, wifi_failed, save_ok = true;
static bool connect_ok = true, network_ok = true, storage_ok = true;
static uint8_t sent[16];
static unsigned sent_size;
static char supplied_password[128], rendered[10000];
static host_check_fn host_verify;
static uint64_t clock_us;
static key_event_t queue[512];
static unsigned head, tail;
static wifi_scan_result_t scan_results[16];
static unsigned scan_count, scans;
static bool scanning, scan_start_ok = true;
static bool scan_timed_out, ssh_fails;
static prefs_t disk_prefs;
static bool prefs_exist, prefs_save_ok = true, has_kbd_light = true;
static unsigned prefs_saves;
static int lcd_level = -1, kbd_level = -1, battery_level = 87;
static bool battery_charging = true;
static int popup_x = -1, popup_y = -1, popup_bottom = -1, centre_errors;
uint16_t lcd_fb[LCD_W * LCD_H];
static update_state_t upd_state;
static bool upd_found = true;
static unsigned upd_checks, upd_installs, upd_cancels, reboots;
static size_t upd_done, upd_total;
static char upd_error[64];
bool updater_check(const char *version) {
    assert(!strcmp(version, "9.8.7-test") && !updater_busy());
    upd_checks++;
    upd_state = UPDATE_CHECKING;
    return true;
}
bool updater_install(void) {
    assert(upd_state == UPDATE_AVAILABLE);
    upd_installs++;
    upd_state = UPDATE_DOWNLOADING;
    return true;
}
void updater_cancel(void) {
    upd_cancels++;
    if (upd_state != UPDATE_VERIFYING && upd_state != UPDATE_INSTALLING) upd_state = UPDATE_IDLE;
}
update_state_t updater_poll(void) { return upd_state; }
update_state_t updater_state(void) { return upd_state; }
bool updater_busy(void) {
    return upd_state == UPDATE_CHECKING || upd_state == UPDATE_LOCATING || upd_state == UPDATE_DOWNLOADING ||
           upd_state == UPDATE_VERIFYING || upd_state == UPDATE_INSTALLING;
}
const char *updater_error(void) { return upd_error; }
const char *updater_version(void) { return "9.9.0"; }
const char *updater_target(void) { return "/pico2-apps/Utilities/PicoCalc SSH client (latest).uf2"; }
bool updater_target_found(void) { return upd_found; }
size_t updater_done_bytes(void) { return upd_done; }
size_t updater_total_bytes(void) { return upd_total; }
void system_reboot_to_loader(void) { reboots++; }
const font_t font_6x12 = {6, 12, NULL};

absolute_time_t get_absolute_time(void) { return clock_us; }
absolute_time_t make_timeout_time_ms(uint32_t ms) { return clock_us + (uint64_t)ms * 1000; }
bool time_reached(absolute_time_t t) { return clock_us >= t; }
void sleep_ms(uint32_t ms) { clock_us += (uint64_t)ms * 1000; }
void kbd_poll(void) {}
bool kbd_get(key_event_t *ev) {
    if (head == tail) return false;
    *ev = queue[head++];
    return true;
}
bool kbd_take_power_cycle(void) { return false; }
void kbd_set_lcd_backlight(uint8_t level) { lcd_level = level; }
void kbd_set_kbd_backlight(uint8_t level) { assert(has_kbd_light); kbd_level = level; }
bool kbd_has_kbd_backlight(void) { return has_kbd_light; }
int kbd_battery_percent(bool *charging) { *charging = battery_charging; return battery_level; }
bool storage_load_prefs(prefs_t *p) {
    if (prefs_exist) *p = disk_prefs;
    else prefs_defaults(p);
    return true;
}
bool storage_save_prefs(const prefs_t *p) {
    if (!prefs_save_ok) return false;
    assert(prefs_valid(p));
    disk_prefs = *p;
    prefs_exist = true;
    prefs_saves++;
    return true;
}
void lcd_flush(void) {}
void lcd_reinit(void) {}
void gfx_clear(uint16_t c) { (void)c; rendered[0] = 0; popup_x = popup_y = popup_bottom = -1; centre_errors = 0; }
void gfx_fill(int x, int y, int w, int h, uint16_t c) { (void)x; (void)y; (void)w; (void)h; (void)c; }
void gfx_hline(int x, int y, int w, uint16_t c) { (void)x; (void)y; (void)w; (void)c; }
int gfx_text_n(int x, int y, const char *s, int n, const font_t *f, uint16_t fg, int32_t bg) {
    (void)f; (void)fg;
    size_t left = sizeof(rendered) - strlen(rendered) - 2;
    size_t count = strlen(s);
    if (count > (size_t)n) count = (size_t)n;
    assert(x >= 0 && y >= 0 && y + 12 <= LCD_H && x + (int)count * 6 <= LCD_W);
    assert(count <= left);
    if (count == 46 && s[0] == '+' && s[45] == '+') { /* Popup frame edge. */
        if (popup_y < 0 || y < popup_y) { popup_x = x; popup_y = y; }
        if (y > popup_bottom) popup_bottom = y;
    }
    /* Popup text rows: left and right margins inside the frame must match (to one cell). */
    if (popup_x >= 0 && y > popup_y && x > popup_x + 6 && x + (int)count * 6 < popup_x + 270 &&
        bg == TRANSPARENT && popup_bottom > popup_y) {
        int left_gap = x - popup_x, right_gap = popup_x + 276 - (x + (int)count * 6);
        if (left_gap - right_gap > 6 || right_gap - left_gap > 6) centre_errors++;
    }
    size_t used = strlen(rendered);
    assert(used + count + 2 <= sizeof(rendered));
    memcpy(rendered + used, s, count);
    memcpy(rendered + used + count, "\n", 2);
    return x + (int)count * 6;
}
int gfx_text_fit(int x, int y, const char *s, int w, const font_t *f, uint16_t fg, int32_t bg) {
    return gfx_text_n(x, y, s, w / 6, f, fg, bg);
}
int gfx_text(int x, int y, const char *s, const font_t *f, uint16_t fg, int32_t bg) {
    return gfx_text_n(x, y, s, 1000, f, fg, bg);
}
bool storage_init(settings_t *settings) { *settings = disk; return storage_ok; }
bool storage_save(const settings_t *settings) {
    if (!save_ok) return false;
    assert(settings_valid(settings));
    disk = *settings;
    saves++;
    return true;
}
const char *storage_error(void) { return "Simulated SD write failure"; }
bool storage_list(const char *path, file_entry_t *entries, unsigned capacity,
                  unsigned *count, unsigned skip, bool *more) {
    assert(!strcmp(path, "/") && capacity >= 1 && skip == 0);
    strcpy(entries[0].name, "id.pem");
    entries[0].directory = false;
    *count = 1; *more = false;
    return true;
}
bool network_init(void) { return network_ok; }
void network_poll(void) {}
bool wifi_connect(const wifi_profile_t *p) {
    joined = *p;
    joins++;
    joining = connect_ok;
    wifi_failed = !connect_ok;
    online = false;
    return connect_ok;
}
void wifi_disconnect(void) { online = joining = false; }
bool wifi_ready(void) { return online; }
bool wifi_busy(void) { return joining; }
const char *wifi_address(void) { return "192.168.1.50"; }
const char *wifi_status(void) {
    return online ? "WiFi ready" : joining ? "Joining" :
           wifi_failed ? "WiFi join failed / network unavailable" : "WiFi disconnected";
}
bool wifi_scan_start(void) {
    scans++;
    scan_count = 0;
    scan_timed_out = false;
    scanning = scan_start_ok;
    return scan_start_ok;
}
bool wifi_scan_busy(void) { return scanning; }
const char *wifi_scan_status(void) {
    return !scan_start_ok ? "Cannot start WiFi scan" : scan_timed_out ? "WiFi scan timed out" : scanning ? "Scanning..." :
           scan_count ? "Scan complete" : "No networks found";
}
const wifi_scan_result_t *wifi_scan_results(unsigned *count) { *count = scan_count; return scan_results; }
bool ssh_start(const ssh_profile_t *p, const char *pw, host_check_fn verify) {
    assert(online);
    connected_profile = *p;
    strcpy(supplied_password, pw);
    host_verify = verify;
    active = true;
    connects++;
    return true;
}
void ssh_stop(void) { active = established = false; }
void ssh_poll(void) { if (ssh_fails) active = established = ssh_fails = false; }
bool ssh_active(void) { return active; }
bool ssh_connected(void) { return established; }
const char *ssh_status(void) { return "Resolving host / connecting TCP..."; }
bool ssh_send(const void *p, unsigned n) {
    assert(n <= sizeof(sent));
    memcpy(sent, p, n);
    sent_size = n;
    return true;
}
int ssh_read(void *p, unsigned n) { (void)p; (void)n; return 0; }

static void enqueue(uint8_t key) {
    if (head == tail) head = tail = 0;
    assert(tail < sizeof(queue) / sizeof(queue[0]));
    queue[tail++] = (key_event_t){key, 0};
}
static void tick(void) { clock_us += 50000; app_poll(); }
static void press(uint8_t key) { enqueue(key); tick(); }
static void type(const char *s) { while (*s) press((uint8_t)*s++); }
static void confirmed_delete(void) { enqueue(KEY_F8); enqueue(KEY_ENTER); tick(); }
static void sees(const char *s) {
    if (!strstr(rendered, s)) fprintf(stderr, "Expected '%s' in:\n%s", s, rendered);
    assert(strstr(rendered, s));
}
static void not_sees(const char *s) {
    if (strstr(rendered, s)) fprintf(stderr, "Unexpected '%s' in:\n%s", s, rendered);
    assert(!strstr(rendered, s));
}
static void open_wifi_saved(void) {
    press(KEY_F1);
    sees("Saved WiFi networks");
}
static void open_ssh_saved(void) {
    press(KEY_F2);
    sees("Saved SSH connections");
}
static void open_scan(void) {
    press(KEY_F1);
    for (unsigned i = 0; i < disk.wifi_count; i++) press(KEY_DOWN);
    press(KEY_ENTER); press(KEY_DOWN); press(KEY_DOWN); press(KEY_ENTER);
    sees("Nearby WiFi networks");
}
static void reboot(void) {
    online = joining = wifi_failed = active = established = scanning = false;
    app_init(); tick();
}
static void advance(unsigned ms) { clock_us += (uint64_t)ms * 1000; app_poll(); }
static void result(unsigned index, const char *ssid, int rssi, unsigned auth) {
    assert(index < sizeof(scan_results) / sizeof(*scan_results));
    snprintf(scan_results[index].ssid, sizeof(scan_results[index].ssid), "%s", ssid);
    scan_results[index].rssi = rssi;
    scan_results[index].auth_mode = auth;
    if (scan_count <= index) scan_count = index + 1;
}
static void complete_scan(void) { scanning = false; tick(); }
static void wifi_success(void) { online = true; joining = false; tick(); }
static char spin_after(const char *prefix) {
    const char *at = strstr(rendered, prefix);
    if (!at) fprintf(stderr, "Expected '%s' in:\n%s", prefix, rendered);
    assert(at);
    char c = at[strlen(prefix)];
    assert(c && strchr("/-\\|", c));
    return c;
}

int main(void) {
    disk.version = 1;
    wifi_profile_t w = {.ssid = "home", .password = "password"};
    assert(wifi_upsert(&disk, &w) == 0);
    strcpy(w.ssid, "office");
    assert(wifi_upsert(&disk, &w) == 1);
    ssh_profile_t p = {.host = "saved.example", .port = "22", .username = "saved"};
    assert(ssh_upsert(&disk, &p) == 0);
    app_init(); tick();
    sees("Saved WiFi networks");
    sees("+--------------------------------------------------+");
    sees("[ Add new WiFi connection ]");
    sees("home"); sees("office");
    sees("Scanning...");
    sees("F5 Join WiFi");
    sees("F1 WiFi  F2 SSH  F3 Host Keys  F4 Settings  F10 Help\n");
    not_sees("Username");
    assert(joins == 0 && scans == 1); /* Legacy profiles also participate in startup scanning. */
    result(0, "office", -70, 5);
    result(1, "unknown", -10, 5);
    result(2, "home", -60, 5);
    result(3, "office", -30, 5); /* Unsorted results: choose maximum RSSI, not first match. */
    result(4, "home", -5, 0); /* An open AP must not reuse protected credentials. */
    result(5, "home", -1, 1); /* WEP is not supported. */
    complete_scan();
    assert(joining && joins == 1 && !strcmp(joined.ssid, "office"));
    assert(!strcmp(joined.password, "password"));
    unsigned before = saves;
    wifi_success();
    assert(saves == before + 1 && disk.version == 2 && !strcmp(disk.wifi[0].ssid, "office"));
    sees("SSH in 3...");
    advance(1040); sees("SSH in 2...");
    advance(960); sees("SSH in 1...");
    advance(999); sees("Saved WiFi networks");
    advance(40); sees("Saved SSH connections"); /* Allow one display frame at the deadline. */
    assert(connects == 0); /* Moving to SSH must not select a preset automatically. */
    press(KEY_F10); sees("Help"); press(KEY_ESC); sees("Saved SSH connections");
    online = false; tick();
    press(KEY_F2);
    sees("SSH locked");
    sees("[ Add new SSH connection ]");
    not_sees("SSID");
    open_ssh_saved(); press(KEY_ENTER);
    sees("Connect WiFi first");
    assert(connects == 0);
    press(KEY_DOWN); press(KEY_ENTER); sees("Connect WiFi first");
    before = saves;
    open_wifi_saved(); press(KEY_ENTER);
    assert(joining && !online && !strcmp(joined.ssid, "office"));
    wifi_success();
    assert(saves == before && disk.wifi_count == 2);
    sees("SSH in 3...");
    press(KEY_ESC); advance(3100); sees("Saved WiFi networks");
    press(KEY_F2); press(KEY_DOWN); press(KEY_ENTER); type("new.example");
    press(KEY_DOWN); press(KEY_DOWN); type("alice");
    press(KEY_DOWN); type("secret");
    press(KEY_F6);
    assert(disk.ssh_count == 2);
    assert(!strcmp(disk.ssh[1].host, "new.example"));
    press(KEY_F4); sees(":: Settings"); press(KEY_ESC);
    press(KEY_F2); press(KEY_DOWN); press('e'); sees("new.example");
    type("secret"); press(KEY_DOWN);
    press(KEY_ENTER); sees("Select private key"); /* F4 is Settings; Enter on Private key browses. */
    press(KEY_ENTER); press(KEY_F6);
    assert(!strcmp(disk.ssh[1].key_path, "/id.pem"));
    press(KEY_F10); sees("Help");
    assert(connects == 0); /* F10 is help, never connect. */
    press(KEY_ESC); sees(":: SSH connection");
    press(KEY_F9);
    assert(connects == 1 && !strcmp(supplied_password, "secret"));
    assert(!strcmp(connected_profile.key_path, "/id.pem"));
    uint8_t hash[32] = {1};
    enqueue(KEY_ENTER);
    assert(host_verify("new.example", 22, hash));
    assert(disk.host_count == 1);
    before = saves;
    assert(host_verify("new.example", 22, hash) && saves == before);
    hash[0] = 2;
    assert(!host_verify("new.example", 22, hash) && saves == before);
    press(KEY_ESC); /* Cancel the mocked connection. */
    press(KEY_F3); sees("Trusted host keys"); confirmed_delete();
    assert(disk.host_count == 0);
    before = connects;
    open_ssh_saved(); press(KEY_DOWN); press(KEY_ENTER);
    assert(connects == 2 && supplied_password[0] == 0); /* Password was not saved. */
    assert(connects == before + 1 && !strcmp(connected_profile.host, "new.example"));
    established = true; tick();
    sees("Ctrl+] disconnect");
    press(KEY_F10);
    assert(sent_size == 5 && !memcmp(sent, "\033[21~", 5));
    sees("Ctrl+] disconnect"); /* Navigation keys still go to the server. */
    press(29); sees("Saved SSH connections");
    press('e'); sees(":: SSH connection");
    sees("Profile loaded");
    assert(connects == before + 1); /* Editing does not start a connection. */
    press(KEY_ESC);
    open_ssh_saved(); press(KEY_DOWN); confirmed_delete();
    assert(disk.ssh_count == 1);
    open_wifi_saved();
    save_ok = false;
    confirmed_delete();
    assert(disk.wifi_count == 2);
    sees("Simulated SD write failure");
    save_ok = true;
    confirmed_delete();
    assert(disk.version == 1 && disk.wifi_count == 1 && !strcmp(disk.wifi[0].ssid, "home") && !online);
    press(KEY_F2); sees("SSH locked");
    before = saves;
    press(KEY_F6);
    assert(saves == before);
    open_wifi_saved(); confirmed_delete();
    assert(disk.wifi_count == 0);
    sees("No saved entries");
    before = saves;
    open_scan();
    sees("Scanning...");
    assert(!online);
    strcpy(scan_results[0].ssid, "Cafe");
    scan_results[0].rssi = -42;
    scan_results[0].auth_mode = 0;
    strcpy(scan_results[1].ssid, "Secure");
    scan_results[1].rssi = -60;
    scan_results[1].auth_mode = 5;
    strcpy(scan_results[2].ssid, "Legacy");
    scan_results[2].auth_mode = 1;
    scan_results[2].rssi = -70;
    scan_count = 3;
    press(KEY_ENTER);
    sees("Wait for the scan");
    scanning = false; tick();
    sees("Scan complete");
    sees("Open -42 dBm");
    press(KEY_DOWN); press(KEY_DOWN); press(KEY_ENTER);
    sees("WEP is not supported");
    press(KEY_UP); press(KEY_ENTER);
    sees("Secure");
    sees(":: WiFi");
    assert(saves == before); /* Selecting a result does not save it. */
    type("securepass");
    press(KEY_F5);
    assert(joining && !strcmp(joined.ssid, "Secure") && !strcmp(joined.password, "securepass"));
    online = true; joining = false; tick();
    assert(disk.wifi_count == 1 && !strcmp(disk.wifi[0].ssid, "Secure"));
    press(KEY_F1); press(KEY_DOWN); press(KEY_DOWN); press(KEY_ENTER); /* Disconnect button. */
    assert(!online);
    open_scan();
    result(0, "Cafe", -42, 0);
    result(1, "Secure", -60, 5);
    scanning = false; tick();
    press(KEY_ENTER); /* Select open network after protected one. */
    sees("No password required");
    press(KEY_F5);
    assert(!strcmp(joined.ssid, "Cafe") && joined.password[0] == 0);
    press(KEY_ESC); /* Cancel joining. */
    open_scan(); result(0, "Cafe", -42, 0); result(1, "Secure", -60, 5);
    scanning = false; tick();
    press(KEY_DOWN); press(KEY_ENTER); press(KEY_F5);
    assert(!strcmp(joined.ssid, "Secure") && !strcmp(joined.password, "securepass")); /* Saved password reused. */
    press(KEY_ESC);
    open_scan();
    scan_count = 0; scanning = false; tick();
    sees("No networks found");
    press(KEY_ENTER); sees("Nearby WiFi networks");
    press(KEY_TAB); sees("> [ Scan again ]");
    scan_start_ok = false;
    press(KEY_ENTER); sees("Cannot start WiFi scan");
    scan_start_ok = true;
    press(KEY_ENTER); assert(scanning);
    for (unsigned i = 3; i < 16; i++) {
        snprintf(scan_results[i].ssid, sizeof(scan_results[i].ssid), "Network%u", i);
        scan_results[i].auth_mode = 5;
        scan_results[i].rssi = -80;
    }
    scan_count = 16;
    scanning = false; tick();
    press(KEY_TAB); /* Back to the result list. */
    press(KEY_PAGE_DOWN); sees("Network12");
    press(KEY_PAGE_UP); sees("Cafe");
    press(KEY_TAB); press(KEY_ENTER); assert(scanning);
    press(KEY_ESC); sees("PicoCalc SSH"); /* Leaving the screen does not join. */
    assert(!joining);
    scanning = false;
    /* A failed join must not replace the previous successful startup network. */
    before = joins;
    reboot();
    assert(scanning && !joining && joins == before);
    result(0, "Secure", -60, 5); complete_scan();
    assert(joining && joins == before + 1 && !strcmp(joined.ssid, "Secure"));
    assert(!strcmp(joined.password, "securepass"));
    sees("Joining");
    before = saves;
    online = true; joining = false; tick();
    assert(saves == before); /* Startup reconnection does not rewrite unchanged SD data. */
    press(KEY_F2); not_sees("SSH locked");

    before = joins;
    reboot();
    result(0, "Secure", -60, 5); complete_scan();
    joining = false; wifi_failed = true; tick();
    sees("WiFi join failed / network unavailable");
    for (unsigned i = 0; i < 10; i++) tick();
    assert(joins == before + 1 && !strcmp(disk.wifi[0].ssid, "Secure"));
    press(KEY_F2); sees("SSH locked");
    press(KEY_F10); sees("Help"); press(KEY_ESC); sees("Saved SSH connections");

    reboot(); result(0, "Secure", -60, 5); complete_scan(); press(KEY_ESC);
    assert(!joining);
    sees("WiFi join cancelled");
    before = joins;
    for (unsigned i = 0; i < 10; i++) tick();
    assert(joins == before);
    /* The joining popup is modal: navigation/deletion is ignored until it finishes or Esc cancels. */
    reboot(); result(0, "Secure", -60, 5); complete_scan();
    sees("[ Connecting WiFi ]"); sees("Secure"); sees("Esc: cancel");
    press(KEY_F3); press(KEY_F8); press(KEY_DEL); press(KEY_ENTER);
    assert(joining && disk.wifi_count == 1 && joins == before + 1);
    sees("Saved WiFi networks"); sees("[ Connecting WiFi ]");
    press(KEY_ESC); assert(!joining); not_sees("[ Connecting WiFi ]");
    open_wifi_saved(); confirmed_delete();
    assert(disk.version == 1 && disk.wifi_count == 0);
    before = joins;
    reboot();
    complete_scan();
    assert(!joining && joins == before);

    /* Reordering on success survives a reboot. */
    strcpy(w.ssid, "home");
    assert(wifi_remember_connection(&disk, &w));
    strcpy(w.ssid, "office");
    assert(wifi_remember_connection(&disk, &w));
    reboot();
    result(0, "office", -70, 5); complete_scan();
    assert(!strcmp(joined.ssid, "office"));
    online = true; joining = false; tick();
    open_wifi_saved(); press(KEY_DOWN); confirmed_delete();
    assert(disk.wifi_count == 1);
    assert(disk.version == 2 && !strcmp(disk.wifi[0].ssid, "office"));
    /* A failed metadata write keeps the on-disk last connection unchanged. */
    press(KEY_F1); press('e');
    type("new");
    save_ok = false; press(KEY_F5);
    online = true; joining = false; tick();
    sees("Simulated SD write failure");
    assert(!strcmp(disk.wifi[0].ssid, "office"));
    save_ok = true;
    reboot(); result(0, "office", -70, 5); complete_scan();
    assert(!strcmp(joined.ssid, "office"));
    connect_ok = false; reboot(); result(0, "office", -70, 5); complete_scan();
    assert(!joining); sees("WiFi join failed");
    connect_ok = true;
    before = joins;
    storage_ok = false; reboot();
    assert(joins == before); sees("Simulated SD write failure");
    storage_ok = true; network_ok = false; reboot();
    assert(joins == before);
    network_ok = true;
    scan_start_ok = false; reboot();
    assert(joins == before && !joining); sees("Cannot start WiFi scan");
    scan_start_ok = true; reboot();
    result(0, "unknown", -10, 5); complete_scan();
    assert(joins == before && !joining); sees("No compatible known WiFi");
    advance(5000); sees("Saved WiFi networks");
    reboot(); press(KEY_ESC);
    result(0, "office", -10, 5); complete_scan();
    assert(joins == before); /* User input cancels startup auto-join, not the radio scan. */
    reboot(); complete_scan();
    assert(joins == before); sees("No networks found");
    reboot(); scan_timed_out = true; complete_scan();
    assert(joins == before); sees("WiFi scan timed out");
    /* Loss of WiFi cancels an active countdown. */
    reboot(); result(0, "office", -10, 5); complete_scan(); wifi_success();
    sees("SSH in 3...");
    online = false; tick(); advance(4000);
    sees("Saved WiFi networks"); not_sees("SSH in");
    /* Empty SSH lists still expose the secondary add action. */
    disk.ssh_count = 0; reboot(); complete_scan();
    online = true; tick();
    press(KEY_F2); sees("No saved entries");
    press(KEY_ENTER); sees(":: SSH connection"); sees("Host");
    /* Offline saved-profile deletion and a genuinely blank Add New form. */
    assert(ssh_upsert(&disk, &p) == 0);
    reboot(); complete_scan();
    press(KEY_F2); confirmed_delete();
    assert(disk.ssh_count == 0 && !online);
    sees("No saved entries");
    /* Saved-list paging must leave the secondary controls reachable at capacity. */
    memset(&disk, 0, sizeof(disk)); disk.version = 1;
    for (unsigned i = 0; i < MAX_WIFI; i++) {
        snprintf(w.ssid, sizeof(w.ssid), "Saved%u", i);
        assert(wifi_upsert(&disk, &w) == (int)i);
    }
    reboot(); complete_scan();
    press(KEY_PAGE_DOWN); sees("Saved9"); sees("Saved15");
    press(KEY_PAGE_DOWN); sees("> [ Disconnect WiFi ]");
    press(KEY_UP); sees("> [ Add new WiFi connection ]");
    press(KEY_ENTER); sees(":: WiFi"); not_sees("Saved15");
    /* Open saved WiFi can win, while a protected AP cannot reuse its empty password. */
    memset(&disk, 0, sizeof(disk)); disk.version = 1;
    wifi_profile_t open = {.ssid = "Open"};
    assert(wifi_upsert(&disk, &open) == 0);
    reboot(); result(0, "Open", -70, 0); result(1, "Open", -10, 5); complete_scan();
    assert(joining && !strcmp(joined.ssid, "Open") && joined.password[0] == 0);
    wifi_success(); sees("SSH in 3...");
    press(KEY_F10); advance(3100); sees("Help"); /* Countdown cannot hijack explicit navigation. */
    /* Deadline arithmetic also works across the 32-bit millisecond wrap. */
    clock_us = ((uint64_t)UINT32_MAX - 1000) * 1000;
    reboot(); result(0, "Open", -70, 0); complete_scan(); wifi_success();
    sees("SSH in 3...");
    advance(2999); sees("Saved WiFi networks");
    advance(40); sees("Saved SSH connections");
    /* Countdown popup keys: Enter opens SSH now, other keys stay (and are consumed), Fn navigates. */
    unsigned before_connects = connects;
    reboot(); result(0, "Open", -70, 0); complete_scan(); wifi_success();
    sees("[ WiFi connected ]"); sees("Open"); sees("Opening SSH in 3..."); sees("Enter: SSH now");
    press(KEY_ENTER); sees("Saved SSH connections"); not_sees("[ WiFi connected ]");
    assert(connects == before_connects);
    reboot(); result(0, "Open", -70, 0); complete_scan(); wifi_success();
    press(KEY_DOWN); advance(3100);
    not_sees("> [ Add new WiFi connection ]"); /* The dismissing key did not also move the selection. */
    sees("Saved WiFi networks");
    not_sees("[ WiFi connected ]");
    reboot(); result(0, "Open", -70, 0); complete_scan(); wifi_success();
    press(KEY_F3); advance(3100); sees("Trusted host keys");
    /* WiFi joining and SSH connecting popups animate so progress is visible. */
    reboot(); result(0, "Open", -70, 0); complete_scan();
    assert(joining);
    char spin = spin_after("Joining ");
    sees("[ Connecting WiFi ]"); sees("Esc: cancel");
    /* The dialog is centred in the page frame (x 4..316, y 0..252), and so is each line inside it. */
    assert(popup_x - 4 == 316 - (popup_x + 276));
    assert(popup_y == 252 - (popup_bottom + 12));
    assert(centre_errors == 0);
    advance(250);
    assert(spin_after("Joining ") != spin);
    wifi_success(); press(KEY_ESC);
    not_sees("Esc: cancel"); not_sees("[ WiFi connected ]");
    assert(ssh_upsert(&disk, &p) == 0);
    reboot(); complete_scan(); online = true; tick();
    press(KEY_F2); press(KEY_ENTER);
    assert(active);
    sees("> [ Connect SSH ]"); /* A preset focuses Connect, not Password. */
    sees("[ Connecting SSH ]"); sees("saved@saved.example:22"); sees("Esc: cancel");
    spin = spin_after("Resolving host / connecting TCP... ");
    advance(250);
    assert(spin_after("Resolving host / connecting TCP... ") != spin);
    press('x'); spin_after("Resolving host / connecting TCP... ");
    ssh_fails = true; tick();
    sees(":: SSH connection"); sees("> [ Connect SSH ]"); not_sees("[ Connecting SSH ]");
    assert(connected_profile.host[0]);
    press(KEY_F2); press('e'); not_sees("> [ Connect SSH ]"); /* Editing still focuses Password. */

    /* F4 settings apply immediately, persist after a debounce and survive reboot. */
    assert(lcd_level == 72 && kbd_level == -1); /* Defaults: 30% screen, keyboard unchanged. */
    press(KEY_F4); sees(":: Settings");
    sees("<[###-------]  30%>"); sees("System default");
    sees("Battery   87% (charging)"); sees("Address   192.168.1.50");
    sees("Version   9.8.7-test");
    unsigned prefs_before = prefs_saves;
    press(KEY_RIGHT); press(KEY_RIGHT);
    assert(lcd_level == 120 && prefs_saves == prefs_before);
    sees("<[#####-----]  50%>");
    advance(800);
    assert(prefs_saves == prefs_before + 1 && disk_prefs.screen_brightness == 50);
    for (unsigned i = 0; i < 12; i++) press(KEY_LEFT);
    assert(lcd_level == 24); sees(" 10%>"); /* Never fully dark. */
    for (unsigned i = 0; i < 12; i++) press(KEY_RIGHT);
    assert(lcd_level == 240); sees("100%>");
    press(KEY_ENTER); assert(lcd_level == 24); /* Enter wraps. */
    press(KEY_DOWN); press(KEY_RIGHT);
    assert(kbd_level == 24);
    press(KEY_LEFT); press(KEY_LEFT); assert(kbd_level == 0);
    press(KEY_DOWN); press(KEY_ENTER); sees("< Manual >");
    press(KEY_DOWN); press(KEY_LEFT); sees("< Manual (F2) >");
    advance(800);
    assert(!disk_prefs.auto_join && !disk_prefs.auto_ssh && disk_prefs.keyboard_brightness == 0);
    lcd_level = kbd_level = -1;
    unsigned scans_before = scans;
    before = joins;
    reboot();
    assert(lcd_level == 24 && kbd_level == 0 && scans == scans_before && joins == before);
    sees("Startup auto-join is off");
    press(KEY_ENTER); wifi_success();
    sees("WiFi"); not_sees("SSH in"); advance(4000); sees("Saved WiFi networks");
    /* Older keyboard firmware cannot set the keyboard light. */
    has_kbd_light = false;
    press(KEY_F4); sees("n/a (keyboard firmware)");
    press(KEY_DOWN); press(KEY_RIGHT); sees("needs newer keyboard firmware.");
    has_kbd_light = true;
    prefs_save_ok = false;
    press(KEY_UP); press(KEY_RIGHT); advance(800);
    sees("Simulated SD write failure");
    prefs_save_ok = true;
    press(KEY_ESC); sees("Saved WiFi networks");
    established = active = true; tick(); sees("Ctrl+] disconnect");
    press(KEY_F4); assert(sent_size == 3 && !memcmp(sent, "\033OS", 3)); /* Terminal still sends F4. */
    /* Editing a preset and saving overwrites it, even when host/user/port change. */
    ssh_stop(); press(29);
    disk.ssh_count = 0;
    ssh_profile_t a = {.host = "alpha.example", .port = "22", .username = "ann"};
    ssh_profile_t b = {.host = "beta.example", .port = "22", .username = "bob"};
    assert(ssh_upsert(&disk, &a) == 0 && ssh_upsert(&disk, &b) == 1);
    reboot(); complete_scan(); online = true; tick();
    press(KEY_F2); press(KEY_DOWN); press('e');
    sees("Editing saved connection; F6 updates it.");
    press(KEY_UP); press(KEY_BACKSPACE); press(KEY_BACKSPACE); press(KEY_BACKSPACE); type("robert");
    press(KEY_UP); press(KEY_BACKSPACE); press(KEY_BACKSPACE); type("2222");
    before = saves;
    press(KEY_F6);
    assert(saves == before + 1 && disk.ssh_count == 2);
    assert(!strcmp(disk.ssh[1].username, "robert") && !strcmp(disk.ssh[1].port, "2222"));
    assert(!strcmp(disk.ssh[0].host, "alpha.example"));
    sees("SSH profile updated");
    type("0"); press(KEY_F6); /* Repeated saves keep updating the same entry. */
    assert(disk.ssh_count == 2 && !strcmp(disk.ssh[1].port, "22220"));
    /* Edit after a failed preset attempt also updates the preset. */
    press(KEY_F2); press(KEY_ENTER); ssh_fails = true; tick();
    sees("> [ Connect SSH ]"); sees("Editing saved connection");
    press(KEY_UP); press(KEY_UP); press(KEY_UP); press(KEY_UP); type("2");
    press(KEY_F6);
    assert(disk.ssh_count == 2 && !strcmp(disk.ssh[0].username, "ann2"));
    /* Changing an edit to match another preset merges instead of duplicating. */
    press(KEY_F2); press('e');
    press(KEY_UP); press(KEY_BACKSPACE); press(KEY_BACKSPACE); press(KEY_BACKSPACE); press(KEY_BACKSPACE);
    type("robert");
    press(KEY_UP); type("220"); press(KEY_UP); /* Port 22220 now equals robert's preset. */
    for (unsigned i = 0; i < 13; i++) press(KEY_BACKSPACE);
    type("beta.example");
    press(KEY_F6);
    assert(disk.ssh_count == 1 && !strcmp(disk.ssh[0].host, "beta.example"));
    press(KEY_F6); assert(disk.ssh_count == 1);
    /* Add New still creates a separate profile. */
    press(KEY_F2); press(KEY_DOWN); press(KEY_ENTER);
    sees("New connection; F6 saves it as a profile.");
    type("gamma.example"); press(KEY_DOWN); press(KEY_DOWN); type("gil");
    press(KEY_F6); assert(disk.ssh_count == 2 && !strcmp(disk.ssh[1].host, "gamma.example"));
    sees("Editing saved connection");
    type("x"); press(KEY_F6); assert(disk.ssh_count == 2 && !strcmp(disk.ssh[1].username, "gilx"));
    /* After deleting an earlier preset, editing the shifted preset still updates it. */
    press(KEY_F2); confirmed_delete();
    assert(disk.ssh_count == 1 && !strcmp(disk.ssh[0].username, "gilx"));
    press('e'); press(KEY_UP); type("y"); press(KEY_F6);
    assert(disk.ssh_count == 1 && !strcmp(disk.ssh[0].username, "gilxy"));
    /* Deleting the edited preset makes the form save as a new one. */
    press(KEY_F2); confirmed_delete(); assert(disk.ssh_count == 0);
    press(KEY_ESC); press(KEY_F2); press(KEY_ENTER); sees("New connection");

    /* Battery level is shown in every top bar, refreshed when it changes. */
    press(KEY_F1); sees("[Bat 87%+]");
    press(KEY_F3); sees("[Bat 87%+]");
    press(KEY_F10); sees("[Bat 87%+]"); press(KEY_ESC);
    battery_charging = false; battery_level = 25; tick(); sees("[Bat 25%]");
    battery_level = 9; tick(); sees("[Bat 9%]");
    battery_level = -1; tick(); sees("[Bat --]");
    battery_level = 100; tick(); sees("[Bat 100%]");
    /* The terminal's own top bar also shows the battery. */
    established = active = true; strcpy(connected_profile.host, "x"); tick();
    sees("Ctrl+] disconnect"); sees("[Bat 100%]");
    battery_level = 50; tick(); sees("[Bat 50%]");
    /* Leaving an established session: saved presets return to the saved list with that
       preset selected; typed connections return to the form. */
    ssh_stop(); press(29); tick();
    disk.ssh_count = 0;
    assert(ssh_upsert(&disk, &a) == 0 && ssh_upsert(&disk, &b) == 1);
    reboot(); complete_scan(); online = true; tick();
    press(KEY_F2); press(KEY_DOWN); press(KEY_ENTER);
    established = true; tick(); sees("Ctrl+] disconnect");
    ssh_fails = true; tick(); /* Remote side closes (e.g. `exit`). */
    sees("Saved SSH connections"); sees("Resolving host"); not_sees("Ctrl+] disconnect");
    press(KEY_ENTER); assert(!strcmp(connected_profile.host, "beta.example")); /* Same preset still selected. */
    established = true; tick();
    press(29); sees("Saved SSH connections"); sees("SSH disconnected.");
    press(KEY_ENTER); assert(!strcmp(connected_profile.host, "beta.example"));
    established = true; tick(); ssh_stop(); press(29);
    /* Editing a preset then connecting from the form still counts as a saved connection. */
    press(KEY_F2); press('e'); press(KEY_F9);
    established = true; tick(); ssh_fails = true; tick();
    sees("Saved SSH connections");
    /* A failed (never established) preset attempt still stays on the form for retry. */
    press(KEY_ENTER); ssh_fails = true; tick();
    sees(":: SSH connection"); sees("> [ Connect SSH ]");
    /* A new, unsaved connection returns to the SSH form either way. */
    press(KEY_ESC); press(KEY_DOWN); press(KEY_DOWN); press(KEY_ENTER);
    type("new.example"); press(KEY_DOWN); press(KEY_DOWN); type("nia"); press(KEY_F9);
    established = true; tick(); ssh_fails = true; tick();
    sees(":: SSH connection"); sees("> [ Connect SSH ]"); sees("[new.example");
    press(KEY_F9); established = true; tick(); press(29);
    sees(":: SSH connection"); sees("SSH disconnected.");
    /* Once saved with F6, the same form's later sessions return to the list. */
    press(KEY_F6); assert(disk.ssh_count == 3);
    press(KEY_F9); established = true; tick(); press(29);
    sees("Saved SSH connections");

    /* F4 software update: check, prompt, progress, verification and the abortable reboot. */
    online = true; active = established = false; tick();
    press(KEY_F4); for (int i = 0; i < 4; i++) press(KEY_DOWN);
    sees("Software"); sees("[ Check for updates ]");
    online = false; tick(); sees("[ Check for updates ] (WiFi)");
    press(KEY_ENTER); sees("Connect to WiFi (F1) before checking for updates."); assert(upd_checks == 0);
    online = true; press(KEY_ENTER); assert(upd_checks == 1);
    sees("[ Checking for updates ]"); sees("Installed: v9.8.7-test"); spin_after("Contacting GitHub... ");
    assert(popup_y >= 0 && centre_errors == 0);
    press('x'); press(KEY_F1); sees("Checking for updates"); sees(":: Settings"); /* Modal. */
    upd_state = UPDATE_LOCATING; advance(300); spin_after("Finding app on SD... ");
    press(KEY_ESC); not_sees("Checking for updates"); sees("Update cancelled."); assert(upd_state == UPDATE_IDLE);
    press(KEY_ENTER); upd_state = UPDATE_CURRENT; advance(300);
    sees("[ No update available ]"); sees("v9.8.7-test is installed"); sees("Latest release: v9.9.0");
    press('q'); not_sees("No update available"); sees(":: Settings");
    press(KEY_ENTER); upd_state = UPDATE_FAILED; strcpy(upd_error, "HTTPS certificate not trusted (-188)"); advance(300);
    sees("[ Update failed ]"); sees("HTTPS certificate not trusted (-188)"); sees("installed app was not changed");
    press(KEY_ENTER); not_sees("Update failed"); assert(upd_checks == 3);
    press(KEY_ENTER); upd_state = UPDATE_AVAILABLE; advance(300);
    sees("[ Update available ]"); sees("v9.8.7-test -> v9.9.0"); sees("Replace launched file:");
    sees("...lities/PicoCalc SSH client (latest).uf2"); sees("Enter: download & update   Esc: later");
    assert(centre_errors == 0);
    press(KEY_ESC); sees("Update postponed."); assert(upd_installs == 0);
    upd_found = false;
    press(KEY_ENTER); upd_state = UPDATE_AVAILABLE; advance(300); sees("App file not found; save as:");
    online = false; press(KEY_ENTER); sees("WiFi disconnected; update cancelled."); assert(upd_installs == 0);
    online = true; upd_found = true;
    press(KEY_ENTER); upd_state = UPDATE_AVAILABLE; advance(300); press(KEY_ENTER); assert(upd_installs == 1);
    upd_total = 0; upd_done = 0; advance(300); spin_after("Starting download... ");
    upd_total = 1400 * 1024; upd_done = 700 * 1024; advance(300);
    sees("[ Downloading update ]"); sees("picocalc_ssh-v9.9.0.uf2"); spin_after("[#####-----]  50% ");
    sees("700 / 1400 KB");
    upd_state = UPDATE_VERIFYING; upd_done = 1400 * 1024; advance(300); sees("[ Verifying update ]");
    unsigned cancels = upd_cancels;
    press(KEY_ESC); press('q'); sees("[ Verifying update ]"); assert(upd_cancels == cancels);
    sees("Please wait; do not power off");
    upd_state = UPDATE_INSTALLING; advance(300); sees("[ Installing update ]"); spin_after("Checking app file 100% ");
    upd_state = UPDATE_DONE; advance(300);
    sees("[ Update installed ]"); sees("v9.9.0 saved and verified"); sees("Rebooting to loader in 5...");
    sees("Then choose PicoCalc SSH to install"); sees("Enter: reboot now   Esc: abort");
    assert(centre_errors == 0);
    advance(2100); sees("Rebooting to loader in 3...");
    press('q'); sees("Rebooting to loader"); /* Only Esc aborts. */
    press(KEY_ESC); assert(reboots == 0); not_sees("Rebooting");
    sees("Update saved. To install it, hold Up at power-on,");
    advance(6000); assert(reboots == 0);
    press(KEY_ENTER); upd_state = UPDATE_AVAILABLE; advance(300); press(KEY_ENTER);
    upd_state = UPDATE_DONE; advance(300); press(KEY_ENTER); assert(reboots == 1);
    press(KEY_ENTER); upd_state = UPDATE_AVAILABLE; advance(300); press(KEY_ENTER);
    upd_state = UPDATE_DONE; advance(300); advance(4500); assert(reboots == 1); advance(600); assert(reboots == 2);

    puts("ASCII screens, navigation, gating, scans, persistence, startup reconnect, terminal and host trust, software update passed.");
    return 0;
}

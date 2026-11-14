#include "app.h"
#include "model.h"
#include "network.h"
#include "ssh_client.h"
#include "storage.h"
#include "system.h"
#include "terminal.h"
#include "ui/gfx.h"
#include "updater.h"
#include "pico/stdlib.h"
#include <stdio.h>
#include <string.h>

#ifndef APP_VERSION
#define APP_VERSION "dev"
#endif

static settings_t settings, candidate;
static wifi_profile_t wifi, joining_wifi;
static ssh_profile_t ssh = {.port = "22"};
static char password[128], notice[128], active_ssid[33];
static terminal_t terminal;
static enum { WIFI, SSH, NETWORKS, SCAN, PROFILES, FILES, HOSTS, HELP, SETTINGS, TERMINAL } screen, help_return;
enum {
    W_SSID, W_PASSWORD, W_JOIN, S_HOST, S_PORT, S_USER, S_PASSWORD,
    S_KEY, S_CLEAR, S_CONNECT, S_SAVE, S_SAVED, W_SCAN, W_SAVED, W_DISCONNECT
};
static int field, selected;
static bool dirty = true, save_join, was_connected, was_wifi;
static bool startup_scan, ssh_countdown;
static uint32_t ssh_deadline;
static uint32_t last_draw;
static prefs_t prefs;
static bool prefs_dirty;
static uint32_t prefs_save_at;
enum { P_SCREEN, P_KEYBOARD, P_AUTOJOIN, P_AUTOSSH, P_UPDATE, P_COUNT };
/* Software update dialog: progress while checking/downloading, then the reboot countdown. */
#define REBOOT_DELAY_MS 5000
static bool update_ui, reboot_countdown;
static uint32_t reboot_deadline;
static int setting;
/* Saved SSH profile loaded into the form (-1 = new); F6 overwrites it in place. */
static int editing_ssh = -1;
/* Saved profile the current session was started from (-1 = typed/new connection). */
static int session_profile = -1;
static int shown_battery = -2;
static bool shown_charging;
static char browser_path[KEY_PATH_LEN] = "/";
static file_entry_t files[12];
static unsigned file_count, file_page;
static bool more_files;
static const int field_y[] = {76, 100, 184, 60, 80, 100, 120, 144, 164, 184, 204, 224, 136, 160, 208};
static const int wifi_fields[] = {W_SSID, W_PASSWORD, W_SCAN, W_SAVED, W_JOIN, W_DISCONNECT};
static const int ssh_fields[] = {S_HOST, S_PORT, S_USER, S_PASSWORD, S_KEY, S_CLEAR, S_CONNECT, S_SAVE, S_SAVED};

static void text(int x, int y, const char *s, uint16_t color) {
    gfx_text_fit(x, y, s, LCD_W - x - 9, &font_6x12, color, TRANSPARENT);
}
static void status(const char *s) {
    snprintf(notice, sizeof(notice), "%s", s);
    dirty = true;
}
static bool ssh_pending(void) { return ssh_active() && !ssh_connected(); }
static const char *progress(const char *s, bool busy) {
    static char line[128];
    if (!busy) return s;
    snprintf(line, sizeof(line), "%s %c", s, "/-\\|"[(to_ms_since_boot(get_absolute_time()) / 250) % 4]);
    return line;
}
static void battery_text(char *out, size_t size, uint16_t *color) {
    bool charging = false;
    int level = kbd_battery_percent(&charging);
    shown_battery = level;
    shown_charging = charging;
    if (level < 0) snprintf(out, size, "[Bat --%s]", charging ? "+" : "");
    else snprintf(out, size, "[Bat %d%%%s]", level, charging ? "+" : "");
    if (color) *color = level >= 0 && level <= 15 && !charging ? C_RED : level >= 0 && level <= 30 ? C_YELLOW : C_DIM;
}
static void header(const char *title) {
    gfx_clear(C_BG);
    char edge[53];
    memset(edge, '-', 52);
    edge[0] = edge[51] = '+';
    edge[52] = 0;
    gfx_text(4, 0, edge, &font_6x12, C_BLUE, TRANSPARENT);
    gfx_text(4, 24, edge, &font_6x12, C_BLUE, TRANSPARENT);
    gfx_text(4, 240, edge, &font_6x12, C_BLUE, TRANSPARENT);
    for (int y = 12; y < 240; y += 12) {
        if (y == 24) continue;
        gfx_text(4, y, "|", &font_6x12, C_BLUE, TRANSPARENT);
        gfx_text(310, y, "|", &font_6x12, C_BLUE, TRANSPARENT);
    }
    char battery[24];
    uint16_t color;
    battery_text(battery, sizeof(battery), &color);
    int bx = 304 - (int)strlen(battery) * 6;
    gfx_text_fit(16, 12, title, bx - 22, &font_6x12, C_ACCENT, TRANSPARENT);
    gfx_text(bx, 12, battery, &font_6x12, color, TRANSPARENT);
}
static void footer(const char *a, const char *b) {
    gfx_fill(0, 288, LCD_W, 32, C_SURFACE);
    gfx_text_n(3, 289, a, 52, &font_6x12, C_DIM, TRANSPARENT);
    gfx_text_n(3, 305, b, 52, &font_6x12, C_DIM, TRANSPARENT);
}
static void navigation(const char *actions) {
    footer(actions, "F1 WiFi  F2 SSH  F3 Host Keys  F4 Settings  F10 Help");
}
static void notice_draw(void) {
    const char *shown = notice;
    gfx_text_n(4, 260, shown, 52, &font_6x12, C_YELLOW, TRANSPARENT);
    if (strlen(shown) > 52)
        gfx_text_n(3, 273, shown + 52, 52, &font_6x12, C_YELLOW, TRANSPARENT);
}

static bool confirm(const char *title, const char *detail, const char *hash) {
    header(title);
    for (unsigned i = 0; i < 3 && i * 49 < strlen(detail); i++)
        gfx_text_n(16, 40 + (int)i * 13, detail + i * 49, 49, &font_6x12, C_TEXT, TRANSPARENT);
    if (hash) {
        text(16, 82, "SHA256 fingerprint (hex):", C_DIM);
        for (int i = 0; i < 4; i++) {
            char line[17];
            memcpy(line, hash + i * 16, 16);
            line[16] = 0;
            text(20, 100 + i * 18, line, C_YELLOW);
        }
        text(16, 181, "Verify using a trusted device before accepting.", C_TEXT);
        text(16, 200, "Accepting saves this host key to the SD card.", C_DIM);
    }
    footer("Enter = confirm / accept", "Esc = cancel / reject");
    lcd_flush();
    absolute_time_t timeout = make_timeout_time_ms(60000);
    bool accepted = false;
    while (!time_reached(timeout)) {
        network_poll();
        kbd_poll();
        key_event_t ev;
        if (kbd_get(&ev)) {
            if (ev.key == KEY_ENTER) { accepted = true; break; }
            if (ev.key == KEY_ESC) break;
        }
        sleep_ms(1);
    }
    dirty = true;
    return accepted;
}

static bool commit(void) {
    bool ok = storage_save(&candidate);
    if (ok) settings = candidate;
    else status(storage_error());
    secure_clear(&candidate, sizeof(candidate));
    return ok;
}

static bool check_host(const char *host, uint16_t port, const uint8_t fingerprint[32]) {
    int i = host_find(&settings, host, port);
    if (i >= 0) {
        if (!memcmp(settings.hosts[i].fingerprint, fingerprint, 32)) return true;
        status("HOST KEY CHANGED! Connection blocked. Verify on a trusted device; F3 manages keys.");
        return false;
    }
    if (settings.host_count == MAX_HOSTS) {
        status("Known-host list full; remove an old entry using F3.");
        return false;
    }
    char hash[65], endpoint[144];
    static const char hex[] = "0123456789abcdef";
    for (unsigned j = 0; j < 32; j++) {
        hash[j * 2] = hex[fingerprint[j] >> 4];
        hash[j * 2 + 1] = hex[fingerprint[j] & 15];
    }
    hash[64] = 0;
    snprintf(endpoint, sizeof(endpoint), "%s:%u", host, port);
    if (!confirm("Unknown SSH host - trust key?", endpoint, hash)) {
        status("Host key rejected (or approval timed out).");
        return false;
    }
    candidate = settings;
    known_host_t *h = &candidate.hosts[candidate.host_count++];
    snprintf(h->host, sizeof(h->host), "%s", host);
    h->port = port;
    memcpy(h->fingerprint, fingerprint, 32);
    return commit();
}

static void row(int id, const char *label, const char *value, bool secret) {
    bool enabled = screen != SSH || id == S_SAVED || wifi_ready();
    int y = field_y[id];
    bool focused = id == field && enabled;
    if (focused) gfx_fill(10, y - 1, 300, 14, C_SURFACE2);
    text(16, y, label, enabled ? focused ? C_ACCENT : C_TEXT : C_FAINT);
    char hidden[64];
    if (secret) {
        size_t n = strlen(value);
        if (n > sizeof(hidden) - 1) n = sizeof(hidden) - 1;
        memset(hidden, '*', n);
        hidden[n] = 0;
        value = hidden;
    }
    size_t n = strlen(value);
    if (n > 28) value += n - 28;
    char input[32];
    snprintf(input, sizeof(input), "[%-28.28s]", value);
    gfx_text(124, y, input, &font_6x12, enabled ? C_TEXT : C_FAINT, TRANSPARENT);
    if (focused) gfx_hline(130 + (int)(strlen(value) < 28 ? strlen(value) : 27) * 6, y + 11, 6, C_ACCENT);
}

static void button(int id, const char *label) {
    bool enabled = screen != SSH || id == S_SAVED || wifi_ready();
    char line[52];
    snprintf(line, sizeof(line), "%c [ %s ]", field == id && enabled ? '>' : ' ', label);
    if (field == id && enabled) gfx_fill(10, field_y[id] - 1, 300, 14, C_SURFACE2);
    text(16, field_y[id], line, enabled ? field == id ? C_ACCENT : C_TEXT : C_FAINT);
}

static void next_field(int direction) {
    const int *fields = screen == WIFI ? wifi_fields : ssh_fields;
    unsigned count = screen == WIFI ? sizeof(wifi_fields) / sizeof(*wifi_fields) :
                                     sizeof(ssh_fields) / sizeof(*ssh_fields);
    if (screen == SSH && !wifi_ready()) { field = S_SAVED; return; }
    unsigned i;
    for (i = 0; i < count; i++) if (fields[i] == field) break;
    field = fields[i == count ? 0 : (i + count + direction) % count];
}

static unsigned list_count(void) {
    if (screen == NETWORKS) return settings.wifi_count;
    if (screen == PROFILES) return settings.ssh_count;
    return settings.host_count;
}

static void draw_wifi(void) {
    header("PicoCalc SSH :: WiFi");
    text(16, 40, wifi_status(), wifi_ready() ? C_ACCENT : C_YELLOW);
    text(16, 56, active_ssid[0] ? active_ssid : "2.4 GHz / DHCP / open or WPA/WPA2", C_DIM);
    row(W_SSID, "SSID", wifi.ssid, false);
    row(W_PASSWORD, "Password", wifi.password, true);
    button(W_SCAN, "Scan networks");
    button(W_SAVED, "Saved networks");
    button(W_JOIN, "Join WiFi");
    button(W_DISCONNECT, "Disconnect WiFi");
    notice_draw();
    navigation("Tab/Arrows: focus  Enter: select  F5 Join WiFi");
}

static void draw_ssh(void) {
    header("PicoCalc SSH :: SSH connection");
    text(16, 40, !wifi_ready() ? "SSH locked until WiFi is established (F1)." :
                 editing_ssh >= 0 ? "Editing saved connection; F6 updates it." :
                 "New connection; F6 saves it as a profile.", C_DIM);
    row(S_HOST, "Host", ssh.host, false);
    row(S_PORT, "Port", ssh.port, false);
    row(S_USER, "Username", ssh.username, false);
    row(S_PASSWORD, "Password", password, true);
    row(S_KEY, "Private key", ssh.key_path[0] ? ssh.key_path : "Enter to browse (optional)", false);
    button(S_CLEAR, "Clear private key");
    button(S_CONNECT, "Connect SSH");
    button(S_SAVE, "Save SSH profile (no password)");
    button(S_SAVED, "Saved connections");
    notice_draw();
    navigation("Enter: select  F6 Save  F9 Connect  Esc: saved");
}

static void draw_list(void) {
    header(screen == NETWORKS ? "Saved WiFi networks" :
           screen == PROFILES ? "Saved SSH connections" : "Trusted host keys");
    unsigned count = list_count();
    text(16, 40, screen == NETWORKS ? startup_scan ? wifi_scan_status() : wifi_status() :
         screen == PROFILES ? wifi_ready() ? "Enter: connect preset   E: edit   Delete: delete" :
         "SSH locked until WiFi is established (F1)." :
         "F8/Delete: forget host key (requires confirmation)", C_DIM);
    if (!count) text(16, 60, "(No saved entries)", C_DIM);
    unsigned page_size = screen == HOSTS ? 11 : 9;
    unsigned item = selected < (int)count ? (unsigned)selected : count ? count - 1 : 0;
    unsigned start = (item / page_size) * page_size;
    for (unsigned i = start; i < count && i < start + page_size; i++) {
        int y = 60 + (int)(i - start) * 16;
        if ((int)i == selected) gfx_fill(10, y - 1, 300, 14, C_SURFACE2);
        char line[256];
        if (screen == NETWORKS) snprintf(line, sizeof(line), "%s", settings.wifi[i].ssid);
        else if (screen == PROFILES)
            snprintf(line, sizeof(line), "%s@%s:%s", settings.ssh[i].username, settings.ssh[i].host, settings.ssh[i].port);
        else snprintf(line, sizeof(line), "%s:%u", settings.hosts[i].host, settings.hosts[i].port);
        text(16, y, line, (int)i == selected ? C_ACCENT : C_TEXT);
    }
    if (screen != HOSTS) {
        if (selected == (int)count) gfx_fill(10, 207, 300, 14, C_SURFACE2);
        text(16, 208, screen == NETWORKS ?
             (selected == (int)count ? "> [ Add new WiFi connection ]" : "  [ Add new WiFi connection ]") :
             (selected == (int)count ? "> [ Add new SSH connection ]" : "  [ Add new SSH connection ]"), C_ACCENT);
        if (screen == NETWORKS) {
            if (selected == (int)count + 1) gfx_fill(10, 223, 300, 14, C_SURFACE2);
            text(16, 224, selected == (int)count + 1 ? "> [ Disconnect WiFi ]" : "  [ Disconnect WiFi ]", C_DIM);
        }
    }
    notice_draw();
    navigation(screen == NETWORKS ? "Arrows/Tab: select  Enter/F5 Join WiFi  Del: forget" :
               "Arrows/Tab: select  PgUp/PgDn: page  Enter: use");
}

static void open_scan(void) {
    if (wifi_busy()) { status("Wait for WiFi join, or cancel it with Esc."); return; }
    screen = SCAN;
    selected = 0;
    notice[0] = 0;
    if (!wifi_scan_busy()) wifi_scan_start();
    dirty = true;
}

static void draw_scan(void) {
    header("Nearby WiFi networks");
    text(16, 40, wifi_scan_status(), C_DIM);
    unsigned count;
    const wifi_scan_result_t *results = wifi_scan_results(&count);
    unsigned start = selected < 0 ? 0 : ((unsigned)selected / 10) * 10;
    for (unsigned i = start; i < count && i < start + 10; i++) {
        int y = 60 + (int)(i - start) * 16;
        if ((int)i == selected) gfx_fill(10, y - 1, 300, 14, C_SURFACE2);
        gfx_text_n(16, y, results[i].ssid, 31, &font_6x12,
                   (int)i == selected ? C_ACCENT : C_TEXT, TRANSPARENT);
        const char *security = results[i].auth_mode == 0 ? "Open" :
                               results[i].auth_mode & 6 ? "WPA/2" : "WEP!";
        gfx_fill(205, y - 1, 105, 16, (int)i == selected ? C_SURFACE2 : C_BG);
        char detail[24];
        snprintf(detail, sizeof(detail), "%s %d dBm", security, results[i].rssi);
        text(207, y, detail, C_DIM);
    }
    if (selected == -1) gfx_fill(10, 223, 300, 14, C_SURFACE2);
    text(16, 224, selected == -1 ? "> [ Scan again ]" : "  [ Scan again ]",
         selected == -1 ? C_ACCENT : C_TEXT);
    notice_draw();
    navigation("Tab: rescan button  Enter: use  Esc: WiFi");
}

static void scan_key(key_event_t ev) {
    if (ev.key == KEY_TAB) { selected = selected == -1 ? 0 : -1; dirty = true; return; }
    if (ev.key == KEY_ENTER && selected == -1) {
        notice[0] = 0;
        wifi_scan_start();
        dirty = true;
        return;
    }
    unsigned count;
    const wifi_scan_result_t *results = wifi_scan_results(&count);
    if (selected >= (int)count) selected = count ? (int)count - 1 : 0;
    if (!count) return;
    if (selected == -1 && (ev.key == KEY_UP || ev.key == KEY_DOWN)) selected = 0;
    else if (ev.key == KEY_UP && selected > 0) selected--;
    else if (ev.key == KEY_DOWN && selected + 1 < (int)count) selected++;
    else if (ev.key == KEY_PAGE_UP) selected = selected > 10 ? selected - 10 : 0;
    else if (ev.key == KEY_PAGE_DOWN) selected = selected + 10 < (int)count ? selected + 10 : (int)count - 1;
    else if (ev.key == KEY_ENTER) {
        if (wifi_scan_busy()) { status("Wait for the scan to finish before selecting."); return; }
        const wifi_scan_result_t *result = &results[selected];
        if (result->auth_mode && !(result->auth_mode & 6)) {
            status("WEP is not supported. Choose open or WPA/WPA2 WiFi.");
            return;
        }
        secure_clear(&wifi, sizeof(wifi));
        snprintf(wifi.ssid, sizeof(wifi.ssid), "%s", result->ssid);
        if (result->auth_mode) {
            for (unsigned i = 0; i < settings.wifi_count; i++)
                if (!strcmp(settings.wifi[i].ssid, wifi.ssid)) {
                    snprintf(wifi.password, sizeof(wifi.password), "%s", settings.wifi[i].password);
                    break;
                }
        }
        screen = WIFI;
        field = result->auth_mode ? W_PASSWORD : W_JOIN;
        status(result->auth_mode ? "Network selected. Enter password (or use saved); F5 Join WiFi." :
                                  "Open network selected. No password required; F5 Join WiFi.");
    }
    dirty = true;
}

static void load_directory(void) {
    selected = 0;
    if (!storage_list(browser_path, files, 12, &file_count, file_page * 12, &more_files)) {
        status(storage_error());
        file_count = 0;
    }
    dirty = true;
}

static void browse(void) {
    if (!wifi_ready()) { status("Connect WiFi on F1 before editing SSH details."); return; }
    screen = FILES;
    file_page = 0;
    load_directory();
}

static void draw_files(void) {
    header("Select private key from SD card");
    text(16, 40, browser_path, C_DIM);
    if (!file_count) text(16, 60, "(No files in this directory)", C_DIM);
    for (unsigned i = 0; i < file_count; i++) {
        int y = 60 + (int)i * 14;
        if ((int)i == selected) gfx_fill(10, y - 1, 300, 14, C_SURFACE2);
        char line[sizeof(files[i].name) + 8];
        snprintf(line, sizeof(line), "%s%.*s", files[i].directory ? "[DIR] " : "",
                 (int)sizeof(files[i].name) - 1, files[i].name);
        text(16, y, line, (int)i == selected ? C_ACCENT : C_TEXT);
    }
    char page[64];
    snprintf(page, sizeof(page), "Page %u%s", file_page + 1, more_files ? " (more files: PgDn)" : "");
    text(16, 228, page, C_DIM);
    notice_draw();
    navigation("Enter: select  Backspace: up  PgUp/PgDn: page");
}

static void draw_terminal(void) {
    gfx_clear(C_BLACK);
    gfx_fill(0, 0, LCD_W, 16, C_SURFACE);
    char endpoint[256];
    snprintf(endpoint, sizeof(endpoint), "%s@%s", ssh.username, ssh.host);
    char battery[24];
    uint16_t color;
    battery_text(battery, sizeof(battery), &color);
    int bx = LCD_W - 2 - (int)strlen(battery) * 6;
    gfx_text_fit(2, 1, endpoint, bx - 8, &font_6x12, C_ACCENT, TRANSPARENT);
    gfx_text(bx, 1, battery, &font_6x12, color, TRANSPARENT);
    for (int y = 0; y < TERM_ROWS; y++)
        for (int x = 0; x < TERM_COLS; x++) {
            term_cell_t c = terminal.cells[y][x];
            char glyph[3] = {'?', 0, 0};
            if (c.ch < 128) glyph[0] = (char)c.ch;
            else if (c.ch < 256) { glyph[0] = (char)(0xc0 | (c.ch >> 6)); glyph[1] = (char)(0x80 | (c.ch & 63)); }
            else if (c.ch >= 0x2500 && c.ch <= 0x257f)
                glyph[0] = (c.ch == 0x2500 || c.ch == 0x2501) ? '-' :
                           (c.ch == 0x2502 || c.ch == 0x2503) ? '|' : '+';
            gfx_text(x * 6, 16 + y * 12, glyph, &font_6x12, c.fg, c.bg);
        }
    if (terminal.cursor) gfx_hline(terminal.x * 6, 27 + terminal.y * 12, 6, C_WHITE);
    gfx_fill(0, 304, LCD_W, 16, C_SURFACE);
    text(2, 305, notice[0] ? notice : "Ctrl+] disconnect   F1-F10 sent to remote", C_DIM);
    terminal.dirty = false;
}

static void draw_help(void) {
    header("PicoCalc SSH - Help");
    static const char *lines[] = {
        "F1 WiFi: Enter/F5 joins a saved network.",
        "[ Add new WiFi connection ]: scan or type.",
        "Startup scans, joins strongest known WiFi.",
        "WiFi ready: SSH in 3s; Enter now, other: stay.",
        "F2 SSH: fields unlock after WiFi gets an IP.",
        "Host, port, username; password is optional.",
        "Private key: Enter picks RSA/P-256 PEM/DER.",
        "F2 SSH: Enter connects a preset immediately.",
        "[ Connect SSH ] / F9 starts the terminal.",
        "[ Save SSH profile ] / F6 saves NO password.",
        "F3 Host Keys: Delete/F8 forgets after approval.",
        "F4 Settings: brightness, startup, updates.",
        "Tab/Shift+Tab or arrows: move between items.",
        "Enter: activate button / next input field.",
        "E: edit preset; F6 then updates it in place.",
        "In terminal: Ctrl+] disconnects.",
        "SD WiFi passwords and keys are unencrypted."
    };
    for (unsigned i = 0; i < sizeof(lines) / sizeof(lines[0]); i++) text(16, 32 + (int)i * 12, lines[i], C_TEXT);
    navigation("Esc: return to previous screen");
}

#define POPUP_COLS 46
#define POPUP_ROWS 8
#define POPUP_W (POPUP_COLS * 6)
#define POPUP_H (POPUP_ROWS * 12)
/* Centred inside the page's ASCII frame (x 4..316, y 0..252), not the whole LCD with its footer. */
#define FRAME_LEFT 4
#define FRAME_RIGHT 316
#define FRAME_BOTTOM 252
#define POPUP_X ((FRAME_LEFT + FRAME_RIGHT - POPUP_W) / 2)
#define POPUP_Y ((FRAME_BOTTOM - POPUP_H) / 2)

static void popup_line(int row, const char *s, uint16_t color) {
    int n = (int)strlen(s);
    if (n > POPUP_COLS - 4) n = POPUP_COLS - 4;
    gfx_text_n(POPUP_X + (POPUP_W - n * 6) / 2, POPUP_Y + row * 12, s, n, &font_6x12, color, TRANSPARENT);
}

/* Curses-style dialog over the current window for WiFi/SSH progress and the countdown. */
/* Long SD paths keep their tail (the file name) visible. */
static const char *path_tail(const char *path, char *out, size_t size) {
    size_t n = strlen(path), max = POPUP_COLS - 4;
    if (n <= max) return path;
    snprintf(out, size, "...%s", path + n - (max - 3));
    return out;
}

typedef struct {
    const char *title, *target, *state, *extra, *hint;
    uint16_t target_color, state_color;
    char target_line[64], state_line[64], extra_line[64];
} popup_text_t;

static void update_text(popup_text_t *p) {
    size_t done = updater_done_bytes(), total = updater_total_bytes();
    unsigned percent = total ? (unsigned)(done * 100u / total) : 0;
    p->target = p->target_line;
    p->state = p->state_line;
    p->extra = "";
    p->hint = "Esc: cancel";
    if (reboot_countdown) {
        int32_t remaining = (int32_t)(reboot_deadline - to_ms_since_boot(get_absolute_time()));
        p->title = "Update installed";
        snprintf(p->target_line, sizeof(p->target_line), "v%s saved and verified", updater_version());
        snprintf(p->state_line, sizeof(p->state_line), "Rebooting to loader in %u...",
                 remaining > 0 ? ((unsigned)remaining + 999) / 1000 : 0);
        p->state_color = C_ACCENT;
        p->extra = "Then choose PicoCalc SSH to install";
        p->hint = "Enter: reboot now   Esc: abort";
        return;
    }
    update_state_t s = updater_state();
    switch (s) {
    case UPDATE_CHECKING:
    case UPDATE_LOCATING:
        p->title = "Checking for updates";
        snprintf(p->target_line, sizeof(p->target_line), "Installed: v%s", APP_VERSION);
        p->state = progress(s == UPDATE_CHECKING ? "Contacting GitHub..." : "Finding app on SD...", true);
        break;
    case UPDATE_AVAILABLE:
        p->title = "Update available";
        snprintf(p->target_line, sizeof(p->target_line), "v%s -> v%s", APP_VERSION, updater_version());
        p->state = updater_target_found() ? "Replace launched file:" : "App file not found; save as:";
        p->state_color = C_TEXT;
        p->extra = path_tail(updater_target(), p->extra_line, sizeof(p->extra_line));
        p->hint = "Enter: download & update   Esc: later";
        break;
    case UPDATE_DOWNLOADING: {
        char bar[11];
        for (unsigned i = 0; i < 10; i++) bar[i] = i < percent / 10 ? '#' : '-';
        bar[10] = 0;
        p->title = "Downloading update";
        snprintf(p->target_line, sizeof(p->target_line), "picocalc_ssh-v%s.uf2", updater_version());
        if (total) snprintf(p->state_line, sizeof(p->state_line), "[%s] %3u%%", bar, percent);
        else snprintf(p->state_line, sizeof(p->state_line), "Starting download...");
        p->state = progress(p->state_line, true);
        snprintf(p->extra_line, sizeof(p->extra_line), "%u / %u KB", (unsigned)(done / 1024),
                 (unsigned)(total / 1024));
        p->extra = p->extra_line;
        break;
    }
    case UPDATE_VERIFYING:
    case UPDATE_INSTALLING:
        p->title = s == UPDATE_VERIFYING ? "Verifying update" : "Installing update";
        p->target = path_tail(updater_target(), p->target_line, sizeof(p->target_line));
        snprintf(p->state_line, sizeof(p->state_line), "%s %3u%%",
                 s == UPDATE_VERIFYING ? "Checking download" : "Checking app file", percent);
        p->state = progress(p->state_line, true);
        p->hint = "Please wait; do not power off";
        break;
    case UPDATE_CURRENT:
        p->title = "No update available";
        snprintf(p->target_line, sizeof(p->target_line), "v%s is installed", APP_VERSION);
        snprintf(p->state_line, sizeof(p->state_line), "Latest release: v%s", updater_version());
        p->state_color = C_ACCENT;
        p->hint = "Any key: close";
        break;
    default:
        p->title = "Update failed";
        p->target = updater_error();
        p->target_color = C_RED;
        p->state = "The installed app was not changed";
        p->state_color = C_TEXT;
        p->hint = "Any key: close";
        break;
    }
}

static bool draw_popup(void) {
    const char *title, *target, *state, *hint, *extra = "";
    char target_line[sizeof(ssh.username) + sizeof(ssh.host) + sizeof(ssh.port)], state_line[64];
    uint16_t state_color = C_YELLOW, target_color = C_TEXT;
    static popup_text_t update;
    if (screen == TERMINAL) return false;
    if (update_ui) {
        memset(&update, 0, sizeof(update));
        update.target_color = C_TEXT;
        update.state_color = C_YELLOW;
        update_text(&update);
        title = update.title;
        target = update.target;
        state = update.state;
        extra = update.extra;
        hint = update.hint;
        target_color = update.target_color;
        state_color = update.state_color;
    } else if (ssh_pending()) {
        title = "Connecting SSH";
        snprintf(target_line, sizeof(target_line), "%s@%s:%s", ssh.username, ssh.host, ssh.port);
        target = target_line;
        state = progress(ssh_status(), true);
        hint = "Esc: cancel";
    } else if (wifi_busy()) {
        title = "Connecting WiFi";
        target = joining_wifi.ssid[0] ? joining_wifi.ssid : wifi.ssid;
        state = progress(wifi_status(), true);
        hint = "Esc: cancel";
    } else if (ssh_countdown) {
        int32_t remaining = (int32_t)(ssh_deadline - to_ms_since_boot(get_absolute_time()));
        title = "WiFi connected";
        target = active_ssid;
        snprintf(state_line, sizeof(state_line), "Opening SSH in %u...",
                 remaining > 0 ? ((unsigned)remaining + 999) / 1000 : 0);
        state = state_line;
        state_color = C_ACCENT;
        hint = "Enter: SSH now   Other key: stay";
    } else return false;
    char edge[POPUP_COLS + 1];
    gfx_fill(POPUP_X + 4, POPUP_Y + 4, POPUP_W, POPUP_H, C_BLACK); /* drop shadow */
    gfx_fill(POPUP_X, POPUP_Y, POPUP_W, POPUP_H, C_SURFACE);
    memset(edge, '-', POPUP_COLS);
    edge[0] = edge[POPUP_COLS - 1] = '+';
    edge[POPUP_COLS] = 0;
    gfx_text(POPUP_X, POPUP_Y, edge, &font_6x12, C_ACCENT, TRANSPARENT);
    gfx_text(POPUP_X, POPUP_Y + POPUP_H - 12, edge, &font_6x12, C_ACCENT, TRANSPARENT);
    for (int y = POPUP_Y + 12; y < POPUP_Y + POPUP_H - 12; y += 12) {
        gfx_text(POPUP_X, y, "|", &font_6x12, C_ACCENT, TRANSPARENT);
        gfx_text(POPUP_X + POPUP_W - 6, y, "|", &font_6x12, C_ACCENT, TRANSPARENT);
    }
    char caption[48];
    snprintf(caption, sizeof(caption), "[ %s ]", title);
    gfx_text(POPUP_X + (POPUP_W - (int)strlen(caption) * 6) / 2, POPUP_Y, caption, &font_6x12, C_ACCENT, C_SURFACE);
    popup_line(2, target, target_color);
    popup_line(3, state, state_color);
    popup_line(4, extra, C_TEXT);
    popup_line(5, hint, C_DIM);
    return true;
}

static void apply_backlights(void) {
    kbd_set_lcd_backlight(backlight_level(prefs.screen_brightness));
    if (prefs.keyboard_brightness != KBD_LIGHT_UNCHANGED && kbd_has_kbd_backlight())
        kbd_set_kbd_backlight(backlight_level(prefs.keyboard_brightness));
}

static void meter(char *out, size_t size, unsigned percent) {
    char bar[11];
    for (unsigned i = 0; i < 10; i++) bar[i] = i < percent / 10 ? '#' : '-';
    bar[10] = 0;
    snprintf(out, size, "<[%s] %3u%%>", bar, percent);
}

static void draw_settings(void) {
    header("PicoCalc SSH :: Settings");
    text(16, 40, "Left/Right change; saved to SD automatically.", C_DIM);
    static const char *labels[P_COUNT] = {"Screen", "Keyboard light", "Startup WiFi", "Open SSH page", "Software"};
    for (int i = 0; i < P_COUNT; i++) {
        int y = 60 + i * 18;
        char value[40];
        if (i == P_SCREEN) meter(value, sizeof(value), prefs.screen_brightness);
        else if (i == P_KEYBOARD) {
            if (!kbd_has_kbd_backlight()) snprintf(value, sizeof(value), "n/a (keyboard firmware)");
            else if (prefs.keyboard_brightness == KBD_LIGHT_UNCHANGED) snprintf(value, sizeof(value), "<  System default  >");
            else meter(value, sizeof(value), prefs.keyboard_brightness);
        } else if (i == P_AUTOJOIN)
            snprintf(value, sizeof(value), "< %s >", prefs.auto_join ? "Auto-join known" : "Manual");
        else if (i == P_AUTOSSH)
            snprintf(value, sizeof(value), "< %s >", prefs.auto_ssh ? "After 3s countdown" : "Manual (F2)");
        else snprintf(value, sizeof(value), "[ Check for updates ]%s", wifi_ready() ? "" : " (WiFi)");
        if (i == setting) gfx_fill(10, y - 1, 300, 14, C_SURFACE2);
        text(16, y, labels[i], i == setting ? C_ACCENT : C_TEXT);
        text(118, y, value, i == setting ? C_ACCENT : C_TEXT);
    }
    text(16, 152, "+-- Status ------------------------------------+", C_BLUE);
    bool charging = false;
    int battery = kbd_battery_percent(&charging);
    char line[160];
    if (battery < 0) snprintf(line, sizeof(line), "Battery   unknown%s", charging ? " (charging)" : "");
    else snprintf(line, sizeof(line), "Battery   %d%%%s", battery, charging ? " (charging)" : "");
    text(16, 166, line, C_TEXT);
    snprintf(line, sizeof(line), "WiFi      %s", wifi_ready() && active_ssid[0] ? active_ssid :
             progress(wifi_status(), wifi_busy()));
    text(16, 180, line, C_TEXT);
    snprintf(line, sizeof(line), "Address   %s", wifi_ready() ? wifi_address() : "-");
    text(16, 194, line, C_TEXT);
    text(16, 208, "Version   " APP_VERSION, C_TEXT);
    text(16, 222, "Saved: WiFi/SSH/keys in /sshclient on SD", C_DIM);
    notice_draw();
    navigation("Up/Down: select  Left/Right/Enter: change");
}

static void settings_key(key_event_t ev) {
    if (ev.key == KEY_UP || (ev.key == KEY_TAB && (ev.mods & MOD_SHIFT)))
        setting = (setting + P_COUNT - 1) % P_COUNT;
    else if (ev.key == KEY_DOWN || ev.key == KEY_TAB) setting = (setting + 1) % P_COUNT;
    else if (setting == P_UPDATE) {
        if (ev.key != KEY_ENTER) return;
        if (!wifi_ready()) status("Connect to WiFi (F1) before checking for updates.");
        else if (ssh_active()) status("Disconnect SSH before checking for updates.");
        else if (updater_check(APP_VERSION)) {
            update_ui = true;
            reboot_countdown = false;
            notice[0] = 0;
        }
    } else if (ev.key == KEY_LEFT || ev.key == KEY_RIGHT || ev.key == KEY_ENTER) {
        int step = ev.key == KEY_LEFT ? -10 : 10;
        bool wrap = ev.key == KEY_ENTER;
        if (setting == P_SCREEN) {
            int v = prefs.screen_brightness + step;
            prefs.screen_brightness = (uint8_t)(v > 100 ? wrap ? 10 : 100 : v < 10 ? 10 : v);
        } else if (setting == P_KEYBOARD) {
            if (!kbd_has_kbd_backlight()) {
                status("Keyboard light needs newer keyboard firmware.");
                return;
            }
            int v = prefs.keyboard_brightness == KBD_LIGHT_UNCHANGED ? step < 0 ? 10 : 0 :
                    prefs.keyboard_brightness;
            v += step;
            prefs.keyboard_brightness = (uint8_t)(v > 100 ? wrap ? 0 : 100 : v < 0 ? 0 : v);
        } else if (setting == P_AUTOJOIN) prefs.auto_join = !prefs.auto_join;
        else prefs.auto_ssh = !prefs.auto_ssh;
        apply_backlights();
        prefs_dirty = true;
        /* Debounce SD writes while a brightness key repeats. */
        prefs_save_at = to_ms_since_boot(get_absolute_time()) + 750;
    }
    dirty = true;
}

static void join_wifi(void) {
    startup_scan = ssh_countdown = false;
    if (ssh_active()) { status("Cancel SSH before switching networks."); return; }
    if (!valid_wifi(&wifi)) { status("SSID required; WiFi password empty, 8-63 chars, or 64 hex digits."); return; }
    joining_wifi = wifi;
    save_join = wifi_connect(&joining_wifi);
    if (!save_join) {
        secure_clear(&joining_wifi, sizeof(joining_wifi));
        status(wifi_status());
    } else { notice[0] = 0; dirty = true; }
}

static void save_ssh(void) {
    if (!wifi_ready()) { status("SSH details are disabled until WiFi is ready."); return; }
    if (!valid_ssh(&ssh)) { status("Host, port 1..65535 and username are required."); return; }
    candidate = settings;
    bool update = editing_ssh >= 0;
    int index = ssh_replace(&candidate, editing_ssh, &ssh);
    if (index < 0) {
        secure_clear(&candidate, sizeof(candidate));
        status("SSH list full; use Saved connections to delete an entry.");
    } else if (commit()) {
        editing_ssh = index;
        status(update ? "SSH profile updated. Passwords are never saved." :
                        "SSH profile saved. Passwords are never saved.");
    }
}

static void connect_ssh(void) {
    if (!wifi_ready()) { status("Connect WiFi before entering SSH details."); return; }
    terminal_init(&terminal);
    bool ok = ssh_start(&ssh, password, check_host);
    secure_clear(password, sizeof(password));
    if (!ok) { status(ssh_status()); return; }
    notice[0] = 0;
    dirty = true;
    was_connected = false;
    session_profile = editing_ssh;
}

static bool edit_field(key_event_t ev) {
    char *value = NULL;
    size_t capacity = 0;
    switch (field) {
    case 0: value = wifi.ssid; capacity = sizeof(wifi.ssid); break;
    case 1: value = wifi.password; capacity = sizeof(wifi.password); break;
    case 3: value = ssh.host; capacity = sizeof(ssh.host); break;
    case 4: value = ssh.port; capacity = sizeof(ssh.port); break;
    case 5: value = ssh.username; capacity = sizeof(ssh.username); break;
    case 6: value = password; capacity = sizeof(password); break;
    default: return false;
    }
    size_t n = strlen(value);
    if ((ev.mods & MOD_CTRL) && (ev.key == 'u' || ev.key == 'U' || ev.key == 21)) {
        secure_clear(value, capacity);
    } else if (ev.key == KEY_BACKSPACE || ev.key == KEY_DEL) {
        if (n) value[n - 1] = 0;
    } else if (ev.key >= 32 && ev.key < 127 && !(ev.mods & (MOD_CTRL | MOD_ALT))) {
        if (field == 4 && (ev.key < '0' || ev.key > '9')) { status("Port must contain digits only."); return true; }
        if (n + 1 >= capacity) { status("Field is full."); return true; }
        value[n] = (char)ev.key;
        value[n + 1] = 0;
    } else return false;
    dirty = true;
    return true;
}

static void form_key(key_event_t ev) {
    if (screen == SSH && !wifi_ready()) field = S_SAVED;
    if (ev.key == KEY_UP) next_field(-1);
    else if (ev.key == KEY_DOWN || ev.key == KEY_TAB)
        next_field(ev.mods & MOD_SHIFT ? -1 : 1);
    else if (ev.key == KEY_ENTER) {
        if (field == W_SCAN) open_scan();
        else if (field == W_SAVED || field == S_SAVED) {
            screen = field == W_SAVED ? NETWORKS : PROFILES;
            selected = 0; notice[0] = 0;
        } else if (screen == SSH && !wifi_ready()) status("Connect WiFi first.");
        else if (field == W_JOIN) join_wifi();
        else if (field == S_KEY) browse();
        else if (field == S_CLEAR) { ssh.key_path[0] = 0; status("Private key cleared."); }
        else if (field == S_CONNECT) connect_ssh();
        else if (field == S_SAVE) save_ssh();
        else if (field == W_DISCONNECT) {
            wifi_disconnect(); save_join = false;
            secure_clear(&joining_wifi, sizeof(joining_wifi));
            status("WiFi disconnected.");
        } else next_field(1);
    } else if (screen == WIFI && ev.key == KEY_F5) join_wifi();
    else if (screen == SSH && ev.key == KEY_F6) save_ssh();
    else if (screen == SSH && ev.key == KEY_F9) connect_ssh();
    else if (screen == SSH && !wifi_ready()) status("Connect WiFi first; SSH details are locked.");
    else edit_field(ev);
    dirty = true;
}

static void list_key(key_event_t ev) {
    unsigned count = list_count();
    unsigned total = count + (screen == NETWORKS ? 2 : screen == PROFILES ? 1 : 0);
    unsigned page = screen == HOSTS ? 11 : 9;
    if (!total) return;
    if (ev.key == KEY_UP && selected > 0) selected--;
    else if (ev.key == KEY_DOWN && selected + 1 < (int)total) selected++;
    else if (ev.key == KEY_TAB)
        selected = (selected + total + (ev.mods & MOD_SHIFT ? -1 : 1)) % total;
    else if (ev.key == KEY_PAGE_UP) selected = selected > (int)page ? selected - (int)page : 0;
    else if (ev.key == KEY_PAGE_DOWN) selected = selected + (int)page < (int)total ? selected + (int)page : (int)total - 1;
    else if (ev.key == KEY_ENTER && selected == (int)count && screen != HOSTS) {
        if (screen == PROFILES && !wifi_ready()) { status("Connect WiFi first."); return; }
        bool adding_wifi = screen == NETWORKS;
        screen = adding_wifi ? WIFI : SSH;
        field = adding_wifi ? W_SSID : S_HOST;
        if (adding_wifi) secure_clear(&wifi, sizeof(wifi));
        else {
            memset(&ssh, 0, sizeof(ssh)); strcpy(ssh.port, "22");
            editing_ssh = -1;
            secure_clear(password, sizeof(password));
        }
        notice[0] = 0;
    } else if (ev.key == KEY_ENTER && screen == NETWORKS && selected == (int)count + 1) {
        wifi_disconnect(); save_join = false;
        secure_clear(&joining_wifi, sizeof(joining_wifi));
        status("WiFi disconnected.");
    } else if (selected < (int)count &&
               (ev.key == KEY_ENTER || (screen == NETWORKS && ev.key == KEY_F5) ||
                ((screen == PROFILES || screen == NETWORKS) && (ev.key == 'e' || ev.key == 'E')))) {
        bool edit = ev.key == 'e' || ev.key == 'E';
        if (screen == NETWORKS) {
            wifi = settings.wifi[selected];
            if (edit) { screen = WIFI; field = W_PASSWORD; }
            else join_wifi();
        } else if (screen == PROFILES) {
            if (!wifi_ready()) { status("Connect WiFi first. Saved profiles can still be deleted."); return; }
            ssh = settings.ssh[selected];
            editing_ssh = selected;
            secure_clear(password, sizeof(password));
            screen = SSH; field = edit ? S_PASSWORD : S_CONNECT;
            if (edit) status("Profile loaded. Enter password if needed; F9 connects.");
            else connect_ssh();
        }
    } else if (selected < (int)count && (ev.key == KEY_DEL || ev.key == KEY_F8)) {
        if (!confirm("Delete saved entry?", "This removes it from the SD settings.", NULL)) return;
        candidate = settings;
        if (screen == NETWORKS) {
            bool active = !strcmp(active_ssid, settings.wifi[selected].ssid);
            bool pending = save_join && !strcmp(joining_wifi.ssid, settings.wifi[selected].ssid);
            bool in_form = !strcmp(wifi.ssid, settings.wifi[selected].ssid);
            wifi_remove(&candidate, (unsigned)selected);
            if (commit()) {
                if (active || pending) {
                    wifi_disconnect(); save_join = false; active_ssid[0] = 0;
                    secure_clear(&joining_wifi, sizeof(joining_wifi));
                }
                if (active || pending || in_form) secure_clear(&wifi, sizeof(wifi));
                status("WiFi network forgotten.");
            }
        } else if (screen == PROFILES) {
            ssh_remove(&candidate, (unsigned)selected);
            if (commit()) {
                if (editing_ssh == selected) editing_ssh = -1;
                else if (editing_ssh > selected) editing_ssh--;
                status("SSH profile deleted.");
            }
        } else {
            host_remove(&candidate, (unsigned)selected);
            if (commit()) status("Host key forgotten. Verify it again next connection.");
        }
        count = list_count();
        if (selected >= (int)count) selected = count ? (int)count - 1 : 0;
    }
    dirty = true;
}

static void files_key(key_event_t ev) {
    if (!wifi_ready()) { screen = SSH; field = S_SAVED; status("WiFi lost; SSH details are locked."); return; }
    if (ev.key == KEY_UP && selected > 0) selected--;
    else if (ev.key == KEY_DOWN && selected + 1 < (int)file_count) selected++;
    else if (ev.key == KEY_PAGE_DOWN && more_files) { file_page++; load_directory(); }
    else if (ev.key == KEY_PAGE_UP && file_page) { file_page--; load_directory(); }
    else if (ev.key == KEY_BACKSPACE) {
        char *slash = strrchr(browser_path, '/');
        if (slash && slash != browser_path) *slash = 0;
        else strcpy(browser_path, "/");
        file_page = 0; load_directory();
    } else if (ev.key == KEY_ENTER && file_count) {
        const file_entry_t *f = &files[selected];
        char path[KEY_PATH_LEN];
        int n = snprintf(path, sizeof(path), "%s%s%s", browser_path,
                         strcmp(browser_path, "/") ? "/" : "", f->name);
        if (n < 0 || (size_t)n >= sizeof(path)) { status("Path exceeds 255 characters."); return; }
        if (f->directory) { strcpy(browser_path, path); file_page = 0; load_directory(); }
        else {
            strcpy(ssh.key_path, path);
            screen = SSH; field = S_KEY;
            status("Key selected. F9 connects; F6 saves profile.");
        }
    }
    dirty = true;
}

/* After an established session ends: saved presets return to their list, new ones to the form. */
static void session_ended(const char *message) {
    was_connected = false;
    if (session_profile >= 0 && (unsigned)session_profile < settings.ssh_count) {
        screen = PROFILES;
        selected = session_profile;
    } else {
        screen = SSH;
        field = wifi_ready() ? S_CONNECT : S_SAVED;
    }
    status(message);
}

static void reboot_now(void) {
    if (prefs_dirty) {
        prefs_dirty = false;
        storage_save_prefs(&prefs);
    }
    gfx_fill(POPUP_X + 6, POPUP_Y + 24, POPUP_W - 12, 48, C_SURFACE);
    popup_line(3, "Rebooting...", C_ACCENT);
    lcd_flush();
    update_ui = reboot_countdown = false;
    system_reboot_to_loader();
}

static void update_key(key_event_t ev) {
    dirty = true;
    if (reboot_countdown) {
        if (ev.key == KEY_ENTER) reboot_now();
        else if (ev.key == KEY_ESC) {
            update_ui = reboot_countdown = false;
            status("Update saved. To install it, hold Up at power-on, then choose PicoCalc SSH in the loader menu.");
        }
        return;
    }
    update_state_t s = updater_state();
    if (s == UPDATE_AVAILABLE) {
        if (ev.key == KEY_ENTER && wifi_ready()) updater_install();
        else if (ev.key == KEY_ENTER || ev.key == KEY_ESC) {
            updater_cancel();
            update_ui = false;
            status(ev.key == KEY_ESC ? "Update postponed." : "WiFi disconnected; update cancelled.");
        }
    } else if (s == UPDATE_VERIFYING || s == UPDATE_INSTALLING) {
        /* Not cancellable: the app file is being replaced. */
    } else if (updater_busy()) {
        if (ev.key == KEY_ESC) {
            updater_cancel();
            update_ui = false;
            status("Update cancelled.");
        }
    } else {
        updater_cancel();
        update_ui = false;
    }
}

static void key(key_event_t ev) {
    startup_scan = false;
    if (update_ui) { update_key(ev); return; }
    if (ssh_countdown) {
        /* The countdown popup: Enter goes now, function keys navigate, others just dismiss. */
        ssh_countdown = false;
        dirty = true;
        if (ev.key == KEY_ENTER) { screen = PROFILES; selected = 0; return; }
        if (!(ev.key >= KEY_F1 && ev.key <= KEY_F10)) return;
    }
    if (screen == TERMINAL) {
        if (((ev.mods & MOD_CTRL) && ev.key == ']') || ev.key == 29) {
            ssh_stop(); session_ended("SSH disconnected."); return;
        }
        uint8_t bytes[16];
        unsigned n = terminal_key(&terminal, ev, bytes);
        if (n && !ssh_send(bytes, n)) status("SSH input buffer full; key was not sent.");
        return;
    }
    if (ssh_active()) {
        if (ev.key == KEY_ESC) { ssh_stop(); screen = PROFILES; selected = 0; status("SSH connection cancelled."); }
        return;
    }
    if (wifi_busy()) {
        /* The joining popup is modal; only Esc (cancel) is accepted. */
        if (ev.key == KEY_ESC) {
            wifi_disconnect(); save_join = false;
            secure_clear(&joining_wifi, sizeof(joining_wifi));
            status("WiFi join cancelled.");
        }
        return;
    }
    if (ev.key == KEY_ESC) {
        if (screen == FILES) { screen = SSH; field = S_KEY; }
        else if (screen == SSH) { screen = PROFILES; selected = 0; }
        else if (screen == WIFI) { screen = NETWORKS; selected = 0; }
        else if (screen == SCAN) { screen = WIFI; field = W_SSID; }
        else if (screen == SETTINGS) { screen = NETWORKS; selected = 0; }
        else if (screen == HELP) {
            screen = help_return;
            if (screen == SSH) field = wifi_ready() ? S_HOST : S_SAVED;
        }
        dirty = true;
        return;
    }
    if (ev.key == KEY_F1 || ev.key == KEY_F2 || ev.key == KEY_F3) {
        screen = ev.key == KEY_F1 ? NETWORKS : ev.key == KEY_F2 ? PROFILES : HOSTS;
        selected = 0; notice[0] = 0; dirty = true; return;
    }
    if (ev.key == KEY_F4) {
        screen = SETTINGS; setting = P_SCREEN; notice[0] = 0; dirty = true; return;
    }
    if (ev.key == KEY_F10) {
        if (screen != HELP) help_return = screen;
        screen = HELP; dirty = true; return;
    }
    if (screen == FILES) { files_key(ev); return; }
    if (screen == WIFI || screen == SSH) form_key(ev);
    else if (screen == SCAN) scan_key(ev);
    else if (screen == SETTINGS) settings_key(ev);
    else if (screen == NETWORKS || screen == PROFILES || screen == HOSTS) list_key(ev);
}

void app_init(void) {
    screen = help_return = NETWORKS;
    field = W_SSID;
    selected = 0;
    dirty = true;
    save_join = was_connected = was_wifi = false;
    session_profile = -1;
    startup_scan = ssh_countdown = false;
    update_ui = reboot_countdown = false;
    last_draw = 0;
    editing_ssh = -1;
    prefs_dirty = false;
    notice[0] = active_ssid[0] = 0;
    secure_clear(&wifi, sizeof(wifi));
    secure_clear(&joining_wifi, sizeof(joining_wifi));
    secure_clear(password, sizeof(password));
    memset(&ssh, 0, sizeof(ssh));
    strcpy(ssh.port, "22");
    bool ok = storage_init(&settings);
    if (!ok) status(storage_error());
    if (!storage_load_prefs(&prefs) && ok) status(storage_error());
    apply_backlights();
    bool network_ok = network_init();
    if (!network_ok) status(wifi_status());
    if (network_ok && !prefs.auto_join) {
        if (ok) status("Startup auto-join is off (F4). Choose a network.");
    } else if (network_ok) {
        bool started = wifi_scan_start();
        startup_scan = started && ok;
        if (!started && ok) status(wifi_scan_status());
    }
    terminal_init(&terminal);
}

void app_poll(void) {
    network_poll();
    kbd_poll();
    if (startup_scan && !wifi_scan_busy()) {
        startup_scan = false;
        unsigned count;
        const wifi_scan_result_t *results = wifi_scan_results(&count);
        int best = -1;
        int signal = -32768;
        for (unsigned i = 0; i < count; i++) {
            if (results[i].auth_mode && !(results[i].auth_mode & 6)) continue;
            for (unsigned j = 0; j < settings.wifi_count; j++) {
                if (!strcmp(results[i].ssid, settings.wifi[j].ssid) &&
                    (results[i].auth_mode != 0) == (settings.wifi[j].password[0] != 0) &&
                    results[i].rssi > signal) {
                    best = (int)j;
                    signal = results[i].rssi;
                }
            }
        }
        if (best >= 0) { wifi = settings.wifi[best]; join_wifi(); }
        else status(count ? "No compatible known WiFi found. Choose a network or add one." : wifi_scan_status());
    }
    bool up = wifi_ready();
    if (up != was_wifi) {
        was_wifi = up;
        dirty = true;
        if (!up && screen == SSH) field = S_SAVED;
        if (!up) { active_ssid[0] = 0; ssh_countdown = false; }
    }
    if (save_join && up) {
        save_join = false;
        snprintf(active_ssid, sizeof(active_ssid), "%s", joining_wifi.ssid);
        candidate = settings;
        if (!wifi_remember_connection(&candidate, &joining_wifi)) {
            secure_clear(&candidate, sizeof(candidate));
            status("WiFi connected; list full. Delete an entry on F1 to remember this network.");
        } else if (!memcmp(&candidate, &settings, sizeof(settings))) {
            secure_clear(&candidate, sizeof(candidate));
            status("WiFi reconnected. F2 opens SSH.");
        } else if (commit()) status("WiFi connected and remembered. F2 opens SSH.");
        secure_clear(&joining_wifi, sizeof(joining_wifi));
        if (screen == WIFI || screen == NETWORKS || screen == SCAN) {
            screen = NETWORKS; selected = 0;
            ssh_countdown = prefs.auto_ssh;
            ssh_deadline = to_ms_since_boot(get_absolute_time()) + 3000;
            dirty = true;
        }
    } else if (save_join && !wifi_busy()) {
        save_join = false;
        secure_clear(&joining_wifi, sizeof(joining_wifi));
        status(wifi_status());
    }
    bool active_before = ssh_active();
    ssh_poll();
    if (ssh_connected() && !was_connected) {
        was_connected = true;
        screen = TERMINAL;
        notice[0] = 0;
        dirty = true;
    }
    if (ssh_connected()) {
        uint8_t data[1024];
        for (int i = 0; i < 4; i++) {
            int n = ssh_read(data, sizeof(data));
            if (n <= 0) break;
            terminal_feed(&terminal, data, (size_t)n, ssh_send);
        }
    }
    if (active_before && !ssh_active() && was_connected) {
        session_ended(ssh_status());
    } else if (active_before && !ssh_active()) {
        /* Failed attempts stay on the form with Connect focused for an easy retry. */
        screen = SSH; field = wifi_ready() ? S_CONNECT : S_SAVED;
        if (!notice[0]) status(ssh_status());
        else {
            /* Keep specific trust/storage failures instead of replacing them with a generic SSH error. */
            dirty = true;
        }
    }
    if (update_ui && !reboot_countdown) {
        update_state_t before = updater_state();
        if (updater_poll() == UPDATE_DONE) {
            reboot_countdown = true;
            reboot_deadline = to_ms_since_boot(get_absolute_time()) + REBOOT_DELAY_MS;
        }
        if (updater_state() != before) dirty = true;
    }
    key_event_t ev;
    for (int i = 0; i < 8 && kbd_get(&ev); i++) key(ev);
    if (kbd_take_power_cycle()) { lcd_reinit(); dirty = true; }
    uint32_t now = to_ms_since_boot(get_absolute_time());
    if (ssh_countdown) {
        dirty = true;
        if (!wifi_ready()) ssh_countdown = false;
        else if ((int32_t)(now - ssh_deadline) >= 0) {
            ssh_countdown = false;
            screen = PROFILES; selected = 0;
        }
    }
    if (reboot_countdown && (int32_t)(now - reboot_deadline) >= 0) reboot_now();
    if (prefs_dirty && (int32_t)(now - prefs_save_at) >= 0) {
        prefs_dirty = false;
        if (!storage_save_prefs(&prefs)) status(storage_error());
    }
    bool charging = false;
    int level = kbd_battery_percent(&charging);
    if (level != shown_battery || charging != shown_charging) dirty = true;
    static uint32_t frame;
    uint32_t this_frame = now / 250;
    if (this_frame != frame) {
        frame = this_frame;
        if (wifi_busy() || ssh_pending() || update_ui || (screen == SETTINGS && this_frame % 4 == 0)) dirty = true;
    }
    static bool scan_was_busy;
    bool scan_busy = wifi_scan_busy();
    if ((screen == SCAN || startup_scan) && (scan_busy || scan_was_busy)) dirty = true;
    scan_was_busy = scan_busy;
    if ((dirty || (screen == TERMINAL && terminal.dirty)) && now - last_draw >= 40) {
        if (screen == WIFI) draw_wifi();
        else if (screen == SSH) draw_ssh();
        else if (screen == FILES) draw_files();
        else if (screen == SCAN) draw_scan();
        else if (screen == TERMINAL) draw_terminal();
        else if (screen == HELP) draw_help();
        else if (screen == SETTINGS) draw_settings();
        else draw_list();
        draw_popup();
        lcd_flush();
        dirty = false;
        last_draw = now;
    }
}

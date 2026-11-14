#include "network.h"
#include "pico/cyw43_arch.h"
#include "lwip/dns.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

cyw43_t cyw43_state;
static uint64_t clock_us;
static int init_result, scan_result_code, link_status, joins, leaves;
static bool driver_scanning;
static int (*callback)(void *, const cyw43_ev_scan_result_t *);
static void *callback_arg;
absolute_time_t get_absolute_time(void) { return clock_us; }
int cyw43_arch_init(void) { return init_result; }
void cyw43_arch_enable_sta_mode(void) {}
void cyw43_arch_poll(void) {}
int cyw43_wifi_leave(cyw43_t *state, int itf) { (void)state; (void)itf; leaves++; return 0; }
int cyw43_arch_wifi_connect_async(const char *ssid, const char *pw, uint32_t auth) {
    (void)ssid; (void)pw; (void)auth; joins++; return 0;
}
int cyw43_tcpip_link_status(cyw43_t *state, int itf) { (void)state; (void)itf; return link_status; }
int cyw43_wifi_scan(cyw43_t *state, cyw43_wifi_scan_options_t *opts, void *arg,
                    int (*cb)(void *, const cyw43_ev_scan_result_t *)) {
    (void)state; (void)opts;
    callback = cb;
    callback_arg = arg;
    driver_scanning = true;
    return scan_result_code;
}
bool cyw43_wifi_scan_active(cyw43_t *state) { (void)state; return driver_scanning; }
void tcp_arg(struct tcp_pcb *pcb, void *arg) { (void)pcb; (void)arg; }
void tcp_recv(struct tcp_pcb *pcb, err_t (*cb)(void *, struct tcp_pcb *, struct pbuf *, err_t)) { (void)pcb; (void)cb; }
void tcp_err(struct tcp_pcb *pcb, void (*cb)(void *, err_t)) { (void)pcb; (void)cb; }
void tcp_abort(struct tcp_pcb *pcb) { (void)pcb; }
unsigned pbuf_copy_partial(const struct pbuf *p, void *data, unsigned n, unsigned off) {
    (void)p; (void)data; (void)off; return n;
}
void pbuf_free(struct pbuf *p) { (void)p; }
struct tcp_pcb *tcp_new_ip_type(unsigned type) { (void)type; return NULL; }
err_t tcp_connect(struct tcp_pcb *pcb, const ip_addr_t *ip, uint16_t port,
                  err_t (*cb)(void *, struct tcp_pcb *, err_t)) {
    (void)pcb; (void)ip; (void)port; (void)cb; return ERR_CONN;
}
void tcp_recved(struct tcp_pcb *pcb, u16_t n) { (void)pcb; (void)n; }
unsigned tcp_sndbuf(struct tcp_pcb *pcb) { (void)pcb; return 0; }
err_t tcp_write(struct tcp_pcb *pcb, const void *data, u16_t n, unsigned flags) {
    (void)pcb; (void)data; (void)n; (void)flags; return ERR_CONN;
}
err_t tcp_output(struct tcp_pcb *pcb) { (void)pcb; return ERR_CONN; }
char *ip4addr_ntoa_r(const ip4_addr_t *ip, char *out, int size) {
    (void)ip; snprintf(out, (size_t)size, "192.0.2.1"); return out;
}
err_t dns_gethostbyname(const char *host, ip_addr_t *ip,
                        void (*cb)(const char *, const ip_addr_t *, void *), void *arg) {
    (void)host; (void)ip; (void)cb; (void)arg; return ERR_VAL;
}
static void result(const char *ssid, int16_t rssi, uint8_t auth) {
    cyw43_ev_scan_result_t r = {.rssi = rssi, .auth_mode = auth, .ssid_len = (uint8_t)strlen(ssid)};
    assert(r.ssid_len <= 32);
    memcpy(r.ssid, ssid, r.ssid_len);
    assert(callback(callback_arg, &r) == 0);
}
int main(void) {
    unsigned count;
    assert(!wifi_scan_start());
    assert(strstr(wifi_scan_status(), "unavailable"));
    init_result = -1;
    assert(!network_init() && !wifi_scan_start());
    init_result = 0;
    assert(network_init());
    scan_result_code = -7;
    assert(!wifi_scan_start() && !wifi_scan_busy());
    assert(strstr(wifi_scan_status(), "-7"));
    scan_result_code = 0;
    assert(wifi_scan_start() && wifi_scan_busy());
    assert(!wifi_scan_start());
    wifi_profile_t p = {.ssid = "home", .password = "password"};
    assert(!wifi_connect(&p) && joins == 0 && leaves == 0);
    result("", -10, 0); /* Hidden SSIDs are not selectable. */
    cyw43_ev_scan_result_t invalid = {.ssid_len = 33};
    callback(callback_arg, &invalid);
    invalid.ssid_len = 2; invalid.ssid[0] = 'a'; invalid.ssid[1] = 0;
    callback(callback_arg, &invalid);
    result("Home", -70, 5);
    result("Cafe", -30, 0);
    result("Home", -50, 5);
    result("Home", -80, 5);
    result("Home", -60, 0); /* Same SSID with different security is distinct. */
    result("12345678901234567890123456789012", -65, 5);
    wifi_scan_results(&count);
    assert(count == 4);
    driver_scanning = false;
    network_poll();
    assert(!wifi_scan_busy());
    const wifi_scan_result_t *results = wifi_scan_results(&count);
    assert(!strcmp(results[0].ssid, "Cafe") && results[0].rssi == -30);
    assert(!strcmp(results[1].ssid, "Home") && results[1].rssi == -50);
    assert(strlen(results[3].ssid) == 32);
    result("Late", -20, 0);
    wifi_scan_results(&count); assert(count == 4);
    assert(wifi_connect(&p) && joins == 1 && wifi_busy());
    assert(!wifi_scan_start() && strstr(wifi_scan_status(), "join"));
    link_status = CYW43_LINK_UP;
    cyw43_state.netif[0].address.addr = 1;
    network_poll();
    assert(wifi_ready());
    int prior_leaves = leaves;
    assert(wifi_scan_start() && wifi_ready() && leaves == prior_leaves);
    for (unsigned i = 0; i < MAX_SCAN_NETWORKS + 5; i++) {
        char ssid[33];
        snprintf(ssid, sizeof(ssid), "AP%u", i);
        result(ssid, (int16_t)(-20 - (int)i), 5);
    }
    driver_scanning = false; network_poll();
    wifi_scan_results(&count);
    assert(count == MAX_SCAN_NETWORKS && strstr(wifi_scan_status(), "limit"));
    assert(wifi_ready() && leaves == prior_leaves);
    assert(wifi_scan_start());
    clock_us += 20000000;
    network_poll();
    assert(!wifi_scan_busy() && strstr(wifi_scan_status(), "timed out"));
    result("Late", -10, 0);
    wifi_scan_results(&count); assert(count == 0);
    assert(wifi_scan_start()); /* Recovery while the driver still reports the timed-out scan active. */
    driver_scanning = false; network_poll();
    assert(strstr(wifi_scan_status(), "No networks found"));
    puts("Async WiFi scanning, deduplication, sorting, limits, timeout/error recovery and link preservation passed.");
    return 0;
}

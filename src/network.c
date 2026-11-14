#include "network.h"
#include "pico/cyw43_arch.h"
#include "lwip/dns.h"
#include "lwip/tcp.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool initialized, joining, ready;
static uint32_t wifi_deadline, tcp_deadline;
static char status[80] = "WiFi not connected", address[20];
static struct tcp_pcb *pcb;
static enum { NET_IDLE, NET_DNS, NET_CONNECT, NET_READY, NET_FAILED } state;
static char tcp_error[80];
static uint16_t target_port;
static uintptr_t generation;
static bool peer_closed;
static uint8_t rx[32768];
static unsigned rx_head, rx_count;
static wifi_scan_result_t scan_results[MAX_SCAN_NETWORKS];
static unsigned scan_count;
static bool scanning, scan_full;
static uint32_t scan_deadline;
static char scan_status[80] = "Select Scan networks to search for WiFi";

static uint32_t now_ms(void) { return to_ms_since_boot(get_absolute_time()); }
static bool expired(uint32_t deadline) { return (int32_t)(now_ms() - deadline) >= 0; }

bool network_init(void) {
    if (cyw43_arch_init()) {
        snprintf(status, sizeof(status), "WiFi hardware initialization failed");
        return false;
    }
    initialized = true;
    cyw43_arch_enable_sta_mode();
    return true;
}

bool wifi_ready(void) { return ready; }
bool wifi_busy(void) { return joining; }
const char *wifi_status(void) { return status; }
const char *wifi_address(void) { return address; }

bool wifi_scan_busy(void) { return scanning; }
const char *wifi_scan_status(void) { return scan_status; }
const wifi_scan_result_t *wifi_scan_results(unsigned *count) {
    *count = scan_count;
    return scan_results;
}

static int scan_result(void *arg, const cyw43_ev_scan_result_t *result) {
    (void)arg;
    if (!scanning || !result || !result->ssid_len || result->ssid_len > 32) return 0;
    if (memchr(result->ssid, 0, result->ssid_len)) return 0;
    unsigned i;
    for (i = 0; i < scan_count; i++)
        if (strlen(scan_results[i].ssid) == result->ssid_len &&
            !memcmp(scan_results[i].ssid, result->ssid, result->ssid_len) &&
            scan_results[i].auth_mode == result->auth_mode) break;
    if (i == scan_count) {
        if (scan_count == MAX_SCAN_NETWORKS) { scan_full = true; return 0; }
        memcpy(scan_results[i].ssid, result->ssid, result->ssid_len);
        scan_results[i].ssid[result->ssid_len] = 0;
        scan_results[i].auth_mode = result->auth_mode;
        scan_results[i].rssi = result->rssi;
        scan_count++;
    } else if (result->rssi > scan_results[i].rssi) scan_results[i].rssi = result->rssi;
    return 0;
}

bool wifi_scan_start(void) {
    if (!initialized || joining || scanning) {
        snprintf(scan_status, sizeof(scan_status), "%s",
                 !initialized ? "WiFi hardware unavailable" :
                 joining ? "Wait for WiFi join, or cancel it with Esc" : "WiFi scan already in progress...");
        return false;
    }
    scan_count = 0;
    scan_full = false;
    scanning = true;
    scan_deadline = now_ms() + 20000;
    cyw43_wifi_scan_options_t options = {0};
    int r = cyw43_wifi_scan(&cyw43_state, &options, NULL, scan_result);
    if (r) {
        scanning = false;
        snprintf(scan_status, sizeof(scan_status), "Cannot start WiFi scan (%d); select Scan again", r);
        return false;
    }
    snprintf(scan_status, sizeof(scan_status), "Scanning... please wait (Esc cancels auto-join)");
    return true;
}

static int compare_signal(const void *a, const void *b) {
    const wifi_scan_result_t *left = a, *right = b;
    return (int)right->rssi - left->rssi;
}

void transport_close(void) {
    generation++;
    if (pcb) {
        tcp_arg(pcb, NULL);
        tcp_recv(pcb, NULL);
        tcp_err(pcb, NULL);
        tcp_abort(pcb);
        pcb = NULL;
    }
    state = NET_IDLE;
    peer_closed = false;
    rx_count = rx_head = 0;
}

void wifi_disconnect(void) {
    transport_close();
    if (initialized) cyw43_wifi_leave(&cyw43_state, CYW43_ITF_STA);
    ready = joining = false;
    address[0] = 0;
    snprintf(status, sizeof(status), "WiFi disconnected");
}

bool wifi_connect(const wifi_profile_t *p) {
    if (!initialized) return false;
    if (scanning) {
        snprintf(status, sizeof(status), "WiFi scan in progress; wait before joining");
        return false;
    }
    if (!valid_wifi(p)) {
        snprintf(status, sizeof(status), "SSID required; password: 8-63 chars");
        return false;
    }
    wifi_disconnect();
    int r = cyw43_arch_wifi_connect_async(p->ssid, p->password[0] ? p->password : NULL,
                                         p->password[0] ? CYW43_AUTH_WPA2_MIXED_PSK : CYW43_AUTH_OPEN);
    if (r) {
        snprintf(status, sizeof(status), "WiFi start failed (%d)", r);
        return false;
    }
    joining = true;
    wifi_deadline = now_ms() + 30000;
    snprintf(status, sizeof(status), "Joining WiFi / waiting for DHCP...");
    return true;
}

static void tcp_fail(const char *message, int code) {
    transport_close();
    state = NET_FAILED;
    snprintf(tcp_error, sizeof(tcp_error), "%s (%d)", message, code);
}

static void on_error(void *arg, err_t err) {
    (void)arg;
    pcb = NULL; /* lwIP already freed the PCB. */
    tcp_fail("TCP connection failed", err);
}

static err_t on_receive(void *arg, struct tcp_pcb *connection, struct pbuf *p, err_t err) {
    (void)arg;
    (void)connection;
    if (!p) {
        peer_closed = true;
        return ERR_OK;
    }
    if (err != ERR_OK) return err;
    if (p->tot_len > sizeof(rx) - rx_count) return ERR_MEM;
    unsigned tail = (rx_head + rx_count) % sizeof(rx);
    unsigned first = sizeof(rx) - tail;
    if (first > p->tot_len) first = p->tot_len;
    pbuf_copy_partial(p, rx + tail, first, 0);
    pbuf_copy_partial(p, rx, p->tot_len - first, first);
    rx_count += p->tot_len;
    pbuf_free(p);
    return ERR_OK;
}

static err_t on_connected(void *arg, struct tcp_pcb *connection, err_t err) {
    (void)arg;
    (void)connection;
    if (err != ERR_OK) {
        tcp_fail("TCP connect failed", err);
        return ERR_ABRT;
    }
    state = NET_READY;
    return ERR_OK;
}

static void start_tcp(const ip_addr_t *ip) {
    pcb = tcp_new_ip_type(IPADDR_TYPE_V4);
    if (!pcb) { tcp_fail("No memory for TCP", ERR_MEM); return; }
    tcp_recv(pcb, on_receive);
    tcp_err(pcb, on_error);
    state = NET_CONNECT;
    err_t r = tcp_connect(pcb, ip, target_port, on_connected);
    if (r != ERR_OK) tcp_fail("TCP connect failed", r);
}

static void on_dns(const char *name, const ip_addr_t *ip, void *arg) {
    (void)name;
    if ((uintptr_t)arg != generation || state != NET_DNS) return;
    if (!ip) tcp_fail("DNS lookup failed", ERR_VAL);
    else start_tcp(ip);
}

bool transport_connect(const char *host, uint16_t port) {
    transport_close();
    if (!ready) { tcp_fail("WiFi not connected", ERR_CONN); return false; }
    target_port = port;
    tcp_deadline = now_ms() + 20000;
    state = NET_DNS;
    ip_addr_t ip;
    err_t r = dns_gethostbyname(host, &ip, on_dns, (void *)generation);
    if (r == ERR_OK) start_tcp(&ip);
    else if (r != ERR_INPROGRESS) tcp_fail("DNS lookup failed", r);
    return state != NET_FAILED;
}

bool transport_ready(void) { return state == NET_READY; }
bool transport_failed(void) { return state == NET_FAILED; }
const char *transport_error(void) { return tcp_error; }

int transport_read(void *data, unsigned size) {
    if (!rx_count) return peer_closed || state != NET_READY ? -1 : 0;
    if (size > rx_count) size = rx_count;
    unsigned first = sizeof(rx) - rx_head;
    if (first > size) first = size;
    memcpy(data, rx + rx_head, first);
    memcpy((uint8_t *)data + first, rx, size - first);
    rx_head = (rx_head + size) % sizeof(rx);
    rx_count -= size;
    if (pcb) tcp_recved(pcb, (u16_t)size);
    return (int)size;
}

int transport_write(const void *data, unsigned size) {
    if (!pcb || state != NET_READY || peer_closed) return -1;
    if (size > tcp_sndbuf(pcb)) size = tcp_sndbuf(pcb);
    if (!size) return 0;
    err_t r = tcp_write(pcb, data, (u16_t)size, TCP_WRITE_FLAG_COPY);
    if (r == ERR_MEM) return 0;
    if (r != ERR_OK) { tcp_fail("TCP write failed", r); return -1; }
    r = tcp_output(pcb);
    if (r != ERR_OK && r != ERR_MEM) {
        tcp_fail("TCP output failed", r);
        return -1;
    }
    return (int)size;
}

void network_poll(void) {
    if (!initialized) return;
    cyw43_arch_poll();
    if (scanning && (!cyw43_wifi_scan_active(&cyw43_state) || expired(scan_deadline))) {
        bool timed_out = cyw43_wifi_scan_active(&cyw43_state);
        scanning = false;
        qsort(scan_results, scan_count, sizeof(scan_results[0]), compare_signal);
        if (timed_out) {
            scan_count = 0;
            snprintf(scan_status, sizeof(scan_status), "WiFi scan timed out; select Scan again");
        } else if (!scan_count)
            snprintf(scan_status, sizeof(scan_status), "No networks found; select Scan again or enter SSID");
        else
            snprintf(scan_status, sizeof(scan_status), "%u networks%s. Enter selects; Tab: Scan again",
                     scan_count, scan_full ? " (list limit reached)" : "");
    }
    int link = cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA);
    bool up = link == CYW43_LINK_UP &&
              !ip4_addr_isany_val(*netif_ip4_addr(&cyw43_state.netif[CYW43_ITF_STA]));
    if (up) {
        if (!ready) {
            ip4addr_ntoa_r(netif_ip4_addr(&cyw43_state.netif[CYW43_ITF_STA]), address, sizeof(address));
            snprintf(status, sizeof(status), "WiFi ready: %s", address);
        }
        ready = true;
        joining = false;
    } else if (joining && (link < 0 || expired(wifi_deadline))) {
        wifi_disconnect();
        snprintf(status, sizeof(status), "WiFi failed (%d): check SSID/password", link);
    } else if (ready) {
        ready = false;
        address[0] = 0;
        tcp_fail("WiFi connection lost", link);
        snprintf(status, sizeof(status), "WiFi lost; reconnect to continue");
    }
    if ((state == NET_DNS || state == NET_CONNECT) && expired(tcp_deadline))
        tcp_fail("DNS/TCP connection timed out", ERR_TIMEOUT);
}

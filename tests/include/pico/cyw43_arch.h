#ifndef TEST_CYW43_ARCH_H
#define TEST_CYW43_ARCH_H
#include "pico/stdlib.h"
#include "lwip/tcp.h"
#define CYW43_ITF_STA 0
#define CYW43_AUTH_OPEN 0
#define CYW43_AUTH_WPA2_MIXED_PSK 1
#define CYW43_LINK_UP 3
typedef struct { struct netif netif[1]; } cyw43_t;
typedef struct { unsigned unused; } cyw43_wifi_scan_options_t;
typedef struct {
    uint8_t ssid_len, ssid[32], auth_mode;
    int16_t rssi;
} cyw43_ev_scan_result_t;
extern cyw43_t cyw43_state;
int cyw43_arch_init(void);
void cyw43_arch_enable_sta_mode(void);
void cyw43_arch_poll(void);
int cyw43_wifi_leave(cyw43_t *, int);
int cyw43_arch_wifi_connect_async(const char *, const char *, uint32_t);
int cyw43_tcpip_link_status(cyw43_t *, int);
int cyw43_wifi_scan(cyw43_t *, cyw43_wifi_scan_options_t *, void *,
                    int (*)(void *, const cyw43_ev_scan_result_t *));
bool cyw43_wifi_scan_active(cyw43_t *);
#endif

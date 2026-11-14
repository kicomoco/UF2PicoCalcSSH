#ifndef NETWORK_H
#define NETWORK_H
#include "model.h"
bool network_init(void);
void network_poll(void);
bool wifi_connect(const wifi_profile_t *profile);
void wifi_disconnect(void);
bool wifi_ready(void);
bool wifi_busy(void);
const char *wifi_status(void);
const char *wifi_address(void);
#define MAX_SCAN_NETWORKS 32
typedef struct {
    char ssid[33];
    int16_t rssi;
    uint8_t auth_mode;
} wifi_scan_result_t;
bool wifi_scan_start(void);
bool wifi_scan_busy(void);
const char *wifi_scan_status(void);
const wifi_scan_result_t *wifi_scan_results(unsigned *count);
bool transport_connect(const char *host, uint16_t port);
void transport_close(void);
bool transport_ready(void);
bool transport_failed(void);
const char *transport_error(void);
int transport_read(void *data, unsigned size);
int transport_write(const void *data, unsigned size);
#endif

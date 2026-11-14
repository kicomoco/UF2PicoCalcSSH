#ifndef HTTPS_H
#define HTTPS_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Nonblocking HTTPS GET over the shared transport_* TCP connection.
 * Certificates are verified against the embedded CA roots (ca_roots.c).
 * The sink receives the 200 response body; returning false aborts the request. */
typedef bool (*https_sink_fn)(void *ctx, const uint8_t *data, size_t size);
typedef enum { HTTPS_IDLE, HTTPS_BUSY, HTTPS_DONE, HTTPS_FAILED } https_state_t;

#define HTTPS_URL_LEN 1280

bool https_get(const char *url, size_t max_body, https_sink_fn sink, void *ctx);
https_state_t https_poll(void);
void https_cancel(void);
const char *https_error(void);
/* Body bytes delivered so far and the announced length (0 when unknown). */
size_t https_received(void);
size_t https_length(void);
#endif

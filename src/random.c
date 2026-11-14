#include "pico/rand.h"
#include "pico/time.h"
#include <time.h>
#include <string.h>
int ssh_random_seed(unsigned char *out, unsigned int size) {
    while (size) {
        uint64_t value = get_rand_64();
        unsigned n = size < sizeof(value) ? size : sizeof(value);
        memcpy(out, &value, n);
        out += n;
        size -= n;
    }
    return 0;
}

#ifndef BUILD_EPOCH
#define BUILD_EPOCH 1767225600 /* 2026-01-01 */
#endif
/* No RTC: never earlier than the firmware build, so expired certificates are still rejected. */
time_t tls_time(time_t *t) {
    time_t now = (time_t)BUILD_EPOCH + (time_t)(to_us_since_boot(get_absolute_time()) / 1000000u);
    if (t) *t = now;
    return now;
}

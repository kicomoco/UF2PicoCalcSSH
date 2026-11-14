#include "pico/rand.h"
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

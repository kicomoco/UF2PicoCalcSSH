#ifndef TEST_PICO_STDLIB_H
#define TEST_PICO_STDLIB_H
#include <stdbool.h>
#include <stdint.h>
typedef uint64_t absolute_time_t;
absolute_time_t get_absolute_time(void);
absolute_time_t make_timeout_time_ms(uint32_t ms);
bool time_reached(absolute_time_t deadline);
void sleep_ms(uint32_t ms);
static inline uint32_t to_ms_since_boot(absolute_time_t t) { return (uint32_t)(t / 1000); }
#endif

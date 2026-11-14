#include "log.h"
#include <stdarg.h>
#include <stdio.h>
void app_log(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
    putchar('\n');
}

#ifndef LOG_H
#define LOG_H
void app_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
#endif

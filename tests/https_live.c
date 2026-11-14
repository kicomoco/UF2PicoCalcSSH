/* Live HTTPS checks against GitHub (needs internet): real certificate chains, redirects
 * to release-assets.githubusercontent.com, a stale clock, and rejection cases. */
#define _POSIX_C_SOURCE 200809L
#include "https.h"
#include "network.h"
#include "pico/stdlib.h"
#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

extern time_t tls_test_time;
absolute_time_t get_absolute_time(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000u + (uint64_t)t.tv_nsec / 1000u;
}

static char body[4096];
static size_t body_size;
static bool sink(void *ctx, const uint8_t *data, size_t size) {
    (void)ctx;
    if (size > sizeof(body) - 1 - body_size) return false;
    memcpy(body + body_size, data, size);
    body_size += size;
    body[body_size] = 0;
    return true;
}

static https_state_t run(const char *url, size_t limit) {
    body_size = 0;
    body[0] = 0;
    if (!https_get(url, limit, sink, NULL)) return HTTPS_FAILED;
    https_state_t s;
    const struct timespec pause = {0, 1000000};
    while ((s = https_poll()) == HTTPS_BUSY) nanosleep(&pause, NULL);
    return s;
}

int main(int argc, char **argv) {
    const char *repo = argc > 1 ? argv[1] : "kicomoco/UF2PicoCalcSSH";
    char url[256];
    signal(SIGPIPE, SIG_IGN);
    alarm(120);
    snprintf(url, sizeof(url), "https://github.com/%s/releases/latest/download/SHA256SUMS", repo);

    assert(run(url, sizeof(body) - 1) == HTTPS_DONE);
    assert(body_size > 64 && strstr(body, "picocalc_ssh-v") && strstr(body, ".uf2"));
    printf("latest SHA256SUMS (%zu bytes): %s", body_size, body);

    /* The device clock starts at the build time, before the server certificates were issued. */
    tls_test_time = 1704067200; /* 2024-01-01 */
    assert(run(url, sizeof(body) - 1) == HTTPS_DONE && strstr(body, ".uf2"));
    puts("stale clock: accepted (not-yet-valid tolerated)");

    tls_test_time = 2524608000; /* 2050-01-01: every certificate has expired */
    assert(run(url, sizeof(body) - 1) == HTTPS_FAILED);
    printf("future clock: %s\n", https_error());
    assert(strstr(https_error(), "expired"));
    tls_test_time = 0;

    assert(run("https://www.google.com/", sizeof(body) - 1) == HTTPS_FAILED);
    printf("untrusted root: %s\n", https_error());
    assert(strstr(https_error(), "not trusted"));

    assert(run(url, 16) == HTTPS_FAILED);
    printf("size limit: %s\n", https_error());

    snprintf(url, sizeof(url), "https://github.com/%s/releases/download/v0.0.0-missing/none", repo);
    assert(run(url, sizeof(body) - 1) == HTTPS_FAILED && strstr(https_error(), "HTTP 404"));
    printf("missing asset: %s\n", https_error());
    puts("HTTPS live tests passed");
    return 0;
}

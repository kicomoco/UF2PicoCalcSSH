#include "model.h"
#include "ssh_client.h"
#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static bool allow;
static unsigned verified;
static bool verify(const char *host, uint16_t port, const uint8_t hash[32]) {
    assert(!strcmp(host, "127.0.0.1") && port > 0);
    unsigned sum = 0;
    for (unsigned i = 0; i < 32; i++) sum += hash[i];
    assert(sum != 0);
    verified++;
    return allow;
}
int main(int argc, char **argv) {
    assert(argc == 5);
    signal(SIGPIPE, SIG_IGN);
    alarm(30);
    allow = strcmp(argv[4], "reject") != 0;
    ssh_profile_t p = {.host = "127.0.0.1", .username = "test"};
    snprintf(p.port, sizeof(p.port), "%s", argv[1]);
    snprintf(p.key_path, sizeof(p.key_path), "%s", argv[3]);
    if (!ssh_start(&p, argv[2], verify)) {
        fprintf(stderr, "%s\n", ssh_status());
        return 2;
    }
    unsigned received = 0, sent = 0;
    while (ssh_active()) {
        ssh_poll();
        if (ssh_connected()) {
            if (!sent) {
                assert(verified > 0);
                assert(ssh_send("integration-ping\n", 17));
                sent = 1;
            }
            uint8_t data[1024];
            int n = ssh_read(data, sizeof(data));
            for (int i = 0; i < n; i++) {
                assert(data[i] == (uint8_t)('a' + received % 26));
                received++;
            }
            if (received == 65536) {
                ssh_stop();
                puts("SSH PTY, authentication, fragmented transport and 64 KiB receive passed.");
                return 0;
            }
        }
        usleep(1000);
    }
    if (!allow && verified && !sent) {
        puts("Host key rejection blocked authentication.");
        return 0;
    }
    if (!strcmp(argv[4], "bad-password") && verified && !sent) {
        puts("Invalid password rejected.");
        return 0;
    }
    fprintf(stderr, "SSH failed after %u bytes: %s\n", received, ssh_status());
    return 1;
}

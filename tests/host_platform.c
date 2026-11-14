#include "network.h"
#include "storage.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <netdb.h>
#include <time.h>
#include <unistd.h>

static int socket_fd = -1;
static bool connected, failed;
bool wifi_ready(void) { return true; }
const char *transport_error(void) { return "Test TCP connection failed"; }
void transport_close(void) {
    if (socket_fd >= 0) close(socket_fd);
    socket_fd = -1;
    connected = failed = false;
}
bool transport_connect(const char *host, uint16_t port) {
    transport_close();
    struct addrinfo hints = {.ai_family = AF_INET, .ai_socktype = SOCK_STREAM}, *found = NULL;
    char service[8];
    snprintf(service, sizeof(service), "%u", port);
    if (getaddrinfo(host, service, &hints, &found) || !found) return false;
    socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    bool ok = socket_fd >= 0 && !connect(socket_fd, found->ai_addr, found->ai_addrlen);
    freeaddrinfo(found);
    if (!ok) return false;
    int flags = fcntl(socket_fd, F_GETFL, 0);
    if (flags < 0 || fcntl(socket_fd, F_SETFL, flags | O_NONBLOCK)) return false;
    connected = true;
    return true;
}
bool transport_ready(void) { return connected; }
bool transport_failed(void) { return failed; }
int transport_read(void *data, unsigned size) {
    /* Fragment packets to exercise the production nonblocking SSH state machine. */
    if (size > 113) size = 113;
    ssize_t n = recv(socket_fd, data, size, 0);
    if (n > 0) return (int)n;
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return 0;
    return -1;
}
int transport_write(const void *data, unsigned size) {
    if (size > 127) size = 127;
    ssize_t n = send(socket_fd, data, size, 0);
    if (n >= 0) return (int)n;
    if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
    failed = true;
    return -1;
}
time_t tls_test_time;
time_t tls_time(time_t *t) {
    time_t now = tls_test_time ? tls_test_time : time(NULL);
    if (t) *t = now;
    return now;
}
int ssh_random_seed(unsigned char *out, unsigned int size) {
    arc4random_buf(out, size);
    return 0;
}
const char *storage_error(void) { return "Cannot read test key"; }
bool storage_read_key(const char *path, uint8_t **data, size_t *size) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    if (fseek(f, 0, SEEK_END)) { fclose(f); return false; }
    long length = ftell(f);
    if (length <= 0 || length > 16384 || fseek(f, 0, SEEK_SET)) { fclose(f); return false; }
    *data = malloc((size_t)length + 1);
    if (!*data) { fclose(f); return false; }
    *size = fread(*data, 1, (size_t)length, f);
    (*data)[*size] = 0;
    fclose(f);
    return *size == (size_t)length;
}

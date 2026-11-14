#include "network.h"
#include "storage.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
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
    socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (socket_fd < 0) return false;
    struct sockaddr_in address = {.sin_family = AF_INET, .sin_port = htons(port)};
    if (inet_pton(AF_INET, host, &address.sin_addr) != 1) return false;
    if (connect(socket_fd, (struct sockaddr *)&address, sizeof(address))) return false;
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

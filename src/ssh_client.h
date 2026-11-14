#ifndef SSH_CLIENT_H
#define SSH_CLIENT_H
#include "model.h"
typedef bool (*host_check_fn)(const char *host, uint16_t port, const uint8_t fingerprint[32]);
bool ssh_start(const ssh_profile_t *profile, const char *password, host_check_fn verify);
void ssh_poll(void);
void ssh_stop(void);
bool ssh_active(void);
bool ssh_connected(void);
const char *ssh_status(void);
bool ssh_send(const void *data, unsigned size);
int ssh_read(void *data, unsigned size);
#endif

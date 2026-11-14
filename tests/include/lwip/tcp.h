#ifndef TEST_LWIP_TCP_H
#define TEST_LWIP_TCP_H
#include <stdbool.h>
#include <stdint.h>
typedef int err_t;
typedef uint16_t u16_t;
typedef struct { uint32_t addr; } ip_addr_t;
typedef ip_addr_t ip4_addr_t;
struct netif { ip4_addr_t address; };
#define netif_ip4_addr(n) (&(n)->address)
#define ip4_addr_isany_val(ip) ((ip).addr == 0)
#define ERR_OK 0
#define ERR_MEM -1
#define ERR_CONN -2
#define ERR_VAL -3
#define ERR_INPROGRESS -4
#define ERR_TIMEOUT -5
#define ERR_ABRT -6
#define IPADDR_TYPE_V4 0
#define TCP_WRITE_FLAG_COPY 1
struct pbuf { uint16_t tot_len; };
struct tcp_pcb { int unused; };
void tcp_arg(struct tcp_pcb *, void *);
void tcp_recv(struct tcp_pcb *, err_t (*)(void *, struct tcp_pcb *, struct pbuf *, err_t));
void tcp_err(struct tcp_pcb *, void (*)(void *, err_t));
void tcp_abort(struct tcp_pcb *);
unsigned pbuf_copy_partial(const struct pbuf *, void *, unsigned, unsigned);
void pbuf_free(struct pbuf *);
struct tcp_pcb *tcp_new_ip_type(unsigned);
err_t tcp_connect(struct tcp_pcb *, const ip_addr_t *, uint16_t,
                  err_t (*)(void *, struct tcp_pcb *, err_t));
void tcp_recved(struct tcp_pcb *, u16_t);
unsigned tcp_sndbuf(struct tcp_pcb *);
err_t tcp_write(struct tcp_pcb *, const void *, u16_t, unsigned);
err_t tcp_output(struct tcp_pcb *);
char *ip4addr_ntoa_r(const ip4_addr_t *, char *, int);
#endif

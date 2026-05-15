#pragma once
#ifndef EMD_NET_H
#define EMD_NET_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Error codes */
#define EMD_NET_OK        0
#define EMD_NET_ERR      -1
#define EMD_NET_TIMEOUT  -2
#define EMD_NET_AGAIN    -3   /* EAGAIN / EWOULDBLOCK */
#define EMD_NET_EOF      -4

/* -------------------------------------------------------------------------
 * TCP
 * ---------------------------------------------------------------------- */

/*
 * Connect to host:port with a connect timeout (ms).
 * Returns a non-blocking fd on success, or < 0 on error.
 */
int emd_tcp_connect(const char *host, uint16_t port, uint32_t timeout_ms);

/*
 * Send all bytes in buf (blocking up to timeout_ms total).
 * Returns number of bytes sent, or < 0 on error.
 */
int emd_tcp_send_all(int fd, const uint8_t *buf, size_t len, uint32_t timeout_ms);

/*
 * Receive exactly len bytes (blocking up to timeout_ms total).
 * Returns number of bytes received, or < 0 on error / EOF.
 */
int emd_tcp_recv_exact(int fd, uint8_t *buf, size_t len, uint32_t timeout_ms);

/*
 * Receive up to len bytes, non-blocking.
 * Returns bytes received, 0 for EOF, EMD_NET_AGAIN if nothing available, < 0 on error.
 */
int emd_tcp_recv(int fd, uint8_t *buf, size_t len);

/* Close a socket fd. */
void emd_net_close(int fd);

/* -------------------------------------------------------------------------
 * UDP
 * ---------------------------------------------------------------------- */

/*
 * Create a UDP socket bound to local_port (0 = any).
 * Returns fd or < 0 on error.
 */
int emd_udp_socket(uint16_t local_port);

/*
 * Send a UDP datagram to host:port.
 * Returns bytes sent, or < 0 on error.
 */
int emd_udp_send(int fd, const char *host, uint16_t port,
                 const uint8_t *buf, size_t len);

/*
 * Receive a UDP datagram into buf.
 * timeout_ms == 0 → non-blocking.
 * Returns bytes received, EMD_NET_AGAIN if none, < 0 on error.
 */
int emd_udp_recv(int fd, uint8_t *buf, size_t bufsz, uint32_t timeout_ms,
                 char *src_host, size_t src_host_len, uint16_t *src_port);

/* -------------------------------------------------------------------------
 * epoll helper
 * ---------------------------------------------------------------------- */
typedef struct emd_epoll emd_epoll_t;

emd_epoll_t *emd_epoll_create(void);
void         emd_epoll_destroy(emd_epoll_t *ep);

int  emd_epoll_add(emd_epoll_t *ep, int fd, void *userdata, bool readable, bool writable);
int  emd_epoll_del(emd_epoll_t *ep, int fd);
int  emd_epoll_mod(emd_epoll_t *ep, int fd, void *userdata, bool readable, bool writable);

typedef struct {
    int   fd;
    void *userdata;
    bool  readable;
    bool  writable;
    bool  error;
} emd_epoll_event_t;

/* Wait for events.  Returns number of ready events (≥0) or <0 on error. */
int emd_epoll_wait(emd_epoll_t *ep, emd_epoll_event_t *events,
                   int max_events, int timeout_ms);

/* -------------------------------------------------------------------------
 * Address utilities
 * ---------------------------------------------------------------------- */
/* Parse "host:port" into host (NUL-terminated, up to host_len) and port. */
int emd_net_parse_hostport(const char *hostport,
                           char *host, size_t host_len,
                           uint16_t *port);

#ifdef __cplusplus
}
#endif

#endif /* EMD_NET_H */

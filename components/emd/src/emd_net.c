#include "emd/net.h"
#include "emd/log.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <netdb.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <poll.h>
#include <stdint.h>
#include <stdbool.h>

/* MSG_NOSIGNAL is Linux-specific; on macOS use SO_NOSIGPIPE instead */
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

/* -------------------------------------------------------------------------
 * Internal helpers
 * ---------------------------------------------------------------------- */
static int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static int set_blocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    return fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
}

static int wait_ready(int fd, bool for_write, uint32_t timeout_ms) {
    struct pollfd pfd = { .fd = fd, .events = for_write ? POLLOUT : POLLIN };
    int r = poll(&pfd, 1, (int)timeout_ms);
    if (r < 0) return EMD_NET_ERR;
    if (r == 0) return EMD_NET_TIMEOUT;
    if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) return EMD_NET_ERR;
    return EMD_NET_OK;
}

/* -------------------------------------------------------------------------
 * TCP
 * ---------------------------------------------------------------------- */
int emd_tcp_connect(const char *host, uint16_t port, uint32_t timeout_ms) {
    struct addrinfo hints, *res = NULL;
    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%u", (unsigned)port);
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    int gai = getaddrinfo(host, port_str, &hints, &res);
    if (gai != 0) {
        EMD_LOGE("net", gai_strerror(gai));
        return EMD_NET_ERR;
    }

    int fd = -1;
    for (struct addrinfo *rp = res; rp; rp = rp->ai_next) {
        fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd < 0) continue;

        /* Enable TCP_NODELAY */
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

        set_nonblocking(fd);

        int r = connect(fd, rp->ai_addr, rp->ai_addrlen);
        if (r == 0) break; /* immediate */
        if (errno != EINPROGRESS) {
            close(fd); fd = -1; continue;
        }

        /* Wait for connect */
        r = wait_ready(fd, true, timeout_ms);
        if (r != EMD_NET_OK) { close(fd); fd = -1; continue; }

        /* Check SO_ERROR */
        int err = 0; socklen_t elen = sizeof(err);
        getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &elen);
        if (err != 0) { close(fd); fd = -1; continue; }

        break;
    }

    freeaddrinfo(res);

    if (fd >= 0) {
        /* Connected successfully — make socket blocking with recv timeout */
        set_blocking(fd);

        /* Set SO_RCVTIMEO to 100ms to avoid busy-loop on recv */
        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 100000;  /* 100ms */
        if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) {
            EMD_LOGW("net", "failed to set SO_RCVTIMEO");
        }

        /* On macOS, also set SO_NOSIGPIPE to avoid SIGPIPE on send */
        #ifdef SO_NOSIGPIPE
        int nosigpipe = 1;
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &nosigpipe, sizeof(nosigpipe));
        #endif
    }

    return fd; /* -1 if all failed */
}

int emd_tcp_send_all(int fd, const uint8_t *buf, size_t len, uint32_t timeout_ms) {
    size_t sent = 0;
    while (sent < len) {
        ssize_t r = send(fd, buf + sent, len - sent, MSG_NOSIGNAL);
        if (r < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                int wr = wait_ready(fd, true, timeout_ms);
                if (wr != EMD_NET_OK) return wr;
                continue;
            }
            return EMD_NET_ERR;
        }
        sent += (size_t)r;
    }
    return (int)sent;
}

int emd_tcp_recv_exact(int fd, uint8_t *buf, size_t len, uint32_t timeout_ms) {
    size_t got = 0;
    while (got < len) {
        ssize_t r = recv(fd, buf + got, len - got, 0);
        if (r < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                int wr = wait_ready(fd, false, timeout_ms);
                if (wr != EMD_NET_OK) return wr;
                continue;
            }
            return EMD_NET_ERR;
        }
        if (r == 0) return EMD_NET_EOF;
        got += (size_t)r;
    }
    return (int)got;
}

int emd_tcp_recv(int fd, uint8_t *buf, size_t len) {
    ssize_t r = recv(fd, buf, len, 0);
    if (r < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return EMD_NET_AGAIN;
        return EMD_NET_ERR;
    }
    if (r == 0) return EMD_NET_EOF;
    return (int)r;
}

void emd_net_close(int fd) {
    if (fd >= 0) close(fd);
}

/* -------------------------------------------------------------------------
 * UDP
 * ---------------------------------------------------------------------- */
int emd_udp_socket(uint16_t local_port) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return EMD_NET_ERR;
    set_nonblocking(fd);
    if (local_port) {
        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port   = htons(local_port);
        addr.sin_addr.s_addr = INADDR_ANY;
        if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
            close(fd); return EMD_NET_ERR;
        }
    }
    return fd;
}

int emd_udp_send(int fd, const char *host, uint16_t port,
                 const uint8_t *buf, size_t len)
{
    struct addrinfo hints, *res = NULL;
    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%u", (unsigned)port);
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;

    if (getaddrinfo(host, port_str, &hints, &res) != 0) return EMD_NET_ERR;
    ssize_t r = sendto(fd, buf, len, 0, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);
    return r < 0 ? EMD_NET_ERR : (int)r;
}

int emd_udp_recv(int fd, uint8_t *buf, size_t bufsz, uint32_t timeout_ms,
                 char *src_host, size_t src_host_len, uint16_t *src_port)
{
    if (timeout_ms > 0) {
        int r = wait_ready(fd, false, timeout_ms);
        if (r != EMD_NET_OK) return r;
    }

    struct sockaddr_storage ss;
    socklen_t sslen = sizeof(ss);
    ssize_t r = recvfrom(fd, buf, bufsz, 0, (struct sockaddr *)&ss, &sslen);
    if (r < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return EMD_NET_AGAIN;
        return EMD_NET_ERR;
    }

    if (src_host && src_host_len) {
        getnameinfo((struct sockaddr *)&ss, sslen,
                    src_host, (socklen_t)src_host_len, NULL, 0, NI_NUMERICHOST);
    }
    if (src_port) {
        if (ss.ss_family == AF_INET)
            *src_port = ntohs(((struct sockaddr_in *)&ss)->sin_port);
        else if (ss.ss_family == AF_INET6)
            *src_port = ntohs(((struct sockaddr_in6 *)&ss)->sin6_port);
        else
            *src_port = 0;
    }
    return (int)r;
}

/* -------------------------------------------------------------------------
 * epoll-compatible wrapper (implemented with poll for portability)
 * ---------------------------------------------------------------------- */
#define EMD_EPOLL_MAX_FDS 256

typedef struct {
    int    fd;
    void  *userdata;
    bool   readable;
    bool   writable;
} epoll_entry_t;

struct emd_epoll {
    epoll_entry_t entries[EMD_EPOLL_MAX_FDS];
    int           nentries;
};

emd_epoll_t *emd_epoll_create(void) {
    emd_epoll_t *ep = calloc(1, sizeof(*ep));
    return ep;
}

void emd_epoll_destroy(emd_epoll_t *ep) {
    free(ep);
}

int emd_epoll_add(emd_epoll_t *ep, int fd, void *userdata,
                  bool readable, bool writable)
{
    if (!ep || ep->nentries >= EMD_EPOLL_MAX_FDS) return -1;
    epoll_entry_t *e = &ep->entries[ep->nentries++];
    e->fd       = fd;
    e->userdata = userdata;
    e->readable = readable;
    e->writable = writable;
    return 0;
}

int emd_epoll_del(emd_epoll_t *ep, int fd) {
    if (!ep) return -1;
    for (int i = 0; i < ep->nentries; i++) {
        if (ep->entries[i].fd == fd) {
            ep->entries[i] = ep->entries[--ep->nentries];
            return 0;
        }
    }
    return -1;
}

int emd_epoll_mod(emd_epoll_t *ep, int fd, void *userdata,
                  bool readable, bool writable)
{
    if (!ep) return -1;
    for (int i = 0; i < ep->nentries; i++) {
        if (ep->entries[i].fd == fd) {
            ep->entries[i].userdata = userdata;
            ep->entries[i].readable = readable;
            ep->entries[i].writable = writable;
            return 0;
        }
    }
    return -1;
}

int emd_epoll_wait(emd_epoll_t *ep, emd_epoll_event_t *events,
                   int max_events, int timeout_ms)
{
    if (!ep || !events || max_events <= 0) return 0;

    int n = ep->nentries;
    if (n == 0) {
        /* Nothing to watch — just sleep */
        if (timeout_ms > 0) {
            struct pollfd dummy;
            memset(&dummy, 0, sizeof(dummy));
            dummy.fd = -1;
            poll(&dummy, 0, timeout_ms);
        }
        return 0;
    }

    struct pollfd pfds[EMD_EPOLL_MAX_FDS];
    for (int i = 0; i < n; i++) {
        pfds[i].fd      = ep->entries[i].fd;
        pfds[i].events  = 0;
        pfds[i].revents = 0;
        if (ep->entries[i].readable) pfds[i].events |= POLLIN;
        if (ep->entries[i].writable) pfds[i].events |= POLLOUT;
    }

    int r = poll(pfds, (nfds_t)n, timeout_ms);
    if (r < 0) return EMD_NET_ERR;
    if (r == 0) return 0;

    int out = 0;
    for (int i = 0; i < n && out < max_events; i++) {
        if (pfds[i].revents == 0) continue;
        events[out].fd       = pfds[i].fd;
        events[out].userdata = ep->entries[i].userdata;
        events[out].readable = !!(pfds[i].revents & POLLIN);
        events[out].writable = !!(pfds[i].revents & POLLOUT);
        events[out].error    = !!(pfds[i].revents & (POLLERR | POLLHUP | POLLNVAL));
        out++;
    }
    return out;
}

/* -------------------------------------------------------------------------
 * Address parsing
 * ---------------------------------------------------------------------- */
int emd_net_parse_hostport(const char *hostport,
                            char *host, size_t host_len,
                            uint16_t *port)
{
    if (!hostport || !host || !port) return -1;

    const char *colon = strrchr(hostport, ':');
    if (!colon) return -1;

    size_t hlen = (size_t)(colon - hostport);
    if (hlen == 0 || hlen >= host_len) return -1;

    memcpy(host, hostport, hlen);
    host[hlen] = '\0';

    unsigned long p = strtoul(colon + 1, NULL, 10);
    if (p == 0 || p > 65535) return -1;
    *port = (uint16_t)p;
    return 0;
}

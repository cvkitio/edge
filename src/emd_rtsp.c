/*
 * emd_rtsp.c — RTSP client state machine (DESCRIBE/SETUP/PLAY/TEARDOWN).
 * Supports TCP interleaved (default) and basic/digest auth.
 * RFC 2326 / RFC 7826.
 */

#include "emd/rtsp.h"
#include "emd/net.h"
#include "emd/log.h"
#include "emd/config.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <ctype.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>

/* ---------------------------------------------------------------------- */
/* MD5 (RFC 2617 digest auth) — minimal self-contained impl               */
/* ---------------------------------------------------------------------- */

typedef struct { uint32_t s[4]; uint32_t count[2]; uint8_t buf[64]; } md5_ctx_t;

static void md5_init(md5_ctx_t *c);
static void md5_update(md5_ctx_t *c, const uint8_t *data, size_t len);
static void md5_final(uint8_t digest[16], md5_ctx_t *c);

#define MD5_F(x,y,z) ((x & y) | (~x & z))
#define MD5_G(x,y,z) ((x & z) | (y & ~z))
#define MD5_H(x,y,z) (x ^ y ^ z)
#define MD5_I(x,y,z) (y ^ (x | ~z))
#define MD5_RL(x,n)  (((x) << (n)) | ((x) >> (32 - (n))))
#define MD5_FF(a,b,c,d,x,s,ac) a = MD5_RL(a + MD5_F(b,c,d) + (x) + (uint32_t)(ac), s) + b
#define MD5_GG(a,b,c,d,x,s,ac) a = MD5_RL(a + MD5_G(b,c,d) + (x) + (uint32_t)(ac), s) + b
#define MD5_HH(a,b,c,d,x,s,ac) a = MD5_RL(a + MD5_H(b,c,d) + (x) + (uint32_t)(ac), s) + b
#define MD5_II(a,b,c,d,x,s,ac) a = MD5_RL(a + MD5_I(b,c,d) + (x) + (uint32_t)(ac), s) + b

static void md5_transform(uint32_t state[4], const uint8_t block[64]) {
    uint32_t a = state[0], b = state[1], c = state[2], d = state[3], x[16];
    for (int i = 0; i < 16; i++) {
        x[i] = ((uint32_t)block[i*4])       | ((uint32_t)block[i*4+1] << 8) |
               ((uint32_t)block[i*4+2] << 16) | ((uint32_t)block[i*4+3] << 24);
    }
    MD5_FF(a,b,c,d,x[0], 7,0xd76aa478); MD5_FF(d,a,b,c,x[1], 12,0xe8c7b756);
    MD5_FF(c,d,a,b,x[2], 17,0x242070db); MD5_FF(b,c,d,a,x[3], 22,0xc1bdceee);
    MD5_FF(a,b,c,d,x[4], 7,0xf57c0faf); MD5_FF(d,a,b,c,x[5], 12,0x4787c62a);
    MD5_FF(c,d,a,b,x[6], 17,0xa8304613); MD5_FF(b,c,d,a,x[7], 22,0xfd469501);
    MD5_FF(a,b,c,d,x[8], 7,0x698098d8); MD5_FF(d,a,b,c,x[9], 12,0x8b44f7af);
    MD5_FF(c,d,a,b,x[10],17,0xffff5bb1); MD5_FF(b,c,d,a,x[11],22,0x895cd7be);
    MD5_FF(a,b,c,d,x[12], 7,0x6b901122); MD5_FF(d,a,b,c,x[13],12,0xfd987193);
    MD5_FF(c,d,a,b,x[14],17,0xa679438e); MD5_FF(b,c,d,a,x[15],22,0x49b40821);
    MD5_GG(a,b,c,d,x[1], 5,0xf61e2562); MD5_GG(d,a,b,c,x[6], 9,0xc040b340);
    MD5_GG(c,d,a,b,x[11],14,0x265e5a51); MD5_GG(b,c,d,a,x[0], 20,0xe9b6c7aa);
    MD5_GG(a,b,c,d,x[5], 5,0xd62f105d); MD5_GG(d,a,b,c,x[10], 9,0x02441453);
    MD5_GG(c,d,a,b,x[15],14,0xd8a1e681); MD5_GG(b,c,d,a,x[4], 20,0xe7d3fbc8);
    MD5_GG(a,b,c,d,x[9], 5,0x21e1cde6); MD5_GG(d,a,b,c,x[14], 9,0xc33707d6);
    MD5_GG(c,d,a,b,x[3], 14,0xf4d50d87); MD5_GG(b,c,d,a,x[8], 20,0x455a14ed);
    MD5_GG(a,b,c,d,x[13], 5,0xa9e3e905); MD5_GG(d,a,b,c,x[2], 9,0xfcefa3f8);
    MD5_GG(c,d,a,b,x[7], 14,0x676f02d9); MD5_GG(b,c,d,a,x[12],20,0x8d2a4c8a);
    MD5_HH(a,b,c,d,x[5], 4,0xfffa3942); MD5_HH(d,a,b,c,x[8], 11,0x8771f681);
    MD5_HH(c,d,a,b,x[11],16,0x6d9d6122); MD5_HH(b,c,d,a,x[14],23,0xfde5380c);
    MD5_HH(a,b,c,d,x[1], 4,0xa4beea44); MD5_HH(d,a,b,c,x[4], 11,0x4bdecfa9);
    MD5_HH(c,d,a,b,x[7], 16,0xf6bb4b60); MD5_HH(b,c,d,a,x[10],23,0xbebfbc70);
    MD5_HH(a,b,c,d,x[13], 4,0x289b7ec6); MD5_HH(d,a,b,c,x[0], 11,0xeaa127fa);
    MD5_HH(c,d,a,b,x[3], 16,0xd4ef3085); MD5_HH(b,c,d,a,x[6], 23,0x04881d05);
    MD5_HH(a,b,c,d,x[9], 4,0xd9d4d039); MD5_HH(d,a,b,c,x[12],11,0xe6db99e5);
    MD5_HH(c,d,a,b,x[15],16,0x1fa27cf8); MD5_HH(b,c,d,a,x[2], 23,0xc4ac5665);
    MD5_II(a,b,c,d,x[0], 6,0xf4292244); MD5_II(d,a,b,c,x[7], 10,0x432aff97);
    MD5_II(c,d,a,b,x[14],15,0xab9423a7); MD5_II(b,c,d,a,x[5], 21,0xfc93a039);
    MD5_II(a,b,c,d,x[12], 6,0x655b59c3); MD5_II(d,a,b,c,x[3], 10,0x8f0ccc92);
    MD5_II(c,d,a,b,x[10],15,0xffeff47d); MD5_II(b,c,d,a,x[1], 21,0x85845dd1);
    MD5_II(a,b,c,d,x[8], 6,0x6fa87e4f); MD5_II(d,a,b,c,x[15],10,0xfe2ce6e0);
    MD5_II(c,d,a,b,x[6], 15,0xa3014314); MD5_II(b,c,d,a,x[13],21,0x4e0811a1);
    MD5_II(a,b,c,d,x[4], 6,0xf7537e82); MD5_II(d,a,b,c,x[11],10,0xbd3af235);
    MD5_II(c,d,a,b,x[2], 15,0x2ad7d2bb); MD5_II(b,c,d,a,x[9], 21,0xeb86d391);
    state[0] += a; state[1] += b; state[2] += c; state[3] += d;
}

static void md5_init(md5_ctx_t *c) {
    c->count[0] = c->count[1] = 0;
    c->s[0] = 0x67452301u; c->s[1] = 0xefcdab89u;
    c->s[2] = 0x98badcfeu; c->s[3] = 0x10325476u;
}

static void md5_update(md5_ctx_t *c, const uint8_t *data, size_t len) {
    uint32_t idx = (c->count[0] >> 3) & 0x3Fu;
    c->count[0] += (uint32_t)(len << 3);
    if (c->count[0] < (uint32_t)(len << 3)) c->count[1]++;
    c->count[1] += (uint32_t)(len >> 29);
    uint32_t part = 64u - idx;
    size_t i;
    if (len >= part) {
        memcpy(c->buf + idx, data, part);
        md5_transform(c->s, c->buf);
        for (i = part; i + 63 < len; i += 64)
            md5_transform(c->s, data + i);
        idx = 0;
    } else { i = 0; }
    memcpy(c->buf + idx, data + i, len - i);
}

static void md5_final(uint8_t digest[16], md5_ctx_t *c) {
    static const uint8_t PAD[64] = {0x80};
    uint8_t bits[8];
    for (int i = 0; i < 4; i++) {
        bits[i]   = (uint8_t)(c->count[0] >> (i * 8));
        bits[i+4] = (uint8_t)(c->count[1] >> (i * 8));
    }
    uint32_t idx = (c->count[0] >> 3) & 0x3Fu;
    uint32_t pad = (idx < 56u) ? (56u - idx) : (120u - idx);
    md5_update(c, PAD, pad);
    md5_update(c, bits, 8);
    for (int i = 0; i < 4; i++) {
        digest[i*4]   = (uint8_t)(c->s[i]);
        digest[i*4+1] = (uint8_t)(c->s[i] >> 8);
        digest[i*4+2] = (uint8_t)(c->s[i] >> 16);
        digest[i*4+3] = (uint8_t)(c->s[i] >> 24);
    }
}

static void md5_hex(const char *s, char out[33]) {
    md5_ctx_t c; md5_init(&c);
    md5_update(&c, (const uint8_t *)s, strlen(s));
    uint8_t dig[16]; md5_final(dig, &c);
    for (int i = 0; i < 16; i++) snprintf(out + i*2, 3, "%02x", dig[i]);
    out[32] = '\0';
}

/* ---------------------------------------------------------------------- */
/* RTSP client                                                              */
/* ---------------------------------------------------------------------- */

#define RTSP_BUF_SIZE   (64 * 1024)
#define RTSP_MAX_HDR    8192
#define RTSP_TIMEOUT_MS 5000

struct emd_rtsp_client {
    char             url[EMD_MAX_URL_LEN];
    char             host[256];
    uint16_t         port;
    char             path[512];
    bool             use_tcp;
    uint32_t         keepalive_ms;

    emd_rtsp_state_t state;
    emd_rtsp_codec_t codec;
    emd_rtsp_sdp_t   sdp;
    emd_rtsp_auth_t  auth;

    int              fd;
    int              cseq;
    char             session_id[128];
    uint32_t         session_timeout_secs;

    /* Read buffer for framing */
    uint8_t          rbuf[RTSP_BUF_SIZE];
    size_t           rbuf_pos;
    size_t           rbuf_len;

    /* RTP interleaved accumulator */
    uint8_t          rtp_buf[65536];
    size_t           rtp_need;
    bool             rtp_in_frame;
    uint8_t          rtp_channel;

    /* Callback */
    emd_rtsp_rtp_cb  rtp_cb;
    void            *userdata;

    /* Keepalive timer */
    uint64_t         last_keepalive_ns;

    /* Backoff */
    int              backoff_idx;
    uint64_t         reconnect_at_ns;
};

/* ---------------------------------------------------------------------- */
/* URL parsing                                                              */
/* ---------------------------------------------------------------------- */

/* Decode %XX percent-encoding in-place; dst may alias src */
static void url_decode(const char *src, char *dst, size_t dst_sz) {
    size_t out = 0;
    for (size_t i = 0; src[i] && out + 1 < dst_sz; i++) {
        if (src[i] == '%' && src[i+1] && src[i+2]) {
            char hex[3] = { src[i+1], src[i+2], '\0' };
            char *end;
            long val = strtol(hex, &end, 16);
            if (end == hex + 2) {
                dst[out++] = (char)val;
                i += 2;
                continue;
            }
        }
        dst[out++] = src[i];
    }
    dst[out] = '\0';
}

static int parse_rtsp_url(const char *url, char *host, size_t host_sz,
                           uint16_t *port, char *path, size_t path_sz,
                           char *user, size_t user_sz,
                           char *pass, size_t pass_sz)
{
    user[0] = '\0'; pass[0] = '\0';
    const char *p = url;
    if (strncmp(p, "rtsp://", 7) != 0) return -1;
    p += 7;

    /* user:pass@ — both fields may be %XX percent-encoded */
    const char *at = strchr(p, '@');
    if (at) {
        const char *colon = (const char *)memchr(p, ':', (size_t)(at - p));
        if (colon) {
            size_t ulen = (size_t)(colon - p);
            if (ulen >= user_sz) return -1;
            memcpy(user, p, ulen); user[ulen] = '\0';
            url_decode(user, user, user_sz);
            size_t plen = (size_t)(at - colon - 1);
            if (plen >= pass_sz) return -1;
            memcpy(pass, colon + 1, plen); pass[plen] = '\0';
            url_decode(pass, pass, pass_sz);
        } else {
            size_t ulen = (size_t)(at - p);
            if (ulen >= user_sz) return -1;
            memcpy(user, p, ulen); user[ulen] = '\0';
            url_decode(user, user, user_sz);
        }
        p = at + 1;
    }

    /* host:port */
    const char *slash = strchr(p, '/');
    const char *colon = strchr(p, ':');
    if (slash && colon && colon < slash) {
        size_t hlen = (size_t)(colon - p);
        if (hlen >= host_sz) return -1;
        memcpy(host, p, hlen); host[hlen] = '\0';
        char port_buf[8];
        size_t plen2 = slash ? (size_t)(slash - colon - 1) : strlen(colon + 1);
        if (plen2 >= sizeof(port_buf)) return -1;
        memcpy(port_buf, colon + 1, plen2); port_buf[plen2] = '\0';
        *port = (uint16_t)atoi(port_buf);
    } else {
        const char *end = slash ? slash : p + strlen(p);
        size_t hlen = (size_t)(end - p);
        if (hlen >= host_sz) return -1;
        memcpy(host, p, hlen); host[hlen] = '\0';
        *port = 554;
    }

    if (slash) {
        size_t plen3 = strlen(slash);
        if (plen3 >= path_sz) plen3 = path_sz - 1;
        memcpy(path, slash, plen3); path[plen3] = '\0';
    } else {
        path[0] = '/'; path[1] = '\0';
    }
    return 0;
}

/* ---------------------------------------------------------------------- */
/* Monotonic clock helper                                                   */
/* ---------------------------------------------------------------------- */
static uint64_t mono_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* ---------------------------------------------------------------------- */
/* Create / free                                                            */
/* ---------------------------------------------------------------------- */

emd_rtsp_client_t *emd_rtsp_client_new(const char *url,
                                        emd_rtsp_rtp_cb rtp_cb,
                                        void *userdata)
{
    emd_rtsp_client_t *c = calloc(1, sizeof(*c));
    if (!c) return NULL;

    strncpy(c->url, url, sizeof(c->url) - 1);
    char user[128], pass[128];
    if (parse_rtsp_url(url, c->host, sizeof(c->host),
                        &c->port, c->path, sizeof(c->path),
                        user, sizeof(user), pass, sizeof(pass)) < 0) {
        free(c);
        return NULL;
    }
    if (user[0]) {
        strncpy(c->auth.username, user, sizeof(c->auth.username) - 1);
        strncpy(c->auth.password, pass, sizeof(c->auth.password) - 1);
    }

    c->use_tcp        = true;
    c->keepalive_ms   = 30000;
    c->state          = RTSP_STATE_IDLE;
    c->fd             = -1;
    c->rtp_cb         = rtp_cb;
    c->userdata       = userdata;
    c->cseq           = 1;
    return c;
}

void emd_rtsp_client_free(emd_rtsp_client_t *c) {
    if (!c) return;
    if (c->fd >= 0) { close(c->fd); c->fd = -1; }
    free(c);
}

void emd_rtsp_set_transport(emd_rtsp_client_t *c, bool use_tcp) {
    if (c) c->use_tcp = use_tcp;
}

void emd_rtsp_set_keepalive_ms(emd_rtsp_client_t *c, uint32_t ms) {
    if (c) c->keepalive_ms = ms;
}

emd_rtsp_state_t emd_rtsp_get_state(const emd_rtsp_client_t *c) {
    return c ? c->state : RTSP_STATE_ERROR;
}

const emd_rtsp_sdp_t *emd_rtsp_get_sdp(const emd_rtsp_client_t *c) {
    return c ? &c->sdp : NULL;
}

int emd_rtsp_get_fd(const emd_rtsp_client_t *c) {
    return c ? c->fd : -1;
}

/* ---------------------------------------------------------------------- */
/* Request building                                                         */
/* ---------------------------------------------------------------------- */

static void build_auth_header(emd_rtsp_client_t *c, const char *method,
                               const char *uri, char *buf, size_t bufsz)
{
    if (!c->auth.required) { buf[0] = '\0'; return; }

    if (!c->auth.digest) {
        /* Basic auth */
        char creds[256];
        snprintf(creds, sizeof(creds), "%s:%s", c->auth.username, c->auth.password);
        /* Simple base64 */
        static const char B64[] =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        char b64[512];
        size_t clen = strlen(creds);
        size_t bi = 0;
        for (size_t i = 0; i < clen && bi + 4 < sizeof(b64); i += 3) {
            uint32_t v  = (uint32_t)((unsigned char)creds[i]) << 16;
            if (i+1 < clen) v |= (uint32_t)((unsigned char)creds[i+1]) << 8;
            if (i+2 < clen) v |= (uint32_t)((unsigned char)creds[i+2]);
            b64[bi++] = B64[(v >> 18) & 0x3F];
            b64[bi++] = B64[(v >> 12) & 0x3F];
            b64[bi++] = (i+1 < clen) ? B64[(v >> 6) & 0x3F] : '=';
            b64[bi++] = (i+2 < clen) ? B64[v & 0x3F]         : '=';
        }
        b64[bi] = '\0';
        snprintf(buf, bufsz, "Authorization: Basic %s\r\n", b64);
        return;
    }

    /* Digest auth */
    char ha1[33], ha2[33], ha_resp[33];
    char tmp[512];

    snprintf(tmp, sizeof(tmp), "%s:%s:%s",
             c->auth.username, c->auth.realm, c->auth.password);
    md5_hex(tmp, ha1);

    snprintf(tmp, sizeof(tmp), "%s:%s", method, uri);
    md5_hex(tmp, ha2);

    snprintf(tmp, sizeof(tmp), "%s:%s:%s", ha1, c->auth.nonce, ha2);
    md5_hex(tmp, ha_resp);

    snprintf(buf, bufsz,
             "Authorization: Digest username=\"%s\","
             " realm=\"%s\", nonce=\"%s\","
             " uri=\"%s\", response=\"%s\"\r\n",
             c->auth.username, c->auth.realm,
             c->auth.nonce, uri, ha_resp);
}

static int send_request(emd_rtsp_client_t *c, const char *method,
                         const char *uri, const char *extra_hdrs,
                         const char *body)
{
    char auth_hdr[512];
    build_auth_header(c, method, uri, auth_hdr, sizeof(auth_hdr));

    char req[4096];
    int n = snprintf(req, sizeof(req),
                     "%s %s RTSP/1.0\r\n"
                     "CSeq: %d\r\n"
                     "User-Agent: emd/0.1\r\n"
                     "%s"
                     "%s"
                     "%s"
                     "\r\n",
                     method, uri, c->cseq++,
                     auth_hdr,
                     extra_hdrs ? extra_hdrs : "",
                     body ? body : "");
    if (n <= 0 || (size_t)n >= sizeof(req)) return -1;
    return emd_tcp_send_all(c->fd, (const uint8_t *)req, (size_t)n, RTSP_TIMEOUT_MS);
}

/* ---------------------------------------------------------------------- */
/* Response parsing                                                         */
/* ---------------------------------------------------------------------- */

static int recv_response_line(emd_rtsp_client_t *c, char *line, size_t linesz) {
    /* Read byte-by-byte until \r\n */
    size_t pos = 0;
    while (pos + 1 < linesz) {
        uint8_t b;
        int r = emd_tcp_recv_exact(c->fd, &b, 1, RTSP_TIMEOUT_MS);
        if (r <= 0) return -1;
        if (b == '\r') {
            uint8_t b2;
            emd_tcp_recv_exact(c->fd, &b2, 1, RTSP_TIMEOUT_MS);
            break;
        }
        line[pos++] = (char)b;
    }
    line[pos] = '\0';
    return (int)pos;
}

static int recv_response(emd_rtsp_client_t *c, int *status_code,
                          char *hdrs, size_t hdr_sz,
                          char *body, size_t body_sz)
{
    /* Status line */
    char status_line[256];
    if (recv_response_line(c, status_line, sizeof(status_line)) < 0) return -1;

    /* RTSP/1.0 200 OK */
    if (sscanf(status_line, "RTSP/%*s %d", status_code) != 1) return -1;

    /* Headers */
    size_t hpos = 0;
    int content_length = 0;
    for (;;) {
        char line[1024];
        int r = recv_response_line(c, line, sizeof(line));
        if (r < 0) return -1;
        if (r == 0) break; /* empty line = end of headers */
        if (hpos + (size_t)r + 2 < hdr_sz) {
            memcpy(hdrs + hpos, line, (size_t)r);
            hpos += (size_t)r;
            hdrs[hpos++] = '\n';
        }

        /* Parse auth challenge */
        if (strncasecmp(line, "WWW-Authenticate:", 17) == 0) {
            const char *val = line + 17;
            while (*val == ' ') val++;
            if (strncasecmp(val, "Digest", 6) == 0) {
                c->auth.digest = true;
                /* Extract realm and nonce */
                const char *r2 = strstr(val, "realm=\"");
                if (r2) {
                    r2 += 7;
                    const char *end = strchr(r2, '"');
                    if (end) {
                        size_t len = (size_t)(end - r2);
                        if (len < sizeof(c->auth.realm)) {
                            memcpy(c->auth.realm, r2, len);
                            c->auth.realm[len] = '\0';
                        }
                    }
                }
                const char *n2 = strstr(val, "nonce=\"");
                if (n2) {
                    n2 += 7;
                    const char *end = strchr(n2, '"');
                    if (end) {
                        size_t len = (size_t)(end - n2);
                        if (len < sizeof(c->auth.nonce)) {
                            memcpy(c->auth.nonce, n2, len);
                            c->auth.nonce[len] = '\0';
                        }
                    }
                }
                c->auth.required = true;
            } else if (strncasecmp(val, "Basic", 5) == 0) {
                c->auth.digest = false;
                c->auth.required = true;
            }
        }

        /* Session ID and timeout */
        if (strncasecmp(line, "Session:", 8) == 0) {
            const char *val = line + 8;
            while (*val == ' ') val++;
            const char *semi = strchr(val, ';');
            size_t sid_len = semi ? (size_t)(semi - val) : strlen(val);
            if (sid_len >= sizeof(c->session_id)) sid_len = sizeof(c->session_id) - 1;
            memcpy(c->session_id, val, sid_len);
            c->session_id[sid_len] = '\0';
            if (semi) {
                const char *tout = strstr(semi, "timeout=");
                if (tout) c->session_timeout_secs = (uint32_t)atoi(tout + 8);
            }
        }

        if (strncasecmp(line, "Content-Length:", 15) == 0)
            content_length = atoi(line + 15);
    }

    if (hpos < hdr_sz) hdrs[hpos] = '\0';

    /* Body */
    if (content_length > 0 && body && body_sz > 0) {
        size_t to_read = (size_t)content_length;
        if (to_read >= body_sz) to_read = body_sz - 1;
        int r = emd_tcp_recv_exact(c->fd, (uint8_t *)body, to_read, RTSP_TIMEOUT_MS);
        if (r < 0) return -1;
        body[to_read] = '\0';
    } else if (body && body_sz > 0) {
        body[0] = '\0';
    }

    return 0;
}

/* ---------------------------------------------------------------------- */
/* SDP parsing                                                              */
/* ---------------------------------------------------------------------- */

static void parse_sdp(emd_rtsp_client_t *c, const char *sdp_body) {
    memset(&c->sdp, 0, sizeof(c->sdp));
    c->sdp.clock_rate = 90000;

    /* Walk lines */
    const char *p = sdp_body;
    while (p && *p) {
        const char *eol = strchr(p, '\n');
        size_t llen = eol ? (size_t)(eol - p) : strlen(p);

        char line[512];
        if (llen >= sizeof(line)) llen = sizeof(line) - 1;
        memcpy(line, p, llen);
        /* Strip trailing \r */
        while (llen > 0 && (line[llen-1] == '\r' || line[llen-1] == ' '))
            llen--;
        line[llen] = '\0';

        /* a=rtpmap */
        if (strncmp(line, "a=rtpmap:", 9) == 0) {
            const char *rest = line + 9;
            int pt; char enc[64]; int clock;
            if (sscanf(rest, "%d %63[^/]/%d", &pt, enc, &clock) >= 2) {
                if (strcasecmp(enc, "H264") == 0) {
                    c->sdp.codec       = RTSP_CODEC_H264;
                    c->sdp.payload_type = (uint8_t)pt;
                    c->sdp.clock_rate  = (uint32_t)clock;
                } else if (strcasecmp(enc, "H265") == 0 ||
                           strcasecmp(enc, "HEVC") == 0) {
                    c->sdp.codec       = RTSP_CODEC_H265;
                    c->sdp.payload_type = (uint8_t)pt;
                    c->sdp.clock_rate  = (uint32_t)clock;
                }
            }
        }

        /* a=control */
        if (strncmp(line, "a=control:", 10) == 0) {
            strncpy(c->sdp.control_url, line + 10, sizeof(c->sdp.control_url) - 1);
        }

        /* a=fmtp */
        if (strncmp(line, "a=fmtp:", 7) == 0) {
            const char *fmtp = line + 7;
            /* sprop-parameter-sets for H.264 */
            const char *sps = strstr(fmtp, "sprop-parameter-sets=");
            if (sps) {
                sps += 21;
                strncpy(c->sdp.sprop_parameter_sets, sps,
                        sizeof(c->sdp.sprop_parameter_sets) - 1);
            }
            /* H.265 */
            const char *svps = strstr(fmtp, "sprop-vps=");
            if (svps) {
                svps += 10;
                const char *semi = strchr(svps, ';');
                size_t vlen = semi ? (size_t)(semi - svps) : strlen(svps);
                if (vlen >= sizeof(c->sdp.sprop_vps)) vlen = sizeof(c->sdp.sprop_vps) - 1;
                memcpy(c->sdp.sprop_vps, svps, vlen);
            }
        }

        p = eol ? eol + 1 : NULL;
    }
}

/* ---------------------------------------------------------------------- */
/* Interleaved RTP dispatch                                                 */
/* ---------------------------------------------------------------------- */

static void dispatch_rtp_data(emd_rtsp_client_t *c,
                               uint8_t channel, const uint8_t *data, uint16_t len)
{
    if (c->rtp_cb)
        c->rtp_cb(channel, data, len, c->userdata);
}

/* Read and process interleaved data from TCP.  Returns -1 on socket error. */
static int process_interleaved(emd_rtsp_client_t *c) {
    /* Format: $ channel(1) length(2) data... */
    static int dbg_frames = 0;
    for (;;) {
        uint8_t hdr[4];
        int r = emd_tcp_recv(c->fd, hdr, 1);
        if (r == EMD_NET_AGAIN) return 0;
        if (r == 0) { fprintf(stderr, "[DBG interleaved] EOF fd=%d frames=%d\n", c->fd, dbg_frames); return -1; }
        if (r < 0) { fprintf(stderr, "[DBG interleaved] ERR fd=%d frames=%d\n", c->fd, dbg_frames); return -1; }

        int fc = dbg_frames++;
        if (fc < 30 || fc % 100 == 0)
            fprintf(stderr, "[DBG interleaved] byte=0x%02X frame#%d fd=%d\n", hdr[0], fc, c->fd);

        if (hdr[0] == '$') {
            /* Interleaved frame */
            if (emd_tcp_recv_exact(c->fd, hdr + 1, 3, RTSP_TIMEOUT_MS) < 0) return -1;
            uint16_t dlen = (uint16_t)(((uint16_t)hdr[2] << 8) | hdr[3]);
            if (dlen > 65535u) return -1;
            uint8_t *buf = malloc(dlen);
            if (!buf) return -1;
            int rb = emd_tcp_recv_exact(c->fd, buf, dlen, RTSP_TIMEOUT_MS);
            if (rb < 0) { free(buf); return -1; }
            dispatch_rtp_data(c, hdr[1], buf, dlen);
            free(buf);
        } else if (hdr[0] == 'R') {
            /* Likely RTSP response to GET_PARAMETER keepalive — drain */
            char line[256];
            recv_response_line(c, line, sizeof(line));
            /* Discard */
        } else {
            /* Unknown — skip byte */
        }
    }
}

/* ---------------------------------------------------------------------- */
/* State machine                                                            */
/* ---------------------------------------------------------------------- */

static int do_connect(emd_rtsp_client_t *c) {
    c->fd = emd_tcp_connect(c->host, c->port, RTSP_TIMEOUT_MS);
    if (c->fd < 0) {
        EMD_LOGE("rtsp", "TCP connect failed");
        return -1;
    }
    c->state = RTSP_STATE_OPTIONS;
    c->rbuf_pos = 0; c->rbuf_len = 0;
    c->session_id[0] = '\0';
    c->last_keepalive_ns = mono_ns();
    return 0;
}

static int do_options(emd_rtsp_client_t *c) {
    char hdrs[RTSP_MAX_HDR], body[1024];
    int status = 0;
    if (send_request(c, "OPTIONS", c->url, NULL, NULL) < 0) return -1;
    if (recv_response(c, &status, hdrs, sizeof(hdrs), body, sizeof(body)) < 0) return -1;
    if (status == 401) {
        /* Retry with auth */
        if (send_request(c, "OPTIONS", c->url, NULL, NULL) < 0) return -1;
        if (recv_response(c, &status, hdrs, sizeof(hdrs), body, sizeof(body)) < 0) return -1;
    }
    if (status != 200) { EMD_LOGE("rtsp", "OPTIONS failed"); return -1; }
    c->state = RTSP_STATE_DESCRIBE;
    return 0;
}

static int do_describe(emd_rtsp_client_t *c) {
    char hdrs[RTSP_MAX_HDR], body[8192];
    int status = 0;
    if (send_request(c, "DESCRIBE", c->url,
                     "Accept: application/sdp\r\n", NULL) < 0) return -1;
    if (recv_response(c, &status, hdrs, sizeof(hdrs), body, sizeof(body)) < 0) return -1;
    if (status == 401) {
        if (send_request(c, "DESCRIBE", c->url,
                         "Accept: application/sdp\r\n", NULL) < 0) return -1;
        if (recv_response(c, &status, hdrs, sizeof(hdrs), body, sizeof(body)) < 0) return -1;
    }
    if (status != 200) { EMD_LOGE("rtsp", "DESCRIBE failed"); return -1; }
    parse_sdp(c, body);
    c->state = RTSP_STATE_SETUP;
    return 0;
}

static int do_setup(emd_rtsp_client_t *c) {
    char track_url[768];
    /* Build track URL */
    const char *ctrl = c->sdp.control_url;
    if (ctrl[0] == '\0' || strcmp(ctrl, "*") == 0) {
        snprintf(track_url, sizeof(track_url), "%s/trackID=1", c->url);
    } else if (strncmp(ctrl, "rtsp://", 7) == 0) {
        strncpy(track_url, ctrl, sizeof(track_url) - 1);
    } else {
        snprintf(track_url, sizeof(track_url), "%s/%s", c->url, ctrl);
    }

    char transport_hdr[256];
    if (c->use_tcp) {
        snprintf(transport_hdr, sizeof(transport_hdr),
                 "Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n");
    } else {
        snprintf(transport_hdr, sizeof(transport_hdr),
                 "Transport: RTP/AVP;unicast;client_port=5004-5005\r\n");
    }

    char hdrs[RTSP_MAX_HDR], body[1024];
    int status = 0;
    if (send_request(c, "SETUP", track_url, transport_hdr, NULL) < 0) return -1;
    if (recv_response(c, &status, hdrs, sizeof(hdrs), body, sizeof(body)) < 0) return -1;
    if (status == 401) {
        if (send_request(c, "SETUP", track_url, transport_hdr, NULL) < 0) return -1;
        if (recv_response(c, &status, hdrs, sizeof(hdrs), body, sizeof(body)) < 0) return -1;
    }
    if (status != 200) { EMD_LOGE("rtsp", "SETUP failed"); return -1; }

    if (c->session_timeout_secs > 0)
        c->keepalive_ms = (c->session_timeout_secs * 1000u) / 2u;

    c->state = RTSP_STATE_PLAY;
    return 0;
}

static int do_play(emd_rtsp_client_t *c) {
    char sess_hdr[256];
    snprintf(sess_hdr, sizeof(sess_hdr),
             "Session: %s\r\nRange: npt=0.000-\r\n", c->session_id);
    char hdrs[RTSP_MAX_HDR], body[1024];
    int status = 0;
    if (send_request(c, "PLAY", c->url, sess_hdr, NULL) < 0) return -1;
    if (recv_response(c, &status, hdrs, sizeof(hdrs), body, sizeof(body)) < 0) return -1;
    if (status != 200) { EMD_LOGE("rtsp", "PLAY failed"); return -1; }
    c->state = RTSP_STATE_PLAYING;
    return 0;
}

static int do_keepalive(emd_rtsp_client_t *c) {
    char sess_hdr[256];
    snprintf(sess_hdr, sizeof(sess_hdr), "Session: %s\r\n", c->session_id);
    /* GET_PARAMETER with empty body for keepalive */
    if (send_request(c, "GET_PARAMETER", c->url, sess_hdr, NULL) < 0) return -1;
    /* Response will be handled in interleaved processing */
    c->last_keepalive_ns = mono_ns();
    return 0;
}

int emd_rtsp_tick(emd_rtsp_client_t *c) {
    if (!c) return -1;

    switch (c->state) {
    case RTSP_STATE_IDLE:
    case RTSP_STATE_RECONNECTING: {
        uint64_t now = mono_ns();
        if (c->reconnect_at_ns > 0 && now < c->reconnect_at_ns) return 0;
        if (do_connect(c) < 0) {
            if (c->backoff_idx < RTSP_BACKOFF_COUNT - 1) c->backoff_idx++;
            c->reconnect_at_ns = now + (uint64_t)RTSP_BACKOFF_SECS[c->backoff_idx] * 1000000000ULL;
            c->state = RTSP_STATE_RECONNECTING;
            return -1;
        }
        c->backoff_idx = 0;
        return 0;
    }

    case RTSP_STATE_OPTIONS:
        if (do_options(c) < 0) { c->state = RTSP_STATE_ERROR; return -1; }
        return 0;

    case RTSP_STATE_DESCRIBE:
        if (do_describe(c) < 0) { c->state = RTSP_STATE_ERROR; return -1; }
        return 0;

    case RTSP_STATE_SETUP:
        if (do_setup(c) < 0) { c->state = RTSP_STATE_ERROR; return -1; }
        return 0;

    case RTSP_STATE_PLAY:
        if (do_play(c) < 0) { c->state = RTSP_STATE_ERROR; return -1; }
        return 0;

    case RTSP_STATE_PLAYING: {
        /* Send keepalive if needed */
        uint64_t now = mono_ns();
        if (now - c->last_keepalive_ns >= (uint64_t)c->keepalive_ms * 1000000ULL) {
            do_keepalive(c);
        }
        /* Process incoming interleaved data */
        if (process_interleaved(c) < 0) {
            EMD_LOGE("rtsp", "socket error, reconnecting");
            close(c->fd); c->fd = -1;
            c->state = RTSP_STATE_RECONNECTING;
            c->reconnect_at_ns = mono_ns() + (uint64_t)RTSP_BACKOFF_SECS[0] * 1000000000ULL;
            return -1;
        }
        return 0;
    }

    case RTSP_STATE_ERROR:
        close(c->fd); c->fd = -1;
        c->state = RTSP_STATE_RECONNECTING;
        c->reconnect_at_ns = mono_ns() + (uint64_t)RTSP_BACKOFF_SECS[c->backoff_idx] * 1000000000ULL;
        return -1;

    case RTSP_STATE_TEARDOWN:
    case RTSP_STATE_PAUSE:
    case RTSP_STATE_CONNECTING:
        return 0;
    }
    return 0;
}

void emd_rtsp_teardown(emd_rtsp_client_t *c) {
    if (!c || c->fd < 0) return;
    char sess_hdr[256];
    snprintf(sess_hdr, sizeof(sess_hdr), "Session: %s\r\n", c->session_id);
    send_request(c, "TEARDOWN", c->url, sess_hdr, NULL);
    close(c->fd);
    c->fd = -1;
    c->state = RTSP_STATE_IDLE;
}

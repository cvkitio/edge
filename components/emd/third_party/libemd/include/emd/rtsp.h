#pragma once
#ifndef EMD_RTSP_H
#define EMD_RTSP_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* RTSP states */
typedef enum {
    RTSP_STATE_IDLE = 0,
    RTSP_STATE_CONNECTING,
    RTSP_STATE_OPTIONS,
    RTSP_STATE_DESCRIBE,
    RTSP_STATE_SETUP,
    RTSP_STATE_PLAY,
    RTSP_STATE_PLAYING,
    RTSP_STATE_PAUSE,
    RTSP_STATE_TEARDOWN,
    RTSP_STATE_ERROR,
    RTSP_STATE_RECONNECTING,
} emd_rtsp_state_t;

/* Media codec advertised by the server */
typedef enum {
    RTSP_CODEC_UNKNOWN = 0,
    RTSP_CODEC_H264,
    RTSP_CODEC_H265,
} emd_rtsp_codec_t;

/* SDP media info extracted during DESCRIBE */
typedef struct {
    emd_rtsp_codec_t codec;
    uint8_t          payload_type;
    uint32_t         clock_rate;
    char             control_url[256]; /* track URL */
    /* SDP fmtp params */
    char             sprop_parameter_sets[512]; /* H.264 */
    char             sprop_vps[256];            /* H.265 */
    char             sprop_sps[256];
    char             sprop_pps[256];
    uint32_t         sprop_max_don_diff;        /* H.265 DONL */
} emd_rtsp_sdp_t;

/* Auth state */
typedef struct {
    bool   required;
    bool   digest;
    char   realm[128];
    char   nonce[128];
    char   opaque[128];
    char   username[128];
    char   password[128];
} emd_rtsp_auth_t;

/* Backoff levels (seconds) */
#define RTSP_BACKOFF_COUNT 6
static const uint32_t RTSP_BACKOFF_SECS[RTSP_BACKOFF_COUNT] = {1,2,4,8,16,30};

/*
 * Callback invoked for each interleaved RTP/RTCP channel packet.
 *   channel: 0/1 for RTP/RTCP on first track (and 2/3 for second, etc.)
 */
typedef void (*emd_rtsp_rtp_cb)(uint8_t channel, const uint8_t *data,
                                 uint16_t len, void *userdata);

/* RTSP client handle */
typedef struct emd_rtsp_client emd_rtsp_client_t;

/*
 * Create a new RTSP client.
 * url  – full RTSP URL (rtsp://[user:pass@]host:port/path)
 * rtp_cb   – called for every received RTP/RTCP packet
 * userdata – passed through to callbacks
 */
emd_rtsp_client_t *emd_rtsp_client_new(const char *url,
                                        emd_rtsp_rtp_cb rtp_cb,
                                        void *userdata);

void emd_rtsp_client_free(emd_rtsp_client_t *c);

/*
 * Drive the RTSP state machine.  Call repeatedly from the camera worker loop.
 * Returns 0 if the session is PLAYING or transitioning,
 *        -1 if a fatal error occurred (should reconnect or exit).
 */
int emd_rtsp_tick(emd_rtsp_client_t *c);

emd_rtsp_state_t emd_rtsp_get_state(const emd_rtsp_client_t *c);
const emd_rtsp_sdp_t *emd_rtsp_get_sdp(const emd_rtsp_client_t *c);

/* Force teardown (e.g., on config reload) */
void emd_rtsp_teardown(emd_rtsp_client_t *c);

/* Get the underlying fd (for epoll integration).  -1 if not connected. */
int emd_rtsp_get_fd(const emd_rtsp_client_t *c);

/* Configure TCP interleaved (default) or UDP */
void emd_rtsp_set_transport(emd_rtsp_client_t *c, bool use_tcp);

/* Set session keepalive interval (ms).  Default: session_timeout/2. */
void emd_rtsp_set_keepalive_ms(emd_rtsp_client_t *c, uint32_t ms);

#ifdef __cplusplus
}
#endif

#endif /* EMD_RTSP_H */

#pragma once
#ifndef EMD_RTP_H
#define EMD_RTP_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* RTP header constants */
#define RTP_VERSION         2
#define RTP_HEADER_MIN_LEN  12

/* RTP fixed header (big-endian on wire) */
typedef struct {
    uint8_t  version_p_x_cc; /* V=2, P, X, CC */
    uint8_t  m_pt;            /* M, PT          */
    uint16_t seq;
    uint32_t timestamp;
    uint32_t ssrc;
} emd_rtp_header_t;

/* Parsed RTP packet */
typedef struct {
    uint8_t   version;
    bool      padding;
    bool      extension;
    uint8_t   csrc_count;
    bool      marker;
    uint8_t   payload_type;
    uint16_t  seq;
    uint32_t  timestamp;
    uint32_t  ssrc;
    const uint8_t *payload;
    size_t    payload_len;
} emd_rtp_pkt_t;

/*
 * Parse a raw RTP packet from buf (len bytes).
 * Returns 0 on success, -1 if packet is malformed.
 */
int emd_rtp_parse(const uint8_t *buf, size_t len, emd_rtp_pkt_t *pkt);

/* -------------------------------------------------------------------------
 * Reorder / jitter buffer (SPSC-friendly, size = power-of-two slots)
 * Reorder window: 32 packets.
 * ---------------------------------------------------------------------- */
#define EMD_RTP_REORDER_WIN 32

typedef struct {
    uint64_t  slots_filled;        /* bitmask of which slots have packets */
    uint16_t  next_seq;            /* next seq we expect to deliver */
    bool      initialized;

    /* Slot storage */
    uint8_t   buf[EMD_RTP_REORDER_WIN][2048];
    size_t    lens[EMD_RTP_REORDER_WIN];
    uint16_t  seqs[EMD_RTP_REORDER_WIN];

    /* Loss tracking */
    uint64_t  loss_count;
    uint64_t  received_count;
} emd_rtp_reorder_t;

/* Initialise a reorder buffer. */
void emd_rtp_reorder_init(emd_rtp_reorder_t *r);

typedef void (*emd_rtp_deliver_fn)(const emd_rtp_pkt_t *pkt, void *userdata);

/*
 * Feed a raw RTP packet into the reorder buffer.
 * When the next in-order packet(s) are ready, deliver_fn is called for each.
 * Returns number of packets delivered (≥0), or -1 on error.
 */
int emd_rtp_reorder_feed(emd_rtp_reorder_t *r,
                          const uint8_t *raw, size_t len,
                          emd_rtp_deliver_fn deliver_fn, void *userdata);

/*
 * Flush all held packets (call when a NAL is known lost or stream ends).
 */
void emd_rtp_reorder_flush(emd_rtp_reorder_t *r,
                            emd_rtp_deliver_fn deliver_fn, void *userdata);

/* -------------------------------------------------------------------------
 * RTCP
 * ---------------------------------------------------------------------- */
#define RTCP_PT_SR   200
#define RTCP_PT_RR   201
#define RTCP_PT_SDES 202
#define RTCP_PT_BYE  203

typedef struct {
    uint32_t ntp_sec;
    uint32_t ntp_frac;
    uint32_t rtp_ts;
    uint32_t pkt_count;
    uint32_t byte_count;
} emd_rtcp_sr_t;

int emd_rtcp_parse_sr(const uint8_t *buf, size_t len, emd_rtcp_sr_t *sr);

/* Build an RTCP RR packet into buf (must be ≥32 bytes).  Returns bytes written. */
int emd_rtcp_build_rr(uint8_t *buf, size_t bufsz,
                      uint32_t ssrc_self, uint32_t ssrc_sender,
                      uint8_t fraction_lost, uint32_t cumulative_lost,
                      uint32_t highest_seq, uint32_t jitter,
                      uint32_t last_sr, uint32_t delay_since_sr);

#ifdef __cplusplus
}
#endif

#endif /* EMD_RTP_H */

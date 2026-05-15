#pragma once
#ifndef EMD_H264_DEPAY_H
#define EMD_H264_DEPAY_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "emd/rtp.h"

#ifdef __cplusplus
extern "C" {
#endif

/* RFC 6184 RTP payload types for H.264:
 *  1..23  – single NAL
 *  24     – STAP-A
 *  25     – STAP-B (not implemented)
 *  26     – MTAP16 (not implemented)
 *  27     – MTAP24 (not implemented)
 *  28     – FU-A
 *  29     – FU-B  (not implemented)
 */

#define H264_RTP_STAP_A  24u
#define H264_RTP_STAP_B  25u
#define H264_RTP_MTAP16  26u
#define H264_RTP_MTAP24  27u
#define H264_RTP_FU_A    28u
#define H264_RTP_FU_B    29u

/* FU indicator bits */
#define H264_FU_S_BIT 0x80u
#define H264_FU_E_BIT 0x40u
#define H264_FU_R_BIT 0x20u

/* Maximum reassembly buffer size (10 MB) */
#define EMD_H264_REASSEMBLY_MAX (10u * 1024u * 1024u)

/* Callback: called when a complete NAL unit is ready.
 * nal     – pointer to the NAL data (NOT including start code)
 * len     – NAL length in bytes
 * marker  – true if RTP marker bit was set (last NAL of an access unit)
 * pts     – RTP timestamp (32-bit, caller converts to 90 kHz)
 * userdata – opaque
 */
typedef void (*emd_h264_nal_cb)(const uint8_t *nal, size_t len,
                                 bool marker, uint32_t pts,
                                 void *userdata);

/* Counter struct for statistics */
typedef struct {
    uint64_t single_nal;
    uint64_t stap_a;
    uint64_t fu_a;
    uint64_t unsupported;
    uint64_t reassembly_errors;
} emd_h264_depay_stats_t;

/* Depacketizer state */
typedef struct {
    /* FU-A reassembly buffer */
    uint8_t    *fu_buf;
    size_t      fu_len;
    size_t      fu_cap;
    bool        fu_started;
    bool        fu_lost;       /* a packet was dropped mid-fragment */
    uint16_t    fu_start_seq;
    uint32_t    fu_pts;

    emd_h264_nal_cb  nal_cb;
    void            *userdata;

    emd_h264_depay_stats_t stats;
} emd_h264_depay_t;

/* Initialise the depacketizer.  nal_cb is called for each complete NAL. */
int emd_h264_depay_init(emd_h264_depay_t *d,
                         emd_h264_nal_cb nal_cb, void *userdata);

/* Free resources. */
void emd_h264_depay_free(emd_h264_depay_t *d);

/* Reset state (e.g., after packet loss or reconnect). */
void emd_h264_depay_reset(emd_h264_depay_t *d);

/*
 * Feed one RTP packet.  Calls nal_cb for each complete NAL unit produced.
 * Returns 0 on success, -1 on fatal error.
 */
int emd_h264_depay_feed(emd_h264_depay_t *d, const emd_rtp_pkt_t *pkt);

/*
 * Notify the depacketizer that a sequence number was lost.
 * This will abort any in-progress FU-A reassembly.
 */
void emd_h264_depay_lost(emd_h264_depay_t *d, uint16_t lost_seq);

#ifdef __cplusplus
}
#endif

#endif /* EMD_H264_DEPAY_H */

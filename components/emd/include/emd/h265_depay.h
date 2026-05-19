#pragma once
#ifndef EMD_H265_DEPAY_H
#define EMD_H265_DEPAY_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "emd/rtp.h"

#ifdef __cplusplus
extern "C" {
#endif

/* RFC 7798 RTP payload types for H.265:
 *  nal_unit_type 48 – AP  (Aggregation Packet)
 *  nal_unit_type 49 – FU  (Fragmentation Unit)
 *  nal_unit_type 50 – PACI (not implemented)
 */
#define H265_RTP_AP   48u
#define H265_RTP_FU   49u
#define H265_RTP_PACI 50u

#define H265_FU_S_BIT 0x80u
#define H265_FU_E_BIT 0x40u

#define EMD_H265_REASSEMBLY_MAX (10u * 1024u * 1024u)

typedef void (*emd_h265_nal_cb)(const uint8_t *nal, size_t len,
                                 bool marker, uint32_t pts,
                                 void *userdata);

typedef struct {
    uint64_t single_nal;
    uint64_t ap;
    uint64_t fu;
    uint64_t unsupported;
    uint64_t reassembly_errors;
    uint64_t donl_reorders;
} emd_h265_depay_stats_t;

typedef struct {
    uint8_t   *fu_buf;
    size_t     fu_len;
    size_t     fu_cap;
    bool       fu_started;
    bool       fu_lost;
    uint16_t   fu_start_seq;
    uint32_t   fu_pts;
    uint8_t    fu_nal_type; /* reconstructed NAL type from FU header */

    bool       donl_enabled;  /* sprop-max-don-diff > 0 */

    emd_h265_nal_cb  nal_cb;
    void            *userdata;

    emd_h265_depay_stats_t stats;
} emd_h265_depay_t;

int  emd_h265_depay_init(emd_h265_depay_t *d, emd_h265_nal_cb nal_cb,
                          void *userdata, bool donl_enabled);
void emd_h265_depay_free(emd_h265_depay_t *d);
void emd_h265_depay_reset(emd_h265_depay_t *d);
int  emd_h265_depay_feed(emd_h265_depay_t *d, const emd_rtp_pkt_t *pkt);
void emd_h265_depay_lost(emd_h265_depay_t *d, uint16_t lost_seq);

#ifdef __cplusplus
}
#endif

#endif /* EMD_H265_DEPAY_H */

#include "emd/h264_depay.h"
#include "emd/log.h"

#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* -------------------------------------------------------------------------
 * Init / free
 * ---------------------------------------------------------------------- */
int emd_h264_depay_init(emd_h264_depay_t *d,
                         emd_h264_nal_cb nal_cb, void *userdata)
{
    if (!d || !nal_cb) return -1;
    memset(d, 0, sizeof(*d));
    d->nal_cb   = nal_cb;
    d->userdata = userdata;

    d->fu_cap = 65536;
    d->fu_buf = malloc(d->fu_cap);
    if (!d->fu_buf) return -1;
    return 0;
}

void emd_h264_depay_free(emd_h264_depay_t *d) {
    if (!d) return;
    free(d->fu_buf);
    d->fu_buf = NULL;
}

void emd_h264_depay_reset(emd_h264_depay_t *d) {
    if (!d) return;
    d->fu_started = false;
    d->fu_lost    = false;
    d->fu_len     = 0;
}

void emd_h264_depay_lost(emd_h264_depay_t *d, uint16_t lost_seq) {
    (void)lost_seq;
    if (!d) return;
    if (d->fu_started) {
        d->fu_lost    = true;
        d->fu_started = false;
        d->fu_len     = 0;
        d->stats.reassembly_errors++;
    }
}

/* Grow FU reassembly buffer */
static int fu_ensure_cap(emd_h264_depay_t *d, size_t needed) {
    if (needed <= d->fu_cap) return 0;
    if (needed > EMD_H264_REASSEMBLY_MAX) return -1;
    size_t new_cap = d->fu_cap;
    while (new_cap < needed) new_cap *= 2;
    if (new_cap > EMD_H264_REASSEMBLY_MAX) new_cap = EMD_H264_REASSEMBLY_MAX;
    uint8_t *nb = realloc(d->fu_buf, new_cap);
    if (!nb) return -1;
    d->fu_buf = nb;
    d->fu_cap = new_cap;
    return 0;
}

/* -------------------------------------------------------------------------
 * Feed one RTP packet (RFC 6184)
 * ---------------------------------------------------------------------- */
int emd_h264_depay_feed(emd_h264_depay_t *d, const emd_rtp_pkt_t *pkt) {
    if (!d || !pkt) return -1;
    if (pkt->payload_len == 0) return 0;

    const uint8_t *payload = pkt->payload;
    size_t         plen    = pkt->payload_len;

    uint8_t nal_hdr = payload[0];
    uint8_t nal_type = nal_hdr & 0x1Fu;

    if (nal_type >= 1 && nal_type <= 23) {
        /* Single NAL unit */
        d->stats.single_nal++;
        d->nal_cb(payload, plen, pkt->marker, pkt->timestamp, d->userdata);

    } else if (nal_type == H264_RTP_STAP_A) {
        /* STAP-A: one or more NAL units packed */
        d->stats.stap_a++;
        size_t off = 1; /* skip NAL header */
        while (off + 2 <= plen) {
            uint16_t nlen = (uint16_t)((uint16_t)payload[off] << 8 | payload[off+1]);
            off += 2;
            if (off + nlen > plen) {
                d->stats.reassembly_errors++;
                break;
            }
            /* Check if this is the last NAL in the packet */
            bool last = (off + nlen >= plen);
            d->nal_cb(payload + off, nlen, pkt->marker && last,
                      pkt->timestamp, d->userdata);
            off += nlen;
        }

    } else if (nal_type == H264_RTP_FU_A) {
        /* FU-A fragmentation */
        d->stats.fu_a++;
        if (plen < 2) return -1;

        uint8_t fu_hdr = payload[1];
        bool    start  = !!(fu_hdr & H264_FU_S_BIT);
        bool    end    = !!(fu_hdr & H264_FU_E_BIT);
        /* Reconstructed NAL type: high 3 bits from nal_hdr, low 5 from fu_hdr */
        uint8_t recon_nal_type = (nal_hdr & 0xE0u) | (fu_hdr & 0x1Fu);

        if (start) {
            d->fu_started   = true;
            d->fu_lost      = false;
            d->fu_len       = 0;
            d->fu_pts       = pkt->timestamp;
            d->fu_start_seq = pkt->seq;

            /* Write reconstructed NAL header first */
            if (fu_ensure_cap(d, 1) < 0) return -1;
            d->fu_buf[0] = recon_nal_type;
            d->fu_len    = 1;
        }

        if (!d->fu_started || d->fu_lost) return 0;

        const uint8_t *frag = payload + 2;
        size_t         frag_len = plen - 2;

        if (fu_ensure_cap(d, d->fu_len + frag_len) < 0) {
            d->fu_started = false;
            d->stats.reassembly_errors++;
            return -1;
        }
        memcpy(d->fu_buf + d->fu_len, frag, frag_len);
        d->fu_len += frag_len;

        if (end) {
            d->nal_cb(d->fu_buf, d->fu_len, pkt->marker,
                      d->fu_pts, d->userdata);
            d->fu_started = false;
            d->fu_len     = 0;
        }

    } else if (nal_type == H264_RTP_STAP_B || nal_type == H264_RTP_MTAP16 ||
               nal_type == H264_RTP_MTAP24 || nal_type == H264_RTP_FU_B) {
        d->stats.unsupported++;
        EMD_LOGW("h264_depay", "unsupported RTP NAL type");
    } else {
        d->stats.unsupported++;
    }

    return 0;
}

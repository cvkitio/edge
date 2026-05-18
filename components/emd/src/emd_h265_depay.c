#include "emd/h265_depay.h"
#include "emd/log.h"

#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* -------------------------------------------------------------------------
 * Init / free / reset
 * ---------------------------------------------------------------------- */
int emd_h265_depay_init(emd_h265_depay_t *d, emd_h265_nal_cb nal_cb,
                         void *userdata, bool donl_enabled)
{
    if (!d || !nal_cb) return -1;
    memset(d, 0, sizeof(*d));
    d->nal_cb      = nal_cb;
    d->userdata    = userdata;
    d->donl_enabled = donl_enabled;

    d->fu_cap = 65536;
    d->fu_buf = malloc(d->fu_cap);
    if (!d->fu_buf) return -1;
    return 0;
}

void emd_h265_depay_free(emd_h265_depay_t *d) {
    if (!d) return;
    free(d->fu_buf);
    d->fu_buf = NULL;
}

void emd_h265_depay_reset(emd_h265_depay_t *d) {
    if (!d) return;
    d->fu_started = false;
    d->fu_lost    = false;
    d->fu_len     = 0;
}

void emd_h265_depay_lost(emd_h265_depay_t *d, uint16_t lost_seq) {
    (void)lost_seq;
    if (!d) return;
    if (d->fu_started) {
        d->fu_lost    = true;
        d->fu_started = false;
        d->fu_len     = 0;
        d->stats.reassembly_errors++;
    }
}

/* Grow FU buffer */
static int fu_ensure(emd_h265_depay_t *d, size_t needed) {
    if (needed <= d->fu_cap) return 0;
    if (needed > EMD_H265_REASSEMBLY_MAX) return -1;
    size_t nc = d->fu_cap;
    while (nc < needed) nc *= 2;
    if (nc > EMD_H265_REASSEMBLY_MAX) nc = EMD_H265_REASSEMBLY_MAX;
    uint8_t *nb = realloc(d->fu_buf, nc);
    if (!nb) return -1;
    d->fu_buf = nb;
    d->fu_cap = nc;
    return 0;
}

/* -------------------------------------------------------------------------
 * Feed (RFC 7798)
 * ---------------------------------------------------------------------- */
int emd_h265_depay_feed(emd_h265_depay_t *d, const emd_rtp_pkt_t *pkt) {
    if (!d || !pkt) return -1;
    if (pkt->payload_len < 2) return 0;

    const uint8_t *payload = pkt->payload;
    size_t         plen    = pkt->payload_len;

    /* H.265 NAL header: 2 bytes
     * bits[15:9] = forbidden_zero | nal_unit_type(6)
     * bits[8:6]  = nuh_layer_id
     * bits[5:0]  = nuh_temporal_id_plus1
     */
    uint8_t nal_type = (payload[0] >> 1) & 0x3Fu;

    if (nal_type == H265_RTP_AP) {
        /* Aggregation Packet */
        d->stats.ap++;
        size_t off = 2; /* skip 2-byte NAL header */

        /* DONL field (if enabled): 2 bytes */
        if (d->donl_enabled) {
            if (off + 2 > plen) return -1;
            off += 2; /* skip DONL */
        }

        while (off + 2 <= plen) {
            /* DOND field (if DONL enabled) is per-unit except first: 1 byte */
            if (d->donl_enabled && off != 2) {
                if (off + 1 > plen) break;
                off += 1; /* skip DOND */
                d->stats.donl_reorders++;
            }
            uint16_t nlen = (uint16_t)((uint16_t)payload[off] << 8 | payload[off+1]);
            off += 2;
            if (off + nlen > plen) {
                d->stats.reassembly_errors++;
                break;
            }
            bool last = (off + nlen >= plen);
            d->nal_cb(payload + off, nlen, pkt->marker && last,
                      pkt->timestamp, d->userdata);
            off += nlen;
        }

    } else if (nal_type == H265_RTP_FU) {
        /* Fragmentation Unit */
        d->stats.fu++;
        if (plen < 3) return -1;

        uint8_t fu_hdr = payload[2];
        bool    start  = !!(fu_hdr & H265_FU_S_BIT);
        bool    end    = !!(fu_hdr & H265_FU_E_BIT);
        uint8_t fu_nal_type = fu_hdr & 0x3Fu;

        /* DONL field on the start unit */
        size_t skip = 3;
        if (start && d->donl_enabled) {
            if (plen < 5) return -1;
            skip += 2; /* DONL */
        }

        if (start) {
            d->fu_started  = true;
            d->fu_lost     = false;
            d->fu_len      = 0;
            d->fu_pts      = pkt->timestamp;
            d->fu_start_seq= pkt->seq;
            d->fu_nal_type = fu_nal_type;

            /* Reconstruct 2-byte NAL header with the original nal_unit_type */
            if (fu_ensure(d, 2) < 0) return -1;
            d->fu_buf[0] = (uint8_t)(fu_nal_type << 1); /* forbidden=0, nal_type, layer=0 */
            d->fu_buf[1] = 0x01u;              /* temporal_id_plus1 = 1 */
            d->fu_len = 2;
        }

        if (!d->fu_started || d->fu_lost) return 0;

        const uint8_t *frag     = payload + skip;
        size_t         frag_len = plen - skip;

        if (fu_ensure(d, d->fu_len + frag_len) < 0) {
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

    } else if (nal_type == H265_RTP_PACI) {
        d->stats.unsupported++;
        EMD_LOGW("h265_depay", "PACI not supported");

    } else {
        /* Single NAL unit */
        d->stats.single_nal++;
        d->nal_cb(payload, plen, pkt->marker, pkt->timestamp, d->userdata);
    }

    return 0;
}

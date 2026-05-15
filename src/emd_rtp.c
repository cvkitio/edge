#include "emd/rtp.h"
#include "emd/log.h"

#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <arpa/inet.h>

/* -------------------------------------------------------------------------
 * RTP header parsing
 * ---------------------------------------------------------------------- */
int emd_rtp_parse(const uint8_t *buf, size_t len, emd_rtp_pkt_t *pkt) {
    if (!buf || !pkt || len < RTP_HEADER_MIN_LEN) return -1;

    pkt->version     = (buf[0] >> 6) & 0x03u;
    if (pkt->version != RTP_VERSION) return -1;

    pkt->padding     = !!(buf[0] & 0x20u);
    pkt->extension   = !!(buf[0] & 0x10u);
    pkt->csrc_count  = buf[0] & 0x0Fu;
    pkt->marker      = !!(buf[1] & 0x80u);
    pkt->payload_type = buf[1] & 0x7Fu;

    pkt->seq       = (uint16_t)((uint16_t)buf[2] << 8 | buf[3]);
    pkt->timestamp = ((uint32_t)buf[4] << 24) | ((uint32_t)buf[5] << 16) |
                     ((uint32_t)buf[6] <<  8) |  (uint32_t)buf[7];
    pkt->ssrc      = ((uint32_t)buf[8]  << 24) | ((uint32_t)buf[9]  << 16) |
                     ((uint32_t)buf[10] <<  8) |  (uint32_t)buf[11];

    size_t hdr_len = RTP_HEADER_MIN_LEN + (size_t)pkt->csrc_count * 4u;

    /* RTP extension header */
    if (pkt->extension) {
        if (len < hdr_len + 4) return -1;
        uint16_t ext_len = (uint16_t)((uint16_t)buf[hdr_len + 2] << 8 | buf[hdr_len + 3]);
        hdr_len += 4u + (size_t)ext_len * 4u;
    }

    if (len < hdr_len) return -1;

    pkt->payload     = buf + hdr_len;
    pkt->payload_len = len - hdr_len;

    /* Strip padding */
    if (pkt->padding && pkt->payload_len > 0) {
        uint8_t pad_bytes = pkt->payload[pkt->payload_len - 1];
        if (pad_bytes > pkt->payload_len) return -1;
        pkt->payload_len -= pad_bytes;
    }

    return 0;
}

/* -------------------------------------------------------------------------
 * Reorder buffer
 * ---------------------------------------------------------------------- */
void emd_rtp_reorder_init(emd_rtp_reorder_t *r) {
    memset(r, 0, sizeof(*r));
    r->initialized = false;
}

/* Sequence comparison: RFC 3550 §A.1 */
static inline bool seq_gt(uint16_t a, uint16_t b)
    __attribute__((unused));
static inline bool seq_gt(uint16_t a, uint16_t b) {
    return (int16_t)(a - b) > 0;
}

int emd_rtp_reorder_feed(emd_rtp_reorder_t *r,
                          const uint8_t *raw, size_t len,
                          emd_rtp_deliver_fn deliver_fn, void *userdata)
{
    emd_rtp_pkt_t pkt;
    if (emd_rtp_parse(raw, len, &pkt) < 0) return -1;

    r->received_count++;

    if (!r->initialized) {
        r->next_seq  = pkt.seq;
        r->initialized = true;
    }

    uint16_t seq = pkt.seq;
    /* Within window? */
    int16_t delta = (int16_t)(seq - r->next_seq);

    if (delta < 0) {
        /* Duplicate / very old: discard */
        return 0;
    }

    if (delta == 0) {
        /* In-order: deliver immediately */
        deliver_fn(&pkt, userdata);
        r->next_seq++;

        /* Drain any buffered consecutive packets */
        bool found;
        do {
            found = false;
            uint32_t slot = r->next_seq % EMD_RTP_REORDER_WIN;
            if ((r->slots_filled >> slot) & 1u) {
                emd_rtp_pkt_t q;
                if (emd_rtp_parse(r->buf[slot], r->lens[slot], &q) == 0) {
                    if (q.seq == r->next_seq) {
                        deliver_fn(&q, userdata);
                        r->slots_filled &= ~(1ULL << slot);
                        r->next_seq++;
                        found = true;
                    }
                } else {
                    r->slots_filled &= ~(1ULL << slot);
                }
            }
        } while (found);

        return 1;
    }

    if (delta < EMD_RTP_REORDER_WIN) {
        /* Out of order but within window: buffer */
        uint32_t slot = seq % EMD_RTP_REORDER_WIN;
        if (len <= sizeof(r->buf[slot])) {
            memcpy(r->buf[slot], raw, len);
            r->lens[slot] = len;
            r->seqs[slot] = seq;
            r->slots_filled |= (1ULL << slot);
        }
        return 0;
    }

    /* Too far ahead: flush and deliver */
    emd_rtp_reorder_flush(r, deliver_fn, userdata);
    r->next_seq = seq;
    deliver_fn(&pkt, userdata);
    r->next_seq++;
    r->loss_count += (uint64_t)(delta - 1);
    return 1;
}

void emd_rtp_reorder_flush(emd_rtp_reorder_t *r,
                             emd_rtp_deliver_fn deliver_fn, void *userdata)
{
    /* Deliver all buffered packets in seq order */
    for (int i = 0; i < EMD_RTP_REORDER_WIN; i++) {
        if (r->slots_filled == 0) break;
        uint32_t slot = r->next_seq % EMD_RTP_REORDER_WIN;
        if ((r->slots_filled >> slot) & 1u) {
            emd_rtp_pkt_t q;
            if (emd_rtp_parse(r->buf[slot], r->lens[slot], &q) == 0) {
                deliver_fn(&q, userdata);
            }
            r->slots_filled &= ~(1ULL << slot);
        }
        r->next_seq++;
    }
}

/* -------------------------------------------------------------------------
 * RTCP
 * ---------------------------------------------------------------------- */
int emd_rtcp_parse_sr(const uint8_t *buf, size_t len, emd_rtcp_sr_t *sr) {
    /* RTCP common header: V(2) P(1) RC(5) PT(8) length(16) SSRC(32) */
    if (len < 28) return -1;

    uint8_t pt = buf[1];
    if (pt != RTCP_PT_SR) return -1;

    sr->ntp_sec   = ((uint32_t)buf[8]  << 24) | ((uint32_t)buf[9]  << 16) |
                    ((uint32_t)buf[10] <<  8) |  (uint32_t)buf[11];
    sr->ntp_frac  = ((uint32_t)buf[12] << 24) | ((uint32_t)buf[13] << 16) |
                    ((uint32_t)buf[14] <<  8) |  (uint32_t)buf[15];
    sr->rtp_ts    = ((uint32_t)buf[16] << 24) | ((uint32_t)buf[17] << 16) |
                    ((uint32_t)buf[18] <<  8) |  (uint32_t)buf[19];
    sr->pkt_count = ((uint32_t)buf[20] << 24) | ((uint32_t)buf[21] << 16) |
                    ((uint32_t)buf[22] <<  8) |  (uint32_t)buf[23];
    sr->byte_count= ((uint32_t)buf[24] << 24) | ((uint32_t)buf[25] << 16) |
                    ((uint32_t)buf[26] <<  8) |  (uint32_t)buf[27];
    return 0;
}

int emd_rtcp_build_rr(uint8_t *buf, size_t bufsz,
                       uint32_t ssrc_self, uint32_t ssrc_sender,
                       uint8_t fraction_lost, uint32_t cumulative_lost,
                       uint32_t highest_seq, uint32_t jitter,
                       uint32_t last_sr, uint32_t delay_since_sr)
{
    if (bufsz < 32) return -1;

    /* V=2, P=0, RC=1, PT=RR, length=7 (in 32-bit words minus 1) */
    buf[0] = 0x81u; /* V=2, P=0, RC=1 */
    buf[1] = RTCP_PT_RR;
    buf[2] = 0x00u;
    buf[3] = 0x07u; /* length = 7 */

    /* SSRC of packet sender */
    buf[4] = (uint8_t)(ssrc_self >> 24);
    buf[5] = (uint8_t)(ssrc_self >> 16);
    buf[6] = (uint8_t)(ssrc_self >>  8);
    buf[7] = (uint8_t)(ssrc_self);

    /* Report block: SSRC_1 */
    buf[8]  = (uint8_t)(ssrc_sender >> 24);
    buf[9]  = (uint8_t)(ssrc_sender >> 16);
    buf[10] = (uint8_t)(ssrc_sender >>  8);
    buf[11] = (uint8_t)(ssrc_sender);

    buf[12] = fraction_lost;
    /* 24-bit cumulative lost */
    buf[13] = (uint8_t)((cumulative_lost >> 16) & 0xFF);
    buf[14] = (uint8_t)((cumulative_lost >>  8) & 0xFF);
    buf[15] = (uint8_t)( cumulative_lost        & 0xFF);

    buf[16] = (uint8_t)(highest_seq >> 24);
    buf[17] = (uint8_t)(highest_seq >> 16);
    buf[18] = (uint8_t)(highest_seq >>  8);
    buf[19] = (uint8_t)(highest_seq);

    buf[20] = (uint8_t)(jitter >> 24);
    buf[21] = (uint8_t)(jitter >> 16);
    buf[22] = (uint8_t)(jitter >>  8);
    buf[23] = (uint8_t)(jitter);

    buf[24] = (uint8_t)(last_sr >> 24);
    buf[25] = (uint8_t)(last_sr >> 16);
    buf[26] = (uint8_t)(last_sr >>  8);
    buf[27] = (uint8_t)(last_sr);

    buf[28] = (uint8_t)(delay_since_sr >> 24);
    buf[29] = (uint8_t)(delay_since_sr >> 16);
    buf[30] = (uint8_t)(delay_since_sr >>  8);
    buf[31] = (uint8_t)(delay_since_sr);

    return 32;
}

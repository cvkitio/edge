/*
 * test_h265_depay.c — Unit tests for RFC 7798 H.265 RTP depacketizer.
 */

#include <cmocka.h>
#include "emd/h265_depay.h"
#include "emd/rtp.h"

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>

/* --------------------------------------------------------------------- */
/* NAL capture helper                                                       */
/* --------------------------------------------------------------------- */

#define MAX_NAL_CAPTURE 16

typedef struct {
    uint8_t  *data[MAX_NAL_CAPTURE];
    size_t    len[MAX_NAL_CAPTURE];
    int       count;
} nal_capture_t;

static void nal_capture_cb(const uint8_t *nal, size_t len,
                            bool marker, uint32_t pts, void *userdata)
{
    (void)marker; (void)pts;
    nal_capture_t *cap = (nal_capture_t *)userdata;
    if (cap->count >= MAX_NAL_CAPTURE) return;
    int i = cap->count;
    cap->data[i] = malloc(len);
    if (!cap->data[i]) return;
    memcpy(cap->data[i], nal, len);
    cap->len[i]  = len;
    cap->count++;
}

static void nal_capture_reset(nal_capture_t *cap) {
    for (int i = 0; i < cap->count; i++) free(cap->data[i]);
    memset(cap, 0, sizeof(*cap));
}

static void make_rtp_pkt(emd_rtp_pkt_t *pkt,
                          const uint8_t *payload, size_t len,
                          uint16_t seq, uint32_t ts, bool marker)
{
    memset(pkt, 0, sizeof(*pkt));
    pkt->payload     = payload;
    pkt->payload_len = len;
    pkt->seq         = seq;
    pkt->timestamp   = ts;
    pkt->marker      = marker;
    pkt->version      = 2;
    pkt->payload_type = 96;
}

/* --------------------------------------------------------------------- */
/* Single NAL unit (type != FU=49, != AP=48)                              */
/* --------------------------------------------------------------------- */

static void test_single_nal(void **state) {
    (void)state;
    nal_capture_t cap;
    memset(&cap, 0, sizeof(cap));

    emd_h265_depay_t d;
    int rc = emd_h265_depay_init(&d, nal_capture_cb, &cap, false);
    assert_int_equal(rc, 0);

    /* HEVC single NAL: type=TRAIL_N(1), layer_id=0, temporal_id=1
     * byte0 = (1 << 1) = 0x02, byte1 = 0x01 */
    uint8_t payload[] = {0x02, 0x01, 0xAA, 0xBB, 0xCC};
    emd_rtp_pkt_t pkt;
    make_rtp_pkt(&pkt, payload, sizeof(payload), 1, 9000, true);

    int r = emd_h265_depay_feed(&d, &pkt);
    assert_true(r >= 0);
    assert_int_equal(cap.count, 1);
    if (cap.count >= 1) {
        assert_int_equal((int)cap.len[0], (int)sizeof(payload));
        assert_int_equal(cap.data[0][0], 0x02);
    }

    nal_capture_reset(&cap);
    emd_h265_depay_free(&d);
}

/* --------------------------------------------------------------------- */
/* AP (Aggregation Packet, type 48)                                        */
/* --------------------------------------------------------------------- */

static void test_ap_two_nals(void **state) {
    (void)state;
    nal_capture_t cap;
    memset(&cap, 0, sizeof(cap));

    emd_h265_depay_t d;
    emd_h265_depay_init(&d, nal_capture_cb, &cap, false);

    /* AP header: type=48 → byte0=(48<<1)=0x60, byte1=0x01 */
    uint8_t nal1[] = {0x40, 0x01, 0x0C, 0x01}; /* VPS */
    uint8_t nal2[] = {0x42, 0x01, 0x01, 0x01}; /* SPS */

    size_t plen = 2 + 2 + sizeof(nal1) + 2 + sizeof(nal2);
    uint8_t *payload = malloc(plen);
    assert_non_null(payload);

    size_t pos = 0;
    payload[pos++] = 0x60; /* AP type */
    payload[pos++] = 0x01;
    payload[pos++] = 0x00; payload[pos++] = (uint8_t)sizeof(nal1);
    memcpy(payload + pos, nal1, sizeof(nal1)); pos += sizeof(nal1);
    payload[pos++] = 0x00; payload[pos++] = (uint8_t)sizeof(nal2);
    memcpy(payload + pos, nal2, sizeof(nal2)); pos += sizeof(nal2);

    emd_rtp_pkt_t pkt;
    make_rtp_pkt(&pkt, payload, pos, 2, 9000, true);

    int r = emd_h265_depay_feed(&d, &pkt);
    free(payload);
    assert_true(r >= 0);
    assert_int_equal(cap.count, 2);
    if (cap.count >= 1) {
        assert_int_equal((int)cap.len[0], (int)sizeof(nal1));
    }

    nal_capture_reset(&cap);
    emd_h265_depay_free(&d);
}

/* --------------------------------------------------------------------- */
/* FU (Fragmentation Unit, type 49) reassembly                             */
/* --------------------------------------------------------------------- */

static void test_fu_reassembly(void **state) {
    (void)state;
    nal_capture_t cap;
    memset(&cap, 0, sizeof(cap));

    emd_h265_depay_t d;
    emd_h265_depay_init(&d, nal_capture_cb, &cap, false);

    /* Original NAL: IDR_W_RADL (type=19), 8-byte body */
    uint8_t body[] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};

    /* FU PayloadHdr: type=49 → (49<<1)=0x62, byte1=0x01 */
    /* FU header: S=1,E=0,type=19 → 0x80|19=0x93 */
    uint8_t pkt1[] = {0x62, 0x01, 0x93, body[0], body[1], body[2], body[3]};
    /* FU header: S=0,E=1,type=19 → 0x40|19=0x53 */
    uint8_t pkt2[] = {0x62, 0x01, 0x53, body[4], body[5], body[6], body[7]};

    emd_rtp_pkt_t pkt;

    make_rtp_pkt(&pkt, pkt1, sizeof(pkt1), 10, 18000, false);
    int r = emd_h265_depay_feed(&d, &pkt);
    assert_true(r >= 0);
    assert_int_equal(cap.count, 0);

    make_rtp_pkt(&pkt, pkt2, sizeof(pkt2), 11, 18000, true);
    r = emd_h265_depay_feed(&d, &pkt);
    assert_true(r >= 0);
    assert_int_equal(cap.count, 1);
    if (cap.count >= 1) {
        /* Reassembled: 2-byte NAL header + body(8) */
        assert_int_equal((int)cap.len[0], 2 + 8);
        uint8_t reconstructed_type = (cap.data[0][0] >> 1) & 0x3Fu;
        assert_int_equal((int)reconstructed_type, 19); /* IDR_W_RADL */
        assert_memory_equal(cap.data[0] + 2, body, 8);
    }

    nal_capture_reset(&cap);
    emd_h265_depay_free(&d);
}

/* --------------------------------------------------------------------- */
/* Test runner                                                              */
/* --------------------------------------------------------------------- */

int main(void) {
    const CMUnitTest tests[] = {
        cmocka_unit_test(test_single_nal),
        cmocka_unit_test(test_ap_two_nals),
        cmocka_unit_test(test_fu_reassembly),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}

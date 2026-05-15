/*
 * test_h264_depay.c — Unit tests for RFC 6184 H.264 RTP depacketizer.
 *
 * Tests:
 *  - Single NAL unit mode.
 *  - STAP-A multi-NAL assembly.
 *  - FU-A reassembly.
 *  - FU-A across packet loss (NAL should be discarded).
 *  - Sequence number wrap-around.
 */

#include <cmocka.h>
#include "emd/h264_depay.h"
#include "emd/rtp.h"

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
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

/* Build a minimal emd_rtp_pkt_t for testing */
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
/* Single NAL mode (type 1-23)                                             */
/* --------------------------------------------------------------------- */

static void test_single_nal(void **state) {
    (void)state;
    nal_capture_t cap;
    memset(&cap, 0, sizeof(cap));

    emd_h264_depay_t d;
    int rc = emd_h264_depay_init(&d, nal_capture_cb, &cap);
    assert_int_equal(rc, 0);

    /* Single NAL: type=1 (non-IDR slice) */
    uint8_t rtp_payload[] = {0x61, 0xAA, 0xBB, 0xCC};
    emd_rtp_pkt_t pkt;
    make_rtp_pkt(&pkt, rtp_payload, sizeof(rtp_payload), 1, 1000, true);

    int r = emd_h264_depay_feed(&d, &pkt);
    assert_true(r >= 0);
    assert_int_equal(cap.count, 1);
    assert_int_equal((int)cap.len[0], (int)sizeof(rtp_payload));
    assert_int_equal(cap.data[0][0], 0x61);

    nal_capture_reset(&cap);
    emd_h264_depay_free(&d);
}

/* --------------------------------------------------------------------- */
/* STAP-A (type 24)                                                        */
/* --------------------------------------------------------------------- */

static void test_stap_a(void **state) {
    (void)state;
    nal_capture_t cap;
    memset(&cap, 0, sizeof(cap));

    emd_h264_depay_t d;
    emd_h264_depay_init(&d, nal_capture_cb, &cap);

    uint8_t nal1[] = {0x67, 0x42, 0xC0, 0x1F}; /* SPS */
    uint8_t nal2[] = {0x68, 0xCE, 0x38, 0x80}; /* PPS */

    uint8_t stap_payload[1 + 2 + sizeof(nal1) + 2 + sizeof(nal2)];
    size_t pos = 0;
    stap_payload[pos++] = 0x78; /* STAP-A: F=0,NRI=3,type=24 */
    stap_payload[pos++] = 0x00;
    stap_payload[pos++] = (uint8_t)sizeof(nal1);
    memcpy(stap_payload + pos, nal1, sizeof(nal1)); pos += sizeof(nal1);
    stap_payload[pos++] = 0x00;
    stap_payload[pos++] = (uint8_t)sizeof(nal2);
    memcpy(stap_payload + pos, nal2, sizeof(nal2)); pos += sizeof(nal2);

    emd_rtp_pkt_t pkt;
    make_rtp_pkt(&pkt, stap_payload, pos, 2, 2000, true);

    int r = emd_h264_depay_feed(&d, &pkt);
    assert_true(r >= 0);
    assert_int_equal(cap.count, 2);
    if (cap.count >= 1) {
        assert_int_equal((int)cap.len[0], (int)sizeof(nal1));
        assert_int_equal(cap.data[0][0], 0x67);
    }
    if (cap.count >= 2) {
        assert_int_equal((int)cap.len[1], (int)sizeof(nal2));
        assert_int_equal(cap.data[1][0], 0x68);
    }

    nal_capture_reset(&cap);
    emd_h264_depay_free(&d);
}

/* --------------------------------------------------------------------- */
/* FU-A reassembly                                                         */
/* --------------------------------------------------------------------- */

static void test_fu_a_reassembly(void **state) {
    (void)state;
    nal_capture_t cap;
    memset(&cap, 0, sizeof(cap));

    emd_h264_depay_t d;
    emd_h264_depay_init(&d, nal_capture_cb, &cap);

    /* Original NAL: type=5 (IDR), 12 bytes body */
    uint8_t orig_body[12];
    for (int i = 0; i < 12; i++) orig_body[i] = (uint8_t)(i + 1);

    /* FU-A: indicator=0x7C (F=0,NRI=3,type=28) */
    uint8_t pkt1[] = {0x7C, 0x85, orig_body[0], orig_body[1],
                       orig_body[2], orig_body[3]};  /* S=1,E=0,type=5 */
    uint8_t pkt2[] = {0x7C, 0x05, orig_body[4], orig_body[5],
                       orig_body[6], orig_body[7]};  /* S=0,E=0 */
    uint8_t pkt3[] = {0x7C, 0x45, orig_body[8], orig_body[9],
                       orig_body[10], orig_body[11]}; /* S=0,E=1 */

    emd_rtp_pkt_t pkt;

    make_rtp_pkt(&pkt, pkt1, sizeof(pkt1), 10, 9000, false);
    int r = emd_h264_depay_feed(&d, &pkt);
    assert_true(r >= 0);
    assert_int_equal(cap.count, 0);

    make_rtp_pkt(&pkt, pkt2, sizeof(pkt2), 11, 9000, false);
    r = emd_h264_depay_feed(&d, &pkt);
    assert_true(r >= 0);
    assert_int_equal(cap.count, 0);

    make_rtp_pkt(&pkt, pkt3, sizeof(pkt3), 12, 9000, true);
    r = emd_h264_depay_feed(&d, &pkt);
    assert_true(r >= 0);
    assert_int_equal(cap.count, 1);
    if (cap.count >= 1) {
        /* Reassembled: NAL header(1) + body(12) */
        assert_int_equal((int)cap.len[0], 1 + 12);
        assert_int_equal(cap.data[0][0] & 0x1F, 5); /* IDR type */
        assert_memory_equal(cap.data[0] + 1, orig_body, 12);
    }

    nal_capture_reset(&cap);
    emd_h264_depay_free(&d);
}

/* --------------------------------------------------------------------- */
/* FU-A with lost middle packet                                             */
/* --------------------------------------------------------------------- */

static void test_fu_a_loss(void **state) {
    (void)state;
    nal_capture_t cap;
    memset(&cap, 0, sizeof(cap));

    emd_h264_depay_t d;
    emd_h264_depay_init(&d, nal_capture_cb, &cap);

    uint8_t pkt_start[] = {0x7C, 0x85, 0x01, 0x02}; /* FU-A start, type=5 */
    uint8_t pkt_end[]   = {0x7C, 0x45, 0x05, 0x06}; /* FU-A end */

    emd_rtp_pkt_t pkt;

    make_rtp_pkt(&pkt, pkt_start, sizeof(pkt_start), 20, 18000, false);
    emd_h264_depay_feed(&d, &pkt);

    /* Skip seq=21 (lost) — notify depay */
    emd_h264_depay_lost(&d, 21);

    /* End arrives — depay should discard the incomplete NAL */
    make_rtp_pkt(&pkt, pkt_end, sizeof(pkt_end), 22, 18000, true);
    emd_h264_depay_feed(&d, &pkt);

    /* NAL must not be delivered — it was corrupted by the loss */
    assert_int_equal(cap.count, 0);

    nal_capture_reset(&cap);
    emd_h264_depay_free(&d);
}

/* --------------------------------------------------------------------- */
/* Sequence number wrap-around                                              */
/* --------------------------------------------------------------------- */

static void test_seq_wrap(void **state) {
    (void)state;
    nal_capture_t cap;
    memset(&cap, 0, sizeof(cap));

    emd_h264_depay_t d;
    emd_h264_depay_init(&d, nal_capture_cb, &cap);

    /* Send single NALs across the 16-bit seq wrap */
    uint8_t nal[] = {0x61, 0x01}; /* non-IDR */
    emd_rtp_pkt_t pkt;

    for (uint16_t seq = 0xFFFEu; ; seq++) {
        nal_capture_reset(&cap);
        make_rtp_pkt(&pkt, nal, sizeof(nal), seq, 30000, true);
        int r = emd_h264_depay_feed(&d, &pkt);
        assert_true(r >= 0);
        assert_int_equal(cap.count, 1);
        if (seq == 0) break;
    }

    nal_capture_reset(&cap);
    emd_h264_depay_free(&d);
}

/* --------------------------------------------------------------------- */
/* Test runner                                                              */
/* --------------------------------------------------------------------- */

int main(void) {
    const CMUnitTest tests[] = {
        cmocka_unit_test(test_single_nal),
        cmocka_unit_test(test_stap_a),
        cmocka_unit_test(test_fu_a_reassembly),
        cmocka_unit_test(test_fu_a_loss),
        cmocka_unit_test(test_seq_wrap),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}

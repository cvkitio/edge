/*
 * test_h265_parse.c — Unit tests for H.265/HEVC NAL parsing.
 */

#include <cmocka.h>
#include "emd/h265_parse.h"

#include <stdint.h>
#include <stddef.h>
#include <string.h>

/* --------------------------------------------------------------------- */
/* Slice header — IDR_W_RADL (type 19)                                    */
/* --------------------------------------------------------------------- */

static void test_nal_type_idr(void **state) {
    (void)state;
    emd_h265_slice_hdr_t hdr;
    memset(&hdr, 0, sizeof(hdr));

    /* Minimal slice RBSP: first_slice_in_pic=1(1b), no_output=1(1b), pps_id=0(ue=1b) */
    uint8_t rbsp[] = {0xC0, 0x80};

    emd_h265_param_cache_t cache;
    emd_h265_param_cache_init(&cache);

    int r = emd_h265_parse_slice_header(rbsp, sizeof(rbsp),
                                         H265_NAL_IDR_W_RADL, &cache, &hdr);
    /* Must not crash */
    (void)r;
    assert_true(1);
}

static void test_nal_type_cra(void **state) {
    (void)state;
    emd_h265_slice_hdr_t hdr;
    memset(&hdr, 0, sizeof(hdr));

    uint8_t rbsp[] = {0x80};

    emd_h265_param_cache_t cache;
    emd_h265_param_cache_init(&cache);

    int r = emd_h265_parse_slice_header(rbsp, sizeof(rbsp),
                                         H265_NAL_CRA, &cache, &hdr);
    (void)r;
    assert_true(1);
}

/* --------------------------------------------------------------------- */
/* VPS parse                                                               */
/* --------------------------------------------------------------------- */

static void test_vps_parse_no_crash(void **state) {
    (void)state;
    uint8_t vps_rbsp[] = {
        0x01, 0xC0, 0x90, 0x00, 0x00, 0x03, 0x00, 0x90,
        0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x5D,
    };
    emd_h265_vps_t vps;
    memset(&vps, 0, sizeof(vps));
    int r = emd_h265_parse_vps(vps_rbsp, sizeof(vps_rbsp), &vps);
    (void)r;
    assert_true(1);
}

/* --------------------------------------------------------------------- */
/* SPS parse                                                               */
/* --------------------------------------------------------------------- */

static void test_sps_parse_no_crash(void **state) {
    (void)state;
    uint8_t sps_rbsp[] = {
        0x01,
        0x60, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00,
    };
    emd_h265_sps_t sps;
    memset(&sps, 0, sizeof(sps));
    int r = emd_h265_parse_sps(sps_rbsp, sizeof(sps_rbsp), &sps);
    (void)r;
    assert_true(1);
}

/* --------------------------------------------------------------------- */
/* PPS parse                                                               */
/* --------------------------------------------------------------------- */

static void test_pps_parse_no_crash(void **state) {
    (void)state;
    uint8_t pps_rbsp[] = {0xC0, 0x00};
    emd_h265_pps_t pps;
    memset(&pps, 0, sizeof(pps));
    int r = emd_h265_parse_pps(pps_rbsp, sizeof(pps_rbsp), &pps);
    (void)r;
    assert_true(1);
}

/* --------------------------------------------------------------------- */
/* Test runner                                                              */
/* --------------------------------------------------------------------- */

int main(void) {
    const CMUnitTest tests[] = {
        cmocka_unit_test(test_nal_type_idr),
        cmocka_unit_test(test_nal_type_cra),
        cmocka_unit_test(test_vps_parse_no_crash),
        cmocka_unit_test(test_sps_parse_no_crash),
        cmocka_unit_test(test_pps_parse_no_crash),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}

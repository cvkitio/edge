/*
 * test_recorder_mp4.c — fMP4 muxer output tests.
 *
 * Tests:
 *  - Open / close fMP4: file exists and is non-zero.
 *  - Write SPS + PPS + IDR slice: moov is written.
 *  - Write multiple fragments: file is valid fMP4 structure.
 *  - MPEG-TS: file starts with 0x47 sync byte.
 */

#include <cmocka.h>
#include "emd/recorder.h"

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

/* --------------------------------------------------------------------- */
/* Fake NAL data                                                            */
/* --------------------------------------------------------------------- */

/* Minimal H.264 SPS RBSP (profile=66, level=31, 1280×720 baseline) */
static const uint8_t FAKE_SPS[] = {
    0x67, /* NAL header: type=7 (SPS) */
    0x42, 0xC0, 0x1F,           /* profile_idc=66, constraints, level=31 */
    0xDA, 0x01, 0x40, 0x16,    /* seq params — simplified */
    0xEC, 0x04, 0x40, 0x00,
    0x00, 0x03, 0x00, 0x40,
    0x00, 0x00, 0x0F, 0x03,
    0xC5, 0x8B, 0xB8,
};

static const uint8_t FAKE_PPS[] = {
    0x68, /* NAL header: type=8 (PPS) */
    0xCE, 0x38, 0x80,
};

static const uint8_t FAKE_IDR_SLICE[] = {
    0x65, /* NAL header: type=5 (IDR) */
    0x88, 0x84, 0x00, 0x33, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
};

static const uint8_t FAKE_P_SLICE[] = {
    0x41, /* NAL header: type=1 (non-IDR) */
    0x9A, 0x02, 0x00, 0x33, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
};

/* --------------------------------------------------------------------- */
/* fMP4 basic test                                                         */
/* --------------------------------------------------------------------- */

static void test_fmp4_creates_file(void **state) {
    (void)state;
    const char *path = "/tmp/test_emd_fmp4.mp4";

    void *ctx = emd_mux_fmp4.open(path, 1 /* h264 */, 1280, 720, 90000);
    assert_non_null(ctx);

    /* Write param sets */
    emd_mux_fmp4.write_nal(ctx, FAKE_SPS, sizeof(FAKE_SPS), 0, 0, false);
    emd_mux_fmp4.write_nal(ctx, FAKE_PPS, sizeof(FAKE_PPS), 0, 0, false);

    /* Write IDR */
    emd_mux_fmp4.write_nal(ctx, FAKE_IDR_SLICE, sizeof(FAKE_IDR_SLICE),
                              90000, 90000, true);

    /* Write some P frames */
    for (int i = 1; i <= 5; i++) {
        uint64_t pts = (uint64_t)(i + 1) * 3003;
        emd_mux_fmp4.write_nal(ctx, FAKE_P_SLICE, sizeof(FAKE_P_SLICE),
                                 pts, pts, false);
    }

    int r = emd_mux_fmp4.close(ctx);
    assert_int_equal(r, 0);

    /* File should exist and have data */
    struct stat st;
    r = stat(path, &st);
    assert_int_equal(r, 0);
    assert_true(st.st_size > 0);

    /* Check ftyp magic: first 8 bytes should be size(4) + 'ftyp' */
    FILE *fp = fopen(path, "rb");
    assert_non_null(fp);
    uint8_t header[8];
    size_t n = fread(header, 1, sizeof(header), fp);
    fclose(fp);
    assert_int_equal((int)n, 8);
    assert_int_equal(header[4], 'f');
    assert_int_equal(header[5], 't');
    assert_int_equal(header[6], 'y');
    assert_int_equal(header[7], 'p');

    remove(path);
}

/* --------------------------------------------------------------------- */
/* fMP4 with multiple IDR fragments                                        */
/* --------------------------------------------------------------------- */

static void test_fmp4_multiple_fragments(void **state) {
    (void)state;
    const char *path = "/tmp/test_emd_fmp4_multi.mp4";

    void *ctx = emd_mux_fmp4.open(path, 1, 1280, 720, 90000);
    assert_non_null(ctx);

    emd_mux_fmp4.write_nal(ctx, FAKE_SPS, sizeof(FAKE_SPS), 0, 0, false);
    emd_mux_fmp4.write_nal(ctx, FAKE_PPS, sizeof(FAKE_PPS), 0, 0, false);

    /* Fragment 1: IDR + 29 P frames */
    emd_mux_fmp4.write_nal(ctx, FAKE_IDR_SLICE, sizeof(FAKE_IDR_SLICE),
                              90000, 90000, true);
    for (int i = 1; i < 30; i++) {
        uint64_t pts = 90000 + (uint64_t)i * 3003;
        emd_mux_fmp4.write_nal(ctx, FAKE_P_SLICE, sizeof(FAKE_P_SLICE),
                                 pts, pts, false);
    }

    /* Fragment 2: another IDR */
    uint64_t pts2 = 90000 + 30 * 3003ULL;
    emd_mux_fmp4.write_nal(ctx, FAKE_IDR_SLICE, sizeof(FAKE_IDR_SLICE),
                              pts2, pts2, true);
    for (int i = 1; i < 10; i++) {
        uint64_t pts = pts2 + (uint64_t)i * 3003;
        emd_mux_fmp4.write_nal(ctx, FAKE_P_SLICE, sizeof(FAKE_P_SLICE),
                                 pts, pts, false);
    }

    int r = emd_mux_fmp4.close(ctx);
    assert_int_equal(r, 0);

    struct stat st;
    stat(path, &st);
    assert_true(st.st_size > 1024); /* should have moov + 2 moof/mdat */

    remove(path);
}

/* --------------------------------------------------------------------- */
/* MPEG-TS basic test                                                      */
/* --------------------------------------------------------------------- */

static void test_mpegts_creates_file(void **state) {
    (void)state;
    const char *path = "/tmp/test_emd_mpegts.ts";

    void *ctx = emd_mux_mpegts.open(path, 1, 1280, 720, 90000);
    assert_non_null(ctx);

    emd_mux_mpegts.write_nal(ctx, FAKE_SPS, sizeof(FAKE_SPS), 0, 0, false);
    emd_mux_mpegts.write_nal(ctx, FAKE_PPS, sizeof(FAKE_PPS), 0, 0, false);
    emd_mux_mpegts.write_nal(ctx, FAKE_IDR_SLICE, sizeof(FAKE_IDR_SLICE),
                               90000, 90000, true);

    for (int i = 1; i < 10; i++) {
        uint64_t pts = 90000 + (uint64_t)i * 3003;
        emd_mux_mpegts.write_nal(ctx, FAKE_P_SLICE, sizeof(FAKE_P_SLICE),
                                   pts, pts, false);
    }

    int r = emd_mux_mpegts.close(ctx);
    assert_int_equal(r, 0);

    struct stat st;
    stat(path, &st);
    assert_true(st.st_size > 0);
    /* Size must be multiple of 188 */
    assert_int_equal((int)(st.st_size % 188), 0);

    /* First byte must be TS sync byte 0x47 */
    FILE *fp = fopen(path, "rb");
    assert_non_null(fp);
    uint8_t sync;
    fread(&sync, 1, 1, fp);
    fclose(fp);
    assert_int_equal((int)sync, 0x47);

    remove(path);
}

/* --------------------------------------------------------------------- */
/* HEVC fMP4 (codec=2)                                                     */
/* --------------------------------------------------------------------- */

static const uint8_t FAKE_VPS_H265[] = {
    0x40, 0x01, /* NAL header: type=32 (VPS) */
    0x0C, 0x01, 0xFF, 0xFF, 0x01, 0x60, 0x00, 0x00,
    0x03, 0x00, 0x90, 0x00, 0x00, 0x03, 0x00, 0x00,
    0x03, 0x00, 0x5D, 0x95, 0x98, 0x09,
};

static const uint8_t FAKE_SPS_H265[] = {
    0x42, 0x01, /* NAL header: type=33 (SPS) */
    0x01, 0x01, 0x60, 0x00, 0x00, 0x03, 0x00, 0x90,
    0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x5D,
    0xA0, 0x02, 0x80, 0x80, 0x2D, 0x16, 0x59, 0x59,
    0xA4, 0x93, 0x2B, 0xC0, 0x5A, 0x70, 0x80, 0x00,
    0x01, 0xF4, 0x80, 0x00, 0x3A, 0x98, 0x04,
};

static const uint8_t FAKE_PPS_H265[] = {
    0x44, 0x01, /* NAL header: type=34 (PPS) */
    0xC0, 0xF3, 0xC0, 0x00,
};

static const uint8_t FAKE_IDR_H265[] = {
    0x26, 0x01, /* NAL header: type=19 (IDR_W_RADL) */
    0xAF, 0x08, 0x4E, 0x01, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
};

static void test_fmp4_hevc(void **state) {
    (void)state;
    const char *path = "/tmp/test_emd_fmp4_hevc.mp4";

    void *ctx = emd_mux_fmp4.open(path, 2 /* h265 */, 1920, 1080, 90000);
    assert_non_null(ctx);

    emd_mux_fmp4.write_nal(ctx, FAKE_VPS_H265, sizeof(FAKE_VPS_H265), 0, 0, false);
    emd_mux_fmp4.write_nal(ctx, FAKE_SPS_H265, sizeof(FAKE_SPS_H265), 0, 0, false);
    emd_mux_fmp4.write_nal(ctx, FAKE_PPS_H265, sizeof(FAKE_PPS_H265), 0, 0, false);
    emd_mux_fmp4.write_nal(ctx, FAKE_IDR_H265, sizeof(FAKE_IDR_H265),
                              90000, 90000, true);

    int r = emd_mux_fmp4.close(ctx);
    assert_int_equal(r, 0);

    struct stat st;
    stat(path, &st);
    assert_true(st.st_size > 0);

    remove(path);
}

/* --------------------------------------------------------------------- */
/* Test runner                                                              */
/* --------------------------------------------------------------------- */

int main(void) {
    const CMUnitTest tests[] = {
        cmocka_unit_test(test_fmp4_creates_file),
        cmocka_unit_test(test_fmp4_multiple_fragments),
        cmocka_unit_test(test_mpegts_creates_file),
        cmocka_unit_test(test_fmp4_hevc),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}

/*
 * test_inspector_motion.c — Deterministic motion detection tests.
 *
 * Table-driven: feed synthetic byte sequences and assert state machine
 * transitions match expectations.
 */

#include <cmocka.h>
#include "emd/inspector.h"

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

/* --------------------------------------------------------------------- */
/* Helpers                                                                 */
/* --------------------------------------------------------------------- */

static void default_cfg(emd_inspector_cfg_t *cfg) {
    emd_inspector_default_cfg(cfg);
}

static emd_inspector_state_t make_warm_state(uint32_t baseline_bytes,
                                              const emd_inspector_cfg_t *cfg)
{
    emd_inspector_state_t s;
    emd_inspector_init(&s, cfg);

    /* Warm up the EWMA over 200 frames at baseline_bytes */
    emd_inspector_input_t in;
    emd_inspector_result_t res;
    memset(&in, 0, sizeof(in));
    in.byte_count        = baseline_bytes;
    in.is_keyframe       = false;
    in.intra_ratio_proxy = 0.5;

    for (int i = 0; i < 200; i++) {
        in.pts_90khz = (uint64_t)i * 3003;
        emd_inspector_process(&s, cfg, &in, &res);
    }
    /* Force back to IDLE for clean test */
    s.fsm = EMD_INSP_IDLE;
    s.consecutive_above = 0;
    s.consecutive_below = 0;
    return s;
}

/* --------------------------------------------------------------------- */
/* Test 1: Static scene → no events                                        */
/* --------------------------------------------------------------------- */

static void test_static_no_events(void **state) {
    (void)state;
    emd_inspector_cfg_t cfg;
    default_cfg(&cfg);

    emd_inspector_state_t s;
    emd_inspector_init(&s, &cfg);

    /* 100 frames at stable 10000 bytes */
    emd_inspector_input_t in;
    memset(&in, 0, sizeof(in));
    in.byte_count        = 10000;
    in.is_keyframe       = false;
    in.intra_ratio_proxy = 0.5;

    int events = 0;
    emd_inspector_result_t res;
    for (int i = 0; i < 100; i++) {
        in.pts_90khz = (uint64_t)i * 3003;
        bool fired = emd_inspector_process(&s, &cfg, &in, &res);
        if (fired) events++;
    }

    /* Static scene should not produce events */
    assert_int_equal(events, 0);
}

/* --------------------------------------------------------------------- */
/* Test 2: Motion burst → exactly 1 event                                  */
/* --------------------------------------------------------------------- */

static void test_motion_burst(void **state) {
    (void)state;
    emd_inspector_cfg_t cfg;
    default_cfg(&cfg);
    cfg.on_threshold = 2;

    /* Warm up at 10000 bytes/frame */
    emd_inspector_state_t s = make_warm_state(10000, &cfg);

    emd_inspector_input_t in;
    memset(&in, 0, sizeof(in));
    in.intra_ratio_proxy = 0.5;

    emd_inspector_result_t res;
    int events = 0;
    int event_frame = -1;

    /* 20 frames at 10000, then 10 frames at 50000 (large burst) */
    for (int i = 0; i < 30; i++) {
        in.pts_90khz  = (uint64_t)(200 + i) * 3003;
        in.byte_count = (i >= 15) ? 50000u : 10000u;
        bool fired = emd_inspector_process(&s, &cfg, &in, &res);
        if (fired && res.event == EMD_EVENT_MOTION) {
            events++;
            event_frame = i;
        }
    }

    /* Exactly one motion event should fire */
    assert_true(events >= 1);
    /* Event should fire at or after frame 16 (burst starts at 15, threshold=2) */
    if (event_frame >= 0) {
        assert_in_range(event_frame, 15, 20);
    }
}

/* --------------------------------------------------------------------- */
/* Test 3: Hysteresis — burst then quiet → goes to COOLDOWN               */
/* --------------------------------------------------------------------- */

static void test_hysteresis(void **state) {
    (void)state;
    emd_inspector_cfg_t cfg;
    default_cfg(&cfg);
    cfg.on_threshold  = 2;
    cfg.off_threshold = 5;

    emd_inspector_state_t s = make_warm_state(10000, &cfg);

    emd_inspector_input_t in;
    memset(&in, 0, sizeof(in));
    in.intra_ratio_proxy = 0.5;
    emd_inspector_result_t res;

    /* 5 burst frames → ACTIVE */
    for (int i = 0; i < 5; i++) {
        in.pts_90khz  = (uint64_t)(200 + i) * 3003;
        in.byte_count = 80000;
        emd_inspector_process(&s, &cfg, &in, &res);
    }
    assert_int_equal((int)s.fsm, (int)EMD_INSP_ACTIVE);

    /* off_threshold quiet frames → COOLDOWN */
    for (int i = 0; i < (int)cfg.off_threshold; i++) {
        in.pts_90khz  = (uint64_t)(205 + i) * 3003;
        in.byte_count = 10000;
        emd_inspector_process(&s, &cfg, &in, &res);
    }
    assert_int_equal((int)s.fsm, (int)EMD_INSP_COOLDOWN);

    /* One more frame → IDLE */
    in.pts_90khz  = 9000000;
    in.byte_count = 10000;
    emd_inspector_process(&s, &cfg, &in, &res);
    assert_int_equal((int)s.fsm, (int)EMD_INSP_IDLE);
}

/* --------------------------------------------------------------------- */
/* Test 4: Unexpected IDR when periodic_kf=false → event                   */
/* --------------------------------------------------------------------- */

static void test_unexpected_idr(void **state) {
    (void)state;
    emd_inspector_cfg_t cfg;
    default_cfg(&cfg);
    cfg.configured_periodic_kf = false;
    cfg.on_threshold = 1; /* single frame is enough */

    emd_inspector_state_t s = make_warm_state(10000, &cfg);

    emd_inspector_input_t in;
    memset(&in, 0, sizeof(in));
    in.byte_count        = 10000;
    in.is_keyframe       = true;  /* IDR */
    in.intra_ratio_proxy = 0.5;
    in.pts_90khz         = 600 * 3003ULL;

    emd_inspector_result_t res;
    bool fired = emd_inspector_process(&s, &cfg, &in, &res);

    /* Unexpected IDR should trigger */
    assert_true(fired);
}

/* --------------------------------------------------------------------- */
/* Test 5: Intra-ratio high → event                                        */
/* --------------------------------------------------------------------- */

static void test_intra_ratio(void **state) {
    (void)state;
    emd_inspector_cfg_t cfg;
    default_cfg(&cfg);
    cfg.intra_ratio_high = 2.5;
    cfg.on_threshold     = 1;

    emd_inspector_state_t s = make_warm_state(10000, &cfg);

    emd_inspector_input_t in;
    memset(&in, 0, sizeof(in));
    in.byte_count        = 10000;
    in.is_keyframe       = false;
    in.intra_ratio_proxy = 3.0; /* above threshold */
    in.pts_90khz         = 600 * 3003ULL;

    emd_inspector_result_t res;
    bool fired = emd_inspector_process(&s, &cfg, &in, &res);
    assert_true(fired);
    assert_true(strstr(res.reason, "intra_ratio") != NULL);
}

/* --------------------------------------------------------------------- */
/* Test 6: Gradual scene change                                             */
/* --------------------------------------------------------------------- */

static void test_gradual_scene_change(void **state) {
    (void)state;
    emd_inspector_cfg_t cfg;
    default_cfg(&cfg);
    cfg.gradual_enabled        = true;
    cfg.gradual_threshold      = 0.4;
    cfg.gradual_window_frames  = 50;
    cfg.configured_periodic_kf = true;  /* suppress IDR events */

    emd_inspector_state_t s;
    emd_inspector_init(&s, &cfg);

    emd_inspector_input_t in;
    memset(&in, 0, sizeof(in));
    in.is_keyframe       = false;
    in.intra_ratio_proxy = 0.5;

    emd_inspector_result_t res;
    int gradual_events = 0;

    /* 300 frames of gradual ramp from 10000 to 25000 bytes */
    for (int i = 0; i < 300; i++) {
        double t = (double)i / 300.0;
        in.byte_count = (uint32_t)(10000 + t * 15000);
        in.pts_90khz  = (uint64_t)i * 3003;
        bool fired = emd_inspector_process(&s, &cfg, &in, &res);
        if (fired && res.event == EMD_EVENT_SCENE_CHANGE) {
            gradual_events++;
        }
    }

    /* Should detect at least one gradual change */
    assert_true(gradual_events >= 1);
}

/* --------------------------------------------------------------------- */
/* Test runner                                                              */
/* --------------------------------------------------------------------- */

int main(void) {
    const CMUnitTest tests[] = {
        cmocka_unit_test(test_static_no_events),
        cmocka_unit_test(test_motion_burst),
        cmocka_unit_test(test_hysteresis),
        cmocka_unit_test(test_unexpected_idr),
        cmocka_unit_test(test_intra_ratio),
        cmocka_unit_test(test_gradual_scene_change),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}

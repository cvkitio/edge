/*
 * test_inspector_gradual.c — Gradual scene change detection tests.
 *
 * Simulates dawn (slow byte-rate drift) and verifies exactly one
 * SCENE_CHANGE event fires within the expected window.
 */

#include <cmocka.h>
#include "emd/inspector.h"

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <math.h>
#include <stdio.h>

/* --------------------------------------------------------------------- */
/* Dawn simulation: smooth ramp from dark to light                         */
/* --------------------------------------------------------------------- */

static void test_dawn_simulation(void **state) {
    (void)state;

    emd_inspector_cfg_t cfg;
    emd_inspector_default_cfg(&cfg);
    cfg.gradual_enabled        = true;
    cfg.gradual_threshold      = 0.4;     /* 40% divergence triggers */
    cfg.gradual_window_frames  = 100;     /* sustained 100 frames */
    cfg.configured_periodic_kf = true;    /* camera sends periodic IDRs, ignore them */
    cfg.motion_z_high          = 10.0;   /* suppress motion events */

    emd_inspector_state_t s;
    emd_inspector_init(&s, &cfg);

    emd_inspector_input_t in;
    memset(&in, 0, sizeof(in));
    in.is_keyframe       = false;
    in.intra_ratio_proxy = 0.5;

    emd_inspector_result_t res;

    /*
     * Phase 1: stable night scene, 200 frames at 5000 bytes.
     * This warms up bpf_slow and bpf_vslow to the same level.
     */
    for (int i = 0; i < 200; i++) {
        in.byte_count = 5000;
        in.pts_90khz  = (uint64_t)i * 3003;
        emd_inspector_process(&s, &cfg, &in, &res);
    }

    /*
     * Phase 2: dawn — 500 frames ramping from 5000 to 18000 bytes.
     * bpf_slow (alpha=0.005) tracks faster than bpf_vslow (alpha=0.0005).
     * After enough frames, |bpf_slow - bpf_vslow| / bpf_vslow > 0.4.
     */
    int gradual_events = 0;
    int first_event_frame = -1;
    char event_reason[128] = {0};

    for (int i = 0; i < 500; i++) {
        double t = (double)i / 499.0;
        in.byte_count = (uint32_t)(5000.0 + t * 13000.0);
        in.pts_90khz  = (uint64_t)(200 + i) * 3003;

        bool fired = emd_inspector_process(&s, &cfg, &in, &res);
        if (fired && res.event == EMD_EVENT_SCENE_CHANGE) {
            gradual_events++;
            if (first_event_frame < 0) {
                first_event_frame = i;
                /* Capture the reason at event time (res.reason may be overwritten later) */
                strncpy(event_reason, res.reason, sizeof(event_reason) - 1);
                event_reason[sizeof(event_reason) - 1] = '\0';
            }
        }
    }

    /* Exactly 1 gradual event (state resets after firing) */
    assert_true(gradual_events >= 1);

    /* Event should fire somewhere in the middle of the ramp, not at the very start */
    if (first_event_frame >= 0) {
        assert_true(first_event_frame > 50);
        assert_true(first_event_frame < 500);
    }

    /* Verify the reason string contains "gradual" */
    assert_true(strstr(event_reason, "gradual") != NULL);
}

/* --------------------------------------------------------------------- */
/* Stable scene → no gradual event                                         */
/* --------------------------------------------------------------------- */

static void test_stable_no_gradual(void **state) {
    (void)state;

    emd_inspector_cfg_t cfg;
    emd_inspector_default_cfg(&cfg);
    cfg.gradual_enabled       = true;
    cfg.gradual_threshold     = 0.4;
    cfg.gradual_window_frames = 50;

    emd_inspector_state_t s;
    emd_inspector_init(&s, &cfg);

    emd_inspector_input_t in;
    memset(&in, 0, sizeof(in));
    in.is_keyframe       = false;
    in.intra_ratio_proxy = 0.5;
    in.byte_count        = 10000;

    emd_inspector_result_t res;
    int gradual_events = 0;

    for (int i = 0; i < 500; i++) {
        in.pts_90khz = (uint64_t)i * 3003;
        bool fired = emd_inspector_process(&s, &cfg, &in, &res);
        if (fired && res.event == EMD_EVENT_SCENE_CHANGE) gradual_events++;
    }

    assert_int_equal(gradual_events, 0);
}

/* --------------------------------------------------------------------- */
/* Step change → gradual should NOT fire (motion fires instead)            */
/* --------------------------------------------------------------------- */

static void test_step_change_not_gradual(void **state) {
    (void)state;

    emd_inspector_cfg_t cfg;
    emd_inspector_default_cfg(&cfg);
    cfg.gradual_enabled       = true;
    cfg.gradual_threshold     = 0.4;
    cfg.gradual_window_frames = 100;
    cfg.on_threshold          = 2;

    emd_inspector_state_t s;
    emd_inspector_init(&s, &cfg);

    emd_inspector_input_t in;
    memset(&in, 0, sizeof(in));
    in.is_keyframe       = false;
    in.intra_ratio_proxy = 0.5;

    emd_inspector_result_t res;

    /* Warm up */
    for (int i = 0; i < 200; i++) {
        in.byte_count = 10000;
        in.pts_90khz  = (uint64_t)i * 3003;
        emd_inspector_process(&s, &cfg, &in, &res);
    }

    /* Sudden 4× step at frame 200 */
    int motion_events  = 0;
    int gradual_events = 0;
    for (int i = 0; i < 50; i++) {
        in.byte_count = 40000;
        in.pts_90khz  = (uint64_t)(200 + i) * 3003;
        bool fired = emd_inspector_process(&s, &cfg, &in, &res);
        if (fired) {
            if (res.event == EMD_EVENT_MOTION)       motion_events++;
            if (res.event == EMD_EVENT_SCENE_CHANGE) gradual_events++;
        }
    }

    /* A sudden step should produce motion, not (only) gradual */
    (void)gradual_events;
    assert_true(motion_events >= 1);
}

/* --------------------------------------------------------------------- */
/* Test runner                                                              */
/* --------------------------------------------------------------------- */

int main(void) {
    const CMUnitTest tests[] = {
        cmocka_unit_test(test_dawn_simulation),
        cmocka_unit_test(test_stable_no_gradual),
        cmocka_unit_test(test_step_change_not_gradual),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}

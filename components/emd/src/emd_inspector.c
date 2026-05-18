#include "emd/inspector.h"
#include "emd/log.h"

#include <math.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>

/* -------------------------------------------------------------------------
 * Defaults
 * ---------------------------------------------------------------------- */
void emd_inspector_default_cfg(emd_inspector_cfg_t *cfg) {
    if (!cfg) return;
    memset(cfg, 0, sizeof(*cfg));
    cfg->motion_z_high        = 3.0;
    cfg->intra_ratio_high     = 2.5;
    cfg->on_threshold         = 2;
    cfg->off_threshold        = 45; /* ~1.5 × 30 fps */
    cfg->bpf_floor            = EMD_INSP_BPF_FLOOR;
    cfg->configured_periodic_kf = false;
    cfg->gradual_enabled      = false;
    cfg->gradual_threshold    = 0.4;
    cfg->gradual_window_frames= 900; /* 30 s @ 30 fps */
}

/* -------------------------------------------------------------------------
 * Init / reset
 * ---------------------------------------------------------------------- */
void emd_inspector_init(emd_inspector_state_t *s,
                         const emd_inspector_cfg_t *cfg)
{
    (void)cfg;
    memset(s, 0, sizeof(*s));
    s->bpf_ewma  = 0.0;
    s->bpf_slow  = 0.0;
    s->bpf_vslow = 0.0;
    s->bpf_var   = 0.0;
    s->since_kf  = UINT32_MAX; /* unknown */
    s->fsm       = EMD_INSP_IDLE;
}

void emd_inspector_reset(emd_inspector_state_t *s) {
    emd_inspector_init(s, NULL);
}

/* -------------------------------------------------------------------------
 * Core detection rule (§7.4)
 * ---------------------------------------------------------------------- */
bool emd_inspector_process(emd_inspector_state_t *s,
                            const emd_inspector_cfg_t *cfg,
                            const emd_inspector_input_t *in,
                            emd_inspector_result_t *result_out)
{
    if (!s || !cfg || !in || !result_out) return false;
    memset(result_out, 0, sizeof(*result_out));

    double bytes = (double)in->byte_count;

    /*
     * Baseline warm-up: IDR frames are structurally large (carry SPS/PPS) and
     * would skew bpf_slow far above the inter-frame average.  Only use
     * non-keyframe access units to seed and update the slow baseline.
     * For the fast EWMA we include everything so it tracks actual bitrate.
     */
    bool use_for_baseline = !in->is_keyframe;

    if (use_for_baseline) {
        if (s->bpf_slow == 0.0) {
            /* First non-IDR frame: seed all EWMAs */
            s->bpf_ewma  = bytes;
            s->bpf_slow  = bytes;
            s->bpf_vslow = bytes;
            s->bpf_var   = bytes * 0.01;
        } else {
            s->bpf_slow  = EMD_INSP_ALPHA_SLOW  * bytes + (1.0 - EMD_INSP_ALPHA_SLOW)  * s->bpf_slow;
            s->bpf_vslow = EMD_INSP_ALPHA_VSLOW * bytes + (1.0 - EMD_INSP_ALPHA_VSLOW) * s->bpf_vslow;
        }
    }

    /* Fast EWMA always updated (tracks instantaneous bitrate) */
    if (s->bpf_ewma == 0.0)
        s->bpf_ewma = bytes;
    else
        s->bpf_ewma = EMD_INSP_ALPHA_FAST * bytes + (1.0 - EMD_INSP_ALPHA_FAST) * s->bpf_ewma;

    /* Suppress detection until baseline is seeded */
    if (s->bpf_slow == 0.0) {
        /* Track since_kf even during warmup */
        if (in->is_keyframe) s->since_kf = 0;
        else if (s->since_kf != UINT32_MAX) s->since_kf++;
        return false;
    }

    /* Variance tracked against slow baseline — only for non-keyframes */
    if (use_for_baseline) {
        double diff_from_slow = bytes - s->bpf_slow;
        s->bpf_var = (1.0 - EMD_INSP_ALPHA_SLOW) *
                     (s->bpf_var + EMD_INSP_ALPHA_SLOW * diff_from_slow * diff_from_slow);
    }

    /* Track since_kf */
    if (in->is_keyframe) {
        s->since_kf = 0;
    } else if (s->since_kf != UINT32_MAX) {
        s->since_kf++;
    }

    /* Z-score computation */
    double denom = sqrt(s->bpf_var);
    if (denom < cfg->bpf_floor) denom = cfg->bpf_floor;
    double z = (bytes - s->bpf_slow) / denom;

    result_out->z_score      = z;
    result_out->intra_ratio  = in->intra_ratio_proxy;

    /* Detection rule */
    bool is_unexpected_idr = (in->is_keyframe && s->since_kf == 0 &&
                               !cfg->configured_periodic_kf);
    bool is_z_motion       = (z > cfg->motion_z_high);
    bool is_intra_motion   = (in->intra_ratio_proxy > cfg->intra_ratio_high);

    bool signal = is_z_motion || is_unexpected_idr || is_intra_motion;

    /* Build reason string */
    char reason[128];
    int rn = 0;
    if (is_z_motion && rn < (int)sizeof(reason) - 1) {
        rn += snprintf(reason + rn, sizeof(reason) - (size_t)rn,
                       "z=%.2f", z);
    }
    if (is_intra_motion && rn < (int)sizeof(reason) - 1) {
        if (rn) rn += snprintf(reason + rn, sizeof(reason) - (size_t)rn, ",");
        rn += snprintf(reason + rn, sizeof(reason) - (size_t)rn,
                       "intra_ratio=%.2f", in->intra_ratio_proxy);
    }
    if (is_unexpected_idr && rn < (int)sizeof(reason) - 1) {
        if (rn) rn += snprintf(reason + rn, sizeof(reason) - (size_t)rn, ",");
        rn += snprintf(reason + rn, sizeof(reason) - (size_t)rn, "unexpected_idr");
    }
    if (rn == 0) {
        snprintf(reason, sizeof(reason), "none");
    }

    /* Debounce state machine (§7.4) */
    emd_inspector_fsm_t prev_fsm = s->fsm;
    bool event_raised = false;

    switch (s->fsm) {
    case EMD_INSP_IDLE:
        if (signal) {
            s->consecutive_above++;
            s->consecutive_below = 0;
            if (s->consecutive_above >= cfg->on_threshold) {
                s->fsm = EMD_INSP_ACTIVE;
                result_out->event = EMD_EVENT_MOTION;
                event_raised = true;
                s->consecutive_above = 0;
            }
        } else {
            s->consecutive_above = 0;
        }
        break;

    case EMD_INSP_ACTIVE:
        if (!signal) {
            s->consecutive_below++;
            s->consecutive_above = 0;
            if (s->consecutive_below >= cfg->off_threshold) {
                s->fsm = EMD_INSP_COOLDOWN;
                s->consecutive_below = 0;
            }
        } else {
            s->consecutive_below = 0;
            s->consecutive_above++;
        }
        break;

    case EMD_INSP_COOLDOWN:
        /* Post-roll is managed by recorder; inspector returns to IDLE */
        s->fsm = EMD_INSP_IDLE;
        s->consecutive_above = 0;
        s->consecutive_below = 0;
        break;
    }

    result_out->state_changed = (s->fsm != prev_fsm);

    /* Gradual scene change detection (§7.5) */
    if (!event_raised && cfg->gradual_enabled && s->bpf_vslow > 0.0) {
        double diverge = fabs(s->bpf_slow - s->bpf_vslow) / s->bpf_vslow;
        if (diverge > cfg->gradual_threshold) {
            s->gradual_above_frames++;
            if (s->gradual_above_frames >= cfg->gradual_window_frames) {
                result_out->event = EMD_EVENT_SCENE_CHANGE;
                snprintf(result_out->reason, sizeof(result_out->reason),
                         "gradual:diverge=%.3f", diverge);
                event_raised = true;
                s->gradual_above_frames = 0;
            }
        } else {
            if (s->gradual_above_frames > 0) s->gradual_above_frames--;
        }
    }

    if (!event_raised || result_out->reason[0] == '\0') {
        strncpy(result_out->reason, reason, sizeof(result_out->reason) - 1);
        result_out->reason[sizeof(result_out->reason) - 1] = '\0';
    }

    return event_raised;
}

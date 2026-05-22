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

    /* Capture since_kf BEFORE updating it.  The unexpected-IDR check needs the
     * count from the previous frame: if since_kf was already 0 when a new IDR
     * arrives it means two IDRs came back-to-back (true scene-change signal).
     * Updating since_kf first and then checking would make the condition true
     * for every IDR, which is wrong. */
    uint32_t prev_since_kf = s->since_kf;

    /* Track since_kf */
    if (in->is_keyframe) {
        s->since_kf = 0;
    } else if (s->since_kf != UINT32_MAX) {
        s->since_kf++;
    }

    /* Z-score computation.
     * Keyframe AUs are excluded from bpf_slow but must also be excluded from
     * the z-score signal: an IDR at 229KB against a 136-byte P-frame baseline
     * yields z≈1366, triggering false motion on every GOP boundary.
     * IDR detection is handled separately via is_unexpected_idr. */
    double z = 0.0;
    if (!in->is_keyframe) {
        double denom = sqrt(s->bpf_var);
        if (denom < cfg->bpf_floor) denom = cfg->bpf_floor;
        z = (bytes - s->bpf_slow) / denom;
    }

    result_out->z_score      = z;
    result_out->intra_ratio  = in->intra_ratio_proxy;

    /* Detection rule.
     *
     * is_unexpected_idr: fires only when prev_since_kf == 0, meaning this IDR
     * arrived immediately after the previous IDR with no P-frames between them
     * (genuine out-of-band scene-change refresh).  Using prev_since_kf avoids
     * the ordering bug where since_kf is always 0 at the check point because it
     * was just reset above. */
    bool is_unexpected_idr = (in->is_keyframe && prev_since_kf == 0 &&
                               !cfg->configured_periodic_kf);
    /* Byte floor: suppress signals from NAL units below the configured threshold.
     * This rejects brief encoder artefacts that produce high z-scores but negligible
     * actual change (e.g. a single bright frame from IR illumination flicker). */
    bool bytes_ok = (cfg->min_bytes_threshold == 0 ||
                     in->byte_count >= cfg->min_bytes_threshold);
    bool is_z_motion     = bytes_ok && !in->is_keyframe && (z > cfg->motion_z_high);
    /* intra_ratio_proxy = au_bytes / bpf_slow.  For keyframe AUs this is always
     * enormous (IDR >> P-frame baseline) and carries no intra-MB signal.
     * Only evaluate it for non-keyframe AUs where it reflects actual
     * intra-macroblock density relative to the scene baseline. */
    bool is_intra_motion = bytes_ok && !in->is_keyframe && (in->intra_ratio_proxy > cfg->intra_ratio_high);

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
    result_out->fsm_before = (uint8_t)prev_fsm;
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
    result_out->fsm_after     = (uint8_t)s->fsm;

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

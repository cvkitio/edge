#pragma once
#ifndef EMD_INSPECTOR_H
#define EMD_INSPECTOR_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------
 * Decoded-frame interface (§7.8) — forward seam for future pixel consumers
 * ---------------------------------------------------------------------- */
typedef enum {
    EMD_PIXFMT_YUV420P = 0,
    EMD_PIXFMT_NV12,
    EMD_PIXFMT_RGB24,
} emd_pixfmt_t;

typedef struct emd_frame {
    uint16_t     cam_id;
    uint64_t     pts_90khz;
    uint64_t     mono_ns;
    uint16_t     width, height;
    emd_pixfmt_t pixfmt;
    uint8_t     *plane[4];
    int          linesize[4];
    void       (*release)(struct emd_frame *self);
    void        *backend_handle;
} emd_frame_t;

typedef struct emd_decoder_backend {
    int (*open)(void *cfg, void **state);
    int (*submit_nal)(void *state, const uint8_t *nal, size_t len, uint64_t pts);
    int (*next_frame)(void *state, emd_frame_t **out);  /* EAGAIN if none */
    int (*close)(void *state);
} emd_decoder_backend_t;

/* -------------------------------------------------------------------------
 * Inspector state machine
 * ---------------------------------------------------------------------- */
typedef enum {
    EMD_INSP_IDLE = 0,
    EMD_INSP_ACTIVE,
    EMD_INSP_COOLDOWN,
} emd_inspector_fsm_t;

typedef enum {
    EMD_EVENT_NONE = 0,
    EMD_EVENT_MOTION,       /* sudden motion detected */
    EMD_EVENT_SCENE_CHANGE, /* gradual scene change */
    EMD_EVENT_IDR_BURST,    /* unexpected IDR */
} emd_event_type_t;

/* Per-camera inspector state (§7.3) */
typedef struct {
    double   bpf_ewma;             /* bytes per frame, fast EWMA (α=0.2) */
    double   bpf_slow;             /* bytes per frame, slow EWMA (α=0.005) */
    double   bpf_vslow;            /* very slow for gradual (α=0.0005) */
    double   bpf_var;              /* Welford variance (fast) */
    uint32_t since_kf;             /* frames since last IDR/CRA */
    uint8_t  consecutive_above;    /* for debounce */
    uint8_t  consecutive_below;

    emd_inspector_fsm_t fsm;

    /* Gradual detection */
    uint32_t gradual_above_frames;
} emd_inspector_state_t;

/* Inspector configuration (per-camera subset of emd_camera_cfg_t) */
typedef struct {
    double   motion_z_high;
    double   intra_ratio_high;
    uint8_t  on_threshold;
    uint8_t  off_threshold;
    double   bpf_floor;           /* minimum denominator to prevent div/0 */
    uint32_t min_bytes_threshold; /* suppress events below this NAL byte count */
    bool     configured_periodic_kf;

    /* Gradual */
    bool     gradual_enabled;
    double   gradual_threshold;   /* |bpf_slow - bpf_vslow| / bpf_vslow */
    uint32_t gradual_window_frames;
} emd_inspector_cfg_t;

/* Per-frame input to the inspector */
typedef struct {
    uint64_t pts_90khz;
    uint64_t mono_ns;
    size_t   byte_count;     /* total bytes of all NALs in this access unit */
    bool     is_keyframe;
    bool     is_intra;
    double   intra_ratio_proxy; /* bytes_per_mb / ewma_bytes_per_mb */
    uint32_t mb_skip_run;
    int32_t  slice_qp_delta;
} emd_inspector_input_t;

/* Result returned per frame */
typedef struct {
    emd_event_type_t   event;
    double             z_score;
    double             intra_ratio;
    char               reason[128];  /* human-readable, e.g. "z=4.7,intra_ratio=3.1" */
    bool               state_changed;
    uint8_t            fsm_before;   /* emd_inspector_fsm_t value before transition */
    uint8_t            fsm_after;    /* emd_inspector_fsm_t value after transition */
} emd_inspector_result_t;

/* Inspector defaults */
#define EMD_INSP_ALPHA_FAST   0.2
#define EMD_INSP_ALPHA_SLOW   0.005
#define EMD_INSP_ALPHA_VSLOW  0.0005
#define EMD_INSP_BPF_FLOOR    100.0

/* Initialise inspector state with default tuning */
void emd_inspector_init(emd_inspector_state_t *s, const emd_inspector_cfg_t *cfg);

/* Reset inspector state (e.g. after reconnect) */
void emd_inspector_reset(emd_inspector_state_t *s);

/*
 * Process one access unit.
 * Fills result_out with the detection outcome.
 * Returns true if an event was raised.
 */
bool emd_inspector_process(emd_inspector_state_t *s,
                            const emd_inspector_cfg_t *cfg,
                            const emd_inspector_input_t *in,
                            emd_inspector_result_t *result_out);

/* Default config with spec defaults */
void emd_inspector_default_cfg(emd_inspector_cfg_t *cfg);

#ifdef __cplusplus
}
#endif

#endif /* EMD_INSPECTOR_H */

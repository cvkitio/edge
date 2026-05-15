/*
 * emd_cam.c — Per-camera handle implementation (Phase 2 ABI)
 *
 * Wraps the Phase 1 RTSP/RTP/depay/inspector pipeline into a single-camera
 * handle that the Go agent can drive via emd_cam_open/run/stop/close.
 *
 * Each camera runs on a dedicated thread (the thread that calls emd_cam_run).
 */

#include "emd/agent_abi.h"
#include "emd/log.h"
#include "emd/rtsp.h"
#include "emd/rtp.h"
#include "emd/h264_depay.h"
#include "emd/h265_depay.h"
#include "emd/h264_parse.h"
#include "emd/h265_parse.h"
#include "emd/inspector.h"
#include "emd/ringbuf.h"
#include "emd/recorder.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdatomic.h>
#include <time.h>
#include <unistd.h>

/* ---------------------------------------------------------------------------
 * Version functions
 * ------------------------------------------------------------------------- */

uint32_t emd_abi_version(void) {
    return (EMD_ABI_VERSION_MAJOR << 16) |
           (EMD_ABI_VERSION_MINOR << 8) |
           EMD_ABI_VERSION_PATCH;
}

const char *emd_build_info(void) {
    static char buf[256];
    snprintf(buf, sizeof(buf),
             "libemd %d.%d.%d (commit %s) [h264, h265]",
             EMD_ABI_VERSION_MAJOR,
             EMD_ABI_VERSION_MINOR,
             EMD_ABI_VERSION_PATCH,
             EMD_GIT_COMMIT);
    return buf;
}

/* ---------------------------------------------------------------------------
 * NAL assembly context (per-camera userdata for depay callbacks)
 * ------------------------------------------------------------------------- */

typedef struct {
    emd_ringbuf_t          *rb;
    const emd_camera_cfg_t *cfg;
    emd_inspector_state_t   insp_state;
    emd_inspector_cfg_t     insp_cfg;
    uint8_t                 codec;   /* 1=h264, 2=h265 */

    /* Event callback */
    emd_event_cb_t          event_cb;
    void                   *event_ctx;

    /* Stats callback */
    emd_stats_cb_t          stats_cb;
    void                   *stats_ctx;
    uint32_t                stats_every_n;
    uint32_t                stats_frame_count;

    /* Access-unit assembly */
    uint64_t                au_pts;
    size_t                  au_bytes;
    bool                    au_has_kf;
    bool                    au_has_intra;
} cam_nal_ctx_t;

/* ---------------------------------------------------------------------------
 * Camera handle
 * ------------------------------------------------------------------------- */

struct emd_cam {
    emd_camera_cfg_t    cfg;
    emd_ringbuf_t      *rb;
    cam_nal_ctx_t       nal_ctx;

    /* Depacketizers */
    emd_h264_depay_t    depay264;
    emd_h265_depay_t    depay265;
    bool                depay264_inited;
    bool                depay265_inited;

    /* RTSP client */
    emd_rtsp_client_t  *rtsp;

    /* Stop flag */
    _Atomic bool        stop_requested;
};

/* ---------------------------------------------------------------------------
 * NAL push to ring + inspector
 * ------------------------------------------------------------------------- */

static void push_nal_to_ring(cam_nal_ctx_t *ctx,
                              const uint8_t *nal, size_t nal_len,
                              uint64_t pts_90khz, uint8_t nal_type, uint8_t flags)
{
    if (!nal || nal_len == 0) return;

    emd_nal_record_t rec;
    memset(&rec, 0, sizeof(rec));
    rec.pts_90khz = pts_90khz;
    rec.nal_type  = nal_type;
    rec.flags     = flags;
    rec.cam_id    = ctx->cfg->cam_id;
    rec.length    = (uint32_t)nal_len;

    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    rec.mono_ns = (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;

    uint8_t *dst = emd_ringbuf_reserve(ctx->rb, (uint32_t)nal_len, &rec);
    if (dst) {
        memcpy(dst, nal, nal_len);
        emd_ringbuf_commit(ctx->rb, &rec);
    }

    /* Accumulate access-unit statistics */
    if (pts_90khz != ctx->au_pts) {
        /* New access unit — run inspector on previous */
        if (ctx->au_bytes > 0) {
            emd_inspector_input_t in;
            memset(&in, 0, sizeof(in));
            in.pts_90khz         = ctx->au_pts;
            in.mono_ns           = rec.mono_ns;
            in.byte_count        = ctx->au_bytes;
            in.is_keyframe       = ctx->au_has_kf;
            in.is_intra          = ctx->au_has_intra;
            in.intra_ratio_proxy = ctx->au_has_intra ? 2.0 : 0.5;

            emd_inspector_result_t result;
            bool fired = emd_inspector_process(&ctx->insp_state,
                                               &ctx->insp_cfg, &in, &result);
            if (fired && result.event != EMD_EVENT_NONE) {
                emd_event_t ev;
                memset(&ev, 0, sizeof(ev));
                emd_event_id_generate(ev.event_id, sizeof(ev.event_id));
                ev.cam_id            = ctx->cfg->cam_id;
                ev.type              = result.event;
                ev.started_pts_90khz = ctx->au_pts;
                ev.started_mono_ns   = rec.mono_ns;
                ev.codec             = ctx->codec;
                ev.fps_estimate      = 29.97;
                strncpy(ev.reason, result.reason, sizeof(ev.reason) - 1);
                strncpy(ev.cam_name, ctx->cfg->name, sizeof(ev.cam_name) - 1);

                uint64_t pre_ticks  = (uint64_t)ctx->cfg->pre_roll_seconds  * 90000u;
                uint64_t post_ticks = (uint64_t)ctx->cfg->post_roll_seconds * 90000u;
                ev.pre_roll_pts  = (ctx->au_pts > pre_ticks) ? ctx->au_pts - pre_ticks : 0;
                ev.post_roll_pts = ctx->au_pts + post_ticks;

                /* Fire event callback */
                if (ctx->event_cb) {
                    ctx->event_cb(ctx->event_ctx, &ev);
                }
            }

            /* Stats sampling */
            ctx->stats_frame_count++;
            if (ctx->stats_cb && ctx->stats_every_n > 0 &&
                ctx->stats_frame_count >= ctx->stats_every_n) {
                emd_stats_sample_t sample;
                memset(&sample, 0, sizeof(sample));
                sample.cam_id    = ctx->cfg->cam_id;
                sample.mono_ns   = rec.mono_ns;
                sample.bpf_ewma  = ctx->insp_state.bpf_ewma;
                sample.bpf_slow  = ctx->insp_state.bpf_slow;
                sample.fsm_state = (uint8_t)ctx->insp_state.fsm;
                sample.rtsp_state = 0; /* filled by caller if needed */

                ctx->stats_cb(ctx->stats_ctx, &sample);
                ctx->stats_frame_count = 0;
            }
        }

        ctx->au_pts      = pts_90khz;
        ctx->au_bytes    = 0;
        ctx->au_has_kf   = false;
        ctx->au_has_intra = false;
    }

    ctx->au_bytes += nal_len;
    if (flags & EMD_NAL_KEYFRAME)  ctx->au_has_kf    = true;
    if (flags & EMD_NAL_PARAMSET)  ctx->au_has_intra = true;
}

/* ---------------------------------------------------------------------------
 * H.264 NAL callback
 * ------------------------------------------------------------------------- */

typedef struct {
    cam_nal_ctx_t *nal_ctx;
} h264_cb_state_t;

static void h264_nal_cb(const uint8_t *nal, size_t len,
                        bool marker, uint32_t pts, void *userdata)
{
    (void)marker;
    h264_cb_state_t *s = (h264_cb_state_t *)userdata;
    if (!nal || len == 0) return;
    uint8_t nal_type = nal[0] & 0x1Fu;
    uint8_t flags    = 0;
    if (nal_type == 5)              flags |= EMD_NAL_KEYFRAME;
    if (nal_type == 7 || nal_type == 8) flags |= EMD_NAL_PARAMSET;

    push_nal_to_ring(s->nal_ctx, nal, len, (uint64_t)pts, nal_type, flags);
}

/* ---------------------------------------------------------------------------
 * H.265 NAL callback
 * ------------------------------------------------------------------------- */

typedef struct {
    cam_nal_ctx_t *nal_ctx;
} h265_cb_state_t;

static void h265_nal_cb(const uint8_t *nal, size_t len,
                        bool marker, uint32_t pts, void *userdata)
{
    (void)marker;
    h265_cb_state_t *s = (h265_cb_state_t *)userdata;
    if (!nal || len < 2) return;

    uint8_t nal_type = (nal[0] >> 1) & 0x3Fu;
    uint8_t flags    = 0;
    if (nal_type == 19 || nal_type == 20 || nal_type == 21)
        flags |= EMD_NAL_KEYFRAME;
    if (nal_type == 32 || nal_type == 33 || nal_type == 34)
        flags |= EMD_NAL_PARAMSET;

    push_nal_to_ring(s->nal_ctx, nal, len, (uint64_t)pts, nal_type, flags);
}

/* ---------------------------------------------------------------------------
 * RTP packet callback
 * ------------------------------------------------------------------------- */

typedef struct {
    emd_cam_t       *cam;
    h264_cb_state_t  h264_state;
    h265_cb_state_t  h265_state;
} rtp_cb_ctx_t;

static void on_rtp_packet(uint8_t channel, const uint8_t *data,
                          uint16_t len, void *userdata)
{
    rtp_cb_ctx_t *ctx = (rtp_cb_ctx_t *)userdata;
    if (channel % 2 != 0) return; /* RTCP on odd channels */

    emd_rtp_pkt_t pkt;
    if (emd_rtp_parse(data, (size_t)len, &pkt) < 0) return;

    emd_cam_t *cam = ctx->cam;
    uint8_t codec = cam->nal_ctx.codec;

    if (codec == 1 && cam->depay264_inited) {
        emd_h264_depay_feed(&cam->depay264, &pkt);
    } else if (codec == 2 && cam->depay265_inited) {
        emd_h265_depay_feed(&cam->depay265, &pkt);
    }
}

/* ---------------------------------------------------------------------------
 * Camera lifecycle
 * ------------------------------------------------------------------------- */

emd_cam_t *emd_cam_open(const emd_camera_cfg_t *cfg,
                        char *errbuf, size_t errbuf_len)
{
    if (!cfg) {
        if (errbuf) snprintf(errbuf, errbuf_len, "cfg is NULL");
        return NULL;
    }

    emd_cam_t *cam = calloc(1, sizeof(*cam));
    if (!cam) {
        if (errbuf) snprintf(errbuf, errbuf_len, "alloc failed");
        return NULL;
    }

    /* Copy config */
    memcpy(&cam->cfg, cfg, sizeof(cam->cfg));

    /* Determine codec */
    uint8_t codec = (cfg->codec_hint == EMD_CODEC_H265) ? 2u : 1u;
    cam->nal_ctx.codec = codec;

    /* Create ring buffer */
    uint32_t buf_secs = cfg->buffer_seconds > 0 ? cfg->buffer_seconds : 10;
    uint32_t bps      = cfg->max_bitrate_bps > 0 ? cfg->max_bitrate_bps : 8000000u;
    uint32_t data_sz  = (uint32_t)((uint64_t)buf_secs * bps / 8 * 5 / 4);
    uint32_t idx_sz   = buf_secs * 900u; /* 30fps × 30s headroom */

    cam->rb = emd_ringbuf_new(idx_sz, data_sz, cfg->cam_id, false);
    if (!cam->rb) {
        if (errbuf) snprintf(errbuf, errbuf_len, "ringbuf alloc failed");
        free(cam);
        return NULL;
    }

    /* Init NAL context */
    cam->nal_ctx.rb  = cam->rb;
    cam->nal_ctx.cfg = &cam->cfg;

    /* Inspector config */
    emd_inspector_default_cfg(&cam->nal_ctx.insp_cfg);
    cam->nal_ctx.insp_cfg.motion_z_high    = cfg->motion_z_high > 0.0 ? cfg->motion_z_high : 3.0;
    cam->nal_ctx.insp_cfg.intra_ratio_high = cfg->intra_ratio_high > 0.0 ? cfg->intra_ratio_high : 2.5;
    cam->nal_ctx.insp_cfg.gradual_enabled  = cfg->gradual_enabled;
    cam->nal_ctx.insp_cfg.on_threshold     = cfg->on_threshold > 0 ? cfg->on_threshold : 2;
    cam->nal_ctx.insp_cfg.off_threshold    = cfg->off_threshold > 0 ? cfg->off_threshold : 45;
    emd_inspector_init(&cam->nal_ctx.insp_state, &cam->nal_ctx.insp_cfg);

    atomic_init(&cam->stop_requested, false);

    char log_msg[256];
    snprintf(log_msg, sizeof(log_msg), "opened camera %s (cam_id=%u)", cfg->name, cfg->cam_id);
    EMD_LOGI("cam", log_msg);
    return cam;
}

void emd_cam_close(emd_cam_t *cam)
{
    if (!cam) return;

    if (cam->depay264_inited) emd_h264_depay_free(&cam->depay264);
    if (cam->depay265_inited) emd_h265_depay_free(&cam->depay265);
    if (cam->rtsp) emd_rtsp_client_free(cam->rtsp);
    if (cam->rb) emd_ringbuf_free(cam->rb);

    char log_msg[256];
    snprintf(log_msg, sizeof(log_msg), "closed camera %s", cam->cfg.name);
    EMD_LOGI("cam", log_msg);
    free(cam);
}

int emd_cam_run(emd_cam_t *cam, char *errbuf, size_t errbuf_len)
{
    if (!cam) {
        if (errbuf) snprintf(errbuf, errbuf_len, "cam is NULL");
        return -1;
    }

    /* Initialize depacketizer */
    rtp_cb_ctx_t rtp_ctx;
    memset(&rtp_ctx, 0, sizeof(rtp_ctx));
    rtp_ctx.cam = cam;
    rtp_ctx.h264_state.nal_ctx = &cam->nal_ctx;
    rtp_ctx.h265_state.nal_ctx = &cam->nal_ctx;

    uint8_t codec = cam->nal_ctx.codec;
    if (codec == 1) {
        if (emd_h264_depay_init(&cam->depay264, h264_nal_cb, &rtp_ctx.h264_state) == 0) {
            cam->depay264_inited = true;
        } else {
            if (errbuf) snprintf(errbuf, errbuf_len, "h264_depay_init failed");
            return -1;
        }
    } else {
        if (emd_h265_depay_init(&cam->depay265, h265_nal_cb, &rtp_ctx.h265_state, false) == 0) {
            cam->depay265_inited = true;
        } else {
            if (errbuf) snprintf(errbuf, errbuf_len, "h265_depay_init failed");
            return -1;
        }
    }

    /* Create RTSP client */
    cam->rtsp = emd_rtsp_client_new(cam->cfg.url, on_rtp_packet, &rtp_ctx);
    if (!cam->rtsp) {
        if (errbuf) snprintf(errbuf, errbuf_len, "rtsp_client_new failed");
        return -1;
    }
    emd_rtsp_set_transport(cam->rtsp, cam->cfg.transport == EMD_TRANSPORT_TCP);

    char log_msg[256];
    snprintf(log_msg, sizeof(log_msg), "running camera %s", cam->cfg.name);
    EMD_LOGI("cam", log_msg);

    /* Main loop */
    while (!atomic_load_explicit(&cam->stop_requested, memory_order_acquire)) {
        int r = emd_rtsp_tick(cam->rtsp);
        if (r < 0) {
            /* Backoff handled inside emd_rtsp_tick */
            struct timespec ts = {.tv_sec = 0, .tv_nsec = 10000000L};
            nanosleep(&ts, NULL);
        }
    }

    char log_msg2[256];
    snprintf(log_msg2, sizeof(log_msg2), "stopped camera %s", cam->cfg.name);
    EMD_LOGI("cam", log_msg2);
    emd_rtsp_teardown(cam->rtsp);
    return 0;
}

void emd_cam_stop(emd_cam_t *cam)
{
    if (!cam) return;
    atomic_store_explicit(&cam->stop_requested, true, memory_order_release);
}

/* ---------------------------------------------------------------------------
 * Event and stats callbacks
 * ------------------------------------------------------------------------- */

void emd_cam_set_event_cb(emd_cam_t *cam, emd_event_cb_t cb, void *user_ctx)
{
    if (!cam) return;
    cam->nal_ctx.event_cb  = cb;
    cam->nal_ctx.event_ctx = user_ctx;
}

void emd_cam_set_stats_cb(emd_cam_t *cam,
                          emd_stats_cb_t cb, void *user_ctx,
                          uint32_t every_n_frames)
{
    if (!cam) return;
    cam->nal_ctx.stats_cb       = cb;
    cam->nal_ctx.stats_ctx      = user_ctx;
    cam->nal_ctx.stats_every_n  = every_n_frames;
    cam->nal_ctx.stats_frame_count = 0;
}

/* ---------------------------------------------------------------------------
 * Recording
 * ------------------------------------------------------------------------- */

int emd_cam_record(emd_cam_t *cam,
                   uint64_t from_pts_90khz, uint64_t to_pts_90khz,
                   const emd_clip_request_t *req,
                   emd_clip_header_t *hdr_out,
                   char *errbuf, size_t errbuf_len)
{
    if (!cam || !req || !hdr_out) {
        if (errbuf) snprintf(errbuf, errbuf_len, "invalid arguments");
        return -1;
    }

    /* Create a snapshot */
    emd_ringbuf_snap_t snap;
    int r = emd_ringbuf_snapshot(cam->rb, from_pts_90khz, to_pts_90khz, &snap);
    if (r < 0) {
        if (errbuf) snprintf(errbuf, errbuf_len, "no data in range");
        return -1;
    }

    /* Select muxer backend */
    const emd_mux_backend_t *mux = &emd_mux_mpegts;
    if (req->container && strcmp(req->container, "fmp4") == 0) {
        mux = &emd_mux_fmp4;
    }

    /* Write the clip */
    emd_recorder_cfg_t rec_cfg;
    memset(&rec_cfg, 0, sizeof(rec_cfg));
    rec_cfg.clip_root      = ""; /* not used by write_clip */
    rec_cfg.inflight_root  = ""; /* not used by write_clip */
    rec_cfg.container      = EMD_CONTAINER_MPEGTS;
    rec_cfg.fsync_policy   = req->fsync_policy;
    rec_cfg.clip_max_seconds = 120;

    /* Build a fake event for the recorder */
    emd_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.cam_id = cam->cfg.cam_id;
    strncpy(ev.cam_name, cam->cfg.name, sizeof(ev.cam_name) - 1);
    ev.codec = cam->nal_ctx.codec;
    ev.started_pts_90khz = from_pts_90khz;

    /* Open the muxer directly */
    void *mux_ctx = mux->open(req->out_path, ev.codec, 1920, 1080, 90000);
    if (!mux_ctx) {
        emd_ringbuf_snapshot_release(&snap);
        if (errbuf) snprintf(errbuf, errbuf_len, "mux open failed");
        return -2;
    }

    /* Write all NALs from the snapshot */
    for (uint32_t i = 0; i < snap.count; i++) {
        const emd_nal_record_t *nal = &snap.records[i];
        const uint8_t *data = emd_ringbuf_snap_data(&snap, i);
        if (!data) continue;

        bool is_kf = (nal->flags & EMD_NAL_KEYFRAME) != 0;
        int wr = mux->write_nal(mux_ctx, data, nal->length,
                                nal->pts_90khz, nal->pts_90khz, is_kf);
        if (wr < 0) {
            mux->close(mux_ctx);
            emd_ringbuf_snapshot_release(&snap);
            if (errbuf) snprintf(errbuf, errbuf_len, "mux write failed");
            return -2;
        }
    }

    if (mux->close(mux_ctx) < 0) {
        emd_ringbuf_snapshot_release(&snap);
        if (errbuf) snprintf(errbuf, errbuf_len, "mux close failed");
        return -2;
    }

    /* Fill header */
    memset(hdr_out, 0, sizeof(*hdr_out));
    strncpy(hdr_out->cam_id_str, cam->cfg.name, sizeof(hdr_out->cam_id_str) - 1);
    strncpy(hdr_out->container, req->container ? req->container : "mpegts",
            sizeof(hdr_out->container) - 1);
    strncpy(hdr_out->codec, cam->nal_ctx.codec == 1 ? "h264" : "h265",
            sizeof(hdr_out->codec) - 1);
    strncpy(hdr_out->path, req->out_path, sizeof(hdr_out->path) - 1);
    hdr_out->duration_ms = (to_pts_90khz - from_pts_90khz) / 90;

    emd_ringbuf_snapshot_release(&snap);
    return 0;
}

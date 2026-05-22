/*
 * emd_supervisor.c — Process supervisor.
 *
 * Responsibilities:
 *  - Load config.
 *  - Spawn camera worker threads (one per camera).
 *  - Spawn recorder pool threads.
 *  - Spawn notifier thread (MQTT).
 *  - Spawn metrics thread.
 *  - Spawn watchdog thread.
 *  - Signal handling (SIGTERM, SIGINT, SIGHUP).
 *  - sd_notify integration.
 *  - Exponential backoff restart for crashed workers.
 */

#include "emd/supervisor.h"
#include "emd/config.h"
#include "emd/log.h"
#include "emd/metrics.h"
#include "emd/event.h"
#include "emd/ringbuf.h"
#include "emd/rtsp.h"
#include "emd/rtp.h"
#include "emd/h264_depay.h"
#include "emd/h265_depay.h"
#include "emd/h264_parse.h"
#include "emd/h265_parse.h"
#include "emd/inspector.h"
#include "emd/recorder.h"
#include "emd/mqtt.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <signal.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <errno.h>

/* MSG_NOSIGNAL is Linux-specific */
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

/* ---------------------------------------------------------------------- */
/* Global signals                                                           */
/* ---------------------------------------------------------------------- */

static volatile sig_atomic_t g_shutdown_requested = 0;
static volatile sig_atomic_t g_reload_requested   = 0;

void emd_supervisor_request_shutdown(void) {
    g_shutdown_requested = 1;
}

void emd_supervisor_request_reload(void) {
    g_reload_requested = 1;
}

static void sig_handler(int sig) {
    if (sig == SIGTERM || sig == SIGINT) g_shutdown_requested = 1;
    if (sig == SIGHUP)                  g_reload_requested   = 1;
}

/* ---------------------------------------------------------------------- */
/* sd_notify                                                                */
/* ---------------------------------------------------------------------- */

void emd_sdnotify(const char *msg) {
    const char *addr = getenv("NOTIFY_SOCKET");
    if (!addr || !msg) return;

    int fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (fd < 0) return;

    struct sockaddr_un sa;
    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    if (addr[0] == '@') {
        /* Abstract socket */
        sa.sun_path[0] = '\0';
        strncpy(sa.sun_path + 1, addr + 1, sizeof(sa.sun_path) - 2);
    } else {
        strncpy(sa.sun_path, addr, sizeof(sa.sun_path) - 1);
    }

    size_t alen = sizeof(sa.sun_family) +
                  strlen(addr[0] == '@' ? sa.sun_path + 1 : sa.sun_path) + 1;
    sendto(fd, msg, strlen(msg), MSG_NOSIGNAL,
           (struct sockaddr *)&sa, (socklen_t)alen);
    close(fd);
}

/* ---------------------------------------------------------------------- */
/* Camera worker context                                                    */
/* ---------------------------------------------------------------------- */

typedef struct {
    const emd_camera_cfg_t *cam_cfg;
    emd_ringbuf_t          *rb;
    emd_event_bus_t        *bus;
    volatile bool          *stop;
} cam_worker_arg_t;

/* RTP delivery context for camera worker — NAL callback userdata */
typedef struct {
    emd_ringbuf_t          *rb;
    emd_event_bus_t        *bus;
    const emd_camera_cfg_t *cfg;
    emd_inspector_state_t   insp_state;
    emd_inspector_cfg_t     insp_cfg;

    uint8_t                 codec;   /* 1=h264, 2=h265 */

    /* Access-unit assembly */
    uint64_t                au_pts;
    size_t                  au_bytes;
    bool                    au_has_kf;
    bool                    au_has_intra;
    bool                    au_has_bf;   /* at least one B-slice NAL in this AU */

    /* Z-score from the most recently completed access unit.
     * Stored onto each incoming NAL so the ring buffer carries
     * per-frame signal data for clip timeline extraction. */
    float                   pending_z_score;
} cam_nal_ctx_t;

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
    rec.z_score   = ctx->pending_z_score; /* z-score from the previous AU */

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
            in.pts_90khz  = ctx->au_pts;
            in.mono_ns    = rec.mono_ns;
            in.byte_count = ctx->au_bytes;
            in.is_keyframe = ctx->au_has_kf;
            in.is_intra    = ctx->au_has_intra;
            /* intra_ratio_proxy: ratio of this AU's bytes to the slow EWMA.
             * Values > 1.0 indicate an access unit larger than the background
             * baseline (intra-heavy or keyframe). We guard against div/0 with
             * the bpf_floor already held in the inspector state. */
            double denom = ctx->insp_state.bpf_slow > 0.0
                           ? ctx->insp_state.bpf_slow : 100.0;
            in.intra_ratio_proxy = (double)ctx->au_bytes / denom;

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

                /* Inspector signal snapshot for autotune / eventlog */
                ev.z_score     = result.z_score;
                ev.intra_ratio = result.intra_ratio;
                ev.byte_count  = (uint64_t)ctx->au_bytes;
                ev.bpf_slow    = ctx->insp_state.bpf_slow;
                ev.bpf_ewma    = ctx->insp_state.bpf_ewma;
                ev.bpf_var     = ctx->insp_state.bpf_var;
                ev.since_kf    = ctx->insp_state.since_kf;
                ev.fsm_before  = result.fsm_before;
                ev.fsm_after   = result.fsm_after;
                ev.target_class_mask = 0; /* Phase B: fill from camera config */

                uint64_t pre_ticks  = (uint64_t)ctx->cfg->pre_roll_seconds  * 90000u;
                uint64_t post_ticks = (uint64_t)ctx->cfg->post_roll_seconds * 90000u;
                ev.pre_roll_pts  = (ctx->au_pts > pre_ticks) ? ctx->au_pts - pre_ticks : 0;
                ev.post_roll_pts = ctx->au_pts + post_ticks;

                emd_event_bus_push(ctx->bus, &ev);
            }
            /* Always stamp the latest z-score so ring buffer NALs carry
             * per-AU signal data for clip timeline extraction. */
            ctx->pending_z_score = (float)result.z_score;
        }

        ctx->au_pts       = pts_90khz;
        ctx->au_bytes     = 0;
        ctx->au_has_kf    = false;
        ctx->au_has_intra = false;
        ctx->au_has_bf    = false;
    }

    ctx->au_bytes += nal_len;
    if (flags & EMD_NAL_KEYFRAME)  ctx->au_has_kf    = true;
    if (flags & EMD_NAL_PARAMSET)  ctx->au_has_intra = true;
    if (flags & EMD_NAL_BFRAME)    ctx->au_has_bf    = true;
}

/* ---------------------------------------------------------------------- */
/* H.264 NAL callback                                                       */
/* ---------------------------------------------------------------------- */

typedef struct {
    cam_nal_ctx_t *nal_ctx;
    uint32_t       last_pts;
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

    /* Detect B-slice (H.264 §7.4.3): for non-IDR slice NALs (type 1),
     * parse first_mb_in_slice (ue) then slice_type (ue) from the RBSP.
     * slice_type % 5 == 1 → B-slice.  Safe to call on hot path: reads ≤8 bytes. */
    if (nal_type == 1 && len >= 2) {
        emd_bitreader_t br;
        emd_bitreader_init(&br, nal + 1, len - 1);
        (void)emd_br_read_ue(&br);           /* first_mb_in_slice */
        uint32_t st = emd_br_read_ue(&br);   /* slice_type 0-9 */
        if (!emd_br_eof(&br) && (st % 5u) == 1u)
            flags |= EMD_NAL_BFRAME;
    }

    push_nal_to_ring(s->nal_ctx, nal, len, (uint64_t)pts, nal_type, flags);
    s->last_pts = pts;
}

/* ---------------------------------------------------------------------- */
/* H.265 NAL callback                                                       */
/* ---------------------------------------------------------------------- */

typedef struct {
    cam_nal_ctx_t *nal_ctx;
    uint32_t       last_pts;
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

    /* Detect B-slice in H.265 non-IRAP VCL NALs (type 0-9: TRAIL/TSA/STSA/RADL/RASL).
     * H.265 slice_segment_header for first-slice independent segments:
     *   first_slice_segment_in_pic_flag u(1)
     *   slice_pic_parameter_set_id      ue(v)
     *   slice_type                      ue(v)  → 0=B, 1=P, 2=I
     * Only parse when first_slice_segment_in_pic_flag=1 (avoids needing PPS for
     * dependent_slice_segments_enabled_flag).  Covers the vast majority of frames. */
    if (nal_type <= 9u && len >= 4) {
        emd_bitreader_t br;
        emd_bitreader_init(&br, nal + 2, len - 2);   /* skip 2-byte NAL unit header */
        int first_flag = emd_br_read_bit(&br);        /* first_slice_segment_in_pic_flag */
        if (first_flag && !emd_br_eof(&br)) {
            (void)emd_br_read_ue(&br);               /* slice_pic_parameter_set_id */
            uint32_t st = emd_br_read_ue(&br);       /* slice_type: 0=B, 1=P, 2=I */
            if (!emd_br_eof(&br) && st == 0u)
                flags |= EMD_NAL_BFRAME;
        }
    }

    push_nal_to_ring(s->nal_ctx, nal, len, (uint64_t)pts, nal_type, flags);
    s->last_pts = pts;
}

/* ---------------------------------------------------------------------- */
/* RTP callback from RTSP                                                   */
/* ---------------------------------------------------------------------- */

typedef struct {
    cam_nal_ctx_t     nal_ctx;
    emd_h264_depay_t  depay264;
    emd_h265_depay_t  depay265;
    h264_cb_state_t   h264_state;
    h265_cb_state_t   h265_state;
    bool              depay264_inited;
    bool              depay265_inited;
    uint8_t           codec; /* 1=h264, 2=h265 */
} cam_rtp_ctx_t;

static void on_rtp_packet(uint8_t channel, const uint8_t *data,
                           uint16_t len, void *userdata)
{
    cam_rtp_ctx_t *ctx = (cam_rtp_ctx_t *)userdata;
    if (channel % 2 != 0) return; /* RTCP on odd channels */

    static int rtp_pkt_count = 0;
    int pc = rtp_pkt_count++;
    if (pc < 10 || pc % 500 == 0)
        fprintf(stderr, "[DBG rtp] pkt#%d ch=%d len=%u\n", pc, channel, len);

    emd_rtp_pkt_t pkt;
    int pr = emd_rtp_parse(data, (size_t)len, &pkt);
    if (pr < 0) {
        fprintf(stderr, "[DBG rtp] parse FAILED pkt#%d\n", pc);
        return;
    }
    if (pc < 10 || pc % 500 == 0)
        fprintf(stderr, "[DBG rtp] pkt#%d seq=%u ts=%u pt=%u marker=%d payload_len=%zu nal_type=%d\n",
                pc, pkt.seq, pkt.timestamp, pkt.payload_type, pkt.marker,
                pkt.payload_len, pkt.payload_len > 0 ? (pkt.payload[0] & 0x1F) : -1);

    if (ctx->codec == 1 && ctx->depay264_inited) {
        emd_h264_depay_feed(&ctx->depay264, &pkt);
    } else if (ctx->codec == 2 && ctx->depay265_inited) {
        emd_h265_depay_feed(&ctx->depay265, &pkt);
    }
}

static void *camera_worker(void *arg) {
    cam_worker_arg_t *wa = (cam_worker_arg_t *)arg;
    const emd_camera_cfg_t *cfg = wa->cam_cfg;
    cam_rtp_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.codec = (cfg->codec_hint == EMD_CODEC_H265) ? 2u : 1u;

    /* Init NAL context */
    ctx.nal_ctx.rb     = wa->rb;
    ctx.nal_ctx.bus    = wa->bus;
    ctx.nal_ctx.cfg    = cfg;
    ctx.nal_ctx.codec  = ctx.codec;

    emd_inspector_default_cfg(&ctx.nal_ctx.insp_cfg);
    ctx.nal_ctx.insp_cfg.motion_z_high      = cfg->motion_z_high > 0.0 ? cfg->motion_z_high : 3.0;
    ctx.nal_ctx.insp_cfg.intra_ratio_high   = cfg->intra_ratio_high > 0.0 ? cfg->intra_ratio_high : 2.5;
    ctx.nal_ctx.insp_cfg.gradual_enabled    = cfg->gradual_enabled;
    ctx.nal_ctx.insp_cfg.on_threshold       = cfg->on_threshold > 0 ? cfg->on_threshold : 2;
    ctx.nal_ctx.insp_cfg.off_threshold      = cfg->off_threshold > 0 ? cfg->off_threshold : 45;
    ctx.nal_ctx.insp_cfg.min_bytes_threshold = cfg->min_bytes_threshold;
    emd_inspector_init(&ctx.nal_ctx.insp_state, &ctx.nal_ctx.insp_cfg);

    /* Init depacketizer */
    ctx.h264_state.nal_ctx = &ctx.nal_ctx;
    ctx.h265_state.nal_ctx = &ctx.nal_ctx;

    if (ctx.codec == 1) {
        if (emd_h264_depay_init(&ctx.depay264, h264_nal_cb, &ctx.h264_state) == 0)
            ctx.depay264_inited = true;
    } else {
        if (emd_h265_depay_init(&ctx.depay265, h265_nal_cb, &ctx.h265_state, false) == 0)
            ctx.depay265_inited = true;
    }

    emd_rtsp_client_t *rtsp = emd_rtsp_client_new(cfg->url, on_rtp_packet, &ctx);
    if (!rtsp) {
        EMD_LOGE("supervisor", "failed to create RTSP client");
        goto cleanup;
    }
    emd_rtsp_set_transport(rtsp, cfg->transport == EMD_TRANSPORT_TCP);

    while (!*wa->stop) {
        int r = emd_rtsp_tick(rtsp);
        if (r < 0) {
            struct timespec ts = {.tv_sec = 0, .tv_nsec = 10000000L};
            nanosleep(&ts, NULL);
        }
    }

    emd_rtsp_teardown(rtsp);
    emd_rtsp_client_free(rtsp);

cleanup:
    if (ctx.depay264_inited) emd_h264_depay_free(&ctx.depay264);
    if (ctx.depay265_inited) emd_h265_depay_free(&ctx.depay265);
    free(wa);
    return NULL;
}

/* ---------------------------------------------------------------------- */
/* Watchdog thread                                                          */
/* ---------------------------------------------------------------------- */

typedef struct {
    volatile bool *running;
} watchdog_arg_t;

static void *watchdog_thread(void *arg) {
    watchdog_arg_t *wa = (watchdog_arg_t *)arg;
    while (*wa->running) {
        emd_sdnotify("WATCHDOG=1");
        sleep(15);
    }
    free(wa);
    return NULL;
}

/* ---------------------------------------------------------------------- */
/* Supervisor run                                                           */
/* ---------------------------------------------------------------------- */

int emd_supervisor_run(const char *config_path) {
    /* Install signal handlers */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sig_handler;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGHUP,  &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    /* Load config */
    emd_config_t cfg;
    char errbuf[256];
    if (emd_config_load(config_path, &cfg, errbuf, sizeof(errbuf)) < 0) {
        fprintf(stderr, "emd: config error: %s\n", errbuf);
        return 1;
    }

    /* Init logging */
    emd_log_set_level((emd_log_level_t)cfg.log_level);
    emd_log_set_hotpath_rate(10);

    /* Init metrics */
    emd_metrics_init();

    EMD_LOGI("supervisor", "starting emd " EMD_VERSION);

    /* Create event bus */
    emd_event_bus_t *bus = emd_event_bus_new(256);
    if (!bus) { EMD_LOGF("supervisor", "event bus alloc failed"); return 1; }

    /* Create ring buffers for each camera */
    uint32_t nc = cfg.num_cameras;
    emd_ringbuf_t **rings = calloc(nc ? nc : 1, sizeof(*rings));
    if (!rings) { EMD_LOGF("supervisor", "rings alloc failed"); return 1; }

    for (uint32_t i = 0; i < nc; i++) {
        const emd_camera_cfg_t *cam = &cfg.cameras[i];
        uint32_t buf_secs = cam->buffer_seconds > 0 ? cam->buffer_seconds : 10;
        uint32_t bps      = cam->max_bitrate_bps > 0 ? cam->max_bitrate_bps : 8000000u;
        uint32_t data_sz  = (uint32_t)((uint64_t)buf_secs * bps / 8 * 5 / 4);
        uint32_t idx_sz   = buf_secs * 900u; /* 30fps × 30s headroom */
        rings[i] = emd_ringbuf_new(idx_sz, data_sz, cam->cam_id, false);
        if (!rings[i]) { EMD_LOGF("supervisor", "ringbuf alloc failed"); return 1; }
    }

    /* Recorder pool */
    emd_recorder_cfg_t rec_cfg;
    memset(&rec_cfg, 0, sizeof(rec_cfg));
    rec_cfg.clip_root      = cfg.clip_root;
    rec_cfg.inflight_root  = cfg.inflight_root;
    rec_cfg.container      = cfg.container;
    rec_cfg.fsync_policy   = cfg.fsync_policy;
    rec_cfg.clip_max_seconds = 120;
    rec_cfg.thread_count   = (uint32_t)(nc > 8 ? nc / 4 : 2);

    emd_recorder_pool_t *recorder = emd_recorder_pool_new(&rec_cfg, rings, nc);
    if (!recorder) { EMD_LOGF("supervisor", "recorder pool alloc failed"); return 1; }
    emd_recorder_pool_start(recorder, bus);

    /* MQTT client */
    emd_mqtt_cfg_t mqtt_cfg;
    memset(&mqtt_cfg, 0, sizeof(mqtt_cfg));
    strncpy(mqtt_cfg.url, cfg.mqtt_url, sizeof(mqtt_cfg.url) - 1);
    strncpy(mqtt_cfg.client_id, cfg.mqtt_client_id_prefix, sizeof(mqtt_cfg.client_id) - 1);
    strncpy(mqtt_cfg.instance_id, "emd-01", sizeof(mqtt_cfg.instance_id) - 1);
    mqtt_cfg.queue_max = 512;
    emd_mqtt_topic_status(mqtt_cfg.instance_id,
                           mqtt_cfg.lwt_topic, sizeof(mqtt_cfg.lwt_topic));
    snprintf(mqtt_cfg.lwt_payload, sizeof(mqtt_cfg.lwt_payload),
             "{\"online\":false,\"reason\":\"lwt\"}");

    emd_mqtt_client_t *mqtt = emd_mqtt_client_new(&mqtt_cfg, NULL, NULL);
    if (mqtt) emd_mqtt_start(mqtt);

    /* Camera worker threads */
    pthread_t *cam_threads = calloc(nc ? nc : 1, sizeof(pthread_t));
    bool *cam_stop_buf = calloc(nc ? nc : 1, sizeof(bool));
    volatile bool *cam_stop = (volatile bool *)cam_stop_buf;
    if (!cam_threads || !cam_stop_buf) {
        EMD_LOGF("supervisor", "alloc failed");
        return 1;
    }

    for (uint32_t i = 0; i < nc; i++) {
        cam_worker_arg_t *wa = calloc(1, sizeof(*wa));
        if (!wa) continue;
        wa->cam_cfg = &cfg.cameras[i];
        wa->rb      = rings[i];
        wa->bus     = bus;
        wa->stop    = &cam_stop[i];
        pthread_create(&cam_threads[i], NULL, camera_worker, wa);
    }

    /* Watchdog thread */
    volatile bool watchdog_running = true;
    pthread_t wdog_thread;
    bool wdog_started = false;
    watchdog_arg_t *wdog_arg = calloc(1, sizeof(*wdog_arg));
    if (wdog_arg) {
        wdog_arg->running = &watchdog_running;
        if (pthread_create(&wdog_thread, NULL, watchdog_thread, wdog_arg) == 0)
            wdog_started = true;
    }

    /* Notify systemd we are ready */
    emd_sdnotify("READY=1\nSTATUS=Running");

    /* Publish online status */
    if (mqtt) {
        char status_topic[256], status_payload[128];
        emd_mqtt_topic_status(mqtt_cfg.instance_id, status_topic, sizeof(status_topic));
        emd_mqtt_build_status_payload(true, "startup", status_payload, sizeof(status_payload));
        emd_mqtt_publish_str(mqtt, status_topic, status_payload, EMD_MQTT_QOS1, true);
    }

    /* Main loop */
    while (!g_shutdown_requested) {
        if (g_reload_requested) {
            g_reload_requested = 0;
            emd_config_t new_cfg;
            char err2[256];
            if (emd_config_load(config_path, &new_cfg, err2, sizeof(err2)) == 0) {
                EMD_LOGI("supervisor", "config reloaded");
                emd_log_set_level((emd_log_level_t)new_cfg.log_level);
                emd_config_free(&new_cfg);
            } else {
                EMD_LOGW("supervisor", "hotreload failed, keeping old config");
            }
        }

        struct timespec ts = {.tv_sec = 0, .tv_nsec = 100000000L};
        nanosleep(&ts, NULL);
    }

    EMD_LOGI("supervisor", "shutting down");

    /* Stop workers */
    for (uint32_t i = 0; i < nc; i++) cam_stop[i] = true;
    for (uint32_t i = 0; i < nc; i++) pthread_join(cam_threads[i], NULL);

    /* Stop subsystems */
    emd_recorder_pool_stop(recorder);
    emd_recorder_pool_free(recorder);
    if (mqtt) { emd_mqtt_stop(mqtt); emd_mqtt_client_free(mqtt); }

    watchdog_running = false;
    if (wdog_started) pthread_join(wdog_thread, NULL);

    /* Cleanup */
    for (uint32_t i = 0; i < nc; i++) emd_ringbuf_free(rings[i]);
    free(rings);
    free(cam_threads);
    free(cam_stop_buf);
    emd_event_bus_free(bus);
    emd_config_free(&cfg);

    emd_sdnotify("STOPPING=1");
    return 0;
}

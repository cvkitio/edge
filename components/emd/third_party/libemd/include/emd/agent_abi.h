/*
 * agent_abi.h — Phase 2 C ABI for emd-agent (Go outer)
 *
 * This header defines the stable C interface between libemd (the Phase 1
 * core repackaged as a library) and the emd-agent Go supervisor.
 *
 * The ABI is versioned; semantic versioning applies:
 *   MAJOR – breaking changes (struct layout, signature changes)
 *   MINOR – backward-compatible additions
 *   PATCH – bug fixes, no ABI change
 *
 * See edge-motion-detector-phase2-spec.md §3 for details.
 */

#pragma once
#ifndef EMD_AGENT_ABI_H
#define EMD_AGENT_ABI_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "emd/config.h"
#include "emd/event.h"
#include "emd/recorder.h"
#include "emd/inspector.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * ABI version (§3.1)
 * ------------------------------------------------------------------------- */

#define EMD_ABI_VERSION_MAJOR 1
#define EMD_ABI_VERSION_MINOR 0
#define EMD_ABI_VERSION_PATCH 0

/*
 * Returns packed ABI version: 0xMMmmpp (MAJOR.MINOR.PATCH).
 * Example: v1.2.3 → 0x010203
 */
uint32_t emd_abi_version(void);

/*
 * Returns human-readable build info string:
 *   "libemd 1.0.0 (commit abc1234) [h264, h265]"
 */
const char *emd_build_info(void);

/* ---------------------------------------------------------------------------
 * Per-camera handle (§3.2)
 * ------------------------------------------------------------------------- */

/*
 * Opaque handle to a camera instance.
 *
 * Internally owns:
 *   - RTSP client state
 *   - Ring buffer
 *   - Inspector state
 *   - Depacketizer state
 *
 * One handle per camera; each runs on a dedicated OS thread.
 */
typedef struct emd_cam emd_cam_t;

/*
 * Open a camera.
 *
 * cfg       – camera configuration; copied internally
 * errbuf    – buffer to receive error message on failure
 * errbuf_len – size of errbuf
 *
 * Returns non-NULL handle on success, NULL on failure (errbuf filled).
 *
 * Example:
 *   char err[256];
 *   emd_cam_t *cam = emd_cam_open(&cfg, err, sizeof(err));
 *   if (!cam) { fprintf(stderr, "open failed: %s\n", err); }
 */
emd_cam_t *emd_cam_open(const emd_camera_cfg_t *cfg,
                        char *errbuf, size_t errbuf_len);

/*
 * Run the camera ingest loop on the calling thread.
 *
 * This function blocks until:
 *   - emd_cam_stop() is called from another thread, OR
 *   - a fatal unrecoverable error occurs
 *
 * Return values:
 *    0  – stopped cleanly via emd_cam_stop()
 *   -1  – fatal error (errbuf carries reason)
 *   -2  – config rejected by camera (e.g., unsupported codec in SDP)
 *
 * Reconnects on transient RTSP/RTP failures with exponential backoff.
 *
 * IMPORTANT: The calling thread should be locked to an OS thread
 * (runtime.LockOSThread() in Go) before calling this function.
 *
 * Example:
 *   char err[256];
 *   int r = emd_cam_run(cam, err, sizeof(err));
 *   if (r < 0) { fprintf(stderr, "run failed: %s\n", err); }
 */
int emd_cam_run(emd_cam_t *cam, char *errbuf, size_t errbuf_len);

/*
 * Signal the running camera to stop.
 *
 * Thread-safe. Returns immediately. The emd_cam_run() call will return
 * once the inner loop unblocks (typically within a few milliseconds).
 *
 * Safe to call multiple times; idempotent.
 */
void emd_cam_stop(emd_cam_t *cam);

/*
 * Close a stopped camera and release all resources.
 *
 * The camera must have been stopped via emd_cam_stop() and emd_cam_run()
 * must have returned before calling this.
 *
 * After this call, the handle is invalid.
 */
void emd_cam_close(emd_cam_t *cam);

/* ---------------------------------------------------------------------------
 * Event callback (C → Go) (§3.3)
 * ------------------------------------------------------------------------- */

/*
 * Callback invoked from the camera thread on each detection event.
 *
 * The callee MUST NOT block. It should copy the event and return quickly
 * (typically by sending to a Go channel with non-blocking semantics).
 *
 * 'evt' is owned by libemd and is valid only for the duration of the call.
 *
 * user_ctx – opaque pointer registered via emd_cam_set_event_cb()
 */
typedef void (*emd_event_cb_t)(void *user_ctx, const emd_event_t *evt);

/*
 * Register an event callback.
 *
 * cb       – callback function
 * user_ctx – opaque pointer passed to callback (typically a cgo.Handle)
 *
 * Safe to call before or during emd_cam_run().
 */
void emd_cam_set_event_cb(emd_cam_t *cam, emd_event_cb_t cb, void *user_ctx);

/* ---------------------------------------------------------------------------
 * Stats sampling (§3.3 optional)
 * ------------------------------------------------------------------------- */

/*
 * Periodic statistics sample from the inspector and RTSP client.
 *
 * Delivered via emd_stats_cb_t callback at the configured cadence.
 */
typedef struct {
    uint16_t cam_id;
    uint64_t mono_ns;
    double   bpf_ewma;           /* inspector fast EWMA */
    double   bpf_slow;           /* inspector slow EWMA */
    uint8_t  fsm_state;          /* emd_inspector_fsm_t */
    uint8_t  rtsp_state;         /* emd_rtsp_state_t */
} emd_stats_sample_t;

/*
 * Stats callback. Same contract as emd_event_cb_t: MUST NOT block.
 */
typedef void (*emd_stats_cb_t)(void *user_ctx, const emd_stats_sample_t *sample);

/*
 * Register a stats callback.
 *
 * cb              – callback function
 * user_ctx        – opaque pointer passed to callback
 * every_n_frames  – emit a sample every N frames (0 = disabled)
 *
 * Typical usage: every_n_frames = fps × 5 (≈ 5-second cadence).
 */
void emd_cam_set_stats_cb(emd_cam_t *cam,
                          emd_stats_cb_t cb, void *user_ctx,
                          uint32_t every_n_frames);

/* ---------------------------------------------------------------------------
 * Snapshot and recording (§3.4)
 * ------------------------------------------------------------------------- */

/*
 * Clip recording request.
 *
 * Used by emd_cam_record() to specify where and how to write the clip.
 *
 * z_buf / z_buf_count / z_out_count are optional: set z_buf=NULL to skip
 * timeline extraction. When provided, one emd_z_point_t is written per
 * unique access unit PTS in the snapshot (deduplicated by PTS), up to
 * z_buf_count entries. *z_out_count is set to the actual count written.
 */
typedef struct {
    const char         *out_path;     /* path to .part file in inflight dir */
    const char         *container;    /* "mpegts" or "fmp4" */
    emd_fsync_policy_t  fsync_policy; /* EMD_FSYNC_ON_CLOSE, etc. */
    /* Optional z-score timeline output (NULL = skip) */
    emd_z_point_t      *z_buf;        /* caller-allocated; NULL to skip */
    uint32_t            z_buf_count;  /* capacity of z_buf */
    uint32_t           *z_out_count;  /* set to actual count written (may be NULL) */
} emd_clip_request_t;

/*
 * Record a clip from the camera's ring buffer.
 *
 * This function:
 *   1. Pulls a snapshot covering [from_pts, to_pts] from the camera's ring.
 *   2. Widens the snapshot backwards to the nearest IDR + parameter sets.
 *   3. Writes the clip to req->out_path (the .part file).
 *   4. Fills hdr_out with clip metadata.
 *
 * The caller is responsible for:
 *   - fsync()'ing the directory (if required by policy)
 *   - rename()'ing the .part file to its final location
 *
 * cam              – camera handle
 * from_pts_90khz   – start PTS (90 kHz)
 * to_pts_90khz     – end PTS (90 kHz)
 * req              – recording parameters
 * hdr_out          – filled with clip metadata on success
 * errbuf           – buffer to receive error message on failure
 * errbuf_len       – size of errbuf
 *
 * Returns:
 *    0  – success (hdr_out filled, clip written to out_path)
 *   -1  – no data in range (ring buffer already wrapped past these PTS)
 *   -2  – muxer error (errbuf filled)
 *
 * Example:
 *   emd_clip_request_t req = {
 *       .out_path = "/tmp/inflight/01HXY3Q.ts.part",
 *       .container = "mpegts",
 *       .fsync_policy = EMD_FSYNC_ON_CLOSE,
 *   };
 *   emd_clip_header_t hdr;
 *   char err[256];
 *   int r = emd_cam_record(cam, from_pts, to_pts, &req, &hdr, err, sizeof(err));
 *   if (r == 0) {
 *       fsync(fd); // if policy requires
 *       rename(req.out_path, final_path);
 *   }
 */
int emd_cam_record(emd_cam_t *cam,
                   uint64_t from_pts_90khz, uint64_t to_pts_90khz,
                   const emd_clip_request_t *req,
                   emd_clip_header_t *hdr_out,
                   char *errbuf, size_t errbuf_len);

/* ---------------------------------------------------------------------------
 * Runtime configuration updates (§3.5)
 * ------------------------------------------------------------------------- */

/*
 * Update the inspector configuration for a running camera.
 *
 * This allows adjusting motion detection sensitivity parameters at runtime
 * without restarting the camera worker thread.
 *
 * cam     – camera handle
 * cfg     – new inspector configuration; copied internally
 *
 * Thread-safe. Changes take effect immediately (next frame processed).
 *
 * Returns:
 *    0  – success
 *   -1  – invalid parameter (e.g., negative threshold)
 *
 * Example:
 *   emd_inspector_cfg_t cfg;
 *   emd_inspector_default_cfg(&cfg);
 *   cfg.motion_z_high = 4.5;  // less sensitive
 *   emd_cam_update_inspector_cfg(cam, &cfg);
 */
int emd_cam_update_inspector_cfg(emd_cam_t *cam, const emd_inspector_cfg_t *cfg);

/*
 * Get the current inspector configuration from a camera.
 *
 * cam     – camera handle
 * cfg_out – filled with current configuration
 *
 * Thread-safe.
 *
 * Returns:
 *    0  – success
 *   -1  – invalid handle
 */
int emd_cam_get_inspector_cfg(emd_cam_t *cam, emd_inspector_cfg_t *cfg_out);

#ifdef __cplusplus
}
#endif

#endif /* EMD_AGENT_ABI_H */

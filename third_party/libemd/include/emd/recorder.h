#pragma once
#ifndef EMD_RECORDER_H
#define EMD_RECORDER_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "emd/event.h"
#include "emd/ringbuf.h"
#include "emd/config.h"

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------
 * Muxer backend interface (§13 extension slot emd_mux_*)
 * ---------------------------------------------------------------------- */
typedef struct {
    /* Open a new output.  path is the .part file to write.
     * codec: 1=h264, 2=h265.
     * Returns opaque context, or NULL on failure. */
    void *(*open)(const char *path, uint8_t codec,
                  uint32_t width, uint32_t height,
                  uint32_t timescale);

    /* Write one NAL unit.  pts/dts in 90 kHz. */
    int (*write_nal)(void *ctx, const uint8_t *nal, size_t len,
                     uint64_t pts, uint64_t dts, bool is_keyframe);

    /* Flush and close.  Returns 0 on success. */
    int (*close)(void *ctx);
} emd_mux_backend_t;

extern const emd_mux_backend_t emd_mux_mpegts;
extern const emd_mux_backend_t emd_mux_fmp4;

/* -------------------------------------------------------------------------
 * Clip sidecar header (written as .json beside the clip)
 * ---------------------------------------------------------------------- */
typedef struct {
    char     event_id[27];
    char     cam_id_str[64];
    char     container[8];   /* "mpegts" or "fmp4" */
    char     codec[6];
    char     path[512];
    uint64_t size_bytes;
    uint64_t duration_ms;
    uint64_t pre_roll_ms;
    uint64_t post_roll_ms;
    char     sha256[65];
} emd_clip_header_t;

/* -------------------------------------------------------------------------
 * Recorder pool
 * ---------------------------------------------------------------------- */
typedef struct emd_recorder_pool emd_recorder_pool_t;

typedef struct {
    const char             *clip_root;
    const char             *inflight_root;
    uint32_t                thread_count;   /* 0 = auto */
    emd_container_t         container;
    emd_fsync_policy_t      fsync_policy;
    uint32_t                clip_max_seconds;
} emd_recorder_cfg_t;

/*
 * Create a recorder pool.
 * ring_map: array indexed by cam_id pointing to the ring buffer for that camera.
 * ring_map_count: number of entries.
 */
emd_recorder_pool_t *emd_recorder_pool_new(const emd_recorder_cfg_t *cfg,
                                            emd_ringbuf_t **ring_map,
                                            uint32_t ring_map_count);

void emd_recorder_pool_free(emd_recorder_pool_t *pool);

/*
 * Start the recorder worker threads.
 */
int emd_recorder_pool_start(emd_recorder_pool_t *pool, emd_event_bus_t *bus);

/*
 * Stop all worker threads (join).
 */
void emd_recorder_pool_stop(emd_recorder_pool_t *pool);

/* -------------------------------------------------------------------------
 * Low-level clip write (used by recorder workers; also testable standalone)
 * ---------------------------------------------------------------------- */

/*
 * Write a clip from a ring buffer snapshot to disk.
 * Follows the atomic write protocol: .part → fsync → rename.
 * Fills hdr_out with metadata about the written clip.
 */
int emd_recorder_write_clip(const emd_recorder_cfg_t *cfg,
                             const emd_event_t *ev,
                             emd_ringbuf_snap_t *snap,
                             const emd_mux_backend_t *mux,
                             emd_clip_header_t *hdr_out);

/* Write the sidecar .json file next to the clip. */
int emd_recorder_write_sidecar(const emd_clip_header_t *hdr);

#ifdef __cplusplus
}
#endif

#endif /* EMD_RECORDER_H */

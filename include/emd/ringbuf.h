#pragma once
#ifndef EMD_RINGBUF_H
#define EMD_RINGBUF_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdatomic.h>

#ifdef __cplusplus
extern "C" {
#endif

/* NAL flags */
#define EMD_NAL_KEYFRAME  0x01u
#define EMD_NAL_PARAMSET  0x02u
#define EMD_NAL_LOST      0x04u  /* synthetic loss record */

/* Record describing one NAL unit in the ring */
typedef struct {
    uint64_t pts_90khz;   /* RTP timestamp, promoted to 64-bit */
    uint64_t mono_ns;     /* CLOCK_MONOTONIC at receipt */
    uint32_t offset;      /* byte offset into backing data ring */
    uint32_t length;      /* byte length */
    uint8_t  nal_type;    /* codec-specific NAL type byte */
    uint8_t  flags;       /* EMD_NAL_KEYFRAME | EMD_NAL_PARAMSET | EMD_NAL_LOST */
    uint16_t cam_id;
} emd_nal_record_t;

/*
 * SPSC lock-free ring buffer.
 *
 * - One writer thread (camera worker) — the producer.
 * - One reader thread (recorder snapshot) — the consumer at flush time.
 * - Backing store: two separate rings:
 *     index_ring[] — array of emd_nal_record_t
 *     data_ring[]  — byte array holding NAL data
 *
 * Memory ordering:
 *   write_head is stored with memory_order_release after writing.
 *   Readers load write_head with memory_order_acquire.
 */
typedef struct {
    /* Index ring */
    emd_nal_record_t *index_ring;
    uint32_t          index_cap;    /* power of two */
    uint32_t          index_mask;

    /* Data byte ring */
    uint8_t          *data_ring;
    uint32_t          data_cap;     /* power of two */
    uint32_t          data_mask;

    /* Shared atomic positions (SPSC) */
    _Atomic uint32_t  write_head;   /* producer owns; written with release */
    _Atomic uint32_t  read_tail;    /* consumer updates with release */

    /* Data ring positions */
    _Atomic uint32_t  data_write;   /* bytes committed by producer */
    _Atomic uint32_t  data_read;    /* bytes consumed by snapshot reader */

    uint16_t          cam_id;
    bool              mlock_enabled;
} emd_ringbuf_t;

/*
 * A lightweight snapshot: a contiguous view of the index ring from
 * tail_idx to head_idx (exclusive).  The backing data is NOT copied;
 * instead a refcount keeps the producer from overwriting it until released.
 *
 * In our SPSC model the recorder holds read_tail, preventing the producer
 * from advancing past it.
 */
typedef struct {
    const emd_nal_record_t *records; /* pointer into index_ring at tail_idx */
    uint32_t                count;   /* number of records */
    uint32_t                tail_idx;
    uint32_t                head_idx;
    emd_ringbuf_t          *rb;      /* back-pointer for release */
} emd_ringbuf_snap_t;

/*
 * Create a ring buffer.
 * index_count — number of NAL record slots (rounded up to power of 2).
 * data_bytes  — byte capacity of the data ring (rounded up to power of 2).
 */
emd_ringbuf_t *emd_ringbuf_new(uint32_t index_count, uint32_t data_bytes,
                                uint16_t cam_id, bool mlock_enabled);

void emd_ringbuf_free(emd_ringbuf_t *rb);

/*
 * Writer API (called by camera worker thread only).
 *
 * Reserve space for a NAL of 'len' bytes.
 * Returns a pointer to where the caller should write the NAL data,
 * or NULL if the ring is full (caller must drop oldest record first
 * via emd_ringbuf_advance_tail()).
 */
uint8_t *emd_ringbuf_reserve(emd_ringbuf_t *rb, uint32_t len,
                              emd_nal_record_t *rec_out);

/*
 * Commit a previously reserved NAL.  rec must be filled in by caller.
 */
void emd_ringbuf_commit(emd_ringbuf_t *rb, const emd_nal_record_t *rec);

/*
 * Drop the oldest record (advances read_tail).  Called by writer when ring
 * is full, to evict oldest content rather than block.
 */
void emd_ringbuf_drop_oldest(emd_ringbuf_t *rb);

/* How many index slots are currently in use? */
uint32_t emd_ringbuf_count(const emd_ringbuf_t *rb);

/*
 * Snapshot API (called by recorder thread).
 *
 * Produces a snapshot covering [from_pts, to_pts].
 * The snapshot is widened backwards to the most recent SPS/PPS + IDR.
 * Returns 0 on success, -1 if no data in range.
 */
int emd_ringbuf_snapshot(emd_ringbuf_t *rb,
                          uint64_t from_pts, uint64_t to_pts,
                          emd_ringbuf_snap_t *snap);

/*
 * Extend an existing snapshot to cover up to new_to_pts.
 * Used for post-roll extension.
 */
int emd_ringbuf_snapshot_extend(emd_ringbuf_snap_t *snap, uint64_t new_to_pts);

/*
 * Release a snapshot (allows the producer to reclaim space).
 */
void emd_ringbuf_snapshot_release(emd_ringbuf_snap_t *snap);

/*
 * Retrieve the raw NAL data for a record in a snapshot.
 * Returns a pointer into the data ring (valid until snapshot_release).
 */
const uint8_t *emd_ringbuf_snap_data(const emd_ringbuf_snap_t *snap,
                                      uint32_t record_idx);

#ifdef __cplusplus
}
#endif

#endif /* EMD_RINGBUF_H */

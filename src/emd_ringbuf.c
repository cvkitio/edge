#include "emd/ringbuf.h"
#include "emd/log.h"

#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/mman.h>

/* -------------------------------------------------------------------------
 * Helpers: round up to power of 2
 * ---------------------------------------------------------------------- */
static uint32_t round_pow2(uint32_t v) {
    if (v == 0) return 1;
    v--;
    v |= v >> 1;
    v |= v >> 2;
    v |= v >> 4;
    v |= v >> 8;
    v |= v >> 16;
    return v + 1;
}

/* -------------------------------------------------------------------------
 * Create / free
 * ---------------------------------------------------------------------- */
emd_ringbuf_t *emd_ringbuf_new(uint32_t index_count, uint32_t data_bytes,
                                uint16_t cam_id, bool mlock_enabled)
{
    emd_ringbuf_t *rb = calloc(1, sizeof(*rb));
    if (!rb) return NULL;

    rb->index_cap  = round_pow2(index_count);
    rb->index_mask = rb->index_cap - 1;
    rb->data_cap   = round_pow2(data_bytes);
    rb->data_mask  = rb->data_cap - 1;
    rb->cam_id     = cam_id;
    rb->mlock_enabled = mlock_enabled;

    rb->index_ring = calloc(rb->index_cap, sizeof(emd_nal_record_t));
    if (!rb->index_ring) { free(rb); return NULL; }

    rb->data_ring = malloc(rb->data_cap);
    if (!rb->data_ring) { free(rb->index_ring); free(rb); return NULL; }

    if (mlock_enabled) {
        mlock(rb->data_ring, rb->data_cap);
#ifdef MADV_DONTFORK
        madvise(rb->data_ring, rb->data_cap, MADV_DONTFORK);
#endif
    }

    atomic_store_explicit(&rb->write_head, 0u, memory_order_relaxed);
    atomic_store_explicit(&rb->read_tail,  0u, memory_order_relaxed);
    atomic_store_explicit(&rb->data_write, 0u, memory_order_relaxed);
    atomic_store_explicit(&rb->data_read,  0u, memory_order_relaxed);

    return rb;
}

void emd_ringbuf_free(emd_ringbuf_t *rb) {
    if (!rb) return;
    if (rb->mlock_enabled && rb->data_ring) {
        munlock(rb->data_ring, rb->data_cap);
    }
    free(rb->index_ring);
    free(rb->data_ring);
    free(rb);
}

/* -------------------------------------------------------------------------
 * Writer API (producer thread only)
 * ---------------------------------------------------------------------- */
uint32_t emd_ringbuf_count(const emd_ringbuf_t *rb) {
    uint32_t wh = atomic_load_explicit(&rb->write_head, memory_order_acquire);
    uint32_t rt = atomic_load_explicit(&rb->read_tail,  memory_order_acquire);
    return (wh - rt) & rb->index_mask;
}

void emd_ringbuf_drop_oldest(emd_ringbuf_t *rb) {
    uint32_t rt = atomic_load_explicit(&rb->read_tail, memory_order_acquire);
    uint32_t wh = atomic_load_explicit(&rb->write_head, memory_order_acquire);
    if (rt == wh) return; /* empty */

    const emd_nal_record_t *oldest = &rb->index_ring[rt & rb->index_mask];
    uint32_t dr = atomic_load_explicit(&rb->data_read, memory_order_acquire);
    /* Advance data_read past oldest entry */
    atomic_store_explicit(&rb->data_read, dr + oldest->length, memory_order_release);
    atomic_store_explicit(&rb->read_tail, rt + 1, memory_order_release);
}

uint8_t *emd_ringbuf_reserve(emd_ringbuf_t *rb, uint32_t len,
                              emd_nal_record_t *rec_out)
{
    if (!rb || !rec_out || len == 0) return NULL;
    if (len > rb->data_cap / 2) return NULL; /* sanity */

    /* Check if index ring is full; if so, drop oldest */
    uint32_t wh = atomic_load_explicit(&rb->write_head, memory_order_relaxed);
    uint32_t rt = atomic_load_explicit(&rb->read_tail,  memory_order_acquire);
    uint32_t used = (wh - rt) & rb->index_mask;

    while (used >= rb->index_cap - 1) {
        emd_ringbuf_drop_oldest(rb);
        rt   = atomic_load_explicit(&rb->read_tail, memory_order_acquire);
        used = (wh - rt) & rb->index_mask;
    }

    /* Reserve data ring space */
    uint32_t dw = atomic_load_explicit(&rb->data_write, memory_order_relaxed);
    /* Data ring: check enough space (wrap-around is OK, we use mask) */
    /* Just return the pointer; wrap handling via mask */

    uint32_t offset = dw & rb->data_mask;
    /* Ensure contiguous: if wrapping would split the data, pad to boundary */
    if (offset + len > rb->data_cap) {
        /* Skip to start of ring */
        atomic_store_explicit(&rb->data_write, dw + (rb->data_cap - offset),
                               memory_order_relaxed);
        dw = atomic_load_explicit(&rb->data_write, memory_order_relaxed);
        offset = 0;
    }

    rec_out->offset = offset;
    rec_out->length = len;
    rec_out->cam_id = rb->cam_id;

    return rb->data_ring + offset;
}

void emd_ringbuf_commit(emd_ringbuf_t *rb, const emd_nal_record_t *rec) {
    if (!rb || !rec) return;

    uint32_t wh = atomic_load_explicit(&rb->write_head, memory_order_relaxed);
    rb->index_ring[wh & rb->index_mask] = *rec;

    /* Advance data_write */
    uint32_t dw = atomic_load_explicit(&rb->data_write, memory_order_relaxed);
    atomic_store_explicit(&rb->data_write, dw + rec->length, memory_order_release);

    /* Publish index slot */
    atomic_store_explicit(&rb->write_head, wh + 1, memory_order_release);
}

/* -------------------------------------------------------------------------
 * Snapshot API (consumer / recorder thread)
 * ---------------------------------------------------------------------- */
int emd_ringbuf_snapshot(emd_ringbuf_t *rb,
                          uint64_t from_pts, uint64_t to_pts,
                          emd_ringbuf_snap_t *snap)
{
    if (!rb || !snap) return -1;

    uint32_t wh = atomic_load_explicit(&rb->write_head, memory_order_acquire);
    uint32_t rt = atomic_load_explicit(&rb->read_tail,  memory_order_acquire);

    if (wh == rt) return -1; /* empty */

    /* Find the range [tail_idx, head_idx) within [rt, wh) */
    uint32_t tail_idx = wh; /* will search backwards */
    uint32_t head_idx = wh;
    bool found_kf = false;
    bool in_range = false;

    /* Scan from newest to oldest */
    for (uint32_t i = wh; i != rt; ) {
        i--;
        const emd_nal_record_t *rec = &rb->index_ring[i & rb->index_mask];

        if (rec->pts_90khz <= to_pts) {
            if (!in_range) {
                head_idx = i + 1;
                in_range = true;
            }
        }
        if (rec->pts_90khz < from_pts) {
            /* We've gone past the from_pts.  Widen backwards to nearest keyframe. */
            if (!found_kf) {
                if (rec->flags & (EMD_NAL_KEYFRAME | EMD_NAL_PARAMSET)) {
                    tail_idx = i;
                    found_kf = true;
                }
            }
            if (found_kf) break;
        }

        tail_idx = i;
    }

    if (!in_range) return -1;

    snap->rb       = rb;
    snap->tail_idx = tail_idx;
    snap->head_idx = head_idx;
    snap->records  = &rb->index_ring[tail_idx & rb->index_mask];
    snap->count    = head_idx - tail_idx;

    /* Pin: advance read_tail to prevent producer from overwriting */
    atomic_store_explicit(&rb->read_tail, tail_idx, memory_order_release);

    return 0;
}

int emd_ringbuf_snapshot_extend(emd_ringbuf_snap_t *snap, uint64_t new_to_pts) {
    if (!snap || !snap->rb) return -1;
    emd_ringbuf_t *rb = snap->rb;

    uint32_t wh = atomic_load_explicit(&rb->write_head, memory_order_acquire);

    /* Extend head_idx to cover new_to_pts */
    for (uint32_t i = snap->head_idx; i != wh; i++) {
        const emd_nal_record_t *rec = &rb->index_ring[i & rb->index_mask];
        if (rec->pts_90khz > new_to_pts) break;
        snap->head_idx = i + 1;
    }
    snap->count = snap->head_idx - snap->tail_idx;
    return 0;
}

void emd_ringbuf_snapshot_release(emd_ringbuf_snap_t *snap) {
    if (!snap || !snap->rb) return;
    /* Release pin: let producer advance past head_idx */
    atomic_store_explicit(&snap->rb->read_tail, snap->head_idx, memory_order_release);
    memset(snap, 0, sizeof(*snap));
}

const uint8_t *emd_ringbuf_snap_data(const emd_ringbuf_snap_t *snap,
                                      uint32_t record_idx)
{
    if (!snap || !snap->rb || record_idx >= snap->count) return NULL;
    uint32_t abs_idx = snap->tail_idx + record_idx;
    const emd_nal_record_t *rec = &snap->rb->index_ring[abs_idx & snap->rb->index_mask];
    return snap->rb->data_ring + rec->offset;
}

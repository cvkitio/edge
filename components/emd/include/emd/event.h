#pragma once
#ifndef EMD_EVENT_H
#define EMD_EVENT_H

#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>
#include "emd/inspector.h"

#ifdef __cplusplus
extern "C" {
#endif

#define EMD_EVENT_ID_LEN    26  /* ULID-like: 26 chars + NUL */
#define EMD_EVENT_REASON_LEN 128

/* Event record passed from camera worker to event bus */
typedef struct {
    char             event_id[EMD_EVENT_ID_LEN + 1];
    uint16_t         cam_id;
    emd_event_type_t type;
    char             reason[EMD_EVENT_REASON_LEN];
    uint64_t         started_pts_90khz;
    uint64_t         started_mono_ns;
    uint64_t         pre_roll_pts;   /* pts_90khz of pre-roll start */
    uint64_t         post_roll_pts;  /* estimated pts_90khz of post-roll end */
    uint8_t          codec;          /* 1=h264, 2=h265 */
    double           fps_estimate;
    char             cam_name[64];
} emd_event_t;

/* -------------------------------------------------------------------------
 * Lock-free MPMC event bus
 *
 * Bounded ring, power-of-two size.
 * Producers: N camera workers (each does SPSC handoff into the bus).
 * Consumers: recorder pool threads + notifier thread.
 *
 * Uses a sequence number per slot (CAS-based, wait-free for producers,
 * spinning dequeue for consumers).
 * ---------------------------------------------------------------------- */
#define EMD_BUS_DEFAULT_CAPACITY 256u

typedef struct {
    emd_event_t      data;
    _Atomic uint64_t sequence;  /* slot sequence number */
} emd_bus_slot_t;

typedef struct {
    emd_bus_slot_t  *slots;
    uint32_t         capacity;   /* power of two */
    uint32_t         mask;

    _Atomic uint64_t enqueue_pos;  /* next slot to write */
    _Atomic uint64_t dequeue_pos;  /* next slot to read */

    /* Drop tracking */
    _Atomic uint64_t dropped_total;
} emd_event_bus_t;

/* Create a bus with the given capacity (rounded up to power of 2). */
emd_event_bus_t *emd_event_bus_new(uint32_t capacity);
void             emd_event_bus_free(emd_event_bus_t *bus);

/*
 * Enqueue an event (non-blocking).
 * Returns 0 on success, -1 if full (event is dropped, counter incremented).
 */
int emd_event_bus_push(emd_event_bus_t *bus, const emd_event_t *ev);

/*
 * Dequeue an event (non-blocking).
 * Returns 0 and fills *ev if an event is available, -1 (EAGAIN) if empty.
 */
int emd_event_bus_pop(emd_event_bus_t *bus, emd_event_t *ev);

uint64_t emd_event_bus_dropped(const emd_event_bus_t *bus);

/* -------------------------------------------------------------------------
 * Event ID generation (ULID-style monotonic)
 * ---------------------------------------------------------------------- */
void emd_event_id_generate(char *out, size_t len);

/* -------------------------------------------------------------------------
 * Codec enum helpers
 * ---------------------------------------------------------------------- */
static inline const char *emd_codec_name(uint8_t codec) {
    if (codec == 1) return "h264";
    if (codec == 2) return "h265";
    return "unknown";
}

#ifdef __cplusplus
}
#endif

#endif /* EMD_EVENT_H */

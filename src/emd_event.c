#include "emd/event.h"
#include "emd/log.h"

#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdatomic.h>
#include <time.h>
#include <stdio.h>

/* -------------------------------------------------------------------------
 * MPMC ring buffer (Dmitry Vyukov's classic design)
 * ---------------------------------------------------------------------- */

static uint32_t round_pow2_u32(uint32_t v) {
    if (v == 0) return 1;
    v--;
    v |= v >> 1; v |= v >> 2; v |= v >> 4; v |= v >> 8; v |= v >> 16;
    return v + 1;
}

emd_event_bus_t *emd_event_bus_new(uint32_t capacity) {
    uint32_t cap = round_pow2_u32(capacity);
    if (cap < 2) cap = 2;

    emd_event_bus_t *bus = calloc(1, sizeof(*bus));
    if (!bus) return NULL;

    bus->slots = calloc(cap, sizeof(emd_bus_slot_t));
    if (!bus->slots) { free(bus); return NULL; }

    bus->capacity = cap;
    bus->mask     = cap - 1;

    /* Initialise sequence numbers: slot i gets initial sequence = i */
    for (uint32_t i = 0; i < cap; i++) {
        atomic_store_explicit(&bus->slots[i].sequence, (uint64_t)i, memory_order_relaxed);
    }

    atomic_store_explicit(&bus->enqueue_pos, 0u, memory_order_relaxed);
    atomic_store_explicit(&bus->dequeue_pos, 0u, memory_order_relaxed);
    atomic_store_explicit(&bus->dropped_total, 0u, memory_order_relaxed);

    return bus;
}

void emd_event_bus_free(emd_event_bus_t *bus) {
    if (!bus) return;
    free(bus->slots);
    free(bus);
}

int emd_event_bus_push(emd_event_bus_t *bus, const emd_event_t *ev) {
    if (!bus || !ev) return -1;

    uint64_t pos = atomic_load_explicit(&bus->enqueue_pos, memory_order_relaxed);

    for (;;) {
        emd_bus_slot_t *slot = &bus->slots[pos & bus->mask];
        uint64_t seq = atomic_load_explicit(&slot->sequence, memory_order_acquire);
        int64_t diff = (int64_t)seq - (int64_t)pos;

        if (diff == 0) {
            /* Slot is free; try to claim it */
            if (atomic_compare_exchange_weak_explicit(
                    &bus->enqueue_pos, &pos, pos + 1,
                    memory_order_relaxed, memory_order_relaxed)) {
                /* We own the slot */
                slot->data = *ev;
                atomic_store_explicit(&slot->sequence, pos + 1, memory_order_release);
                return 0;
            }
            /* CAS failed; retry with updated pos */
        } else if (diff < 0) {
            /* Ring is full */
            atomic_fetch_add_explicit(&bus->dropped_total, 1, memory_order_relaxed);
            return -1;
        } else {
            /* Another producer advanced pos; reload */
            pos = atomic_load_explicit(&bus->enqueue_pos, memory_order_relaxed);
        }
    }
}

int emd_event_bus_pop(emd_event_bus_t *bus, emd_event_t *ev) {
    if (!bus || !ev) return -1;

    uint64_t pos = atomic_load_explicit(&bus->dequeue_pos, memory_order_relaxed);

    for (;;) {
        emd_bus_slot_t *slot = &bus->slots[pos & bus->mask];
        uint64_t seq = atomic_load_explicit(&slot->sequence, memory_order_acquire);
        int64_t diff = (int64_t)seq - (int64_t)(pos + 1);

        if (diff == 0) {
            /* Slot has data; try to claim it */
            if (atomic_compare_exchange_weak_explicit(
                    &bus->dequeue_pos, &pos, pos + 1,
                    memory_order_relaxed, memory_order_relaxed)) {
                *ev = slot->data;
                atomic_store_explicit(&slot->sequence,
                                       pos + bus->capacity, memory_order_release);
                return 0;
            }
        } else if (diff < 0) {
            /* Queue empty */
            return -1;
        } else {
            pos = atomic_load_explicit(&bus->dequeue_pos, memory_order_relaxed);
        }
    }
}

uint64_t emd_event_bus_dropped(const emd_event_bus_t *bus) {
    if (!bus) return 0;
    return atomic_load_explicit(&bus->dropped_total, memory_order_relaxed);
}

/* -------------------------------------------------------------------------
 * Event ID generation (monotonic ULID-like, base32-encoded)
 * ---------------------------------------------------------------------- */
/* Crockford Base32 alphabet */
static const char BASE32[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";

static _Atomic uint64_t g_event_seq = 0;

void emd_event_id_generate(char *out, size_t len) {
    if (!out || len < EMD_EVENT_ID_LEN + 1) return;

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    uint64_t ms = (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000L);
    uint64_t seq = atomic_fetch_add_explicit(&g_event_seq, 1, memory_order_relaxed);

    /* Encode 48-bit timestamp in first 10 chars */
    for (int i = 9; i >= 0; i--) {
        out[i] = BASE32[ms & 0x1Fu];
        ms >>= 5;
    }

    /* Encode seq + clock_ns in remaining 16 chars */
    uint64_t rnd = ((uint64_t)ts.tv_nsec ^ (seq * 6364136223846793005ULL + 1442695040888963407ULL));
    for (int i = 25; i >= 10; i--) {
        out[i] = BASE32[rnd & 0x1Fu];
        rnd >>= 5;
    }
    out[EMD_EVENT_ID_LEN] = '\0';
}

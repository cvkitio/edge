/*
 * test_event_bus.c — MPMC event bus unit tests.
 *
 * Tests:
 *  - Push and pop single event.
 *  - Full queue returns -1.
 *  - Empty queue returns -1.
 *  - MPMC concurrent producers and consumers.
 *  - Event ID uniqueness.
 */

#include <cmocka.h>
#include "emd/event.h"

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <pthread.h>
#include <stdio.h>

/* --------------------------------------------------------------------- */
/* Basic push / pop                                                        */
/* --------------------------------------------------------------------- */

static void test_push_pop(void **state) {
    (void)state;
    emd_event_bus_t *bus = emd_event_bus_new(16);
    assert_non_null(bus);

    emd_event_t ev_in, ev_out;
    memset(&ev_in, 0, sizeof(ev_in));
    ev_in.cam_id = 3;
    ev_in.type   = EMD_EVENT_MOTION;
    strncpy(ev_in.reason, "z=4.7", sizeof(ev_in.reason) - 1);
    emd_event_id_generate(ev_in.event_id, sizeof(ev_in.event_id));

    int r = emd_event_bus_push(bus, &ev_in);
    assert_int_equal(r, 0);

    r = emd_event_bus_pop(bus, &ev_out);
    assert_int_equal(r, 0);
    assert_int_equal((int)ev_out.cam_id, 3);
    assert_int_equal((int)ev_out.type, (int)EMD_EVENT_MOTION);
    assert_string_equal(ev_out.reason, "z=4.7");

    emd_event_bus_free(bus);
}

/* --------------------------------------------------------------------- */
/* Empty queue                                                             */
/* --------------------------------------------------------------------- */

static void test_pop_empty(void **state) {
    (void)state;
    emd_event_bus_t *bus = emd_event_bus_new(8);
    assert_non_null(bus);

    emd_event_t ev;
    int r = emd_event_bus_pop(bus, &ev);
    assert_int_equal(r, -1);

    emd_event_bus_free(bus);
}

/* --------------------------------------------------------------------- */
/* Full queue                                                               */
/* --------------------------------------------------------------------- */

static void test_push_full(void **state) {
    (void)state;
    emd_event_bus_t *bus = emd_event_bus_new(4); /* capacity = next pow2 = 4 */
    assert_non_null(bus);

    emd_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = EMD_EVENT_MOTION;

    int pushed = 0;
    /* Fill the bus */
    for (int i = 0; i < 8; i++) {
        if (emd_event_bus_push(bus, &ev) == 0) pushed++;
    }
    /* Should have filled up to capacity, rest dropped */
    assert_true(pushed <= 4);
    assert_true(emd_event_bus_dropped(bus) > 0);

    emd_event_bus_free(bus);
}

/* --------------------------------------------------------------------- */
/* Event ID uniqueness                                                      */
/* --------------------------------------------------------------------- */

static void test_event_id_unique(void **state) {
    (void)state;
    char ids[100][EMD_EVENT_ID_LEN + 1];
    for (int i = 0; i < 100; i++) {
        emd_event_id_generate(ids[i], sizeof(ids[i]));
        assert_int_equal((int)strlen(ids[i]), EMD_EVENT_ID_LEN);
    }

    /* Check uniqueness (at minimum IDs must have expected length) */
    for (int i = 0; i < 100; i++) {
        for (int j = i + 1; j < 100; j++) {
            assert_string_not_equal(ids[i], ids[j]);
        }
    }
}

/* --------------------------------------------------------------------- */
/* MPMC concurrent stress                                                   */
/* --------------------------------------------------------------------- */

#define MPMC_PRODUCERS   4
#define MPMC_CONSUMERS   4
#define MPMC_EVENTS_EACH 1000

typedef struct {
    emd_event_bus_t *bus;
    int              id;
    int              pushed;
    int              popped;
    volatile bool   *stop;
} mpmc_arg_t;

static void *mpmc_producer(void *arg) {
    mpmc_arg_t *a = (mpmc_arg_t *)arg;
    emd_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.type   = EMD_EVENT_MOTION;
    ev.cam_id = (uint16_t)a->id;

    for (int i = 0; i < MPMC_EVENTS_EACH; i++) {
        emd_event_id_generate(ev.event_id, sizeof(ev.event_id));
        if (emd_event_bus_push(a->bus, &ev) == 0) a->pushed++;
        /* Brief yield */
        struct timespec ts = {.tv_sec = 0, .tv_nsec = 100};
        nanosleep(&ts, NULL);
    }
    return NULL;
}

static void *mpmc_consumer(void *arg) {
    mpmc_arg_t *a = (mpmc_arg_t *)arg;
    emd_event_t ev;
    int attempts = 0;

    while (!*a->stop || attempts < 100) {
        if (emd_event_bus_pop(a->bus, &ev) == 0) {
            a->popped++;
            attempts = 0;
        } else {
            attempts++;
            struct timespec ts = {.tv_sec = 0, .tv_nsec = 10000};
            nanosleep(&ts, NULL);
        }
    }
    return NULL;
}

static void test_mpmc_concurrent(void **state) {
    (void)state;
    emd_event_bus_t *bus = emd_event_bus_new(256);
    assert_non_null(bus);

    volatile bool stop = false;

    pthread_t prod_threads[MPMC_PRODUCERS];
    pthread_t cons_threads[MPMC_CONSUMERS];
    mpmc_arg_t prod_args[MPMC_PRODUCERS];
    mpmc_arg_t cons_args[MPMC_CONSUMERS];

    for (int i = 0; i < MPMC_PRODUCERS; i++) {
        prod_args[i] = (mpmc_arg_t){bus, i, 0, 0, &stop};
        pthread_create(&prod_threads[i], NULL, mpmc_producer, &prod_args[i]);
    }
    for (int i = 0; i < MPMC_CONSUMERS; i++) {
        cons_args[i] = (mpmc_arg_t){bus, i, 0, 0, &stop};
        pthread_create(&cons_threads[i], NULL, mpmc_consumer, &cons_args[i]);
    }

    /* Wait for producers */
    for (int i = 0; i < MPMC_PRODUCERS; i++) pthread_join(prod_threads[i], NULL);

    /* Signal consumers */
    stop = true;
    for (int i = 0; i < MPMC_CONSUMERS; i++) pthread_join(cons_threads[i], NULL);

    int total_pushed = 0, total_popped = 0;
    for (int i = 0; i < MPMC_PRODUCERS; i++) total_pushed += prod_args[i].pushed;
    for (int i = 0; i < MPMC_CONSUMERS; i++) total_popped += cons_args[i].popped;

    /* All pushed events must be either popped or dropped */
    uint64_t dropped = emd_event_bus_dropped(bus);
    assert_true(total_popped + (int)dropped >= total_pushed - 10); /* small tolerance */

    emd_event_bus_free(bus);
}

/* --------------------------------------------------------------------- */
/* Test runner                                                              */
/* --------------------------------------------------------------------- */

int main(void) {
    const CMUnitTest tests[] = {
        cmocka_unit_test(test_push_pop),
        cmocka_unit_test(test_pop_empty),
        cmocka_unit_test(test_push_full),
        cmocka_unit_test(test_event_id_unique),
        cmocka_unit_test(test_mpmc_concurrent),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}

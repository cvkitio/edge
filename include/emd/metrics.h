#pragma once
#ifndef EMD_METRICS_H
#define EMD_METRICS_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque handle to a counter */
typedef struct emd_counter emd_counter_t;
/* Opaque handle to a gauge */
typedef struct emd_gauge   emd_gauge_t;
/* Opaque handle to a histogram */
typedef struct emd_histogram emd_histogram_t;

/*
 * Initialise the metrics subsystem.  Must be called once before any
 * metric registration.  Returns 0 on success.
 */
int emd_metrics_init(void);

/*
 * Destroy the metrics subsystem (call at shutdown).
 */
void emd_metrics_destroy(void);

/* -------------------------------------------------------------------------
 * Counter API (monotonically increasing, lock-free)
 * ---------------------------------------------------------------------- */
emd_counter_t *emd_counter_new(const char *name, const char *help,
                               const char **label_names, uint32_t nlabels);

/* Returns the counter for a specific label set (creates if needed). */
emd_counter_t *emd_counter_labels(emd_counter_t *family,
                                   const char **label_values, uint32_t nlabels);

void emd_counter_inc(emd_counter_t *c);
void emd_counter_add(emd_counter_t *c, uint64_t n);
uint64_t emd_counter_get(const emd_counter_t *c);

/* -------------------------------------------------------------------------
 * Gauge API (can go up or down, atomic store/load)
 * ---------------------------------------------------------------------- */
emd_gauge_t *emd_gauge_new(const char *name, const char *help,
                            const char **label_names, uint32_t nlabels);
emd_gauge_t *emd_gauge_labels(emd_gauge_t *family,
                               const char **label_values, uint32_t nlabels);
void emd_gauge_set(emd_gauge_t *g, double v);
void emd_gauge_inc(emd_gauge_t *g);
void emd_gauge_dec(emd_gauge_t *g);
double emd_gauge_get(const emd_gauge_t *g);

/* -------------------------------------------------------------------------
 * Histogram API (fixed buckets, lock-free)
 * ---------------------------------------------------------------------- */
emd_histogram_t *emd_histogram_new(const char *name, const char *help,
                                    const double *buckets, uint32_t nbuckets,
                                    const char **label_names, uint32_t nlabels);
emd_histogram_t *emd_histogram_labels(emd_histogram_t *family,
                                       const char **label_values, uint32_t nlabels);
void emd_histogram_observe(emd_histogram_t *h, double v);

/* -------------------------------------------------------------------------
 * Prometheus text serialisation
 * Write all metrics to buf (up to bufsz bytes).
 * Returns the number of bytes written (may be truncated if buf is too small).
 * ---------------------------------------------------------------------- */
size_t emd_metrics_serialize(char *buf, size_t bufsz);

/* -------------------------------------------------------------------------
 * Built-in application metrics (registered at init)
 * ---------------------------------------------------------------------- */
/* These are the global counters/gauges referenced throughout the codebase. */
extern emd_counter_t   *g_nal_received_total;
extern emd_counter_t   *g_frames_dropped_total;
extern emd_counter_t   *g_event_total;
extern emd_counter_t   *g_mqtt_publish_total;
extern emd_counter_t   *g_notifications_dropped_total;
extern emd_gauge_t     *g_rtsp_state;
extern emd_gauge_t     *g_inspector_bpf_ewma;
extern emd_gauge_t     *g_mqtt_connected;
extern emd_gauge_t     *g_recorder_queue_depth;
extern emd_histogram_t *g_recording_seconds;
extern emd_histogram_t *g_worker_loop_latency_seconds;

/* Register all built-in metrics (called from emd_metrics_init). */
int emd_metrics_register_builtin(void);

#ifdef __cplusplus
}
#endif

#endif /* EMD_METRICS_H */

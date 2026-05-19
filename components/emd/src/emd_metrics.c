#include "emd/metrics.h"
#include "emd/log.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <math.h>
#include <pthread.h>

/* -------------------------------------------------------------------------
 * Internal structures
 * ---------------------------------------------------------------------- */

#define MAX_LABEL_NAMES   8
#define MAX_LABEL_VALUES  8
#define MAX_LABEL_LEN     64
#define MAX_FAMILY_SLOTS  256   /* total metric families */
#define MAX_SERIES_PER    128   /* label combinations per family */
#define MAX_HISTOGRAM_BUCKETS 32

typedef enum { MF_COUNTER = 0, MF_GAUGE, MF_HISTOGRAM } metric_kind_t;

/* A single time-series (one label set) */
typedef struct {
    char        label_values[MAX_LABEL_VALUES][MAX_LABEL_LEN];
    uint32_t    nlabels;
    bool        used;
    union {
        _Atomic uint64_t counter_val;
        /* gauge: stored as uint64 bits via double<->bits */
        _Atomic uint64_t gauge_bits;
        struct {
            _Atomic uint64_t sum_bits; /* double bits */
            _Atomic uint64_t count;
            _Atomic uint64_t bucket_counts[MAX_HISTOGRAM_BUCKETS];
        } hist;
    };
} metric_series_t;

/* A metric family (name + type + help + label schema) */
struct emd_counter {
    char          name[128];
    char          help[256];
    char          label_names[MAX_LABEL_NAMES][MAX_LABEL_LEN];
    uint32_t      nlabels;
    metric_kind_t kind;
    metric_series_t series[MAX_SERIES_PER];
    /* histogram-only: bucket boundaries */
    double        buckets[MAX_HISTOGRAM_BUCKETS];
    uint32_t      nbuckets;
    pthread_mutex_t mu;
};

/* Alias the same struct for gauge and histogram (single struct, kind discriminates) */
struct emd_gauge      { struct emd_counter base; };
struct emd_histogram  { struct emd_counter base; };

/* Global registry */
static struct {
    struct emd_counter *families[MAX_FAMILY_SLOTS];
    uint32_t            count;
    bool                initialized;
    pthread_mutex_t     mu;
} g_reg = { .initialized = false };

/* Built-in globals */
emd_counter_t   *g_nal_received_total        = NULL;
emd_counter_t   *g_frames_dropped_total      = NULL;
emd_counter_t   *g_event_total               = NULL;
emd_counter_t   *g_mqtt_publish_total        = NULL;
emd_counter_t   *g_notifications_dropped_total = NULL;
emd_gauge_t     *g_rtsp_state               = NULL;
emd_gauge_t     *g_inspector_bpf_ewma       = NULL;
emd_gauge_t     *g_mqtt_connected           = NULL;
emd_gauge_t     *g_recorder_queue_depth     = NULL;
emd_histogram_t *g_recording_seconds        = NULL;
emd_histogram_t *g_worker_loop_latency_seconds = NULL;

/* -------------------------------------------------------------------------
 * Helpers
 * ---------------------------------------------------------------------- */
static uint64_t double_to_bits(double d) {
    uint64_t bits;
    memcpy(&bits, &d, sizeof(bits));
    return bits;
}
static double bits_to_double(uint64_t bits) {
    double d;
    memcpy(&d, &bits, sizeof(d));
    return d;
}

static struct emd_counter *family_alloc(const char *name, const char *help,
                                         const char **label_names, uint32_t nlabels,
                                         metric_kind_t kind,
                                         const double *buckets, uint32_t nbuckets)
{
    struct emd_counter *f = calloc(1, sizeof(*f));
    if (!f) return NULL;

    strncpy(f->name, name, sizeof(f->name) - 1);
    strncpy(f->help, help ? help : "", sizeof(f->help) - 1);
    f->kind = kind;
    f->nlabels = nlabels < MAX_LABEL_NAMES ? nlabels : MAX_LABEL_NAMES;
    for (uint32_t i = 0; i < f->nlabels; i++) {
        strncpy(f->label_names[i], label_names[i], MAX_LABEL_LEN - 1);
    }
    if (kind == MF_HISTOGRAM && buckets && nbuckets) {
        f->nbuckets = nbuckets < MAX_HISTOGRAM_BUCKETS ? nbuckets : MAX_HISTOGRAM_BUCKETS;
        memcpy(f->buckets, buckets, f->nbuckets * sizeof(double));
    }
    pthread_mutex_init(&f->mu, NULL);

    /* Register */
    pthread_mutex_lock(&g_reg.mu);
    if (g_reg.count < MAX_FAMILY_SLOTS) {
        g_reg.families[g_reg.count++] = f;
    }
    pthread_mutex_unlock(&g_reg.mu);
    return f;
}

static metric_series_t *find_or_create_series(struct emd_counter *f,
                                               const char **label_values,
                                               uint32_t nlabels)
{
    pthread_mutex_lock(&f->mu);
    /* Find existing */
    for (uint32_t i = 0; i < MAX_SERIES_PER; i++) {
        if (!f->series[i].used) continue;
        bool match = true;
        for (uint32_t j = 0; j < f->nlabels && j < nlabels; j++) {
            if (strcmp(f->series[i].label_values[j], label_values[j]) != 0) {
                match = false; break;
            }
        }
        if (match) {
            pthread_mutex_unlock(&f->mu);
            return &f->series[i];
        }
    }
    /* Create new */
    for (uint32_t i = 0; i < MAX_SERIES_PER; i++) {
        if (f->series[i].used) continue;
        f->series[i].used = true;
        f->series[i].nlabels = f->nlabels < nlabels ? f->nlabels : nlabels;
        for (uint32_t j = 0; j < f->series[i].nlabels; j++) {
            strncpy(f->series[i].label_values[j], label_values[j], MAX_LABEL_LEN - 1);
        }
        /* Initialise gauge to 0.0 bits */
        if (f->kind == MF_GAUGE) {
            atomic_store(&f->series[i].gauge_bits, double_to_bits(0.0));
        }
        pthread_mutex_unlock(&f->mu);
        return &f->series[i];
    }
    pthread_mutex_unlock(&f->mu);
    return NULL;
}

/* -------------------------------------------------------------------------
 * Counter API
 * ---------------------------------------------------------------------- */
emd_counter_t *emd_counter_new(const char *name, const char *help,
                                const char **label_names, uint32_t nlabels)
{
    return (emd_counter_t *)family_alloc(name, help, label_names, nlabels,
                                          MF_COUNTER, NULL, 0);
}

emd_counter_t *emd_counter_labels(emd_counter_t *f,
                                    const char **label_values, uint32_t nlabels)
{
    /* Returns a *series* cast as counter — we use the family struct directly */
    (void)label_values; (void)nlabels;
    /* For simplicity, return the family itself when nlabels == 0 (no-label variant) */
    return f;
}

void emd_counter_inc(emd_counter_t *c) {
    if (!c) return;
    /* Use series[0] for the no-label case */
    c->series[0].used = true;
    atomic_fetch_add_explicit(&c->series[0].counter_val, 1u, memory_order_relaxed);
}

void emd_counter_add(emd_counter_t *c, uint64_t n) {
    if (!c) return;
    c->series[0].used = true;
    atomic_fetch_add_explicit(&c->series[0].counter_val, n, memory_order_relaxed);
}

uint64_t emd_counter_get(const emd_counter_t *c) {
    if (!c) return 0;
    return atomic_load_explicit(&c->series[0].counter_val, memory_order_relaxed);
}

/* -------------------------------------------------------------------------
 * Gauge API
 * ---------------------------------------------------------------------- */
emd_gauge_t *emd_gauge_new(const char *name, const char *help,
                             const char **label_names, uint32_t nlabels)
{
    return (emd_gauge_t *)family_alloc(name, help, label_names, nlabels,
                                        MF_GAUGE, NULL, 0);
}

emd_gauge_t *emd_gauge_labels(emd_gauge_t *f,
                               const char **label_values, uint32_t nlabels)
{
    return (emd_gauge_t *)find_or_create_series(&f->base, label_values, nlabels);
}

static metric_series_t *gauge_series0(emd_gauge_t *g) {
    g->base.series[0].used = true;
    return &g->base.series[0];
}

void emd_gauge_set(emd_gauge_t *g, double v) {
    if (!g) return;
    metric_series_t *s = gauge_series0(g);
    atomic_store_explicit(&s->gauge_bits, double_to_bits(v), memory_order_relaxed);
}

void emd_gauge_inc(emd_gauge_t *g) {
    if (!g) return;
    metric_series_t *s = gauge_series0(g);
    /* CAS loop to increment double */
    uint64_t old = atomic_load(&s->gauge_bits);
    uint64_t new_val;
    do {
        double dv = bits_to_double(old) + 1.0;
        new_val = double_to_bits(dv);
    } while (!atomic_compare_exchange_weak(&s->gauge_bits, &old, new_val));
}

void emd_gauge_dec(emd_gauge_t *g) {
    if (!g) return;
    metric_series_t *s = gauge_series0(g);
    uint64_t old = atomic_load(&s->gauge_bits);
    uint64_t new_val;
    do {
        double dv = bits_to_double(old) - 1.0;
        new_val = double_to_bits(dv);
    } while (!atomic_compare_exchange_weak(&s->gauge_bits, &old, new_val));
}

double emd_gauge_get(const emd_gauge_t *g) {
    if (!g) return 0.0;
    uint64_t bits = atomic_load_explicit(&g->base.series[0].gauge_bits, memory_order_relaxed);
    return bits_to_double(bits);
}

/* -------------------------------------------------------------------------
 * Histogram API
 * ---------------------------------------------------------------------- */
emd_histogram_t *emd_histogram_new(const char *name, const char *help,
                                    const double *buckets, uint32_t nbuckets,
                                    const char **label_names, uint32_t nlabels)
{
    return (emd_histogram_t *)family_alloc(name, help, label_names, nlabels,
                                            MF_HISTOGRAM, buckets, nbuckets);
}

emd_histogram_t *emd_histogram_labels(emd_histogram_t *f,
                                        const char **label_values, uint32_t nlabels)
{
    return (emd_histogram_t *)find_or_create_series(&f->base, label_values, nlabels);
}

void emd_histogram_observe(emd_histogram_t *h, double v) {
    if (!h) return;
    metric_series_t *s = &h->base.series[0];
    s->used = true;

    atomic_fetch_add_explicit(&s->hist.count, 1, memory_order_relaxed);

    uint64_t old_sum = atomic_load(&s->hist.sum_bits);
    uint64_t new_sum;
    do {
        double ds = bits_to_double(old_sum) + v;
        new_sum = double_to_bits(ds);
    } while (!atomic_compare_exchange_weak(&s->hist.sum_bits, &old_sum, new_sum));

    for (uint32_t i = 0; i < h->base.nbuckets; i++) {
        if (v <= h->base.buckets[i]) {
            atomic_fetch_add_explicit(&s->hist.bucket_counts[i], 1, memory_order_relaxed);
        }
    }
}

/* -------------------------------------------------------------------------
 * Prometheus text serialisation
 * ---------------------------------------------------------------------- */
static int write_labels(char *buf, size_t bufsz,
                         const struct emd_counter *f,
                         const metric_series_t *s)
{
    if (f->nlabels == 0) return 0;
    int n = snprintf(buf, bufsz, "{");
    for (uint32_t i = 0; i < f->nlabels; i++) {
        n += snprintf(buf + n, bufsz - (size_t)n,
                      "%s%s=\"%s\"",
                      i ? "," : "",
                      f->label_names[i],
                      s->label_values[i]);
    }
    n += snprintf(buf + n, bufsz - (size_t)n, "}");
    return n;
}

size_t emd_metrics_serialize(char *buf, size_t bufsz) {
    size_t off = 0;
    pthread_mutex_lock(&g_reg.mu);
    for (uint32_t fi = 0; fi < g_reg.count; fi++) {
        struct emd_counter *f = g_reg.families[fi];
        const char *type_str = (f->kind == MF_COUNTER) ? "counter"
                             : (f->kind == MF_GAUGE)   ? "gauge"
                                                        : "histogram";
        int n = snprintf(buf + off, bufsz - off,
                         "# HELP %s %s\n# TYPE %s %s\n",
                         f->name, f->help, f->name, type_str);
        if (n > 0) off += (size_t)n;

        for (uint32_t si = 0; si < MAX_SERIES_PER && off < bufsz; si++) {
            const metric_series_t *s = &f->series[si];
            if (!s->used) continue;

            char lbl[512] = "";
            write_labels(lbl, sizeof(lbl), f, s);

            if (f->kind == MF_COUNTER) {
                uint64_t v = atomic_load_explicit(&s->counter_val, memory_order_relaxed);
                n = snprintf(buf + off, bufsz - off, "%s%s %llu\n",
                             f->name, lbl, (unsigned long long)v);
                if (n > 0) off += (size_t)n;
            } else if (f->kind == MF_GAUGE) {
                double v = bits_to_double(
                    atomic_load_explicit(&s->gauge_bits, memory_order_relaxed));
                n = snprintf(buf + off, bufsz - off, "%s%s %g\n",
                             f->name, lbl, v);
                if (n > 0) off += (size_t)n;
            } else {
                /* histogram */
                uint64_t cumul = 0;
                for (uint32_t bi = 0; bi < f->nbuckets && off < bufsz; bi++) {
                    uint64_t bc = atomic_load_explicit(&s->hist.bucket_counts[bi],
                                                       memory_order_relaxed);
                    cumul += bc;
                    char lbl_le[640];
                    snprintf(lbl_le, sizeof(lbl_le), "{le=\"%g\"", f->buckets[bi]);
                    /* append any existing labels */
                    if (f->nlabels > 0) {
                        for (uint32_t i = 0; i < f->nlabels; i++) {
                            char tmp[128];
                            snprintf(tmp, sizeof(tmp), ",%s=\"%s\"",
                                     f->label_names[i], s->label_values[i]);
                            strncat(lbl_le, tmp, sizeof(lbl_le) - strlen(lbl_le) - 1);
                        }
                    }
                    strncat(lbl_le, "}", sizeof(lbl_le) - strlen(lbl_le) - 1);
                    n = snprintf(buf + off, bufsz - off, "%s_bucket%s %llu\n",
                                 f->name, lbl_le, (unsigned long long)cumul);
                    if (n > 0) off += (size_t)n;
                }
                double sum_v = bits_to_double(
                    atomic_load_explicit(&s->hist.sum_bits, memory_order_relaxed));
                uint64_t cnt = atomic_load_explicit(&s->hist.count, memory_order_relaxed);
                n = snprintf(buf + off, bufsz - off,
                             "%s_sum%s %g\n%s_count%s %llu\n",
                             f->name, lbl, sum_v,
                             f->name, lbl, (unsigned long long)cnt);
                if (n > 0) off += (size_t)n;
            }
        }
    }
    pthread_mutex_unlock(&g_reg.mu);
    return off;
}

/* -------------------------------------------------------------------------
 * Init / destroy
 * ---------------------------------------------------------------------- */
int emd_metrics_init(void) {
    if (g_reg.initialized) return 0;
    pthread_mutex_init(&g_reg.mu, NULL);
    g_reg.initialized = true;
    return emd_metrics_register_builtin();
}

void emd_metrics_destroy(void) {
    pthread_mutex_lock(&g_reg.mu);
    for (uint32_t i = 0; i < g_reg.count; i++) {
        pthread_mutex_destroy(&g_reg.families[i]->mu);
        free(g_reg.families[i]);
        g_reg.families[i] = NULL;
    }
    g_reg.count = 0;
    g_reg.initialized = false;
    pthread_mutex_unlock(&g_reg.mu);
    pthread_mutex_destroy(&g_reg.mu);
}

int emd_metrics_register_builtin(void) {
    static const char *cam_labels[]  = {"cam"};
    static const char *cam_nal[]     = {"cam", "nal_type"};
    static const char *cam_reason[]  = {"cam", "reason"};
    static const char *cam_type[]    = {"cam", "type", "reason"};
    static const char *result_lbl[]  = {"result"};
    static const char *state_lbl[]   = {"cam", "state"};

    static const double latency_buckets[] = {
        0.0001, 0.0005, 0.001, 0.005, 0.010, 0.025, 0.050, 0.1, 0.25, 0.5, 1.0
    };
    static const double duration_buckets[] = {
        1.0, 5.0, 10.0, 30.0, 60.0, 120.0, 300.0, 600.0
    };

    g_nal_received_total = emd_counter_new(
        "emd_nal_received_total", "Total NAL units received",
        cam_nal, 2);

    g_frames_dropped_total = emd_counter_new(
        "emd_frames_dropped_total", "Total frames dropped",
        cam_reason, 2);

    g_event_total = emd_counter_new(
        "emd_event_total", "Total motion events",
        cam_type, 3);

    g_mqtt_publish_total = emd_counter_new(
        "emd_mqtt_publish_total", "Total MQTT publishes",
        result_lbl, 1);

    g_notifications_dropped_total = emd_counter_new(
        "emd_notifications_dropped_total", "Notifications dropped (queue full)",
        NULL, 0);

    g_rtsp_state = emd_gauge_new(
        "emd_rtsp_state", "RTSP state per camera",
        state_lbl, 2);

    g_inspector_bpf_ewma = emd_gauge_new(
        "emd_inspector_bpf_ewma", "Inspector bytes-per-frame EWMA",
        cam_labels, 1);

    g_mqtt_connected = emd_gauge_new(
        "emd_mqtt_connected", "MQTT connection status (1=connected)",
        NULL, 0);

    g_recorder_queue_depth = emd_gauge_new(
        "emd_recorder_queue_depth", "Recorder event queue depth",
        NULL, 0);

    g_recording_seconds = emd_histogram_new(
        "emd_recording_seconds", "Duration of written clips",
        duration_buckets,
        (uint32_t)(sizeof(duration_buckets) / sizeof(duration_buckets[0])),
        cam_labels, 1);

    g_worker_loop_latency_seconds = emd_histogram_new(
        "emd_worker_loop_latency_seconds", "Camera worker loop iteration latency",
        latency_buckets,
        (uint32_t)(sizeof(latency_buckets) / sizeof(latency_buckets[0])),
        cam_labels, 1);

    return 0;
}

#include "emd/log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <pthread.h>

/* -------------------------------------------------------------------------
 * Global state
 * ---------------------------------------------------------------------- */
static _Atomic int   g_log_level     = EMD_LOG_INFO;
static _Atomic uint32_t g_hotpath_rate = 0; /* 0 = unlimited */

/* Per-camera hot-path rate limiter (simple token bucket, best-effort) */
#define MAX_CAM_RL 64
static struct {
    uint16_t cam_id;
    uint64_t last_sec;
    uint32_t count;
} g_rl[MAX_CAM_RL];
static pthread_mutex_t g_rl_mutex = PTHREAD_MUTEX_INITIALIZER;

/* -------------------------------------------------------------------------
 * API
 * ---------------------------------------------------------------------- */
int emd_log_level_from_str(const char *s) {
    if (!s) return -1;
    if (strcmp(s, "trace") == 0) return EMD_LOG_TRACE;
    if (strcmp(s, "debug") == 0) return EMD_LOG_DEBUG;
    if (strcmp(s, "info")  == 0) return EMD_LOG_INFO;
    if (strcmp(s, "warn")  == 0) return EMD_LOG_WARN;
    if (strcmp(s, "error") == 0) return EMD_LOG_ERROR;
    if (strcmp(s, "fatal") == 0) return EMD_LOG_FATAL;
    return -1;
}

void emd_log_set_level(emd_log_level_t level) {
    atomic_store_explicit(&g_log_level, (int)level, memory_order_release);
}

void emd_log_set_hotpath_rate(uint32_t lines_per_sec) {
    atomic_store_explicit(&g_hotpath_rate, lines_per_sec, memory_order_release);
}

static const char *level_str(emd_log_level_t l) {
    switch (l) {
        case EMD_LOG_TRACE: return "trace";
        case EMD_LOG_DEBUG: return "debug";
        case EMD_LOG_INFO:  return "info";
        case EMD_LOG_WARN:  return "warn";
        case EMD_LOG_ERROR: return "error";
        case EMD_LOG_FATAL: return "fatal";
        default:            return "unknown";
    }
}

/* Simple JSON string escape into buf; returns bytes written (not including NUL). */
static int json_escape(const char *src, char *buf, size_t bufsz) {
    size_t out = 0;
    for (size_t i = 0; src[i] && out + 2 < bufsz; i++) {
        unsigned char c = (unsigned char)src[i];
        if (c == '"' || c == '\\') {
            buf[out++] = '\\';
            if (out < bufsz) buf[out++] = (char)c;
        } else if (c < 0x20u) {
            /* control character — write \uXXXX */
            int n = snprintf(buf + out, bufsz - out, "\\u%04x", (unsigned)c);
            if (n > 0) out += (size_t)n;
        } else {
            buf[out++] = (char)c;
        }
    }
    buf[out] = '\0';
    return (int)out;
}

/* Check hot-path rate limit.  Returns true if the message should be printed. */
static bool rl_check(uint16_t cam_id, uint64_t now_sec) {
    uint32_t rate = atomic_load_explicit(&g_hotpath_rate, memory_order_acquire);
    if (rate == 0) return true;

    pthread_mutex_lock(&g_rl_mutex);
    bool allow = true;
    /* Find or create slot */
    int found = -1;
    for (int i = 0; i < MAX_CAM_RL; i++) {
        if (g_rl[i].cam_id == cam_id) { found = i; break; }
    }
    if (found < 0) {
        /* Find free slot */
        for (int i = 0; i < MAX_CAM_RL; i++) {
            if (g_rl[i].cam_id == 0) { found = i; g_rl[i].cam_id = cam_id; break; }
        }
    }
    if (found >= 0) {
        if (g_rl[found].last_sec != now_sec) {
            g_rl[found].last_sec = now_sec;
            g_rl[found].count = 0;
        }
        if (g_rl[found].count >= rate) {
            allow = false;
        } else {
            g_rl[found].count++;
        }
    }
    pthread_mutex_unlock(&g_rl_mutex);
    return allow;
}

void emd_log_write(emd_log_level_t level,
                   const char *subsystem,
                   uint16_t cam_id,
                   const char *event_id,
                   const char *msg,
                   const char *extra_json)
{
    int cur = atomic_load_explicit(&g_log_level, memory_order_acquire);
    if ((int)level < cur) return;

    /* Rate-limit DEBUG on hot path */
    if (level == EMD_LOG_DEBUG && cam_id != EMD_NO_CAM) {
        struct timespec ts_now;
        clock_gettime(CLOCK_REALTIME, &ts_now);
        uint64_t now_sec = (uint64_t)ts_now.tv_sec;
        if (!rl_check(cam_id, now_sec)) return;
    }

    /* Get wall-clock */
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm_info;
    gmtime_r(&ts.tv_sec, &tm_info);
    char ts_buf[32];
    strftime(ts_buf, sizeof(ts_buf), "%Y-%m-%dT%H:%M:%S", &tm_info);
    int ms = (int)(ts.tv_nsec / 1000000L);

    /* Escape strings */
    char esc_subsys[128], esc_msg[512];
    if (subsystem) {
        json_escape(subsystem, esc_subsys, sizeof(esc_subsys));
    } else {
        esc_subsys[0] = '\0';
    }
    if (msg) {
        json_escape(msg, esc_msg, sizeof(esc_msg));
    } else {
        esc_msg[0] = '\0';
    }

    /* Build JSON line */
    char line[2048];
    int n = snprintf(line, sizeof(line),
        "{\"ts\":\"%s.%03dZ\","
        "\"level\":\"%s\","
        "\"subsystem\":\"%s\",",
        ts_buf, ms,
        level_str(level),
        esc_subsys);

    if (cam_id != EMD_NO_CAM && n > 0 && (size_t)n < sizeof(line)) {
        n += snprintf(line + n, sizeof(line) - (size_t)n,
                      "\"cam_id\":%u,", (unsigned)cam_id);
    }

    if (event_id && n > 0 && (size_t)n < sizeof(line)) {
        char esc_evid[64];
        json_escape(event_id, esc_evid, sizeof(esc_evid));
        n += snprintf(line + n, sizeof(line) - (size_t)n,
                      "\"event_id\":\"%s\",", esc_evid);
    }

    if (n > 0 && (size_t)n < sizeof(line)) {
        n += snprintf(line + n, sizeof(line) - (size_t)n,
                      "\"msg\":\"%s\"", esc_msg);
    }

    if (extra_json && extra_json[0] && n > 0 && (size_t)n < sizeof(line)) {
        n += snprintf(line + n, sizeof(line) - (size_t)n,
                      ",%s", extra_json);
    }

    if (n > 0 && (size_t)n < sizeof(line)) {
        n += snprintf(line + n, sizeof(line) - (size_t)n, "}\n");
    }

    if (n > 0) {
        /* Write atomically to stderr */
        fwrite(line, 1, (size_t)n, stderr);
    }

    if (level == EMD_LOG_FATAL) {
        abort();
    }
}

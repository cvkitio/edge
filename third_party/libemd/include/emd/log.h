#pragma once
#ifndef EMD_LOG_H
#define EMD_LOG_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    EMD_LOG_TRACE = 0,
    EMD_LOG_DEBUG,
    EMD_LOG_INFO,
    EMD_LOG_WARN,
    EMD_LOG_ERROR,
    EMD_LOG_FATAL,
} emd_log_level_t;

/* Parse a level string ("info", "debug", …).  Returns -1 on unknown. */
int emd_log_level_from_str(const char *s);

/* Set global log level.  Thread-safe (atomic store). */
void emd_log_set_level(emd_log_level_t level);

/* Rate-limit hot-path DEBUG lines: max lines/sec per camera (0 = unlimited). */
void emd_log_set_hotpath_rate(uint32_t lines_per_sec);

/*
 * Core logging function.  Emits a single JSON line to stderr.
 * Fields:
 *   ts         – ISO-8601 wall clock
 *   level      – "trace"|"debug"|"info"|"warn"|"error"|"fatal"
 *   subsystem  – caller-supplied string
 *   cam_id     – 0xFFFF means "not applicable"
 *   event_id   – NULL means "not in an event context"
 *   msg        – free-form message
 *   … extra_json – optional additional JSON fragment appended (can be NULL)
 */
void emd_log_write(emd_log_level_t level,
                   const char *subsystem,
                   uint16_t cam_id,
                   const char *event_id,
                   const char *msg,
                   const char *extra_json);

/* Convenience macros */
#define EMD_NO_CAM   ((uint16_t)0xFFFFu)

#define EMD_LOG(level, subsys, cam, evid, msg, extra) \
    emd_log_write((level), (subsys), (uint16_t)(cam), (evid), (msg), (extra))

#define EMD_LOGI(subsys, msg) \
    emd_log_write(EMD_LOG_INFO,  (subsys), EMD_NO_CAM, NULL, (msg), NULL)

#define EMD_LOGW(subsys, msg) \
    emd_log_write(EMD_LOG_WARN,  (subsys), EMD_NO_CAM, NULL, (msg), NULL)

#define EMD_LOGE(subsys, msg) \
    emd_log_write(EMD_LOG_ERROR, (subsys), EMD_NO_CAM, NULL, (msg), NULL)

#define EMD_LOGF(subsys, msg) \
    emd_log_write(EMD_LOG_FATAL, (subsys), EMD_NO_CAM, NULL, (msg), NULL)

#ifdef __cplusplus
}
#endif

#endif /* EMD_LOG_H */

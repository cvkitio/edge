#pragma once
#ifndef EMD_SUPERVISOR_H
#define EMD_SUPERVISOR_H

#include <stdint.h>
#include <stdbool.h>
#include "emd/config.h"
#include "emd/event.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Camera worker state (managed by supervisor) */
typedef enum {
    EMD_WORKER_STOPPED = 0,
    EMD_WORKER_STARTING,
    EMD_WORKER_RUNNING,
    EMD_WORKER_RESTARTING,
    EMD_WORKER_FAILED,
} emd_worker_state_t;

typedef struct {
    uint16_t          cam_id;
    emd_worker_state_t state;
    uint32_t          restart_count;
    uint32_t          backoff_secs;
    uint64_t          last_restart_mono_ns;
    /* pthread_t is OS-specific; store as void* for portability in header */
    void             *thread_handle;
} emd_worker_slot_t;

/* Supervisor context */
typedef struct emd_supervisor emd_supervisor_t;

/*
 * Create and run the supervisor.
 * This is the main entry point after argument parsing.
 * Blocks until shutdown (SIGTERM/SIGINT).
 * Returns exit code.
 */
int emd_supervisor_run(const char *config_path);

/*
 * Signal the supervisor to reload config (called from signal handler or
 * from the MQTT cmd/reload handler).
 */
void emd_supervisor_request_reload(void);

/*
 * Signal the supervisor to shut down cleanly.
 */
void emd_supervisor_request_shutdown(void);

/*
 * sd_notify wrapper — silently ignored if not running under systemd.
 */
void emd_sdnotify(const char *msg);

#ifdef __cplusplus
}
#endif

#endif /* EMD_SUPERVISOR_H */

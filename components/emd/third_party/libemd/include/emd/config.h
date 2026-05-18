#pragma once
#ifndef EMD_CONFIG_H
#define EMD_CONFIG_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define EMD_MAX_CAMERAS      64
#define EMD_MAX_CAM_NAME_LEN 64
#define EMD_MAX_URL_LEN      512
#define EMD_MAX_PATH_LEN     512

/* Fsync policy for recorded clips */
typedef enum {
    EMD_FSYNC_ON_CLOSE = 0,
    EMD_FSYNC_ALWAYS,
    EMD_FSYNC_NEVER,
} emd_fsync_policy_t;

/* Container format */
typedef enum {
    EMD_CONTAINER_MPEGTS = 0,
    EMD_CONTAINER_FMP4,
} emd_container_t;

/* Camera transport */
typedef enum {
    EMD_TRANSPORT_TCP = 0,
    EMD_TRANSPORT_UDP,
} emd_transport_t;

/* Codec hint */
typedef enum {
    EMD_CODEC_AUTO = 0,
    EMD_CODEC_H264,
    EMD_CODEC_H265,
} emd_codec_hint_t;

/* Per-camera configuration */
typedef struct {
    char            name[EMD_MAX_CAM_NAME_LEN];
    char            url[EMD_MAX_URL_LEN];
    uint16_t        cam_id;                  /* auto-assigned index */
    emd_transport_t transport;
    emd_codec_hint_t codec_hint;

    uint32_t        buffer_seconds;
    uint32_t        buffer_frames;           /* 0 = use buffer_seconds */
    uint32_t        pre_roll_seconds;
    uint32_t        post_roll_seconds;
    uint32_t        clip_max_seconds;

    uint32_t        max_bitrate_bps;         /* 0 → default 8 Mbit/s */

    double          motion_z_high;
    double          intra_ratio_high;
    uint8_t         on_threshold;
    uint8_t         off_threshold;

    bool            gradual_enabled;
    double          gradual_threshold;
    uint32_t        gradual_window_seconds;

    bool            configured_periodic_kf;  /* camera sends IDRs on a schedule */
} emd_camera_cfg_t;

/* Global runtime config */
typedef struct {
    /* [runtime] */
    int             log_level;               /* emd_log_level_t value */
    char            metrics_listen[64];      /* e.g. "0.0.0.0:9464" */
    char            clip_root[EMD_MAX_PATH_LEN];
    char            inflight_root[EMD_MAX_PATH_LEN];

    /* [recording] */
    emd_container_t   container;
    emd_fsync_policy_t fsync_policy;

    /* [mqtt] */
    char            mqtt_url[EMD_MAX_URL_LEN];
    char            mqtt_client_id_prefix[64];
    uint8_t         mqtt_qos;
    char            mqtt_tls_ca_file[EMD_MAX_PATH_LEN];

    /* [disk] */
    uint64_t        disk_max_bytes_per_camera;
    uint32_t        disk_retention_days;

    /* cameras */
    uint32_t        num_cameras;
    emd_camera_cfg_t cameras[EMD_MAX_CAMERAS];
} emd_config_t;

/*
 * Parse a TOML config file.  Returns 0 on success, -1 on error.
 * On error, writes a precise message to *errbuf (up to errbuf_len bytes).
 */
int emd_config_load(const char *path, emd_config_t *cfg,
                    char *errbuf, size_t errbuf_len);

/*
 * Validate an already-parsed config.  Returns 0 if valid.
 * Used by hotreload: parse into a temp struct, validate, then swap.
 */
int emd_config_validate(const emd_config_t *cfg,
                        char *errbuf, size_t errbuf_len);

/* Free any heap resources inside cfg (currently none, all fixed-size). */
void emd_config_free(emd_config_t *cfg);

#ifdef __cplusplus
}
#endif

#endif /* EMD_CONFIG_H */

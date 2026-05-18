#include "emd/config.h"
#include "emd/log.h"
#include "toml.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

/* -------------------------------------------------------------------------
 * Helpers
 * ---------------------------------------------------------------------- */
#define ERR(buf, len, ...) snprintf((buf), (len), __VA_ARGS__)

static int parse_transport(const char *s, emd_transport_t *out) {
    if (!s) { *out = EMD_TRANSPORT_TCP; return 0; }
    if (strcmp(s, "tcp") == 0) { *out = EMD_TRANSPORT_TCP; return 0; }
    if (strcmp(s, "udp") == 0) { *out = EMD_TRANSPORT_UDP; return 0; }
    return -1;
}

static int parse_codec_hint(const char *s, emd_codec_hint_t *out) {
    if (!s || strcmp(s, "auto") == 0) { *out = EMD_CODEC_AUTO; return 0; }
    if (strcmp(s, "h264") == 0) { *out = EMD_CODEC_H264; return 0; }
    if (strcmp(s, "h265") == 0) { *out = EMD_CODEC_H265; return 0; }
    return -1;
}

static int parse_container(const char *s, emd_container_t *out) {
    if (!s || strcmp(s, "mpegts") == 0) { *out = EMD_CONTAINER_MPEGTS; return 0; }
    if (strcmp(s, "fmp4") == 0) { *out = EMD_CONTAINER_FMP4; return 0; }
    return -1;
}

static int parse_fsync(const char *s, emd_fsync_policy_t *out) {
    if (!s || strcmp(s, "on_close") == 0) { *out = EMD_FSYNC_ON_CLOSE; return 0; }
    if (strcmp(s, "always") == 0) { *out = EMD_FSYNC_ALWAYS; return 0; }
    if (strcmp(s, "never")  == 0) { *out = EMD_FSYNC_NEVER;  return 0; }
    return -1;
}

/* Safe copy with truncation check */
static int safe_copy(char *dst, size_t dst_sz, const char *src,
                     const char *key, char *errbuf, size_t eblen) {
    if (strlen(src) >= dst_sz) {
        snprintf(errbuf, eblen, "key=%s: value too long (max %zu chars)", key, dst_sz - 1);
        return -1;
    }
    strncpy(dst, src, dst_sz - 1);
    dst[dst_sz - 1] = '\0';
    return 0;
}

/* -------------------------------------------------------------------------
 * Camera parsing
 * ---------------------------------------------------------------------- */
static int parse_camera(toml_table_t *cam_tbl, const char *cam_name,
                         uint16_t cam_id,
                         emd_camera_cfg_t *cam,
                         char *errbuf, size_t eblen)
{
    memset(cam, 0, sizeof(*cam));
    cam->cam_id = cam_id;

    /* Defaults */
    cam->buffer_seconds    = 10;
    cam->pre_roll_seconds  = 6;
    cam->post_roll_seconds = 8;
    cam->clip_max_seconds  = 120;
    cam->max_bitrate_bps   = 0; /* → 8 Mbit/s default in ringbuf */
    cam->motion_z_high     = 3.0;
    cam->intra_ratio_high  = 2.5;
    cam->on_threshold      = 2;
    cam->off_threshold     = 0; /* computed later */
    cam->gradual_threshold = 0.4;
    cam->gradual_window_seconds = 30;

    if (safe_copy(cam->name, sizeof(cam->name), cam_name, "camera.name", errbuf, eblen) < 0)
        return -1;

    toml_datum_t d;

    /* url (mandatory) */
    d = toml_string_in(cam_tbl, "url");
    if (!d.ok) {
        snprintf(errbuf, eblen, "key=cameras.%s.url: required field missing", cam_name);
        return -1;
    }
    int rc = safe_copy(cam->url, sizeof(cam->url), d.u.s, "url", errbuf, eblen);
    free(d.u.s);
    if (rc < 0) return -1;

    /* transport */
    d = toml_string_in(cam_tbl, "transport");
    if (d.ok) {
        if (parse_transport(d.u.s, &cam->transport) < 0) {
            snprintf(errbuf, eblen, "key=cameras.%s.transport: unknown value '%s'", cam_name, d.u.s);
            free(d.u.s);
            return -1;
        }
        free(d.u.s);
    }

    /* codec_hint */
    d = toml_string_in(cam_tbl, "codec_hint");
    if (d.ok) {
        if (parse_codec_hint(d.u.s, &cam->codec_hint) < 0) {
            snprintf(errbuf, eblen, "key=cameras.%s.codec_hint: unknown value '%s'", cam_name, d.u.s);
            free(d.u.s);
            return -1;
        }
        free(d.u.s);
    }

    /* buffer_seconds */
    d = toml_int_in(cam_tbl, "buffer_seconds");
    if (d.ok) cam->buffer_seconds = (uint32_t)d.u.i;

    d = toml_int_in(cam_tbl, "buffer_frames");
    if (d.ok) cam->buffer_frames = (uint32_t)d.u.i;

    d = toml_int_in(cam_tbl, "pre_roll_seconds");
    if (d.ok) cam->pre_roll_seconds = (uint32_t)d.u.i;

    d = toml_int_in(cam_tbl, "post_roll_seconds");
    if (d.ok) cam->post_roll_seconds = (uint32_t)d.u.i;

    d = toml_int_in(cam_tbl, "clip_max_seconds");
    if (d.ok) cam->clip_max_seconds = (uint32_t)d.u.i;

    d = toml_double_in(cam_tbl, "motion_z_high");
    if (d.ok) cam->motion_z_high = d.u.d;

    d = toml_double_in(cam_tbl, "intra_ratio_high");
    if (d.ok) cam->intra_ratio_high = d.u.d;

    d = toml_bool_in(cam_tbl, "gradual_enabled");
    if (d.ok) cam->gradual_enabled = (bool)d.u.b;

    d = toml_bool_in(cam_tbl, "configured_periodic_kf");
    if (d.ok) cam->configured_periodic_kf = (bool)d.u.b;

    d = toml_int_in(cam_tbl, "min_bytes_threshold");
    if (d.ok) {
        if (d.u.i < 0 || d.u.i > 10000000) {
            snprintf(errbuf, eblen, "key=cameras.%s.min_bytes_threshold: out of range [0, 10000000]", cam_name);
            return -1;
        }
        cam->min_bytes_threshold = (uint32_t)d.u.i;
    }

    d = toml_double_in(cam_tbl, "bpf_relative_floor");
    if (d.ok) {
        if (d.u.d < 0.0 || d.u.d > 100.0) {
            snprintf(errbuf, eblen, "key=cameras.%s.bpf_relative_floor: out of range [0.0, 100.0]", cam_name);
            return -1;
        }
        cam->bpf_relative_floor = d.u.d;
    }

    d = toml_double_in(cam_tbl, "z_high_warmup");
    if (d.ok) {
        if (d.u.d < 0.0 || d.u.d > 100.0) {
            snprintf(errbuf, eblen, "key=cameras.%s.z_high_warmup: out of range [0.0, 100.0]", cam_name);
            return -1;
        }
        cam->z_high_warmup = d.u.d;
    }

    d = toml_int_in(cam_tbl, "z_high_warmup_frames");
    if (d.ok) {
        if (d.u.i < 0 || d.u.i > 600) {
            snprintf(errbuf, eblen, "key=cameras.%s.z_high_warmup_frames: out of range [0, 600]", cam_name);
            return -1;
        }
        cam->z_high_warmup_frames = (uint16_t)d.u.i;
    }

    /* target_classes: string array → bitmask */
    /* NOTE: Disabled - tomlc99 minimal parser doesn't support arrays.
     * This feature is reserved for future implementation with a full TOML library.
    toml_array_t *tc_arr = toml_array_in(cam_tbl, "target_classes");
    if (tc_arr) {
        int tc_n = toml_array_nelem(tc_arr);
        for (int j = 0; j < tc_n; j++) {
            toml_datum_t td = toml_string_at(tc_arr, j);
            if (!td.ok) continue;
            if (strcmp(td.u.s, "person")  == 0) cam->target_class_mask |= (uint8_t)(1u << 0);
            else if (strcmp(td.u.s, "vehicle") == 0) cam->target_class_mask |= (uint8_t)(1u << 1);
            else if (strcmp(td.u.s, "animal")  == 0) cam->target_class_mask |= (uint8_t)(1u << 2);
            else if (strcmp(td.u.s, "other")   == 0) cam->target_class_mask |= (uint8_t)(1u << 7);
            else {
                snprintf(errbuf, eblen,
                         "key=cameras.%s.target_classes: unknown class '%s' (known: person, vehicle, animal, other)",
                         cam_name, td.u.s);
                free(td.u.s);
                return -1;
            }
            free(td.u.s);
        }
    }
    */

    return 0;
}

/* -------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------- */
int emd_config_load(const char *path, emd_config_t *cfg,
                    char *errbuf, size_t errbuf_len)
{
    memset(cfg, 0, sizeof(*cfg));

    /* Set global defaults */
    cfg->log_level = EMD_LOG_INFO;
    strncpy(cfg->metrics_listen, "0.0.0.0:9464", sizeof(cfg->metrics_listen) - 1);
    strncpy(cfg->clip_root, "/var/lib/emd/clips", sizeof(cfg->clip_root) - 1);
    strncpy(cfg->inflight_root, "/var/lib/emd/inflight", sizeof(cfg->inflight_root) - 1);
    cfg->container = EMD_CONTAINER_MPEGTS;
    cfg->fsync_policy = EMD_FSYNC_ON_CLOSE;
    cfg->mqtt_qos = 1;
    cfg->disk_max_bytes_per_camera = 20ULL * 1000ULL * 1000ULL * 1000ULL;
    cfg->disk_retention_days = 14;

    FILE *fp = fopen(path, "r");
    if (!fp) {
        snprintf(errbuf, errbuf_len, "cannot open config file: %s", path);
        return -1;
    }

    char toml_errbuf[256];
    toml_table_t *root = toml_parse_file(fp, toml_errbuf, sizeof(toml_errbuf));
    fclose(fp);

    if (!root) {
        snprintf(errbuf, errbuf_len, "TOML parse error: %s", toml_errbuf);
        return -1;
    }

    /* [runtime] */
    toml_table_t *runtime = toml_table_in(root, "runtime");
    if (runtime) {
        toml_datum_t d;
        d = toml_string_in(runtime, "log_level");
        if (d.ok) {
            int lv = emd_log_level_from_str(d.u.s);
            free(d.u.s);
            if (lv < 0) {
                snprintf(errbuf, errbuf_len, "key=runtime.log_level: unknown level");
                toml_free(root);
                return -1;
            }
            cfg->log_level = lv;
        }
        d = toml_string_in(runtime, "metrics_listen");
        if (d.ok) {
            safe_copy(cfg->metrics_listen, sizeof(cfg->metrics_listen), d.u.s,
                      "runtime.metrics_listen", errbuf, errbuf_len);
            free(d.u.s);
        }
        d = toml_string_in(runtime, "clip_root");
        if (d.ok) {
            safe_copy(cfg->clip_root, sizeof(cfg->clip_root), d.u.s,
                      "runtime.clip_root", errbuf, errbuf_len);
            free(d.u.s);
        }
        d = toml_string_in(runtime, "inflight_root");
        if (d.ok) {
            safe_copy(cfg->inflight_root, sizeof(cfg->inflight_root), d.u.s,
                      "runtime.inflight_root", errbuf, errbuf_len);
            free(d.u.s);
        }
    }

    /* [recording] */
    toml_table_t *recording = toml_table_in(root, "recording");
    if (recording) {
        toml_datum_t d;
        d = toml_string_in(recording, "container");
        if (d.ok) {
            if (parse_container(d.u.s, &cfg->container) < 0) {
                snprintf(errbuf, errbuf_len,
                         "key=recording.container: unknown value '%s'", d.u.s);
                free(d.u.s);
                toml_free(root);
                return -1;
            }
            free(d.u.s);
        }
        d = toml_string_in(recording, "fsync_policy");
        if (d.ok) {
            if (parse_fsync(d.u.s, &cfg->fsync_policy) < 0) {
                snprintf(errbuf, errbuf_len,
                         "key=recording.fsync_policy: unknown value '%s'", d.u.s);
                free(d.u.s);
                toml_free(root);
                return -1;
            }
            free(d.u.s);
        }
    }

    /* [mqtt] */
    toml_table_t *mqtt = toml_table_in(root, "mqtt");
    if (mqtt) {
        toml_datum_t d;
        d = toml_string_in(mqtt, "url");
        if (d.ok) {
            safe_copy(cfg->mqtt_url, sizeof(cfg->mqtt_url), d.u.s,
                      "mqtt.url", errbuf, errbuf_len);
            free(d.u.s);
        }
        d = toml_string_in(mqtt, "client_id_prefix");
        if (d.ok) {
            safe_copy(cfg->mqtt_client_id_prefix, sizeof(cfg->mqtt_client_id_prefix),
                      d.u.s, "mqtt.client_id_prefix", errbuf, errbuf_len);
            free(d.u.s);
        }
        d = toml_int_in(mqtt, "qos");
        if (d.ok) cfg->mqtt_qos = (uint8_t)d.u.i;
        d = toml_string_in(mqtt, "tls_ca_file");
        if (d.ok) {
            safe_copy(cfg->mqtt_tls_ca_file, sizeof(cfg->mqtt_tls_ca_file),
                      d.u.s, "mqtt.tls_ca_file", errbuf, errbuf_len);
            free(d.u.s);
        }
    }

    /* [disk] */
    toml_table_t *disk = toml_table_in(root, "disk");
    if (disk) {
        toml_datum_t d;
        d = toml_int_in(disk, "max_bytes_per_camera");
        if (d.ok) cfg->disk_max_bytes_per_camera = (uint64_t)d.u.i;
        d = toml_int_in(disk, "retention_days");
        if (d.ok) cfg->disk_retention_days = (uint32_t)d.u.i;
    }

    /* [cameras] */
    toml_table_t *cameras = toml_table_in(root, "cameras");
    if (cameras) {
        int n = toml_table_ntab(cameras);
        for (int i = 0; i < n; i++) {
            const char *cam_name = toml_key_in(cameras, i);
            if (!cam_name) continue;
            toml_table_t *cam_tbl = toml_table_in(cameras, cam_name);
            if (!cam_tbl) continue;

            if (cfg->num_cameras >= EMD_MAX_CAMERAS) {
                snprintf(errbuf, errbuf_len,
                         "too many cameras (max %d)", EMD_MAX_CAMERAS);
                toml_free(root);
                return -1;
            }

            if (parse_camera(cam_tbl, cam_name, (uint16_t)cfg->num_cameras,
                             &cfg->cameras[cfg->num_cameras],
                             errbuf, errbuf_len) < 0) {
                toml_free(root);
                return -1;
            }
            cfg->num_cameras++;
        }
    }

    toml_free(root);
    return emd_config_validate(cfg, errbuf, errbuf_len);
}

int emd_config_validate(const emd_config_t *cfg,
                         char *errbuf, size_t errbuf_len)
{
    for (uint32_t i = 0; i < cfg->num_cameras; i++) {
        const emd_camera_cfg_t *c = &cfg->cameras[i];
        if (c->url[0] == '\0') {
            snprintf(errbuf, errbuf_len,
                     "key=cameras.%s.url: empty URL not allowed", c->name);
            return -1;
        }
        if (c->buffer_seconds == 0 && c->buffer_frames == 0) {
            snprintf(errbuf, errbuf_len,
                     "key=cameras.%s: buffer_seconds or buffer_frames must be >0", c->name);
            return -1;
        }
        if (c->pre_roll_seconds > c->buffer_seconds && c->buffer_frames == 0) {
            snprintf(errbuf, errbuf_len,
                     "key=cameras.%s: pre_roll_seconds (%u) > buffer_seconds (%u)",
                     c->name, c->pre_roll_seconds, c->buffer_seconds);
            return -1;
        }
        if (c->motion_z_high <= 0.0) {
            snprintf(errbuf, errbuf_len,
                     "key=cameras.%s.motion_z_high: must be positive", c->name);
            return -1;
        }
    }
    if (cfg->mqtt_qos > 2) {
        snprintf(errbuf, errbuf_len, "key=mqtt.qos: must be 0, 1, or 2");
        return -1;
    }
    return 0;
}

void emd_config_free(emd_config_t *cfg) {
    /* All storage is in-struct; nothing to free. */
    memset(cfg, 0, sizeof(*cfg));
}

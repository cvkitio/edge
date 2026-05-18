#pragma once
#ifndef EMD_MQTT_H
#define EMD_MQTT_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define EMD_MQTT_MAX_TOPIC_LEN   256
#define EMD_MQTT_MAX_PAYLOAD_LEN (64 * 1024)

typedef enum {
    EMD_MQTT_QOS0 = 0,
    EMD_MQTT_QOS1 = 1,
    EMD_MQTT_QOS2 = 2,
} emd_mqtt_qos_t;

/* Reconnect state */
typedef enum {
    EMD_MQTT_DISCONNECTED = 0,
    EMD_MQTT_CONNECTING,
    EMD_MQTT_CONNECTED,
} emd_mqtt_state_t;

/* Inbound command callback */
typedef void (*emd_mqtt_cmd_cb)(const char *topic, const uint8_t *payload,
                                 size_t payload_len, void *userdata);

typedef struct emd_mqtt_client emd_mqtt_client_t;

typedef struct {
    char            url[512];           /* mqtts://host:port or mqtt://host:port */
    char            client_id[128];
    char            tls_ca_file[512];
    uint8_t         default_qos;
    uint32_t        queue_max;          /* max queued events while offline */
    char            instance_id[64];    /* for topic construction */

    /* LWT */
    char            lwt_topic[EMD_MQTT_MAX_TOPIC_LEN];
    char            lwt_payload[256];
} emd_mqtt_cfg_t;

/*
 * Create an MQTT client.  Does not connect until emd_mqtt_start() is called.
 */
emd_mqtt_client_t *emd_mqtt_client_new(const emd_mqtt_cfg_t *cfg,
                                        emd_mqtt_cmd_cb cmd_cb,
                                        void *userdata);

void emd_mqtt_client_free(emd_mqtt_client_t *c);

/* Start the notifier thread (MQTT loop). */
int emd_mqtt_start(emd_mqtt_client_t *c);

/* Stop the notifier thread (join). */
void emd_mqtt_stop(emd_mqtt_client_t *c);

/* Current connection state. */
emd_mqtt_state_t emd_mqtt_state(const emd_mqtt_client_t *c);

/*
 * Publish a message.  Thread-safe (enqueues to the notifier loop).
 * Returns 0 on success, -1 if the queue is full (event dropped).
 */
int emd_mqtt_publish(emd_mqtt_client_t *c,
                     const char *topic,
                     const uint8_t *payload, size_t payload_len,
                     emd_mqtt_qos_t qos, bool retain);

/* Convenience: publish a NUL-terminated string payload. */
int emd_mqtt_publish_str(emd_mqtt_client_t *c, const char *topic,
                          const char *payload_str,
                          emd_mqtt_qos_t qos, bool retain);

/* -------------------------------------------------------------------------
 * Topic helpers
 * ---------------------------------------------------------------------- */

/* Build the status topic for this instance. */
void emd_mqtt_topic_status(const char *instance, char *buf, size_t bufsz);
void emd_mqtt_topic_event(const char *instance, const char *cam_id,
                           char *buf, size_t bufsz);
void emd_mqtt_topic_clip(const char *instance, const char *cam_id,
                          char *buf, size_t bufsz);
void emd_mqtt_topic_stats(const char *instance, const char *cam_id,
                           char *buf, size_t bufsz);
void emd_mqtt_topic_cmd_sub(const char *instance, char *buf, size_t bufsz);

/* -------------------------------------------------------------------------
 * JSON payload builders
 * ---------------------------------------------------------------------- */
#include "emd/event.h"
#include "emd/recorder.h"

int emd_mqtt_build_event_payload(const emd_event_t *ev,
                                  const char *instance,
                                  char *buf, size_t bufsz);

int emd_mqtt_build_clip_payload(const emd_clip_header_t *hdr,
                                 const emd_event_t *ev,
                                 const char *instance,
                                 char *buf, size_t bufsz);

int emd_mqtt_build_status_payload(bool online, const char *reason,
                                   char *buf, size_t bufsz);

#ifdef __cplusplus
}
#endif

#endif /* EMD_MQTT_H */

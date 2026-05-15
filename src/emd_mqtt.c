/*
 * emd_mqtt.c — MQTT client front-end.
 *
 * Owns its own notifier thread.  Internally uses the vendored mqtt-c
 * wire protocol builder and a plain TCP socket.
 * Reconnect: exponential backoff [1, 2, 4, 8, 16, 30] seconds.
 */

#include "emd/mqtt.h"
#include "emd/event.h"
#include "emd/recorder.h"
#include "emd/net.h"
#include "emd/log.h"
#include "mqtt.h"   /* vendored mqtt-c wire protocol */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>
#include <inttypes.h>

/* ---------------------------------------------------------------------- */
/* Internal queue for outbound messages                                     */
/* ---------------------------------------------------------------------- */

#define MQTT_QUEUE_SIZE  512

typedef struct {
    char    topic[EMD_MQTT_MAX_TOPIC_LEN];
    uint8_t payload[EMD_MQTT_MAX_PAYLOAD_LEN];
    size_t  payload_len;
    uint8_t qos;
    bool    retain;
} mqtt_msg_t;

/* Simple lock-based bounded queue (the notifier path is not ultra-hot) */
typedef struct {
    mqtt_msg_t  slots[MQTT_QUEUE_SIZE];
    uint32_t    head;
    uint32_t    tail;
    uint32_t    count;
    pthread_mutex_t mu;
    pthread_cond_t  cond;
} mqtt_queue_t;

static void mq_init(mqtt_queue_t *q) {
    memset(q, 0, sizeof(*q));
    pthread_mutex_init(&q->mu, NULL);
    pthread_cond_init(&q->cond, NULL);
}

static int mq_push(mqtt_queue_t *q, const mqtt_msg_t *m, uint32_t max_size) {
    pthread_mutex_lock(&q->mu);
    if (q->count >= max_size) {
        pthread_mutex_unlock(&q->mu);
        return -1; /* full */
    }
    q->slots[q->tail % MQTT_QUEUE_SIZE] = *m;
    q->tail++;
    q->count++;
    pthread_cond_signal(&q->cond);
    pthread_mutex_unlock(&q->mu);
    return 0;
}

static int mq_pop_timeout(mqtt_queue_t *q, mqtt_msg_t *m, int timeout_ms) {
    pthread_mutex_lock(&q->mu);
    if (q->count == 0) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec  += timeout_ms / 1000;
        ts.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
        if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
        pthread_cond_timedwait(&q->cond, &q->mu, &ts);
    }
    if (q->count == 0) {
        pthread_mutex_unlock(&q->mu);
        return -1;
    }
    *m = q->slots[q->head % MQTT_QUEUE_SIZE];
    q->head++;
    q->count--;
    pthread_mutex_unlock(&q->mu);
    return 0;
}

/* ---------------------------------------------------------------------- */
/* Backoff table                                                            */
/* ---------------------------------------------------------------------- */
static const uint32_t BACKOFF_SECS[] = {1, 2, 4, 8, 16, 30};
#define N_BACKOFF 6

/* ---------------------------------------------------------------------- */
/* Client struct                                                            */
/* ---------------------------------------------------------------------- */

struct emd_mqtt_client {
    emd_mqtt_cfg_t   cfg;
    emd_mqtt_cmd_cb  cmd_cb;
    void            *userdata;

    /* Thread */
    pthread_t        thread;
    volatile bool    running;

    /* Connection state */
    volatile emd_mqtt_state_t state;
    int              fd;
    uint16_t         next_pkt_id;
    uint64_t         last_ping_ns;

    /* Reconnect */
    int              backoff_idx;
    uint64_t         reconnect_at_ns;

    /* Outbound queue */
    mqtt_queue_t     queue;
};

/* ---------------------------------------------------------------------- */
/* Utility                                                                  */
/* ---------------------------------------------------------------------- */

static uint64_t now_mono_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static uint16_t next_pkt_id(emd_mqtt_client_t *c) {
    c->next_pkt_id++;
    if (c->next_pkt_id == 0) c->next_pkt_id = 1;
    return c->next_pkt_id;
}

/* Parse mqtt[s]://host:port */
static int parse_mqtt_url(const char *url, char *host, size_t hsz,
                            uint16_t *port, bool *tls)
{
    *tls = false;
    const char *p = url;
    if (strncmp(p, "mqtts://", 8) == 0) { *tls = true; p += 8; }
    else if (strncmp(p, "mqtt://", 7) == 0) { p += 7; }
    else return -1;

    const char *colon = strchr(p, ':');
    if (colon) {
        size_t hlen = (size_t)(colon - p);
        if (hlen >= hsz) return -1;
        memcpy(host, p, hlen); host[hlen] = '\0';
        *port = (uint16_t)atoi(colon + 1);
    } else {
        strncpy(host, p, hsz - 1);
        *port = *tls ? 8883u : 1883u;
    }
    return 0;
}

/* ---------------------------------------------------------------------- */
/* Connect / disconnect                                                     */
/* ---------------------------------------------------------------------- */

static int do_connect(emd_mqtt_client_t *c) {
    char host[256]; uint16_t port; bool tls;
    if (parse_mqtt_url(c->cfg.url, host, sizeof(host), &port, &tls) < 0) {
        EMD_LOGE("mqtt", "invalid URL");
        return -1;
    }

    c->fd = emd_tcp_connect(host, port, 5000);
    if (c->fd < 0) return -1;

    /* Build CONNECT packet */
    char client_id[128];
    snprintf(client_id, sizeof(client_id), "%s-%s",
             c->cfg.client_id, c->cfg.instance_id);

    mqtt_connect_opts_t opts = {
        .client_id       = client_id,
        .keepalive_secs  = 60,
        .clean_session   = true,
        .lwt_topic       = c->cfg.lwt_topic[0] ? c->cfg.lwt_topic : NULL,
        .lwt_payload     = c->cfg.lwt_payload[0] ? c->cfg.lwt_payload : NULL,
        .lwt_payload_len = c->cfg.lwt_payload[0] ? strlen(c->cfg.lwt_payload) : 0,
        .lwt_qos         = 1,
        .lwt_retain      = true,
    };

    uint8_t buf[1024];
    int n = mqtt_build_connect(buf, sizeof(buf), &opts);
    if (n < 0) { emd_net_close(c->fd); c->fd = -1; return -1; }

    if (emd_tcp_send_all(c->fd, buf, (size_t)n, 5000) < 0) {
        emd_net_close(c->fd); c->fd = -1; return -1;
    }

    /* Wait for CONNACK */
    uint8_t rbuf[64];
    int r = emd_tcp_recv_exact(c->fd, rbuf, 4, 5000);
    if (r < 0) { emd_net_close(c->fd); c->fd = -1; return -1; }

    mqtt_pkt_t pkt;
    if (mqtt_parse(rbuf, (size_t)r, &pkt) < 0 || pkt.type != MQTT_CONNACK) {
        emd_net_close(c->fd); c->fd = -1; return -1;
    }

    bool sp; uint8_t rc;
    if (mqtt_decode_connack(&pkt, &sp, &rc) < 0 || rc != MQTT_CONNACK_ACCEPTED) {
        EMD_LOGE("mqtt", "CONNACK refused");
        emd_net_close(c->fd); c->fd = -1; return -1;
    }

    c->state = EMD_MQTT_CONNECTED;
    c->last_ping_ns = now_mono_ns();
    c->backoff_idx = 0;
    EMD_LOGI("mqtt", "connected");

    /* Subscribe to commands */
    char cmd_topic[EMD_MQTT_MAX_TOPIC_LEN];
    emd_mqtt_topic_cmd_sub(c->cfg.instance_id, cmd_topic, sizeof(cmd_topic));
    n = mqtt_build_subscribe(buf, sizeof(buf), cmd_topic, 1, next_pkt_id(c));
    if (n > 0) emd_tcp_send_all(c->fd, buf, (size_t)n, 1000);

    return 0;
}

static void do_disconnect(emd_mqtt_client_t *c) {
    if (c->fd >= 0) {
        uint8_t buf[4];
        mqtt_build_disconnect(buf, sizeof(buf));
        emd_tcp_send_all(c->fd, buf, 2, 1000);
        emd_net_close(c->fd);
        c->fd = -1;
    }
    c->state = EMD_MQTT_DISCONNECTED;
}

static int send_publish(emd_mqtt_client_t *c, const mqtt_msg_t *m) {
    uint8_t buf[EMD_MQTT_MAX_PAYLOAD_LEN + 512];
    uint16_t pid = (m->qos > 0) ? next_pkt_id(c) : 0;
    int n = mqtt_build_publish(buf, sizeof(buf), m->topic,
                                m->payload, m->payload_len,
                                m->qos, m->retain, pid);
    if (n < 0) return -1;
    return emd_tcp_send_all(c->fd, buf, (size_t)n, 5000);
}

/* ---------------------------------------------------------------------- */
/* Notifier thread                                                          */
/* ---------------------------------------------------------------------- */

static void *notifier_thread(void *arg) {
    emd_mqtt_client_t *c = (emd_mqtt_client_t *)arg;

    while (c->running) {
        /* Connection management */
        if (c->state != EMD_MQTT_CONNECTED) {
            uint64_t now = now_mono_ns();
            if (now < c->reconnect_at_ns) {
                struct timespec ts = {.tv_sec = 0, .tv_nsec = 100000000L};
                nanosleep(&ts, NULL);
                continue;
            }
            if (do_connect(c) < 0) {
                if (c->backoff_idx < N_BACKOFF - 1) c->backoff_idx++;
                c->reconnect_at_ns = now_mono_ns() +
                    (uint64_t)BACKOFF_SECS[c->backoff_idx] * 1000000000ULL;
                c->state = EMD_MQTT_DISCONNECTED;
            }
            continue;
        }

        /* Keepalive PINGREQ every 30s */
        uint64_t now = now_mono_ns();
        if (now - c->last_ping_ns > 30000000000ULL) {
            uint8_t ping[4];
            mqtt_build_pingreq(ping, sizeof(ping));
            if (emd_tcp_send_all(c->fd, ping, 2, 1000) < 0) {
                EMD_LOGW("mqtt", "ping failed, reconnecting");
                do_disconnect(c);
                c->reconnect_at_ns = now_mono_ns() + 1000000000ULL;
                continue;
            }
            c->last_ping_ns = now;
        }

        /* Drain inbound (PINGRESP, PUBACK, PUBLISH from server) */
        {
            uint8_t rbuf[256];
            int r = emd_tcp_recv(c->fd, rbuf, sizeof(rbuf));
            if (r < 0 && r != EMD_NET_AGAIN) {
                do_disconnect(c);
                c->reconnect_at_ns = now_mono_ns() + 1000000000ULL;
                continue;
            }
            /* Handle inbound PUBLISH (commands) */
            if (r > 0 && c->cmd_cb) {
                mqtt_pkt_t pkt;
                if (mqtt_parse(rbuf, (size_t)r, &pkt) >= 0 &&
                    pkt.type == MQTT_PUBLISH)
                {
                    char topic[EMD_MQTT_MAX_TOPIC_LEN];
                    uint16_t pid2; const uint8_t *pl; size_t pl_len;
                    if (mqtt_decode_publish(&pkt, pkt.flags, topic, sizeof(topic),
                                            &pid2, &pl, &pl_len) == 0) {
                        c->cmd_cb(topic, pl, pl_len, c->userdata);
                    }
                }
            }
        }

        /* Send queued messages */
        mqtt_msg_t msg;
        if (mq_pop_timeout(&c->queue, &msg, 20) == 0) {
            if (send_publish(c, &msg) < 0) {
                EMD_LOGW("mqtt", "publish failed, reconnecting");
                do_disconnect(c);
                c->reconnect_at_ns = now_mono_ns() + 1000000000ULL;
            }
        }
    }

    do_disconnect(c);
    return NULL;
}

/* ---------------------------------------------------------------------- */
/* Public API                                                               */
/* ---------------------------------------------------------------------- */

emd_mqtt_client_t *emd_mqtt_client_new(const emd_mqtt_cfg_t *cfg,
                                        emd_mqtt_cmd_cb cmd_cb,
                                        void *userdata)
{
    emd_mqtt_client_t *c = calloc(1, sizeof(*c));
    if (!c) return NULL;
    c->cfg      = *cfg;
    c->cmd_cb   = cmd_cb;
    c->userdata = userdata;
    c->fd       = -1;
    c->state    = EMD_MQTT_DISCONNECTED;
    c->next_pkt_id = 1;
    mq_init(&c->queue);
    return c;
}

void emd_mqtt_client_free(emd_mqtt_client_t *c) {
    if (!c) return;
    free(c);
}

int emd_mqtt_start(emd_mqtt_client_t *c) {
    if (!c) return -1;
    c->running = true;
    return pthread_create(&c->thread, NULL, notifier_thread, c);
}

void emd_mqtt_stop(emd_mqtt_client_t *c) {
    if (!c) return;
    c->running = false;
    pthread_join(c->thread, NULL);
}

emd_mqtt_state_t emd_mqtt_state(const emd_mqtt_client_t *c) {
    return c ? c->state : EMD_MQTT_DISCONNECTED;
}

int emd_mqtt_publish(emd_mqtt_client_t *c,
                     const char *topic,
                     const uint8_t *payload, size_t payload_len,
                     emd_mqtt_qos_t qos, bool retain)
{
    if (!c || !topic) return -1;

    mqtt_msg_t m;
    memset(&m, 0, sizeof(m));
    strncpy(m.topic, topic, sizeof(m.topic) - 1);

    size_t copy_len = payload_len < sizeof(m.payload) ? payload_len : sizeof(m.payload);
    if (payload && copy_len > 0) memcpy(m.payload, payload, copy_len);
    m.payload_len = copy_len;
    m.qos    = (uint8_t)qos;
    m.retain = retain;

    uint32_t qmax = c->cfg.queue_max > 0 ? c->cfg.queue_max : MQTT_QUEUE_SIZE;
    return mq_push(&c->queue, &m, qmax);
}

int emd_mqtt_publish_str(emd_mqtt_client_t *c, const char *topic,
                          const char *payload_str,
                          emd_mqtt_qos_t qos, bool retain)
{
    if (!payload_str) return -1;
    return emd_mqtt_publish(c, topic,
                             (const uint8_t *)payload_str, strlen(payload_str),
                             qos, retain);
}

/* ---------------------------------------------------------------------- */
/* Topic helpers                                                            */
/* ---------------------------------------------------------------------- */

void emd_mqtt_topic_status(const char *instance, char *buf, size_t bufsz) {
    snprintf(buf, bufsz, "emd/%s/status", instance);
}

void emd_mqtt_topic_event(const char *instance, const char *cam_id,
                           char *buf, size_t bufsz)
{
    snprintf(buf, bufsz, "emd/%s/cameras/%s/event", instance, cam_id);
}

void emd_mqtt_topic_clip(const char *instance, const char *cam_id,
                          char *buf, size_t bufsz)
{
    snprintf(buf, bufsz, "emd/%s/cameras/%s/clip", instance, cam_id);
}

void emd_mqtt_topic_stats(const char *instance, const char *cam_id,
                           char *buf, size_t bufsz)
{
    snprintf(buf, bufsz, "emd/%s/cameras/%s/stats", instance, cam_id);
}

void emd_mqtt_topic_cmd_sub(const char *instance, char *buf, size_t bufsz) {
    snprintf(buf, bufsz, "emd/%s/cmd/+", instance);
}

/* ---------------------------------------------------------------------- */
/* JSON payload builders                                                    */
/* ---------------------------------------------------------------------- */

int emd_mqtt_build_event_payload(const emd_event_t *ev,
                                  const char *instance,
                                  char *buf, size_t bufsz)
{
    if (!ev || !buf || bufsz == 0) return -1;
    int n = snprintf(buf, bufsz,
        "{"
        "\"v\":1,"
        "\"instance\":\"%s\","
        "\"cam_id\":\"%s\","
        "\"event_id\":\"%s\","
        "\"type\":\"%s\","
        "\"reason\":\"%s\","
        "\"started_pts_90khz\":%" PRIu64 ","
        "\"fps_estimate\":%.2f,"
        "\"codec\":\"%s\""
        "}",
        instance,
        ev->cam_name,
        ev->event_id,
        (ev->type == EMD_EVENT_SCENE_CHANGE) ? "scene_change" : "motion",
        ev->reason,
        ev->started_pts_90khz,
        ev->fps_estimate,
        emd_codec_name(ev->codec));
    return (n > 0 && (size_t)n < bufsz) ? n : -1;
}

int emd_mqtt_build_clip_payload(const emd_clip_header_t *hdr,
                                 const emd_event_t *ev,
                                 const char *instance,
                                 char *buf, size_t bufsz)
{
    if (!hdr || !buf || bufsz == 0) return -1;
    int n = snprintf(buf, bufsz,
        "{"
        "\"v\":1,"
        "\"instance\":\"%s\","
        "\"event_id\":\"%s\","
        "\"cam_id\":\"%s\","
        "\"container\":\"%s\","
        "\"codec\":\"%s\","
        "\"path\":\"%s\","
        "\"size_bytes\":%" PRIu64 ","
        "\"duration_ms\":%" PRIu64 ","
        "\"pre_roll_ms\":%" PRIu64 ","
        "\"post_roll_ms\":%" PRIu64 ","
        "\"sha256\":\"%s\""
        "}",
        instance,
        hdr->event_id,
        hdr->cam_id_str,
        hdr->container,
        hdr->codec,
        hdr->path,
        hdr->size_bytes,
        hdr->duration_ms,
        hdr->pre_roll_ms,
        hdr->post_roll_ms,
        hdr->sha256);
    if (ev) { /* suppress unused */ (void)ev; }
    return (n > 0 && (size_t)n < bufsz) ? n : -1;
}

int emd_mqtt_build_status_payload(bool online, const char *reason,
                                   char *buf, size_t bufsz)
{
    if (!buf || bufsz == 0) return -1;
    int n = snprintf(buf, bufsz,
        "{\"online\":%s,\"reason\":\"%s\"}",
        online ? "true" : "false",
        reason ? reason : "");
    return (n > 0 && (size_t)n < bufsz) ? n : -1;
}

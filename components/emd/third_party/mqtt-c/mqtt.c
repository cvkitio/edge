/*
 * MIT License
 * Minimal MQTT 3.1.1 wire protocol implementation.
 */

#include "mqtt.h"

#include <string.h>
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* --------------------------------------------------------------------- */
/* Helpers                                                                 */
/* --------------------------------------------------------------------- */

/* Write a 2-byte big-endian length + string */
static int write_utf8(uint8_t *buf, size_t bufsz, size_t *pos,
                      const char *s, size_t slen) {
    if (*pos + 2 + slen > bufsz) return MQTT_ERR_BUF_TOO_SMALL;
    buf[(*pos)++] = (uint8_t)(slen >> 8);
    buf[(*pos)++] = (uint8_t)(slen & 0xFF);
    memcpy(buf + *pos, s, slen);
    *pos += slen;
    return MQTT_OK;
}

int mqtt_encode_varlen(uint8_t *buf, size_t bufsz, uint32_t value) {
    int count = 0;
    do {
        if ((size_t)count >= bufsz) return MQTT_ERR_BUF_TOO_SMALL;
        uint8_t byte = (uint8_t)(value & 0x7Fu);
        value >>= 7;
        if (value > 0) byte |= 0x80u;
        buf[count++] = byte;
    } while (value > 0);
    return count;
}

int mqtt_decode_varlen(const uint8_t *buf, size_t len, uint32_t *value_out) {
    uint32_t value = 0;
    int shift = 0;
    int i = 0;
    do {
        if ((size_t)i >= len) return MQTT_ERR_INCOMPLETE;
        if (i >= 4) return MQTT_ERR_MALFORMED_PACKET;
        uint8_t b = buf[i++];
        value |= (uint32_t)(b & 0x7Fu) << shift;
        shift += 7;
        if (!(b & 0x80u)) break;
    } while (1);
    *value_out = value;
    return i;
}

/* --------------------------------------------------------------------- */
/* CONNECT                                                                 */
/* --------------------------------------------------------------------- */

int mqtt_build_connect(uint8_t *buf, size_t bufsz,
                       const mqtt_connect_opts_t *opts)
{
    if (!buf || !opts) return MQTT_ERR_INVALID_ARG;

    /* Build payload in a scratch buffer */
    uint8_t pay[1024];
    size_t pp = 0;

    /* Client ID (required) */
    size_t cid_len = opts->client_id ? strlen(opts->client_id) : 0;
    if (write_utf8(pay, sizeof(pay), &pp, opts->client_id ? opts->client_id : "", cid_len) < 0)
        return MQTT_ERR_BUF_TOO_SMALL;

    /* Connect flags */
    uint8_t conn_flags = 0;
    if (opts->clean_session) conn_flags |= 0x02u;

    uint8_t lwt_qos = 0;
    bool    lwt_retain = false;

    if (opts->lwt_topic && opts->lwt_topic[0]) {
        conn_flags |= 0x04u; /* Will flag */
        lwt_qos = opts->lwt_qos & 0x03u;
        conn_flags |= (uint8_t)(lwt_qos << 3);
        lwt_retain = opts->lwt_retain;
        if (lwt_retain) conn_flags |= 0x20u;

        size_t lt_len = strlen(opts->lwt_topic);
        if (write_utf8(pay, sizeof(pay), &pp, opts->lwt_topic, lt_len) < 0)
            return MQTT_ERR_BUF_TOO_SMALL;
        /* Will payload: UTF-8 string (may be binary) */
        size_t lp_len = opts->lwt_payload ? opts->lwt_payload_len : 0;
        if (pp + 2 + lp_len > sizeof(pay)) return MQTT_ERR_BUF_TOO_SMALL;
        pay[pp++] = (uint8_t)(lp_len >> 8);
        pay[pp++] = (uint8_t)(lp_len & 0xFF);
        if (opts->lwt_payload && lp_len > 0)
            memcpy(pay + pp, opts->lwt_payload, lp_len);
        pp += lp_len;
    }

    if (opts->username) {
        conn_flags |= 0x80u;
        size_t ul = strlen(opts->username);
        if (write_utf8(pay, sizeof(pay), &pp, opts->username, ul) < 0)
            return MQTT_ERR_BUF_TOO_SMALL;
    }
    if (opts->password) {
        conn_flags |= 0x40u;
        size_t pl = strlen(opts->password);
        if (write_utf8(pay, sizeof(pay), &pp, opts->password, pl) < 0)
            return MQTT_ERR_BUF_TOO_SMALL;
    }

    /* Variable header: Protocol Name, Level, Flags, Keepalive */
    uint8_t vhdr[10];
    /* Protocol Name "MQTT" */
    vhdr[0] = 0; vhdr[1] = 4;
    vhdr[2] = 'M'; vhdr[3] = 'Q'; vhdr[4] = 'T'; vhdr[5] = 'T';
    /* Protocol Level 4 = MQTT 3.1.1 */
    vhdr[6] = 4;
    vhdr[7] = conn_flags;
    vhdr[8] = (uint8_t)(opts->keepalive_secs >> 8);
    vhdr[9] = (uint8_t)(opts->keepalive_secs & 0xFF);

    uint32_t remaining = (uint32_t)(10 + pp);

    /* Fixed header */
    uint8_t varlen[4];
    int vl = mqtt_encode_varlen(varlen, sizeof(varlen), remaining);
    if (vl < 0) return MQTT_ERR_BUF_TOO_SMALL;

    size_t total = 1u + (size_t)vl + 10u + pp;
    if (total > bufsz) return MQTT_ERR_BUF_TOO_SMALL;

    size_t pos = 0;
    buf[pos++] = (uint8_t)((MQTT_CONNECT << 4) & 0xF0u);
    memcpy(buf + pos, varlen, (size_t)vl); pos += (size_t)vl;
    memcpy(buf + pos, vhdr, 10); pos += 10;
    memcpy(buf + pos, pay, pp); pos += pp;

    (void)lwt_qos;
    (void)lwt_retain;
    return (int)total;
}

/* --------------------------------------------------------------------- */
/* PUBLISH                                                                 */
/* --------------------------------------------------------------------- */

int mqtt_build_publish(uint8_t *buf, size_t bufsz,
                       const char *topic,
                       const uint8_t *payload, size_t payload_len,
                       uint8_t qos, bool retain, uint16_t pkt_id)
{
    if (!buf || !topic) return MQTT_ERR_INVALID_ARG;
    qos &= 0x03u;

    size_t topic_len = strlen(topic);
    size_t vhdr_len  = 2 + topic_len + (qos > 0 ? 2u : 0u);
    uint32_t remaining = (uint32_t)(vhdr_len + payload_len);

    uint8_t varlen[4];
    int vl = mqtt_encode_varlen(varlen, sizeof(varlen), remaining);
    if (vl < 0) return MQTT_ERR_BUF_TOO_SMALL;

    size_t total = 1u + (size_t)vl + vhdr_len + payload_len;
    if (total > bufsz) return MQTT_ERR_BUF_TOO_SMALL;

    uint8_t flags = (uint8_t)((qos << 1) | (retain ? 1u : 0u));
    buf[0] = (uint8_t)((MQTT_PUBLISH << 4) | flags);
    size_t pos = 1;
    memcpy(buf + pos, varlen, (size_t)vl); pos += (size_t)vl;

    /* Topic */
    buf[pos++] = (uint8_t)(topic_len >> 8);
    buf[pos++] = (uint8_t)(topic_len & 0xFF);
    memcpy(buf + pos, topic, topic_len); pos += topic_len;

    /* Packet ID for QoS 1/2 */
    if (qos > 0) {
        buf[pos++] = (uint8_t)(pkt_id >> 8);
        buf[pos++] = (uint8_t)(pkt_id & 0xFF);
    }

    /* Payload */
    if (payload && payload_len > 0)
        memcpy(buf + pos, payload, payload_len);

    return (int)total;
}

/* --------------------------------------------------------------------- */
/* SUBSCRIBE                                                               */
/* --------------------------------------------------------------------- */

int mqtt_build_subscribe(uint8_t *buf, size_t bufsz,
                          const char *topic, uint8_t qos, uint16_t pkt_id)
{
    if (!buf || !topic || pkt_id == 0) return MQTT_ERR_INVALID_ARG;
    size_t topic_len = strlen(topic);
    /* variable header: pkt_id (2) + topic length (2) + topic + requested QoS (1) */
    uint32_t remaining = (uint32_t)(2 + 2 + topic_len + 1);

    uint8_t varlen[4];
    int vl = mqtt_encode_varlen(varlen, sizeof(varlen), remaining);
    if (vl < 0) return MQTT_ERR_BUF_TOO_SMALL;

    size_t total = 1u + (size_t)vl + remaining;
    if (total > bufsz) return MQTT_ERR_BUF_TOO_SMALL;

    buf[0] = (uint8_t)((MQTT_SUBSCRIBE << 4) | 0x02u); /* reserved flags */
    size_t pos = 1;
    memcpy(buf + pos, varlen, (size_t)vl); pos += (size_t)vl;
    buf[pos++] = (uint8_t)(pkt_id >> 8);
    buf[pos++] = (uint8_t)(pkt_id & 0xFF);
    buf[pos++] = (uint8_t)(topic_len >> 8);
    buf[pos++] = (uint8_t)(topic_len & 0xFF);
    memcpy(buf + pos, topic, topic_len); pos += topic_len;
    buf[pos] = (uint8_t)(qos & 0x03u);

    return (int)total;
}

/* --------------------------------------------------------------------- */
/* PINGREQ / DISCONNECT / PUBACK                                           */
/* --------------------------------------------------------------------- */

int mqtt_build_pingreq(uint8_t *buf, size_t bufsz) {
    if (bufsz < 2) return MQTT_ERR_BUF_TOO_SMALL;
    buf[0] = (uint8_t)(MQTT_PINGREQ << 4);
    buf[1] = 0;
    return 2;
}

int mqtt_build_disconnect(uint8_t *buf, size_t bufsz) {
    if (bufsz < 2) return MQTT_ERR_BUF_TOO_SMALL;
    buf[0] = (uint8_t)(MQTT_DISCONNECT << 4);
    buf[1] = 0;
    return 2;
}

int mqtt_build_puback(uint8_t *buf, size_t bufsz, uint16_t pkt_id) {
    if (bufsz < 4) return MQTT_ERR_BUF_TOO_SMALL;
    buf[0] = (uint8_t)(MQTT_PUBACK << 4);
    buf[1] = 2;
    buf[2] = (uint8_t)(pkt_id >> 8);
    buf[3] = (uint8_t)(pkt_id & 0xFF);
    return 4;
}

/* --------------------------------------------------------------------- */
/* Parser                                                                  */
/* --------------------------------------------------------------------- */

int mqtt_parse(const uint8_t *buf, size_t len, mqtt_pkt_t *pkt) {
    if (!buf || !pkt || len < 2) return MQTT_ERR_INCOMPLETE;

    pkt->type  = (buf[0] >> 4) & 0x0Fu;
    pkt->flags = buf[0] & 0x0Fu;

    uint32_t remaining = 0;
    int vl = mqtt_decode_varlen(buf + 1, len - 1, &remaining);
    if (vl < 0) return vl; /* INCOMPLETE or MALFORMED */

    size_t total = 1u + (size_t)vl + remaining;
    if (len < total) return MQTT_ERR_INCOMPLETE;

    const uint8_t *vhdr = buf + 1 + (size_t)vl;
    pkt->remaining_len     = remaining;
    pkt->variable_hdr      = vhdr;
    pkt->variable_hdr_len  = remaining; /* will be split below if needed */
    pkt->payload           = NULL;
    pkt->payload_len       = 0;

    return (int)total;
}

int mqtt_decode_connack(const mqtt_pkt_t *pkt,
                        bool *session_present, uint8_t *return_code)
{
    if (!pkt || pkt->type != MQTT_CONNACK) return MQTT_ERR_INVALID_ARG;
    if (pkt->remaining_len < 2) return MQTT_ERR_MALFORMED_PACKET;
    *session_present = !!(pkt->variable_hdr[0] & 0x01u);
    *return_code = pkt->variable_hdr[1];
    return MQTT_OK;
}

int mqtt_decode_publish(const mqtt_pkt_t *pkt,
                        uint8_t flags,
                        char *topic_buf, size_t topic_buf_sz,
                        uint16_t *pkt_id_out,
                        const uint8_t **payload_out,
                        size_t *payload_len_out)
{
    if (!pkt || pkt->type != MQTT_PUBLISH) return MQTT_ERR_INVALID_ARG;

    const uint8_t *p = pkt->variable_hdr;
    size_t rem = pkt->remaining_len;

    if (rem < 2) return MQTT_ERR_MALFORMED_PACKET;
    uint16_t topic_len = (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
    p += 2; rem -= 2;

    if (rem < topic_len) return MQTT_ERR_MALFORMED_PACKET;
    size_t copy_len = topic_len < topic_buf_sz - 1 ? topic_len : topic_buf_sz - 1;
    memcpy(topic_buf, p, copy_len);
    topic_buf[copy_len] = '\0';
    p += topic_len; rem -= topic_len;

    uint8_t qos = (uint8_t)((flags >> 1) & 0x03u);
    *pkt_id_out = 0;
    if (qos > 0) {
        if (rem < 2) return MQTT_ERR_MALFORMED_PACKET;
        *pkt_id_out = (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
        p += 2; rem -= 2;
    }

    *payload_out    = p;
    *payload_len_out = rem;
    return MQTT_OK;
}

/*
 * MIT License
 * Minimal MQTT 3.1.1 client — wire protocol implementation.
 * Supports: CONNECT, PUBLISH, SUBSCRIBE, PINGREQ/PINGRESP, DISCONNECT.
 * Designed for use with emd_mqtt.c which owns the socket.
 */
#ifndef MQTT_C_H
#define MQTT_C_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* MQTT control packet types */
#define MQTT_CONNECT     1
#define MQTT_CONNACK     2
#define MQTT_PUBLISH     3
#define MQTT_PUBACK      4
#define MQTT_PUBREC      5
#define MQTT_PUBREL      6
#define MQTT_PUBCOMP     7
#define MQTT_SUBSCRIBE   8
#define MQTT_SUBACK      9
#define MQTT_UNSUBSCRIBE 10
#define MQTT_UNSUBACK    11
#define MQTT_PINGREQ     12
#define MQTT_PINGRESP    13
#define MQTT_DISCONNECT  14

/* Return codes */
#define MQTT_OK                       0
#define MQTT_ERR_BUF_TOO_SMALL       -1
#define MQTT_ERR_INVALID_ARG         -2
#define MQTT_ERR_MALFORMED_PACKET    -3
#define MQTT_ERR_INCOMPLETE          -4  /* need more data */

/* CONNACK return codes */
#define MQTT_CONNACK_ACCEPTED         0
#define MQTT_CONNACK_REFUSED_PROTO    1
#define MQTT_CONNACK_REFUSED_ID       2
#define MQTT_CONNACK_REFUSED_SERVER   3
#define MQTT_CONNACK_REFUSED_AUTH     4
#define MQTT_CONNACK_REFUSED_UNAUTH   5

/* QoS levels */
#define MQTT_QOS0  0
#define MQTT_QOS1  1
#define MQTT_QOS2  2

/* --------------------------------------------------------------------- */
/* Packet builders — write into caller-supplied buf; return bytes written */
/* or negative error code.                                                */
/* --------------------------------------------------------------------- */

typedef struct {
    const char *client_id;
    const char *username;          /* NULL = no username */
    const char *password;          /* NULL = no password */
    const char *lwt_topic;         /* NULL = no LWT */
    const char *lwt_payload;
    size_t      lwt_payload_len;
    uint8_t     lwt_qos;
    bool        lwt_retain;
    uint16_t    keepalive_secs;
    bool        clean_session;
} mqtt_connect_opts_t;

/* Build a CONNECT packet. Returns bytes written or <0. */
int mqtt_build_connect(uint8_t *buf, size_t bufsz,
                       const mqtt_connect_opts_t *opts);

/* Build a PUBLISH packet.
 *  topic, payload, payload_len: message content.
 *  qos: 0, 1, or 2.
 *  retain: retain flag.
 *  pkt_id: packet identifier (required for QoS 1/2; 0 for QoS 0).
 */
int mqtt_build_publish(uint8_t *buf, size_t bufsz,
                       const char *topic,
                       const uint8_t *payload, size_t payload_len,
                       uint8_t qos, bool retain, uint16_t pkt_id);

/* Build a SUBSCRIBE packet.
 *  topic: topic filter.
 *  qos: requested QoS.
 *  pkt_id: packet identifier (must be != 0).
 */
int mqtt_build_subscribe(uint8_t *buf, size_t bufsz,
                          const char *topic, uint8_t qos, uint16_t pkt_id);

/* Build a PINGREQ packet (2 bytes). */
int mqtt_build_pingreq(uint8_t *buf, size_t bufsz);

/* Build a DISCONNECT packet (2 bytes). */
int mqtt_build_disconnect(uint8_t *buf, size_t bufsz);

/* Build a PUBACK packet (4 bytes, for QoS 1 acknowledgement). */
int mqtt_build_puback(uint8_t *buf, size_t bufsz, uint16_t pkt_id);

/* --------------------------------------------------------------------- */
/* Packet parser                                                           */
/* --------------------------------------------------------------------- */

typedef struct {
    uint8_t   type;           /* control packet type (MQTT_*) */
    uint8_t   flags;          /* lower 4 bits of fixed header */
    size_t    remaining_len;  /* remaining length (variable-length encoded) */
    /* Pointers into the supplied buffer */
    const uint8_t *variable_hdr;
    size_t         variable_hdr_len;
    const uint8_t *payload;
    size_t         payload_len;
} mqtt_pkt_t;

/* Parse a raw packet from buf (len bytes).
 * On success returns total packet length (fixed + remaining).
 * Returns MQTT_ERR_INCOMPLETE if more bytes are needed.
 * Returns MQTT_ERR_MALFORMED_PACKET on error.
 */
int mqtt_parse(const uint8_t *buf, size_t len, mqtt_pkt_t *pkt);

/* Decode CONNACK: fills *session_present and *return_code.
 * Returns 0 on success. */
int mqtt_decode_connack(const mqtt_pkt_t *pkt,
                        bool *session_present, uint8_t *return_code);

/* Decode a PUBLISH packet fields.
 * Fills topic_buf (NUL-terminated), *pkt_id (0 for QoS 0),
 * *payload and *payload_len.
 */
int mqtt_decode_publish(const mqtt_pkt_t *pkt,
                        uint8_t flags,
                        char *topic_buf, size_t topic_buf_sz,
                        uint16_t *pkt_id_out,
                        const uint8_t **payload_out,
                        size_t *payload_len_out);

/* Encode the MQTT variable-length integer.
 * Returns bytes written (1-4). */
int mqtt_encode_varlen(uint8_t *buf, size_t bufsz, uint32_t value);

/* Decode the MQTT variable-length integer from buf.
 * Returns bytes consumed, or -1 on error. */
int mqtt_decode_varlen(const uint8_t *buf, size_t len, uint32_t *value_out);

#ifdef __cplusplus
}
#endif

#endif /* MQTT_C_H */

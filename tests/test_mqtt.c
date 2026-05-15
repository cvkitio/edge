/*
 * test_mqtt.c — MQTT client unit tests.
 *
 * Tests:
 *  - MQTT wire protocol: build_connect valid packet.
 *  - MQTT wire protocol: build_publish valid packet.
 *  - MQTT wire protocol: build_subscribe valid packet.
 *  - MQTT wire protocol: build_pingreq.
 *  - MQTT wire protocol: parse CONNACK.
 *  - Topic helper functions.
 *  - JSON payload builders.
 */

#include <cmocka.h>
#include "mqtt.h"
#include "emd/mqtt.h"
#include "emd/event.h"
#include "emd/recorder.h"

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>

/* --------------------------------------------------------------------- */
/* MQTT wire protocol tests                                                */
/* --------------------------------------------------------------------- */

static void test_build_connect(void **state) {
    (void)state;
    uint8_t buf[256];

    mqtt_connect_opts_t opts = {
        .client_id       = "test-client",
        .keepalive_secs  = 60,
        .clean_session   = true,
    };

    int n = mqtt_build_connect(buf, sizeof(buf), &opts);
    assert_true(n > 0);

    /* Fixed header: type=1 (CONNECT), flags=0 */
    assert_int_equal((buf[0] >> 4), MQTT_CONNECT);
    assert_int_equal((buf[0] & 0x0F), 0);

    /* Variable header starts at buf+2 (after 1-byte varlen for small packets).
     * Protocol name should be "MQTT" at bytes 2..7 */
    assert_int_equal(buf[4], 'M');
    assert_int_equal(buf[5], 'Q');
    assert_int_equal(buf[6], 'T');
    assert_int_equal(buf[7], 'T');
    /* Protocol level = 4 */
    assert_int_equal(buf[8], 4);
}

static void test_build_connect_with_lwt(void **state) {
    (void)state;
    uint8_t buf[512];
    mqtt_connect_opts_t opts = {
        .client_id       = "emd-01",
        .lwt_topic       = "emd/emd-01/status",
        .lwt_payload     = "{\"online\":false,\"reason\":\"lwt\"}",
        .lwt_payload_len = 31,
        .lwt_qos         = 1,
        .lwt_retain      = true,
        .keepalive_secs  = 30,
        .clean_session   = false,
    };
    int n = mqtt_build_connect(buf, sizeof(buf), &opts);
    assert_true(n > 0);
    /* Flags byte (buf[9]) should have will flag (0x04), will retain (0x20), will QoS (0x08) */
    uint8_t flags = buf[9];
    assert_true((flags & 0x04u) != 0); /* will flag */
}

static void test_build_publish_qos0(void **state) {
    (void)state;
    uint8_t buf[256];
    const uint8_t payload[] = {'{', '}', '\0'};

    int n = mqtt_build_publish(buf, sizeof(buf),
                                "emd/emd-01/cameras/cam1/event",
                                payload, 2, MQTT_QOS0, false, 0);
    assert_true(n > 0);
    /* Fixed header byte 0: type=3 (PUBLISH), flags: QoS=0, retain=0 → 0x30 */
    assert_int_equal((buf[0] >> 4), MQTT_PUBLISH);
    assert_int_equal((buf[0] & 0x06u), 0); /* QoS=0 */
}

static void test_build_publish_qos1(void **state) {
    (void)state;
    uint8_t buf[256];
    const char *topic = "emd/test/event";
    const uint8_t payload[] = {0x01, 0x02};

    int n = mqtt_build_publish(buf, sizeof(buf), topic, payload, 2,
                                MQTT_QOS1, false, 42);
    assert_true(n > 0);
    assert_int_equal((buf[0] >> 4), MQTT_PUBLISH);
    /* QoS bits: (buf[0] >> 1) & 0x03 */
    assert_int_equal(((buf[0] >> 1) & 0x03u), 1);
}

static void test_build_subscribe(void **state) {
    (void)state;
    uint8_t buf[256];
    int n = mqtt_build_subscribe(buf, sizeof(buf),
                                  "emd/emd-01/cmd/+", MQTT_QOS1, 1);
    assert_true(n > 0);
    assert_int_equal((buf[0] >> 4), MQTT_SUBSCRIBE);
    /* SUBSCRIBE flags must be 0x02 */
    assert_int_equal((buf[0] & 0x0Fu), 0x02u);
}

static void test_build_pingreq(void **state) {
    (void)state;
    uint8_t buf[8];
    int n = mqtt_build_pingreq(buf, sizeof(buf));
    assert_int_equal(n, 2);
    assert_int_equal((buf[0] >> 4), MQTT_PINGREQ);
    assert_int_equal(buf[1], 0); /* remaining length = 0 */
}

static void test_parse_connack_ok(void **state) {
    (void)state;
    /* Synthetic CONNACK: type=2, flags=0, remaining=2, session=0, rc=0 */
    uint8_t raw[] = {0x20u, 0x02u, 0x00u, 0x00u};
    mqtt_pkt_t pkt;
    int n = mqtt_parse(raw, sizeof(raw), &pkt);
    assert_true(n == 4);
    assert_int_equal((int)pkt.type, MQTT_CONNACK);

    bool sp; uint8_t rc;
    int r = mqtt_decode_connack(&pkt, &sp, &rc);
    assert_int_equal(r, 0);
    assert_false(sp);
    assert_int_equal((int)rc, MQTT_CONNACK_ACCEPTED);
}

static void test_parse_connack_refused(void **state) {
    (void)state;
    /* CONNACK: refused, bad credentials (rc=4) */
    uint8_t raw[] = {0x20u, 0x02u, 0x00u, 0x04u};
    mqtt_pkt_t pkt;
    mqtt_parse(raw, sizeof(raw), &pkt);
    bool sp; uint8_t rc;
    mqtt_decode_connack(&pkt, &sp, &rc);
    assert_int_equal((int)rc, MQTT_CONNACK_REFUSED_AUTH);
}

static void test_varlen_encoding(void **state) {
    (void)state;
    uint8_t buf[4];
    uint32_t val;

    /* Single byte: value 0 */
    int n = mqtt_encode_varlen(buf, sizeof(buf), 0);
    assert_int_equal(n, 1);
    assert_int_equal(buf[0], 0);
    int d = mqtt_decode_varlen(buf, 1, &val);
    assert_int_equal(d, 1);
    assert_int_equal((int)val, 0);

    /* Single byte: value 127 */
    n = mqtt_encode_varlen(buf, sizeof(buf), 127);
    assert_int_equal(n, 1);
    d = mqtt_decode_varlen(buf, 1, &val);
    assert_int_equal(d, 1);
    assert_int_equal((int)val, 127);

    /* Two bytes: value 128 */
    n = mqtt_encode_varlen(buf, sizeof(buf), 128);
    assert_int_equal(n, 2);
    assert_int_equal(buf[0], 0x80u);
    assert_int_equal(buf[1], 0x01u);
    d = mqtt_decode_varlen(buf, 2, &val);
    assert_int_equal(d, 2);
    assert_int_equal((int)val, 128);

    /* Four bytes: max value 268435455 */
    n = mqtt_encode_varlen(buf, sizeof(buf), 268435455u);
    assert_int_equal(n, 4);
    d = mqtt_decode_varlen(buf, 4, &val);
    assert_int_equal(d, 4);
    assert_int_equal((int)val, 268435455);
}

/* --------------------------------------------------------------------- */
/* Topic helpers                                                            */
/* --------------------------------------------------------------------- */

static void test_topic_helpers(void **state) {
    (void)state;
    char buf[256];

    emd_mqtt_topic_status("emd-01", buf, sizeof(buf));
    assert_string_equal(buf, "emd/emd-01/status");

    emd_mqtt_topic_event("emd-01", "driveway", buf, sizeof(buf));
    assert_string_equal(buf, "emd/emd-01/cameras/driveway/event");

    emd_mqtt_topic_clip("emd-01", "driveway", buf, sizeof(buf));
    assert_string_equal(buf, "emd/emd-01/cameras/driveway/clip");

    emd_mqtt_topic_cmd_sub("emd-01", buf, sizeof(buf));
    assert_string_equal(buf, "emd/emd-01/cmd/+");
}

/* --------------------------------------------------------------------- */
/* JSON payload builders                                                    */
/* --------------------------------------------------------------------- */

static void test_build_event_payload(void **state) {
    (void)state;
    emd_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.cam_id            = 1;
    ev.type              = EMD_EVENT_MOTION;
    ev.started_pts_90khz = 4123456789ULL;
    ev.fps_estimate      = 29.97;
    ev.codec             = 1; /* h264 */
    strncpy(ev.reason,   "z=4.7,intra_ratio=3.1", sizeof(ev.reason) - 1);
    strncpy(ev.cam_name, "driveway", sizeof(ev.cam_name) - 1);
    emd_event_id_generate(ev.event_id, sizeof(ev.event_id));

    char payload[4096];
    int n = emd_mqtt_build_event_payload(&ev, "emd-edge-01", payload, sizeof(payload));
    assert_true(n > 0);
    assert_true(strstr(payload, "\"type\":\"motion\"") != NULL);
    assert_true(strstr(payload, "\"codec\":\"h264\"") != NULL);
    assert_true(strstr(payload, "z=4.7") != NULL);
}

static void test_build_status_payload(void **state) {
    (void)state;
    char payload[256];
    int n = emd_mqtt_build_status_payload(true, "startup", payload, sizeof(payload));
    assert_true(n > 0);
    assert_true(strstr(payload, "\"online\":true") != NULL);
    assert_true(strstr(payload, "startup") != NULL);

    n = emd_mqtt_build_status_payload(false, "lwt", payload, sizeof(payload));
    assert_true(n > 0);
    assert_true(strstr(payload, "\"online\":false") != NULL);
}

/* --------------------------------------------------------------------- */
/* Test runner                                                              */
/* --------------------------------------------------------------------- */

int main(void) {
    const CMUnitTest tests[] = {
        cmocka_unit_test(test_build_connect),
        cmocka_unit_test(test_build_connect_with_lwt),
        cmocka_unit_test(test_build_publish_qos0),
        cmocka_unit_test(test_build_publish_qos1),
        cmocka_unit_test(test_build_subscribe),
        cmocka_unit_test(test_build_pingreq),
        cmocka_unit_test(test_parse_connack_ok),
        cmocka_unit_test(test_parse_connack_refused),
        cmocka_unit_test(test_varlen_encoding),
        cmocka_unit_test(test_topic_helpers),
        cmocka_unit_test(test_build_event_payload),
        cmocka_unit_test(test_build_status_payload),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}

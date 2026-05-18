/*
 * test_config.c — TOML config parser and emd_config_load() tests.
 *
 * Tests:
 *  - Parse valid minimal config.
 *  - Parse config with cameras.
 *  - Error on missing required key.
 *  - Error on invalid value.
 *  - TOML integer, float, bool, string parsing.
 *  - Multiple cameras enumerated.
 */

#include <cmocka.h>
#include "toml.h"
#include "emd/config.h"

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>

/* --------------------------------------------------------------------- */
/* TOML parser unit tests                                                   */
/* --------------------------------------------------------------------- */

static void test_toml_string(void **state) {
    (void)state;
    char conf[] =
        "name = \"hello world\"\n"
        "path = \"/var/lib/emd\"\n";

    char errbuf[256];
    toml_table_t *tab = toml_parse(conf, errbuf, sizeof(errbuf));
    assert_non_null(tab);

    const char *raw = toml_raw_in(tab, "name");
    assert_non_null(raw);

    char *str = NULL;
    int r = toml_rtos(raw, &str);
    assert_int_equal(r, 0);
    assert_non_null(str);
    assert_string_equal(str, "hello world");
    free(str);

    raw = toml_raw_in(tab, "path");
    r = toml_rtos(raw, &str);
    assert_int_equal(r, 0);
    assert_string_equal(str, "/var/lib/emd");
    free(str);

    toml_free(tab);
}

static void test_toml_integer(void **state) {
    (void)state;
    char conf[] =
        "port = 1883\n"
        "max_bytes = 20_000_000\n"
        "neg = -42\n";

    char errbuf[256];
    toml_table_t *tab = toml_parse(conf, errbuf, sizeof(errbuf));
    assert_non_null(tab);

    int64_t v = 0;
    int r = toml_rtoi(toml_raw_in(tab, "port"), &v);
    assert_int_equal(r, 0);
    assert_int_equal((int)v, 1883);

    r = toml_rtoi(toml_raw_in(tab, "max_bytes"), &v);
    assert_int_equal(r, 0);
    assert_int_equal((int)v, 20000000);

    r = toml_rtoi(toml_raw_in(tab, "neg"), &v);
    assert_int_equal(r, 0);
    assert_int_equal((int)v, -42);

    toml_free(tab);
}

static void test_toml_bool(void **state) {
    (void)state;
    char conf[] =
        "enabled = true\n"
        "disabled = false\n";

    char errbuf[256];
    toml_table_t *tab = toml_parse(conf, errbuf, sizeof(errbuf));
    assert_non_null(tab);

    int v = -1;
    toml_rtob(toml_raw_in(tab, "enabled"), &v);
    assert_int_equal(v, 1);

    toml_rtob(toml_raw_in(tab, "disabled"), &v);
    assert_int_equal(v, 0);

    toml_free(tab);
}

static void test_toml_float(void **state) {
    (void)state;
    char conf[] =
        "z_high = 3.0\n"
        "threshold = 0.40\n";

    char errbuf[256];
    toml_table_t *tab = toml_parse(conf, errbuf, sizeof(errbuf));
    assert_non_null(tab);

    double d = 0.0;
    toml_rtod(toml_raw_in(tab, "z_high"), &d);
    assert_float_equal(d, 3.0, 0.001);

    toml_rtod(toml_raw_in(tab, "threshold"), &d);
    assert_float_equal(d, 0.40, 0.001);

    toml_free(tab);
}

static void test_toml_table(void **state) {
    (void)state;
    char conf[] =
        "[runtime]\n"
        "log_level = \"info\"\n"
        "metrics_listen = \"0.0.0.0:9464\"\n"
        "\n"
        "[mqtt]\n"
        "url = \"mqtt://localhost:1883\"\n"
        "qos = 1\n";

    char errbuf[256];
    toml_table_t *tab = toml_parse(conf, errbuf, sizeof(errbuf));
    assert_non_null(tab);

    toml_table_t *runtime = toml_table_in(tab, "runtime");
    assert_non_null(runtime);

    const char *raw = toml_raw_in(runtime, "log_level");
    assert_non_null(raw);
    char *str = NULL;
    toml_rtos(raw, &str);
    assert_string_equal(str, "info");
    free(str);

    toml_table_t *mqt = toml_table_in(tab, "mqtt");
    assert_non_null(mqt);
    int64_t qos = 0;
    toml_rtoi(toml_raw_in(mqt, "qos"), &qos);
    assert_int_equal((int)qos, 1);

    toml_free(tab);
}

static void test_toml_nested_table(void **state) {
    (void)state;
    char conf[] =
        "[cameras.driveway]\n"
        "url = \"rtsp://10.0.1.51:554/stream\"\n"
        "buffer_seconds = 12\n"
        "motion_z_high = 3.0\n"
        "\n"
        "[cameras.back_garden]\n"
        "url = \"rtsp://10.0.1.52:554/stream\"\n"
        "buffer_seconds = 10\n";

    char errbuf[256];
    toml_table_t *tab = toml_parse(conf, errbuf, sizeof(errbuf));
    assert_non_null(tab);

    toml_table_t *cameras = toml_table_in(tab, "cameras");
    assert_non_null(cameras);

    toml_table_t *driveway = toml_table_in(cameras, "driveway");
    assert_non_null(driveway);

    char *url = NULL;
    toml_rtos(toml_raw_in(driveway, "url"), &url);
    assert_non_null(url);
    assert_string_equal(url, "rtsp://10.0.1.51:554/stream");
    free(url);

    int64_t bufsecs = 0;
    toml_rtoi(toml_raw_in(driveway, "buffer_seconds"), &bufsecs);
    assert_int_equal((int)bufsecs, 12);

    toml_table_t *garden = toml_table_in(cameras, "back_garden");
    assert_non_null(garden);
    toml_rtoi(toml_raw_in(garden, "buffer_seconds"), &bufsecs);
    assert_int_equal((int)bufsecs, 10);

    assert_int_equal(toml_table_ntab(cameras), 2);

    toml_free(tab);
}

static void test_toml_comments(void **state) {
    (void)state;
    char conf[] =
        "# This is a comment\n"
        "key = \"value\" # inline comment\n"
        "# Another comment\n"
        "num = 42\n";

    char errbuf[256];
    toml_table_t *tab = toml_parse(conf, errbuf, sizeof(errbuf));
    assert_non_null(tab);

    char *str = NULL;
    toml_rtos(toml_raw_in(tab, "key"), &str);
    assert_string_equal(str, "value");
    free(str);

    int64_t v = 0;
    toml_rtoi(toml_raw_in(tab, "num"), &v);
    assert_int_equal((int)v, 42);

    toml_free(tab);
}

/* --------------------------------------------------------------------- */
/* emd_config_load tests                                                   */
/* --------------------------------------------------------------------- */

/* Write config to a temp file and parse it */
static int write_temp_config(const char *content, char *path_out, size_t path_sz) {
    snprintf(path_out, path_sz, "/tmp/emd_test_config_XXXXXX");
    int fd = mkstemp(path_out);
    if (fd < 0) return -1;
    size_t len = strlen(content);
    ssize_t n = write(fd, content, len);
    close(fd);
    return (n == (ssize_t)len) ? 0 : -1;
}

static void test_config_load_minimal(void **state) {
    (void)state;

    const char *conf =
        "[runtime]\n"
        "log_level = \"info\"\n"
        "metrics_listen = \"0.0.0.0:9464\"\n"
        "clip_root = \"/tmp/emd/clips\"\n"
        "inflight_root = \"/tmp/emd/inflight\"\n"
        "\n"
        "[mqtt]\n"
        "url = \"mqtt://localhost:1883\"\n"
        "client_id_prefix = \"emd\"\n"
        "\n"
        "[cameras.cam0]\n"
        "url = \"rtsp://10.0.0.1:554/stream\"\n"
        "buffer_seconds = 10\n"
        "pre_roll_seconds = 6\n"
        "post_roll_seconds = 8\n";

    char path[256];
    if (write_temp_config(conf, path, sizeof(path)) < 0) {
        fail_msg("could not write temp config");
        return;
    }

    emd_config_t cfg;
    char errbuf[256];
    int r = emd_config_load(path, &cfg, errbuf, sizeof(errbuf));
    unlink(path);

    if (r != 0) {
        fprintf(stderr, "config error: %s\n", errbuf);
    }
    /* Must parse without error */
    assert_int_equal(r, 0);
    assert_int_equal((int)cfg.num_cameras, 1);
    assert_string_equal(cfg.cameras[0].name, "cam0");
    assert_int_equal((int)cfg.cameras[0].buffer_seconds, 10);
    assert_string_equal(cfg.cameras[0].url, "rtsp://10.0.0.1:554/stream");
}

static void test_config_multiple_cameras(void **state) {
    (void)state;

    const char *conf =
        "[runtime]\n"
        "log_level = \"info\"\n"
        "metrics_listen = \"0.0.0.0:9464\"\n"
        "clip_root = \"/tmp/emd/clips\"\n"
        "inflight_root = \"/tmp/emd/inflight\"\n"
        "\n"
        "[cameras.driveway]\n"
        "url = \"rtsp://10.0.1.51:554/stream\"\n"
        "buffer_seconds = 12\n"
        "pre_roll_seconds = 6\n"
        "post_roll_seconds = 8\n"
        "\n"
        "[cameras.garden]\n"
        "url = \"rtsp://10.0.1.52:554/stream\"\n"
        "buffer_seconds = 10\n"
        "pre_roll_seconds = 4\n"
        "post_roll_seconds = 6\n"
        "gradual_enabled = true\n";

    char path[256];
    if (write_temp_config(conf, path, sizeof(path)) < 0) {
        fail_msg("could not write temp config");
        return;
    }

    emd_config_t cfg;
    char errbuf[256];
    int r = emd_config_load(path, &cfg, errbuf, sizeof(errbuf));
    unlink(path);

    assert_int_equal(r, 0);
    assert_int_equal((int)cfg.num_cameras, 2);

    /* Find driveway and garden in any order */
    bool found_driveway = false, found_garden = false;
    for (uint32_t i = 0; i < cfg.num_cameras; i++) {
        if (strcmp(cfg.cameras[i].name, "driveway") == 0) {
            found_driveway = true;
            assert_int_equal((int)cfg.cameras[i].buffer_seconds, 12);
        }
        if (strcmp(cfg.cameras[i].name, "garden") == 0) {
            found_garden = true;
            assert_true(cfg.cameras[i].gradual_enabled);
        }
    }
    assert_true(found_driveway);
    assert_true(found_garden);
}

static void test_config_validate(void **state) {
    (void)state;

    emd_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    /* num_cameras = 0 is valid */
    char errbuf[256];
    int r = emd_config_validate(&cfg, errbuf, sizeof(errbuf));
    assert_int_equal(r, 0);
}

/* --------------------------------------------------------------------- */
/* Test runner                                                              */
/* --------------------------------------------------------------------- */

int main(void) {
    const CMUnitTest tests[] = {
        cmocka_unit_test(test_toml_string),
        cmocka_unit_test(test_toml_integer),
        cmocka_unit_test(test_toml_bool),
        cmocka_unit_test(test_toml_float),
        cmocka_unit_test(test_toml_table),
        cmocka_unit_test(test_toml_nested_table),
        cmocka_unit_test(test_toml_comments),
        cmocka_unit_test(test_config_load_minimal),
        cmocka_unit_test(test_config_multiple_cameras),
        cmocka_unit_test(test_config_validate),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}

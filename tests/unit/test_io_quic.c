#include <unity/unity.h>
#include "http/io_quic.h"
#include <errno.h>
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

/* ---- Config tests ---- */

void test_quic_config_init_defaults(void)
{
    io_quic_config_t cfg;
    io_quic_config_init(&cfg);
    TEST_ASSERT_EQUAL_UINT32(100, cfg.max_streams_bidi);
    TEST_ASSERT_EQUAL_UINT32(256 * 1024, cfg.max_stream_data_bidi);
    TEST_ASSERT_EQUAL_UINT32(1024 * 1024, cfg.max_data);
    TEST_ASSERT_EQUAL_UINT32(30000, cfg.idle_timeout_ms);
    TEST_ASSERT_EQUAL_UINT32(1200, cfg.max_udp_payload);
    TEST_ASSERT_NULL(cfg.cert_file);
    TEST_ASSERT_NULL(cfg.key_file);
}

void test_quic_config_validate_valid(void)
{
    io_quic_config_t cfg;
    io_quic_config_init(&cfg);
    cfg.cert_file = "cert.pem";
    cfg.key_file = "key.pem";
    TEST_ASSERT_EQUAL_INT(0, io_quic_config_validate(&cfg));
}

void test_quic_config_validate_null(void)
{
    TEST_ASSERT_EQUAL_INT(-EINVAL, io_quic_config_validate(nullptr));
}

void test_quic_config_validate_no_cert(void)
{
    io_quic_config_t cfg;
    io_quic_config_init(&cfg);
    /* QUIC requires TLS — cert is mandatory */
    TEST_ASSERT_EQUAL_INT(-EINVAL, io_quic_config_validate(&cfg));
}

void test_quic_config_validate_no_key(void)
{
    io_quic_config_t cfg;
    io_quic_config_init(&cfg);
    cfg.cert_file = "cert.pem";
    TEST_ASSERT_EQUAL_INT(-EINVAL, io_quic_config_validate(&cfg));
}

void test_quic_config_validate_zero_streams(void)
{
    io_quic_config_t cfg;
    io_quic_config_init(&cfg);
    cfg.cert_file = "cert.pem";
    cfg.key_file = "key.pem";
    cfg.max_streams_bidi = 0;
    TEST_ASSERT_EQUAL_INT(-EINVAL, io_quic_config_validate(&cfg));
}

void test_quic_config_validate_zero_timeout(void)
{
    io_quic_config_t cfg;
    io_quic_config_init(&cfg);
    cfg.cert_file = "cert.pem";
    cfg.key_file = "key.pem";
    cfg.idle_timeout_ms = 0;
    TEST_ASSERT_EQUAL_INT(-EINVAL, io_quic_config_validate(&cfg));
}

/* ---- Connection lifecycle (without real handshake) ---- */

void test_quic_conn_create_null_cfg(void)
{
    io_quic_callbacks_t cbs = {0};
    uint8_t dcid[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    uint8_t scid[8] = {9, 10, 11, 12, 13, 14, 15, 16};
    struct sockaddr_in local = {.sin_family = AF_INET, .sin_port = htons(443)};
    struct sockaddr_in remote = {.sin_family = AF_INET, .sin_port = htons(12345)};
    TEST_ASSERT_NULL(io_quic_conn_create(nullptr, &cbs, dcid, 8, scid, 8,
                                          (struct sockaddr *)&local,
                                          (struct sockaddr *)&remote, nullptr));
}

void test_quic_conn_destroy_null(void)
{
    /* Must not crash */
    io_quic_conn_destroy(nullptr);
}

void test_quic_on_recv_null(void)
{
    uint8_t buf[10] = {0};
    struct sockaddr_in remote = {.sin_family = AF_INET};
    TEST_ASSERT_EQUAL_INT(-EINVAL, io_quic_on_recv(nullptr, buf, 10,
                                                     (struct sockaddr *)&remote));
}

void test_quic_flush_null(void)
{
    const uint8_t *data;
    size_t len;
    TEST_ASSERT_EQUAL_INT(-EINVAL, io_quic_flush(nullptr, &data, &len));
}

void test_quic_state_queries_null(void)
{
    TEST_ASSERT_EQUAL_UINT64(UINT64_MAX, io_quic_get_timeout(nullptr));
    TEST_ASSERT_FALSE(io_quic_is_handshake_done(nullptr));
    TEST_ASSERT_TRUE(io_quic_is_closed(nullptr)); /* null = closed */
    TEST_ASSERT_FALSE(io_quic_want_write(nullptr));
}

void test_quic_close_null(void)
{
    TEST_ASSERT_EQUAL_INT(-EINVAL, io_quic_close(nullptr, 0));
}

void test_quic_write_stream_null(void)
{
    uint8_t data[] = "hello";
    TEST_ASSERT_TRUE(io_quic_write_stream(nullptr, 0, data, 5, false) < 0);
}

int main(void)
{
    UNITY_BEGIN();
    /* Config */
    RUN_TEST(test_quic_config_init_defaults);
    RUN_TEST(test_quic_config_validate_valid);
    RUN_TEST(test_quic_config_validate_null);
    RUN_TEST(test_quic_config_validate_no_cert);
    RUN_TEST(test_quic_config_validate_no_key);
    RUN_TEST(test_quic_config_validate_zero_streams);
    RUN_TEST(test_quic_config_validate_zero_timeout);
    /* Lifecycle */
    RUN_TEST(test_quic_conn_create_null_cfg);
    RUN_TEST(test_quic_conn_destroy_null);
    /* Data processing null safety */
    RUN_TEST(test_quic_on_recv_null);
    RUN_TEST(test_quic_flush_null);
    RUN_TEST(test_quic_state_queries_null);
    RUN_TEST(test_quic_close_null);
    RUN_TEST(test_quic_write_stream_null);
    return UNITY_END();
}

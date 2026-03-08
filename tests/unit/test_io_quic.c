#include <arpa/inet.h>
#include <errno.h>
#include <stdbool.h>
#include <string.h>
#include <unity/unity.h>
#include "http/io_quic.h"

void setUp(void)
{
}
void tearDown(void)
{
}

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
    TEST_ASSERT_NULL(io_quic_conn_create(nullptr, &cbs, dcid, 8, scid, 8, (struct sockaddr *)&local,
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
    TEST_ASSERT_EQUAL_INT(-EINVAL, io_quic_on_recv(nullptr, buf, 10, (struct sockaddr *)&remote));
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

/* ---- Handshake tests (require test certs) ---- */

static io_quic_config_t test_server_config(void)
{
    io_quic_config_t cfg;
    io_quic_config_init(&cfg);
    cfg.cert_file = TEST_CERTS_DIR "/server.crt";
    cfg.key_file = TEST_CERTS_DIR "/server.key";
    return cfg;
}

/* Track callback invocations */
static bool g_handshake_done;
static int g_streams_opened;
static int g_data_received;
static uint8_t g_received_data[4096];
static size_t g_received_len;

static void reset_globals(void)
{
    g_handshake_done = false;
    g_streams_opened = 0;
    g_data_received = 0;
    g_received_len = 0;
    memset(g_received_data, 0, sizeof(g_received_data));
}

static void on_handshake_done(io_quic_conn_t *conn, void *user_data)
{
    (void)conn;
    (void)user_data;
    g_handshake_done = true;
}

static int on_stream_open(io_quic_conn_t *conn, int64_t stream_id, void *user_data)
{
    (void)conn;
    (void)stream_id;
    (void)user_data;
    g_streams_opened++;
    return 0;
}

static int on_stream_data(io_quic_conn_t *conn, int64_t stream_id, const uint8_t *data, size_t len,
                          bool fin, void *user_data)
{
    (void)conn;
    (void)stream_id;
    (void)fin;
    (void)user_data;
    g_data_received++;
    if (g_received_len + len <= sizeof(g_received_data)) {
        memcpy(g_received_data + g_received_len, data, len);
        g_received_len += len;
    }
    return 0;
}

void test_quic_conn_create_with_certs(void)
{
    reset_globals();
    io_quic_config_t cfg = test_server_config();
    io_quic_callbacks_t cbs = {
        .on_stream_data = on_stream_data,
        .on_stream_open = on_stream_open,
        .on_handshake_done = on_handshake_done,
    };
    uint8_t dcid[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    uint8_t scid[8] = {9, 10, 11, 12, 13, 14, 15, 16};
    struct sockaddr_in local = {.sin_family = AF_INET, .sin_port = htons(443)};
    struct sockaddr_in remote = {.sin_family = AF_INET, .sin_port = htons(12345)};

    io_quic_conn_t *conn = io_quic_conn_create(&cfg, &cbs, dcid, 8, scid, 8,
                                               (struct sockaddr *)&local,
                                               (struct sockaddr *)&remote, nullptr);
    /* Connection creation may fail if cert files don't exist */
    if (conn == nullptr) {
        TEST_IGNORE_MESSAGE("Could not create QUIC conn (cert files missing?)");
        return;
    }
    TEST_ASSERT_FALSE(io_quic_is_handshake_done(conn));
    TEST_ASSERT_FALSE(io_quic_is_closed(conn));

    /* Server should have initial data to send (crypto frames) */
    const uint8_t *out;
    size_t out_len;
    int rc = io_quic_flush(conn, &out, &out_len);
    TEST_ASSERT_EQUAL_INT(0, rc);

    io_quic_conn_destroy(conn);
}

void test_quic_get_timeout_not_max_after_create(void)
{
    io_quic_config_t cfg = test_server_config();
    io_quic_callbacks_t cbs = {0};
    uint8_t dcid[8] = {1};
    uint8_t scid[8] = {2};
    struct sockaddr_in local = {.sin_family = AF_INET, .sin_port = htons(443)};
    struct sockaddr_in remote = {.sin_family = AF_INET, .sin_port = htons(12345)};

    io_quic_conn_t *conn = io_quic_conn_create(&cfg, &cbs, dcid, 8, scid, 8,
                                               (struct sockaddr *)&local,
                                               (struct sockaddr *)&remote, nullptr);
    if (conn == nullptr) {
        TEST_IGNORE_MESSAGE("cert files missing");
        return;
    }
    /* After creation, there should be a timeout (handshake timers) */
    uint64_t timeout = io_quic_get_timeout(conn);
    TEST_ASSERT_TRUE(timeout < UINT64_MAX);
    io_quic_conn_destroy(conn);
}

void test_quic_close_sets_closed(void)
{
    io_quic_config_t cfg = test_server_config();
    io_quic_callbacks_t cbs = {0};
    uint8_t dcid[8] = {1};
    uint8_t scid[8] = {2};
    struct sockaddr_in local = {.sin_family = AF_INET, .sin_port = htons(443)};
    struct sockaddr_in remote = {.sin_family = AF_INET, .sin_port = htons(12345)};

    io_quic_conn_t *conn = io_quic_conn_create(&cfg, &cbs, dcid, 8, scid, 8,
                                               (struct sockaddr *)&local,
                                               (struct sockaddr *)&remote, nullptr);
    if (conn == nullptr) {
        TEST_IGNORE_MESSAGE("cert files missing");
        return;
    }
    int rc = io_quic_close(conn, 0);
    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_TRUE(io_quic_is_closed(conn));
    io_quic_conn_destroy(conn);
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
    /* Handshake (require test certs) */
    RUN_TEST(test_quic_conn_create_with_certs);
    RUN_TEST(test_quic_get_timeout_not_max_after_create);
    RUN_TEST(test_quic_close_sets_closed);
    return UNITY_END();
}

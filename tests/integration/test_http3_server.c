/**
 * @file test_http3_server.c
 * @brief Integration tests for QUIC + HTTP/3 session lifecycle.
 *
 * In-memory QUIC server connection lifecycle + HTTP/3 session creation.
 * No real UDP sockets — just verifying the layers work together.
 * Same approach as test_http2_server.c.
 */

#include "core/ioh_ctx.h"
#include "http/ioh_http3.h"
#include "http/ioh_quic.h"
#include "http/ioh_request.h"
#include "http/ioh_response.h"

#include <string.h>

#include <unity.h>

void setUp(void)
{
}
void tearDown(void)
{
}

/* ---- Certificate paths ---- */

static const char *TEST_SERVER_CERT;
static const char *TEST_SERVER_KEY;

static void init_cert_paths(void)
{
#ifdef TEST_CERTS_DIR
    static char server_cert[512];
    static char server_key[512];

    snprintf(server_cert, sizeof(server_cert), "%s/server-cert.pem", TEST_CERTS_DIR);
    snprintf(server_key, sizeof(server_key), "%s/server-key.pem", TEST_CERTS_DIR);

    TEST_SERVER_CERT = server_cert;
    TEST_SERVER_KEY = server_key;
#else
    TEST_SERVER_CERT = "tests/certs/server-cert.pem";
    TEST_SERVER_KEY = "tests/certs/server-key.pem";
#endif
}

/* ---- Test request handler ---- */

static int test_handler(ioh_ctx_t *c, int64_t stream_id, void *user_data)
{
    (void)stream_id;
    (void)user_data;
    return ioh_ctx_json(c, 200, "{\"protocol\":\"h3\"}");
}

/* ---- Tests ---- */

void test_quic_server_conn_lifecycle(void)
{
    ioh_quic_config_t cfg;
    ioh_quic_config_init(&cfg);
    cfg.cert_file = TEST_SERVER_CERT;
    cfg.key_file = TEST_SERVER_KEY;
    ioh_quic_callbacks_t cbs = {0};
    uint8_t dcid[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    uint8_t scid[8] = {9, 10, 11, 12, 13, 14, 15, 16};
    struct sockaddr_in local = {.sin_family = AF_INET, .sin_port = htons(443)};
    struct sockaddr_in remote = {.sin_family = AF_INET, .sin_port = htons(12345)};

    ioh_quic_conn_t *conn = ioh_quic_conn_create(&cfg, &cbs, dcid, 8, scid, 8,
                                               (struct sockaddr *)&local,
                                               (struct sockaddr *)&remote, nullptr);
    if (conn == nullptr) {
        TEST_IGNORE_MESSAGE("QUIC conn creation failed (certs?)");
        return;
    }

    /* Verify initial state */
    TEST_ASSERT_FALSE(ioh_quic_is_handshake_done(conn));
    TEST_ASSERT_FALSE(ioh_quic_is_closed(conn));
    TEST_ASSERT_TRUE(ioh_quic_want_write(conn));

    /* Close gracefully */
    TEST_ASSERT_EQUAL_INT(0, ioh_quic_close(conn, 0));
    ioh_quic_conn_destroy(conn);
}

void test_http3_session_on_quic_conn(void)
{
    ioh_quic_config_t qcfg;
    ioh_quic_config_init(&qcfg);
    qcfg.cert_file = TEST_SERVER_CERT;
    qcfg.key_file = TEST_SERVER_KEY;
    ioh_quic_callbacks_t cbs = {0};
    uint8_t dcid[8] = {1}; //-V1009
    uint8_t scid[8] = {2}; //-V1009
    struct sockaddr_in local = {.sin_family = AF_INET, .sin_port = htons(443)};
    struct sockaddr_in remote = {.sin_family = AF_INET, .sin_port = htons(12345)};

    ioh_quic_conn_t *qconn = ioh_quic_conn_create(&qcfg, &cbs, dcid, 8, scid, 8,
                                                (struct sockaddr *)&local,
                                                (struct sockaddr *)&remote, nullptr);
    if (qconn == nullptr) {
        TEST_IGNORE_MESSAGE("QUIC conn failed");
        return;
    }

    ioh_http3_config_t h3cfg;
    ioh_http3_config_init(&h3cfg);
    ioh_http3_session_t *h3 = ioh_http3_session_create(&h3cfg, qconn, test_handler, nullptr);
    TEST_ASSERT_NOT_NULL(h3);
    TEST_ASSERT_FALSE(ioh_http3_is_shutdown(h3));

    /* Shutdown */
    TEST_ASSERT_EQUAL_INT(0, ioh_http3_shutdown(h3));
    TEST_ASSERT_TRUE(ioh_http3_is_shutdown(h3));

    ioh_http3_session_destroy(h3);
    ioh_quic_conn_destroy(qconn);
}

void test_alt_svc_in_response(void)
{
    ioh_response_t resp;
    int rc = ioh_response_init(&resp);
    TEST_ASSERT_EQUAL_INT(0, rc);

    rc = ioh_response_set_header(&resp, "alt-svc", "h3=\":443\"");
    TEST_ASSERT_EQUAL_INT(0, rc);

    bool found = false;
    for (uint32_t i = 0; i < resp.header_count; i++) {
        if (resp.headers[i].name_len == 7 && memcmp(resp.headers[i].name, "alt-svc", 7) == 0) {
            found = true;
        }
    }
    TEST_ASSERT_TRUE(found);
    ioh_response_destroy(&resp);
}

void test_quic_conn_destroy_after_close(void)
{
    ioh_quic_config_t cfg;
    ioh_quic_config_init(&cfg);
    cfg.cert_file = TEST_SERVER_CERT;
    cfg.key_file = TEST_SERVER_KEY;
    ioh_quic_callbacks_t cbs = {0};
    uint8_t dcid[8] = {1}; //-V1009
    uint8_t scid[8] = {2}; //-V1009
    struct sockaddr_in local = {.sin_family = AF_INET, .sin_port = htons(443)};
    struct sockaddr_in remote = {.sin_family = AF_INET, .sin_port = htons(12345)};

    ioh_quic_conn_t *conn = ioh_quic_conn_create(&cfg, &cbs, dcid, 8, scid, 8,
                                               (struct sockaddr *)&local,
                                               (struct sockaddr *)&remote, nullptr);
    if (conn == nullptr) {
        TEST_IGNORE_MESSAGE("QUIC conn failed");
        return;
    }
    (void)ioh_quic_close(conn, 0);
    ioh_quic_conn_destroy(conn);
}

int main(void)
{
    init_cert_paths();

    UNITY_BEGIN();
    RUN_TEST(test_quic_server_conn_lifecycle);
    RUN_TEST(test_http3_session_on_quic_conn);
    RUN_TEST(test_alt_svc_in_response);
    RUN_TEST(test_quic_conn_destroy_after_close);
    return UNITY_END();
}

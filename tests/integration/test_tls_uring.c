/**
 * @file test_tls_uring.c
 * @brief Integration tests for TLS buffer-based I/O path.
 *
 * Tests the buffer-based TLS API that enables io_uring integration.
 * Both server and client sides use custom I/O callbacks that read/write
 * from internal cipher buffers — no socket I/O occurs during TLS operations.
 * A shuttle function transfers ciphertext between the two sides in memory.
 */

#include "tls/ioh_tls.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <wolfssl/options.h>
#include <wolfssl/ssl.h>

#include <unity.h>

/* ---- Certificate paths ---- */

static const char *TEST_CA_CERT;
static const char *TEST_SERVER_CERT;
static const char *TEST_SERVER_KEY;

static void init_cert_paths(void)
{
#ifdef TEST_CERTS_DIR
    static char ca_cert[512];
    static char server_cert[512];
    static char server_key[512];

    snprintf(ca_cert, sizeof(ca_cert), "%s/ca-cert.pem", TEST_CERTS_DIR);
    snprintf(server_cert, sizeof(server_cert), "%s/server-cert.pem", TEST_CERTS_DIR);
    snprintf(server_key, sizeof(server_key), "%s/server-key.pem", TEST_CERTS_DIR);

    TEST_CA_CERT = ca_cert;
    TEST_SERVER_CERT = server_cert;
    TEST_SERVER_KEY = server_key;
#else
    TEST_CA_CERT = "tests/certs/ca-cert.pem";
    TEST_SERVER_CERT = "tests/certs/server-cert.pem";
    TEST_SERVER_KEY = "tests/certs/server-key.pem";
#endif
}

/* ---- Client-side TLS state (mirrors server's buffer-based approach) ---- */

typedef struct {
    WOLFSSL_CTX *ctx;
    WOLFSSL *ssl;
    uint8_t cipher_in_buf[IOH_TLS_CIPHER_BUF_SIZE];
    size_t cipher_in_len;
    size_t cipher_in_pos;
    uint8_t cipher_out_buf[IOH_TLS_CIPHER_BUF_SIZE];
    size_t cipher_out_len;
} test_tls_client_t;

/* ---- Client I/O callbacks ---- */

static int client_recv_cb(WOLFSSL *ssl, char *buf, int sz, void *ctx)
{
    (void)ssl;
    test_tls_client_t *client = ctx;

    size_t avail = client->cipher_in_len - client->cipher_in_pos;
    if (avail == 0) {
        return WOLFSSL_CBIO_ERR_WANT_READ;
    }

    size_t to_copy = (size_t)sz < avail ? (size_t)sz : avail;
    memcpy(buf, client->cipher_in_buf + client->cipher_in_pos, to_copy);
    client->cipher_in_pos += to_copy;

    /* Compact when fully consumed */
    if (client->cipher_in_pos == client->cipher_in_len) {
        client->cipher_in_pos = 0;
        client->cipher_in_len = 0;
    }

    return (int)to_copy;
}

static int client_send_cb(WOLFSSL *ssl, char *buf, int sz, void *ctx)
{
    (void)ssl;
    test_tls_client_t *client = ctx;

    size_t space = IOH_TLS_CIPHER_BUF_SIZE - client->cipher_out_len;
    if (space == 0) {
        return WOLFSSL_CBIO_ERR_WANT_WRITE;
    }

    size_t to_copy = (size_t)sz < space ? (size_t)sz : space;
    memcpy(client->cipher_out_buf + client->cipher_out_len, buf, to_copy);
    client->cipher_out_len += to_copy;

    return (int)to_copy;
}

/* ---- Client lifecycle helpers ---- */

static test_tls_client_t *client_create(void)
{
    test_tls_client_t *client = calloc(1, sizeof(*client));
    if (client == nullptr) {
        return nullptr;
    }

    client->ctx = wolfSSL_CTX_new(wolfTLSv1_3_client_method());
    if (client->ctx == nullptr) {
        free(client);
        return nullptr;
    }

    if (wolfSSL_CTX_load_verify_locations(client->ctx, TEST_CA_CERT, nullptr) != WOLFSSL_SUCCESS) {
        wolfSSL_CTX_free(client->ctx);
        free(client);
        return nullptr;
    }

    /* Set buffer-based I/O callbacks on CTX */
    wolfSSL_CTX_SetIORecv(client->ctx, client_recv_cb);
    wolfSSL_CTX_SetIOSend(client->ctx, client_send_cb);

    client->ssl = wolfSSL_new(client->ctx);
    if (client->ssl == nullptr) {
        wolfSSL_CTX_free(client->ctx);
        free(client);
        return nullptr;
    }

    /* Point I/O context to our client struct */
    wolfSSL_SetIOReadCtx(client->ssl, client);
    wolfSSL_SetIOWriteCtx(client->ssl, client);

    return client;
}

static void client_destroy(test_tls_client_t *client)
{
    if (client == nullptr) {
        return;
    }
    if (client->ssl != nullptr) {
        wolfSSL_free(client->ssl);
    }
    if (client->ctx != nullptr) {
        wolfSSL_CTX_free(client->ctx);
    }
    free(client);
}

/* ---- Shuttle: transfer ciphertext between server and client ---- */

static void shuttle_data(ioh_tls_conn_t *server, test_tls_client_t *client)
{
    /* Server output -> client input */
    const uint8_t *sout;
    size_t slen;
    (void)ioh_tls_get_output(server, &sout, &slen);
    if (slen > 0) {
        size_t space = IOH_TLS_CIPHER_BUF_SIZE - client->cipher_in_len;
        size_t copy = slen < space ? slen : space;
        if (copy > 0) {
            memcpy(client->cipher_in_buf + client->cipher_in_len, sout, copy);
            client->cipher_in_len += copy;
            ioh_tls_consume_output(server, copy);
        }
    }

    /* Client output -> server input */
    if (client->cipher_out_len > 0) {
        (void)ioh_tls_feed_input(server, client->cipher_out_buf, client->cipher_out_len);
        client->cipher_out_len = 0;
    }
}

/* ---- Handshake helper: drive both sides to completion ---- */

static bool do_buffer_handshake(ioh_tls_conn_t *server, test_tls_client_t *client)
{
    bool server_done = false;
    bool client_done = false;

    for (int i = 0; i < 200; i++) {
        shuttle_data(server, client);

        if (!server_done) {
            int ret = ioh_tls_handshake(server);
            if (ret == 0) {
                server_done = true;
            } else if (ret != -EAGAIN) {
                return false;
            }
        }

        shuttle_data(server, client);

        if (!client_done) {
            int ret = wolfSSL_connect(client->ssl);
            if (ret == WOLFSSL_SUCCESS) {
                client_done = true;
            } else {
                int err = wolfSSL_get_error(client->ssl, ret);
                if (err != WOLFSSL_ERROR_WANT_READ && err != WOLFSSL_ERROR_WANT_WRITE) {
                    return false;
                }
            }
        }

        shuttle_data(server, client);

        if (server_done && client_done) {
            return true;
        }
    }

    return false;
}

/* ---- Test setup/teardown ---- */

void setUp(void)
{
}

void tearDown(void)
{
}

/* ---- Test: buffer-based handshake ---- */

void test_tls_buffer_handshake(void)
{
    ioh_tls_config_t cfg;
    ioh_tls_config_init(&cfg);
    cfg.cert_file = TEST_SERVER_CERT;
    cfg.key_file = TEST_SERVER_KEY;

    ioh_tls_ctx_t *sctx = ioh_tls_ctx_create(&cfg);
    TEST_ASSERT_NOT_NULL(sctx);

    ioh_tls_conn_t *sconn = ioh_tls_conn_create(sctx, -1);
    TEST_ASSERT_NOT_NULL(sconn);

    test_tls_client_t *client = client_create();
    TEST_ASSERT_NOT_NULL(client);

    bool ok = do_buffer_handshake(sconn, client);
    TEST_ASSERT_TRUE_MESSAGE(ok, "Buffer-based TLS handshake failed");
    TEST_ASSERT_TRUE(sconn->handshake_done);

    client_destroy(client);
    ioh_tls_conn_destroy(sconn);
    ioh_tls_ctx_destroy(sctx);
}

/* ---- Test: data roundtrip through buffer API ---- */

void test_tls_buffer_data_roundtrip(void)
{
    ioh_tls_config_t cfg;
    ioh_tls_config_init(&cfg);
    cfg.cert_file = TEST_SERVER_CERT;
    cfg.key_file = TEST_SERVER_KEY;

    ioh_tls_ctx_t *sctx = ioh_tls_ctx_create(&cfg);
    TEST_ASSERT_NOT_NULL(sctx);

    ioh_tls_conn_t *sconn = ioh_tls_conn_create(sctx, -1);
    TEST_ASSERT_NOT_NULL(sconn);

    test_tls_client_t *client = client_create();
    TEST_ASSERT_NOT_NULL(client);

    bool ok = do_buffer_handshake(sconn, client);
    TEST_ASSERT_TRUE(ok);

    /* Server -> Client: write plaintext through buffer API */
    const char *server_msg = "Hello from server";
    int wret = ioh_tls_write(sconn, (const uint8_t *)server_msg, strlen(server_msg));
    TEST_ASSERT_EQUAL_INT((int)strlen(server_msg), wret);

    /* Shuttle server ciphertext to client */
    shuttle_data(sconn, client);

    /* Client reads plaintext */
    char rbuf[256];
    int rret = wolfSSL_read(client->ssl, rbuf, (int)sizeof(rbuf));
    TEST_ASSERT_EQUAL_INT((int)strlen(server_msg), rret);
    TEST_ASSERT_EQUAL_STRING_LEN(server_msg, rbuf, (size_t)rret);

    /* Client -> Server: write plaintext */
    const char *client_msg = "Hello from client";
    int cwret = wolfSSL_write(client->ssl, client_msg, (int)strlen(client_msg));
    TEST_ASSERT_EQUAL_INT((int)strlen(client_msg), cwret);

    /* Shuttle client ciphertext to server */
    shuttle_data(sconn, client);

    /* Server reads plaintext through buffer API */
    memset(rbuf, 0, sizeof(rbuf));
    rret = ioh_tls_read(sconn, (uint8_t *)rbuf, sizeof(rbuf));
    TEST_ASSERT_EQUAL_INT((int)strlen(client_msg), rret);
    TEST_ASSERT_EQUAL_STRING_LEN(client_msg, rbuf, (size_t)rret);

    client_destroy(client);
    ioh_tls_conn_destroy(sconn);
    ioh_tls_ctx_destroy(sctx);
}

/* ---- Test: multiple messages in sequence ---- */

void test_tls_buffer_multiple_messages(void)
{
    ioh_tls_config_t cfg;
    ioh_tls_config_init(&cfg);
    cfg.cert_file = TEST_SERVER_CERT;
    cfg.key_file = TEST_SERVER_KEY;

    ioh_tls_ctx_t *sctx = ioh_tls_ctx_create(&cfg);
    TEST_ASSERT_NOT_NULL(sctx);

    ioh_tls_conn_t *sconn = ioh_tls_conn_create(sctx, -1);
    TEST_ASSERT_NOT_NULL(sconn);

    test_tls_client_t *client = client_create();
    TEST_ASSERT_NOT_NULL(client);

    bool ok = do_buffer_handshake(sconn, client);
    TEST_ASSERT_TRUE(ok);

    /* Send 5 messages server -> client, verify each */
    constexpr int NUM_MSGS = 5;
    const char *messages[5] = {
        "Message one", "Second message here", "Third", "A slightly longer fourth message payload",
        "Five",
    };

    for (int i = 0; i < NUM_MSGS; i++) {
        const char *msg = messages[i];
        size_t mlen = strlen(msg);

        int wret = ioh_tls_write(sconn, (const uint8_t *)msg, mlen);
        TEST_ASSERT_EQUAL_INT((int)mlen, wret);

        shuttle_data(sconn, client);

        char rbuf[256];
        int rret = wolfSSL_read(client->ssl, rbuf, (int)sizeof(rbuf));
        TEST_ASSERT_EQUAL_INT((int)mlen, rret);
        TEST_ASSERT_EQUAL_STRING_LEN(msg, rbuf, mlen);
    }

    /* Send 5 messages client -> server, verify each */
    for (int i = 0; i < NUM_MSGS; i++) {
        const char *msg = messages[i];
        size_t mlen = strlen(msg);

        int wret = wolfSSL_write(client->ssl, msg, (int)mlen);
        TEST_ASSERT_EQUAL_INT((int)mlen, wret);

        shuttle_data(sconn, client);

        char rbuf[256];
        int rret = ioh_tls_read(sconn, (uint8_t *)rbuf, sizeof(rbuf));
        TEST_ASSERT_EQUAL_INT((int)mlen, rret);
        TEST_ASSERT_EQUAL_STRING_LEN(msg, rbuf, mlen);
    }

    client_destroy(client);
    ioh_tls_conn_destroy(sconn);
    ioh_tls_ctx_destroy(sctx);
}

/* ---- Test: large payload (8KB) through buffer API ---- */

void test_tls_buffer_large_payload(void)
{
    ioh_tls_config_t cfg;
    ioh_tls_config_init(&cfg);
    cfg.cert_file = TEST_SERVER_CERT;
    cfg.key_file = TEST_SERVER_KEY;

    ioh_tls_ctx_t *sctx = ioh_tls_ctx_create(&cfg);
    TEST_ASSERT_NOT_NULL(sctx);

    ioh_tls_conn_t *sconn = ioh_tls_conn_create(sctx, -1);
    TEST_ASSERT_NOT_NULL(sconn);

    test_tls_client_t *client = client_create();
    TEST_ASSERT_NOT_NULL(client);

    bool ok = do_buffer_handshake(sconn, client);
    TEST_ASSERT_TRUE(ok);

    /* Build an 8KB payload with recognizable pattern */
    constexpr size_t PAYLOAD_SIZE = 8192;
    uint8_t *payload = malloc(PAYLOAD_SIZE);
    TEST_ASSERT_NOT_NULL(payload);
    for (size_t i = 0; i < PAYLOAD_SIZE; i++) {
        payload[i] = (uint8_t)(i & 0xFF);
    }

    uint8_t *received = malloc(PAYLOAD_SIZE);
    TEST_ASSERT_NOT_NULL(received);

    /* Server writes large payload in chunks (buffer may not accept all at once) */
    size_t total_written = 0;
    size_t total_read = 0;

    while (total_written < PAYLOAD_SIZE || total_read < PAYLOAD_SIZE) {
        /* Write as much as possible */
        if (total_written < PAYLOAD_SIZE) {
            size_t remaining = PAYLOAD_SIZE - total_written;
            int wret = ioh_tls_write(sconn, payload + total_written, remaining);
            if (wret > 0) {
                total_written += (size_t)wret;
            }
        }

        shuttle_data(sconn, client);

        /* Client reads as much as available */
        if (total_read < PAYLOAD_SIZE) {
            size_t remaining = PAYLOAD_SIZE - total_read;
            int rret = wolfSSL_read(client->ssl, received + total_read, (int)remaining);
            if (rret > 0) {
                total_read += (size_t)rret;
            }
        }

        shuttle_data(sconn, client);
    }

    TEST_ASSERT_EQUAL_size_t(PAYLOAD_SIZE, total_written);
    TEST_ASSERT_EQUAL_size_t(PAYLOAD_SIZE, total_read);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(payload, received, PAYLOAD_SIZE);

    free(received);
    free(payload);
    client_destroy(client);
    ioh_tls_conn_destroy(sconn);
    ioh_tls_ctx_destroy(sctx);
}

/* ---- Test: graceful shutdown via buffer API ---- */

void test_tls_buffer_shutdown(void)
{
    ioh_tls_config_t cfg;
    ioh_tls_config_init(&cfg);
    cfg.cert_file = TEST_SERVER_CERT;
    cfg.key_file = TEST_SERVER_KEY;

    ioh_tls_ctx_t *sctx = ioh_tls_ctx_create(&cfg);
    TEST_ASSERT_NOT_NULL(sctx);

    ioh_tls_conn_t *sconn = ioh_tls_conn_create(sctx, -1);
    TEST_ASSERT_NOT_NULL(sconn);

    test_tls_client_t *client = client_create();
    TEST_ASSERT_NOT_NULL(client);

    bool ok = do_buffer_handshake(sconn, client);
    TEST_ASSERT_TRUE(ok);

    /* Verify data works before shutdown */
    const char *msg = "pre-shutdown";
    int wret = ioh_tls_write(sconn, (const uint8_t *)msg, strlen(msg));
    TEST_ASSERT_EQUAL_INT((int)strlen(msg), wret);
    shuttle_data(sconn, client);
    char rbuf[64];
    int rret = wolfSSL_read(client->ssl, rbuf, (int)sizeof(rbuf));
    TEST_ASSERT_EQUAL_INT((int)strlen(msg), rret);

    /* Server initiates shutdown */
    bool shutdown_done = false;
    for (int i = 0; i < 100; i++) {
        int sret = ioh_tls_shutdown(sconn);
        shuttle_data(sconn, client);

        /* Client processes the close_notify */
        int cret = wolfSSL_shutdown(client->ssl);
        shuttle_data(sconn, client);

        if (sret == 0 || cret == WOLFSSL_SUCCESS) {
            shutdown_done = true;
            break;
        }
    }

    TEST_ASSERT_TRUE_MESSAGE(shutdown_done, "TLS shutdown did not complete");

    client_destroy(client);
    ioh_tls_conn_destroy(sconn);
    ioh_tls_ctx_destroy(sctx);
}

/* ---- Test runner ---- */

int main(void)
{
    init_cert_paths();

    UNITY_BEGIN();

    RUN_TEST(test_tls_buffer_handshake);
    RUN_TEST(test_tls_buffer_data_roundtrip);
    RUN_TEST(test_tls_buffer_multiple_messages);
    RUN_TEST(test_tls_buffer_large_payload);
    RUN_TEST(test_tls_buffer_shutdown);

    return UNITY_END();
}

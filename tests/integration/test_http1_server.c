/**
 * @file test_http1_server.c
 * @brief Integration tests for the full HTTP/1.1 pipeline.
 *
 * Exercises: parse request -> build response -> serialize,
 * keep-alive, connection close, POST bodies, TLS + HTTP,
 * and PROXY protocol + HTTP chaining.
 */

#include "http/io_http1.h"
#include "http/io_proxy_proto.h"
#include "http/io_request.h"
#include "http/io_response.h"
#include "tls/io_tls.h"

#include <errno.h>
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

/* ---- Test setup/teardown ---- */

void setUp(void)
{
}

void tearDown(void)
{
}

/* ---- Helper: check substring in buffer ---- */

static bool buf_contains(const uint8_t *buf, size_t len, const char *needle)
{
    size_t nlen = strlen(needle);
    if (nlen > len) {
        return false;
    }
    for (size_t i = 0; i + nlen <= len; i++) {
        if (memcmp(buf + i, needle, nlen) == 0) {
            return true;
        }
    }
    return false;
}

/* ---- Test 1: Full request/response cycle ---- */

void test_http1_full_request_response(void)
{
    const char *raw = "GET /api/health HTTP/1.1\r\n"
                      "Host: localhost\r\n"
                      "\r\n";

    io_request_t req;
    int consumed = io_http1_parse_request((const uint8_t *)raw, strlen(raw), &req);
    TEST_ASSERT_GREATER_THAN(0, consumed);
    TEST_ASSERT_EQUAL(IO_METHOD_GET, req.method);
    TEST_ASSERT_EQUAL_STRING_LEN("/api/health", req.path, req.path_len);
    TEST_ASSERT_NOT_NULL(req.host);
    TEST_ASSERT_TRUE(req.keep_alive);

    /* Build JSON response */
    io_response_t resp;
    int rc = io_response_init(&resp);
    TEST_ASSERT_EQUAL_INT(0, rc);

    rc = io_respond_json(&resp, 200, "{\"status\":\"ok\"}");
    TEST_ASSERT_EQUAL_INT(0, rc);

    /* Serialize */
    uint8_t out[4096];
    int slen = io_http1_serialize_response(&resp, out, sizeof(out));
    TEST_ASSERT_GREATER_THAN(0, slen);

    /* Verify serialized output */
    TEST_ASSERT_TRUE(buf_contains(out, (size_t)slen, "HTTP/1.1 200 OK"));
    TEST_ASSERT_TRUE(buf_contains(out, (size_t)slen, "Content-Type: application/json"));
    TEST_ASSERT_TRUE(buf_contains(out, (size_t)slen, "{\"status\":\"ok\"}"));

    io_response_destroy(&resp);
}

/* ---- Test 2: Keep-alive with multiple sequential requests ---- */

void test_http1_keepalive_multiple(void)
{
    const char *requests[] = {
        "GET /path/one HTTP/1.1\r\nHost: localhost\r\n\r\n",
        "GET /path/two HTTP/1.1\r\nHost: localhost\r\n\r\n",
        "GET /path/three HTTP/1.1\r\nHost: localhost\r\n\r\n",
    };
    const char *expected_paths[] = {"/path/one", "/path/two", "/path/three"};
    size_t expected_lens[] = {9, 9, 11};

    for (int i = 0; i < 3; i++) {
        io_request_t req;
        int consumed =
            io_http1_parse_request((const uint8_t *)requests[i], strlen(requests[i]), &req);
        TEST_ASSERT_GREATER_THAN(0, consumed);
        TEST_ASSERT_EQUAL(IO_METHOD_GET, req.method);
        TEST_ASSERT_EQUAL_size_t(expected_lens[i], req.path_len);
        TEST_ASSERT_EQUAL_STRING_LEN(expected_paths[i], req.path, req.path_len);
        TEST_ASSERT_TRUE_MESSAGE(req.keep_alive, "HTTP/1.1 should default keep-alive");
    }
}

/* ---- Test 3: Connection: close handling ---- */

void test_http1_connection_close(void)
{
    const char *raw = "GET /goodbye HTTP/1.1\r\n"
                      "Host: localhost\r\n"
                      "Connection: close\r\n"
                      "\r\n";

    io_request_t req;
    int consumed = io_http1_parse_request((const uint8_t *)raw, strlen(raw), &req);
    TEST_ASSERT_GREATER_THAN(0, consumed);
    TEST_ASSERT_EQUAL(IO_METHOD_GET, req.method);
    TEST_ASSERT_FALSE_MESSAGE(req.keep_alive, "Connection: close must set keep_alive=false");

    /* Build response with Connection: close */
    io_response_t resp;
    int rc = io_response_init(&resp);
    TEST_ASSERT_EQUAL_INT(0, rc);

    rc = io_respond(&resp, 200, "text/plain", (const uint8_t *)"bye", 3);
    TEST_ASSERT_EQUAL_INT(0, rc);

    rc = io_response_set_header(&resp, "Connection", "close");
    TEST_ASSERT_EQUAL_INT(0, rc);

    uint8_t out[4096];
    int slen = io_http1_serialize_response(&resp, out, sizeof(out));
    TEST_ASSERT_GREATER_THAN(0, slen);

    TEST_ASSERT_TRUE(buf_contains(out, (size_t)slen, "Connection: close"));
    TEST_ASSERT_TRUE(buf_contains(out, (size_t)slen, "bye"));

    io_response_destroy(&resp);
}

/* ---- Test 4: POST with body ---- */

void test_http1_post_with_body(void)
{
    const char *body = "{\"key\":\"value\"}";
    char raw[1024];
    int rawlen = snprintf(raw, sizeof(raw),
                          "POST /api/data HTTP/1.1\r\n"
                          "Host: localhost\r\n"
                          "Content-Type: application/json\r\n"
                          "Content-Length: %zu\r\n"
                          "\r\n"
                          "%s",
                          strlen(body), body);
    TEST_ASSERT_GREATER_THAN(0, rawlen);

    io_request_t req;
    int consumed = io_http1_parse_request((const uint8_t *)raw, (size_t)rawlen, &req);
    TEST_ASSERT_GREATER_THAN(0, consumed);
    TEST_ASSERT_EQUAL(IO_METHOD_POST, req.method);
    TEST_ASSERT_EQUAL_STRING_LEN("/api/data", req.path, req.path_len);
    TEST_ASSERT_NOT_NULL(req.content_type);
    TEST_ASSERT_EQUAL_size_t(strlen(body), req.content_length);

    /* The body starts after the headers; verify we can read it */
    const uint8_t *req_body = (const uint8_t *)raw + consumed;
    size_t req_body_len = (size_t)rawlen - (size_t)consumed;
    TEST_ASSERT_GREATER_OR_EQUAL(strlen(body), req_body_len);
    TEST_ASSERT_EQUAL_STRING_LEN(body, (const char *)req_body, strlen(body));

    /* Echo body back in response */
    io_response_t resp;
    int rc = io_response_init(&resp);
    TEST_ASSERT_EQUAL_INT(0, rc);

    rc = io_respond_json(&resp, 200, body);
    TEST_ASSERT_EQUAL_INT(0, rc);

    uint8_t out[4096];
    int slen = io_http1_serialize_response(&resp, out, sizeof(out));
    TEST_ASSERT_GREATER_THAN(0, slen);

    TEST_ASSERT_TRUE(buf_contains(out, (size_t)slen, body));
    TEST_ASSERT_TRUE(buf_contains(out, (size_t)slen, "HTTP/1.1 200 OK"));

    io_response_destroy(&resp);
}

/* ---- TLS client helpers (adapted from test_tls_uring.c) ---- */

typedef struct {
    WOLFSSL_CTX *ctx;
    WOLFSSL *ssl;
    uint8_t cipher_in_buf[IO_TLS_CIPHER_BUF_SIZE];
    size_t cipher_in_len;
    size_t cipher_in_pos;
    uint8_t cipher_out_buf[IO_TLS_CIPHER_BUF_SIZE];
    size_t cipher_out_len;
} test_tls_client_t;

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

    size_t space = IO_TLS_CIPHER_BUF_SIZE - client->cipher_out_len;
    if (space == 0) {
        return WOLFSSL_CBIO_ERR_WANT_WRITE;
    }

    size_t to_copy = (size_t)sz < space ? (size_t)sz : space;
    memcpy(client->cipher_out_buf + client->cipher_out_len, buf, to_copy);
    client->cipher_out_len += to_copy;

    return (int)to_copy;
}

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

    wolfSSL_CTX_SetIORecv(client->ctx, client_recv_cb);
    wolfSSL_CTX_SetIOSend(client->ctx, client_send_cb);

    client->ssl = wolfSSL_new(client->ctx);
    if (client->ssl == nullptr) {
        wolfSSL_CTX_free(client->ctx);
        free(client);
        return nullptr;
    }

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

static void shuttle_data(io_tls_conn_t *server, test_tls_client_t *client)
{
    const uint8_t *sout;
    size_t slen;
    (void)io_tls_get_output(server, &sout, &slen);
    if (slen > 0) {
        size_t space = IO_TLS_CIPHER_BUF_SIZE - client->cipher_in_len;
        size_t copy = slen < space ? slen : space;
        if (copy > 0) {
            memcpy(client->cipher_in_buf + client->cipher_in_len, sout, copy);
            client->cipher_in_len += copy;
            io_tls_consume_output(server, copy);
        }
    }

    if (client->cipher_out_len > 0) {
        (void)io_tls_feed_input(server, client->cipher_out_buf, client->cipher_out_len);
        client->cipher_out_len = 0;
    }
}

static bool do_buffer_handshake(io_tls_conn_t *server, test_tls_client_t *client)
{
    bool server_done = false;
    bool client_done = false;

    for (int i = 0; i < 200; i++) {
        shuttle_data(server, client);

        if (!server_done) {
            int ret = io_tls_handshake(server);
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

/* ---- Test 5: TLS + HTTP/1.1 end-to-end via buffer API ---- */

void test_http1_tls_request_response(void)
{
    /* Set up server-side TLS context */
    io_tls_config_t cfg;
    io_tls_config_init(&cfg);
    cfg.cert_file = TEST_SERVER_CERT;
    cfg.key_file = TEST_SERVER_KEY;

    io_tls_ctx_t *sctx = io_tls_ctx_create(&cfg);
    TEST_ASSERT_NOT_NULL(sctx);

    io_tls_conn_t *sconn = io_tls_conn_create(sctx, -1);
    TEST_ASSERT_NOT_NULL(sconn);

    test_tls_client_t *client = client_create();
    TEST_ASSERT_NOT_NULL(client);

    bool ok = do_buffer_handshake(sconn, client);
    TEST_ASSERT_TRUE_MESSAGE(ok, "TLS handshake failed");

    /* Client sends HTTP request through TLS */
    const char *http_req = "GET /api/status HTTP/1.1\r\n"
                           "Host: localhost\r\n"
                           "\r\n";

    int wret = wolfSSL_write(client->ssl, http_req, (int)strlen(http_req));
    TEST_ASSERT_EQUAL_INT((int)strlen(http_req), wret);

    /* Shuttle ciphertext to server */
    shuttle_data(sconn, client);

    /* Server reads plaintext */
    uint8_t plaintext[4096];
    int rret = io_tls_read(sconn, plaintext, sizeof(plaintext));
    TEST_ASSERT_GREATER_THAN(0, rret);

    /* Parse the decrypted HTTP request */
    io_request_t req;
    int consumed = io_http1_parse_request(plaintext, (size_t)rret, &req);
    TEST_ASSERT_GREATER_THAN(0, consumed);
    TEST_ASSERT_EQUAL(IO_METHOD_GET, req.method);
    TEST_ASSERT_EQUAL_STRING_LEN("/api/status", req.path, req.path_len);

    /* Build response */
    io_response_t resp;
    int rc = io_response_init(&resp);
    TEST_ASSERT_EQUAL_INT(0, rc);

    rc = io_respond_json(&resp, 200, "{\"running\":true}");
    TEST_ASSERT_EQUAL_INT(0, rc);

    uint8_t resp_buf[4096];
    int slen = io_http1_serialize_response(&resp, resp_buf, sizeof(resp_buf));
    TEST_ASSERT_GREATER_THAN(0, slen);

    /* Server sends response through TLS */
    wret = io_tls_write(sconn, resp_buf, (size_t)slen);
    TEST_ASSERT_EQUAL_INT(slen, wret);

    shuttle_data(sconn, client);

    /* Client reads decrypted response */
    char client_buf[4096];
    rret = wolfSSL_read(client->ssl, client_buf, (int)sizeof(client_buf));
    TEST_ASSERT_GREATER_THAN(0, rret);

    /* Verify the response content */
    TEST_ASSERT_TRUE(buf_contains((const uint8_t *)client_buf, (size_t)rret, "HTTP/1.1 200 OK"));
    TEST_ASSERT_TRUE(buf_contains((const uint8_t *)client_buf, (size_t)rret, "{\"running\":true}"));

    io_response_destroy(&resp);
    client_destroy(client);
    io_tls_conn_destroy(sconn);
    io_tls_ctx_destroy(sctx);
}

/* ---- Test 6: PROXY protocol v1 + HTTP request ---- */

void test_http1_proxy_then_request(void)
{
    /* PPv1 header followed immediately by HTTP request */
    const char *ppv1 = "PROXY TCP4 192.168.1.100 10.0.0.1 12345 80\r\n";
    const char *http = "GET /behind-proxy HTTP/1.1\r\n"
                       "Host: example.com\r\n"
                       "\r\n";

    /* Concatenate into a single buffer */
    uint8_t buf[1024];
    size_t ppv1_len = strlen(ppv1);
    size_t http_len = strlen(http);
    TEST_ASSERT_TRUE(ppv1_len + http_len < sizeof(buf));

    memcpy(buf, ppv1, ppv1_len);
    memcpy(buf + ppv1_len, http, http_len);
    size_t total_len = ppv1_len + http_len;

    /* Step 1: decode PROXY protocol */
    io_proxy_result_t proxy;
    int proxy_consumed = io_proxy_decode(buf, total_len, &proxy);
    TEST_ASSERT_GREATER_THAN(0, proxy_consumed);
    TEST_ASSERT_EQUAL_UINT8(1, proxy.version);
    TEST_ASSERT_FALSE(proxy.is_local);
    TEST_ASSERT_EQUAL(AF_INET, proxy.family);

    /* Verify source address */
    struct sockaddr_in *src = (struct sockaddr_in *)&proxy.src_addr;
    char addr_str[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &src->sin_addr, addr_str, sizeof(addr_str));
    TEST_ASSERT_EQUAL_STRING("192.168.1.100", addr_str);
    TEST_ASSERT_EQUAL_UINT16(12345, ntohs(src->sin_port));

    /* Step 2: parse HTTP request from remaining bytes */
    const uint8_t *http_start = buf + proxy_consumed;
    size_t http_remaining = total_len - (size_t)proxy_consumed;

    io_request_t req;
    int http_consumed = io_http1_parse_request(http_start, http_remaining, &req);
    TEST_ASSERT_GREATER_THAN(0, http_consumed);
    TEST_ASSERT_EQUAL(IO_METHOD_GET, req.method);
    TEST_ASSERT_EQUAL_STRING_LEN("/behind-proxy", req.path, req.path_len);
    TEST_ASSERT_NOT_NULL(req.host);
    TEST_ASSERT_TRUE(req.keep_alive);
}

/* ---- Test runner ---- */

int main(void)
{
    init_cert_paths();

    UNITY_BEGIN();

    RUN_TEST(test_http1_full_request_response);
    RUN_TEST(test_http1_keepalive_multiple);
    RUN_TEST(test_http1_connection_close);
    RUN_TEST(test_http1_post_with_body);
    RUN_TEST(test_http1_tls_request_response);
    RUN_TEST(test_http1_proxy_then_request);

    return UNITY_END();
}

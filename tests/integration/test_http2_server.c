/**
 * @file test_http2_server.c
 * @brief Integration tests for ALPN-based protocol selection and HTTP/2 over TLS.
 *
 * Verifies the full path: TLS handshake with ALPN negotiation -> protocol
 * dispatch -> HTTP/2 or HTTP/1.1 request processing. Uses buffer-based TLS
 * (no sockets) with a wolfSSL client and io_tls server.
 */

#include "http/io_http1.h"
#include "http/io_http2.h"
#include "http/io_request.h"
#include "http/io_response.h"
#include "tls/io_tls.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <wolfssl/options.h>
#include <wolfssl/ssl.h>
#include <nghttp2/nghttp2.h>

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

/* ---- Protocol selection ---- */

typedef enum : uint8_t {
    IO_PROTO_HTTP11 = 0,
    IO_PROTO_H2,
} io_protocol_t;

static io_protocol_t io_protocol_from_alpn(const char *alpn)
{
    if (alpn != nullptr && strcmp(alpn, "h2") == 0) {
        return IO_PROTO_H2;
    }
    return IO_PROTO_HTTP11;
}

/* ---- Client-side TLS state (buffer-based, same pattern as test_tls_uring.c) ---- */

typedef struct {
    WOLFSSL_CTX *ctx;
    WOLFSSL *ssl;
    uint8_t cipher_in_buf[IO_TLS_CIPHER_BUF_SIZE];
    size_t cipher_in_len;
    size_t cipher_in_pos;
    uint8_t cipher_out_buf[IO_TLS_CIPHER_BUF_SIZE];
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

/* ---- Client lifecycle ---- */

static test_tls_client_t *client_create(const char *alpn_proto)
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

    /* Set ALPN if requested */
    if (alpn_proto != nullptr) {
        /* wolfSSL_UseALPN takes a mutable char* and length */
        size_t alen = strlen(alpn_proto);
        char alpn_buf[64];
        if (alen < sizeof(alpn_buf)) {
            memcpy(alpn_buf, alpn_proto, alen + 1);
            if (wolfSSL_UseALPN(client->ssl, alpn_buf, (unsigned int)alen,
                                WOLFSSL_ALPN_CONTINUE_ON_MISMATCH) != WOLFSSL_SUCCESS) {
                wolfSSL_free(client->ssl);
                wolfSSL_CTX_free(client->ctx);
                free(client);
                return nullptr;
            }
        }
    }

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

static void shuttle_data(io_tls_conn_t *server, test_tls_client_t *client)
{
    /* Server output -> client input */
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

    /* Client output -> server input */
    if (client->cipher_out_len > 0) {
        (void)io_tls_feed_input(server, client->cipher_out_buf, client->cipher_out_len);
        client->cipher_out_len = 0;
    }
}

/* ---- Handshake helper ---- */

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

/* ---- HTTP/2 callback context ---- */

typedef struct {
    io_request_t last_req;
    int32_t last_stream_id;
    int request_count;
    io_response_t response;
    bool has_response;
    char path_copy[256];
    char host_copy[128];
    uint8_t body_copy[4096];
} h2_test_ctx_t;

static io_response_t *h2_on_request_cb(const io_request_t *req, int32_t stream_id, void *user_data)
{
    h2_test_ctx_t *ctx = user_data;
    ctx->last_req = *req;
    ctx->last_stream_id = stream_id;
    ctx->request_count++;

    if (req->path != nullptr && req->path_len < sizeof(ctx->path_copy)) {
        memcpy(ctx->path_copy, req->path, req->path_len);
        ctx->path_copy[req->path_len] = '\0';
        ctx->last_req.path = ctx->path_copy;
    }
    if (req->host != nullptr) {
        size_t h_len = strlen(req->host);
        if (h_len < sizeof(ctx->host_copy)) {
            memcpy(ctx->host_copy, req->host, h_len + 1);
            ctx->last_req.host = ctx->host_copy;
        }
    }
    if (req->body != nullptr && req->body_len <= sizeof(ctx->body_copy)) {
        memcpy(ctx->body_copy, req->body, req->body_len);
        ctx->last_req.body = ctx->body_copy;
    }

    if (ctx->has_response) {
        return &ctx->response;
    }
    return nullptr;
}

/* ---- Unity setup/teardown ---- */

void setUp(void)
{
}

void tearDown(void)
{
}

/* ==== Test 1: ALPN selects h2 ==== */

void test_alpn_selects_h2(void)
{
    io_tls_config_t cfg;
    io_tls_config_init(&cfg);
    cfg.cert_file = TEST_SERVER_CERT;
    cfg.key_file = TEST_SERVER_KEY;
    cfg.alpn = "h2,http/1.1";

    io_tls_ctx_t *sctx = io_tls_ctx_create(&cfg);
    TEST_ASSERT_NOT_NULL(sctx);

    io_tls_conn_t *sconn = io_tls_conn_create(sctx, -1);
    TEST_ASSERT_NOT_NULL(sconn);

    /* Client requests h2 via ALPN */
    test_tls_client_t *client = client_create("h2");
    TEST_ASSERT_NOT_NULL(client);

    bool ok = do_buffer_handshake(sconn, client);
    TEST_ASSERT_TRUE_MESSAGE(ok, "TLS handshake with h2 ALPN failed");

    /* Verify ALPN negotiated h2 */
    const char *alpn = io_tls_get_alpn(sconn);
    TEST_ASSERT_NOT_NULL_MESSAGE(alpn, "ALPN should be negotiated after handshake");
    TEST_ASSERT_EQUAL_STRING("h2", alpn);

    /* Protocol selection should pick H2 */
    io_protocol_t proto = io_protocol_from_alpn(alpn);
    TEST_ASSERT_EQUAL_INT(IO_PROTO_H2, proto);

    client_destroy(client);
    io_tls_conn_destroy(sconn);
    io_tls_ctx_destroy(sctx);
}

/* ==== Test 2: ALPN selects http/1.1 ==== */

void test_alpn_selects_http11(void)
{
    io_tls_config_t cfg;
    io_tls_config_init(&cfg);
    cfg.cert_file = TEST_SERVER_CERT;
    cfg.key_file = TEST_SERVER_KEY;
    cfg.alpn = "h2,http/1.1";

    io_tls_ctx_t *sctx = io_tls_ctx_create(&cfg);
    TEST_ASSERT_NOT_NULL(sctx);

    io_tls_conn_t *sconn = io_tls_conn_create(sctx, -1);
    TEST_ASSERT_NOT_NULL(sconn);

    /* Client requests only http/1.1 via ALPN */
    test_tls_client_t *client = client_create("http/1.1");
    TEST_ASSERT_NOT_NULL(client);

    bool ok = do_buffer_handshake(sconn, client);
    TEST_ASSERT_TRUE_MESSAGE(ok, "TLS handshake with http/1.1 ALPN failed");

    /* Verify ALPN negotiated http/1.1 */
    const char *alpn = io_tls_get_alpn(sconn);
    TEST_ASSERT_NOT_NULL_MESSAGE(alpn, "ALPN should be negotiated after handshake");
    TEST_ASSERT_EQUAL_STRING("http/1.1", alpn);

    /* Protocol selection should pick HTTP/1.1 */
    io_protocol_t proto = io_protocol_from_alpn(alpn);
    TEST_ASSERT_EQUAL_INT(IO_PROTO_HTTP11, proto);

    client_destroy(client);
    io_tls_conn_destroy(sconn);
    io_tls_ctx_destroy(sctx);
}

/* ==== Test 3: No ALPN defaults to HTTP/1.1 ==== */

void test_alpn_default_http11(void)
{
    /* Server configured WITHOUT ALPN — simulates a server that
     * does not advertise protocols. */
    io_tls_config_t cfg;
    io_tls_config_init(&cfg);
    cfg.cert_file = TEST_SERVER_CERT;
    cfg.key_file = TEST_SERVER_KEY;
    /* cfg.alpn = nullptr (default) */

    io_tls_ctx_t *sctx = io_tls_ctx_create(&cfg);
    TEST_ASSERT_NOT_NULL(sctx);

    io_tls_conn_t *sconn = io_tls_conn_create(sctx, -1);
    TEST_ASSERT_NOT_NULL(sconn);

    /* Client also does NOT set ALPN */
    test_tls_client_t *client = client_create(nullptr);
    TEST_ASSERT_NOT_NULL(client);

    bool ok = do_buffer_handshake(sconn, client);
    TEST_ASSERT_TRUE_MESSAGE(ok, "TLS handshake without ALPN failed");

    /* No ALPN should be negotiated */
    const char *alpn = io_tls_get_alpn(sconn);
    TEST_ASSERT_NULL_MESSAGE(alpn, "ALPN should be nullptr when not negotiated");

    /* Protocol selection should default to HTTP/1.1 */
    io_protocol_t proto = io_protocol_from_alpn(alpn);
    TEST_ASSERT_EQUAL_INT(IO_PROTO_HTTP11, proto);

    client_destroy(client);
    io_tls_conn_destroy(sconn);
    io_tls_ctx_destroy(sctx);
}

/* ---- nghttp2 client helper for h2 tests ---- */

typedef struct {
    uint8_t data[65536];
    size_t len;
} ng_buf_t;

static nghttp2_ssize ng_client_send_cb(nghttp2_session *session, const uint8_t *data, size_t length,
                                       int flags, void *user_data)
{
    (void)session;
    (void)flags;
    ng_buf_t *buf = user_data;
    if (buf->len + length > sizeof(buf->data)) {
        return NGHTTP2_ERR_WOULDBLOCK;
    }
    memcpy(buf->data + buf->len, data, length);
    buf->len += length;
    return (nghttp2_ssize)length;
}

static nghttp2_session *make_ng_client(ng_buf_t *out)
{
    nghttp2_session_callbacks *cbs;
    nghttp2_session_callbacks_new(&cbs);
    nghttp2_session_callbacks_set_send_callback2(cbs, ng_client_send_cb);

    nghttp2_session *ng_client;
    nghttp2_session_client_new(&ng_client, cbs, out);
    nghttp2_session_callbacks_del(cbs);
    return ng_client;
}

static void ng_client_send_preface(nghttp2_session *ng_client, ng_buf_t *out)
{
    out->len = 0;
    nghttp2_settings_entry iv[] = {
        {NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS, 100},
        {NGHTTP2_SETTINGS_INITIAL_WINDOW_SIZE, 65535},
    };
    nghttp2_submit_settings(ng_client, NGHTTP2_FLAG_NONE, iv, 2);
    nghttp2_session_send(ng_client);
}

static int32_t ng_client_submit_get(nghttp2_session *ng_client, const char *path)
{
    nghttp2_nv nva[] = {
        {(uint8_t *)":method", (uint8_t *)"GET", 7, 3, NGHTTP2_NV_FLAG_NONE},
        {(uint8_t *)":path", (uint8_t *)path, 5, strlen(path), NGHTTP2_NV_FLAG_NONE},
        {(uint8_t *)":scheme", (uint8_t *)"https", 7, 5, NGHTTP2_NV_FLAG_NONE},
        {(uint8_t *)":authority", (uint8_t *)"localhost", 10, 9, NGHTTP2_NV_FLAG_NONE},
    };
    return nghttp2_submit_request2(ng_client, nullptr, nva, 4, nullptr, nullptr);
}

/**
 * Full round-trip for nghttp2 client <-> io_http2 server, through TLS.
 *
 * 1. nghttp2 client produces HTTP/2 frames into ng_out
 * 2. wolfSSL client encrypts ng_out -> shuttle to server
 * 3. io_tls server decrypts -> io_http2_on_recv
 * 4. io_http2_flush -> io_tls server encrypts -> shuttle to client
 * 5. wolfSSL client decrypts -> nghttp2 client consumes response frames
 */
static int h2_tls_pump(io_http2_session_t *h2_server, io_tls_conn_t *sconn,
                       test_tls_client_t *client, nghttp2_session *ng_client, ng_buf_t *ng_out)
{
    /* Step 1: nghttp2 client -> TLS client -> TLS server -> HTTP/2 server */
    if (ng_out->len > 0) {
        /* Encrypt with wolfSSL client */
        size_t total_sent = 0;
        while (total_sent < ng_out->len) {
            int wret = wolfSSL_write(client->ssl, ng_out->data + total_sent,
                                     (int)(ng_out->len - total_sent));
            if (wret <= 0) {
                int err = wolfSSL_get_error(client->ssl, wret);
                if (err == WOLFSSL_ERROR_WANT_WRITE) {
                    shuttle_data(sconn, client);
                    continue;
                }
                return -EIO;
            }
            total_sent += (size_t)wret;
        }
        ng_out->len = 0;

        /* Shuttle ciphertext to server */
        shuttle_data(sconn, client);

        /* Server decrypts and feeds to HTTP/2 */
        uint8_t plain_buf[16384];
        for (;;) {
            int rret = io_tls_read(sconn, plain_buf, sizeof(plain_buf));
            if (rret <= 0) {
                break;
            }
            ssize_t consumed = io_http2_on_recv(h2_server, plain_buf, (size_t)rret);
            if (consumed < 0) {
                return (int)consumed;
            }
        }
    }

    /* Step 2: HTTP/2 server -> TLS server -> TLS client -> nghttp2 client */
    const uint8_t *h2_out;
    size_t h2_out_len;
    int rv = io_http2_flush(h2_server, &h2_out, &h2_out_len);
    if (rv != 0) {
        return rv;
    }

    if (h2_out_len > 0) {
        /* Server encrypts with TLS */
        size_t total_written = 0;
        while (total_written < h2_out_len) {
            int wret = io_tls_write(sconn, h2_out + total_written, h2_out_len - total_written);
            if (wret == -EAGAIN) {
                shuttle_data(sconn, client);
                continue;
            }
            if (wret <= 0) {
                return wret == 0 ? -EIO : wret;
            }
            total_written += (size_t)wret;
        }

        /* Shuttle to client */
        shuttle_data(sconn, client);

        /* Client decrypts and feeds to nghttp2 */
        uint8_t resp_buf[16384];
        for (;;) {
            int rret = wolfSSL_read(client->ssl, resp_buf, (int)sizeof(resp_buf));
            if (rret <= 0) {
                break;
            }
            nghttp2_ssize n = nghttp2_session_mem_recv2(ng_client, resp_buf, (size_t)rret);
            if (n < 0) {
                return -EIO;
            }
        }
    }

    return 0;
}

/* ==== Test 4: End-to-end HTTP/2 request via TLS ==== */

void test_http2_full_request_via_tls(void)
{
    /* Server: TLS with ALPN h2 + HTTP/2 session */
    io_tls_config_t cfg;
    io_tls_config_init(&cfg);
    cfg.cert_file = TEST_SERVER_CERT;
    cfg.key_file = TEST_SERVER_KEY;
    cfg.alpn = "h2,http/1.1";

    io_tls_ctx_t *sctx = io_tls_ctx_create(&cfg);
    TEST_ASSERT_NOT_NULL(sctx);

    io_tls_conn_t *sconn = io_tls_conn_create(sctx, -1);
    TEST_ASSERT_NOT_NULL(sconn);

    /* Client: TLS with h2 ALPN */
    test_tls_client_t *client = client_create("h2");
    TEST_ASSERT_NOT_NULL(client);

    bool ok = do_buffer_handshake(sconn, client);
    TEST_ASSERT_TRUE_MESSAGE(ok, "TLS handshake failed");

    /* Verify ALPN */
    const char *alpn = io_tls_get_alpn(sconn);
    TEST_ASSERT_NOT_NULL(alpn);
    TEST_ASSERT_EQUAL_STRING("h2", alpn);

    /* Create HTTP/2 server session with a response callback */
    h2_test_ctx_t h2ctx = {.request_count = 0, .has_response = true};
    TEST_ASSERT_EQUAL_INT(0, io_response_init(&h2ctx.response));
    const char *body = "Hello, HTTP/2!";
    TEST_ASSERT_EQUAL_INT(0, io_respond(&h2ctx.response, 200, "text/plain", (const uint8_t *)body,
                                        strlen(body)));

    io_http2_session_t *h2_server = io_http2_session_create(nullptr, h2_on_request_cb, &h2ctx);
    TEST_ASSERT_NOT_NULL(h2_server);

    /* nghttp2 client session */
    ng_buf_t ng_out = {.len = 0};
    nghttp2_session *ng_client = make_ng_client(&ng_out);
    TEST_ASSERT_NOT_NULL(ng_client);

    /* Client sends connection preface */
    ng_client_send_preface(ng_client, &ng_out);

    /* Pump: client preface -> TLS -> server */
    int rv = h2_tls_pump(h2_server, sconn, client, ng_client, &ng_out);
    TEST_ASSERT_EQUAL_INT(0, rv);

    /* Pump again to process SETTINGS ACK */
    ng_out.len = 0;
    nghttp2_session_send(ng_client);
    if (ng_out.len > 0) {
        rv = h2_tls_pump(h2_server, sconn, client, ng_client, &ng_out);
        TEST_ASSERT_EQUAL_INT(0, rv);
    }

    /* Client submits GET /hello */
    ng_out.len = 0;
    int32_t stream_id = ng_client_submit_get(ng_client, "/hello");
    TEST_ASSERT_GREATER_THAN(0, stream_id);
    nghttp2_session_send(ng_client);

    /* Pump: request -> TLS -> server -> response -> TLS -> client */
    rv = h2_tls_pump(h2_server, sconn, client, ng_client, &ng_out);
    TEST_ASSERT_EQUAL_INT(0, rv);

    /* Verify the server received the request */
    TEST_ASSERT_EQUAL_INT(1, h2ctx.request_count);
    TEST_ASSERT_EQUAL_INT(IO_METHOD_GET, h2ctx.last_req.method);
    TEST_ASSERT_EQUAL_INT(6, (int)h2ctx.last_req.path_len);
    TEST_ASSERT_EQUAL_INT(0, memcmp(h2ctx.last_req.path, "/hello", 6));
    TEST_ASSERT_EQUAL_UINT8(2, h2ctx.last_req.http_version_major);

    io_response_destroy(&h2ctx.response);
    nghttp2_session_del(ng_client);
    io_http2_session_destroy(h2_server);
    client_destroy(client);
    io_tls_conn_destroy(sconn);
    io_tls_ctx_destroy(sctx);
}

/* ==== Test 5: Multiple HTTP/2 streams via TLS ==== */

void test_http2_multiple_streams_via_tls(void)
{
    io_tls_config_t cfg;
    io_tls_config_init(&cfg);
    cfg.cert_file = TEST_SERVER_CERT;
    cfg.key_file = TEST_SERVER_KEY;
    cfg.alpn = "h2,http/1.1";

    io_tls_ctx_t *sctx = io_tls_ctx_create(&cfg);
    TEST_ASSERT_NOT_NULL(sctx);

    io_tls_conn_t *sconn = io_tls_conn_create(sctx, -1);
    TEST_ASSERT_NOT_NULL(sconn);

    test_tls_client_t *client = client_create("h2");
    TEST_ASSERT_NOT_NULL(client);

    bool ok = do_buffer_handshake(sconn, client);
    TEST_ASSERT_TRUE(ok);

    TEST_ASSERT_EQUAL_STRING("h2", io_tls_get_alpn(sconn));

    /* HTTP/2 session — single shared response is safe here because the body
     * is immutable after init and nghttp2 reads it via data provider. Each
     * stream gets its own h2_resp_data_t internally. */
    h2_test_ctx_t h2ctx = {.request_count = 0, .has_response = true};
    TEST_ASSERT_EQUAL_INT(0, io_response_init(&h2ctx.response));
    TEST_ASSERT_EQUAL_INT(0,
                          io_respond(&h2ctx.response, 200, "text/plain", (const uint8_t *)"OK", 2));

    io_http2_session_t *h2_server = io_http2_session_create(nullptr, h2_on_request_cb, &h2ctx);
    TEST_ASSERT_NOT_NULL(h2_server);

    ng_buf_t ng_out = {.len = 0};
    nghttp2_session *ng_client = make_ng_client(&ng_out);
    TEST_ASSERT_NOT_NULL(ng_client);

    /* Exchange connection preface */
    ng_client_send_preface(ng_client, &ng_out);
    int rv = h2_tls_pump(h2_server, sconn, client, ng_client, &ng_out);
    TEST_ASSERT_EQUAL_INT(0, rv);

    /* SETTINGS ACK */
    ng_out.len = 0;
    nghttp2_session_send(ng_client);
    if (ng_out.len > 0) {
        rv = h2_tls_pump(h2_server, sconn, client, ng_client, &ng_out);
        TEST_ASSERT_EQUAL_INT(0, rv);
    }

    /* Submit 3 concurrent GET requests */
    ng_out.len = 0;
    int32_t s1 = ng_client_submit_get(ng_client, "/one");
    int32_t s2 = ng_client_submit_get(ng_client, "/two");
    int32_t s3 = ng_client_submit_get(ng_client, "/three");
    TEST_ASSERT_GREATER_THAN(0, s1);
    TEST_ASSERT_GREATER_THAN(0, s2);
    TEST_ASSERT_GREATER_THAN(0, s3);
    TEST_ASSERT_NOT_EQUAL(s1, s2);
    TEST_ASSERT_NOT_EQUAL(s2, s3);
    nghttp2_session_send(ng_client);

    /* Pump all 3 through TLS */
    rv = h2_tls_pump(h2_server, sconn, client, ng_client, &ng_out);
    TEST_ASSERT_EQUAL_INT(0, rv);

    /* All 3 requests should have been received */
    TEST_ASSERT_EQUAL_INT(3, h2ctx.request_count);

    io_response_destroy(&h2ctx.response);
    nghttp2_session_del(ng_client);
    io_http2_session_destroy(h2_server);
    client_destroy(client);
    io_tls_conn_destroy(sconn);
    io_tls_ctx_destroy(sctx);
}

/* ==== Test 6: Server push (skipped — phased out by browsers) ==== */

void test_http2_server_push(void)
{
    /* HTTP/2 server push (RFC 9113 §8.4) is being phased out.
     * Chrome removed support in 2022, Firefox disabled by default.
     * Most HTTP/2 implementations now ignore PUSH_PROMISE frames.
     * Marking this test as ignored per industry consensus. */
    TEST_IGNORE_MESSAGE("HTTP/2 server push phased out by browsers — skipped");
}

/* ---- Test runner ---- */

int main(void)
{
    init_cert_paths();

    UNITY_BEGIN();

    RUN_TEST(test_alpn_selects_h2);
    RUN_TEST(test_alpn_selects_http11);
    RUN_TEST(test_alpn_default_http11);
    RUN_TEST(test_http2_full_request_via_tls);
    RUN_TEST(test_http2_multiple_streams_via_tls);
    RUN_TEST(test_http2_server_push);

    return UNITY_END();
}

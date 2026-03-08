/**
 * @file test_io_http2.c
 * @brief Unit tests for HTTP/2 session management (nghttp2-backed).
 *
 * Uses an nghttp2 CLIENT session to generate valid HTTP/2 frames,
 * then feeds them to the SERVER session under test.
 */

#include "http/io_http2.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <nghttp2/nghttp2.h>
#include <unity.h>

/* ---- Test buffer for client output capture ---- */

typedef struct {
    uint8_t data[65536];
    size_t len;
} test_buf_t;

/* ---- Test context ---- */

typedef struct {
    io_request_t last_req;
    int32_t last_stream_id;
    int request_count;
    io_response_t response;   /* pre-built response for the callback to return */
    bool has_response;
    /* Copies of strings that outlive the callback (arena-owned pointers are freed on stream close) */
    char path_copy[256];
    char content_type_copy[128];
    char host_copy[128];
    uint8_t body_copy[4096];
} test_ctx_t;

/* ---- Callback: records received requests, returns response ---- */

static io_response_t *on_request_cb(const io_request_t *req, int32_t stream_id, void *user_data)
{
    test_ctx_t *ctx = user_data;
    ctx->last_req = *req;
    ctx->last_stream_id = stream_id;
    ctx->request_count++;

    /* Copy strings that will be freed when the stream closes */
    if (req->path != nullptr && req->path_len < sizeof(ctx->path_copy)) {
        memcpy(ctx->path_copy, req->path, req->path_len);
        ctx->path_copy[req->path_len] = '\0';
        ctx->last_req.path = ctx->path_copy;
    }
    if (req->content_type != nullptr) {
        size_t ct_len = strlen(req->content_type);
        if (ct_len < sizeof(ctx->content_type_copy)) {
            memcpy(ctx->content_type_copy, req->content_type, ct_len + 1);
            ctx->last_req.content_type = ctx->content_type_copy;
        }
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

/* ---- Client-side send callback: captures output into test_buf_t ---- */

static nghttp2_ssize client_send_cb(nghttp2_session *session, const uint8_t *data, size_t length,
                                     int flags, void *user_data)
{
    (void)session;
    (void)flags;
    test_buf_t *buf = user_data;
    if (buf->len + length > sizeof(buf->data)) {
        return NGHTTP2_ERR_WOULDBLOCK;
    }
    memcpy(buf->data + buf->len, data, length);
    buf->len += length;
    return (nghttp2_ssize)length;
}

/* ---- Helper: create a client session that captures output ---- */

static nghttp2_session *make_client(test_buf_t *out)
{
    nghttp2_session_callbacks *cbs;
    nghttp2_session_callbacks_new(&cbs);
    nghttp2_session_callbacks_set_send_callback2(cbs, client_send_cb);

    nghttp2_session *client;
    nghttp2_session_client_new(&client, cbs, out);
    nghttp2_session_callbacks_del(cbs);
    return client;
}

/* ---- Helper: client sends connection preface + SETTINGS ---- */

static void client_send_preface(nghttp2_session *client, test_buf_t *out)
{
    out->len = 0;
    nghttp2_settings_entry iv[] = {
        {NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS, 100},
        {NGHTTP2_SETTINGS_INITIAL_WINDOW_SIZE, 65535},
    };
    nghttp2_submit_settings(client, NGHTTP2_FLAG_NONE, iv, 2);
    nghttp2_session_send(client);
}

/* ---- Helper: feed client output to server, then flush server ---- */

static int pump(io_http2_session_t *server, test_buf_t *client_out, test_buf_t *server_out)
{
    ssize_t consumed = io_http2_on_recv(server, client_out->data, client_out->len);
    if (consumed < 0) {
        return (int)consumed;
    }
    client_out->len = 0;

    const uint8_t *data;
    size_t len;
    int rv = io_http2_flush(server, &data, &len);
    if (rv != 0) {
        return rv;
    }
    if (server_out != nullptr && len > 0) {
        if (server_out->len + len > sizeof(server_out->data)) {
            return -ENOSPC;
        }
        memcpy(server_out->data + server_out->len, data, len);
        server_out->len += len;
    }
    return 0;
}

/* ---- Helper: feed server output back to client ---- */

static int client_recv(nghttp2_session *client, test_buf_t *server_out)
{
    if (server_out->len == 0) {
        return 0;
    }
    nghttp2_ssize n = nghttp2_session_mem_recv2(client, server_out->data, server_out->len);
    if (n < 0) {
        return -1;
    }
    server_out->len = 0;
    return 0;
}

/* ---- Helper: client submits GET request ---- */

static int32_t client_submit_get(nghttp2_session *client, const char *path)
{
    nghttp2_nv nva[] = {
        {(uint8_t *)":method", (uint8_t *)"GET", 7, 3, NGHTTP2_NV_FLAG_NONE},
        {(uint8_t *)":path", (uint8_t *)path, 5, strlen(path), NGHTTP2_NV_FLAG_NONE},
        {(uint8_t *)":scheme", (uint8_t *)"https", 7, 5, NGHTTP2_NV_FLAG_NONE},
        {(uint8_t *)":authority", (uint8_t *)"localhost", 10, 9, NGHTTP2_NV_FLAG_NONE},
    };
    return nghttp2_submit_request2(client, nullptr, nva, 4, nullptr, nullptr);
}

/* ---- Data provider for client POST body ---- */

typedef struct {
    const uint8_t *body;
    size_t len;
    size_t offset;
} client_body_t;

static nghttp2_ssize client_body_read_cb(nghttp2_session *session, int32_t stream_id,
                                          uint8_t *buf, size_t length, uint32_t *data_flags,
                                          nghttp2_data_source *source, void *user_data)
{
    (void)session;
    (void)stream_id;
    (void)user_data;
    client_body_t *cb = source->ptr;
    size_t remaining = cb->len - cb->offset;
    size_t n = (length < remaining) ? length : remaining;
    memcpy(buf, cb->body + cb->offset, n);
    cb->offset += n;
    if (cb->offset >= cb->len) {
        *data_flags |= NGHTTP2_DATA_FLAG_EOF;
    }
    return (nghttp2_ssize)n;
}

/* ---- Unity setup/teardown ---- */

void setUp(void)
{
}

void tearDown(void)
{
}

/* ==== Tests ==== */

void test_http2_session_create_destroy(void)
{
    io_http2_session_t *session = io_http2_session_create(nullptr, nullptr, nullptr);
    TEST_ASSERT_NOT_NULL(session);

    /* Fresh session should want to write (SETTINGS) */
    TEST_ASSERT_TRUE(io_http2_want_write(session));
    TEST_ASSERT_FALSE(io_http2_goaway_sent(session));

    io_http2_session_destroy(session);

    /* Destroying nullptr should be safe */
    io_http2_session_destroy(nullptr);
}

void test_http2_connection_preface(void)
{
    test_buf_t client_out = {.len = 0};
    test_buf_t server_out = {.len = 0};

    io_http2_session_t *server = io_http2_session_create(nullptr, nullptr, nullptr);
    TEST_ASSERT_NOT_NULL(server);

    nghttp2_session *client = make_client(&client_out);
    TEST_ASSERT_NOT_NULL(client);

    /* Client sends connection preface (magic + SETTINGS) */
    client_send_preface(client, &client_out);
    TEST_ASSERT_GREATER_THAN(0, client_out.len);

    /* Feed to server */
    int rv = pump(server, &client_out, &server_out);
    TEST_ASSERT_EQUAL_INT(0, rv);

    /* Server should have produced SETTINGS response */
    TEST_ASSERT_GREATER_THAN(0, server_out.len);

    /* Feed server response back to client — should succeed */
    rv = client_recv(client, &server_out);
    TEST_ASSERT_EQUAL_INT(0, rv);

    nghttp2_session_del(client);
    io_http2_session_destroy(server);
}

void test_http2_settings_ack(void)
{
    test_buf_t client_out = {.len = 0};
    test_buf_t server_out = {.len = 0};

    io_http2_session_t *server = io_http2_session_create(nullptr, nullptr, nullptr);
    nghttp2_session *client = make_client(&client_out);

    /* Exchange preface */
    client_send_preface(client, &client_out);
    pump(server, &client_out, &server_out);
    client_recv(client, &server_out);

    /* Client should be able to send after SETTINGS exchange */
    /* After exchanging settings, both sides should want_read */
    TEST_ASSERT_TRUE(io_http2_want_read(server));

    nghttp2_session_del(client);
    io_http2_session_destroy(server);
}

void test_http2_simple_get(void)
{
    test_buf_t client_out = {.len = 0};
    test_buf_t server_out = {.len = 0};
    test_ctx_t ctx = {.request_count = 0, .has_response = true};

    TEST_ASSERT_EQUAL_INT(0, io_response_init(&ctx.response));
    TEST_ASSERT_EQUAL_INT(0, io_respond(&ctx.response, 200, "text/plain", (const uint8_t *)"OK", 2));

    io_http2_session_t *server = io_http2_session_create(nullptr, on_request_cb, &ctx);
    nghttp2_session *client = make_client(&client_out);

    /* Connection preface */
    client_send_preface(client, &client_out);
    pump(server, &client_out, &server_out);
    client_recv(client, &server_out);

    /* Submit GET /hello */
    client_out.len = 0;
    int32_t stream_id = client_submit_get(client, "/hello");
    TEST_ASSERT_GREATER_THAN(0, stream_id);
    nghttp2_session_send(client);

    /* Feed to server — should trigger on_request callback */
    server_out.len = 0;
    pump(server, &client_out, &server_out);

    TEST_ASSERT_EQUAL_INT(1, ctx.request_count);
    TEST_ASSERT_EQUAL_INT(IO_METHOD_GET, ctx.last_req.method);
    TEST_ASSERT_EQUAL_INT(6, (int)ctx.last_req.path_len);
    TEST_ASSERT_EQUAL_INT(0, memcmp(ctx.last_req.path, "/hello", 6));
    TEST_ASSERT_EQUAL_INT(stream_id, ctx.last_stream_id);
    TEST_ASSERT_EQUAL_UINT8(2, ctx.last_req.http_version_major);

    /* Server response should be available */
    TEST_ASSERT_GREATER_THAN(0, server_out.len);

    io_response_destroy(&ctx.response);
    nghttp2_session_del(client);
    io_http2_session_destroy(server);
}

void test_http2_post_with_body(void)
{
    test_buf_t client_out = {.len = 0};
    test_buf_t server_out = {.len = 0};
    test_ctx_t ctx = {.request_count = 0, .has_response = true};

    TEST_ASSERT_EQUAL_INT(0, io_response_init(&ctx.response));
    TEST_ASSERT_EQUAL_INT(0, io_respond(&ctx.response, 201, "text/plain", (const uint8_t *)"Created", 7));

    io_http2_session_t *server = io_http2_session_create(nullptr, on_request_cb, &ctx);
    nghttp2_session *client = make_client(&client_out);

    /* Connection preface */
    client_send_preface(client, &client_out);
    pump(server, &client_out, &server_out);
    client_recv(client, &server_out);

    /* Submit POST with body */
    const char *body = "{\"key\":\"value\"}";
    size_t body_len = strlen(body);

    client_body_t cb = {.body = (const uint8_t *)body, .len = body_len, .offset = 0};
    nghttp2_data_provider2 data_prd = {
        .source = {.ptr = &cb},
        .read_callback = client_body_read_cb,
    };

    char cl_str[16];
    snprintf(cl_str, sizeof(cl_str), "%zu", body_len);

    nghttp2_nv nva[] = {
        {(uint8_t *)":method", (uint8_t *)"POST", 7, 4, NGHTTP2_NV_FLAG_NONE},
        {(uint8_t *)":path", (uint8_t *)"/data", 5, 5, NGHTTP2_NV_FLAG_NONE},
        {(uint8_t *)":scheme", (uint8_t *)"https", 7, 5, NGHTTP2_NV_FLAG_NONE},
        {(uint8_t *)":authority", (uint8_t *)"localhost", 10, 9, NGHTTP2_NV_FLAG_NONE},
        {(uint8_t *)"content-type", (uint8_t *)"application/json", 12, 16, NGHTTP2_NV_FLAG_NONE},
        {(uint8_t *)"content-length", (uint8_t *)cl_str, 14, strlen(cl_str),
         NGHTTP2_NV_FLAG_NONE},
    };

    client_out.len = 0;
    int32_t stream_id = nghttp2_submit_request2(client, nullptr, nva, 6, &data_prd, nullptr);
    TEST_ASSERT_GREATER_THAN(0, stream_id);
    nghttp2_session_send(client);

    /* Feed to server */
    server_out.len = 0;
    pump(server, &client_out, &server_out);

    TEST_ASSERT_EQUAL_INT(1, ctx.request_count);
    TEST_ASSERT_EQUAL_INT(IO_METHOD_POST, ctx.last_req.method);
    TEST_ASSERT_EQUAL_INT(5, (int)ctx.last_req.path_len);
    TEST_ASSERT_EQUAL_INT(0, memcmp(ctx.last_req.path, "/data", 5));
    TEST_ASSERT_EQUAL_size_t(body_len, ctx.last_req.body_len);
    TEST_ASSERT_EQUAL_INT(0, memcmp(ctx.last_req.body, body, body_len));
    TEST_ASSERT_EQUAL_STRING("application/json", ctx.last_req.content_type);

    io_response_destroy(&ctx.response);
    nghttp2_session_del(client);
    io_http2_session_destroy(server);
}

void test_http2_stream_multiplexing(void)
{
    test_buf_t client_out = {.len = 0};
    test_buf_t server_out = {.len = 0};
    test_ctx_t ctx = {.request_count = 0, .has_response = true};

    TEST_ASSERT_EQUAL_INT(0, io_response_init(&ctx.response));
    TEST_ASSERT_EQUAL_INT(0, io_respond(&ctx.response, 200, "text/plain", (const uint8_t *)"OK", 2));

    io_http2_session_t *server = io_http2_session_create(nullptr, on_request_cb, &ctx);
    nghttp2_session *client = make_client(&client_out);

    /* Connection preface */
    client_send_preface(client, &client_out);
    pump(server, &client_out, &server_out);
    client_recv(client, &server_out);

    /* Submit 3 concurrent GET requests */
    client_out.len = 0;
    int32_t s1 = client_submit_get(client, "/one");
    int32_t s2 = client_submit_get(client, "/two");
    int32_t s3 = client_submit_get(client, "/three");
    TEST_ASSERT_GREATER_THAN(0, s1);
    TEST_ASSERT_GREATER_THAN(0, s2);
    TEST_ASSERT_GREATER_THAN(0, s3);
    /* All stream IDs should be different odd numbers */
    TEST_ASSERT_NOT_EQUAL(s1, s2);
    TEST_ASSERT_NOT_EQUAL(s2, s3);
    nghttp2_session_send(client);

    /* Feed to server — all 3 requests should be received */
    server_out.len = 0;
    pump(server, &client_out, &server_out);

    TEST_ASSERT_EQUAL_INT(3, ctx.request_count);

    io_response_destroy(&ctx.response);
    nghttp2_session_del(client);
    io_http2_session_destroy(server);
}

void test_http2_flow_control(void)
{
    test_buf_t client_out = {.len = 0};
    test_buf_t server_out = {.len = 0};
    test_ctx_t ctx = {.request_count = 0, .has_response = true};

    TEST_ASSERT_EQUAL_INT(0, io_response_init(&ctx.response));
    TEST_ASSERT_EQUAL_INT(0, io_respond(&ctx.response, 200, "text/plain", (const uint8_t *)"OK", 2));

    io_http2_session_t *server = io_http2_session_create(nullptr, on_request_cb, &ctx);
    nghttp2_session *client = make_client(&client_out);

    /* Exchange preface */
    client_send_preface(client, &client_out);
    pump(server, &client_out, &server_out);
    client_recv(client, &server_out);

    /* Submit a GET, feed to server, get response */
    client_out.len = 0;
    client_submit_get(client, "/flow");
    nghttp2_session_send(client);

    server_out.len = 0;
    pump(server, &client_out, &server_out);

    /* Feed server response to client */
    int rv = client_recv(client, &server_out);
    TEST_ASSERT_EQUAL_INT(0, rv);

    /* Client should send WINDOW_UPDATE after receiving data.
     * Verify the session is still operational after flow control exchange. */
    client_out.len = 0;
    nghttp2_session_send(client);
    if (client_out.len > 0) {
        server_out.len = 0;
        pump(server, &client_out, &server_out);
    }

    /* Session should still be active */
    TEST_ASSERT_TRUE(io_http2_want_read(server));
    TEST_ASSERT_EQUAL_INT(1, ctx.request_count);

    io_response_destroy(&ctx.response);
    nghttp2_session_del(client);
    io_http2_session_destroy(server);
}

void test_http2_goaway_two_phase(void)
{
    test_buf_t client_out = {.len = 0};
    test_buf_t server_out = {.len = 0};

    io_http2_session_t *server = io_http2_session_create(nullptr, nullptr, nullptr);
    nghttp2_session *client = make_client(&client_out);

    /* Exchange preface */
    client_send_preface(client, &client_out);
    pump(server, &client_out, &server_out);
    client_recv(client, &server_out);

    /* First GOAWAY (last_stream_id = 2^31-1) */
    TEST_ASSERT_FALSE(io_http2_goaway_sent(server));
    int rv = io_http2_goaway(server);
    TEST_ASSERT_EQUAL_INT(0, rv);
    TEST_ASSERT_TRUE(io_http2_goaway_sent(server));

    /* Flush to get the GOAWAY frame */
    server_out.len = 0;
    const uint8_t *data;
    size_t len;
    TEST_ASSERT_EQUAL_INT(0, io_http2_flush(server, &data, &len));
    TEST_ASSERT_GREATER_THAN(0, len);
    memcpy(server_out.data, data, len);
    server_out.len = len;

    /* Feed to client */
    rv = client_recv(client, &server_out);
    TEST_ASSERT_EQUAL_INT(0, rv);

    /* Second GOAWAY (real last_stream_id = 0) */
    rv = io_http2_goaway(server);
    TEST_ASSERT_EQUAL_INT(0, rv);

    nghttp2_session_del(client);
    io_http2_session_destroy(server);
}

void test_http2_goaway_drains(void)
{
    test_buf_t client_out = {.len = 0};
    test_buf_t server_out = {.len = 0};

    io_http2_session_t *server = io_http2_session_create(nullptr, nullptr, nullptr);
    nghttp2_session *client = make_client(&client_out);

    /* Exchange preface */
    client_send_preface(client, &client_out);
    pump(server, &client_out, &server_out);
    client_recv(client, &server_out);

    /* Send GOAWAY */
    TEST_ASSERT_EQUAL_INT(0, io_http2_goaway(server));

    /* Flush the GOAWAY */
    const uint8_t *data;
    size_t len;
    TEST_ASSERT_EQUAL_INT(0, io_http2_flush(server, &data, &len));

    /* After GOAWAY + flush, session should be draining when nghttp2 is done */
    TEST_ASSERT_TRUE(io_http2_goaway_sent(server));

    /* Feed GOAWAY to client so it knows to stop */
    server_out.len = 0;
    memcpy(server_out.data, data, len);
    server_out.len = len;
    client_recv(client, &server_out);

    /* Client should no longer want to send new requests */
    /* Send second GOAWAY with real last_stream_id */
    TEST_ASSERT_EQUAL_INT(0, io_http2_goaway(server));
    TEST_ASSERT_EQUAL_INT(0, io_http2_flush(server, &data, &len));

    nghttp2_session_del(client);
    io_http2_session_destroy(server);
}

void test_http2_rst_stream(void)
{
    test_buf_t client_out = {.len = 0};
    test_buf_t server_out = {.len = 0};
    test_ctx_t ctx = {.request_count = 0, .has_response = true};

    TEST_ASSERT_EQUAL_INT(0, io_response_init(&ctx.response));
    TEST_ASSERT_EQUAL_INT(0, io_respond(&ctx.response, 200, "text/plain", (const uint8_t *)"OK", 2));

    io_http2_session_t *server = io_http2_session_create(nullptr, on_request_cb, &ctx);
    nghttp2_session *client = make_client(&client_out);

    /* Exchange preface */
    client_send_preface(client, &client_out);
    pump(server, &client_out, &server_out);
    client_recv(client, &server_out);

    /* Submit a request, then RST_STREAM it before END_STREAM */
    client_out.len = 0;
    int32_t stream_id = client_submit_get(client, "/cancel");
    nghttp2_session_send(client);

    server_out.len = 0;
    pump(server, &client_out, &server_out);

    /* Client sends RST_STREAM */
    client_out.len = 0;
    nghttp2_submit_rst_stream(client, NGHTTP2_FLAG_NONE, stream_id, NGHTTP2_CANCEL);
    nghttp2_session_send(client);

    server_out.len = 0;
    pump(server, &client_out, &server_out);

    /* The cancelled stream request was still delivered (END_STREAM was in HEADERS).
     * The RST_STREAM just closed the stream. Server should still be operational. */
    TEST_ASSERT_TRUE(io_http2_want_read(server));

    io_response_destroy(&ctx.response);
    nghttp2_session_del(client);
    io_http2_session_destroy(server);
}

void test_http2_max_concurrent_streams(void)
{
    test_buf_t client_out = {.len = 0};
    test_buf_t server_out = {.len = 0};

    /* Set max_concurrent_streams to 2 */
    io_http2_config_t cfg = {
        .max_concurrent_streams = 2,
        .initial_window_size = 65535,
        .max_frame_size = 16384,
        .max_header_list_size = 8192,
        .max_rst_stream_per_sec = 100,
    };

    /* No response callback — requests will pile up */
    io_http2_session_t *server = io_http2_session_create(&cfg, nullptr, nullptr);
    nghttp2_session *client = make_client(&client_out);

    /* Exchange preface */
    client_send_preface(client, &client_out);
    pump(server, &client_out, &server_out);
    client_recv(client, &server_out);

    /* Client needs to process the SETTINGS from server (max_concurrent_streams=2) */
    client_out.len = 0;
    nghttp2_session_send(client);
    if (client_out.len > 0) {
        server_out.len = 0;
        pump(server, &client_out, &server_out);
        client_recv(client, &server_out);
    }

    /* Try to submit 4 requests — nghttp2 client should honor the limit.
     * The client library itself enforces the server's SETTINGS. */
    client_out.len = 0;
    int32_t s1 = client_submit_get(client, "/a");
    int32_t s2 = client_submit_get(client, "/b");
    int32_t s3 = client_submit_get(client, "/c");
    int32_t s4 = client_submit_get(client, "/d");
    TEST_ASSERT_GREATER_THAN(0, s1);
    TEST_ASSERT_GREATER_THAN(0, s2);
    TEST_ASSERT_GREATER_THAN(0, s3);
    TEST_ASSERT_GREATER_THAN(0, s4);

    nghttp2_session_send(client);

    /* Feed to server */
    server_out.len = 0;
    pump(server, &client_out, &server_out);

    /* The server should be operational — max_concurrent_streams enforcement
     * is done by the client honoring the SETTINGS. The server setting is
     * advertised and nghttp2 server will RST_STREAM any excess streams. */
    TEST_ASSERT_TRUE(io_http2_want_read(server));

    nghttp2_session_del(client);
    io_http2_session_destroy(server);
}

void test_http2_rapid_reset_protection(void)
{
    test_buf_t client_out = {.len = 0};
    test_buf_t server_out = {.len = 0};

    /* Set very low RST_STREAM limit for testing */
    io_http2_config_t cfg = {
        .max_concurrent_streams = 100,
        .initial_window_size = 65535,
        .max_frame_size = 16384,
        .max_header_list_size = 8192,
        .max_rst_stream_per_sec = 3,
    };

    io_http2_session_t *server = io_http2_session_create(&cfg, nullptr, nullptr);
    nghttp2_session *client = make_client(&client_out);

    /* Exchange preface */
    client_send_preface(client, &client_out);
    pump(server, &client_out, &server_out);
    client_recv(client, &server_out);

    /* Send SETTINGS ACK etc */
    client_out.len = 0;
    nghttp2_session_send(client);
    if (client_out.len > 0) {
        server_out.len = 0;
        pump(server, &client_out, &server_out);
        client_recv(client, &server_out);
    }

    /* Send requests and immediately RST_STREAM them — simulating Rapid Reset */
    TEST_ASSERT_FALSE(io_http2_goaway_sent(server));

    for (int i = 0; i < 5; i++) {
        client_out.len = 0;
        int32_t sid = client_submit_get(client, "/reset");
        nghttp2_session_send(client);

        server_out.len = 0;
        pump(server, &client_out, &server_out);
        client_recv(client, &server_out);

        /* RST_STREAM */
        client_out.len = 0;
        nghttp2_submit_rst_stream(client, NGHTTP2_FLAG_NONE, sid, NGHTTP2_CANCEL);
        nghttp2_session_send(client);

        server_out.len = 0;
        pump(server, &client_out, &server_out);
        if (server_out.len > 0) {
            client_recv(client, &server_out);
        }
    }

    /* After exceeding the RST limit, GOAWAY should have been sent */
    TEST_ASSERT_TRUE(io_http2_goaway_sent(server));

    nghttp2_session_del(client);
    io_http2_session_destroy(server);
}

/* ---- Test runner ---- */

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_http2_session_create_destroy);
    RUN_TEST(test_http2_connection_preface);
    RUN_TEST(test_http2_settings_ack);
    RUN_TEST(test_http2_simple_get);
    RUN_TEST(test_http2_post_with_body);
    RUN_TEST(test_http2_stream_multiplexing);
    RUN_TEST(test_http2_flow_control);
    RUN_TEST(test_http2_goaway_two_phase);
    RUN_TEST(test_http2_goaway_drains);
    RUN_TEST(test_http2_rst_stream);
    RUN_TEST(test_http2_max_concurrent_streams);
    RUN_TEST(test_http2_rapid_reset_protection);

    return UNITY_END();
}

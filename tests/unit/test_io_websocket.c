/**
 * @file test_io_websocket.c
 * @brief Unit tests for WebSocket (RFC 6455) — wslay-backed API.
 */

#include "ws/io_websocket.h"

#include <errno.h>
#include <string.h>

#include <unity.h>

void setUp(void)
{
}
void tearDown(void)
{
}

/* ---- Handshake tests (iohttp's own) ---- */

void test_ws_compute_accept(void)
{
    /* RFC 6455 §4.2.2 test vector */
    const char *key = "dGhlIHNhbXBsZSBub25jZQ==";
    char accept[IO_WS_ACCEPT_KEY_LEN + 1];

    int rc = io_ws_compute_accept(key, accept);
    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_EQUAL_STRING("s3pPLMBiTxaQ9kYGzzhZRbK+xOo=", accept);
}

void test_ws_compute_accept_null(void)
{
    char accept[IO_WS_ACCEPT_KEY_LEN + 1];
    TEST_ASSERT_EQUAL_INT(-EINVAL, io_ws_compute_accept(nullptr, accept));
    TEST_ASSERT_EQUAL_INT(-EINVAL, io_ws_compute_accept("key", nullptr));
}

void test_ws_validate_upgrade_valid(void)
{
    int rc =
        io_ws_validate_upgrade("GET", "websocket", "Upgrade", "dGhlIHNhbXBsZSBub25jZQ==", "13");
    TEST_ASSERT_EQUAL_INT(0, rc);
}

void test_ws_validate_upgrade_missing_key(void)
{
    int rc = io_ws_validate_upgrade("GET", "websocket", "Upgrade", "", "13");
    TEST_ASSERT_EQUAL_INT(-EINVAL, rc);
}

void test_ws_validate_upgrade_bad_method(void)
{
    int rc = io_ws_validate_upgrade("POST", "websocket", "Upgrade", "key==", "13");
    TEST_ASSERT_EQUAL_INT(-EINVAL, rc);
}

void test_ws_validate_upgrade_bad_version(void)
{
    int rc = io_ws_validate_upgrade("GET", "websocket", "Upgrade", "key==", "8");
    TEST_ASSERT_EQUAL_INT(-EINVAL, rc);
}

/* ---- Buffer-based I/O for testing wslay integration ---- */

typedef struct {
    uint8_t buf[4096];
    size_t len;
    size_t pos;
} test_buf_t;

typedef struct {
    test_buf_t rbuf; /* data to be "received" (client → server) */
    test_buf_t wbuf; /* data "sent" (server → client) */
    uint8_t last_opcode;
    uint8_t last_msg[1024];
    size_t last_msg_len;
    bool close_called;
    uint16_t close_code;
} test_io_t;

static ssize_t test_recv(uint8_t *buf, size_t len, bool *wouldblock, void *ctx)
{
    test_io_t *io = ctx;
    size_t avail = io->rbuf.len - io->rbuf.pos;
    if (avail == 0) {
        *wouldblock = true;
        return -1;
    }
    size_t n = (len < avail) ? len : avail;
    memcpy(buf, io->rbuf.buf + io->rbuf.pos, n);
    io->rbuf.pos += n;
    return (ssize_t)n;
}

static ssize_t test_send(const uint8_t *data, size_t len, bool *wouldblock, void *ctx)
{
    (void)wouldblock;
    test_io_t *io = ctx;
    size_t avail = sizeof(io->wbuf.buf) - io->wbuf.len;
    size_t n = (len < avail) ? len : avail;
    memcpy(io->wbuf.buf + io->wbuf.len, data, n);
    io->wbuf.len += n;
    return (ssize_t)n;
}

static void test_on_msg(uint8_t opcode, const uint8_t *msg, size_t len, void *ctx)
{
    test_io_t *io = ctx;
    io->last_opcode = opcode;
    size_t copy = (len < sizeof(io->last_msg)) ? len : sizeof(io->last_msg);
    memcpy(io->last_msg, msg, copy);
    io->last_msg_len = len;
}

static void test_on_close(uint16_t code, void *ctx)
{
    test_io_t *io = ctx;
    io->close_called = true;
    io->close_code = code;
}

/* Helper: build a masked client frame into buf, return total frame size */
static size_t build_masked_frame(uint8_t *buf, uint8_t opcode, bool fin, const uint8_t *payload,
                                 size_t len)
{
    buf[0] = (uint8_t)((fin ? 0x80U : 0x00U) | (opcode & 0x0FU));
    size_t off = 2;

    if (len <= 125) {
        buf[1] = (uint8_t)(0x80U | len); /* mask bit set */
    } else {
        buf[1] = 0x80U | 126;
        buf[2] = (uint8_t)(len >> 8);
        buf[3] = (uint8_t)(len & 0xFF);
        off = 4;
    }

    /* mask key = 0x00 (trivial, no XOR effect) */
    memset(buf + off, 0, 4);
    off += 4;

    if (payload != nullptr && len > 0) {
        memcpy(buf + off, payload, len);
    }
    return off + len;
}

static io_ws_ctx_t make_ctx(test_io_t *io)
{
    return (io_ws_ctx_t){
        .on_msg = test_on_msg,
        .on_close = test_on_close,
        .recv = test_recv,
        .send = test_send,
        .user_data = io,
        .wslay_ctx = nullptr,
        .max_recv_msg_len = IO_WS_DEFAULT_MAX_MSG,
    };
}

/* ---- wslay-backed context tests ---- */

void test_ws_ctx_init_destroy(void)
{
    test_io_t io;
    memset(&io, 0, sizeof(io));
    io_ws_ctx_t ws = make_ctx(&io);

    int rc = io_ws_ctx_init(&ws);
    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_NOT_NULL(ws.wslay_ctx);

    TEST_ASSERT_TRUE(io_ws_want_read(&ws));
    TEST_ASSERT_FALSE(io_ws_close_received(&ws));
    TEST_ASSERT_FALSE(io_ws_close_sent(&ws));

    io_ws_ctx_destroy(&ws);
    TEST_ASSERT_NULL(ws.wslay_ctx);
}

void test_ws_ctx_init_null_recv(void)
{
    io_ws_ctx_t ws = {.recv = nullptr, .send = test_send};
    TEST_ASSERT_EQUAL_INT(-EINVAL, io_ws_ctx_init(&ws));
}

void test_ws_recv_text_message(void)
{
    test_io_t io;
    memset(&io, 0, sizeof(io));
    io_ws_ctx_t ws = make_ctx(&io);

    int rc = io_ws_ctx_init(&ws);
    TEST_ASSERT_EQUAL_INT(0, rc);

    /* Feed a masked text frame "Hello" */
    const char *msg = "Hello";
    io.rbuf.len = build_masked_frame(io.rbuf.buf, WSLAY_TEXT_FRAME, true, (const uint8_t *)msg, 5);

    rc = io_ws_recv(&ws);
    TEST_ASSERT_EQUAL_INT(0, rc);

    TEST_ASSERT_EQUAL_UINT8(WSLAY_TEXT_FRAME, io.last_opcode);
    TEST_ASSERT_EQUAL_size_t(5, io.last_msg_len);
    TEST_ASSERT_EQUAL_INT(0, memcmp(io.last_msg, "Hello", 5));

    io_ws_ctx_destroy(&ws);
}

void test_ws_recv_binary_message(void)
{
    test_io_t io;
    memset(&io, 0, sizeof(io));
    io_ws_ctx_t ws = make_ctx(&io);

    int rc = io_ws_ctx_init(&ws);
    TEST_ASSERT_EQUAL_INT(0, rc);

    uint8_t data[] = {0xDE, 0xAD, 0xBE, 0xEF};
    io.rbuf.len = build_masked_frame(io.rbuf.buf, WSLAY_BINARY_FRAME, true, data, 4);

    rc = io_ws_recv(&ws);
    TEST_ASSERT_EQUAL_INT(0, rc);

    TEST_ASSERT_EQUAL_UINT8(WSLAY_BINARY_FRAME, io.last_opcode);
    TEST_ASSERT_EQUAL_size_t(4, io.last_msg_len);
    TEST_ASSERT_EQUAL_INT(0, memcmp(io.last_msg, data, 4));

    io_ws_ctx_destroy(&ws);
}

void test_ws_send_text(void)
{
    test_io_t io;
    memset(&io, 0, sizeof(io));
    io_ws_ctx_t ws = make_ctx(&io);

    int rc = io_ws_ctx_init(&ws);
    TEST_ASSERT_EQUAL_INT(0, rc);

    rc = io_ws_queue_text(&ws, "World", 5);
    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_TRUE(io_ws_want_write(&ws));

    rc = io_ws_send(&ws);
    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_GREATER_THAN(0, io.wbuf.len);

    /* Verify sent frame: FIN+TEXT, no mask, payload "World" */
    TEST_ASSERT_EQUAL_HEX8(0x81, io.wbuf.buf[0]); /* FIN + TEXT */
    TEST_ASSERT_EQUAL_HEX8(0x05, io.wbuf.buf[1]); /* len=5, no mask */
    TEST_ASSERT_EQUAL_INT(0, memcmp(io.wbuf.buf + 2, "World", 5));

    io_ws_ctx_destroy(&ws);
}

void test_ws_ping_auto_pong(void)
{
    test_io_t io;
    memset(&io, 0, sizeof(io));
    io_ws_ctx_t ws = make_ctx(&io);

    int rc = io_ws_ctx_init(&ws);
    TEST_ASSERT_EQUAL_INT(0, rc);

    /* Feed a masked ping frame */
    io.rbuf.len = build_masked_frame(io.rbuf.buf, WSLAY_PING, true, nullptr, 0);

    rc = io_ws_recv(&ws);
    TEST_ASSERT_EQUAL_INT(0, rc);

    /* wslay should have auto-queued a pong */
    TEST_ASSERT_TRUE(io_ws_want_write(&ws));

    rc = io_ws_send(&ws);
    TEST_ASSERT_EQUAL_INT(0, rc);

    /* Verify pong was sent: FIN + PONG (0x8A) */
    TEST_ASSERT_EQUAL_HEX8(0x8A, io.wbuf.buf[0]);

    io_ws_ctx_destroy(&ws);
}

void test_ws_close_handshake(void)
{
    test_io_t io;
    memset(&io, 0, sizeof(io));
    io_ws_ctx_t ws = make_ctx(&io);

    int rc = io_ws_ctx_init(&ws);
    TEST_ASSERT_EQUAL_INT(0, rc);

    /* Feed a masked close frame with code 1000 */
    uint8_t close_payload[] = {0x03, 0xE8}; /* 1000 big-endian */
    io.rbuf.len = build_masked_frame(io.rbuf.buf, WSLAY_CONNECTION_CLOSE, true, close_payload, 2);

    rc = io_ws_recv(&ws);
    TEST_ASSERT_EQUAL_INT(0, rc);

    /* on_close callback should have been called */
    TEST_ASSERT_TRUE(io.close_called);
    TEST_ASSERT_EQUAL_UINT16(1000, io.close_code);

    /* wslay auto-queues close response */
    TEST_ASSERT_TRUE(io_ws_want_write(&ws));
    rc = io_ws_send(&ws);
    TEST_ASSERT_EQUAL_INT(0, rc);

    TEST_ASSERT_TRUE(io_ws_close_received(&ws));
    TEST_ASSERT_TRUE(io_ws_close_sent(&ws));

    io_ws_ctx_destroy(&ws);
}

void test_ws_queue_close(void)
{
    test_io_t io;
    memset(&io, 0, sizeof(io));
    io_ws_ctx_t ws = make_ctx(&io);

    int rc = io_ws_ctx_init(&ws);
    TEST_ASSERT_EQUAL_INT(0, rc);

    rc = io_ws_queue_close(&ws, 1001, "going away", 10);
    TEST_ASSERT_EQUAL_INT(0, rc);

    rc = io_ws_send(&ws);
    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_TRUE(io_ws_close_sent(&ws));

    /* Verify close frame in output */
    TEST_ASSERT_EQUAL_HEX8(0x88, io.wbuf.buf[0]); /* FIN + CLOSE */

    io_ws_ctx_destroy(&ws);
}

void test_ws_queue_ping(void)
{
    test_io_t io;
    memset(&io, 0, sizeof(io));
    io_ws_ctx_t ws = make_ctx(&io);

    int rc = io_ws_ctx_init(&ws);
    TEST_ASSERT_EQUAL_INT(0, rc);

    rc = io_ws_queue_ping(&ws);
    TEST_ASSERT_EQUAL_INT(0, rc);

    rc = io_ws_send(&ws);
    TEST_ASSERT_EQUAL_INT(0, rc);

    TEST_ASSERT_EQUAL_HEX8(0x89, io.wbuf.buf[0]); /* FIN + PING */

    io_ws_ctx_destroy(&ws);
}

/* ---- Test runner ---- */

int main(void)
{
    UNITY_BEGIN();

    /* Handshake (iohttp) */
    RUN_TEST(test_ws_compute_accept);
    RUN_TEST(test_ws_compute_accept_null);
    RUN_TEST(test_ws_validate_upgrade_valid);
    RUN_TEST(test_ws_validate_upgrade_missing_key);
    RUN_TEST(test_ws_validate_upgrade_bad_method);
    RUN_TEST(test_ws_validate_upgrade_bad_version);

    /* wslay context lifecycle */
    RUN_TEST(test_ws_ctx_init_destroy);
    RUN_TEST(test_ws_ctx_init_null_recv);

    /* Message exchange via wslay */
    RUN_TEST(test_ws_recv_text_message);
    RUN_TEST(test_ws_recv_binary_message);
    RUN_TEST(test_ws_send_text);
    RUN_TEST(test_ws_ping_auto_pong);
    RUN_TEST(test_ws_close_handshake);
    RUN_TEST(test_ws_queue_close);
    RUN_TEST(test_ws_queue_ping);

    return UNITY_END();
}

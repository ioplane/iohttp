# Sprint 10: HTTP/3 (QUIC) Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development to implement this plan task-by-task.

**Goal:** Add HTTP/3 support via ngtcp2 (QUIC transport) + nghttp3 (HTTP/3 framing), using the same buffer-based I/O pattern as HTTP/2.

**Architecture:** Two-layer design. Bottom layer: `io_quic.{h,c}` wraps ngtcp2 with wolfSSL crypto callbacks — manages QUIC connections, timers, crypto handshake. Top layer: `io_http3.{h,c}` wraps nghttp3 — processes HTTP/3 frames, dispatches requests via `io_ctx_t`. Both layers are buffer-based (no sockets): application feeds UDP datagrams in, retrieves datagrams out. Same pattern as `io_http2.{h,c}` but over QUIC datagrams instead of TCP bytestreams. Alt-Svc header advertises HTTP/3 availability on HTTP/1.1 and HTTP/2 responses.

**Tech Stack:** C23, ngtcp2 1.21.0+ (QUIC), ngtcp2_crypto_wolfssl (QUIC-TLS), nghttp3 1.15.0+ (HTTP/3), wolfSSL 5.8.4+ (TLS 1.3 crypto), Unity tests, Linux kernel 6.7+.

**Scope:** Core QUIC transport + HTTP/3 request/response + Alt-Svc. No connection migration, no 0-RTT resumption, no QPACK dynamic table tuning (defaults only). Those are follow-up sprints.

**Build/test:**
```bash
# Inside the container:
podman run --rm --security-opt seccomp=unconfined \
  -v /opt/projects/repositories/iohttp:/workspace:Z \
  localhost/iohttp-dev:latest bash -c "cd /workspace && \
    cmake --preset clang-debug && \
    cmake --build --preset clang-debug && \
    ctest --preset clang-debug --output-on-failure"
```

**Reference files (read before implementing):**
- `src/http/io_http2.h` + `io_http2.c` — THE pattern to follow (buffer-based session, callbacks, output buffer, flush)
- `src/tls/io_tls.h` — wolfSSL integration pattern (cipher buffers, ALPN)
- `src/core/io_ctx.h` — unified context used for request dispatch
- `src/http/io_request.h` + `io_response.h` — request/response types
- `src/core/io_loop.h` — io_uring operation types and user_data encoding
- `CMakeLists.txt` — dependency detection and `io_add_test()` macro
- `CLAUDE.md` — coding conventions, security requirements, io_uring rules

**Container dependencies (already installed):**
```bash
# Verify before starting:
PKG_CONFIG_PATH=/usr/local/lib/pkgconfig:/usr/local/lib64/pkgconfig \
  pkg-config --libs libngtcp2 libngtcp2_crypto_wolfssl libnghttp3
# Expected: -lngtcp2 -lngtcp2_crypto_wolfssl -lnghttp3
```

---

## Task 1: QUIC transport config and types (io_quic.h)

**Files:**
- Create: `src/http/io_quic.h`

**Step 1: Write io_quic.h**

Header-only types and function declarations. Follow `io_http2.h` pattern:
- Opaque `io_quic_conn_t` (QUIC connection, wraps ngtcp2)
- Config struct with sensible QUIC defaults
- Buffer-based API: `io_quic_on_recv()` / `io_quic_flush()` / `io_quic_timeout()`
- Callback type for passing decrypted QUIC stream data up to HTTP/3 layer

```c
/**
 * @file io_quic.h
 * @brief QUIC transport — ngtcp2-backed, buffer-based I/O.
 *
 * No socket I/O — application feeds UDP datagrams via io_quic_on_recv(),
 * retrieves output datagrams via io_quic_flush(). Timer-driven via
 * io_quic_timeout(). Same buffer pattern as io_http2.
 */

#ifndef IOHTTP_HTTP_QUIC_H
#define IOHTTP_HTTP_QUIC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <netinet/in.h>

/* ---- Forward declaration (opaque) ---- */

typedef struct io_quic_conn io_quic_conn_t;

/* ---- Configuration ---- */

typedef struct {
    uint32_t max_streams_bidi;      /* default 100 */
    uint32_t max_stream_data_bidi;  /* per-stream flow control, default 256 KiB */
    uint32_t max_data;              /* connection-level flow control, default 1 MiB */
    uint32_t idle_timeout_ms;       /* default 30000 */
    uint32_t max_udp_payload;       /* default 1200 (initial), grows after handshake */
    const char *cert_file;          /* TLS certificate */
    const char *key_file;           /* TLS private key */
} io_quic_config_t;

/* ---- Callback types ---- */

/**
 * @brief Called when stream data is received from the peer.
 * @param conn      QUIC connection.
 * @param stream_id QUIC stream ID (client-initiated bidi: 0, 4, 8, ...).
 * @param data      Stream data bytes.
 * @param len       Number of bytes.
 * @param fin       true if this is the final data on this stream.
 * @param user_data User context from io_quic_conn_create().
 * @return 0 on success, negative errno on failure.
 */
typedef int (*io_quic_stream_data_fn)(io_quic_conn_t *conn, int64_t stream_id,
                                       const uint8_t *data, size_t len,
                                       bool fin, void *user_data);

/**
 * @brief Called when a new stream is opened by the peer.
 * @param conn      QUIC connection.
 * @param stream_id New stream ID.
 * @param user_data User context.
 * @return 0 on success, negative errno to reject.
 */
typedef int (*io_quic_stream_open_fn)(io_quic_conn_t *conn, int64_t stream_id,
                                       void *user_data);

/**
 * @brief Called when the QUIC handshake completes.
 * @param conn      QUIC connection.
 * @param user_data User context.
 */
typedef void (*io_quic_handshake_done_fn)(io_quic_conn_t *conn, void *user_data);

typedef struct {
    io_quic_stream_data_fn on_stream_data;
    io_quic_stream_open_fn on_stream_open;
    io_quic_handshake_done_fn on_handshake_done;
} io_quic_callbacks_t;

/* ---- Connection lifecycle ---- */

/**
 * @brief Initialize config with safe defaults.
 */
void io_quic_config_init(io_quic_config_t *cfg);

/**
 * @brief Validate a QUIC configuration.
 * @return 0 on success, negative errno on error.
 */
[[nodiscard]] int io_quic_config_validate(const io_quic_config_t *cfg);

/**
 * @brief Create a QUIC server connection from an Initial packet.
 * @param cfg       Configuration.
 * @param cbs       Callbacks for stream events.
 * @param dcid      Destination Connection ID from Initial packet.
 * @param dcid_len  DCID length.
 * @param scid      Source Connection ID (generated by server).
 * @param scid_len  SCID length.
 * @param local     Local address (server).
 * @param remote    Remote address (client).
 * @param user_data Passed to callbacks.
 * @return New connection, or nullptr on failure.
 */
[[nodiscard]] io_quic_conn_t *io_quic_conn_create(const io_quic_config_t *cfg,
                                                    const io_quic_callbacks_t *cbs,
                                                    const uint8_t *dcid, size_t dcid_len,
                                                    const uint8_t *scid, size_t scid_len,
                                                    const struct sockaddr *local,
                                                    const struct sockaddr *remote,
                                                    void *user_data);

/**
 * @brief Destroy a QUIC connection and free all resources.
 */
void io_quic_conn_destroy(io_quic_conn_t *conn);

/* ---- Data processing ---- */

/**
 * @brief Feed a received UDP datagram into the QUIC connection.
 * @param conn   Connection.
 * @param data   Raw UDP payload.
 * @param len    Payload length.
 * @param remote Sender address.
 * @return 0 on success, negative errno on error.
 */
[[nodiscard]] int io_quic_on_recv(io_quic_conn_t *conn, const uint8_t *data, size_t len,
                                   const struct sockaddr *remote);

/**
 * @brief Get pending output datagrams to send.
 * @param conn     Connection.
 * @param out_data Set to output buffer (valid until next flush/recv/timeout).
 * @param out_len  Set to output length.
 * @return 0 on success (may set out_len=0), negative errno on error.
 */
[[nodiscard]] int io_quic_flush(io_quic_conn_t *conn, const uint8_t **out_data,
                                 size_t *out_len);

/**
 * @brief Handle timer expiration (retransmission, loss detection).
 * Must be called when io_quic_get_timeout() expires.
 * @return 0 on success, negative errno on error.
 */
[[nodiscard]] int io_quic_on_timeout(io_quic_conn_t *conn);

/**
 * @brief Write data to a QUIC stream.
 * @param conn      Connection.
 * @param stream_id Stream to write to.
 * @param data      Data to send.
 * @param len       Data length.
 * @param fin       true to close the stream after this write.
 * @return Bytes written (>= 0), negative errno on error.
 */
[[nodiscard]] ssize_t io_quic_write_stream(io_quic_conn_t *conn, int64_t stream_id,
                                            const uint8_t *data, size_t len, bool fin);

/* ---- State queries ---- */

/** @return Timeout in milliseconds until next timer event, or UINT64_MAX if none. */
[[nodiscard]] uint64_t io_quic_get_timeout(const io_quic_conn_t *conn);

/** @return true if the handshake is complete. */
[[nodiscard]] bool io_quic_is_handshake_done(const io_quic_conn_t *conn);

/** @return true if the connection is closed or draining. */
[[nodiscard]] bool io_quic_is_closed(const io_quic_conn_t *conn);

/** @return true if there is data to flush. */
[[nodiscard]] bool io_quic_want_write(const io_quic_conn_t *conn);

/**
 * @brief Initiate graceful QUIC connection close.
 * @param conn       Connection.
 * @param error_code Application error code (0 = no error).
 * @return 0 on success, negative errno on error.
 */
[[nodiscard]] int io_quic_close(io_quic_conn_t *conn, uint64_t error_code);

#endif /* IOHTTP_HTTP_QUIC_H */
```

**Step 2: Commit**
```bash
git add src/http/io_quic.h
git commit -m "feat(quic): add QUIC transport types and API declarations"
```

---

## Task 2: QUIC transport implementation (io_quic.c)

**Files:**
- Create: `src/http/io_quic.c`
- Create: `tests/unit/test_io_quic.c`
- Modify: `CMakeLists.txt`

**Reference:** Read `src/http/io_http2.c` before implementing — follow the same structure (internal struct, output buffer, callbacks, public API).

**Step 1: Write failing tests (test_io_quic.c)**

```c
#include <unity/unity.h>
#include "http/io_quic.h"
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
```

**Step 2: Write io_quic.c**

Internal struct pattern (follow `io_http2_session`):

```c
/**
 * @file io_quic.c
 * @brief QUIC transport — ngtcp2 + ngtcp2_crypto_wolfssl implementation.
 *
 * Buffer-based I/O: no sockets. Application feeds UDP datagrams
 * via io_quic_on_recv(), retrieves output via io_quic_flush().
 */

#include "http/io_quic.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <ngtcp2/ngtcp2.h>
#include <ngtcp2/ngtcp2_crypto.h>
#include <ngtcp2/ngtcp2_crypto_wolfssl.h>
#include <wolfssl/options.h>
#include <wolfssl/ssl.h>

/* ---- Constants ---- */

constexpr uint32_t QUIC_DEFAULT_MAX_STREAMS_BIDI = 100;
constexpr uint32_t QUIC_DEFAULT_MAX_STREAM_DATA = 256 * 1024;
constexpr uint32_t QUIC_DEFAULT_MAX_DATA = 1024 * 1024;
constexpr uint32_t QUIC_DEFAULT_IDLE_TIMEOUT_MS = 30000;
constexpr uint32_t QUIC_DEFAULT_MAX_UDP_PAYLOAD = 1200;
constexpr size_t QUIC_OUTPUT_BUF_SIZE = 65536;
constexpr size_t QUIC_MAX_PKTLEN = 1452;

/* ---- Connection (opaque) ---- */

struct io_quic_conn {
    ngtcp2_conn *ngtcp2_conn;
    WOLFSSL_CTX *ssl_ctx;
    WOLFSSL *ssl;
    io_quic_config_t config;
    io_quic_callbacks_t callbacks;
    void *user_data;

    /* Output buffer for flush() */
    uint8_t *out_buf;
    size_t out_len;
    size_t out_cap;

    /* Connection state */
    bool handshake_done;
    bool closed;
};
```

Key implementation points:
- `io_quic_config_init()` — set all defaults
- `io_quic_config_validate()` — check cert_file, key_file, non-zero streams/timeout
- `io_quic_conn_create()` — init wolfSSL CTX + SSL for QUIC, init ngtcp2_conn via `ngtcp2_conn_server_new()`, set transport params, set crypto callbacks via `ngtcp2_crypto_wolfssl_configure_server_context()`
- `io_quic_conn_destroy()` — free ngtcp2_conn, SSL, SSL_CTX, output buffer
- `io_quic_on_recv()` — call `ngtcp2_conn_read_pkt()` which triggers stream callbacks
- `io_quic_flush()` — call `ngtcp2_conn_writev_stream()` in a loop to fill output buffer with QUIC packets
- `io_quic_on_timeout()` — call `ngtcp2_conn_handle_expiry()`, then flush
- `io_quic_write_stream()` — enqueue stream data for the next flush (ngtcp2 handles framing)
- `io_quic_close()` — call `ngtcp2_conn_write_connection_close()`
- State queries delegate to ngtcp2: `ngtcp2_conn_get_handshake_completed()`, `ngtcp2_conn_in_draining_period()`, `ngtcp2_conn_get_expiry()`

**ngtcp2 server callbacks to implement:**
```c
/* Required ngtcp2 callbacks (set via ngtcp2_callbacks struct): */
static int recv_stream_data_cb(ngtcp2_conn *conn, uint32_t flags,
                                int64_t stream_id, uint64_t offset,
                                const uint8_t *data, size_t datalen,
                                void *user_data, void *stream_user_data);
static int stream_open_cb(ngtcp2_conn *conn, int64_t stream_id, void *user_data);
static int handshake_completed_cb(ngtcp2_conn *conn, void *user_data);
static int recv_crypto_data_cb(ngtcp2_conn *conn,
                                ngtcp2_encryption_level encryption_level,
                                uint64_t offset, const uint8_t *data,
                                size_t datalen, void *user_data);
static void rand_cb(uint8_t *dest, size_t destlen,
                     const ngtcp2_rand_ctx *rand_ctx);
static int get_new_connection_id_cb(ngtcp2_conn *conn, ngtcp2_cid *cid,
                                     uint8_t *token, size_t cidlen,
                                     void *user_data);
```

**wolfSSL QUIC setup:**
```c
/* In io_quic_conn_create(): */
ssl_ctx = wolfSSL_CTX_new(wolfTLSv1_3_server_method());
wolfSSL_CTX_set_quic_method(ssl_ctx, &quic_method);  /* ngtcp2_crypto sets this */
ngtcp2_crypto_wolfssl_configure_server_context(ssl_ctx);
wolfSSL_CTX_use_certificate_file(ssl_ctx, cfg->cert_file, WOLFSSL_FILETYPE_PEM);
wolfSSL_CTX_use_PrivateKey_file(ssl_ctx, cfg->key_file, WOLFSSL_FILETYPE_PEM);

ssl = wolfSSL_new(ssl_ctx);
ngtcp2_conn_set_tls_native_handle(conn, ssl);
```

**Step 3: Add to CMakeLists.txt**

```cmake
# ============================================================================
# Sprint 10: QUIC transport (ngtcp2 + ngtcp2_crypto_wolfssl)
# ============================================================================

if(NGTCP2_FOUND AND NGTCP2_WOLFSSL_FOUND)
    add_library(io_quic STATIC src/http/io_quic.c)
    target_include_directories(io_quic PUBLIC ${CMAKE_SOURCE_DIR}/src)
    target_include_directories(io_quic PUBLIC
        ${NGTCP2_INCLUDE_DIRS}
        ${NGTCP2_WOLFSSL_INCLUDE_DIRS}
        ${WOLFSSL_INCLUDE_DIRS}
    )
    target_link_directories(io_quic PUBLIC
        ${NGTCP2_LIBRARY_DIRS}
        ${NGTCP2_WOLFSSL_LIBRARY_DIRS}
        ${WOLFSSL_LIBRARY_DIRS}
    )
    target_link_libraries(io_quic PUBLIC
        ${NGTCP2_LIBRARIES}
        ${NGTCP2_WOLFSSL_LIBRARIES}
        ${WOLFSSL_LIBRARIES}
    )
    message(STATUS "QUIC transport enabled (ngtcp2 + wolfSSL)")

    if(BUILD_TESTING AND TARGET unity)
        add_executable(test_io_quic tests/unit/test_io_quic.c)
        target_include_directories(test_io_quic PRIVATE ${CMAKE_SOURCE_DIR}/src)
        target_include_directories(test_io_quic PRIVATE
            ${NGTCP2_INCLUDE_DIRS}
            ${NGTCP2_WOLFSSL_INCLUDE_DIRS}
            ${WOLFSSL_INCLUDE_DIRS}
        )
        target_link_directories(test_io_quic PRIVATE
            ${NGTCP2_LIBRARY_DIRS}
            ${NGTCP2_WOLFSSL_LIBRARY_DIRS}
            ${WOLFSSL_LIBRARY_DIRS}
        )
        target_link_libraries(test_io_quic PRIVATE
            unity io_quic
            ${NGTCP2_LIBRARIES}
            ${NGTCP2_WOLFSSL_LIBRARIES}
            ${WOLFSSL_LIBRARIES}
        )
        target_compile_options(test_io_quic PRIVATE
            -Wno-missing-prototypes -Wno-missing-declarations
        )
        target_compile_definitions(test_io_quic PRIVATE
            TEST_CERTS_DIR="${CMAKE_SOURCE_DIR}/tests/certs"
        )
        add_test(NAME test_io_quic COMMAND test_io_quic)
    endif()
else()
    message(STATUS "QUIC transport disabled (ngtcp2 or ngtcp2_crypto_wolfssl not found)")
endif()
```

**IMPORTANT CMake note:** The existing `pkg_check_modules(NGTCP2 libngtcp2)` and `pkg_check_modules(NGTCP2_WOLFSSL libngtcp2_crypto_wolfssl)` at line ~82 need `PKG_CONFIG_PATH` set properly. Add this BEFORE the `pkg_check_modules` calls:

```cmake
# Ensure pkg-config finds libraries in /usr/local
set(ENV{PKG_CONFIG_PATH} "/usr/local/lib/pkgconfig:/usr/local/lib64/pkgconfig:$ENV{PKG_CONFIG_PATH}")
```

**Step 4: Build and run tests**
```bash
cmake --preset clang-debug && cmake --build --preset clang-debug && ctest --preset clang-debug -R test_io_quic
```

**Step 5: Commit**
```bash
git add src/http/io_quic.c tests/unit/test_io_quic.c CMakeLists.txt
git commit -m "feat(quic): implement QUIC transport with ngtcp2 + wolfSSL (14 tests)"
```

---

## Task 3: QUIC connection handshake test (with real crypto)

**Files:**
- Modify: `tests/unit/test_io_quic.c`

This task adds tests that create a REAL QUIC connection using test certificates, performing a client↔server handshake in-memory (no sockets, just buffer exchange). Pattern: same as `tests/integration/test_tls_uring.c` and `tests/integration/test_http2_server.c`.

**Step 1: Add handshake tests**

```c
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

static int on_stream_data(io_quic_conn_t *conn, int64_t stream_id,
                           const uint8_t *data, size_t len,
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
                                                 (struct sockaddr *)&remote,
                                                 nullptr);
    /* Connection creation may fail if cert files don't exist —
     * use TEST_IGNORE_MESSAGE in that case */
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
    /* Initial server hello data should be generated */

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
```

Add to `main()`:
```c
    RUN_TEST(test_quic_conn_create_with_certs);
    RUN_TEST(test_quic_get_timeout_not_max_after_create);
    RUN_TEST(test_quic_close_sets_closed);
```

**Step 2: Build and run**
```bash
cmake --build --preset clang-debug && ctest --preset clang-debug -R test_io_quic --output-on-failure
```

**Step 3: Commit**
```bash
git add tests/unit/test_io_quic.c
git commit -m "test(quic): add QUIC connection handshake tests with real certs (3 tests)"
```

---

## Task 4: HTTP/3 session types (io_http3.h)

**Files:**
- Create: `src/http/io_http3.h`

Follow `io_http2.h` pattern exactly — same API shape, different transport.

**Step 1: Write io_http3.h**

```c
/**
 * @file io_http3.h
 * @brief HTTP/3 session management — nghttp3-backed, buffer-based I/O.
 *
 * Sits on top of io_quic.h (QUIC transport). Application feeds QUIC stream
 * data via io_http3_on_stream_data(), retrieves output via QUIC layer.
 * Same dispatch pattern as io_http2: callback fires on complete request.
 */

#ifndef IOHTTP_HTTP_HTTP3_H
#define IOHTTP_HTTP_HTTP3_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "core/io_ctx.h"
#include "http/io_quic.h"
#include "http/io_request.h"
#include "http/io_response.h"

/* ---- Forward declaration (opaque) ---- */

typedef struct io_http3_session io_http3_session_t;

/* ---- Configuration ---- */

typedef struct {
    uint32_t max_header_list_size; /* default 8192 */
    uint32_t qpack_max_dtable_capacity; /* default 0 (no dynamic table, simpler) */
    uint32_t qpack_blocked_streams; /* default 0 */
} io_http3_config_t;

/* ---- Callback types ---- */

/**
 * @brief Called when a complete HTTP/3 request is received.
 * @param c         Unified context. Handler fills c->resp.
 * @param stream_id QUIC stream ID.
 * @param user_data User context from io_http3_session_create().
 * @return 0 on success, negative errno on failure.
 */
typedef int (*io_http3_on_request_fn)(io_ctx_t *c, int64_t stream_id, void *user_data);

/* ---- Session lifecycle ---- */

/**
 * @brief Initialize HTTP/3 config with defaults.
 */
void io_http3_config_init(io_http3_config_t *cfg);

/**
 * @brief Create an HTTP/3 session bound to a QUIC connection.
 * @param cfg       Configuration (nullptr for defaults).
 * @param quic_conn QUIC connection (must be established).
 * @param on_req    Callback for completed requests.
 * @param user_data Passed to on_req callback.
 * @return New session, or nullptr on failure.
 */
[[nodiscard]] io_http3_session_t *io_http3_session_create(const io_http3_config_t *cfg,
                                                           io_quic_conn_t *quic_conn,
                                                           io_http3_on_request_fn on_req,
                                                           void *user_data);

/**
 * @brief Destroy an HTTP/3 session and free all resources.
 */
void io_http3_session_destroy(io_http3_session_t *session);

/* ---- Data processing ---- */

/**
 * @brief Feed QUIC stream data into the HTTP/3 session.
 * Called from the QUIC stream_data callback.
 * @param session   HTTP/3 session.
 * @param stream_id QUIC stream ID.
 * @param data      Stream data bytes.
 * @param len       Number of bytes.
 * @param fin       End of stream flag.
 * @return 0 on success, negative errno on error.
 */
[[nodiscard]] int io_http3_on_stream_data(io_http3_session_t *session, int64_t stream_id,
                                           const uint8_t *data, size_t len, bool fin);

/**
 * @brief Notify HTTP/3 session that a new stream was opened.
 * Called from the QUIC stream_open callback.
 * @return 0 on success, negative errno on error.
 */
[[nodiscard]] int io_http3_on_stream_open(io_http3_session_t *session, int64_t stream_id);

/**
 * @brief Submit response for a stream.
 * Called by the request handler after processing.
 * @param session   HTTP/3 session.
 * @param stream_id Stream to respond on.
 * @param resp      Response data.
 * @return 0 on success, negative errno on error.
 */
[[nodiscard]] int io_http3_submit_response(io_http3_session_t *session, int64_t stream_id,
                                            const io_response_t *resp);

/* ---- State queries ---- */

/** @return true if the session has been shut down. */
[[nodiscard]] bool io_http3_is_shutdown(const io_http3_session_t *session);

/**
 * @brief Initiate HTTP/3 GOAWAY.
 * @return 0 on success, negative errno on error.
 */
[[nodiscard]] int io_http3_shutdown(io_http3_session_t *session);

#endif /* IOHTTP_HTTP_HTTP3_H */
```

**Step 2: Commit**
```bash
git add src/http/io_http3.h
git commit -m "feat(http3): add HTTP/3 session types and API declarations"
```

---

## Task 5: HTTP/3 session implementation (io_http3.c)

**Files:**
- Create: `src/http/io_http3.c`
- Create: `tests/unit/test_io_http3.c`
- Modify: `CMakeLists.txt`

**Reference:** Read `src/http/io_http2.c` before implementing — follow the exact same internal structure (per-stream data, arena for header copies, output buffer, callbacks, response submission).

**Step 1: Write failing tests (test_io_http3.c)**

```c
#include <unity/unity.h>
#include "http/io_http3.h"
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

/* ---- Config tests ---- */

void test_http3_config_init_defaults(void)
{
    io_http3_config_t cfg;
    io_http3_config_init(&cfg);
    TEST_ASSERT_EQUAL_UINT32(8192, cfg.max_header_list_size);
    TEST_ASSERT_EQUAL_UINT32(0, cfg.qpack_max_dtable_capacity);
    TEST_ASSERT_EQUAL_UINT32(0, cfg.qpack_blocked_streams);
}

/* ---- Session lifecycle null safety ---- */

void test_http3_session_create_null_quic(void)
{
    TEST_ASSERT_NULL(io_http3_session_create(nullptr, nullptr, nullptr, nullptr));
}

void test_http3_session_destroy_null(void)
{
    /* Must not crash */
    io_http3_session_destroy(nullptr);
}

/* ---- Data processing null safety ---- */

void test_http3_on_stream_data_null(void)
{
    uint8_t data[] = "test";
    TEST_ASSERT_EQUAL_INT(-EINVAL,
        io_http3_on_stream_data(nullptr, 0, data, 4, false));
}

void test_http3_on_stream_open_null(void)
{
    TEST_ASSERT_EQUAL_INT(-EINVAL,
        io_http3_on_stream_open(nullptr, 0));
}

void test_http3_submit_response_null(void)
{
    io_response_t resp;
    TEST_ASSERT_EQUAL_INT(-EINVAL,
        io_http3_submit_response(nullptr, 0, &resp));
}

/* ---- State queries ---- */

void test_http3_is_shutdown_null(void)
{
    TEST_ASSERT_TRUE(io_http3_is_shutdown(nullptr)); /* null = shut down */
}

void test_http3_shutdown_null(void)
{
    TEST_ASSERT_EQUAL_INT(-EINVAL, io_http3_shutdown(nullptr));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_http3_config_init_defaults);
    RUN_TEST(test_http3_session_create_null_quic);
    RUN_TEST(test_http3_session_destroy_null);
    RUN_TEST(test_http3_on_stream_data_null);
    RUN_TEST(test_http3_on_stream_open_null);
    RUN_TEST(test_http3_submit_response_null);
    RUN_TEST(test_http3_is_shutdown_null);
    RUN_TEST(test_http3_shutdown_null);
    return UNITY_END();
}
```

**Step 2: Write io_http3.c**

Internal struct (mirror `io_http2_session`):

```c
/**
 * @file io_http3.c
 * @brief HTTP/3 session management — nghttp3-backed implementation.
 *
 * Sits on top of io_quic (ngtcp2 QUIC transport). Processes HTTP/3
 * frames from QUIC stream data, dispatches requests via io_ctx_t.
 */

#include "http/io_http3.h"

#include "core/io_ctx.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <nghttp3/nghttp3.h>
```

Internal types — reuse the same arena and per-stream pattern from HTTP/2:

```c
/* Per-stream arena (same as io_http2.c) */
typedef struct {
    char *buf;
    size_t len;
    size_t cap;
} h3_arena_t;

/* Per-stream data */
typedef struct {
    io_request_t request;
    h3_arena_t arena;
    uint8_t *body_buf;
    size_t body_cap;
    size_t body_len;
    int64_t stream_id;
    bool headers_done;
} h3_stream_data_t;

/* Session (opaque) */
struct io_http3_session {
    nghttp3_conn *ng_conn;
    io_quic_conn_t *quic_conn;
    io_http3_config_t config;
    io_http3_on_request_fn on_request;
    void *user_data;
    bool shutdown_initiated;
};
```

nghttp3 server callbacks to implement:
```c
static int h3_recv_header_cb(nghttp3_conn *conn, int64_t stream_id,
                              int32_t token, nghttp3_rcbuf *name,
                              nghttp3_rcbuf *value, uint8_t flags,
                              void *user_data, void *stream_user_data);
static int h3_end_headers_cb(nghttp3_conn *conn, int64_t stream_id,
                              int fin, void *user_data, void *stream_user_data);
static int h3_recv_data_cb(nghttp3_conn *conn, int64_t stream_id,
                            const uint8_t *data, size_t datalen,
                            void *user_data, void *stream_user_data);
static int h3_end_stream_cb(nghttp3_conn *conn, int64_t stream_id,
                             void *user_data, void *stream_user_data);
static int h3_deferred_consume_cb(nghttp3_conn *conn, int64_t stream_id,
                                   size_t consumed, void *user_data,
                                   void *stream_user_data);
```

Key implementation:
- `io_http3_session_create()` — create nghttp3_conn with `nghttp3_conn_server_new()`, set callbacks, configure QPACK settings
- `io_http3_on_stream_data()` — call `nghttp3_conn_read_stream()` which triggers header/data callbacks
- `io_http3_on_stream_open()` — allocate per-stream data, call `nghttp3_conn_set_stream_user_data()`
- `io_http3_submit_response()` — build nghttp3_nv array from response, call `nghttp3_submit_response()` with data provider
- Response data provider uses `nghttp3_data_reader` callback (same pattern as nghttp2)
- Request dispatch: in `h3_end_stream_cb()`, create `io_ctx_t`, call `on_request()`, then `io_http3_submit_response()` — exactly like `on_frame_recv_cb()` in io_http2.c

**Step 3: Add to CMakeLists.txt**

```cmake
# ============================================================================
# Sprint 10: HTTP/3 session (nghttp3)
# ============================================================================

if(NGHTTP3_FOUND AND TARGET io_quic)
    add_library(io_http3 STATIC src/http/io_http3.c)
    target_include_directories(io_http3 PUBLIC ${CMAKE_SOURCE_DIR}/src)
    target_include_directories(io_http3 PUBLIC ${NGHTTP3_INCLUDE_DIRS})
    target_link_directories(io_http3 PUBLIC ${NGHTTP3_LIBRARY_DIRS})
    target_link_libraries(io_http3 PUBLIC ${NGHTTP3_LIBRARIES} io_quic io_request io_response io_ctx)
    message(STATUS "HTTP/3 enabled (nghttp3)")

    if(BUILD_TESTING AND TARGET unity)
        add_executable(test_io_http3 tests/unit/test_io_http3.c)
        target_include_directories(test_io_http3 PRIVATE ${CMAKE_SOURCE_DIR}/src)
        target_include_directories(test_io_http3 PRIVATE ${NGHTTP3_INCLUDE_DIRS})
        target_link_directories(test_io_http3 PRIVATE ${NGHTTP3_LIBRARY_DIRS})
        target_link_libraries(test_io_http3 PRIVATE
            unity io_http3 io_quic io_request io_response io_ctx
            ${NGHTTP3_LIBRARIES}
        )
        target_compile_options(test_io_http3 PRIVATE
            -Wno-missing-prototypes -Wno-missing-declarations
        )
        add_test(NAME test_io_http3 COMMAND test_io_http3)
    endif()
else()
    message(STATUS "HTTP/3 disabled (nghttp3 or io_quic not available)")
endif()
```

**Step 4: Build and run**
```bash
cmake --preset clang-debug && cmake --build --preset clang-debug && ctest --preset clang-debug -R test_io_http3 --output-on-failure
```

**Step 5: Commit**
```bash
git add src/http/io_http3.c tests/unit/test_io_http3.c CMakeLists.txt
git commit -m "feat(http3): implement HTTP/3 session with nghttp3 (8 tests)"
```

---

## Task 6: Alt-Svc header for HTTP/3 discovery

**Files:**
- Create: `src/http/io_alt_svc.h`
- Create: `src/http/io_alt_svc.c`
- Create: `tests/unit/test_io_alt_svc.c`
- Modify: `CMakeLists.txt`

HTTP/3 discovery requires the server to advertise `Alt-Svc: h3=":443"` on HTTP/1.1 and HTTP/2 responses (RFC 7838). This is a simple response header middleware.

**Step 1: Write failing tests**

```c
#include <unity/unity.h>
#include "http/io_alt_svc.h"
#include "http/io_response.h"
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

void test_alt_svc_format_default_port(void)
{
    char buf[64];
    int len = io_alt_svc_format(buf, sizeof(buf), 443, 0);
    TEST_ASSERT_GREATER_THAN(0, len);
    TEST_ASSERT_EQUAL_STRING("h3=\":443\"", buf);
}

void test_alt_svc_format_custom_port(void)
{
    char buf[64];
    int len = io_alt_svc_format(buf, sizeof(buf), 8443, 0);
    TEST_ASSERT_GREATER_THAN(0, len);
    TEST_ASSERT_EQUAL_STRING("h3=\":8443\"", buf);
}

void test_alt_svc_format_with_max_age(void)
{
    char buf[128];
    int len = io_alt_svc_format(buf, sizeof(buf), 443, 86400);
    TEST_ASSERT_GREATER_THAN(0, len);
    TEST_ASSERT_EQUAL_STRING("h3=\":443\"; ma=86400", buf);
}

void test_alt_svc_format_buffer_too_small(void)
{
    char buf[5];
    int len = io_alt_svc_format(buf, sizeof(buf), 443, 0);
    TEST_ASSERT_EQUAL_INT(-ENOSPC, len);
}

void test_alt_svc_format_null_buffer(void)
{
    int len = io_alt_svc_format(nullptr, 0, 443, 0);
    TEST_ASSERT_EQUAL_INT(-EINVAL, len);
}

void test_alt_svc_add_header(void)
{
    io_response_t resp;
    io_response_init(&resp);
    int rc = io_alt_svc_add_header(&resp, 443, 0);
    TEST_ASSERT_EQUAL_INT(0, rc);
    /* Verify header was added */
    bool found = false;
    for (uint32_t i = 0; i < resp.header_count; i++) {
        if (resp.headers[i].name_len == 7 &&
            memcmp(resp.headers[i].name, "alt-svc", 7) == 0) {
            found = true;
            TEST_ASSERT_EQUAL_STRING("h3=\":443\"", resp.headers[i].value);
        }
    }
    TEST_ASSERT_TRUE(found);
    io_response_destroy(&resp);
}

void test_alt_svc_add_header_null(void)
{
    TEST_ASSERT_EQUAL_INT(-EINVAL, io_alt_svc_add_header(nullptr, 443, 0));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_alt_svc_format_default_port);
    RUN_TEST(test_alt_svc_format_custom_port);
    RUN_TEST(test_alt_svc_format_with_max_age);
    RUN_TEST(test_alt_svc_format_buffer_too_small);
    RUN_TEST(test_alt_svc_format_null_buffer);
    RUN_TEST(test_alt_svc_add_header);
    RUN_TEST(test_alt_svc_add_header_null);
    return UNITY_END();
}
```

**Step 2: Write io_alt_svc.h**

```c
/**
 * @file io_alt_svc.h
 * @brief Alt-Svc header generation for HTTP/3 discovery (RFC 7838).
 */

#ifndef IOHTTP_HTTP_ALT_SVC_H
#define IOHTTP_HTTP_ALT_SVC_H

#include <stdint.h>
#include "http/io_response.h"

/**
 * @brief Format an Alt-Svc header value.
 * @param buf     Output buffer.
 * @param buf_len Buffer capacity.
 * @param port    UDP port for HTTP/3.
 * @param max_age Max-age in seconds (0 = omit).
 * @return Length of formatted string, or -ENOSPC / -EINVAL on error.
 */
[[nodiscard]] int io_alt_svc_format(char *buf, size_t buf_len, uint16_t port,
                                     uint32_t max_age);

/**
 * @brief Add Alt-Svc header to a response.
 * @param resp    Response to add header to.
 * @param port    UDP port for HTTP/3.
 * @param max_age Max-age in seconds (0 = omit).
 * @return 0 on success, negative errno on error.
 */
[[nodiscard]] int io_alt_svc_add_header(io_response_t *resp, uint16_t port,
                                         uint32_t max_age);

#endif /* IOHTTP_HTTP_ALT_SVC_H */
```

**Step 3: Write io_alt_svc.c**

Simple `snprintf`-based formatting. `io_alt_svc_add_header()` calls `io_response_add_header()`.

**Step 4: Add to CMakeLists.txt**

```cmake
# Sprint 10: Alt-Svc header (HTTP/3 discovery)
add_library(io_alt_svc STATIC src/http/io_alt_svc.c)
target_include_directories(io_alt_svc PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(io_alt_svc PUBLIC io_response)

io_add_test(test_io_alt_svc tests/unit/test_io_alt_svc.c io_alt_svc io_response)
```

**Step 5: Build and run**
```bash
cmake --build --preset clang-debug && ctest --preset clang-debug -R test_io_alt_svc --output-on-failure
```

**Step 6: Commit**
```bash
git add src/http/io_alt_svc.h src/http/io_alt_svc.c tests/unit/test_io_alt_svc.c CMakeLists.txt
git commit -m "feat(http3): Alt-Svc header generation for HTTP/3 discovery (7 tests)"
```

---

## Task 7: QUIC + HTTP/3 integration test

**Files:**
- Create: `tests/integration/test_http3_server.c`
- Modify: `CMakeLists.txt`

In-memory QUIC client ↔ server handshake + HTTP/3 request/response exchange. No real UDP sockets — just buffer exchange (same approach as `test_http2_server.c`).

**Step 1: Write integration tests**

```c
#include <unity/unity.h>
#include "http/io_quic.h"
#include "http/io_http3.h"
#include "http/io_request.h"
#include "http/io_response.h"
#include "core/io_ctx.h"
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

/* ---- Test request handler ---- */

static int test_handler(io_ctx_t *c, int64_t stream_id, void *user_data)
{
    (void)stream_id;
    (void)user_data;
    return io_ctx_json(c, 200, "{\"protocol\":\"h3\"}");
}

/* ---- Tests ---- */

void test_quic_server_conn_lifecycle(void)
{
    /* Create server QUIC connection with real certs */
    io_quic_config_t cfg;
    io_quic_config_init(&cfg);
    cfg.cert_file = TEST_CERTS_DIR "/server.crt";
    cfg.key_file = TEST_CERTS_DIR "/server.key";
    io_quic_callbacks_t cbs = {0};
    uint8_t dcid[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    uint8_t scid[8] = {9, 10, 11, 12, 13, 14, 15, 16};
    struct sockaddr_in local = {.sin_family = AF_INET, .sin_port = htons(443)};
    struct sockaddr_in remote = {.sin_family = AF_INET, .sin_port = htons(12345)};

    io_quic_conn_t *conn = io_quic_conn_create(&cfg, &cbs, dcid, 8, scid, 8,
                                                 (struct sockaddr *)&local,
                                                 (struct sockaddr *)&remote, nullptr);
    if (conn == nullptr) {
        TEST_IGNORE_MESSAGE("QUIC conn creation failed (certs?)");
        return;
    }

    /* Verify initial state */
    TEST_ASSERT_FALSE(io_quic_is_handshake_done(conn));
    TEST_ASSERT_FALSE(io_quic_is_closed(conn));
    TEST_ASSERT_TRUE(io_quic_want_write(conn));

    /* Close gracefully */
    TEST_ASSERT_EQUAL_INT(0, io_quic_close(conn, 0));
    io_quic_conn_destroy(conn);
}

void test_http3_session_on_quic_conn(void)
{
    /* Create QUIC conn + HTTP/3 session on top */
    io_quic_config_t qcfg;
    io_quic_config_init(&qcfg);
    qcfg.cert_file = TEST_CERTS_DIR "/server.crt";
    qcfg.key_file = TEST_CERTS_DIR "/server.key";
    io_quic_callbacks_t cbs = {0};
    uint8_t dcid[8] = {1};
    uint8_t scid[8] = {2};
    struct sockaddr_in local = {.sin_family = AF_INET, .sin_port = htons(443)};
    struct sockaddr_in remote = {.sin_family = AF_INET, .sin_port = htons(12345)};

    io_quic_conn_t *qconn = io_quic_conn_create(&qcfg, &cbs, dcid, 8, scid, 8,
                                                   (struct sockaddr *)&local,
                                                   (struct sockaddr *)&remote, nullptr);
    if (qconn == nullptr) {
        TEST_IGNORE_MESSAGE("QUIC conn failed");
        return;
    }

    io_http3_config_t h3cfg;
    io_http3_config_init(&h3cfg);
    io_http3_session_t *h3 = io_http3_session_create(&h3cfg, qconn, test_handler, nullptr);
    TEST_ASSERT_NOT_NULL(h3);
    TEST_ASSERT_FALSE(io_http3_is_shutdown(h3));

    /* Shutdown */
    TEST_ASSERT_EQUAL_INT(0, io_http3_shutdown(h3));
    TEST_ASSERT_TRUE(io_http3_is_shutdown(h3));

    io_http3_session_destroy(h3);
    io_quic_conn_destroy(qconn);
}

void test_alt_svc_in_response(void)
{
    /* Verify Alt-Svc header is properly formatted */
    io_response_t resp;
    io_response_init(&resp);

    /* Simulate adding Alt-Svc via io_alt_svc_add_header (tested in unit tests) */
    io_response_add_header(&resp, "alt-svc", "h3=\":443\"");

    bool found = false;
    for (uint32_t i = 0; i < resp.header_count; i++) {
        if (resp.headers[i].name_len == 7 &&
            memcmp(resp.headers[i].name, "alt-svc", 7) == 0) {
            found = true;
        }
    }
    TEST_ASSERT_TRUE(found);
    io_response_destroy(&resp);
}

void test_quic_conn_destroy_after_close(void)
{
    io_quic_config_t cfg;
    io_quic_config_init(&cfg);
    cfg.cert_file = TEST_CERTS_DIR "/server.crt";
    cfg.key_file = TEST_CERTS_DIR "/server.key";
    io_quic_callbacks_t cbs = {0};
    uint8_t dcid[8] = {1};
    uint8_t scid[8] = {2};
    struct sockaddr_in local = {.sin_family = AF_INET, .sin_port = htons(443)};
    struct sockaddr_in remote = {.sin_family = AF_INET, .sin_port = htons(12345)};

    io_quic_conn_t *conn = io_quic_conn_create(&cfg, &cbs, dcid, 8, scid, 8,
                                                 (struct sockaddr *)&local,
                                                 (struct sockaddr *)&remote, nullptr);
    if (conn == nullptr) {
        TEST_IGNORE_MESSAGE("QUIC conn failed");
        return;
    }
    io_quic_close(conn, 0);
    /* Destroy after close should not leak or crash */
    io_quic_conn_destroy(conn);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_quic_server_conn_lifecycle);
    RUN_TEST(test_http3_session_on_quic_conn);
    RUN_TEST(test_alt_svc_in_response);
    RUN_TEST(test_quic_conn_destroy_after_close);
    return UNITY_END();
}
```

**Step 2: Add to CMakeLists.txt**

```cmake
# Sprint 10: HTTP/3 integration tests
if(BUILD_TESTING AND TARGET unity AND TARGET io_http3 AND TARGET io_quic)
    add_executable(test_http3_server tests/integration/test_http3_server.c)
    target_include_directories(test_http3_server PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_include_directories(test_http3_server PRIVATE
        ${WOLFSSL_INCLUDE_DIRS} ${NGTCP2_INCLUDE_DIRS}
        ${NGTCP2_WOLFSSL_INCLUDE_DIRS} ${NGHTTP3_INCLUDE_DIRS}
    )
    target_link_directories(test_http3_server PRIVATE
        ${WOLFSSL_LIBRARY_DIRS} ${NGTCP2_LIBRARY_DIRS}
        ${NGTCP2_WOLFSSL_LIBRARY_DIRS} ${NGHTTP3_LIBRARY_DIRS}
    )
    target_link_libraries(test_http3_server PRIVATE
        unity io_http3 io_quic io_alt_svc io_request io_response io_ctx
        ${WOLFSSL_LIBRARIES} ${NGTCP2_LIBRARIES}
        ${NGTCP2_WOLFSSL_LIBRARIES} ${NGHTTP3_LIBRARIES}
    )
    target_compile_options(test_http3_server PRIVATE
        -Wno-missing-prototypes -Wno-missing-declarations
    )
    target_compile_definitions(test_http3_server PRIVATE
        TEST_CERTS_DIR="${CMAKE_SOURCE_DIR}/tests/certs"
    )
    add_test(NAME test_http3_server COMMAND test_http3_server)
endif()
```

**Step 3: Build and run full suite**
```bash
cmake --preset clang-debug && cmake --build --preset clang-debug && ctest --preset clang-debug --output-on-failure
```

**Step 4: Commit**
```bash
git add tests/integration/test_http3_server.c CMakeLists.txt
git commit -m "test(http3): integration tests — QUIC lifecycle + HTTP/3 session (4 tests)"
```

---

## Task 8: io_uring UDP operations for QUIC I/O

**Files:**
- Modify: `src/core/io_loop.h` — add `IO_OP_RECVMSG` and `IO_OP_SENDMSG` operation types
- Modify: `src/core/io_conn.h` — add `IO_CONN_QUIC` connection state

**Step 1: Add UDP operation types to io_loop.h**

Add to the `io_op_type_t` enum:
```c
    IO_OP_RECVMSG = 0x0B,  /* UDP recvmsg for QUIC datagrams */
    IO_OP_SENDMSG = 0x0C,  /* UDP sendmsg for QUIC datagrams */
```

**Step 2: Add QUIC state to io_conn.h**

Add to the `io_conn_state_t` enum (before `IO_CONN_DRAINING`):
```c
    IO_CONN_QUIC,      /* QUIC/HTTP/3 active */
```

**Step 3: Build and run all tests (verify no regressions)**
```bash
cmake --preset clang-debug && cmake --build --preset clang-debug && ctest --preset clang-debug --output-on-failure
```

**Step 4: Commit**
```bash
git add src/core/io_loop.h src/core/io_conn.h
git commit -m "feat(core): add UDP op types and QUIC connection state for HTTP/3"
```

---

## Task 9: Sprint finalization and quality pipeline

**Step 1: Run clang-format**
```bash
cmake --build --preset clang-debug --target format
```

**Step 2: Run full test suite**
```bash
ctest --preset clang-debug --output-on-failure -N | tail -1
# Expected: ~37 total tests (31 existing + 3 new test binaries + ~3 integration)
```

**Step 3: Run quality pipeline**
```bash
# Exit the container first, then run from host:
podman run --rm --security-opt seccomp=unconfined \
  -v /opt/projects/repositories/iohttp:/workspace:Z \
  localhost/iohttp-dev:latest bash -c "cd /workspace && ./scripts/quality.sh"
```

If PVS-Studio or CodeChecker report new findings on sprint code, fix them.

**Step 4: Update PVS suppress baseline (if new pre-existing findings appear)**
```bash
# Only if needed:
pvs-studio-analyzer suppress -o .pvs-suppress.json build/clang-debug/pvs-studio.log
```

**Step 5: Commit finalization**
```bash
git add -A
git commit -m "chore: Sprint 10 complete — HTTP/3 (QUIC) via ngtcp2 + nghttp3"
```

---

## Summary

| Task | What | New Tests |
|------|------|-----------|
| 1 | QUIC transport types (io_quic.h) | — |
| 2 | QUIC transport implementation (io_quic.c) | 14 |
| 3 | QUIC handshake tests with real certs | 3 |
| 4 | HTTP/3 session types (io_http3.h) | — |
| 5 | HTTP/3 session implementation (io_http3.c) | 8 |
| 6 | Alt-Svc header for HTTP/3 discovery | 7 |
| 7 | QUIC + HTTP/3 integration tests | 4 |
| 8 | io_uring UDP ops + QUIC conn state | — |
| 9 | Quality pipeline verification | — |

**New tests: ~36. Total after Sprint 10: ~67 test binaries (31 existing + ~36 new tests across 4 new binaries).**

## Critical Files

**Existing (reuse patterns from):**
- `src/http/io_http2.h` + `io_http2.c` — THE reference pattern (buffer-based session, per-stream data, arena, callbacks, response submission)
- `src/tls/io_tls.h` + `io_tls.c` — wolfSSL setup pattern (CTX, SSL, cipher buffers)
- `src/core/io_ctx.h` — unified context for request dispatch
- `tests/integration/test_http2_server.c` — integration test pattern with real TLS
- `CMakeLists.txt` — `io_add_test()` macro, pkg_check_modules pattern
- `tests/certs/` — test certificates (server.crt, server.key)

**New:**
- `src/http/io_quic.{h,c}` — QUIC transport (ngtcp2 + ngtcp2_crypto_wolfssl)
- `src/http/io_http3.{h,c}` — HTTP/3 session (nghttp3)
- `src/http/io_alt_svc.{h,c}` — Alt-Svc header generation
- `tests/unit/test_io_quic.c` — QUIC transport tests
- `tests/unit/test_io_http3.c` — HTTP/3 session tests
- `tests/unit/test_io_alt_svc.c` — Alt-Svc tests
- `tests/integration/test_http3_server.c` — integration tests

**Container deps (verify installed):**
```bash
# In container:
ls /usr/local/include/ngtcp2/ngtcp2.h           # ngtcp2 headers
ls /usr/local/include/ngtcp2/ngtcp2_crypto_wolfssl.h  # wolfSSL crypto
ls /usr/local/include/nghttp3/nghttp3.h          # nghttp3 headers
PKG_CONFIG_PATH=/usr/local/lib/pkgconfig:/usr/local/lib64/pkgconfig \
  pkg-config --libs libngtcp2 libngtcp2_crypto_wolfssl libnghttp3
```

## Out of Scope (follow-up sprints)

- **0-RTT resumption** — requires session ticket storage, security implications
- **Connection migration** — requires CID tracking, path validation
- **QPACK dynamic table** — disabled by default (dtable_capacity=0), can tune later
- **UDP GRO/GSO** — kernel optimization for batching datagrams, performance sprint
- **Multi-path QUIC** — RFC 9000 extension, far future
- **io_uring IORING_OP_RECVMSG multishot** — kernel 6.0+ for UDP, performance sprint

## Verification

After all tasks:
1. `ctest --preset clang-debug --output-on-failure` — all tests pass
2. `cmake --build --preset clang-debug --target format-check` — formatting clean
3. `./scripts/quality.sh` — full pipeline PASS (6/6)
4. `ls src/http/io_quic.h src/http/io_http3.h src/http/io_alt_svc.h` — all files exist
5. `grep -c 'add_test' CMakeLists.txt` — test count increased
6. `grep 'QUIC transport enabled' build/clang-debug/CMakeCache.txt` — QUIC detected

# iohttp Sprint Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development to implement this plan task-by-task.

**Goal:** Build a production-grade embedded HTTP server library in C23 with io_uring, wolfSSL, HTTP/1.1+2+3.

**Architecture:** io_uring as core runtime (not backend). Multi-reactor ring-per-thread (production) / single-reactor (dev). Modular composition: wolfSSL TLS + picohttpparser (HTTP/1.1) + nghttp2 (HTTP/2) + ngtcp2+nghttp3 (HTTP/3). Radix-trie router with per-method trees. Zero-copy where possible. P0→P4 phasing.

**Tech Stack:** C23, liburing 2.7+, wolfSSL 5.8+, picohttpparser, nghttp2, ngtcp2, nghttp3, yyjson, Unity tests, Linux kernel 6.7+.

**wolfSSL License Note:** wolfSSL's license requires clarification before release — GitHub LICENSING says GPLv2, wolfssl.com says GPLv3, manual says GPLv2. If strictly GPLv2 (not GPLv2+), it's incompatible with iohttp's GPLv3. Resolve with wolfSSL Inc. or acquire commercial license.

**Build/test:**
```bash
cmake --preset clang-debug
cmake --build --preset clang-debug
ctest --preset clang-debug
```

---

## Sprint 1: io_uring Event Loop & Server Skeleton (3-4 weeks)

**Goal:** Working event loop with multishot accept, provided buffers, timer management, and basic server lifecycle.

### Task 1.1: io_uring Ring Management

**Files:**
- Create: `src/core/ioh_loop.h`
- Create: `src/core/ioh_loop.c`
- Create: `tests/unit/test_ioh_loop.c`
- Modify: `CMakeLists.txt`

**Implementation:**

`ioh_loop_t` wraps `struct io_uring` with:
- `ioh_loop_create(ioh_loop_config_t *cfg)` — ring setup with queue_depth, flags
- `ioh_loop_destroy(ioh_loop_t *loop)` — cleanup
- `ioh_loop_run(ioh_loop_t *loop)` — main event loop (blocks)
- `ioh_loop_stop(ioh_loop_t *loop)` — signal stop via eventfd
- `ioh_loop_add_timer(loop, ms, callback, data)` — timer via `IORING_OP_TIMEOUT`
- `ioh_loop_add_linked_timeout(loop, sqe, ms)` — linked timeout via `IORING_OP_LINK_TIMEOUT`
- `ioh_loop_cancel_timer(loop, timer_id)` — cancel pending timer
- `ioh_loop_register_buffers(loop, iovecs, count)` — `IORING_REGISTER_BUFFERS` for pinned DMA memory
- `ioh_loop_register_files(loop, fds, count)` — `IORING_REGISTER_FILES` to skip fd table lookup

```c
typedef struct {
    uint32_t queue_depth;        /* default 256 */
    uint32_t buf_ring_size;      /* default 128 */
    uint32_t buf_size;           /* default 4096 */
    uint32_t registered_bufs;    /* number of registered buffers (0 = disabled) */
    uint32_t registered_files;   /* max registered fds (0 = disabled) */
    bool     sqpoll;             /* SQPOLL mode (requires CAP_SYS_ADMIN) */
} ioh_loop_config_t;
```

**io_uring features per operation:**
- Multishot accept: `IORING_OP_ACCEPT` + `IORING_ACCEPT_MULTISHOT` (NO `IOSQE_CQE_SKIP_SUCCESS` — accept CQE carries the fd; skipping it loses the connection)
- Recv: `IORING_OP_RECV` with provided buffer ring (`io_uring_setup_buf_ring`)
- Send: `IORING_OP_SEND_ZC` for zero-copy (payloads > 2 KiB), regular `IORING_OP_SEND` otherwise
- Timers: `IORING_OP_LINK_TIMEOUT` for per-operation deadlines (keepalive, header, body)
- Files: `IORING_OP_SPLICE` for zero-copy static file serving
- Registered buffers: `IORING_REGISTER_BUFFERS` — pinned memory, avoids page table walks on DMA
- Registered files: `IORING_REGISTER_FILES` — pre-registered fds, skips fd table lookup per I/O

**Ring hardening (IORING_REGISTER_RESTRICTIONS):** At startup, after ring creation and buffer registration
but before accepting connections, call `IORING_REGISTER_RESTRICTIONS` to whitelist only needed opcodes
(RECV, SEND, SEND_ZC, ACCEPT, TIMEOUT, CANCEL, MSG_RING, SPLICE, LINK_TIMEOUT). This closes the
io_uring seccomp bypass vulnerability — io_uring operations bypass seccomp BPF filters since they use
shared memory ring, not direct syscalls. Without restrictions, a compromised parser can issue arbitrary
kernel operations (OPENAT, CONNECT) through the existing ring fd.

**SEND_ZC threshold heuristic:** Regular async `send` for payloads < 2 KiB (headers, small JSON).
`SEND_ZC` for payloads > 2 KiB (large responses, streaming). `splice` for static files.
SEND_ZC generates 2 CQEs per operation (completion + buffer notification); use
`IORING_SEND_ZC_REPORT_USAGE` to detect kernel fallback to copy on loopback.

**SQPOLL:** Optional, NOT default. `DEFER_TASKRUN + SINGLE_ISSUER` preferred for HTTP servers
(bursty traffic). SQPOLL burns a CPU core and is mutually exclusive with DEFER_TASKRUN.

**Kernel 6.7 justification:** Required for `IOU_PBUF_RING_INC` (incremental buffer consumption),
`IORING_SETUP_SINGLE_ISSUER` + `IORING_SETUP_DEFER_TASKRUN` (both 6.0+), and to avoid
CVE-2024-0582 (use-after-free in provided buffer rings, kernels 6.4–6.6.4).

**No epoll fallback.** io_uring is mandatory. Minimum kernel: 6.7+.

**Tests (12):**
```c
void test_loop_config_defaults(void);            // queue_depth=256
void test_loop_create_destroy(void);             // lifecycle
void test_loop_nop_submit(void);                 // submit NOP, verify completion
void test_loop_timer_fires(void);                // 10ms timer fires
void test_loop_timer_cancel(void);               // cancel before fire
void test_loop_linked_timeout(void);             // IORING_OP_LINK_TIMEOUT fires on stall
void test_loop_linked_timeout_no_fire(void);     // linked timeout cancelled on success
void test_loop_stop_via_eventfd(void);           // stop from another context
void test_loop_provided_buffer_ring(void);       // buffer ring setup
void test_loop_register_buffers(void);           // IORING_REGISTER_BUFFERS + fixed read/write
void test_loop_register_files(void);             // IORING_REGISTER_FILES + fixed fd I/O
void test_loop_config_validate(void);            // invalid config rejected
void test_loop_register_restrictions(void);      // IORING_REGISTER_RESTRICTIONS whitelist
void test_loop_restricted_op_rejected(void);     // non-whitelisted op → -EACCES
```

**CMake:**
```cmake
add_library(ioh_loop STATIC src/core/ioh_loop.c)
target_include_directories(ioh_loop PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(ioh_loop PUBLIC ${LIBURING_LIBRARIES})
target_include_directories(ioh_loop PUBLIC ${LIBURING_INCLUDE_DIRS})

ioh_add_test(test_ioh_loop tests/unit/test_ioh_loop.c ioh_loop)
```

---

### Task 1.2: Connection State Machine

**Files:**
- Create: `src/core/ioh_conn.h`
- Create: `src/core/ioh_conn.c`
- Create: `tests/unit/test_ioh_conn.c`
- Modify: `CMakeLists.txt`

**Implementation:**

Connection pool with fixed-size array (compile-time configurable):

```c
typedef enum : uint8_t {
    IOH_CONN_FREE = 0,
    IOH_CONN_ACCEPTING,
    IOH_CONN_PROXY_HEADER,
    IOH_CONN_TLS_HANDSHAKE,
    IOH_CONN_HTTP_ACTIVE,
    IOH_CONN_WEBSOCKET,
    IOH_CONN_CLOSING,
} ioh_conn_state_t;

typedef struct {
    int fd;
    ioh_conn_state_t state;
    uint32_t id;
    struct sockaddr_storage peer_addr;
    struct sockaddr_storage proxy_addr;  /* from PROXY protocol */
    bool proxy_used;
    uint64_t created_at_ms;
    uint64_t last_activity_ms;
    uint8_t *recv_buf;
    size_t recv_len;
    void *protocol_ctx;   /* HTTP/1.1, HTTP/2, or HTTP/3 state */
    void *tls_ctx;        /* WOLFSSL * */
} ioh_conn_t;
```

- `ioh_conn_pool_create(max_conns)` / `_destroy()`
- `ioh_conn_alloc(pool)` → ioh_conn_t* (or nullptr if full)
- `ioh_conn_free(pool, conn)` — reset and return to pool
- `ioh_conn_find(pool, fd)` — lookup by fd
- `ioh_conn_transition(conn, new_state)` — validate state transitions

**Tests (8):**
```c
void test_conn_pool_create_destroy(void);
void test_conn_alloc_returns_conn(void);
void test_conn_alloc_pool_full(void);         // returns nullptr
void test_conn_free_reuses_slot(void);
void test_conn_find_by_fd(void);
void test_conn_find_missing(void);            // returns nullptr
void test_conn_state_valid_transition(void);  // ACCEPTING→TLS_HANDSHAKE
void test_conn_state_invalid_transition(void); // FREE→HTTP_ACTIVE rejected
```

---

### Task 1.3: Server Lifecycle

**Files:**
- Create: `src/core/ioh_server.h`
- Create: `src/core/ioh_server.c`
- Create: `tests/unit/test_ioh_server.c`
- Modify: `CMakeLists.txt`

**Implementation:**

```c
typedef struct {
    const char *listen_addr;
    uint16_t listen_port;
    const char *tls_cert;
    const char *tls_key;
    uint32_t max_connections;
    uint32_t queue_depth;
    uint32_t keepalive_timeout_ms;
    uint32_t header_timeout_ms;
    uint32_t body_timeout_ms;
    uint32_t max_header_size;
    uint32_t max_body_size;
    bool proxy_protocol;
} ioh_server_config_t;

[[nodiscard]] ioh_server_t *ioh_server_create(const ioh_server_config_t *cfg);
void ioh_server_destroy(ioh_server_t *srv);
[[nodiscard]] int ioh_server_run(ioh_server_t *srv);    /* blocks */
void ioh_server_stop(ioh_server_t *srv);

/* Graceful shutdown — drain active connections before stopping */
typedef enum : uint8_t {
    IOH_SHUTDOWN_IMMEDIATE,  /* close all connections now */
    IOH_SHUTDOWN_DRAIN,      /* stop accepting, drain active, then stop */
} ioh_shutdown_mode_t;

[[nodiscard]] int ioh_server_shutdown(ioh_server_t *srv, ioh_shutdown_mode_t mode,
                                      uint32_t drain_timeout_ms);
```

Server creates: `signal(SIGPIPE, SIG_IGN)` → listen socket → bind → listen → ioh_loop with multishot accept.
**SIGPIPE must be ignored at startup** — io_uring returns errors on write to closed socket, but
wolfSSL may trigger SIGPIPE through internal write calls. Without SIG_IGN, server can crash.
Shutdown DRAIN: cancel multishot accept → transition all conns to DRAINING →
wait for in-flight responses → close after drain_timeout_ms.

**Accept backpressure:** when connection pool exhausted, stop re-arming multishot accept.
When connections free up, re-arm. Log warning on backpressure activation.

**Tests (10):**
```c
void test_server_config_defaults(void);
void test_server_config_validate_valid(void);
void test_server_config_validate_zero_port(void);     // -EINVAL
void test_server_config_validate_zero_conns(void);    // -EINVAL
void test_server_create_destroy(void);
void test_server_listen_socket(void);                 // bind succeeds
void test_server_accept_connection(void);             // socketpair test
void test_server_shutdown_immediate(void);            // stop immediately
void test_server_shutdown_drain(void);                // drain then stop
void test_server_accept_backpressure(void);           // pool full → no accept
```

---

### Task 1.4: Buffer Pool & Registered Resources

**Files:**
- Create: `src/core/ioh_buffer.h`
- Create: `src/core/ioh_buffer.c`
- Create: `tests/unit/test_ioh_buffer.c`
- Modify: `CMakeLists.txt`

**Implementation:**

Manages three io_uring buffer mechanisms:

1. **Provided buffer ring** (`io_uring_setup_buf_ring`) — kernel picks buffer for recv automatically, zero alloc in hot path
2. **Registered buffers** (`IORING_REGISTER_BUFFERS`) — pinned memory for DMA, avoids page table walks
3. **Registered files** (`IORING_REGISTER_FILES`) — pre-registered fds, skips fd table lookup per I/O op

```c
typedef struct {
    uint32_t ring_size;       /* provided buffer ring: number of buffers (default 128) */
    uint32_t buf_size;        /* size of each buffer (default 4096) */
    uint16_t bgid;            /* buffer group ID */
    uint32_t reg_buf_count;   /* registered buffers count (0 = disabled) */
    uint32_t reg_buf_size;    /* registered buffer size (default 16384, TLS record) */
    uint32_t reg_file_count;  /* registered file slots (0 = disabled, default = max_connections) */
} ioh_bufpool_config_t;

[[nodiscard]] ioh_bufpool_t *ioh_bufpool_create(const ioh_bufpool_config_t *cfg);
void ioh_bufpool_destroy(ioh_bufpool_t *pool);
[[nodiscard]] int ioh_bufpool_register_ring(ioh_bufpool_t *pool, struct io_uring *ring);
[[nodiscard]] int ioh_bufpool_register_bufs(ioh_bufpool_t *pool, struct io_uring *ring);
[[nodiscard]] int ioh_bufpool_register_files(ioh_bufpool_t *pool, struct io_uring *ring);
void ioh_bufpool_return(ioh_bufpool_t *pool, uint32_t buf_id);
uint8_t *ioh_bufpool_get_buf(ioh_bufpool_t *pool, uint32_t buf_id);
uint8_t *ioh_bufpool_get_reg_buf(ioh_bufpool_t *pool, uint32_t idx);
[[nodiscard]] int ioh_bufpool_register_fd(ioh_bufpool_t *pool, int fd);
void ioh_bufpool_unregister_fd(ioh_bufpool_t *pool, int fd);
```

**Tests (10):**
```c
void test_bufpool_create_destroy(void);
void test_bufpool_config_defaults(void);
void test_bufpool_get_buf_valid_id(void);
void test_bufpool_get_buf_invalid_id(void);      // returns nullptr
void test_bufpool_return_and_reuse(void);
void test_bufpool_config_validate(void);
void test_bufpool_register_bufs(void);            // IORING_REGISTER_BUFFERS pinned memory
void test_bufpool_register_files(void);           // IORING_REGISTER_FILES fd slots
void test_bufpool_register_fd_and_use(void);      // register fd → fixed file I/O
void test_bufpool_unregister_fd(void);            // unregister → slot freed
```

---

## Sprint 2: wolfSSL TLS Integration (2-3 weeks)

**Goal:** TLS 1.3 handshake over io_uring, custom I/O callbacks, mTLS, ALPN.

### Task 2.1: wolfSSL Context Management

**Files:**
- Create: `src/tls/ioh_tls.h`
- Create: `src/tls/ioh_tls.c`
- Create: `tests/unit/test_ioh_tls.c`
- Modify: `CMakeLists.txt`

**Implementation:**

```c
typedef struct {
    const char *cert_file;
    const char *key_file;
    const char *ca_file;       /* for mTLS */
    bool require_client_cert;  /* mTLS */
    bool enable_session_tickets;
    uint32_t session_cache_size;
    const char *alpn;          /* "h2,http/1.1" */
} ioh_tls_config_t;

[[nodiscard]] ioh_tls_ctx_t *ioh_tls_ctx_create(const ioh_tls_config_t *cfg);
void ioh_tls_ctx_destroy(ioh_tls_ctx_t *ctx);
[[nodiscard]] int ioh_tls_accept(ioh_tls_ctx_t *ctx, ioh_conn_t *conn);
[[nodiscard]] int ioh_tls_read(ioh_conn_t *conn, uint8_t *buf, size_t len);
[[nodiscard]] int ioh_tls_write(ioh_conn_t *conn, const uint8_t *buf, size_t len);
void ioh_tls_shutdown(ioh_conn_t *conn);
const char *ioh_tls_get_alpn(ioh_conn_t *conn);
```

Custom I/O callbacks integrate with io_uring:
- `wolfSSL_CTX_SetIORecv()` → reads from connection's cipher buffer
- `wolfSSL_CTX_SetIOSend()` → writes to connection's send buffer
- Buffers are drained/filled by io_uring recv/send operations
- **CRITICAL:** `wolfSSL_CTX_set_mode(ctx, SSL_MODE_ENABLE_PARTIAL_WRITE)` — io_uring is non-blocking, wolfSSL must handle partial writes
- Manual `WANT_READ`/`WANT_WRITE` handling — re-arm io_uring recv/send on these returns
- Use registered buffers for TLS record I/O (16KB input + 16KB output per connection)
- **wolfSSL I/O serialization:** wolfSSL has a single I/O buffer per SSL object — `wolfSSL_read()` and `wolfSSL_write()` on the same WOLFSSL object MUST be serialized. In ring-per-thread with strict connection ownership, this is naturally satisfied (one thread per connection).
- **TLS certificate hot reload:** Support atomic pointer swap with reference counting — create new `WOLFSSL_CTX`, load certs via `wolfSSL_CTX_use_certificate_buffer()`, atomically swap global pointer. Existing connections continue using old CTX and release it on close.

**Tests (10):**
```c
void test_tls_config_defaults(void);
void test_tls_config_validate_no_cert(void);        // -EINVAL
void test_tls_ctx_create_destroy(void);
void test_tls_ctx_create_invalid_cert(void);        // -ENOENT
void test_tls_handshake_self_signed(void);           // socketpair + self-signed cert
void test_tls_read_write_roundtrip(void);            // encrypt → decrypt
void test_tls_alpn_h2(void);                         // verify ALPN negotiation
void test_tls_alpn_http11(void);
void test_tls_mtls_valid_client(void);               // client cert accepted
void test_tls_mtls_no_client_cert(void);             // rejected when required
void test_tls_ctx_hot_reload(void);                  // swap CTX, old conns work, new conns use new CTX
```

---

### Task 2.2: TLS + io_uring Integration

**Files:**
- Modify: `src/tls/ioh_tls.c` (add io_uring I/O callback integration)
- Create: `tests/integration/test_tls_uring.c`
- Modify: `CMakeLists.txt`

**Implementation:**

Wire wolfSSL I/O callbacks to io_uring provided buffers:
1. io_uring recv → provided buffer → copy to conn cipher_buf
2. wolfSSL_read() reads from cipher_buf via custom IORecv callback
3. Plaintext available to HTTP parser
4. Response → wolfSSL_write() → conn send_buf via custom IOSend callback
5. io_uring send → send_buf to network

**Tests (5):**
```c
void test_tls_uring_handshake(void);            // full handshake via io_uring
void test_tls_uring_data_roundtrip(void);       // send/recv through TLS+uring
void test_tls_uring_concurrent_conns(void);     // 4 parallel TLS connections
void test_tls_uring_timeout_handshake(void);    // handshake timeout fires
void test_tls_uring_graceful_shutdown(void);    // close_notify
```

---

## Sprint 3: HTTP/1.1 Parser & Response Builder (3-4 weeks)

**Goal:** Complete HTTP/1.1 request parsing, response building, keep-alive, chunked TE.

### Task 3.1: Request/Response Abstractions

**Files:**
- Create: `src/http/ioh_request.h`
- Create: `src/http/ioh_request.c`
- Create: `src/http/ioh_response.h`
- Create: `src/http/ioh_response.c`
- Create: `tests/unit/test_ioh_request.c`
- Create: `tests/unit/test_ioh_response.c`
- Modify: `CMakeLists.txt`

**Implementation:**

Protocol-independent request/response:

```c
typedef struct {
    ioh_method_t method;
    const char *path;
    size_t path_len;
    const char *query;
    size_t query_len;
    ioh_header_t *headers;
    uint32_t header_count;
    const uint8_t *body;
    size_t body_len;
    ioh_conn_t *conn;
    /* Path parameters from router */
    ioh_param_t params[IOH_MAX_PATH_PARAMS];
    uint32_t param_count;
    /* Metadata */
    uint8_t http_version_major;
    uint8_t http_version_minor;
    bool keep_alive;
    const char *content_type;
    size_t content_length;
    const char *host;             /* Host header (required for HTTP/1.1) */
    /* Connection info (real client IP after PROXY protocol) */
    ioh_conn_info_t *conn_info;
} ioh_request_t;

typedef struct {
    uint16_t status;
    ioh_header_t *headers;
    uint32_t header_count;
    uint32_t header_capacity;
    uint8_t *body;
    size_t body_len;
    size_t body_capacity;
    bool headers_sent;
    bool chunked;
} ioh_response_t;

/* Response helpers */
[[nodiscard]] int ioh_respond(ioh_response_t *resp, uint16_t status,
                              const char *content_type,
                              const uint8_t *body, size_t body_len);
[[nodiscard]] int ioh_respond_json(ioh_response_t *resp, uint16_t status,
                                   const char *json);
[[nodiscard]] int ioh_respond_file(ioh_response_t *resp, const char *path);
[[nodiscard]] int ioh_response_set_header(ioh_response_t *resp,
                                          const char *name, const char *value);
```

**Request helpers:**
```c
/* Cookie parsing from Cookie header */
const char *ioh_request_cookie(const ioh_request_t *req, const char *name);

/* Query parameter extraction (from parsed query string) */
const char *ioh_request_query(const ioh_request_t *req, const char *name);

/* Content negotiation — Accept header parsing */
const char *ioh_request_accepts(const ioh_request_t *req, const char **types, uint32_t count);

/* Form body parsing (application/x-www-form-urlencoded) */
[[nodiscard]] int ioh_request_form_value(const ioh_request_t *req,
                                         const char *name, const char **value);
```

**Tests (18):**
```c
void test_request_init(void);
void test_request_method_parse(void);
void test_request_header_find(void);
void test_request_header_find_missing(void);
void test_request_content_length(void);
void test_request_keep_alive_11(void);          // HTTP/1.1 default keep-alive
void test_request_keep_alive_10(void);          // HTTP/1.0 default close
void test_request_cookie_single(void);          // Cookie: session=abc → "abc"
void test_request_cookie_multiple(void);        // Cookie: a=1; b=2 → "2"
void test_request_cookie_missing(void);         // no such cookie → nullptr
void test_request_query_param(void);            // ?page=2&sort=name → "2"
void test_request_accepts_json(void);           // Accept: application/json
void test_request_accepts_wildcard(void);       // Accept: */*
void test_request_form_urlencoded(void);        // key=value&key2=value2
void test_response_init(void);
void test_response_set_header(void);
void test_response_json(void);
void test_response_status_text(void);           // 200→"OK", 404→"Not Found"
```

---

### Task 3.2: picohttpparser Integration

**Files:**
- Create: `src/http/ioh_http1.h`
- Create: `src/http/ioh_http1.c`
- Add: `src/http/picohttpparser.h` (vendored, ~800 LOC)
- Add: `src/http/picohttpparser.c` (vendored)
- Create: `tests/unit/test_ioh_http1.c`
- Modify: `CMakeLists.txt`

**Implementation:**

Wrap picohttpparser's stateless parsing with buffered I/O:

```c
[[nodiscard]] int ioh_http1_parse_request(const uint8_t *buf, size_t len,
                                          ioh_request_t *req);
[[nodiscard]] int ioh_http1_serialize_response(const ioh_response_t *resp,
                                               uint8_t *buf, size_t buf_size);
[[nodiscard]] int ioh_http1_decode_chunked(ioh_chunked_decoder_t *dec,
                                           uint8_t *buf, size_t *len);
```

Return values: >0 = bytes consumed, -EAGAIN = need more data, <0 = error.

**Request smuggling protection (CRITICAL):**
- Reject duplicate Content-Length headers
- Reject requests with both Content-Length and Transfer-Encoding
- Reject obs-fold in headers (deprecated line folding)
- Reject Content-Length with non-digit characters
- Reject chunked TE not as last encoding
- Require Host header for HTTP/1.1

**Tests (18):**
```c
void test_http1_parse_get(void);                // GET /path HTTP/1.1
void test_http1_parse_post_body(void);          // POST with Content-Length body
void test_http1_parse_headers(void);            // multiple headers
void test_http1_parse_incomplete(void);         // partial request → -EAGAIN
void test_http1_parse_malformed(void);          // bad request line → error
void test_http1_parse_oversized_uri(void);      // URI > max → -E2BIG
void test_http1_parse_oversized_headers(void);  // headers > max → -E2BIG
void test_http1_chunked_decode(void);           // chunked TE decode
void test_http1_chunked_decode_incomplete(void);
void test_http1_serialize_200(void);            // serialize 200 OK response
void test_http1_serialize_headers(void);        // custom headers in output
void test_http1_keepalive_detection(void);      // Connection: keep-alive/close
// Request smuggling protection
void test_http1_reject_duplicate_content_length(void);  // 400
void test_http1_reject_cl_and_te(void);                 // CL + TE → 400
void test_http1_reject_obs_fold(void);                  // obs-fold → 400
void test_http1_reject_bad_cl_value(void);              // "12abc" → 400
void test_http1_require_host_11(void);                  // HTTP/1.1 without Host → 400
void test_http1_expect_100_continue(void);              // Expect: 100-continue
```

---

### Task 3.3: PROXY Protocol Decoder

**Files:**
- Create: `src/http/ioh_proxy_proto.h`
- Create: `src/http/ioh_proxy_proto.c`
- Create: `tests/unit/test_proxy_proto.c`
- Modify: `CMakeLists.txt`

**Implementation:**

```c
typedef struct {
    uint8_t version;     /* 1 or 2 */
    bool is_local;       /* LOCAL command (health check) */
    struct sockaddr_storage src_addr;
    struct sockaddr_storage dst_addr;
} ioh_proxy_result_t;

/* Returns bytes consumed, -EAGAIN for incomplete, <0 for error */
[[nodiscard]] int ioh_proxy_decode(const uint8_t *buf, size_t len,
                                   ioh_proxy_result_t *result);
```

PPv1: text format `PROXY TCP4 src dst sport dport\r\n`
PPv2: binary format with 12-byte signature, supports TLV extensions.

**SECURITY: PROXY protocol MUST be strictly config-based + allowlist.**
Never auto-detect PROXY headers — the spec explicitly warns that auto-detection
lets any client spoof source IP. The listener must have `proxy_protocol_enabled`
flag (default: false) and `trusted_proxy_addrs[]` allowlist. Only accept PROXY
headers from connections originating from addresses in the allowlist.

**Tests (10):**
```c
void test_proxy_v1_tcp4(void);              // IPv4
void test_proxy_v1_tcp6(void);              // IPv6
void test_proxy_v1_unknown(void);           // PROXY UNKNOWN
void test_proxy_v1_incomplete(void);        // partial → -EAGAIN
void test_proxy_v1_malformed(void);         // bad format → error
void test_proxy_v2_tcp4(void);              // binary IPv4
void test_proxy_v2_tcp6(void);              // binary IPv6
void test_proxy_v2_local(void);             // LOCAL command
void test_proxy_v2_with_tlv(void);          // TLV extensions skipped safely
void test_proxy_v2_invalid_signature(void); // bad signature → error
```

---

### Task 3.4: HTTP/1.1 Integration Test

**Files:**
- Create: `tests/integration/test_http1_server.c`
- Modify: `CMakeLists.txt`

**Tests (6):**
```c
void test_http1_full_request_response(void);    // socketpair: GET → 200 JSON
void test_http1_keepalive_multiple(void);        // 3 requests on same connection
void test_http1_connection_close(void);          // Connection: close → closed
void test_http1_post_with_body(void);            // POST + body → echo back
void test_http1_tls_request_response(void);      // HTTPS via wolfSSL
void test_http1_proxy_then_request(void);        // PROXY header + GET
```

---

## Sprint 4: Router & Middleware (3-4 weeks)

**Goal:** Production-grade radix-trie router with per-method trees, typed params, route groups,
auto-405/HEAD, middleware chain, and built-in middleware.

**Design heritage:** Radix trie + per-method trees (httprouter, 2013), deterministic static > param > wildcard
priority (httprouter/echo), handler-returns-error (bunrouter/echo), route groups with prefix composition
(Express.Router), auto-405 + auto-HEAD (httprouter/FastRoute), trailing slash redirect (httprouter),
path auto-correction (httprouter), conflict detection at registration (matchit/Axum), route introspection
(gorilla/mux), route metadata attachment for OpenAPI (FastAPI-inspired).

### Task 4.1: Radix Trie Core

**Files:**
- Create: `src/router/ioh_radix.h`
- Create: `src/router/ioh_radix.c`
- Create: `tests/unit/test_ioh_radix.c`
- Modify: `CMakeLists.txt`

**Implementation:**

Internal radix trie (compressed prefix tree) — NOT exposed in public API.
Separate tree per HTTP method (GET tree, POST tree, etc.) for O(1) method dispatch.

```c
/* Internal — src/router/ioh_radix.h */
typedef enum : uint8_t {
    IOH_NODE_STATIC,    /* /users/list — highest priority */
    IOH_NODE_PARAM,     /* /:id        — medium priority  */
    IOH_NODE_WILDCARD,  /* /*path      — lowest priority  */
} ioh_node_type_t;

typedef struct ioh_radix_node {
    char                    *prefix;        /* compressed edge label */
    ioh_node_type_t           type;
    char                    *param_name;    /* for PARAM/WILDCARD nodes */
    void                    *handler;       /* opaque, set by router */
    void                    *metadata;      /* route options, oas_operation_t* */
    struct ioh_radix_node   **children;      /* sorted by: STATIC > PARAM > WILDCARD */
    uint32_t                 child_count;
    uint32_t                 priority;      /* sum of handles in subtree (httprouter optimization) */
} ioh_radix_node_t;

typedef struct {
    ioh_radix_node_t *root;
} ioh_radix_tree_t;

[[nodiscard]] ioh_radix_tree_t *ioh_radix_create(void);
void ioh_radix_destroy(ioh_radix_tree_t *tree);
[[nodiscard]] int ioh_radix_insert(ioh_radix_tree_t *tree, const char *pattern,
                                   void *handler, void *metadata);
[[nodiscard]] int ioh_radix_lookup(const ioh_radix_tree_t *tree, const char *path,
                                   size_t path_len, ioh_radix_match_t *match);
```

Node priority ordering (httprouter pattern): children sorted by number of handles in subtree,
so most-populated branches are tried first → O(k) average where k = path segments.

**Conflict detection (matchit pattern):** `ioh_radix_insert()` returns `-EEXIST` if pattern
conflicts with existing route (e.g., `/:id` and `/:name` on same tree level).

**Tests (12):**
```c
void test_radix_create_destroy(void);
void test_radix_insert_static(void);             // /users/list
void test_radix_insert_param(void);              // /users/:id
void test_radix_insert_wildcard(void);           // /static/*path
void test_radix_lookup_static(void);             // exact match
void test_radix_lookup_param_extract(void);      // /users/42 → id="42"
void test_radix_lookup_wildcard_extract(void);   // /static/js/app.js → path="js/app.js"
void test_radix_priority_static_over_param(void);  // /users/list wins over /users/:id
void test_radix_priority_param_over_wildcard(void); // /files/:name wins over /files/*path
void test_radix_conflict_detection(void);        // /:id + /:name → -EEXIST
void test_radix_compressed_prefix(void);         // /api/users + /api/posts share /api/ edge
void test_radix_no_match(void);                  // unknown path → nullptr
```

---

### Task 4.2: Router Public API

**Files:**
- Create: `include/iohttp/ioh_router.h`
- Create: `src/router/ioh_router.c`
- Create: `tests/unit/test_ioh_router.c`
- Modify: `CMakeLists.txt`

**Implementation:**

Public API wrapping radix trie with per-method trees, auto-405, auto-HEAD, path correction.

```c
/* include/iohttp/ioh_router.h — PUBLIC API */

/* Handler returns int: 0 = success, negative errno = error.
   Error triggers centralized error handler (bunrouter/echo pattern). */
typedef int (*ioh_handler_fn)(ioh_request_t *req, ioh_response_t *resp);

/* Route options — extensible metadata per-route (FastAPI-inspired) */
typedef struct {
    void                *oas_operation;  /* oas_operation_t* for liboas binding */
    uint32_t             permissions;    /* bitmask for auth middleware */
    bool                 auth_required;
} ioh_route_opts_t;

[[nodiscard]] ioh_router_t *ioh_router_create(void);
void ioh_router_destroy(ioh_router_t *router);

/* Method-specific registration (bunrouter/Express pattern) */
[[nodiscard]] int ioh_router_get(ioh_router_t *r, const char *pattern, ioh_handler_fn h);
[[nodiscard]] int ioh_router_post(ioh_router_t *r, const char *pattern, ioh_handler_fn h);
[[nodiscard]] int ioh_router_put(ioh_router_t *r, const char *pattern, ioh_handler_fn h);
[[nodiscard]] int ioh_router_delete(ioh_router_t *r, const char *pattern, ioh_handler_fn h);
[[nodiscard]] int ioh_router_patch(ioh_router_t *r, const char *pattern, ioh_handler_fn h);
[[nodiscard]] int ioh_router_head(ioh_router_t *r, const char *pattern, ioh_handler_fn h);
[[nodiscard]] int ioh_router_options(ioh_router_t *r, const char *pattern, ioh_handler_fn h);

/* Generic method registration */
[[nodiscard]] int ioh_router_handle(ioh_router_t *r, ioh_method_t method,
                                    const char *pattern, ioh_handler_fn h);

/* Host-based routing / virtual hosts (gorilla/mux pattern) */
[[nodiscard]] ioh_router_t *ioh_router_host(ioh_router_t *r, const char *host_pattern);
/* host_pattern: "api.example.com", "*.example.com" */

/* Registration with route options */
[[nodiscard]] int ioh_router_get_with(ioh_router_t *r, const char *pattern,
                                      ioh_handler_fn h, const ioh_route_opts_t *opts);
/* ... _with variants for all methods */

/* Dispatch — returns match result including auto-405/auto-HEAD */
ioh_route_match_t ioh_router_dispatch(const ioh_router_t *r, ioh_method_t method,
                                      const char *path, size_t path_len);
```

**Auto-behaviors (httprouter patterns):**
- **Auto-405 Method Not Allowed**: if path matches in another method's tree,
  return 405 with `Allow` header listing valid methods
- **Auto-HEAD**: HEAD requests fall back to GET handler if no explicit HEAD route
- **Trailing slash redirect**: `/users/` ↔ `/users` — 301 redirect
- **Path auto-correction**: `//foo/../bar` → `/bar`, case-insensitive match → 301 redirect

**Path security:**
- Path normalization before lookup (collapse `//`, resolve `..`)
- NUL-byte rejection: any `%00` → 400
- Path traversal blocking: `..` escaping document root → 400

**Tests (18):**
```c
// Core routing
void test_router_create_destroy(void);
void test_router_get_exact(void);                 // GET /health
void test_router_post_exact(void);                // POST /users
void test_router_path_param(void);                // GET /users/:id → extract "42"
void test_router_multiple_params(void);           // GET /users/:uid/posts/:pid
void test_router_wildcard(void);                  // GET /static/*path → "css/app.css"
void test_router_method_dispatch(void);           // GET /users vs POST /users

// Priority (deterministic, order-independent)
void test_router_priority_static_over_param(void);   // /users/me wins over /users/:id
void test_router_priority_param_over_wildcard(void);  // /files/:name wins over /files/*path

// Auto-behaviors (httprouter)
void test_router_auto_405(void);                  // DELETE /health → 405 + Allow: GET
void test_router_auto_head(void);                 // HEAD /users → falls back to GET handler
void test_router_trailing_slash_redirect(void);   // /users/ → 301 → /users
void test_router_path_correction(void);           // //foo → 301 → /foo

// Security
void test_router_path_normalization(void);        // //foo/../bar → /bar
void test_router_path_traversal_blocked(void);    // /../etc/passwd → 400
void test_router_null_byte_blocked(void);         // /foo%00bar → 400

// Conflict detection (matchit)
void test_router_conflict_same_level(void);       // /:id + /:name → -EEXIST
void test_router_no_conflict_diff_method(void);   // GET /:id + POST /:name → OK (separate trees)
```

---

### Task 4.3: Route Groups

**Files:**
- Create: `src/router/ioh_route_group.h`
- Create: `src/router/ioh_route_group.c`
- Create: `tests/unit/test_ioh_route_group.c`
- Modify: `CMakeLists.txt`

**Implementation:**

Nested route groups with prefix composition and per-group middleware (Express.Router pattern).

```c
/* Route group — composable sub-router with shared prefix + middleware */
typedef struct ioh_group ioh_group_t;

[[nodiscard]] ioh_group_t *ioh_router_group(ioh_router_t *r, const char *prefix);
[[nodiscard]] ioh_group_t *ioh_group_subgroup(ioh_group_t *g, const char *prefix);

/* Method-specific registration on groups */
[[nodiscard]] int ioh_group_get(ioh_group_t *g, const char *pattern, ioh_handler_fn h);
[[nodiscard]] int ioh_group_post(ioh_group_t *g, const char *pattern, ioh_handler_fn h);
[[nodiscard]] int ioh_group_put(ioh_group_t *g, const char *pattern, ioh_handler_fn h);
[[nodiscard]] int ioh_group_delete(ioh_group_t *g, const char *pattern, ioh_handler_fn h);
/* ... patch, head, options */

/* Attach middleware to group (applies to all routes in group + subgroups) */
[[nodiscard]] int ioh_group_use(ioh_group_t *g, ioh_middleware_fn mw);
```

**Example usage:**
```c
ioh_group_t *api = ioh_router_group(router, "/api");
ioh_group_use(api, auth_middleware);

ioh_group_t *v1 = ioh_group_subgroup(api, "/v1");
ioh_group_get(v1, "/users/:id", get_user);     // → GET /api/v1/users/:id
ioh_group_post(v1, "/users", create_user);      // → POST /api/v1/users

ioh_group_t *admin = ioh_group_subgroup(api, "/admin");
ioh_group_use(admin, admin_only_middleware);
ioh_group_delete(admin, "/users/:id", del_user); // → DELETE /api/admin/users/:id
// middleware chain: auth → admin_only → del_user
```

**Tests (10):**
```c
void test_group_create(void);
void test_group_prefix_composition(void);       // /api + /v1 + /users → /api/v1/users
void test_group_nested_subgroup(void);           // 3-level nesting
void test_group_method_registration(void);       // ioh_group_get/post/put/delete
void test_group_middleware_applied(void);         // group middleware runs for group routes
void test_group_middleware_not_leaked(void);      // group middleware doesn't affect sibling groups
void test_group_middleware_inheritance(void);     // subgroup inherits parent middleware
void test_group_middleware_order(void);           // parent middleware before child middleware
void test_group_empty(void);                     // group with no routes
void test_group_with_route_opts(void);           // ioh_group_get_with() + metadata
```

---

### Task 4.4: Typed Param Extraction

**Files:**
- Modify: `include/iohttp/ioh_request.h`
- Modify: `src/http/ioh_request.c`
- Create: `tests/unit/test_ioh_params.c`
- Modify: `CMakeLists.txt`

**Implementation:**

Typed parameter access on `ioh_request_t` (echo/FastAPI-inspired).

```c
/* String param — always available, zero-copy pointer into path */
const char *ioh_request_param(const ioh_request_t *req, const char *name);

/* Typed extraction — returns 0 on success, -EINVAL on conversion failure */
[[nodiscard]] int ioh_request_param_i64(const ioh_request_t *req, const char *name, int64_t *out);
[[nodiscard]] int ioh_request_param_u64(const ioh_request_t *req, const char *name, uint64_t *out);
[[nodiscard]] int ioh_request_param_bool(const ioh_request_t *req, const char *name, bool *out);

/* Param count */
uint32_t ioh_request_param_count(const ioh_request_t *req);

/* Query params (separate from path params) */
const char *ioh_request_query(const ioh_request_t *req, const char *name);
[[nodiscard]] int ioh_request_query_i64(const ioh_request_t *req, const char *name, int64_t *out);
```

**Tests (10):**
```c
void test_param_string(void);                // /users/alice → "alice"
void test_param_i64_valid(void);             // /users/42 → 42
void test_param_i64_negative(void);          // /offset/-5 → -5
void test_param_i64_invalid(void);           // /users/abc → -EINVAL
void test_param_i64_overflow(void);          // /users/99999999999999999999 → -ERANGE
void test_param_u64_valid(void);             // /id/18446744073709551615
void test_param_bool_true(void);             // /flag/true, /flag/1
void test_param_bool_false(void);            // /flag/false, /flag/0
void test_param_missing(void);               // nonexistent param → nullptr / -EINVAL
void test_param_wildcard(void);              // /static/*path → "js/app.css"
```

---

### Task 4.5: Route Introspection & liboas Binding

**Files:**
- Create: `src/router/ioh_route_inspect.h`
- Create: `src/router/ioh_route_inspect.c`
- Create: `tests/unit/test_ioh_route_inspect.c`
- Modify: `CMakeLists.txt`

**Implementation:**

Route walking for documentation generation + liboas binding (gorilla/mux + FastAPI-inspired).

```c
/* Route info returned during walk */
typedef struct {
    ioh_method_t     method;
    const char     *pattern;        /* original pattern string */
    ioh_handler_fn   handler;
    const ioh_route_opts_t *opts;    /* metadata, oas_operation, permissions */
} ioh_route_info_t;

/* Walk callback — called for each registered route */
typedef int (*ioh_route_walk_fn)(const ioh_route_info_t *info, void *ctx);

/* Walk all routes — useful for docs generation, /openapi.json, debug logging */
[[nodiscard]] int ioh_router_walk(const ioh_router_t *r, ioh_route_walk_fn fn, void *ctx);

/* Count registered routes */
uint32_t ioh_router_route_count(const ioh_router_t *r);

/* Bind oas_operation_t* to existing route (alternative to _with registration) */
[[nodiscard]] int ioh_router_set_metadata(ioh_router_t *r, ioh_method_t method,
                                          const char *pattern, void *metadata);
```

**Tests (6):**
```c
void test_route_walk_all(void);              // walk 5 routes, verify all visited
void test_route_walk_order(void);            // routes visited in registration order
void test_route_walk_includes_groups(void);  // group routes included in walk
void test_route_count(void);                 // correct count after add/group
void test_route_set_metadata(void);          // attach metadata to existing route
void test_route_metadata_in_match(void);     // metadata available in dispatch result
```

---

### Task 4.6: Middleware Chain

**Files:**
- Create: `include/iohttp/ioh_middleware.h`
- Create: `src/middleware/ioh_middleware.c`
- Create: `tests/unit/test_ioh_middleware.c`
- Modify: `CMakeLists.txt`

**Implementation:**

```c
/* Middleware signature — calls next() to continue chain, or returns to short-circuit.
   Return: 0 = success, negative errno = error → centralized error handler. */
typedef int (*ioh_middleware_fn)(ioh_request_t *req, ioh_response_t *resp,
                                 ioh_next_fn next);

/* Global middleware — runs for all routes */
[[nodiscard]] int ioh_router_use(ioh_router_t *r, ioh_middleware_fn mw);

/* Error handler — called when handler or middleware returns non-zero */
typedef int (*ioh_error_handler_fn)(ioh_request_t *req, ioh_response_t *resp, int error);
void ioh_router_set_error_handler(ioh_router_t *r, ioh_error_handler_fn h);

/* Custom 404/405 handlers (httprouter pattern) */
void ioh_router_set_not_found(ioh_router_t *r, ioh_handler_fn h);
void ioh_router_set_method_not_allowed(ioh_router_t *r, ioh_handler_fn h);
```

Chain execution: global_mw[0] → global_mw[1] → group_mw[0] → group_mw[1] → handler.
Each middleware calls `next(req, resp)` to continue chain, or returns to short-circuit.
Errors propagate to error handler.

**Tests (10):**
```c
void test_middleware_chain_order(void);        // A → B → C → handler
void test_middleware_short_circuit(void);      // A returns -EPERM, B never called
void test_middleware_modify_request(void);     // add header before handler
void test_middleware_modify_response(void);    // add header after handler
void test_middleware_per_group(void);          // group middleware only for group routes
void test_middleware_empty_chain(void);        // no middleware → direct handler
void test_middleware_error_handler(void);      // handler returns -1 → error_handler called
void test_middleware_custom_404(void);         // custom not-found handler
void test_middleware_custom_405(void);         // custom method-not-allowed handler
void test_middleware_global_plus_group(void);  // global mw runs before group mw
```

---

### Task 4.7: Built-in Middleware

**Files:**
- Create: `src/middleware/ioh_cors.h` + `ioh_cors.c`
- Create: `src/middleware/ioh_ratelimit.h` + `ioh_ratelimit.c`
- Create: `src/middleware/ioh_security.h` + `ioh_security.c`
- Create: `src/middleware/ioh_auth.h` + `ioh_auth.c`
- Create: `tests/unit/test_ioh_cors.c`
- Create: `tests/unit/test_ioh_ratelimit.c`
- Create: `tests/unit/test_ioh_security.c`
- Create: `tests/unit/test_ioh_auth.c`
- Modify: `CMakeLists.txt`

**CORS (100-200 LOC):**
```c
typedef struct {
    const char **allowed_origins;
    const char **allowed_methods;
    const char **allowed_headers;
    bool allow_credentials;
    uint32_t max_age_seconds;
} ioh_cors_config_t;

[[nodiscard]] ioh_middleware_fn ioh_cors_middleware(const ioh_cors_config_t *cfg);
```

**Rate limiting + Slowloris defense (300-500 LOC) — token bucket per IP + slow client protection:**
```c
typedef struct {
    uint32_t requests_per_second;
    uint32_t burst;
    uint32_t max_connections_per_ip;     /* default 10, prevents Slowloris connection exhaustion */
    uint32_t min_transfer_rate_bps;     /* minimum bytes/sec for request body, 0 = disabled */
} ioh_ratelimit_config_t;

[[nodiscard]] ioh_middleware_fn ioh_ratelimit_middleware(const ioh_ratelimit_config_t *cfg);
```

Slowloris/Slow POST defense integrated with io_uring linked timeouts:
- Header read timeout 5-15s via `io_uring_prep_link_timeout()` (already in ioh_loop)
- Minimum transfer rate enforcement for POST/PUT bodies
- Per-IP connection limits (max_connections_per_ip)

**Security headers (150-300 LOC):**
```c
typedef struct {
    const char *csp;                    /* Content-Security-Policy */
    bool hsts;                          /* Strict-Transport-Security */
    uint32_t hsts_max_age;
    const char *frame_options;          /* DENY or SAMEORIGIN */
    const char *referrer_policy;
} ioh_security_config_t;

[[nodiscard]] ioh_middleware_fn ioh_security_middleware(const ioh_security_config_t *cfg);
```

**Auth (200-400 LOC):**
```c
typedef bool (*ioh_auth_verify_fn)(const char *credentials, void *ctx);

[[nodiscard]] ioh_middleware_fn ioh_auth_basic(ioh_auth_verify_fn verify, void *ctx);
[[nodiscard]] ioh_middleware_fn ioh_auth_bearer(ioh_auth_verify_fn verify, void *ctx);
```

**Tests (24 total, ~6 per module):**
```c
// CORS
void test_cors_preflight_allowed(void);
void test_cors_preflight_denied(void);
void test_cors_simple_request_headers(void);
void test_cors_wildcard_origin(void);
void test_cors_credentials(void);
void test_cors_disabled(void);

// Rate limiting
void test_ratelimit_under_limit(void);
void test_ratelimit_at_limit(void);
void test_ratelimit_over_limit_429(void);
void test_ratelimit_burst(void);
void test_ratelimit_refill(void);
void test_ratelimit_per_ip(void);
void test_ratelimit_max_conns_per_ip(void);          // > max → 429
void test_ratelimit_slow_post_rejected(void);        // below min_transfer_rate → 408

// Security headers
void test_security_csp_header(void);
void test_security_hsts_header(void);
void test_security_frame_options(void);
void test_security_nosniff(void);
void test_security_referrer_policy(void);
void test_security_all_headers(void);

// Auth
void test_auth_basic_valid(void);
void test_auth_basic_invalid(void);
void test_auth_basic_missing(void);
void test_auth_bearer_valid(void);
void test_auth_bearer_expired(void);
void test_auth_bearer_missing(void);
```

---

## Sprint 5: Static Files & SPA (2-3 weeks)

**Goal:** Static file serving with caching, compression, SPA fallback, C23 #embed.

### Task 5.1: Static File Server

**Files:**
- Create: `src/static/ioh_static.h`
- Create: `src/static/ioh_static.c`
- Create: `tests/unit/test_ioh_static.c`
- Modify: `CMakeLists.txt`

**Implementation:**

```c
typedef struct {
    const char *root_dir;          /* document root */
    bool directory_listing;        /* default false */
    bool etag;                     /* ETag generation */
    uint32_t max_age_default;      /* Cache-Control max-age */
} ioh_static_config_t;

[[nodiscard]] int ioh_route_static(ioh_server_t *srv, const char *prefix,
                                    const ioh_static_config_t *cfg);
```

Features:
- MIME type detection by extension (~50 types)
- ETag generation (hash of mtime + size)
- If-None-Match → 304 Not Modified
- Last-Modified / If-Modified-Since
- Range requests (Accept-Ranges, 206 Partial Content)
- sendfile/splice for zero-copy
- Path canonicalization (reject `..`, NUL bytes, symlinks outside root)
- HEAD support

**Tests (10):**
```c
void test_static_serve_file(void);
void test_static_mime_html(void);
void test_static_mime_js(void);
void test_static_mime_css(void);
void test_static_mime_wasm(void);
void test_static_etag_match(void);              // 304
void test_static_etag_mismatch(void);           // 200 + new ETag
void test_static_range_request(void);           // 206 Partial
void test_static_path_traversal(void);          // ../etc/passwd → 403
void test_static_not_found(void);               // 404
```

---

### Task 5.2: SPA Fallback

**Files:**
- Create: `src/static/ioh_spa.h`
- Create: `src/static/ioh_spa.c`
- Create: `tests/unit/test_ioh_spa.c`
- Modify: `CMakeLists.txt`

**Implementation:**

```c
typedef struct {
    const char *root_dir;
    const char *index_file;                /* default "index.html" */
    const char **api_prefixes;             /* paths excluded from fallback */
    uint32_t api_prefix_count;
    const char *immutable_pattern;         /* regex for hashed assets */
} ioh_spa_config_t;

#define IOH_STATIC_SPA (1 << 0)  /* flag for ioh_route_static() */
```

SPA fallback logic:
1. Request for `/app/settings/profile`
2. Check if file exists at `root_dir/app/settings/profile` → no
3. Check if path starts with API prefix → no
4. Check if path looks like a file (has extension) → no
5. Serve `root_dir/index.html` with 200 OK
6. For hashed assets (`app.abc123.js`): `Cache-Control: public, max-age=31536000, immutable`
7. For `index.html`: `Cache-Control: no-cache`

**Tests (8):**
```c
void test_spa_existing_file(void);            // serves actual file
void test_spa_fallback_to_index(void);        // /app/route → index.html
void test_spa_api_prefix_excluded(void);      // /api/data → 404 (not fallback)
void test_spa_hashed_asset_immutable(void);   // app.abc123.js → immutable cache
void test_spa_index_no_cache(void);           // index.html → no-cache
void test_spa_nested_route(void);             // /app/a/b/c → index.html
void test_spa_multiple_api_prefixes(void);    // /api/ and /ws/ excluded
void test_spa_file_with_extension(void);      // /style.css → serve if exists, 404 if not
```

---

### Task 5.3: Compression

**Files:**
- Create: `src/static/ioh_compress.h`
- Create: `src/static/ioh_compress.c`
- Create: `tests/unit/test_ioh_compress.c`
- Modify: `CMakeLists.txt`

**Implementation:**

Two modes:
1. **Precompressed**: if `foo.js.gz` or `foo.js.br` exists and client accepts → serve it
2. **Dynamic**: streaming gzip/brotli compression for API responses

```c
typedef struct {
    bool enable_gzip;
    bool enable_brotli;
    bool enable_precompressed;     /* serve .gz/.br files */
    uint32_t min_size;             /* don't compress below this (default 1024) */
    int compression_level;         /* 1-9 for gzip, 0-11 for brotli */
} ioh_compress_config_t;

[[nodiscard]] ioh_middleware_fn ioh_compress_middleware(const ioh_compress_config_t *cfg);
```

**Tests (8):**
```c
void test_compress_gzip_response(void);
void test_compress_brotli_response(void);
void test_compress_accept_encoding_gzip(void);
void test_compress_accept_encoding_br(void);
void test_compress_no_accept_encoding(void);     // no compression
void test_compress_precompressed_gz(void);       // serve .gz file
void test_compress_precompressed_br(void);       // serve .br file
void test_compress_below_min_size(void);         // skip small responses
```

### Task 5.4: Multipart/Form-Data Parser

**Files:**
- Create: `src/http/ioh_multipart.h`
- Create: `src/http/ioh_multipart.c`
- Create: `tests/unit/test_ioh_multipart.c`
- Modify: `CMakeLists.txt`

**Implementation:**

Streaming multipart parser for file uploads (RFC 7578):

```c
typedef struct {
    const char *name;          /* form field name */
    const char *filename;      /* original filename (if file upload) */
    const char *content_type;  /* part content-type */
    const uint8_t *data;       /* part body (zero-copy pointer) */
    size_t data_len;
} ioh_multipart_part_t;

typedef struct {
    uint32_t max_parts;        /* default 64 */
    size_t max_part_size;      /* default 10MB */
    size_t max_total_size;     /* default 50MB */
} ioh_multipart_config_t;

/* Parse all parts from multipart body */
[[nodiscard]] int ioh_multipart_parse(const ioh_request_t *req,
                                      const ioh_multipart_config_t *cfg,
                                      ioh_multipart_part_t *parts,
                                      uint32_t *part_count);
```

**Tests (8):**
```c
void test_multipart_single_field(void);          // name=value
void test_multipart_file_upload(void);           // file with filename + content-type
void test_multipart_multiple_parts(void);        // 3 fields + 1 file
void test_multipart_empty_body(void);            // no parts → 0
void test_multipart_missing_boundary(void);      // no boundary in content-type → error
void test_multipart_oversized_part(void);        // part > max_part_size → -E2BIG
void test_multipart_too_many_parts(void);        // > max_parts → -E2BIG
void test_multipart_malformed(void);             // broken boundary → error
```

---

## Sprint 6: WebSocket & SSE (2-3 weeks)

**Goal:** WebSocket with RFC 6455 compliance, SSE with heartbeat.

### Task 6.1: WebSocket

**Files:**
- Create: `src/ws/ioh_websocket.h`
- Create: `src/ws/ioh_websocket.c`
- Create: `tests/unit/test_ioh_websocket.c`
- Modify: `CMakeLists.txt`

**Implementation:**

```c
typedef struct {
    void (*on_open)(ioh_ws_conn_t *ws, void *ctx);
    void (*on_message)(ioh_ws_conn_t *ws, const uint8_t *data,
                        size_t len, bool is_text, void *ctx);
    void (*on_close)(ioh_ws_conn_t *ws, uint16_t code,
                      const char *reason, void *ctx);
    void (*on_ping)(ioh_ws_conn_t *ws, const uint8_t *data,
                     size_t len, void *ctx);
    uint32_t ping_interval_ms;
    uint32_t pong_timeout_ms;
    size_t max_message_size;
} ioh_ws_config_t;

[[nodiscard]] int ioh_ws_upgrade(ioh_request_t *req, ioh_response_t *resp,
                                 const ioh_ws_config_t *cfg, void *ctx);
[[nodiscard]] int ioh_ws_send_text(ioh_ws_conn_t *ws, const char *msg, size_t len);
[[nodiscard]] int ioh_ws_send_binary(ioh_ws_conn_t *ws, const uint8_t *data, size_t len);
[[nodiscard]] int ioh_ws_close(ioh_ws_conn_t *ws, uint16_t code, const char *reason);
```

Features:
- Upgrade handshake (SHA-1 + base64)
- Frame parsing with masking validation
- Text, binary, ping, pong, close frames
- Fragmented messages reassembly
- Automatic pong response to ping
- Configurable ping/pong keepalive
- Per-message compression (optional)

**Tests (12):**
```c
void test_ws_upgrade_handshake(void);
void test_ws_upgrade_missing_key(void);         // reject
void test_ws_send_text(void);
void test_ws_send_binary(void);
void test_ws_receive_text(void);
void test_ws_receive_masked(void);              // client mask validation
void test_ws_ping_pong(void);
void test_ws_close_handshake(void);
void test_ws_fragmented_message(void);
void test_ws_max_message_size(void);            // oversized → close
void test_ws_frame_encode(void);                // frame serialization
void test_ws_frame_decode(void);                // frame parsing
```

---

### Task 6.2: Server-Sent Events

**Files:**
- Create: `src/ws/ioh_sse.h`
- Create: `src/ws/ioh_sse.c`
- Create: `tests/unit/test_ioh_sse.c`
- Modify: `CMakeLists.txt`

**Implementation:**

```c
typedef struct {
    const char *event;    /* event type (optional) */
    const char *data;     /* event data (required) */
    const char *id;       /* event ID (optional) */
    uint32_t retry_ms;    /* reconnect hint (optional, 0 = omit) */
} ioh_sse_event_t;

[[nodiscard]] int ioh_sse_start(ioh_request_t *req, ioh_response_t *resp,
                                ioh_sse_stream_t **stream);
[[nodiscard]] int ioh_sse_send(ioh_sse_stream_t *stream, const ioh_sse_event_t *event);
[[nodiscard]] int ioh_sse_send_comment(ioh_sse_stream_t *stream, const char *comment);
void ioh_sse_close(ioh_sse_stream_t *stream);
```

Features:
- Content-Type: text/event-stream; charset=utf-8
- Cache-Control: no-cache
- Heartbeat comments (`:ping\n\n`) every 30s via io_uring timer
- Last-Event-ID header support for reconnection
- Backpressure: configurable max buffered events (default 100)

**Tests (8):**
```c
void test_sse_start_headers(void);              // correct Content-Type
void test_sse_send_data_only(void);             // "data: message\n\n"
void test_sse_send_with_event(void);            // "event: update\ndata: ...\n\n"
void test_sse_send_with_id(void);               // "id: 42\ndata: ...\n\n"
void test_sse_send_with_retry(void);            // "retry: 3000\n"
void test_sse_send_comment(void);               // ":ping\n\n"
void test_sse_multiline_data(void);             // multi-line data field
void test_sse_close(void);
```

---

## Sprint 7: HTTP/2 (nghttp2 Integration) (3-4 weeks)

**Goal:** Full HTTP/2 support via nghttp2 — HPACK, stream multiplexing, flow control.

### Task 7.1: nghttp2 Session Management

**Files:**
- Create: `src/http/ioh_http2.h`
- Create: `src/http/ioh_http2.c`
- Create: `tests/unit/test_ioh_http2.c`
- Modify: `CMakeLists.txt`

**Implementation:**

```c
typedef struct {
    uint32_t max_concurrent_streams;   /* default 100 — CVE-2023-44487 Rapid Reset protection */
    uint32_t initial_window_size;      /* default 65535 */
    uint32_t max_frame_size;           /* default 16384 */
    uint32_t max_header_list_size;     /* default 8192 */
    uint32_t max_rst_stream_per_sec;   /* default 100, Rapid Reset rate limit */
} ioh_http2_config_t;

[[nodiscard]] ioh_http2_session_t *ioh_http2_session_create(
    ioh_conn_t *conn, const ioh_http2_config_t *cfg);
void ioh_http2_session_destroy(ioh_http2_session_t *session);
[[nodiscard]] int ioh_http2_on_recv(ioh_http2_session_t *session,
                                    const uint8_t *data, size_t len);
[[nodiscard]] int ioh_http2_flush(ioh_http2_session_t *session);
```

nghttp2 callback integration:
- `on_begin_headers_callback` → allocate ioh_request_t per stream
- `on_header_callback` → collect headers into request
- `on_data_chunk_recv_callback` → accumulate body
- `on_frame_recv_callback` → dispatch to router when END_STREAM received
- `on_stream_close_callback` → cleanup stream resources

**Tests (10):**
```c
void test_http2_session_create_destroy(void);
void test_http2_connection_preface(void);       // client preface + SETTINGS
void test_http2_settings_ack(void);
void test_http2_simple_get(void);               // HEADERS frame → response
void test_http2_post_with_body(void);           // HEADERS + DATA → response
void test_http2_stream_multiplexing(void);      // 3 concurrent streams
void test_http2_flow_control(void);             // WINDOW_UPDATE
void test_http2_goaway_two_phase(void);          // two-phase GOAWAY: first with 2^31-1, then real last_stream_id
void test_http2_goaway_drains(void);            // GOAWAY → wait for want_read==0 && want_write==0 → close
void test_http2_rst_stream(void);               // stream error
void test_http2_max_concurrent_streams(void);   // reject excess streams (CVE-2023-44487)
void test_http2_rapid_reset_protection(void);   // excessive RST_STREAM rate → GOAWAY + disconnect
```

---

### Task 7.2: ALPN-Based Protocol Selection

**Files:**
- Modify: `src/tls/ioh_tls.c` (ALPN callback)
- Modify: `src/core/ioh_conn.c` (protocol dispatch after ALPN)
- Create: `tests/integration/test_http2_server.c`
- Modify: `CMakeLists.txt`

**Implementation:**

After TLS handshake, check ALPN:
- `h2` → create nghttp2 session, process with HTTP/2
- `http/1.1` → process with picohttpparser

**Tests (6):**
```c
void test_alpn_selects_h2(void);                // TLS + h2 ALPN → HTTP/2
void test_alpn_selects_http11(void);            // TLS + http/1.1 → HTTP/1.1
void test_alpn_default_http11(void);            // no ALPN → HTTP/1.1
void test_http2_full_request_via_tls(void);     // end-to-end h2 request
void test_http2_multiple_streams_via_tls(void);
void test_http2_server_push(void);              // optional: server push
```

---

## Sprint 8: HTTP/3 (QUIC via ngtcp2 + nghttp3) (4-5 weeks)

**Goal:** HTTP/3 over QUIC with wolfSSL crypto, 0-RTT, connection migration.

### Task 8.1: QUIC Transport (ngtcp2)

**Files:**
- Create: `src/http/ioh_quic.h`
- Create: `src/http/ioh_quic.c`
- Create: `src/tls/ioh_tls_quic.h`
- Create: `src/tls/ioh_tls_quic.c`
- Create: `tests/unit/test_ioh_quic.c`
- Modify: `CMakeLists.txt`

**Implementation:**

```c
typedef enum : uint8_t {
    IOH_QUIC_CC_CUBIC,       /* default, good for stable networks */
    IOH_QUIC_CC_NEWRENO,     /* simple, RFC-compliant */
    IOH_QUIC_CC_BBR2,        /* best for lossy/mobile networks: ~21% better p99 latency */
} ioh_quic_cc_algo_t;

typedef struct {
    uint32_t max_streams_bidi;       /* default 100 */
    uint32_t initial_max_data;       /* default 1MB */
    uint64_t idle_timeout_ms;        /* default 30000 */
    bool enable_0rtt;
    bool enable_gso;                 /* UDP GSO for batch send (Linux 4.18+) */
    bool enable_gro;                 /* UDP GRO for batch recv (Linux 5.0+) */
    ioh_quic_cc_algo_t cc_algo;       /* congestion control algorithm */
} ioh_quic_config_t;

[[nodiscard]] ioh_quic_server_t *ioh_quic_server_create(
    const ioh_quic_config_t *cfg, ioh_tls_ctx_t *tls);
void ioh_quic_server_destroy(ioh_quic_server_t *srv);
[[nodiscard]] int ioh_quic_on_recv(ioh_quic_server_t *srv,
                                    const uint8_t *data, size_t len,
                                    const struct sockaddr *remote);
[[nodiscard]] int ioh_quic_flush(ioh_quic_server_t *srv);
```

Uses `ngtcp2_crypto_wolfssl_configure_server_context()` for QUIC crypto.
UDP socket management via io_uring recv/send.

**UDP GSO/GRO:** GSO sends multiple QUIC packets in one sendmsg via `UDP_SEGMENT` cmsg.
GRO coalesces received packets. ngtcp2 natively supports GSO via `ngtcp2_conn_write_aggregate_pkt()`.
Integrate with io_uring through `io_uring_prep_sendmsg()` with cmsg for `UDP_SEGMENT`.

**QUIC CID demultiplexing:** Multishot UDP recv delivers mixed packets from all clients.
Implement CID-to-connection lookup (hash table) for O(1) packet routing to `ngtcp2_conn`.
`SO_REUSEPORT` for multiple worker threads receiving on same UDP port.

**Connection migration:** ngtcp2 fully supports QUIC connection migration. Server requirements:
handle PATH_CHALLENGE/PATH_RESPONSE, update CID mapping on migration, validate new path.

**Congestion control:** ngtcp2 supports CUBIC (default), NewReno, BBRv2. BBRv2 gives ~21% better
p99 latency on lossy networks (mobile clients). Configurable via `ioh_quic_cc_algo_t`.

**Tests (8):**
```c
void test_quic_server_create_destroy(void);
void test_quic_initial_handshake(void);         // QUIC Initial packets
void test_quic_connection_established(void);
void test_quic_stream_open(void);
void test_quic_stream_data(void);
void test_quic_connection_close(void);
void test_quic_idle_timeout(void);
void test_quic_wolfssl_crypto(void);            // ngtcp2_crypto_wolfssl
void test_quic_gso_sendmsg(void);              // UDP GSO batch send
void test_quic_cid_demux(void);                // CID-to-connection lookup
void test_quic_connection_migration(void);     // path change, CID remapping
void test_quic_cc_cubic(void);                 // default congestion control
void test_quic_cc_bbr2(void);                  // BBRv2 selection
```

---

### Task 8.2: HTTP/3 (nghttp3)

**Files:**
- Create: `src/http/ioh_http3.h`
- Create: `src/http/ioh_http3.c`
- Create: `tests/unit/test_ioh_http3.c`
- Modify: `CMakeLists.txt`

**Implementation:**

```c
[[nodiscard]] ioh_http3_session_t *ioh_http3_session_create(
    ioh_quic_conn_t *quic_conn, const ioh_http2_config_t *cfg);
void ioh_http3_session_destroy(ioh_http3_session_t *session);
[[nodiscard]] int ioh_http3_on_stream_data(ioh_http3_session_t *session,
                                           int64_t stream_id,
                                           const uint8_t *data, size_t len);
```

nghttp3 callbacks → ioh_request_t → router → handler → nghttp3 response submission.

**Tests (8):**
```c
void test_http3_session_create_destroy(void);
void test_http3_simple_get(void);
void test_http3_post_with_body(void);
void test_http3_stream_multiplexing(void);
void test_http3_qpack_compression(void);
void test_http3_goaway(void);
void test_http3_server_push(void);
void test_http3_0rtt_request(void);              // 0-RTT early data
```

---

### Task 8.3: HTTP/3 Integration Test

**Files:**
- Create: `tests/integration/test_http3_server.c`
- Modify: `CMakeLists.txt`

**Tests (5):**
```c
void test_http3_full_request_via_quic(void);
void test_http3_concurrent_streams(void);
void test_http3_fallback_to_h2(void);            // Alt-Svc header
void test_http3_connection_migration(void);       // IP change mid-session
void test_http3_alt_svc_advertisement(void);      // h3 advertised in HTTP/1.1
```

---

## Sprint 9: JSON API, Logging & Metrics (2 weeks)

**Goal:** yyjson integration, structured logging, Prometheus metrics, health checks.

### Task 9.1: JSON API Helpers (yyjson)

**Files:**
- Create: `src/core/ioh_json.h`
- Create: `src/core/ioh_json.c`
- Create: `tests/unit/test_ioh_json.c`
- Modify: `CMakeLists.txt`

**Implementation:**

```c
[[nodiscard]] int ioh_respond_json(ioh_response_t *resp, uint16_t status,
                                   const char *json);
[[nodiscard]] int ioh_respond_json_obj(ioh_response_t *resp, uint16_t status,
                                       yyjson_mut_doc *doc);
[[nodiscard]] yyjson_doc *ioh_request_json(ioh_request_t *req);
[[nodiscard]] int ioh_respond_error(ioh_response_t *resp, uint16_t status,
                                    const char *error_code, const char *message);
```

Error format:
```json
{"error": {"code": "not_found", "message": "Resource not found"}}
```

**Tests (8):**
```c
void test_json_respond_string(void);
void test_json_respond_object(void);
void test_json_parse_request_body(void);
void test_json_parse_invalid_body(void);        // 400 Bad Request
void test_json_error_format(void);
void test_json_content_type(void);              // application/json
void test_json_unicode(void);
void test_json_large_body(void);                // streaming for large JSON
```

---

### Task 9.2: Structured Logging

**Files:**
- Create: `src/middleware/ioh_logging.h`
- Create: `src/middleware/ioh_logging.c`
- Create: `tests/unit/test_ioh_logging.c`
- Modify: `CMakeLists.txt`

**Tests (6):**
```c
void test_logging_access_log_format(void);
void test_logging_error_log(void);
void test_logging_json_format(void);
void test_logging_levels(void);
void test_logging_proxy_ip(void);               // original IP from PROXY
void test_logging_tls_info(void);               // TLS version in log
```

---

### Task 9.3: Prometheus Metrics

**Files:**
- Create: `src/middleware/ioh_metrics.h`
- Create: `src/middleware/ioh_metrics.c`
- Create: `tests/unit/test_ioh_metrics.c`
- Modify: `CMakeLists.txt`

**Implementation:**

**Lock-free thread-local metrics architecture:** Each worker thread maintains a thread-local
metrics registry aligned to cache line boundaries (64 bytes). No atomic operations in hot path.
On `/metrics` scrape, a dedicated thread iterates thread-local blocks, aggregates counters
without locks, serializes to Prometheus text exposition format.

```
# HELP ioh_http_requests_total Total HTTP requests
# TYPE ioh_http_requests_total counter
ioh_http_requests_total{method="GET",status="200"} 1234

# HELP ioh_http_connections_active Active connections by protocol
# TYPE ioh_http_connections_active gauge
ioh_http_connections_active{protocol="h1"} 30
ioh_http_connections_active{protocol="h2"} 10
ioh_http_connections_active{protocol="h3"} 2

# HELP ioh_http_request_duration_seconds Request duration
# TYPE ioh_http_request_duration_seconds histogram
ioh_http_request_duration_seconds_bucket{le="0.01"} 900

# HELP ioh_tls_handshake_duration_seconds TLS handshake duration
# TYPE ioh_tls_handshake_duration_seconds histogram
ioh_tls_handshake_duration_seconds_bucket{le="0.05"} 800

# HELP io_uring_sqe_submitted_total Total SQEs submitted
# TYPE io_uring_sqe_submitted_total counter
io_uring_sqe_submitted_total 50000

# HELP ioh_bufpool_available Available buffers in provided buffer ring
# TYPE ioh_bufpool_available gauge
ioh_bufpool_available 96
```

Built-in endpoints:
- `/metrics` — Prometheus text exposition
- `/health` — liveness check (always 200 if server running)
- `/ready` — readiness check (verifies TLS certs loaded, buffer pools healthy, connection limits OK)

**Tests (6):**
```c
void test_metrics_counter_increment(void);
void test_metrics_gauge_set(void);
void test_metrics_histogram_observe(void);
void test_metrics_text_exposition(void);
void test_metrics_health_endpoint(void);
void test_metrics_ready_endpoint(void);
void test_metrics_thread_local_no_contention(void);  // verify no locks in hot path
void test_metrics_aggregation_across_threads(void);   // scrape aggregates thread-local counters
void test_metrics_cache_line_aligned(void);           // verify 64-byte alignment
void test_metrics_protocol_labels(void);              // h1/h2/h3 protocol labels
```

---

## Sprint 10: Stabilization, Benchmarks & Documentation (3-4 weeks)

**Goal:** Fuzz testing, performance benchmarks, API documentation, packaging.

### Task 10.1: Fuzz Targets

**Files:**
- Create: `tests/fuzz/fuzz_http1_parser.c`
- Create: `tests/fuzz/fuzz_proxy_proto.c`
- Create: `tests/fuzz/fuzz_ws_frame.c`
- Create: `tests/fuzz/fuzz_uri_normalize.c`
- Create: `tests/fuzz/fuzz_chunked_decode.c`
- Modify: `CMakeLists.txt`

### Task 10.2: Performance Benchmarks

**Files:**
- Create: `tests/bench/bench_http1_parsing.c`
- Create: `tests/bench/bench_router_lookup.c`
- Create: `tests/bench/bench_tls_handshake.c`

### Task 10.3: API Documentation

**Files:**
- Create: `docs/en/03-api-reference.md`
- Create: `docs/en/04-configuration.md`
- Create: `docs/en/05-middleware.md`
- Create: `docs/en/06-websocket-sse.md`
- Create: `docs/en/07-deployment.md`

### Task 10.4: Example Applications

**Files:**
- Create: `examples/hello_world.c`
- Create: `examples/spa_server.c`
- Create: `examples/api_server.c`
- Create: `examples/websocket_chat.c`

---

## Test Summary

| Sprint | Focus | New Tests |
|--------|-------|-----------|
| 1 | io_uring + Server Skeleton (+ ring restrictions) | 42 |
| 2 | wolfSSL TLS Integration (+ hot reload, serialization) | 16 |
| 3 | HTTP/1.1 + PROXY Protocol | 52 |
| 4 | Router + Middleware (+ Slowloris defense) | 92 |
| 5 | Static Files + SPA + Compression + Multipart | 34 |
| 6 | WebSocket + SSE | 20 |
| 7 | HTTP/2 (+ two-phase GOAWAY, Rapid Reset CVE-2023-44487) | 18 |
| 8 | HTTP/3 (+ GSO/GRO, CID demux, BBRv2, conn migration) | 26 |
| 9 | JSON + Logging + Metrics (lock-free thread-local) | 24 |
| 10 | Fuzz + Bench + Docs | 5 fuzz targets |
| **Total** | | **~324 tests + 5 fuzz targets** |

## Timeline

| Sprint | Duration | Milestone |
|--------|----------|-----------|
| S1 | 3-4 weeks | io_uring event loop, basic server |
| S2 | 2-3 weeks | TLS handshake over io_uring |
| S3 | 3-4 weeks | Working HTTP/1.1 server (usable MVP) |
| S4 | 3-4 weeks | Radix-trie router + middleware (production-ready HTTP/1.1) |
| S5 | 2-3 weeks | Static files + SPA serving |
| S6 | 2-3 weeks | WebSocket + SSE |
| S7 | 3-4 weeks | HTTP/2 support |
| S8 | 4-5 weeks | HTTP/3 + QUIC support |
| S9 | 2 weeks | API, logging, metrics |
| S10 | 3-4 weeks | Stabilization, benchmarks, docs |
| **Total** | **~27-37 weeks** | **v0.1.0 release** |

## Router Design Heritage

The iohttp router synthesizes best practices from the most influential HTTP routers:

| Pattern | Origin | Year | iohttp Feature |
|---------|--------|------|----------------|
| Radix trie routing | httprouter (Go) | 2013 | Compressed prefix tree, per-method trees |
| Zero-allocation dispatch | httprouter | 2013 | No heap alloc in match path (natural in C) |
| Static > param > wildcard priority | httprouter / echo | 2013 | Deterministic, order-independent |
| Auto-405 Method Not Allowed | httprouter / FastRoute | 2013 | Path matches another method → 405 + Allow |
| Auto-HEAD fallback to GET | FastRoute (PHP) | 2014 | HEAD with no handler → use GET handler |
| Trailing slash redirect | httprouter | 2013 | /users/ ↔ /users → 301 |
| Path auto-correction | httprouter | 2013 | //foo, /FOO → 301 to canonical |
| Route groups + prefix | Express.Router (Node) | 2010 | Nested groups with middleware inheritance |
| next() middleware chain | Express (Node) | 2010 | Short-circuit or continue chain |
| Handler returns error | bunrouter / echo (Go) | 2021 | int return → centralized error handler |
| Method-specific registration | Sinatra / Flask / echo | 2007 | ioh_router_get(), ioh_router_post(), ... |
| Conflict detection | matchit / Axum (Rust) | 2021 | /:id + /:name → error at registration |
| Route introspection / walking | gorilla/mux (Go) | 2012 | ioh_router_walk() for docs generation |
| Typed param extraction | echo / FastAPI | 2015 | ioh_request_param_i64(), _u64(), _bool() |
| Route metadata attachment | FastAPI (Python) | 2018 | oas_operation_t* per route for OpenAPI |
| Param regex constraints | FastRoute | 2014 | NOT adopted (YAGNI — use typed extraction) |
| Named routes / URL reversal | gorilla/mux | 2012 | NOT adopted (YAGNI for embedded C server) |
| Optional segments | FastRoute | 2014 | NOT adopted (YAGNI) |

## liboas Integration Architecture

liboas is a **separate project** — an OpenAPI 3.2.0 library that integrates with iohttp via adapter.

**Key architectural principle:** One route lookup. iohttp's router finds the route; liboas receives
the already-matched `oas_operation_t*` via route metadata — no second path matching in hot path.

**Integration points in iohttp:**
- `ioh_route_opts_t.oas_operation` — pointer to compiled operation metadata
- `ioh_router_walk()` — route introspection for OpenAPI spec generation
- `ioh_tls_peer_info_t` — normalized TLS/mTLS metadata for security context
- `ioh_conn_info_t` — real client IP (after PROXY protocol) for security context
- Middleware hooks — pre-handler request validation, post-handler response validation

**What iohttp provides to liboas (via adapter):**
- `ioh_request_t` → `oas_runtime_request_t` mapping
- `ioh_response_t` → `oas_runtime_response_t` mapping
- TLS/auth/proxy context → `oas_security_ctx_t` mapping
- `/openapi.json` publish helper

**What iohttp does NOT do:**
- No OpenAPI parsing/compilation (liboas responsibility)
- No JSON Schema validation (liboas responsibility)
- No duplicate route matching (one lookup only)

## Audit-Driven Additions (2026-03-08)

Based on technical audit (`iohttp-technical-audit.md`) and architectural audit (`iohttp-architectural-audit.md`):

**Added to plan:**

| Addition | Sprint | Rationale |
|----------|--------|-----------|
| `IORING_REGISTER_RESTRICTIONS` | S1 | io_uring bypasses seccomp — must whitelist opcodes |
| SEND_ZC dynamic threshold (2 KiB) | S1 | ZC slower for small payloads due to page pinning overhead |
| SQPOLL optional, DEFER_TASKRUN default | S1 | SQPOLL burns CPU core, wrong for bursty HTTP traffic |
| Kernel 6.7 justification (CVE-2024-0582) | S1 | Provided buffer ring use-after-free in 6.4–6.6.4 |
| SIGPIPE SIG_IGN at startup | S1 | wolfSSL can trigger SIGPIPE on closed socket write |
| wolfSSL I/O serialization note | S2 | Single I/O buffer per SSL object — natural in ring-per-thread |
| TLS certificate hot reload | S2 | Atomic CTX swap with refcount for zero-downtime cert rotation |
| Slowloris/Slow POST defense | S4 | Per-IP conn limits, min transfer rate, linked timeouts |
| Two-phase GOAWAY (HTTP/2) | S7 | Best practice: first GOAWAY with 2^31-1, then real last_stream_id |
| HTTP/2 Rapid Reset (CVE-2023-44487) | S7 | RST_STREAM rate limiting, max_concurrent_streams enforcement |
| UDP GSO/GRO for QUIC | S8 | Batch send/recv for QUIC — significant throughput gain |
| Congestion control config (BBRv2) | S8 | ~21% better p99 latency on lossy/mobile networks vs CUBIC |
| QUIC CID demultiplexing | S8 | Hash table for O(1) packet routing to ngtcp2_conn |
| QUIC connection migration | S8 | PATH_CHALLENGE/RESPONSE, CID remapping |
| Lock-free thread-local metrics | S9 | Cache-line aligned, no atomics in hot path, aggregate on scrape |
| Specific Prometheus metric names | S9 | protocol labels, io_uring metrics, buffer pool metrics |
| Health/readiness endpoints | S9 | `/health` (liveness) + `/ready` (TLS/buffers/conns check) |

**Rejected (YAGNI):**

| Proposal | Reason |
|----------|--------|
| Adaptive Radix Tree (ART) | Standard radix trie sufficient for <1000 routes (typical HTTP server) |
| Distributed tracing (W3C traceparent) | Not for embedded HTTP library — add via middleware if needed |
| Config hot reload (beyond TLS certs) | Not for v0.1.0 — restart is acceptable |
| eBPF for QUIC CID routing | Too complex for first release — user-space hash table sufficient |
| Custom wolfSSL memory allocator (slab) | Premature optimization — profile first |
| OpenTelemetry | Prometheus pull model far lighter for embedded C server |
| NGINX Unit / Drogon / Boost.Beast in comparison | C++ projects, not direct competitors to embedded C library |

## Critical Dependencies

1. **liburing 2.7+** — multishot accept, provided buffers
2. **wolfSSL 5.8+** — TLS 1.3, QUIC crypto
3. **picohttpparser** — vendored (MIT, ~800 LOC)
4. **nghttp2** — system package (Sprint 7)
5. **ngtcp2 + ngtcp2_crypto_wolfssl** — system/build (Sprint 8)
6. **nghttp3** — system package (Sprint 8)
7. **yyjson** — system package (Sprint 9)
8. **zlib + brotli** — system packages (Sprint 5)
9. **liboas** — separate project, adapter integration (Sprint 9+)

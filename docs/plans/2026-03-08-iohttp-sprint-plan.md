# iohttp Sprint Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development to implement this plan task-by-task.

**Goal:** Build a production-grade embedded HTTP server library in C23 with io_uring, wolfSSL, HTTP/1.1+2+3.

**Architecture:** Modular composition — io_uring event loop + wolfSSL TLS + picohttpparser (HTTP/1.1) + nghttp2 (HTTP/2) + ngtcp2+nghttp3 (HTTP/3). Single-threaded, callback-based, zero-copy where possible.

**Tech Stack:** C23, liburing 2.7+, wolfSSL 5.8+, picohttpparser, nghttp2, ngtcp2, nghttp3, yyjson, Unity tests, Linux kernel 6.7+.

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
- Create: `src/core/io_loop.h`
- Create: `src/core/io_loop.c`
- Create: `tests/unit/test_io_loop.c`
- Modify: `CMakeLists.txt`

**Implementation:**

`io_loop_t` wraps `struct io_uring` with:
- `io_loop_create(io_loop_config_t *cfg)` — ring setup with queue_depth, flags
- `io_loop_destroy(io_loop_t *loop)` — cleanup
- `io_loop_run(io_loop_t *loop)` — main event loop (blocks)
- `io_loop_stop(io_loop_t *loop)` — signal stop via eventfd
- `io_loop_add_timer(loop, ms, callback, data)` — timer via `IORING_OP_TIMEOUT`
- `io_loop_add_linked_timeout(loop, sqe, ms)` — linked timeout via `IORING_OP_LINK_TIMEOUT`
- `io_loop_cancel_timer(loop, timer_id)` — cancel pending timer
- `io_loop_register_buffers(loop, iovecs, count)` — `IORING_REGISTER_BUFFERS` for pinned DMA memory
- `io_loop_register_files(loop, fds, count)` — `IORING_REGISTER_FILES` to skip fd table lookup

```c
typedef struct {
    uint32_t queue_depth;        /* default 256 */
    uint32_t buf_ring_size;      /* default 128 */
    uint32_t buf_size;           /* default 4096 */
    uint32_t registered_bufs;    /* number of registered buffers (0 = disabled) */
    uint32_t registered_files;   /* max registered fds (0 = disabled) */
    bool     sqpoll;             /* SQPOLL mode (requires CAP_SYS_ADMIN) */
} io_loop_config_t;
```

**io_uring features per operation:**
- Multishot accept: `IORING_OP_ACCEPT` + `IORING_ACCEPT_MULTISHOT` + `IOSQE_CQE_SKIP_SUCCESS`
- Recv: `IORING_OP_RECV` with provided buffer ring (`io_uring_setup_buf_ring`)
- Send: `IORING_OP_SEND_ZC` for zero-copy (payloads > 3KB), regular `IORING_OP_SEND` otherwise
- Timers: `IORING_OP_LINK_TIMEOUT` for per-operation deadlines (keepalive, header, body)
- Files: `IORING_OP_SPLICE` for zero-copy static file serving
- Registered buffers: `IORING_REGISTER_BUFFERS` — pinned memory, avoids page table walks on DMA
- Registered files: `IORING_REGISTER_FILES` — pre-registered fds, skips fd table lookup per I/O

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
```

**CMake:**
```cmake
add_library(io_loop STATIC src/core/io_loop.c)
target_include_directories(io_loop PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(io_loop PUBLIC ${LIBURING_LIBRARIES})
target_include_directories(io_loop PUBLIC ${LIBURING_INCLUDE_DIRS})

io_add_test(test_io_loop tests/unit/test_io_loop.c io_loop)
```

---

### Task 1.2: Connection State Machine

**Files:**
- Create: `src/core/io_conn.h`
- Create: `src/core/io_conn.c`
- Create: `tests/unit/test_io_conn.c`
- Modify: `CMakeLists.txt`

**Implementation:**

Connection pool with fixed-size array (compile-time configurable):

```c
typedef enum : uint8_t {
    IO_CONN_FREE = 0,
    IO_CONN_ACCEPTING,
    IO_CONN_PROXY_HEADER,
    IO_CONN_TLS_HANDSHAKE,
    IO_CONN_HTTP_ACTIVE,
    IO_CONN_WEBSOCKET,
    IO_CONN_CLOSING,
} io_conn_state_t;

typedef struct {
    int fd;
    io_conn_state_t state;
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
} io_conn_t;
```

- `io_conn_pool_create(max_conns)` / `_destroy()`
- `io_conn_alloc(pool)` → io_conn_t* (or nullptr if full)
- `io_conn_free(pool, conn)` — reset and return to pool
- `io_conn_find(pool, fd)` — lookup by fd
- `io_conn_transition(conn, new_state)` — validate state transitions

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
- Create: `src/core/io_server.h`
- Create: `src/core/io_server.c`
- Create: `tests/unit/test_io_server.c`
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
} io_server_config_t;

[[nodiscard]] io_server_t *io_server_create(const io_server_config_t *cfg);
void io_server_destroy(io_server_t *srv);
[[nodiscard]] int io_server_run(io_server_t *srv);    /* blocks */
void io_server_stop(io_server_t *srv);
```

Server creates: listen socket → bind → listen → io_loop with multishot accept.

**Tests (7):**
```c
void test_server_config_defaults(void);
void test_server_config_validate_valid(void);
void test_server_config_validate_zero_port(void);     // -EINVAL
void test_server_config_validate_zero_conns(void);    // -EINVAL
void test_server_create_destroy(void);
void test_server_listen_socket(void);                 // bind succeeds
void test_server_accept_connection(void);             // socketpair test
```

---

### Task 1.4: Buffer Pool & Registered Resources

**Files:**
- Create: `src/core/io_buffer.h`
- Create: `src/core/io_buffer.c`
- Create: `tests/unit/test_io_buffer.c`
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
} io_bufpool_config_t;

[[nodiscard]] io_bufpool_t *io_bufpool_create(const io_bufpool_config_t *cfg);
void io_bufpool_destroy(io_bufpool_t *pool);
[[nodiscard]] int io_bufpool_register_ring(io_bufpool_t *pool, struct io_uring *ring);
[[nodiscard]] int io_bufpool_register_bufs(io_bufpool_t *pool, struct io_uring *ring);
[[nodiscard]] int io_bufpool_register_files(io_bufpool_t *pool, struct io_uring *ring);
void io_bufpool_return(io_bufpool_t *pool, uint32_t buf_id);
uint8_t *io_bufpool_get_buf(io_bufpool_t *pool, uint32_t buf_id);
uint8_t *io_bufpool_get_reg_buf(io_bufpool_t *pool, uint32_t idx);
[[nodiscard]] int io_bufpool_register_fd(io_bufpool_t *pool, int fd);
void io_bufpool_unregister_fd(io_bufpool_t *pool, int fd);
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
- Create: `src/tls/io_tls.h`
- Create: `src/tls/io_tls.c`
- Create: `tests/unit/test_io_tls.c`
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
} io_tls_config_t;

[[nodiscard]] io_tls_ctx_t *io_tls_ctx_create(const io_tls_config_t *cfg);
void io_tls_ctx_destroy(io_tls_ctx_t *ctx);
[[nodiscard]] int io_tls_accept(io_tls_ctx_t *ctx, io_conn_t *conn);
[[nodiscard]] int io_tls_read(io_conn_t *conn, uint8_t *buf, size_t len);
[[nodiscard]] int io_tls_write(io_conn_t *conn, const uint8_t *buf, size_t len);
void io_tls_shutdown(io_conn_t *conn);
const char *io_tls_get_alpn(io_conn_t *conn);
```

Custom I/O callbacks integrate with io_uring:
- `wolfSSL_CTX_SetIORecv()` → reads from connection's cipher buffer
- `wolfSSL_CTX_SetIOSend()` → writes to connection's send buffer
- Buffers are drained/filled by io_uring recv/send operations
- **CRITICAL:** `wolfSSL_CTX_set_mode(ctx, SSL_MODE_ENABLE_PARTIAL_WRITE)` — io_uring is non-blocking, wolfSSL must handle partial writes
- Manual `WANT_READ`/`WANT_WRITE` handling — re-arm io_uring recv/send on these returns
- Use registered buffers for TLS record I/O (16KB input + 16KB output per connection)

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
```

---

### Task 2.2: TLS + io_uring Integration

**Files:**
- Modify: `src/tls/io_tls.c` (add io_uring I/O callback integration)
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
- Create: `src/http/io_request.h`
- Create: `src/http/io_request.c`
- Create: `src/http/io_response.h`
- Create: `src/http/io_response.c`
- Create: `tests/unit/test_io_request.c`
- Create: `tests/unit/test_io_response.c`
- Modify: `CMakeLists.txt`

**Implementation:**

Protocol-independent request/response:

```c
typedef struct {
    io_method_t method;
    const char *path;
    size_t path_len;
    const char *query;
    size_t query_len;
    io_header_t *headers;
    uint32_t header_count;
    const uint8_t *body;
    size_t body_len;
    io_conn_t *conn;
    /* Path parameters from router */
    io_param_t params[IO_MAX_PATH_PARAMS];
    uint32_t param_count;
    /* Metadata */
    uint8_t http_version_major;
    uint8_t http_version_minor;
    bool keep_alive;
    const char *content_type;
    size_t content_length;
} io_request_t;

typedef struct {
    uint16_t status;
    io_header_t *headers;
    uint32_t header_count;
    uint32_t header_capacity;
    uint8_t *body;
    size_t body_len;
    size_t body_capacity;
    bool headers_sent;
    bool chunked;
} io_response_t;

/* Response helpers */
[[nodiscard]] int io_respond(io_response_t *resp, uint16_t status,
                              const char *content_type,
                              const uint8_t *body, size_t body_len);
[[nodiscard]] int io_respond_json(io_response_t *resp, uint16_t status,
                                   const char *json);
[[nodiscard]] int io_respond_file(io_response_t *resp, const char *path);
[[nodiscard]] int io_response_set_header(io_response_t *resp,
                                          const char *name, const char *value);
```

**Tests (12):**
```c
void test_request_init(void);
void test_request_method_parse(void);
void test_request_header_find(void);
void test_request_header_find_missing(void);
void test_request_content_length(void);
void test_request_keep_alive_11(void);          // HTTP/1.1 default keep-alive
void test_request_keep_alive_10(void);          // HTTP/1.0 default close
void test_response_init(void);
void test_response_set_header(void);
void test_response_json(void);
void test_response_serialize_headers(void);
void test_response_status_text(void);           // 200→"OK", 404→"Not Found"
```

---

### Task 3.2: picohttpparser Integration

**Files:**
- Create: `src/http/io_http1.h`
- Create: `src/http/io_http1.c`
- Add: `src/http/picohttpparser.h` (vendored, ~800 LOC)
- Add: `src/http/picohttpparser.c` (vendored)
- Create: `tests/unit/test_io_http1.c`
- Modify: `CMakeLists.txt`

**Implementation:**

Wrap picohttpparser's stateless parsing with buffered I/O:

```c
[[nodiscard]] int io_http1_parse_request(const uint8_t *buf, size_t len,
                                          io_request_t *req);
[[nodiscard]] int io_http1_serialize_response(const io_response_t *resp,
                                               uint8_t *buf, size_t buf_size);
[[nodiscard]] int io_http1_decode_chunked(io_chunked_decoder_t *dec,
                                           uint8_t *buf, size_t *len);
```

Return values: >0 = bytes consumed, -EAGAIN = need more data, <0 = error.

**Tests (12):**
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
```

---

### Task 3.3: PROXY Protocol Decoder

**Files:**
- Create: `src/http/io_proxy_proto.h`
- Create: `src/http/io_proxy_proto.c`
- Create: `tests/unit/test_proxy_proto.c`
- Modify: `CMakeLists.txt`

**Implementation:**

```c
typedef struct {
    uint8_t version;     /* 1 or 2 */
    bool is_local;       /* LOCAL command (health check) */
    struct sockaddr_storage src_addr;
    struct sockaddr_storage dst_addr;
} io_proxy_result_t;

/* Returns bytes consumed, -EAGAIN for incomplete, <0 for error */
[[nodiscard]] int io_proxy_decode(const uint8_t *buf, size_t len,
                                   io_proxy_result_t *result);
```

PPv1: text format `PROXY TCP4 src dst sport dport\r\n`
PPv2: binary format with 12-byte signature, supports TLV extensions.

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

## Sprint 4: Router & Middleware (2-3 weeks)

**Goal:** Trie-based router with path parameters, middleware chain, built-in middleware.

### Task 4.1: Router (Trie-Based)

**Files:**
- Create: `src/core/io_router.h`
- Create: `src/core/io_router.c`
- Create: `tests/unit/test_io_router.c`
- Modify: `CMakeLists.txt`

**Implementation:**

Longest-prefix trie with path parameter extraction:

```c
typedef int (*io_handler_fn)(io_request_t *req, io_response_t *resp, void *ctx);

[[nodiscard]] io_router_t *io_router_create(void);
void io_router_destroy(io_router_t *router);
[[nodiscard]] int io_route_add(io_router_t *router, io_method_t method,
                                const char *pattern, io_handler_fn handler, void *ctx);
[[nodiscard]] int io_route_group(io_router_t *router, const char *prefix,
                                  io_middleware_fn *middleware, uint32_t mw_count);
io_route_match_t io_router_match(io_router_t *router, io_method_t method,
                                   const char *path, size_t path_len);
```

Path patterns: `/api/users/:id/config`, `/static/*`, `/health`

**Tests (10):**
```c
void test_router_create_destroy(void);
void test_router_exact_match(void);              // /health
void test_router_path_param(void);               // /users/:id → extracts id
void test_router_wildcard(void);                 // /static/* matches /static/foo/bar
void test_router_method_dispatch(void);          // GET vs POST same path
void test_router_no_match(void);                 // 404
void test_router_longest_prefix(void);           // /api/v1/ over /api/
void test_router_path_normalization(void);       // //foo/../bar → /bar
void test_router_path_traversal_blocked(void);   // /../etc/passwd → rejected
void test_router_null_byte_blocked(void);        // /foo%00bar → rejected
```

---

### Task 4.2: Middleware Chain

**Files:**
- Create: `src/core/io_middleware.h`
- Create: `src/core/io_middleware.c`
- Create: `tests/unit/test_io_middleware.c`
- Modify: `CMakeLists.txt`

**Implementation:**

```c
typedef int (*io_middleware_fn)(io_request_t *req, io_response_t *resp,
                                 void *ctx, io_next_fn next);

[[nodiscard]] int io_middleware_add(io_server_t *srv, io_middleware_fn fn, void *ctx);
[[nodiscard]] int io_middleware_add_route(io_server_t *srv, const char *prefix,
                                          io_middleware_fn fn, void *ctx);
```

Chain execution: middleware[0] → middleware[1] → ... → handler. Each middleware
calls `next(req, resp)` to continue chain, or returns directly to short-circuit.

**Tests (8):**
```c
void test_middleware_chain_order(void);        // A → B → C → handler
void test_middleware_short_circuit(void);      // A returns 401, B never called
void test_middleware_modify_request(void);     // add header before handler
void test_middleware_modify_response(void);    // add header after handler
void test_middleware_per_route(void);          // /api/* middleware, not /health
void test_middleware_empty_chain(void);        // no middleware → direct handler
void test_middleware_error_propagation(void);  // handler error propagates
void test_middleware_ctx_passed(void);         // user context available
```

---

### Task 4.3: Built-in Middleware

**Files:**
- Create: `src/middleware/io_cors.h` + `io_cors.c`
- Create: `src/middleware/io_ratelimit.h` + `io_ratelimit.c`
- Create: `src/middleware/io_security.h` + `io_security.c`
- Create: `src/middleware/io_auth.h` + `io_auth.c`
- Create: `tests/unit/test_io_cors.c`
- Create: `tests/unit/test_io_ratelimit.c`
- Create: `tests/unit/test_io_security.c`
- Create: `tests/unit/test_io_auth.c`
- Modify: `CMakeLists.txt`

**CORS (100-200 LOC):**
```c
typedef struct {
    const char **allowed_origins;
    const char **allowed_methods;
    const char **allowed_headers;
    bool allow_credentials;
    uint32_t max_age_seconds;
} io_cors_config_t;

[[nodiscard]] io_middleware_fn io_cors_middleware(const io_cors_config_t *cfg);
```

**Rate limiting (200-400 LOC) — token bucket per IP:**
```c
typedef struct {
    uint32_t requests_per_second;
    uint32_t burst;
} io_ratelimit_config_t;

[[nodiscard]] io_middleware_fn io_ratelimit_middleware(const io_ratelimit_config_t *cfg);
```

**Security headers (150-300 LOC):**
```c
typedef struct {
    const char *csp;                    /* Content-Security-Policy */
    bool hsts;                          /* Strict-Transport-Security */
    uint32_t hsts_max_age;
    const char *frame_options;          /* DENY or SAMEORIGIN */
    const char *referrer_policy;
} io_security_config_t;

[[nodiscard]] io_middleware_fn io_security_middleware(const io_security_config_t *cfg);
```

**Auth (200-400 LOC):**
```c
typedef bool (*io_auth_verify_fn)(const char *credentials, void *ctx);

[[nodiscard]] io_middleware_fn io_auth_basic(io_auth_verify_fn verify, void *ctx);
[[nodiscard]] io_middleware_fn io_auth_bearer(io_auth_verify_fn verify, void *ctx);
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
- Create: `src/static/io_static.h`
- Create: `src/static/io_static.c`
- Create: `tests/unit/test_io_static.c`
- Modify: `CMakeLists.txt`

**Implementation:**

```c
typedef struct {
    const char *root_dir;          /* document root */
    bool directory_listing;        /* default false */
    bool etag;                     /* ETag generation */
    uint32_t max_age_default;      /* Cache-Control max-age */
} io_static_config_t;

[[nodiscard]] int io_route_static(io_server_t *srv, const char *prefix,
                                    const io_static_config_t *cfg);
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
- Create: `src/static/io_spa.h`
- Create: `src/static/io_spa.c`
- Create: `tests/unit/test_io_spa.c`
- Modify: `CMakeLists.txt`

**Implementation:**

```c
typedef struct {
    const char *root_dir;
    const char *index_file;                /* default "index.html" */
    const char **api_prefixes;             /* paths excluded from fallback */
    uint32_t api_prefix_count;
    const char *immutable_pattern;         /* regex for hashed assets */
} io_spa_config_t;

#define IO_STATIC_SPA (1 << 0)  /* flag for io_route_static() */
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
- Create: `src/static/io_compress.h`
- Create: `src/static/io_compress.c`
- Create: `tests/unit/test_io_compress.c`
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
} io_compress_config_t;

[[nodiscard]] io_middleware_fn io_compress_middleware(const io_compress_config_t *cfg);
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

---

## Sprint 6: WebSocket & SSE (2-3 weeks)

**Goal:** WebSocket with RFC 6455 compliance, SSE with heartbeat.

### Task 6.1: WebSocket

**Files:**
- Create: `src/ws/io_websocket.h`
- Create: `src/ws/io_websocket.c`
- Create: `tests/unit/test_io_websocket.c`
- Modify: `CMakeLists.txt`

**Implementation:**

```c
typedef struct {
    void (*on_open)(io_ws_conn_t *ws, void *ctx);
    void (*on_message)(io_ws_conn_t *ws, const uint8_t *data,
                        size_t len, bool is_text, void *ctx);
    void (*on_close)(io_ws_conn_t *ws, uint16_t code,
                      const char *reason, void *ctx);
    void (*on_ping)(io_ws_conn_t *ws, const uint8_t *data,
                     size_t len, void *ctx);
    uint32_t ping_interval_ms;
    uint32_t pong_timeout_ms;
    size_t max_message_size;
} io_ws_config_t;

[[nodiscard]] int io_ws_upgrade(io_request_t *req, io_response_t *resp,
                                 const io_ws_config_t *cfg, void *ctx);
[[nodiscard]] int io_ws_send_text(io_ws_conn_t *ws, const char *msg, size_t len);
[[nodiscard]] int io_ws_send_binary(io_ws_conn_t *ws, const uint8_t *data, size_t len);
[[nodiscard]] int io_ws_close(io_ws_conn_t *ws, uint16_t code, const char *reason);
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
- Create: `src/ws/io_sse.h`
- Create: `src/ws/io_sse.c`
- Create: `tests/unit/test_io_sse.c`
- Modify: `CMakeLists.txt`

**Implementation:**

```c
typedef struct {
    const char *event;    /* event type (optional) */
    const char *data;     /* event data (required) */
    const char *id;       /* event ID (optional) */
    uint32_t retry_ms;    /* reconnect hint (optional, 0 = omit) */
} io_sse_event_t;

[[nodiscard]] int io_sse_start(io_request_t *req, io_response_t *resp,
                                io_sse_stream_t **stream);
[[nodiscard]] int io_sse_send(io_sse_stream_t *stream, const io_sse_event_t *event);
[[nodiscard]] int io_sse_send_comment(io_sse_stream_t *stream, const char *comment);
void io_sse_close(io_sse_stream_t *stream);
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
- Create: `src/http/io_http2.h`
- Create: `src/http/io_http2.c`
- Create: `tests/unit/test_io_http2.c`
- Modify: `CMakeLists.txt`

**Implementation:**

```c
typedef struct {
    uint32_t max_concurrent_streams;   /* default 100 */
    uint32_t initial_window_size;      /* default 65535 */
    uint32_t max_frame_size;           /* default 16384 */
    uint32_t max_header_list_size;     /* default 8192 */
} io_http2_config_t;

[[nodiscard]] io_http2_session_t *io_http2_session_create(
    io_conn_t *conn, const io_http2_config_t *cfg);
void io_http2_session_destroy(io_http2_session_t *session);
[[nodiscard]] int io_http2_on_recv(io_http2_session_t *session,
                                    const uint8_t *data, size_t len);
[[nodiscard]] int io_http2_flush(io_http2_session_t *session);
```

nghttp2 callback integration:
- `on_begin_headers_callback` → allocate io_request_t per stream
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
void test_http2_goaway(void);                   // graceful shutdown
void test_http2_rst_stream(void);               // stream error
void test_http2_max_concurrent_streams(void);   // reject excess streams
```

---

### Task 7.2: ALPN-Based Protocol Selection

**Files:**
- Modify: `src/tls/io_tls.c` (ALPN callback)
- Modify: `src/core/io_conn.c` (protocol dispatch after ALPN)
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
- Create: `src/http/io_quic.h`
- Create: `src/http/io_quic.c`
- Create: `src/tls/io_tls_quic.h`
- Create: `src/tls/io_tls_quic.c`
- Create: `tests/unit/test_io_quic.c`
- Modify: `CMakeLists.txt`

**Implementation:**

```c
typedef struct {
    uint32_t max_streams_bidi;       /* default 100 */
    uint32_t initial_max_data;       /* default 1MB */
    uint64_t idle_timeout_ms;        /* default 30000 */
    bool enable_0rtt;
} io_quic_config_t;

[[nodiscard]] io_quic_server_t *io_quic_server_create(
    const io_quic_config_t *cfg, io_tls_ctx_t *tls);
void io_quic_server_destroy(io_quic_server_t *srv);
[[nodiscard]] int io_quic_on_recv(io_quic_server_t *srv,
                                    const uint8_t *data, size_t len,
                                    const struct sockaddr *remote);
[[nodiscard]] int io_quic_flush(io_quic_server_t *srv);
```

Uses `ngtcp2_crypto_wolfssl_configure_server_context()` for QUIC crypto.
UDP socket management via io_uring recv/send.

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
```

---

### Task 8.2: HTTP/3 (nghttp3)

**Files:**
- Create: `src/http/io_http3.h`
- Create: `src/http/io_http3.c`
- Create: `tests/unit/test_io_http3.c`
- Modify: `CMakeLists.txt`

**Implementation:**

```c
[[nodiscard]] io_http3_session_t *io_http3_session_create(
    io_quic_conn_t *quic_conn, const io_http2_config_t *cfg);
void io_http3_session_destroy(io_http3_session_t *session);
[[nodiscard]] int io_http3_on_stream_data(io_http3_session_t *session,
                                           int64_t stream_id,
                                           const uint8_t *data, size_t len);
```

nghttp3 callbacks → io_request_t → router → handler → nghttp3 response submission.

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
- Create: `src/core/io_json.h`
- Create: `src/core/io_json.c`
- Create: `tests/unit/test_io_json.c`
- Modify: `CMakeLists.txt`

**Implementation:**

```c
[[nodiscard]] int io_respond_json(io_response_t *resp, uint16_t status,
                                   const char *json);
[[nodiscard]] int io_respond_json_obj(io_response_t *resp, uint16_t status,
                                       yyjson_mut_doc *doc);
[[nodiscard]] yyjson_doc *io_request_json(io_request_t *req);
[[nodiscard]] int io_respond_error(io_response_t *resp, uint16_t status,
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
- Create: `src/middleware/io_logging.h`
- Create: `src/middleware/io_logging.c`
- Create: `tests/unit/test_io_logging.c`
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
- Create: `src/middleware/io_metrics.h`
- Create: `src/middleware/io_metrics.c`
- Create: `tests/unit/test_io_metrics.c`
- Modify: `CMakeLists.txt`

**Implementation:**

```
# HELP io_http_requests_total Total HTTP requests
# TYPE io_http_requests_total counter
io_http_requests_total{method="GET",status="200"} 1234

# HELP io_http_connections_active Active connections
# TYPE io_http_connections_active gauge
io_http_connections_active 42

# HELP io_http_request_duration_seconds Request duration
# TYPE io_http_request_duration_seconds histogram
io_http_request_duration_seconds_bucket{le="0.01"} 900
```

Built-in `/metrics` endpoint + `/health` + `/ready`.

**Tests (6):**
```c
void test_metrics_counter_increment(void);
void test_metrics_gauge_set(void);
void test_metrics_histogram_observe(void);
void test_metrics_text_exposition(void);
void test_metrics_health_endpoint(void);
void test_metrics_ready_endpoint(void);
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
| 1 | io_uring + Server Skeleton | 37 |
| 2 | wolfSSL TLS Integration | 15 |
| 3 | HTTP/1.1 + PROXY Protocol | 40 |
| 4 | Router + Middleware | 42 |
| 5 | Static Files + SPA + Compression | 26 |
| 6 | WebSocket + SSE | 20 |
| 7 | HTTP/2 (nghttp2) | 16 |
| 8 | HTTP/3 (ngtcp2 + nghttp3) | 21 |
| 9 | JSON + Logging + Metrics | 20 |
| 10 | Fuzz + Bench + Docs | 5 fuzz targets |
| **Total** | | **~237 tests + 5 fuzz targets** |

## Timeline

| Sprint | Duration | Milestone |
|--------|----------|-----------|
| S1 | 3-4 weeks | io_uring event loop, basic server |
| S2 | 2-3 weeks | TLS handshake over io_uring |
| S3 | 3-4 weeks | Working HTTP/1.1 server (usable MVP) |
| S4 | 2-3 weeks | Router + middleware (production-ready HTTP/1.1) |
| S5 | 2-3 weeks | Static files + SPA serving |
| S6 | 2-3 weeks | WebSocket + SSE |
| S7 | 3-4 weeks | HTTP/2 support |
| S8 | 4-5 weeks | HTTP/3 + QUIC support |
| S9 | 2 weeks | API, logging, metrics |
| S10 | 3-4 weeks | Stabilization, benchmarks, docs |
| **Total** | **~26-35 weeks** | **v0.1.0 release** |

## Critical Dependencies

1. **liburing 2.7+** — multishot accept, provided buffers
2. **wolfSSL 5.8+** — TLS 1.3, QUIC crypto
3. **picohttpparser** — vendored (MIT, ~800 LOC)
4. **nghttp2** — system package (Sprint 7)
5. **ngtcp2 + ngtcp2_crypto_wolfssl** — system/build (Sprint 8)
6. **nghttp3** — system package (Sprint 8)
7. **yyjson** — system package (Sprint 9)
8. **zlib + brotli** — system packages (Sprint 5)

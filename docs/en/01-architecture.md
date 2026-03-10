# iohttp Architecture

## Overview

iohttp is a production-grade embedded HTTP server library in C23, built as a modular
composition of battle-tested protocol libraries on top of Linux io_uring and wolfSSL.

**Design philosophy:** Own the I/O engine and glue; delegate protocol parsing to
proven libraries. Write ~14-23K LOC of integration code, not 80K LOC of protocol
implementation.

---

## Core Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                      User Application                       │
│   ioh_server_create() → ioh_route_add() → ioh_server_run()    │
├─────────────────────────────────────────────────────────────┤
│                     Public C API (ioh_*)                      │
├──────────┬──────────┬──────────┬───────────┬────────────────┤
│  Router  │Middleware│  Static  │ WebSocket │      SSE       │
│  (trie)  │  chain   │  files   │ RFC 6455  │  text/stream   │
├──────────┴──────────┴──────────┴───────────┴────────────────┤
│                    HTTP Protocol Layer                       │
│  ┌──────────────┬───────────────┬──────────────────────┐    │
│  │  HTTP/1.1    │    HTTP/2     │      HTTP/3          │    │
│  │picohttpparser│   nghttp2     │  ngtcp2 + nghttp3    │    │
│  │  (stateless) │  (callbacks)  │  (wolfSSL crypto)    │    │
│  └──────────────┴───────────────┴──────────────────────┘    │
├─────────────────────────────────────────────────────────────┤
│                    TLS Layer (wolfSSL)                       │
│  TLS 1.3 · mTLS · ALPN · SNI · Session Resumption · QUIC   │
├─────────────────────────────────────────────────────────────┤
│                  I/O Engine (io_uring)                       │
│  Multishot Accept · Provided Buffers · Zero-Copy Send       │
│  Ring-Based Timers · DEFER_TASKRUN · REGISTER_RESTRICTIONS  │
├─────────────────────────────────────────────────────────────┤
│                    Linux Kernel 6.7+                         │
└─────────────────────────────────────────────────────────────┘
```

---

## Module Decomposition

### 1. I/O Engine (`src/core/`)

The foundation: io_uring event loop driving all I/O. Single-reactor (dev mode) or
multi-reactor ring-per-thread (production mode).

| Component | LOC | Description |
|-----------|-----|-------------|
| `ioh_loop.{h,c}` | ~500 | io_uring ring setup, SQE submission, CQE reaping, timer wheel |
| `ioh_server.{h,c}` | ~1055 | Server lifecycle, linked timeouts, signalfd shutdown, request limits, request ID |
| `ioh_buffer.{h,c}` | ~250 | Provided buffer ring management, buffer pool |
| `ioh_conn.{h,c}` | ~385 | Connection state machine, timeout phase tracking, PROXY state |
| `ioh_log.{h,c}` | ~165 | Structured logging with levels, custom sink callbacks |

**Key io_uring features used:**
- `IORING_OP_ACCEPT` with `IORING_ACCEPT_MULTISHOT` — one SQE accepts all connections
- `IOSQE_CQE_SKIP_SUCCESS` — suppress CQE on successful multishot accept (reduces CQ pressure)
- `IORING_OP_RECV` with provided buffer rings — kernel picks buffer from pool
- `IORING_OP_SEND_ZC` — zero-copy send for payloads > 2 KiB (kernel 6.0+)
- `IORING_OP_LINK_TIMEOUT` — linked timeouts for keepalive/header/body deadlines
- `IORING_OP_SPLICE` / `IORING_OP_SENDFILE` — zero-copy static file serving
- `IORING_REGISTER_BUFFERS` — pinned memory for DMA acceleration on recv/send
- `IORING_REGISTER_FILES` — registered file descriptors, skip fd table lookup per I/O
- `io_uring_setup_buf_ring` — provided buffer ring for automatic recv buffer allocation
- `IORING_SETUP_SQPOLL` — optional kernel-side SQ polling (NOT default; DEFER_TASKRUN preferred)
- `IORING_REGISTER_RESTRICTIONS` — opcode whitelist for seccomp bypass mitigation
- `IORING_OP_MSG_RING` — inter-ring fd/message passing for multi-reactor connection handoff

**No epoll fallback.** io_uring is mandatory. Minimum kernel: 6.7+.

**io_uring restriction registration:** At startup, after ring creation and buffer setup
but before accepting connections, the server MUST call `IORING_REGISTER_RESTRICTIONS` to
whitelist only the needed opcodes: `RECV`, `SEND`, `SEND_ZC`, `ACCEPT`, `TIMEOUT`, `CANCEL`,
`MSG_RING`, `SPLICE`, `LINK_TIMEOUT`. This is critical because io_uring operations bypass
seccomp BPF filters — they use the shared memory ring, not syscalls. Without opcode
restrictions, a compromised process could issue arbitrary io_uring operations that seccomp
cannot intercept.

**CVE-2024-0582 (provided buffer ring UAF):** Kernels 6.4–6.6.4 have a use-after-free
vulnerability in provided buffer rings. The minimum kernel 6.7 requirement avoids this CVE.

**SQPOLL vs DEFER_TASKRUN:** `IORING_SETUP_SQPOLL` is optional and NOT enabled by default.
The preferred mode for HTTP servers is `IORING_SETUP_DEFER_TASKRUN | IORING_SETUP_SINGLE_ISSUER`,
which is better suited for bursty traffic patterns. SQPOLL burns a dedicated CPU core polling
the submission ring and is mutually exclusive with `DEFER_TASKRUN`. Reserve SQPOLL for
latency-critical workloads where dedicated CPU cost is acceptable.

**SEND_ZC dynamic threshold heuristic:**
- Regular async `IORING_OP_SEND` for payloads < 1–2 KiB (headers, small JSON responses)
- `IORING_OP_SEND_ZC` for payloads > 2 KiB (large responses, file data, streaming bodies)
- `IORING_OP_SPLICE` for static files (page cache → socket, zero user-space buffer mapping)
- Use `IORING_SEND_ZC_REPORT_USAGE` flag to detect when kernel falls back to copy (e.g.,
  loopback interface), allowing runtime adaptation of the threshold
- Note: `SEND_ZC` generates 2 CQEs per operation — one for completion, one for zero-copy
  notification. The CQE reaping loop must handle both.

**Multi-reactor architecture (production):**
- One io_uring ring per worker thread
- Each worker owns its connection pool and buffer locality
- Listener strategy: SO_REUSEPORT (per-worker) or shared accept + handoff
- Connection does NOT migrate between rings in normal operation
- Strict ownership: each connection has exactly one owner thread

**CQE dispatch:** typed user_data packing `(conn_id << 8) | op_type` for fast O(1)
discrimination of accept/recv/send/timeout/file/tls operations. No heap allocations.

**Connection state machine:**
```
ACCEPTING → PROXY_HEADER → TLS_HANDSHAKE → PROTOCOL_NEGOTIATION
  → HTTP_ACTIVE → DRAINING → CLOSING → CLOSED
       ↓               ↓
  WEBSOCKET_ACTIVE  SSE_ACTIVE
```

### 2. TLS Layer (`src/tls/`)

wolfSSL native API integration (NOT OpenSSL compatibility layer).

| Component | LOC | Description |
|-----------|-----|-------------|
| `ioh_tls.{h,c}` | ~460 | wolfSSL context, custom I/O callbacks for io_uring |
| `ioh_tls_quic.{h,c}` | planned | QUIC crypto via ngtcp2_crypto_wolfssl |

**I/O callback pattern for io_uring:**
```c
// wolfSSL reads from application-managed cipher buffer
// io_uring recv → cipher_buf → wolfSSL_read() → plaintext → HTTP parser
// HTTP response → wolfSSL_write() → cipher_buf → io_uring send
//
// CRITICAL: wolfSSL_CTX_set_mode(ctx, SSL_MODE_ENABLE_PARTIAL_WRITE)
// io_uring is non-blocking — must handle WANT_READ/WANT_WRITE manually
```

**I/O buffer serialization:** wolfSSL has a single I/O buffer per `WOLFSSL` object. This
means `wolfSSL_read()` and `wolfSSL_write()` on the same `WOLFSSL` object MUST be serialized
— concurrent calls corrupt internal state. In the ring-per-thread architecture with strict
connection ownership, this is naturally satisfied (one thread owns each connection, so reads
and writes are sequential within the event loop).

**SIGPIPE handling:** `signal(SIGPIPE, SIG_IGN)` is REQUIRED at startup. While io_uring
returns errors on write to a closed socket, wolfSSL may trigger `SIGPIPE` through its
internal write calls (the custom I/O callbacks ultimately invoke kernel writes). Without
this, the server process can be killed by an unhandled `SIGPIPE` when a client disconnects
mid-response.

**Supported features:**
- TLS 1.3 (primary), TLS 1.2 (optional compat)
- mTLS with CRL checking
- ALPN negotiation (h2, http/1.1)
- SNI callbacks for multi-tenant
- Session tickets with configurable TTL
- Certificate reload without restart
- OCSP stapling (optional)
- QUIC crypto for HTTP/3 via ngtcp2_crypto_wolfssl

**wolfSSL license note:** wolfSSL's license situation requires clarification before release.
The GitHub `LICENSING` file states GPLv2, wolfssl.com/license states GPLv3, and the wolfSSL
manual states GPLv2. If wolfSSL is strictly GPLv2 (not GPLv2-or-later), it is incompatible
with GPLv3 code — the two licenses cannot be combined in a single work. Recommendation:
clarify the exact license terms with wolfSSL Inc. or acquire a commercial license before
any public release of iohttp.

### 3. HTTP Protocol Layer (`src/http/`)

Three protocol implementations sharing a unified request/response abstraction.

| Component | LOC | Description |
|-----------|-----|-------------|
| `ioh_http1.{h,c}` | ~540 | picohttpparser wrapper, request object, chunked TE |
| `ioh_http2.{h,c}` | ~770 | nghttp2 session, stream mux, HPACK, flow control |
| `ioh_http3.{h,c}` | planned | ngtcp2 QUIC transport + nghttp3 HTTP/3 + QPACK + CID demux |
| `ioh_request.{h,c}` | ~350 | Unified request abstraction across protocols |
| `ioh_response.{h,c}` | ~330 | Response builder, header serialization |
| `ioh_multipart.{h,c}` | ~360 | Multipart form-data parsing (RFC 2046) |
| `ioh_proxy_proto.{h,c}` | ~370 | PROXY protocol v1/v2 decoder |

**Unified request flow:**
```
Bytes from network
    ↓
PROXY protocol decode (if enabled on listener)
    ↓
wolfSSL decrypt (if TLS)
    ↓
ALPN → protocol-specific parser
    ↓
Unified ioh_request_t
    ↓
Router → Middleware → Handler
    ↓
ioh_response_t → protocol-specific framing → wolfSSL encrypt → io_uring send
```

**HTTP/2 graceful shutdown (two-phase GOAWAY):**
1. Send GOAWAY with `last_stream_id = 2^31-1` (signal: no new streams)
2. Wait at least one RTT for in-flight requests to arrive
3. Send final GOAWAY with actual last processed stream ID
4. Wait until `nghttp2_session_want_read() == 0 && nghttp2_session_want_write() == 0`
5. Close connection via io_uring

**HTTP/3 / QUIC specifics:**
- **CID demultiplexing:** Multishot UDP recv delivers mixed packets from all clients.
  Hash table maps Connection ID → `ngtcp2_conn` for O(1) packet routing.
- **UDP GSO/GRO:** GSO batches multiple QUIC packets into one sendmsg via `UDP_SEGMENT`
  cmsg. GRO coalesces received packets. ngtcp2 supports GSO natively via
  `ngtcp2_conn_write_aggregate_pkt()`.
- **Congestion control:** Configurable — CUBIC (default), NewReno, BBRv2. BBRv2 gives
  ~21% better p99 latency on lossy/mobile networks.
- **Connection migration:** ngtcp2 fully supports QUIC path migration. Server handles
  PATH_CHALLENGE/PATH_RESPONSE and updates CID-to-connection mapping.
- **`SO_REUSEPORT`** for multiple worker threads receiving on the same UDP port.
- **Graceful shutdown:** `nghttp3_conn_shutdown()` → reject new streams → poll
  `nghttp3_conn_is_drained()` → close QUIC connection when all streams complete.

### 4. Router (`src/router/`)

| Component | LOC | Description |
|-----------|-----|-------------|
| `ioh_radix.{h,c}` | ~530 | Radix trie (compressed prefix tree), internal |
| `ioh_router.{h,c}` | ~530 | Per-method trees, auto-405/HEAD, path correction |
| `ioh_route_group.{h,c}` | ~310 | Nested groups with prefix composition |
| `ioh_route_inspect.{h,c}` | ~130 | Route walking, introspection, liboas binding |
| `ioh_middleware.{h,c}` | ~190 | Middleware chain, error handler, next() pattern |

**Design heritage:** Radix trie + per-method trees (httprouter), static > param > wildcard
priority (httprouter/echo/bunrouter), handler-returns-error (bunrouter/echo), route groups
(Express.Router), auto-405/HEAD (httprouter/FastRoute), conflict detection (matchit/Axum),
route introspection (gorilla/mux), metadata attachment for OpenAPI (FastAPI).

**Routing features:**
- **Radix trie** with compressed prefix sharing, separate tree per HTTP method
- **Deterministic priority** (order-independent): static > `:param` > `*wildcard`
- **Method-specific registration**: `ioh_router_get()`, `ioh_router_post()`, etc.
- **Path parameters**: `/api/users/:id/config` — typed extraction (string, i64, u64, bool)
- **Wildcard routes**: `/static/*path` — captures remaining path
- **Nested route groups**: prefix composition + per-group middleware inheritance
- **Auto-405 Method Not Allowed**: path matches another method → 405 + `Allow` header
- **Auto-HEAD**: HEAD falls back to GET handler if no explicit HEAD route
- **Trailing slash redirect**: `/users/` ↔ `/users` → 301
- **Path auto-correction**: `//foo/../bar` → `/bar` → 301
- **Conflict detection**: `/:id` + `/:name` on same level → error at registration
- **Route introspection**: `ioh_router_walk()` for docs generation, liboas binding
- **Route metadata**: extensible `ioh_route_opts_t` with `oas_operation_t*` for liboas
- **Custom handlers**: configurable 404 and 405 handlers
- **Centralized error handling**: handler returns int (0 or -errno) → error handler

### 5. Middleware (`src/middleware/`)

| Component | LOC | Description |
|-----------|-----|-------------|
| `ioh_cors.{h,c}` | ~100 | CORS preflight + headers |
| `ioh_ratelimit.{h,c}` | ~200 | Token bucket per IP |
| `ioh_auth.{h,c}` | ~170 | Basic, Bearer, JWT hooks |
| `ioh_security.{h,c}` | ~220 | CSP, HSTS, X-Frame-Options, nosniff |
| `ioh_logging.{h,c}` | planned | Structured JSON access/error logs |
| `ioh_metrics.{h,c}` | planned | Lock-free thread-local Prometheus metrics |

**Metrics architecture (lock-free):** Each worker thread maintains a thread-local metrics
registry aligned to 64-byte cache line boundaries. No atomic operations in the request
hot path — counters are incremented locally. On `/metrics` scrape, a dedicated handler
iterates all thread-local blocks, aggregates values without locks, and serializes to
Prometheus text exposition format. This eliminates cacheline bouncing that degrades
performance by 30-40% with shared atomic counters.

**Standard metrics:**
- `ioh_http_requests_total{method,status}` — counter
- `ioh_http_request_duration_seconds` — histogram
- `ioh_http_connections_active{protocol}` — gauge (h1/h2/h3)
- `ioh_tls_handshake_duration_seconds` — histogram
- `io_uring_sqe_submitted_total` — counter
- `ioh_bufpool_available` — gauge

**Health endpoints:** `/health` (liveness, always 200), `/ready` (readiness: TLS certs,
buffer pools, connection limits).

### 6. Static Files (`src/static/`)

| Component | LOC | Description |
|-----------|-----|-------------|
| `ioh_static.{h,c}` | ~380 | File serving, MIME, ETag, Range, sendfile |
| `ioh_spa.{h,c}` | ~180 | SPA fallback, API prefix exclusion |
| `ioh_compress.{h,c}` | ~380 | gzip/brotli streaming + precompressed .gz/.br |
| `ioh_embed.{h,c}` | planned | C23 #embed for bundled assets |

### 7. WebSocket & SSE (`src/ws/`)

| Component | LOC | Description |
|-----------|-----|-------------|
| `ioh_websocket.{h,c}` | ~330 | RFC 6455 via wslay, frame parse, mask, ping/pong, fragmentation |
| `ioh_sse.{h,c}` | ~260 | SSE format, heartbeat, Last-Event-ID |

### 8. liboas Integration (`src/middleware/ioh_oas.c`)

liboas is a **separate project** — an OpenAPI 3.2.0 library for C23. iohttp provides
integration points via an adapter middleware, NOT a built-in OpenAPI engine.

| Component | LOC | Description |
|-----------|-----|-------------|
| `ioh_oas.{h,c}` | planned | liboas adapter middleware, mount/publish helpers |

**Integration architecture (one route lookup):**
1. At startup, liboas compiles OpenAPI document → `oas_compiled_api_t`
2. Each iohttp route stores `oas_operation_t*` via `ioh_route_opts_t.oas_operation`
3. At runtime, iohttp router finds route (one lookup) → middleware receives already-matched operation
4. liboas middleware validates request/response against operation schema

**What iohttp provides to liboas:**
- `ioh_request_t` → `oas_runtime_request_t` mapping
- `ioh_response_t` → `oas_runtime_response_t` mapping
- `ioh_tls_peer_info_t` → `oas_security_ctx_t` (TLS/mTLS metadata)
- `ioh_conn_info_t` → real client IP (after PROXY protocol decode)
- Pre-handler request validation hook
- Post-handler response validation hook
- `/openapi.json` publish helper

---

## Data Flow

### HTTP/1.1 Request

```
1. io_uring multishot accept → new fd
2. (optional) Read PROXY protocol header → extract real client IP
3. wolfSSL_accept() via custom I/O callbacks
4. io_uring recv → provided buffer → wolfSSL_read() → plaintext
5. picohttpparser parse → ioh_request_t
6. Router lookup → middleware chain → handler
7. Handler writes ioh_response_t
8. Serialize headers → wolfSSL_write() → io_uring send
```

### HTTP/2 Request

```
1-3. Same as HTTP/1.1 (ALPN selects h2)
4. io_uring recv → wolfSSL_read() → nghttp2_session_mem_recv()
5. nghttp2 callbacks fire per-stream → ioh_request_t per stream
6. Router → middleware → handler (per stream)
7. nghttp2_submit_response() → nghttp2_session_mem_send2()
8. wolfSSL_write() → io_uring send
```

### HTTP/3 Request

```
1. io_uring recv (UDP) → ngtcp2_conn_read_pkt()
2. QUIC decryption via ngtcp2_crypto_wolfssl
3. nghttp3_conn_read_stream() → per-stream callbacks → ioh_request_t
4. Router → middleware → handler
5. nghttp3_conn_submit_response() → ngtcp2_conn_write_pkt()
6. io_uring send (UDP)
```

---

## Memory Model

- **Fixed-size connection pool** — compile-time configurable (default 256)
- **Provided buffer rings** — kernel-managed buffer pool for recv (no alloc in hot path).
  On `-ENOBUFS` (ring exhausted), multishot recv is re-armed after buffer replenishment.
  Double-buffering: one group active in kernel, second being replenished by application.
- **Registered buffers** — `IORING_REGISTER_BUFFERS` pins memory for DMA, avoids page table walks
- **Registered files** — `IORING_REGISTER_FILES` pre-registers fds, skips fd table lookup
- **Arena allocators** — per-request lifetime, freed after response sent
- **Zero-copy paths** — splice for static files, `SEND_ZC` for large responses
- **`[[nodiscard]]`** — on all allocation functions
- **`static_assert`** — compile-time struct size checks (e.g., `sizeof(ioh_conn_t) <= 512`)
- **No uncontrolled heap growth** — all buffers bounded, oversized requests rejected

---

## Configuration Model

Two-level configuration:

1. **Compile-time** — `IOH_MAX_CONNECTIONS`, `IOH_MAX_HEADERS`, `IOH_ENABLE_HTTP2`, etc.
2. **Runtime** — `ioh_server_config_t` struct passed to `ioh_server_create()`

```c
ioh_server_config_t cfg = {
    .listen_addr   = "0.0.0.0",
    .listen_port   = 8080,
    .tls_cert      = "/path/to/cert.pem",
    .tls_key       = "/path/to/key.pem",
    .max_connections = 256,
    .queue_depth   = 256,
    .keepalive_timeout_ms = 30000,
    .header_timeout_ms = 5000,
    .body_timeout_ms = 30000,
    .max_header_size = 8192,
    .max_body_size = 1048576,
    .proxy_protocol = false,
};
```

---

## Security Model

### Security Hardening

**HTTP/2 Rapid Reset protection (CVE-2023-44487):** Enforce `SETTINGS_MAX_CONCURRENT_STREAMS`
via nghttp2 session settings. Without stream limits, the server is vulnerable to the Rapid
Reset attack (client opens and immediately resets streams at high rate), which took down
production servers across the industry in October 2023. Configure a reasonable limit (e.g.,
100–256 concurrent streams) and track RST_STREAM rate — if a client sends RSTs faster than
a threshold (e.g., 100 RSTs/second), terminate the connection.

**Slowloris / Slow POST defense:**
- Header read timeout: 5–15 seconds via `io_uring_prep_link_timeout()` linked to the
  initial recv after accept. If headers are not fully received within the deadline, close
  the connection.
- Minimum transfer rate enforcement: for request bodies, enforce a minimum bytes/second
  rate (e.g., 1 KiB/s). Clients sending data slower than this are likely attack probes.
- Per-IP connection limits: track active connections per source IP, reject new connections
  beyond the limit (e.g., 64 per IP). Combined with the header timeout, this prevents
  resource exhaustion from slow-connection attacks.

| Layer | Protection |
|-------|-----------|
| Network | Connection limits, accept rate limiting |
| PROXY protocol | Trusted proxy allowlist, header validation timeout |
| TLS | wolfSSL hardened config, cipher suite restriction, mTLS |
| HTTP parser | Max header size/count, max URI length, max body size |
| Router | Path normalization, traversal protection, NUL-byte rejection |
| Static files | Document root jail, canonicalization, directory listing disabled |
| Middleware | Rate limiting, auth hooks, CORS |
| Headers | CSP, HSTS, X-Frame-Options, nosniff, Referrer-Policy |
| Memory | Bounded buffers, arena per-request, explicit_bzero for secrets |

---

## LOC Summary

As of Sprint 12 (production hardening done). Source + headers, excluding vendored picohttpparser.

| Category | Own Code | Status | External Libraries |
|----------|----------|--------|--------------------|
| I/O Engine (core) | ~2100 | done | liburing ~3K |
| Structured Logging | ~165 | done | -- |
| TLS Integration | ~460 | done | wolfSSL (large) |
| HTTP/1.1 | ~540 | done | picohttpparser ~800 |
| HTTP/2 | ~770 | done | nghttp2 ~18K |
| HTTP/3 + QUIC | planned | -- | ngtcp2 ~28K + nghttp3 ~12K |
| Router | ~1500 | done | -- |
| Middleware | ~690 | done | -- |
| Static Files + SPA | ~940 | done | zlib, brotli |
| WebSocket + SSE | ~590 | done | wslay ~3K |
| Multipart + PROXY | ~730 | done | -- |
| JSON API + Metrics | planned | -- | yyjson ~8K |
| liboas Adapter | planned | -- | liboas (separate project) |
| Headers (.h) | ~2960 | -- | -- |
| **Total own code** | **~11450** | S1-S12 | |
| **Tests** | **~13830** | 46 tests | |
| **Projected total** | **~15-18K** | all sprints | **~70K+** |

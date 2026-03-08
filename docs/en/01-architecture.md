# iohttp Architecture

## Overview

iohttp is a production-grade embedded HTTP server library in C23, built as a modular
composition of battle-tested protocol libraries on top of Linux io_uring and wolfSSL.

**Design philosophy:** Own the I/O engine and glue; delegate protocol parsing to
proven libraries. Write ~11-18K LOC of integration code, not 80K LOC of protocol
implementation.

---

## Core Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                      User Application                       │
│   io_server_create() → io_route_add() → io_server_run()    │
├─────────────────────────────────────────────────────────────┤
│                     Public C API (io_*)                      │
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
│  Ring-Based Timers · Signal Handling · SQPOLL Mode          │
├─────────────────────────────────────────────────────────────┤
│                    Linux Kernel 6.7+                         │
└─────────────────────────────────────────────────────────────┘
```

---

## Module Decomposition

### 1. I/O Engine (`src/core/`)

The foundation: a single-threaded io_uring event loop driving all I/O.

| Component | Est. LOC | Description |
|-----------|----------|-------------|
| `io_loop.{h,c}` | 800-1200 | io_uring ring setup, SQE submission, CQE reaping, timer wheel |
| `io_server.{h,c}` | 300-500 | Server lifecycle: create, configure, run, shutdown |
| `io_buffer.{h,c}` | 200-400 | Provided buffer ring management, buffer pool |
| `io_conn.{h,c}` | 400-600 | Connection state machine, timeout tracking |

**Key io_uring features used:**
- `IORING_OP_ACCEPT` with `IORING_ACCEPT_MULTISHOT` — one SQE accepts all connections
- `IOSQE_CQE_SKIP_SUCCESS` — suppress CQE on successful multishot accept (reduces CQ pressure)
- `IORING_OP_RECV` with provided buffer rings — kernel picks buffer from pool
- `IORING_OP_SEND_ZC` — zero-copy send for responses > 3KB (kernel 6.0+)
- `IORING_OP_LINK_TIMEOUT` — linked timeouts for keepalive/header/body deadlines
- `IORING_OP_SPLICE` / `IORING_OP_SENDFILE` — zero-copy static file serving
- `IORING_REGISTER_BUFFERS` — pinned memory for DMA acceleration on recv/send
- `IORING_REGISTER_FILES` — registered file descriptors, skip fd table lookup per I/O
- `io_uring_setup_buf_ring` — provided buffer ring for automatic recv buffer allocation
- `IORING_SETUP_SQPOLL` — optional kernel-side SQ polling thread (requires CAP_SYS_ADMIN)

**No epoll fallback.** io_uring is mandatory. Minimum kernel: 6.7+.

**Connection state machine:**
```
ACCEPTING → PROXY_HEADER → TLS_HANDSHAKE → HTTP_ACTIVE → CLOSING → CLOSED
                              ↓
                         WEBSOCKET_ACTIVE
```

### 2. TLS Layer (`src/tls/`)

wolfSSL native API integration (NOT OpenSSL compatibility layer).

| Component | Est. LOC | Description |
|-----------|----------|-------------|
| `io_tls.{h,c}` | 500-800 | wolfSSL context, custom I/O callbacks for io_uring |
| `io_tls_quic.{h,c}` | 200-300 | QUIC crypto via ngtcp2_crypto_wolfssl |

**I/O callback pattern for io_uring:**
```c
// wolfSSL reads from application-managed cipher buffer
// io_uring recv → cipher_buf → wolfSSL_read() → plaintext → HTTP parser
// HTTP response → wolfSSL_write() → cipher_buf → io_uring send
//
// CRITICAL: wolfSSL_CTX_set_mode(ctx, SSL_MODE_ENABLE_PARTIAL_WRITE)
// io_uring is non-blocking — must handle WANT_READ/WANT_WRITE manually
```

**Supported features:**
- TLS 1.3 (primary), TLS 1.2 (optional compat)
- mTLS with CRL checking
- ALPN negotiation (h2, http/1.1)
- SNI callbacks for multi-tenant
- Session tickets with configurable TTL
- Certificate reload without restart
- OCSP stapling (optional)
- QUIC crypto for HTTP/3 via ngtcp2_crypto_wolfssl

### 3. HTTP Protocol Layer (`src/http/`)

Three protocol implementations sharing a unified request/response abstraction.

| Component | Est. LOC | Description |
|-----------|----------|-------------|
| `io_http1.{h,c}` | 400-700 | picohttpparser wrapper, request object, chunked TE |
| `io_http2.{h,c}` | 1500-2500 | nghttp2 session, stream mux, HPACK, flow control |
| `io_http3.{h,c}` | 3000-5000 | ngtcp2 QUIC transport + nghttp3 HTTP/3 + QPACK |
| `io_request.{h,c}` | 300-500 | Unified request abstraction across protocols |
| `io_response.{h,c}` | 300-500 | Response builder, header serialization |
| `io_proxy_proto.{h,c}` | 300-500 | PROXY protocol v1/v2 decoder |

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
Unified io_request_t
    ↓
Router → Middleware → Handler
    ↓
io_response_t → protocol-specific framing → wolfSSL encrypt → io_uring send
```

### 4. Router (`src/core/`)

| Component | Est. LOC | Description |
|-----------|----------|-------------|
| `io_router.{h,c}` | 500-800 | Longest-prefix trie, path params, route groups |
| `io_middleware.{h,c}` | 300-500 | Middleware chain, next() pattern |

**Routing features:**
- Longest-prefix match (trie)
- Path parameters: `/api/users/:id/config`
- Wildcard routes: `/static/*`
- Route groups with per-group middleware: `/api/v1/`
- Per-route auth and permission bitmask
- Method-based dispatch

### 5. Middleware (`src/middleware/`)

| Component | Est. LOC | Description |
|-----------|----------|-------------|
| `io_cors.{h,c}` | 100-200 | CORS preflight + headers |
| `io_ratelimit.{h,c}` | 200-400 | Token bucket per IP |
| `io_auth.{h,c}` | 200-400 | Basic, Bearer, JWT hooks |
| `io_security.{h,c}` | 150-300 | CSP, HSTS, X-Frame-Options, nosniff |
| `io_logging.{h,c}` | 300-500 | Structured JSON access/error logs |
| `io_metrics.{h,c}` | 200-400 | Prometheus text exposition |

### 6. Static Files (`src/static/`)

| Component | Est. LOC | Description |
|-----------|----------|-------------|
| `io_static.{h,c}` | 500-800 | File serving, MIME, ETag, Range, sendfile |
| `io_spa.{h,c}` | 150-300 | SPA fallback, API prefix exclusion |
| `io_compress.{h,c}` | 400-700 | gzip/brotli streaming + precompressed .gz/.br |
| `io_embed.{h,c}` | 200-400 | C23 #embed for bundled assets |

### 7. WebSocket & SSE (`src/ws/`)

| Component | Est. LOC | Description |
|-----------|----------|-------------|
| `io_websocket.{h,c}` | 800-1500 | RFC 6455, frame parse, mask, ping/pong, fragmentation |
| `io_sse.{h,c}` | 150-250 | SSE format, heartbeat, Last-Event-ID |

---

## Data Flow

### HTTP/1.1 Request

```
1. io_uring multishot accept → new fd
2. (optional) Read PROXY protocol header → extract real client IP
3. wolfSSL_accept() via custom I/O callbacks
4. io_uring recv → provided buffer → wolfSSL_read() → plaintext
5. picohttpparser parse → io_request_t
6. Router lookup → middleware chain → handler
7. Handler writes io_response_t
8. Serialize headers → wolfSSL_write() → io_uring send
```

### HTTP/2 Request

```
1-3. Same as HTTP/1.1 (ALPN selects h2)
4. io_uring recv → wolfSSL_read() → nghttp2_session_mem_recv()
5. nghttp2 callbacks fire per-stream → io_request_t per stream
6. Router → middleware → handler (per stream)
7. nghttp2_submit_response() → nghttp2_session_mem_send2()
8. wolfSSL_write() → io_uring send
```

### HTTP/3 Request

```
1. io_uring recv (UDP) → ngtcp2_conn_read_pkt()
2. QUIC decryption via ngtcp2_crypto_wolfssl
3. nghttp3_conn_read_stream() → per-stream callbacks → io_request_t
4. Router → middleware → handler
5. nghttp3_conn_submit_response() → ngtcp2_conn_write_pkt()
6. io_uring send (UDP)
```

---

## Memory Model

- **Fixed-size connection pool** — compile-time configurable (default 256)
- **Provided buffer rings** — kernel-managed buffer pool for recv (no alloc in hot path)
- **Registered buffers** — `IORING_REGISTER_BUFFERS` pins memory for DMA, avoids page table walks
- **Registered files** — `IORING_REGISTER_FILES` pre-registers fds, skips fd table lookup
- **Arena allocators** — per-request lifetime, freed after response sent
- **Zero-copy paths** — splice for static files, `SEND_ZC` for large responses
- **`[[nodiscard]]`** — on all allocation functions
- **`static_assert`** — compile-time struct size checks (e.g., `sizeof(io_conn_t) <= 512`)
- **No uncontrolled heap growth** — all buffers bounded, oversized requests rejected

---

## Configuration Model

Two-level configuration:

1. **Compile-time** — `IO_MAX_CONNECTIONS`, `IO_MAX_HEADERS`, `IO_ENABLE_HTTP2`, etc.
2. **Runtime** — `io_server_config_t` struct passed to `io_server_create()`

```c
io_server_config_t cfg = {
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

| Category | Own Code | External Libraries |
|----------|----------|--------------------|
| I/O Engine | 1700-2700 | liburing ~3K |
| TLS Integration | 700-1100 | wolfSSL (large) |
| HTTP/1.1 | 400-700 | picohttpparser ~800 |
| HTTP/2 | 1500-2500 | nghttp2 ~18K |
| HTTP/3 + QUIC | 3000-5000 | ngtcp2 ~28K + nghttp3 ~12K |
| Router + Middleware | 1750-3100 | — |
| Static Files + SPA | 1250-2200 | zlib, brotli |
| WebSocket + SSE | 950-1750 | — |
| Config + Logging | 700-1200 | yyjson ~8K |
| **Total own code** | **~12K-20K** | |
| **Total with deps** | | **~70K** |

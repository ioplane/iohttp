---
name: iohttp-architecture
description: Use when implementing any iohttp component — provides architecture reference, directory layout, naming conventions, connection state machine, ownership model, and P0-P4 phasing. MANDATORY for all new files and modules.
---

# iohttp Architecture Reference

## Core Principle

iohttp is a **server runtime where io_uring IS the core operating model**, not a server that happens to use io_uring as a backend. All state machines, buffers, timeouts, and lifecycles are designed around SQE/CQE semantics.

## Directory Structure

```
include/iohttp/          # Public API headers ONLY
  io_server.h            # Server lifecycle
  io_request.h           # Unified request abstraction
  io_response.h          # Response builder
  io_router.h            # Router + route groups
  io_middleware.h        # Middleware chain
  io_tls.h               # TLS configuration
  io_conn.h              # Connection info
  io_metrics.h           # Metrics exposition

src/core/                # Core runtime
  io_loop.c              # io_uring ring setup, SQE/CQE, timer wheel
  io_worker.c            # Worker thread, per-worker ring + conn pool
  io_conn.c              # Connection state machine, timeout tracking
  io_timeout.c           # Linked timeout management
  io_buffer.c            # Provided buffer rings, buffer pool
  io_fdreg.c             # Registered files/buffers management
  io_server.c            # Server lifecycle: create, configure, run, shutdown

src/net/                 # Network layer
  io_listener.c          # Listener setup, SO_REUSEPORT
  io_accept.c            # Multishot accept, backpressure
  io_socket.c            # Socket options, nonblock
  io_proxy_proto.c       # PROXY protocol v1/v2 decoder

src/tls/                 # TLS layer (wolfSSL native)
  io_tls_wolfssl.c       # wolfSSL context, I/O callbacks
  io_tls_peer.c          # Peer cert metadata extraction
  io_tls_alpn.c          # ALPN negotiation
  io_tls_quic.c          # QUIC crypto via ngtcp2_crypto_wolfssl

src/http/                # HTTP protocol layer
  io_http1.c             # picohttpparser wrapper
  io_http2.c             # nghttp2 session, stream mux
  io_http3.c             # ngtcp2 + nghttp3
  io_request.c           # Unified request builder
  io_response.c          # Response serialization

src/router/              # Routing (separate from core)
  io_router.c            # Longest-prefix trie, path params
  io_route_group.c       # Route groups, per-group middleware

src/middleware/          # Middleware modules
  io_cors.c, io_auth.c, io_ratelimit.c, io_security.c
  io_logging.c, io_metrics.c, io_oas.c

src/static/              # Static file serving
  io_static.c, io_spa.c, io_compress.c, io_embed.c

src/ws/                  # WebSocket + SSE
  io_websocket.c, io_sse.c
```

## Naming Conventions

| Element | Pattern | Example |
|---------|---------|---------|
| Functions | `io_module_verb_noun()` | `io_loop_submit_sqe()` |
| Types | `io_module_name_t` | `io_conn_state_t` |
| Enums | `IO_MODULE_VALUE` | `IO_CONN_HTTP_ACTIVE` |
| Macros | `IO_MODULE_NAME` | `IO_MAX_CONNECTIONS` |
| Include guards | `IOHTTP_MODULE_FILE_H` | `IOHTTP_CORE_LOOP_H` |
| Public headers | `include/iohttp/io_*.h` | `include/iohttp/io_server.h` |
| Internal headers | `src/module/io_*.h` | `src/core/io_loop.h` |

## Connection State Machine

```
ACCEPTING
  → PROXY_HEADER        (if listener expects PROXY protocol)
  → TLS_HANDSHAKE       (if TLS enabled)
  → PROTOCOL_NEGOTIATION (ALPN → h2/http1.1)
  → HTTP_ACTIVE
      → WS_ACTIVE       (after WebSocket upgrade)
      → SSE_ACTIVE      (after SSE response start)
  → DRAINING            (graceful shutdown)
  → CLOSING
  → CLOSED
```

Each state has an associated timeout (linked timeout via io_uring).

**WARNING: Never use `IOSQE_CQE_SKIP_SUCCESS` with multishot accept** — accept CQE carries the new fd; skipping it loses the connection event entirely.

## Ownership Model

- Each connection is owned by **exactly one reactor thread**
- Owner is responsible for: SQE submission, timeout management, io_conn_t lifecycle, TLS object, protocol parser state, state transitions
- Cross-thread handoff is exceptional, explicit, and rare
- No cross-thread contention on hot path

## Multi-Reactor Architecture

- **Development mode**: single-reactor (single thread, single ring)
- **Production mode**: multi-reactor ring-per-thread
  - One io_uring ring per worker thread
  - Each worker owns its connection pool and buffer locality
  - Listener strategy: SO_REUSEPORT (listener-per-worker) or shared accept + handoff
  - Connection does NOT migrate between rings in normal operation

## CQE Dispatch

user_data must encode operation type for fast discrimination:
- Pack: `(conn_id << 8) | op_type` or slab identifiers
- No heap allocations in CQE dispatch
- Must distinguish: accept, recv, send, timeout, file, tls_helper

## Unified Request/Response

Upper layers work with protocol-independent abstractions:
- `io_request_t` — method, path, headers, body, params
- `io_response_t` — status, headers, body builder
- `io_conn_info_t` — peer addr, proxied addr, trusted proxy flag
- `io_tls_peer_info_t` — TLS version, cipher, ALPN, client cert metadata
- `io_route_match_t` — matched route, params, oas_operation_t pointer

## P0-P4 Implementation Phases

| Phase | Scope | Key Deliverables |
|-------|-------|-----------------|
| P0 | Core runtime | Single-reactor, multishot accept, provided buffers, HTTP/1.1 parser, basic router, wolfSSL integration |
| P1 | Production HTTP/1.1 | Middleware, static files, SPA, PROXY protocol, metrics/logging, request limits, timeouts |
| P2 | Scale-up | Multi-reactor ring-per-thread, zero-copy send, registered files/buffers, mTLS metadata, liboas adapter |
| P3 | HTTP/2 + WS + SSE | HTTP/2 (nghttp2), WebSocket, SSE, advanced observability, graceful drain |
| P4 | HTTP/3 / QUIC | QUIC transport (ngtcp2), HTTP/3 (nghttp3), production hardening |

## liboas Integration Points

- `io_oas.c` middleware adapter
- Route metadata: attach `oas_operation_t *` to route definition
- Pre-handler: request validation hook
- Post-handler: response validation hook
- Publish helper: `/openapi.json` endpoint
- Normalized context: real client IP, TLS/mTLS status, auth results, route params, content type

## context7 Documentation

Fetch up-to-date library API docs:
- `/h2o/picohttpparser` — HTTP/1.1 parser
- `/nghttp2/nghttp2` — HTTP/2 (frames, HPACK, streams)
- `/ngtcp2/ngtcp2` — QUIC transport
- `/ngtcp2/nghttp3` — HTTP/3 + QPACK
- `/tatsuhiro-t/wslay` — WebSocket (RFC 6455)
- `/ngtcp2/sfparse` — Structured Fields (RFC 9651)
- `/ibireme/yyjson` — JSON serialization
- `/wolfssl/wolfssl` — TLS 1.3 / QUIC crypto
- `/axboe/liburing` — io_uring

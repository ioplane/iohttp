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
  ioh_server.h            # Server lifecycle
  ioh_request.h           # Unified request abstraction
  ioh_response.h          # Response builder
  ioh_router.h            # Router + route groups
  ioh_middleware.h        # Middleware chain
  ioh_tls.h               # TLS configuration
  ioh_conn.h              # Connection info
  ioh_metrics.h           # Metrics exposition

src/core/                # Core runtime
  ioh_loop.c              # io_uring ring setup, SQE/CQE, timer wheel
  ioh_worker.c            # Worker thread, per-worker ring + conn pool
  ioh_conn.c              # Connection state machine, timeout tracking
  ioh_timeout.c           # Linked timeout management
  ioh_buffer.c            # Provided buffer rings, buffer pool
  ioh_fdreg.c             # Registered files/buffers management
  ioh_server.c            # Server lifecycle: create, configure, run, shutdown

src/net/                 # Network layer
  ioh_listener.c          # Listener setup, SO_REUSEPORT
  ioh_accept.c            # Multishot accept, backpressure
  ioh_socket.c            # Socket options, nonblock
  ioh_proxy_proto.c       # PROXY protocol v1/v2 decoder

src/tls/                 # TLS layer (wolfSSL native)
  ioh_tls_wolfssl.c       # wolfSSL context, I/O callbacks
  ioh_tls_peer.c          # Peer cert metadata extraction
  ioh_tls_alpn.c          # ALPN negotiation
  ioh_tls_quic.c          # QUIC crypto via ngtcp2_crypto_wolfssl

src/http/                # HTTP protocol layer
  ioh_http1.c             # picohttpparser wrapper
  ioh_http2.c             # nghttp2 session, stream mux
  ioh_http3.c             # ngtcp2 + nghttp3
  ioh_request.c           # Unified request builder
  ioh_response.c          # Response serialization

src/router/              # Routing (separate from core)
  ioh_router.c            # Longest-prefix trie, path params
  ioh_route_group.c       # Route groups, per-group middleware

src/middleware/          # Middleware modules
  ioh_cors.c, ioh_auth.c, ioh_ratelimit.c, ioh_security.c
  ioh_logging.c, ioh_metrics.c, ioh_oas.c

src/static/              # Static file serving
  ioh_static.c, ioh_spa.c, ioh_compress.c, ioh_embed.c

src/ws/                  # WebSocket + SSE
  ioh_websocket.c, ioh_sse.c
```

## Naming Conventions

| Element | Pattern | Example |
|---------|---------|---------|
| Functions | `ioh_module_verb_noun()` | `ioh_loop_submit_sqe()` |
| Types | `ioh_module_name_t` | `ioh_conn_state_t` |
| Enums | `IOH_MODULE_VALUE` | `IOH_CONN_HTTP_ACTIVE` |
| Macros | `IOH_MODULE_NAME` | `IOH_MAX_CONNECTIONS` |
| Include guards | `IOHTTP_MODULE_FILE_H` | `IOHTTP_CORE_LOOP_H` |
| Public headers | `include/iohttp/ioh_*.h` | `include/iohttp/ioh_server.h` |
| Internal headers | `src/module/ioh_*.h` | `src/core/ioh_loop.h` |

## Connection State Machine

```
ACCEPTING → PROXY_HEADER → TLS_HANDSHAKE → PROTOCOL_NEGOTIATION → HTTP_ACTIVE
                                                                      ↓
                                                        WS_ACTIVE / SSE_ACTIVE
                                                                      ↓
                                                    DRAINING → CLOSING → CLOSED
```

- `ACCEPTING → PROXY_HEADER`: only if listener expects PROXY protocol
- `ACCEPTING → TLS_HANDSHAKE`: only if TLS enabled (skips PROXY_HEADER)
- `ACCEPTING → HTTP_ACTIVE`: plaintext without PROXY protocol
- `HTTP_ACTIVE → WS_ACTIVE`: after WebSocket upgrade handshake
- `HTTP_ACTIVE → SSE_ACTIVE`: after SSE response start
- Any active state → `DRAINING`: graceful shutdown signal received
- `DRAINING → CLOSING`: drain complete or drain timeout expired
- `CLOSING → CLOSED`: all pending io_uring ops cancelled and CQEs reaped

Each state has an associated timeout (linked timeout via io_uring).

**WARNING: Never use `IOSQE_CQE_SKIP_SUCCESS` with multishot accept** — accept CQE carries the new fd; skipping it loses the connection event entirely.

## Ownership Model

- Each connection is owned by **exactly one reactor thread**
- Owner is responsible for: SQE submission, timeout management, ioh_conn_t lifecycle, TLS object, protocol parser state, state transitions
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
- `ioh_request_t` — method, path, headers, body, params
- `ioh_response_t` — status, headers, body builder
- `ioh_conn_info_t` — peer addr, proxied addr, trusted proxy flag
- `ioh_tls_peer_info_t` — TLS version, cipher, ALPN, client cert metadata
- `ioh_route_match_t` — matched route, params, oas_operation_t pointer

## P0-P4 Implementation Phases

| Phase | Scope | Key Deliverables |
|-------|-------|-----------------|
| P0 | Core runtime | Single-reactor, multishot accept, provided buffers, HTTP/1.1 parser, basic router, wolfSSL integration |
| P1 | Production HTTP/1.1 | Middleware, static files, SPA, PROXY protocol, metrics/logging, request limits, timeouts |
| P2 | Scale-up | Multi-reactor ring-per-thread, zero-copy send, registered files/buffers, mTLS metadata, liboas adapter |
| P3 | HTTP/2 + WS + SSE | HTTP/2 (nghttp2), WebSocket, SSE, advanced observability, graceful drain |
| P4 | HTTP/3 / QUIC | QUIC transport (ngtcp2), HTTP/3 (nghttp3), production hardening |

## Production Features (MANDATORY for 0.1.0)

### Graceful Shutdown
- Two-phase: stop accepting → drain active connections → close
- HTTP/2: two-phase GOAWAY (advertise last-stream-id=MAX, then real last-stream-id)
- HTTP/3: GOAWAY frame with stream ID
- Configurable drain timeout (default 30s), force-cancel io_uring ops after timeout
- Health check endpoint returns 503 during drain phase

### Health Check Endpoints
- `/health` — liveness (200 OK if event loop running)
- `/ready` — readiness (503 during startup/drain, 200 when accepting)
- `/live` — deep check (io_uring ring responsive, TLS context valid, buffer pools healthy)
- Registered as internal routes, excluded from middleware chain

### Request Context Propagation
- `ioh_request_id_t`: UUID generated per request, available in `ioh_request_t`
- Propagated via `X-Request-Id` response header
- W3C Trace Context: `traceparent`/`tracestate` header pass-through
- Available in structured logging context

### Observability Hooks
- Metrics: active connections, request rate, latency percentiles, error rate, TLS handshake time
- Logging: structured JSON with request_id, client_ip, method, path, status, duration_ms
- Tracing: hooks for OpenTelemetry span creation/propagation (not built-in OTel dependency)

## Critical Missing Features (P0 blockers)

| Feature | Blocks | RFC/Spec |
|---------|--------|----------|
| Graceful shutdown | Production deployment | — |
| Health checks | Kubernetes/load balancer | — |
| Request ID | Debugging, tracing | — |
| Structured logging | Observability | — |
| Circuit breaker | Resilience | — |
| Per-route timeouts | SLA enforcement | — |
| Multipart parser | File upload | RFC 7578 |
| Cookie handling | Sessions, auth | RFC 6265bis |
| HTTP caching | CDN, perf | RFC 9111 |
| Content negotiation | API versioning | RFC 9110 §12 |

## liboas Integration Points

- `ioh_oas.c` middleware adapter
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

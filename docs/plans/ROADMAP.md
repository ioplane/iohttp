# iohttp Roadmap

## Release Target

**v0.1.0** — after Sprint 19

## Sprint Status

| Sprint | Status | Features |
|--------|--------|----------|
| S1-S8 | DONE | Core io_uring, HTTP/1.1, router, TLS, middleware, static files, WebSocket, SSE |
| S9 | DONE | Route metadata for introspection |
| S10 | DONE | HTTP/3 (QUIC via ngtcp2 + nghttp3), Alt-Svc |
| S11 | DONE | Integration pipeline: accept → recv → TLS → parse → route → handler → send → close |
| S12 | DONE | Linked timeouts, logging, request ID, PROXY protocol |
| S13 | DONE | HTTP/2 UAF fix, router fix, ASan green (46/46) |
| S14 | DONE | Project hardening, GitHub infra, SPDX headers |
| S15 | DONE | Health check endpoints, per-route timeouts |
| **S16** | **IN PROGRESS** | Set-Cookie (RFC 6265bis), Vary header, host-based routing |
| S17 | PLANNED | Streaming request body (chunked input), circuit breaker |
| S18 | PLANNED | liboas adapter, OpenAPI, Scalar UI, request validation |
| S19 | PLANNED | W3C trace context, SIGHUP config hot reload |

## P0 Features (ALL COMPLETE)

- Graceful shutdown with drain mode (S11)
- Health check endpoint framework (S15)
- Request ID middleware (S12)
- Structured logging with custom fields and levels (S12)
- Per-route timeout configuration (S15)

## Post-0.1.0

- HTTP/3 Datagrams (RFC 9297)
- WebTransport
- HTTP caching (RFC 9111 compliance)
- Dynamic connection pool scaling

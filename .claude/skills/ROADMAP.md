# iohttp Roadmap

## Current Status

- **Completed:** Sprints 1-15, all P0 features
- **In Progress:** Sprint 16 (Set-Cookie, Vary header, host-based routing)
- **Release Target:** v0.1.0 after Sprint 19

## Sprint Schedule

| Sprint | Status | Features |
|--------|--------|----------|
| S1-S8 | DONE | Core io_uring, HTTP/1.1, router, TLS, middleware, static files, WebSocket, SSE |
| S9 | DONE | Route metadata for introspection |
| S10 | DONE | HTTP/3 (QUIC via ngtcp2 + nghttp3), Alt-Svc |
| S11 | DONE | Integration pipeline |
| S12 | DONE | Linked timeouts, logging, request ID, PROXY protocol |
| S13 | DONE | HTTP/2 UAF fix, router fix, ASan green |
| S14 | DONE | Project hardening, GitHub infra |
| S15 | DONE | Health check endpoints, per-route timeouts |
| **S16** | **IN PROGRESS** | Set-Cookie (RFC 6265bis), Vary header, host-based routing |
| S17 | PLANNED | Streaming request body, circuit breaker |
| S18 | PLANNED | liboas adapter, OpenAPI, Scalar UI |
| S19 | PLANNED | W3C trace context, SIGHUP config reload |

## P0 Features (ALL COMPLETE)

- Graceful shutdown with drain mode
- Health check endpoint framework
- Request ID middleware
- Structured logging
- Per-route timeout configuration

## Post-0.1.0

- HTTP/3 Datagrams (RFC 9297)
- WebTransport
- HTTP caching (RFC 9111)
- Dynamic connection pool scaling

## Reference

- Full roadmap: `docs/plans/2026-03-10-sprints-15-19-roadmap.md`
- Backlog: `docs/plans/BACKLOG.md`

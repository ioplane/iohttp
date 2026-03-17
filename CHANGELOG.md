# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added

- Set-Cookie support (RFC 6265bis)
- Vary header accumulator with deduplication
- Host-based virtual routing

## [0.0.15] - 2026-03-15

### Added

- Health check endpoints (`/health`, `/ready`, `/live`)
- Per-route timeout configuration

## [0.0.14] - 2026-03-14

### Added

- `.clangd` configuration, `.editorconfig`, SECURITY.md
- GitHub infrastructure (workflows, issue templates)
- SPDX license headers
- Fuzz seed corpus

## [0.0.13] - 2026-03-13

### Fixed

- HTTP/2 use-after-free in frame handler
- Router stack-use-after-return
- ASan clean (46/46 tests passing)

## [0.0.12] - 2026-03-12

### Added

- Linked timeouts for request processing
- Request size limits
- signalfd-based graceful shutdown
- Structured logging with custom fields and levels
- Request ID middleware (`X-Request-Id`)
- PROXY protocol v1/v2 support

## [0.0.11] - 2026-03-11

### Added

- Full integration pipeline: TCP accept → recv → TLS → parse → route → handler → send → close

## [0.0.10] - 2026-03-10

### Added

- HTTP/3 support via ngtcp2 + nghttp3
- QUIC transport with wolfSSL crypto
- Alt-Svc header support

## [0.0.9] - 2026-03-09

### Added

- Route metadata for introspection

## [0.0.1-0.0.8] - 2026-03-08

### Added

- Core io_uring event loop and worker threads
- HTTP/1.1 parsing (picohttpparser)
- Radix-trie router with route groups
- wolfSSL TLS 1.3 integration
- Middleware framework (rate limiting, CORS, JWT, security headers)
- Static file serving with `#embed` and SPA fallback
- WebSocket (RFC 6455) support
- Server-Sent Events (SSE) support
- HTTP/2 via nghttp2

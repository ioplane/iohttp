---
name: rfc-reference
description: Use when implementing HTTP, TLS, QUIC, WebSocket, or security features. Provides RFC numbers, sections, and key requirements for iohttp protocol implementation.
---

# RFC Reference for iohttp

## Priority Map

| Priority | RFCs | Domain |
|----------|------|--------|
| P0 — Core | 9110, 9112, 9113, 9114, 9000, 9001, 9002, 8446, 7301, 6455 | HTTP/1.1, /2, /3, QUIC, TLS, WebSocket |
| P1 — Production | 9111, 9218, 6797, 6750, 7519, 9325, 9457, 7301 | Caching, priorities, HSTS, auth, security |
| P2 — Completeness | 8441, 9220, 7692, 7932, 8478, 6265, 8288, 9368, 9369 | WS over h2/h3, compression, cookies, QUICv2 |
| P3 — Extended | 6570, 9221, 9530, 8705, 8297, 9512 | URI templates, datagrams, hints |

## HTTP Core

| RFC | Title | Key Sections |
|-----|-------|-------------|
| **9110** | HTTP Semantics | §8 Content, §9 Methods, §12 Content Negotiation, §14 Range, §15 Status Codes |
| **9111** | HTTP Caching | §5 Cache-Control, §8 Validation (ETag, Last-Modified, Vary) |
| **9112** | HTTP/1.1 | §2.1 Message Format, §6 Chunked TE, §7 Connection Management, §9 Keep-Alive |
| **7233** | Range Requests | §2 Range, §4 Content-Range, Accept-Ranges — resume download, video streaming |
| **7232** | Conditional Requests | §2 If-Match, §3 If-None-Match, §4 If-Modified-Since |

### Request Smuggling Protection (RFC 9112)

- §6.3: Reject duplicate Content-Length headers
- §6.3: Reject Content-Length + Transfer-Encoding together
- §5.2: Reject obs-fold in headers
- §6.3: Reject non-digit Content-Length values
- §6.3: chunked MUST be last Transfer-Encoding
- §3.3: Require Host header for HTTP/1.1

## HTTP/2

| RFC | Title | Key Sections |
|-----|-------|-------------|
| **9113** | HTTP/2 | §4 Frames, §5 Streams, §6.5 SETTINGS, §6.8 GOAWAY |
| **9218** | Extensible Priorities | §4 Priority header (`u=`, `i=`) — replaces HTTP/2 priority trees |
| **8740** | TLS 1.3 with HTTP/2 | Prohibits renegotiation, limits post-handshake auth |

## HTTP/3 & QUIC

| RFC | Title | Key Sections |
|-----|-------|-------------|
| **9114** | HTTP/3 | §6 Frames, §7 Streams, §8 QPACK integration |
| **9204** | QPACK | Header compression for HTTP/3 |
| **9000** | QUIC Transport | §2 Streams, §4 Frames, §8 Connection Migration, §10 Closing |
| **9001** | QUIC-TLS | §4 Handshake, §5 0-RTT, §7 Key Update |
| **9002** | QUIC Loss Detection | §5 RTT Estimation, §6 Loss Detection, §7 Congestion Control |
| **9221** | QUIC Datagrams | Unreliable datagrams (WebTransport foundation) |
| **9287** | Greasing QUIC Bit | Anti-ossification — ngtcp2 supports |
| **9368** | QUIC Version Negotiation | Compatible version negotiation without RTT penalty |
| **9369** | QUIC Version 2 | Changed salt/key derivation, ngtcp2 supports |

## TLS

| RFC | Title | Key Sections |
|-----|-------|-------------|
| **8446** | TLS 1.3 | §2 Handshake, §4 Extensions, §8 0-RTT |
| **7301** | ALPN | §3 Protocol Negotiation — critical for h2/http1.1 selection |
| **6066** | TLS Extensions | §3 SNI, §4 Max Fragment Length, §8 OCSP Stapling |
| **9325** | Secure Use of TLS | Best practices: min cipher suites, weak algorithm prohibition |
| **8879** | TLS Certificate Compression | Compress certificates for faster handshake |
| **8449** | Record Size Limit | Extension to negotiate max record size |

### wolfSSL Implementation Notes

- TLS 1.3 primary (`wolfTLSv1_3_server_method()`), TLS 1.2 minimum
- ALPN: `wolfSSL_CTX_UseALPN()` with wire format `"\x02h2\x08http/1.1"`
- SNI: `wolfSSL_CTX_set_servername_callback()` for multi-tenant
- Session tickets: `wolfSSL_CTX_set_timeout()` for rotation
- mTLS: `wolfSSL_CTX_set_verify(ctx, SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT, cb)`

## WebSocket

| RFC | Title | Key Sections |
|-----|-------|-------------|
| **6455** | WebSocket Protocol | §4 Handshake, §5 Framing, §7 Close, §10 Security |
| **7692** | WS Compression | permessage-deflate extension |
| **8441** | WS over HTTP/2 | Extended CONNECT method for WS over h2 |
| **9220** | WS over HTTP/3 | WS over HTTP/3 via Extended CONNECT |

### wslay Integration Notes

- wslay handles framing, masking, close handshake, ping/pong
- iohttp provides I/O callbacks (recv/send from io_uring buffers)
- compression via wslay's permessage-deflate support

## Security Headers

| RFC | Title | Header |
|-----|-------|--------|
| **6797** | HSTS | `Strict-Transport-Security` |
| **9163** | Expect-CT (deprecated) | `Expect-CT` |
| **6454** | Web Origin | `Origin` |

CORS is defined by WHATWG Fetch Standard, not an RFC.

## Authentication

| RFC | Title | Mechanism |
|-----|-------|-----------|
| **9110 §11** | HTTP Auth Framework | `WWW-Authenticate`, `Authorization` |
| **7617** | Basic Auth | `Authorization: Basic base64(user:pass)` |
| **6750** | Bearer Token | `Authorization: Bearer <token>` |
| **7519** | JWT | JSON Web Token format |
| **7515** | JWS | JWT signing |
| **6265** | Cookies | `Set-Cookie`, `Cookie`, SameSite |

## Content Encoding & Compression

| RFC | Title | Token |
|-----|-------|-------|
| **9110 §8.4** | Content-Encoding | Framework |
| **1952** | GZIP | `gzip` |
| **1951** | DEFLATE | `deflate` |
| **7932** | Brotli | `br` |
| **8478** | Zstandard | `zstd` |

## Structured Fields

| RFC | Title | Usage |
|-----|-------|-------|
| **9651** | Structured Fields | Modern header parsing (sfparse library) |
| **9218** | Priorities | Uses Structured Fields syntax |

## URI & Content Types

| RFC | Title | Usage |
|-----|-------|-------|
| **3986** | URI | URI syntax — path parsing, percent-encoding |
| **6570** | URI Template | Route patterns with parameters |
| **8259** | JSON | `application/json` format |
| **9457** | Problem Details | `application/problem+json` error responses |
| **7578** | multipart/form-data | File uploads |

## PROXY Protocol

Not an IETF RFC. Defined by HAProxy specification:
- v1: text format `PROXY TCP4 src dst sport dport\r\n`
- v2: binary with 12-byte signature, TLV extensions
- **SECURITY**: MUST be config-only + allowlist, never auto-detect

## Local RFC Copies

Download to `docs/rfc/` using the scraper:
```bash
python3 deploy/podman/scripts/rfc-scraper.py --download docs/rfc/
python3 deploy/podman/scripts/rfc-scraper.py -o docs/rfc/INDEX.md
```

## context7 Documentation

For up-to-date library API docs, use context7 MCP:
- `/wolfssl/wolfssl` — wolfSSL TLS
- `/axboe/liburing` — io_uring
- `/h2o/picohttpparser` — HTTP/1.1 parser
- `/nghttp2/nghttp2` — HTTP/2
- `/ngtcp2/ngtcp2` — QUIC transport
- `/ngtcp2/nghttp3` — HTTP/3
- `/tatsuhiro-t/wslay` — WebSocket
- `/ngtcp2/sfparse` — Structured Fields
- `/ibireme/yyjson` — JSON

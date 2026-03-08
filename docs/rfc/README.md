# RFC Reference

Local RFC text copies for offline reference during iohttp development.

## Download RFCs

```bash
python3 deploy/podman/scripts/rfc-scraper.py --download docs/rfc/
```

## Generate Index

```bash
python3 deploy/podman/scripts/rfc-scraper.py -o docs/rfc/INDEX.md
```

## Priority

| Priority | RFCs | Domain |
|----------|------|--------|
| P0 — Core | 9110, 9112, 9113, 9114, 9000, 9001, 8446, 6455 | HTTP/1.1, /2, /3, QUIC, TLS, WebSocket |
| P1 — Production | 9111, 9218, 6797, 6750, 7519, 9325, 9457, 7301 | Caching, priorities, HSTS, auth, security |
| P2 — Completeness | 8441, 9220, 7692, 7932, 8478, 6265, 8288 | WS over h2/h3, compression, cookies |
| P3 — Extended | 6570, 9221, 9530, 8705, 8297 | URI templates, datagrams, hints |

## HTTP Core

| RFC | Title |
|-----|-------|
| 9110 | HTTP Semantics |
| 9111 | HTTP Caching |
| 9112 | HTTP/1.1 |
| 9113 | HTTP/2 |
| 9114 | HTTP/3 |
| 9218 | Extensible Priorities |

## QUIC

| RFC | Title |
|-----|-------|
| 9000 | QUIC Transport |
| 9001 | QUIC-TLS |
| 9002 | QUIC Loss Detection |
| 9204 | QPACK |
| 9221 | QUIC Datagrams |
| 9369 | QUIC Version 2 |

## TLS

| RFC | Title |
|-----|-------|
| 8446 | TLS 1.3 |
| 7301 | ALPN |
| 6066 | TLS Extensions (SNI, OCSP) |
| 9325 | Secure Use of TLS |

## WebSocket

| RFC | Title |
|-----|-------|
| 6455 | WebSocket Protocol |
| 7692 | WebSocket Compression |
| 8441 | WebSocket over HTTP/2 |
| 9220 | WebSocket over HTTP/3 |

## Security & Auth

| RFC | Title |
|-----|-------|
| 6797 | HSTS |
| 7617 | Basic Auth |
| 6750 | Bearer Token |
| 7519 | JWT |
| 6265 | Cookies |

## Compression & Content

| RFC | Title |
|-----|-------|
| 7932 | Brotli |
| 8478 | Zstandard |
| 3986 | URI |
| 8259 | JSON |
| 9457 | Problem Details |
| 9651 | Structured Fields |

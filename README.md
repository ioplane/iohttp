[![en](https://img.shields.io/badge/lang-en-blue.svg)](README.md)
[![ru](https://img.shields.io/badge/lang-ru-green.svg)](README.ru.md)

<p align="center">
  <h1 align="center">iohttp</h1>
  <p align="center">Embedded HTTP server for C23 — io_uring · wolfSSL · HTTP/1.1·2·3</p>
</p>

<p align="center">
  <a href="LICENSE"><img src="https://img.shields.io/badge/license-GPLv3-blue?style=for-the-badge" alt="License"></a>
  <img src="https://img.shields.io/badge/C23-ISO%2FIEC%209899%3A2024-blue?style=for-the-badge" alt="C23">
  <img src="https://img.shields.io/badge/Linux-6.7%2B-orange?style=for-the-badge&logo=linux&logoColor=white" alt="Linux">
  <img src="https://img.shields.io/badge/ioh__uring-native-green?style=for-the-badge" alt="io_uring">
  <img src="https://img.shields.io/badge/wolfSSL-TLS%201.3-purple?style=for-the-badge" alt="wolfSSL">
  <img src="https://img.shields.io/badge/PROXY%20protocol-v1%2Fv2-teal?style=for-the-badge" alt="PROXY Protocol">
  <img src="https://img.shields.io/badge/version-0.1.0--dev-red?style=for-the-badge" alt="Version">
</p>

Production-grade embedded HTTP server library in C23. Built on `io_uring` for
zero-syscall I/O, native wolfSSL integration for TLS 1.3 / QUIC, and full
HTTP/1.1 + HTTP/2 + HTTP/3 protocol support. Drop-in replacement for Mongoose,
CivetWeb, and libmicrohttpd — with modern protocol stack and kernel-native
async I/O.

## Quick Start

```bash
git clone https://github.com/dantte-lp/iohttp.git
cd iohttp
cmake --preset clang-debug
cmake --build --preset clang-debug
ctest --preset clang-debug
```

## Architecture

```mermaid
graph TB
    Client[Client] -->|TCP| Accept
    Client -->|QUIC/UDP| QUIC

    subgraph Ring[io_uring Event Loop]
        Accept[Multishot Accept] --> Proxy{PROXY?}
        Proxy -->|yes| PP[PROXY v1/v2 Decode]
        Proxy -->|no| TLS
        PP --> TLS[wolfSSL TLS 1.3]
        TLS --> ALPN{ALPN}
        ALPN -->|h2| H2[nghttp2]
        ALPN -->|http/1.1| H1[picohttpparser]
        QUIC[ngtcp2 + wolfSSL] --> H3[nghttp3]
        SIG[signalfd] -->|SIGTERM| Drain[Graceful Drain]
        SIG -->|SIGQUIT| Stop[Immediate Stop]
    end

    H1 --> Router[Radix Trie Router]
    H2 --> Router
    H3 --> Router
    Router --> MW[Middleware Chain]
    MW --> Handler[Handler]
    Handler --> Static[Static / embed]
    Handler --> API[JSON / yyjson]
    Handler --> SSE[SSE Stream]
    Handler --> WS[WebSocket]
```

## Key Features

- **io_uring native** — multishot accept, provided buffers, zero-copy send, SQPOLL mode
- **wolfSSL integration** — TLS 1.3, mTLS, QUIC crypto, session resumption, FIPS-ready
- **HTTP/1.1** — picohttpparser (SSE4.2 SIMD, ~4+ GB/s), keep-alive, chunked TE
- **HTTP/2** — nghttp2 (HPACK, multiplexed streams, server push)
- **HTTP/3** — ngtcp2 + nghttp3 (QUIC, 0-RTT, connection migration)
- **Router** — longest-prefix match, path parameters, per-route auth/permissions
- **Middleware** — rate limiting, CORS, JWT auth, mTLS, audit log, security headers
- **Static files** — C23 `#embed`, ETag, gzip/brotli, immutable cache, SPA fallback
- **WebSocket** — RFC 6455, ping/pong, fragmentation, per-message compression
- **SSE** — Server-Sent Events with io_uring timers
- **JSON** — yyjson (~2.4 GB/s) for API serialization
- **API docs** — Scalar UI integration for OpenAPI specs
- **Security** — CSP, HSTS, X-Frame-Options, SameSite cookies, RBAC bitmask
- **Single binary** — embed SPA + assets in executable via `#embed` or packed FS

## Production Features

Sprint 12 hardening features for production readiness:

- **Linked timeouts** — header read, body read, and keepalive timeouts via io_uring `LINK_TIMEOUT`; no timer threads, no signal hacks
- **Request limits** — max header size (431 Request Header Fields Too Large), max body size (413 Content Too Large), configurable per-route
- **Signal-driven shutdown** — `SIGTERM` triggers graceful drain (stop accepting, finish in-flight, close), `SIGQUIT` triggers immediate shutdown, both via `signalfd` integrated into the io_uring event loop
- **Structured logging** — `ioh_log` with severity levels (DEBUG/INFO/WARN/ERROR), custom sink callbacks, default stderr output
- **Request ID** — auto-generated 128-bit hex `X-Request-Id` header, propagated through middleware chain and available in logging context
- **PROXY protocol** — v1 (text) and v2 (binary + TLV extensions), explicit listener mode only, trusted source IP allowlist

<details>
<summary>Connection State Machine</summary>

```mermaid
stateDiagram-v2
    [*] --> ACCEPTING
    ACCEPTING --> PROXY_HEADER : proxy_protocol=true
    ACCEPTING --> TLS_HANDSHAKE : TLS enabled
    ACCEPTING --> HTTP_ACTIVE : plain TCP
    PROXY_HEADER --> TLS_HANDSHAKE : header parsed + TLS
    PROXY_HEADER --> HTTP_ACTIVE : header parsed
    TLS_HANDSHAKE --> HTTP_ACTIVE : handshake done
    HTTP_ACTIVE --> DRAINING : SIGTERM
    DRAINING --> CLOSING : drain timeout
    CLOSING --> [*] : fd closed
    PROXY_HEADER --> CLOSING : malformed / timeout
    TLS_HANDSHAKE --> CLOSING : handshake failure
    HTTP_ACTIVE --> CLOSING : error / SIGQUIT / timeout
```

</details>

## Protocol Stack

| Layer | Library | License | LOC |
|-------|---------|---------|-----|
| HTTP/1.1 parser | [picohttpparser](https://github.com/h2o/picohttpparser) | MIT | ~800 |
| HTTP/2 frames | [nghttp2](https://github.com/nghttp2/nghttp2) | MIT | ~18K |
| QUIC transport | [ngtcp2](https://github.com/ngtcp2/ngtcp2) | MIT | ~28K |
| HTTP/3 + QPACK | [nghttp3](https://github.com/ngtcp2/nghttp3) | MIT | ~12K |
| WebSocket | [wslay](https://github.com/tatsuhiro-t/wslay) | MIT | ~3K |
| Structured Fields | [sfparse](https://github.com/ngtcp2/sfparse) | MIT | ~1K |
| TLS 1.3 + QUIC | [wolfSSL](https://github.com/wolfSSL/wolfssl) | GPLv2+* | — |
| Async I/O | [liburing](https://github.com/axboe/liburing) | MIT/LGPL | ~3K |
| JSON | [yyjson](https://github.com/ibireme/yyjson) | MIT | ~8K |

## RFC Compliance

| RFC | Title | Status |
|-----|-------|--------|
| 9110 | HTTP Semantics | Partial |
| 9112 | HTTP/1.1 | Implemented |
| 9113 | HTTP/2 | Implemented |
| 9114 | HTTP/3 | Implemented |
| 9000 | QUIC Transport | Implemented |
| 8446 | TLS 1.3 | Implemented |
| 6455 | WebSocket | Implemented |

## Documentation

| # | Document | Description |
|---|----------|-------------|
| 01 | [Architecture](docs/en/01-architecture.md) | Core design, event loop, module decomposition |
| 02 | [Comparison](docs/en/02-comparison.md) | Feature matrix vs Mongoose, H2O, libmicrohttpd, etc. |
| 03 | [Production Hardening](docs/en/03-production-hardening.md) | Timeouts, limits, signals, logging, request ID, PROXY |

## Example

```c
#include "core/ioh_server.h"
#include "core/ioh_ctx.h"

static int hello(ioh_ctx_t *c, void *data)
{
    (void)data;
    return ioh_ctx_json(c, 200, "{\"message\":\"hello\"}");
}

int main(void)
{
    ioh_server_config_t cfg;
    ioh_server_config_init(&cfg);
    cfg.listen_port = 8080;

    ioh_server_t *srv = ioh_server_create(&cfg);
    ioh_server_set_on_request(srv, hello, nullptr);
    ioh_server_run(srv);
    ioh_server_destroy(srv);
}
```

## Build Requirements

- Linux kernel 6.7+ (io_uring features, CVE-2024-0582 avoidance)
- glibc 2.39+
- Clang 22+ or GCC 15+ (C23 support)
- CMake 4.0+
- liburing 2.7+
- wolfSSL 5.8.4+ (--enable-quic)
- nghttp2, ngtcp2, nghttp3 (HTTP/2 + HTTP/3)

## License

GPLv3 — see [LICENSE](LICENSE).

wolfSSL dependency requires GPL-compatible license. See [wolfSSL license note](docs/en/02-comparison.md#protocol-library-stack-iohttps-approach).

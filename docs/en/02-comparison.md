# Comparison with Existing Embedded HTTP Servers

## Executive Summary

iohttp fills a gap in the C embedded HTTP server landscape: **modern protocol support
(HTTP/1.1 + HTTP/2 + HTTP/3) with native io_uring and wolfSSL integration**. No existing
library combines all three. The closest competitor, H2O, supports all protocols but
requires its own picotls/quicly stack (incompatible with wolfSSL) and weighs 80-120K LOC.
However, H2O does offer libh2o as an embeddable library.

---

## Feature Matrix

| Feature | **iohttp** | Mongoose | CivetWeb | libmicrohttpd | H2O | facil.io | Lwan | libhv | Kore |
|---------|-----------|----------|----------|---------------|-----|----------|------|-------|------|
| **Language** | C23 | C99 | C (C++) | C99 | C11 | C | C11 | C/C++ | C |
| **License** | GPLv3 | GPLv2 | MIT | LGPL 2.1 | MIT | MIT | GPLv2 | BSD | ISC |
| **HTTP/1.1** | picohttpparser | custom | custom | custom | picohttpparser | custom | custom | custom | custom |
| **HTTP/2** | nghttp2 | -- | experimental | -- | custom | -- | -- | nghttp2 | -- |
| **HTTP/3** | ngtcp2+nghttp3 | -- | -- | -- | quicly | -- | -- | -- | -- |
| **WebSocket** | wslay (RFC 6455) | yes | yes | yes (0.9.74+) | yes | yes | yes | yes | yes |
| **SSE** | yes | manual | yes | -- | -- | -- | -- | -- | -- |
| **I/O model** | io_uring | select/poll/epoll | threads | select/poll/epoll | epoll (io_uring partial) | epoll | epoll | epoll/iocp/kqueue | epoll/kqueue |
| **TLS** | wolfSSL native | 5 backends | OpenSSL | GnuTLS | OpenSSL (libcrypto) | OpenSSL | mbedTLS | OpenSSL | OpenSSL |
| **wolfSSL** | native API | MG_TLS_WOLFSSL | partial | -- | -- | -- | -- | -- | -- |
| **QUIC/wolfSSL** | ngtcp2_crypto_wolfssl | -- | -- | -- | -- | -- | -- | -- | -- |
| **PROXY proto** | v1+v2 | -- | -- | -- | v2 | -- | -- | -- | -- |
| **SPA fallback** | yes | -- | -- | -- | via mruby | -- | -- | -- | -- |
| **Static files** | sendfile+#embed | yes | yes | -- | yes | yes | yes | yes | yes |
| **gzip/brotli** | streaming+precomp | precomp only | yes | -- | yes | -- | yes (gzip) | -- | -- |
| **Rate limiting** | token bucket | -- | -- | -- | -- | -- | -- | -- | -- |
| **CORS** | built-in | manual | manual | manual | manual | -- | -- | -- | -- |
| **JSON** | yyjson (~2.4 GB/s) | built-in | -- | -- | -- | -- | -- | -- | -- |
| **Metrics** | Prometheus | -- | -- | -- | -- | -- | -- | -- | -- |
| **Zero-copy** | io_uring SEND_ZC | -- | -- | -- | -- | -- | splice | -- | -- |
| **Cross-platform** | Linux 6.7+ | all | all | all | Linux | Linux | Linux | all | Unix |
| **ACME/LE** | -- | -- | -- | -- | -- | -- | -- | -- | yes |
| **Sandboxing** | -- | -- | -- | -- | -- | -- | -- | -- | seccomp+pledge |
| **Maturity** | alpha (S1–S7) | stable | stable | stable | stable | restructuring | stable | stable | stable |
| **Own LOC** | ~10K (S7), ~15-18K projected | ~25-30K | ~30K | ~25K | ~80-120K | ~15-25K | ~20K | ~20K | ~15K |

---

## Detailed Comparison

### Mongoose (Cesanta, GPLv2)

**Strengths:** Single-file amalgamation, embedded-friendly (MCU, RTOS), extensive
driver support (Wi-Fi, Ethernet), working wolfSSL backend (`MG_TLS=MG_TLS_WOLFSSL`),
WebSocket with auto-fragmentation, precompressed gzip serving.

**Weaknesses:**
- **No HTTP/2** — Cesanta: *"We don't have any timeline"* (GitHub Discussion #1427)
- **No HTTP/3** — TCP-only architecture, no QUIC support
- **select()-based** — default FD_SETSIZE=1024 limit, epoll optional
- **No io_uring** — would require complete I/O core rewrite
- **No middleware** — no rate limiting, CORS, auth chains
- **GPLv2-only** — incompatible with GPLv3, commercial license pricing unpublished
- **No SPA fallback** — requires manual implementation
- **No SSE API** — possible via chunked encoding but no dedicated interface

**Verdict:** Excellent for embedded MCU HTTP/1.1 + WebSocket. Structurally unable to
support HTTP/2+ without architectural rewrite. wolfSSL integration exists but through
compatibility layer, not native API.

### CivetWeb (MIT fork of Mongoose)

**Strengths:** MIT license, multi-threaded model, experimental HTTP/2, CGI/Lua support,
~2900 GitHub stars, active community.

**Weaknesses:**
- **Thread-per-connection** — does not scale beyond hundreds of connections
- **HTTP/2 experimental** — not production-ready
- **No HTTP/3** — no QUIC support
- **No io_uring** — thread pool + poll/epoll
- **Partial wolfSSL** — documented in `docs/yaSSL.md` (issues #436/#437), but OpenSSL is primary backend
- **Codebase diverged** from Mongoose since 2013, ~30K LOC
- **No zero-copy** — traditional read/write

**Verdict:** Good MIT-licensed alternative to Mongoose for HTTP/1.1 with threads.
Not suitable when io_uring, wolfSSL, or HTTP/2+ is required.

### libmicrohttpd (GNU, LGPL 2.1)

**Strengths:** GNU project quality, LGPL license allows proprietary linking,
excellent RFC compliance, battle-tested in production systems, supports
internal thread pool and external select/poll/epoll integration.

**Weaknesses:**
- **HTTP/1.1 only** — no HTTP/2, no HTTP/3
- **WebSocket** — stable since v0.9.74 (2024) via `libmicrohttpd_ws`
- **No io_uring** — select/poll/epoll
- **GnuTLS only** — no wolfSSL support
- **No static file serving** — manual implementation required
- **No routing** — application handles all path matching
- **No middleware** — no built-in CORS, rate limiting, auth
- **Verbose API** — callback-heavy, complex configuration

**Verdict:** Solid HTTP/1.1 library with excellent standards compliance. WebSocket added
in v0.9.74, v1.0.0 released with Sovereign Tech Agency funding and security audit (2025).
Too minimal for modern web serving — no HTTP/2+, no routing, no convenience features.

### H2O (DeNA, MIT)

**Strengths:** Full protocol suite (HTTP/1.1 + HTTP/2 + HTTP/3), excellent performance
(uses picohttpparser), production-tested at scale, mruby scripting, comprehensive
configuration, PROXY protocol v2.

**Weaknesses:**
- **No wolfSSL** — uses OpenSSL (libcrypto); picotls supports OpenSSL, minicrypto, and MbedTLS backends but NOT BoringSSL (lacks OCSP Stapling)
- **io_uring partial** — has `io_uring-batch-size` config option, but epoll remains primary event loop
- **Massive codebase** — 80-120K LOC including bundled dependencies
- **Complex build** — many dependencies, cmake + custom build scripts
- **QUIC via quicly** — custom QUIC implementation (not ngtcp2), tied to picotls

**Verdict:** Most feature-complete HTTP server in C and the strongest competitor.
**libh2o** is an official embeddable library (`examples/libh2o/`), used by PearlDB,
ticketd, and other projects. However, no wolfSSL support, no native io_uring, and
the large codebase (80-120K LOC) make it a poor fit for iohttp's use case.

### facil.io (MIT)

**Strengths:** MIT license, well-designed event-driven architecture, WebSocket support,
pub/sub system, Redis integration.

**Weaknesses:**
- **HTTP/1.1 only** — no HTTP/2 or HTTP/3
- **No wolfSSL** — OpenSSL only
- **No io_uring** — custom epoll event loop
- **15-25K LOC** — medium-sized codebase
- **Restructuring** — project migrated to `facil-io` org on GitHub; `facil-io/cstl` updated Feb 2026

**Verdict:** Well-architected HTTP/1.1 library with good API design. Currently
undergoing restructuring (not abandoned). Protocol limitations (HTTP/1.1 only)
make it unsuitable.

### Lwan (GPLv2)

**Strengths:** Extremely fast HTTP/1.1 (uses coroutines), splice-based zero-copy,
excellent benchmark results, coroutine-based request handling.

**Weaknesses:**
- **HTTP/1.1 only** — no HTTP/2 or HTTP/3
- **mbedTLS TLS** — functional with kTLS offload and AES-NI support; TLSv1.3 in development
- **No wolfSSL** — not supported
- **WebSocket** — supported
- **GPLv2** — viral license
- **Linux-specific** — good for us, but no portability

**Verdict:** Impressive HTTP/1.1 performance through coroutines and splice, with
WebSocket support and functional TLS (mbedTLS + kTLS). Too limited in protocol
support (no HTTP/2+) and no wolfSSL integration.

### libhv (BSD)

**Strengths:** Cross-platform (Linux, macOS, Windows), HTTP/1.1 + HTTP/2 via nghttp2,
WebSocket, event loop with epoll/iocp/kqueue, ~20K LOC, BSD license, active development.

**Weaknesses:**
- **No HTTP/3** — no QUIC support
- **No wolfSSL** — OpenSSL only
- **io_uring experimental** — not production-ready
- **No PROXY protocol** — no load balancer integration
- **No SPA fallback** — manual implementation
- **C/C++ mixed** — not pure C

**Verdict:** Most direct competitor with HTTP/2 and cross-platform support. Lacks HTTP/3,
wolfSSL, and advanced production features. io_uring support is experimental.

### Kore (ISC)

**Strengths:** ISC license (permissive), built-in ACME/Let's Encrypt, seccomp + pledge
sandboxing, Python integration, hot-reload, WebSocket, ~15K LOC, active development.

**Weaknesses:**
- **HTTP/1.1 only** — no HTTP/2 or HTTP/3
- **No wolfSSL** — OpenSSL only
- **No io_uring** — epoll/kqueue
- **No PROXY protocol**
- **Unix-only** — no Windows

**Verdict:** Mature production framework with excellent security features (sandboxing, ACME).
Protocol limitations (HTTP/1.1 only) prevent use in modern HTTP/2+ scenarios.

---

## Corrections and Notes

All corrections from earlier drafts have been applied to the text above.

| Claim in earlier drafts | Correction | Status |
|------------------------|------------|--------|
| libmicrohttpd: "no WebSocket" | WebSocket stable since v0.9.74 (2024) | **Fixed** |
| H2O: "no io_uring" | H2O has partial io_uring support (`io_uring-batch-size` config) | **Fixed** |
| H2O: "uses BoringSSL" | H2O uses OpenSSL (libcrypto); BoringSSL NOT supported (no OCSP) | **Fixed** |
| H2O: "not embeddable" | libh2o is official embeddable library (PearlDB, ticketd) | **Fixed** |
| CivetWeb: "no wolfSSL" | Partial wolfSSL support (`docs/yaSSL.md`, issues #436/#437) | **Fixed** |
| Lwan: "no WebSocket" | Lwan lists WebSocket on official site (lwan.ws) | **Fixed** |
| Lwan: "TLS experimental" | mbedTLS functional with kTLS offload, AES-NI | **Fixed** |
| facil.io: "abandoned" | Migrated to `facil-io` org, `facil-io/cstl` updated Feb 2026 | **Fixed** |
| Mongoose wolfSSL: "compat layer" | Mongoose has `MG_TLS_WOLFSSL` — native wolfSSL support, though through its own abstraction layer rather than direct wolfSSL API calls | **Fixed** |
| picohttpparser: "~2.5 GB/s" | Independent benchmarks show ~4-4.5 GB/s with SSE4.2 | **Fixed** |

---

## Protocol Library Stack (iohttp's Approach)

All protocol libraries are MIT-licensed and I/O-agnostic (callback-based). Most are
written or maintained by the same community (Tatsuhiro Tsujikawa and contributors):
nghttp2, ngtcp2, nghttp3, wslay, sfparse.

| Library | Function | LOC | License | wolfSSL Support |
|---------|----------|-----|---------|-----------------|
| picohttpparser | HTTP/1.1 parser | ~800 | MIT | N/A |
| nghttp2 | HTTP/2 frames + HPACK | ~18K | MIT | Via ALPN |
| ngtcp2 | QUIC transport | ~28K | MIT | **First-class** (ngtcp2_crypto_wolfssl) |
| nghttp3 | HTTP/3 + QPACK | ~12K | MIT | N/A |
| [wslay](https://github.com/tatsuhiro-t/wslay) | WebSocket (RFC 6455) | ~3K | MIT | N/A |
| [sfparse](https://github.com/ngtcp2/sfparse) | Structured Fields (RFC 9651) | ~1K | MIT | N/A |
| liburing | io_uring wrappers | ~3K | LGPL+MIT | N/A |
| yyjson | JSON (2.4 GB/s) | ~8K | MIT | N/A |
| wolfSSL | TLS 1.3, QUIC crypto | — | GPLv2+ | — |

**Key advantage:** ngtcp2 is the **only** QUIC library with native wolfSSL support,
via `libngtcp2_crypto_wolfssl` (added by Stefan Eissing, August 2022, used by curl
for HTTP/3). Verified: quiche (BoringSSL only), MsQuic (Schannel/OpenSSL), lsquic
(BoringSSL), picoquic (picotls), Quinn (rustls), quic-go (Go crypto/tls), mvfst (Fizz).

**Ecosystem coherence:** Five of the protocol libraries (nghttp2, ngtcp2, nghttp3,
wslay, sfparse) share a common author (Tatsuhiro Tsujikawa), consistent API style
(callback-based, I/O-agnostic), and the same MIT license. This reduces integration
friction and ensures consistent behavior across the protocol stack.

**wolfSSL license note:** The license column shows GPLv2+ but this needs verification.
GitHub LICENSING says GPLv2, wolfssl.com says GPLv3, the manual says GPLv2. If strictly
GPLv2 (not "or later"), it is incompatible with GPLv3. Clarify with wolfSSL Inc. or
acquire commercial license before release.

---

## Performance Positioning

| Metric | iohttp (target) | Mongoose | H2O | nginx |
|--------|-----------------|----------|-----|-------|
| HTTP/1.1 parsing | ~4+ GB/s (SSE4.2, picohttpparser) | ~1 GB/s | ~4+ GB/s | ~1.5 GB/s |
| I/O model | io_uring (zero-syscall) | select/epoll | epoll | epoll |
| JSON serialization | ~2.4 GB/s (yyjson) | ~200 MB/s | N/A | N/A |
| Zero-copy send | SEND_ZC (6.0+) | N/A | N/A | sendfile |
| Buffer management | Kernel-managed rings | Application | Application | Application |
| Connection scalability | 10K+ (io_uring) | ~1K (select) | 10K+ | 10K+ |

---

## Why iohttp?

1. **Only** embedded C library combining HTTP/1.1 + HTTP/2 + HTTP/3 with native wolfSSL
   (H2O has libh2o for HTTP/1+2+3 but requires OpenSSL/picotls, no wolfSSL)
2. **Only** QUIC implementation compatible with wolfSSL (via ngtcp2_crypto_wolfssl,
   verified against all major QUIC libraries — quiche, MsQuic, lsquic, picoquic, etc.)
3. **io_uring as core runtime** — not just partial support (like H2O's `io_uring-batch-size`),
   but built around it: multishot accept, provided buffers, zero-copy send, linked timeouts,
   ring restrictions, multi-reactor ring-per-thread
4. **Single TLS stack** — wolfSSL for HTTPS, QUIC, and mTLS (no OpenSSL dependency)
5. **Smallest own code** for full protocol coverage (~15-18K projected vs 80-120K for H2O)
6. **Modern C23** — nullptr, constexpr, [[nodiscard]], type-safe enums, `<stdckdint.h>`
7. **Production features** — SPA, CORS, rate limiting, Slowloris defense, lock-free
   Prometheus metrics, PROXY protocol v1+v2, HTTP/2 Rapid Reset protection
8. **Security hardening** — `IORING_REGISTER_RESTRICTIONS` (seccomp bypass mitigation),
   `memset_explicit()` for secrets, request smuggling protection, CVE-aware kernel minimum

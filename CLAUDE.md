# iohttp - Project Instructions for Claude Code

## Quick Facts

- **Language**: C23 (ISO/IEC 9899:2024), `-std=c23`, `CMAKE_C_EXTENSIONS OFF`
- **Project**: Embedded HTTP server library (HTTP/1.1 + HTTP/2 + HTTP/3, io_uring, wolfSSL)
- **License**: GPLv3 (wolfSSL dependency — see license note below)
- **Status**: Pre-release, first release: 0.1.0
- **Platform**: Linux only (kernel 6.7+, glibc 2.39+)

## Build Commands

```bash
# CMake (primary build system)
cmake --preset clang-debug              # Configure with Clang debug
cmake --build --preset clang-debug      # Build
ctest --preset clang-debug              # Run tests

# Code quality
cmake --build --preset clang-debug --target format    # clang-format
cmake --build --preset clang-debug --target lint      # clang-tidy
cmake --build --preset clang-debug --target cppcheck  # static analysis
cmake --build --preset clang-debug --target docs      # Doxygen
```

## Dev Container

- **Image**: `localhost/ringwall-dev:latest` (OL10-based, shared with ringwall project)
- **MUST run with**: `--security-opt seccomp=unconfined` (io_uring_setup needs it)
- **Compilers**: Clang 22.1.0 (primary), GCC 15.1.1 (gcc-toolset-15, validation)
- **System GCC**: 14.3.1 (OL10 default, used by some library builds)
- **Linker**: mold (debug), GNU ld/lld (release)
- **Key tools**: CMake 4.2.3, Unity 2.6.1, cppcheck, Doxygen 1.16.1
- **Shared deps**: wolfSSL 5.8.2+ (--enable-quic), liburing 2.14, yyjson 0.12.0
- **iohttp-specific deps**: nghttp2, ngtcp2 + ngtcp2_crypto_wolfssl, nghttp3, zlib-devel, brotli-devel
- **picohttpparser**: vendored (~800 LOC, no external dep)
- `_GNU_SOURCE` needed for `explicit_bzero`, `signalfd`, etc. under `-std=c23`
- **ALL development/compilation MUST happen inside the podman container** — never run build commands directly on host

## Compiler Strategy (dual-compiler)

- **Clang 22+**: Primary dev (MSan, LibFuzzer, clang-tidy, fast builds with mold)
- **GCC 15+**: Validation & release (LTO, -fanalyzer, unique warnings)
- **Debug linker**: mold (instant linking)
- **Release linker**: GNU ld (GCC LTO) or lld (Clang ThinLTO)
- Always use `-std=c23` explicitly for both compilers

## Key Directories

```
include/iohttp/     # Public API headers (io_server.h, io_request.h, io_router.h, ...)
src/core/           # io_uring event loop, worker threads, server lifecycle, buffers
src/net/            # Listener, multishot accept, socket options, PROXY protocol
src/http/           # HTTP/1.1 (picohttpparser), HTTP/2 (nghttp2), HTTP/3 (ngtcp2+nghttp3)
src/tls/            # wolfSSL TLS 1.3, QUIC crypto, mTLS, ALPN, session resumption
src/router/         # Radix-trie router, route groups, introspection
src/ws/             # WebSocket (RFC 6455), SSE
src/static/         # Static file serving, #embed, caching, SPA fallback
src/middleware/     # Rate limiting, CORS, JWT, security headers, liboas adapter
tests/unit/         # Unity-based unit tests (test_*.c)
tests/integration/  # Integration tests
tests/bench/        # Performance benchmarks
tests/fuzz/         # LibFuzzer targets (Clang only)
docs/en/            # English documentation
docs/ru/            # Russian documentation
docs/plans/         # Sprint plans
examples/           # Example applications
deploy/podman/      # Container configurations
```

## Code Conventions

**Full reference: `.claude/skills/iohttp-architecture/SKILL.md`** — MUST be followed for all code.

- **Naming**: `io_module_verb_noun()` functions, `io_module_name_t` types, `IO_MODULE_VALUE` enums/macros
- **Prefix**: `io_` for all public API
- **Typedef suffix**: `_t` for all types
- **Include guards**: `IOHTTP_MODULE_FILE_H`
- **Pointer style**: `int *ptr` (right-aligned, Linux kernel style)
- **Column limit**: 100 characters
- **Braces**: Linux kernel style (`BreakBeforeBraces: Linux`)
- **Includes order**: `_GNU_SOURCE` -> matching header -> C stdlib -> POSIX -> third-party
- **No C++ dependencies**: Pure C ecosystem only
- **Errors**: return negative errno (`-ENOMEM`, `-EINVAL`), use `goto cleanup` for multi-resource
- **Allocation**: always `sizeof(*ptr)`, never `sizeof(type)`
- **Comments**: Doxygen `@param`/`@return` in headers only; inline comments explain WHY, not WHAT
- **Tests**: Unity framework, `test_module_action_expected()`, typed assertions, cleanup resources

### C23 (mandatory)

| Use everywhere | Use when appropriate | Avoid |
|----------------|---------------------|-------|
| `nullptr`, `[[nodiscard]]`, `constexpr`, `bool` keyword, `_Static_assert` | `typeof`, `[[maybe_unused]]`, `_Atomic`, `<stdckdint.h>`, digit separators, `unreachable()` | `auto` inference, `_BitInt`, `#embed` (except static files) |

## Security Requirements (MANDATORY)

- Crypto comparisons: constant-time only (`ConstantCompare` from wolfCrypt)
- Secrets: zero after use (`memset_explicit()` preferred (C23), `explicit_bzero()` fallback)
- Error returns: `[[nodiscard]]` on all public API functions
- Hardening: `-fstack-protector-strong -D_FORTIFY_SOURCE=3 -fPIE -pie`
- Linker: `-Wl,-z,relro -Wl,-z,now`
- Overflow checks: `<stdckdint.h>` for size/length arithmetic
- BANNED functions: `strcpy`, `sprintf`, `gets`, `strcat`, `atoi`, `system()`, `memcmp` on secrets
- Use bounded alternatives: `snprintf`, `strnlen`, `memcpy` with size checks
- HTTP parsing: reject oversized headers, enforce content-length limits, timeout slow clients
- **SIGPIPE**: `signal(SIGPIPE, SIG_IGN)` at startup — wolfSSL can trigger SIGPIPE via internal writes
- **io_uring ring hardening**: `IORING_REGISTER_RESTRICTIONS` to whitelist only needed opcodes; io_uring ops bypass seccomp BPF filters (shared memory ring, not syscalls)
- **SEND_ZC threshold**: regular send < 2 KiB, SEND_ZC > 2 KiB, splice for static files; SEND_ZC generates 2 CQEs (completion + buffer notification)
- **wolfSSL I/O serialization**: single I/O buffer per SSL object; reads/writes must be serialized; natural in ring-per-thread model

## Library Stack

### Core
| Library       | Version | Role                          |
|---------------|---------|-------------------------------|
| wolfSSL       | 5.8.2+  | TLS 1.3, QUIC crypto, mTLS   |
| liburing      | 2.7+    | All I/O: network, timers     |
| picohttpparser| latest  | HTTP/1.1 parsing (SSE4.2)    |
| nghttp2       | latest  | HTTP/2 frames + HPACK        |
| ngtcp2        | latest  | QUIC transport               |
| nghttp3       | latest  | HTTP/3 + QPACK               |
| yyjson        | 0.12+   | JSON serialization (~2.4 GB/s)|

### wolfSSL License Note
wolfSSL license needs clarification before release — GitHub LICENSING says GPLv2, wolfssl.com says GPLv3, manual says GPLv2. If strictly GPLv2 (not GPLv2+), it's incompatible with iohttp's GPLv3. Resolve with wolfSSL Inc. or acquire commercial license.

## Testing Rules

- All new code MUST have unit tests (Unity framework)
- Test files: `tests/unit/test_<module>.c`
- Sanitizers: ASan+UBSan (every commit), MSan (Clang, every commit), TSan (before merge)
- Fuzzing: LibFuzzer targets in `tests/fuzz/` (Clang only)
- Coverage target: >= 80%

## Architecture Decisions (DO NOT CHANGE)

- io_uring for ALL async I/O — NOT a backend, IS the core runtime (no epoll fallback)
- `DEFER_TASKRUN + SINGLE_ISSUER` preferred; SQPOLL optional, NOT default
- wolfSSL Native API (not OpenSSL compat layer)
- Pure C libraries only (no C++ dependencies)
- Linux only — kernel 6.7+ (CVE-2024-0582 avoidance, IOU_PBUF_RING_INC), glibc 2.39+
- picohttpparser for HTTP/1.1 (not llhttp)
- nghttp2 for HTTP/2, ngtcp2+nghttp3 for HTTP/3
- yyjson for JSON serialization
- Scalar for API documentation (not Swagger UI)
- Single-binary deployment via C23 `#embed` for static assets
- GPLv3 license

## Git Workflow

- Branch naming: `feature/description`, `fix/issue-description`
- Commit style: conventional commits (`feat:`, `fix:`, `refactor:`, `test:`, `docs:`)
- All commits must pass: clang-format, clang-tidy, unit tests
- Never commit `.deployment-credentials` or secrets
- **NEVER mention "Claude" or any AI assistant in commit messages, comments, or code** — no `Co-Authored-By` AI lines

## Skills Reference

See `.claude/skills/` for detailed guidance on:
- **`iohttp-architecture/`** — Architecture, directory layout, naming, state machine, P0-P4 phasing (MANDATORY)
- **`io-uring-patterns/`** — SQE/CQE patterns, provided buffers, multishot, linked timeouts, zero-copy, REGISTER_RESTRICTIONS (MANDATORY for src/core/, src/net/)
- **`wolfssl-iohttp/`** — wolfSSL I/O callbacks, non-blocking TLS, ALPN/SNI, mTLS, QUIC crypto, I/O serialization (MANDATORY for src/tls/)
- **`rfc-reference/`** — RFC index with key sections, priority map, protocol implementation notes

## MCP Documentation (context7)

Use context7 to fetch up-to-date documentation:
- wolfSSL API: `/wolfssl/wolfssl`
- liburing io_uring: `/axboe/liburing`
- picohttpparser: `/h2o/picohttpparser`
- nghttp2 HTTP/2: `/nghttp2/nghttp2`
- ngtcp2 QUIC: `/ngtcp2/ngtcp2`
- nghttp3 HTTP/3: `/ngtcp2/nghttp3`
- wslay WebSocket: `/tatsuhiro-t/wslay`
- sfparse Structured Fields: `/ngtcp2/sfparse`
- yyjson JSON: `/ibireme/yyjson`
- CMake build: `/websites/cmake_cmake_help`

## RFC References

Local copies in `docs/rfc/` — see `docs/rfc/README.md` for full index.

**Core (must-read for HTTP implementation):**
- RFC 9110 — HTTP Semantics
- RFC 9112 — HTTP/1.1
- RFC 9113 — HTTP/2
- RFC 9114 — HTTP/3

**QUIC:**
- RFC 9000 — QUIC Transport
- RFC 9001 — QUIC-TLS
- RFC 9002 — QUIC Loss Detection

**TLS:**
- RFC 8446 — TLS 1.3
- RFC 7301 — ALPN
- RFC 9325 — Secure Use of TLS

**WebSocket:**
- RFC 6455 — WebSocket Protocol
- RFC 7692 — WebSocket Compression

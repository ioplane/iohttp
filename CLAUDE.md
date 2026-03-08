# iohttp - Project Instructions for Claude Code

## Quick Facts

- **Language**: C23 (ISO/IEC 9899:2024), `-std=c23`, `CMAKE_C_EXTENSIONS OFF`
- **Project**: Embedded HTTP server library (drop-in replacement for Mongoose, CivetWeb, libmicrohttpd)
- **License**: GPLv3 (wolfSSL dependency requires GPLv3)
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

## Compiler Strategy (dual-compiler)

- **Clang 22+**: Primary dev (MSan, LibFuzzer, clang-tidy, fast builds with mold)
- **GCC 15+**: Validation & release (LTO, -fanalyzer, unique warnings)
- **Debug linker**: mold (instant linking)
- **Release linker**: GNU ld (GCC LTO) or lld (Clang ThinLTO)
- Always use `-std=c23` explicitly for both compilers

## Key Directories

```
src/core/           # io_uring event loop, server lifecycle, configuration
src/http/           # HTTP/1.1 (picohttpparser), HTTP/2 (nghttp2), HTTP/3 (ngtcp2+nghttp3)
src/tls/            # wolfSSL TLS 1.3, QUIC crypto, mTLS, session resumption
src/ws/             # WebSocket (RFC 6455), SSE
src/static/         # Static file serving, #embed, caching, SPA fallback
src/middleware/     # Rate limiting, CORS, JWT, security headers, audit
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
- Secrets: zero after use (`explicit_bzero()`)
- Error returns: `[[nodiscard]]` on all public API functions
- Hardening: `-fstack-protector-strong -D_FORTIFY_SOURCE=3 -fPIE -pie`
- Linker: `-Wl,-z,relro -Wl,-z,now`
- Overflow checks: `<stdckdint.h>` for size/length arithmetic
- BANNED functions: `strcpy`, `sprintf`, `gets`, `strcat`, `atoi`, `system()`, `memcmp` on secrets
- Use bounded alternatives: `snprintf`, `strnlen`, `memcpy` with size checks
- HTTP parsing: reject oversized headers, enforce content-length limits, timeout slow clients

## Library Stack

### Core
| Library       | Version | Role                          |
|---------------|---------|-------------------------------|
| wolfSSL       | 5.8+    | TLS 1.3, QUIC crypto, mTLS   |
| liburing      | 2.7+    | All I/O: network, timers     |
| picohttpparser| latest  | HTTP/1.1 parsing (SSE4.2)    |
| nghttp2       | latest  | HTTP/2 frames + HPACK        |
| ngtcp2        | latest  | QUIC transport               |
| nghttp3       | latest  | HTTP/3 + QPACK               |
| yyjson        | 0.12+   | JSON serialization (~2.4 GB/s)|

## Testing Rules

- All new code MUST have unit tests (Unity framework)
- Test files: `tests/unit/test_<module>.c`
- Sanitizers: ASan+UBSan (every commit), MSan (Clang, every commit), TSan (before merge)
- Fuzzing: LibFuzzer targets in `tests/fuzz/` (Clang only)
- Coverage target: >= 80%

## Architecture Decisions (DO NOT CHANGE)

- io_uring for ALL async I/O (multishot accept, provided buffers, zero-copy send)
- wolfSSL Native API (not OpenSSL compat layer)
- Pure C libraries only (no C++ dependencies)
- Linux only — kernel 6.7+, glibc 2.39+
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
- **NEVER mention "Claude" or any AI assistant in commit messages, comments, or code**

## MCP Documentation (context7)

Use context7 to fetch up-to-date documentation:
- wolfSSL API: `/wolfssl/wolfssl`
- liburing io_uring: `/axboe/liburing`
- yyjson JSON: `/ibireme/yyjson`
- CMake build: `/websites/cmake_cmake_help`

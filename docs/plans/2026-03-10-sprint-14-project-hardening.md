# Sprint 14: Project Hardening

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Close documentation and tooling gaps identified by gap analysis against `docs/tmp/draft/git-project-recommendations-for-c23.md`, bringing iohttp to production-grade open-source project standards.

**Architecture:** Fill missing config files (.clangd, .editorconfig), missing documentation (SECURITY.md, SUPPORT.md), add GitHub infrastructure (YAML issue templates, PR template, CODEOWNERS, dependabot), add coverage preset, populate fuzz infrastructure, add CMake options table to READMEs, add .gitignore entries.

**Tech Stack:** C23, CMake 3.25+, Clang 22, GCC 15, Unity, LibFuzzer, Doxygen, PVS-Studio, CodeChecker.

**Source:** Gap analysis of iohttp vs `docs/tmp/draft/git-project-recommendations-for-c23.md`.

**Build/test:**
```bash
cmake --preset clang-debug && cmake --build --preset clang-debug && ctest --preset clang-debug
```

---

## Task 1: Add .clangd, .editorconfig, fix .gitignore

**Priority:** P0

**Files:**
- Create: `.clangd`
- Create: `.editorconfig`
- Modify: `.gitignore` — add `CMakeUserPresets.json`, `.containerignore` pattern

**Step 1: Create .clangd**

```yaml
CompileFlags:
  CompilationDatabase: build/clang-debug/
  Add: [-std=c23, -Wall, -Wextra]
  Remove: [-fstack-clash-protection]
Diagnostics:
  ClangTidy:
    Add: [bugprone-*, cert-*, clang-analyzer-*]
  UnusedIncludes: Strict
InlayHints:
  Enabled: true
  ParameterNames: true
```

**Step 2: Create .editorconfig**

```ini
root = true

[*]
charset = utf-8
end_of_line = lf
insert_final_newline = true
trim_trailing_whitespace = true

[*.{c,h}]
indent_style = space
indent_size = 4

[CMakeLists.txt]
indent_style = space
indent_size = 4

[*.cmake]
indent_style = space
indent_size = 4

[*.{yml,yaml}]
indent_style = space
indent_size = 2

[*.json]
indent_style = space
indent_size = 2

[*.md]
trim_trailing_whitespace = false

[Makefile]
indent_style = tab
```

**Step 3: Add entries to .gitignore**

Append to `.gitignore`:

```
# Developer-local preset overrides
CMakeUserPresets.json

# Coverage artifacts
*.gcno
*.gcda
lcov.info
```

**Step 4: Commit**

```bash
git add .clangd .editorconfig .gitignore
git commit -m "chore: add .clangd, .editorconfig, fix .gitignore completeness"
```

---

## Task 2: Create SECURITY.md and SUPPORT.md

**Priority:** P0

**Files:**
- Create: `SECURITY.md`
- Create: `SUPPORT.md`

**Step 1: Create SECURITY.md**

```markdown
# Security Policy

## Reporting a Vulnerability

**DO NOT create a public issue for security vulnerabilities.**

Use [GitHub Private Vulnerability Reporting](https://github.com/dantte-lp/iohttp/security/advisories/new) to report vulnerabilities.

- **Acknowledgement**: within 48 hours
- **Assessment**: within 1 week
- **Fix**: coordinated disclosure release

## Supported Versions

| Version | Supported |
|---------|-----------|
| 0.1.x   | Yes (current development) |

## Vulnerability Classes We Track

- Buffer overflow (heap/stack)
- Use-after-free
- Double-free
- Integer overflow/underflow in size/length arithmetic
- Uninitialized memory reads
- Data races in io_uring async paths
- Incorrect io_uring SQE/CQE validation
- Chunked encoding smuggling (HTTP/1.1)
- Request-line / header injection
- TLS session handling errors
- HPACK/QPACK decompression bombs (HTTP/2, HTTP/3)

## User Recommendations

- Compile with `-D_FORTIFY_SOURCE=3` and stack protectors
- Run tests with AddressSanitizer: `cmake --preset clang-asan`
- Verify minimum kernel version (6.7+) for io_uring operations
- Use PROXY protocol only on trusted listener interfaces
```

**Step 2: Create SUPPORT.md**

```markdown
# Support

## Getting Help

- **Bugs**: [GitHub Issues](https://github.com/dantte-lp/iohttp/issues)
- **Security**: See [SECURITY.md](SECURITY.md)
- **Questions**: GitHub Discussions

## Build & Test

```bash
cmake --preset clang-debug
cmake --build --preset clang-debug
ctest --preset clang-debug
```

## Sanitizer Testing

```bash
# ASan + UBSan
cmake --preset clang-asan && cmake --build --preset clang-asan && ctest --preset clang-asan

# TSan (thread safety)
cmake --preset clang-tsan && cmake --build --preset clang-tsan

# MSan (uninitialized memory)
cmake --preset clang-msan && cmake --build --preset clang-msan && ctest --preset clang-msan
```

## Quality Pipeline

```bash
podman run --rm --security-opt seccomp=unconfined \
  -v $(pwd):/workspace:Z localhost/iohttp-dev:latest \
  bash -c "cd /workspace && ./scripts/quality.sh"
```
```

**Step 3: Commit**

```bash
git add SECURITY.md SUPPORT.md
git commit -m "docs: add SECURITY.md with vulnerability classes and SUPPORT.md"
```

---

## Task 3: Add GitHub infrastructure

**Priority:** P1

**Files:**
- Create: `.github/ISSUE_TEMPLATE/bug_report.yml`
- Create: `.github/ISSUE_TEMPLATE/feature_request.yml`
- Create: `.github/ISSUE_TEMPLATE/config.yml`
- Create: `.github/PULL_REQUEST_TEMPLATE.md`
- Create: `.github/CODEOWNERS`
- Create: `.github/dependabot.yml`

**Step 1: Create directories**

```bash
mkdir -p .github/ISSUE_TEMPLATE
```

**Step 2: Create YAML bug report template**

`.github/ISSUE_TEMPLATE/bug_report.yml`:

```yaml
name: "Bug Report"
description: "Report a bug in iohttp"
labels: ["bug", "triage"]
body:
  - type: dropdown
    id: compiler
    attributes:
      label: Compiler
      options:
        - Clang 22
        - Clang 19
        - GCC 15
        - GCC 14
        - Other
    validations:
      required: true
  - type: input
    id: kernel-version
    attributes:
      label: "Kernel Version (uname -r)"
      placeholder: "6.12.0-1-generic"
    validations:
      required: true
  - type: textarea
    id: description
    attributes:
      label: Description
      description: Clear description of the bug
    validations:
      required: true
  - type: textarea
    id: reproduce
    attributes:
      label: Steps to Reproduce
  - type: textarea
    id: sanitizer-output
    attributes:
      label: "Sanitizer Output (ASan/UBSan/TSan)"
      render: shell
  - type: checkboxes
    id: terms
    attributes:
      label: Checklist
      options:
        - label: "I searched existing issues"
          required: true
```

**Step 3: Create YAML feature request template**

`.github/ISSUE_TEMPLATE/feature_request.yml`:

```yaml
name: "Feature Request"
description: "Suggest a feature for iohttp"
labels: ["enhancement"]
body:
  - type: textarea
    id: description
    attributes:
      label: Description
      description: Describe the feature and its use case
    validations:
      required: true
  - type: textarea
    id: api
    attributes:
      label: Proposed API
      description: How should the feature be used?
      render: c
  - type: textarea
    id: alternatives
    attributes:
      label: Alternatives Considered
```

**Step 4: Create config.yml**

`.github/ISSUE_TEMPLATE/config.yml`:

```yaml
blank_issues_enabled: true
```

**Step 5: Create PR template**

`.github/PULL_REQUEST_TEMPLATE.md`:

```markdown
## Description
<!-- Describe your changes. Closes #issue_number -->

## C Code Quality Checklist

- [ ] Compiles without warnings with `-Wall -Wextra -Werror -pedantic -std=c23`
- [ ] `clang-format` applied (matches `.clang-format`)
- [ ] ASan + UBSan pass without errors
- [ ] TSan passes (if async/multithreaded code is affected)
- [ ] `clang-tidy` checks passed
- [ ] Tests added for new code
- [ ] Doxygen doc-comments updated in headers
- [ ] CHANGELOG entry added (if user-visible change)
```

**Step 6: Create CODEOWNERS**

`.github/CODEOWNERS`:

```
# Default owner
* @dantte-lp

# Core io_uring and server lifecycle
src/core/       @dantte-lp
src/net/        @dantte-lp

# TLS integration
src/tls/        @dantte-lp

# CI/CD
.github/        @dantte-lp
```

**Step 7: Create dependabot.yml**

`.github/dependabot.yml`:

```yaml
version: 2
updates:
  - package-ecosystem: "github-actions"
    directory: "/"
    schedule:
      interval: "weekly"
```

**Step 8: Commit**

```bash
git add .github/
git commit -m "chore: add GitHub issue templates (YAML), PR template, CODEOWNERS, dependabot"
```

---

## Task 4: Add CMake options tables to READMEs

**Priority:** P1

**Files:**
- Modify: `README.md` — add requirements table and CMake options table
- Modify: `README.ru.md` — add same tables (translated)

**Step 1: Read CMakeLists.txt to identify all user-facing options**

Read `CMakeLists.txt` and extract all `option()` and `set(... CACHE ...)` values. Also verify the existing "Build Requirements" section in both READMEs.

**Step 2: Replace Build Requirements list with table in README.md**

Replace the bulleted list at "## Build Requirements" (lines 175-183) with a structured table:

```markdown
## Build Requirements

| Requirement | Minimum | Notes |
|-------------|---------|-------|
| Linux kernel | 6.7+ | io_uring features, CVE-2024-0582 avoidance |
| glibc | 2.39+ | |
| C compiler | Clang 22+ or GCC 15+ | C23 support required |
| CMake | 4.0+ | |
| liburing | 2.7+ | |
| wolfSSL | 5.8.4+ | `--enable-quic` required |
| nghttp2 | latest | HTTP/2 |
| ngtcp2 | latest | QUIC transport |
| nghttp3 | latest | HTTP/3 |

## CMake Presets

| Preset | Compiler | Description |
|--------|----------|-------------|
| `clang-debug` | Clang | Debug build with mold linker |
| `clang-release` | Clang | Release with ThinLTO + lld |
| `clang-asan` | Clang | ASan + UBSan |
| `clang-msan` | Clang | Memory sanitizer |
| `clang-tsan` | Clang | Thread sanitizer |
| `clang-fuzz` | Clang | LibFuzzer targets |
| `clang-coverage` | Clang | Code coverage (llvm-cov) |
| `gcc-debug` | GCC 15 | Debug with -fanalyzer |
| `gcc-release` | GCC 15 | Release with full LTO |
| `gcc-asan` | GCC 15 | ASan + UBSan cross-validation |
```

**Step 3: Add the same tables to README.ru.md (translated)**

Replace "## Требования к сборке" (lines 170-178) with Russian-translated equivalent tables.

**Step 4: Commit**

```bash
git add README.md README.ru.md
git commit -m "docs: add requirements and CMake presets tables to both READMEs"
```

---

## Task 5: Add clang-coverage preset and fuzz infrastructure

**Priority:** P2

**Files:**
- Modify: `CMakePresets.json` — add `clang-coverage` preset
- Create: `tests/fuzz/corpus/parser/simple_get.raw` — seed input
- Create: `tests/fuzz/corpus/parser/post_with_body.raw` — seed input
- Create: `tests/fuzz/corpus/http2/settings_frame.raw` — seed input
- Create: `tests/fuzz/dictionaries/http.dict` — HTTP token dictionary

**Step 1: Add coverage preset to CMakePresets.json**

Add to `configurePresets` array (after `gcc-asan`):

```json
{
  "name": "clang-coverage",
  "displayName": "Clang Coverage (llvm-cov)",
  "description": "Code coverage via LLVM profiling instrumentation",
  "inherits": "clang-base",
  "cacheVariables": {
    "CMAKE_BUILD_TYPE": "Debug",
    "CMAKE_C_FLAGS": "-fprofile-instr-generate -fcoverage-mapping -g",
    "CMAKE_EXE_LINKER_FLAGS": "-fprofile-instr-generate -fuse-ld=mold"
  }
}
```

Add to `buildPresets`:

```json
{ "name": "clang-coverage", "configurePreset": "clang-coverage", "jobs": 0 }
```

Add to `testPresets`:

```json
{
  "name": "clang-coverage",
  "configurePreset": "clang-coverage",
  "output": { "outputOnFailure": true }
}
```

**Step 2: Create fuzz corpus seed files**

These are raw HTTP messages with literal `\r\n` bytes (use `printf` or binary write):

`tests/fuzz/corpus/parser/simple_get.raw`:
```
GET / HTTP/1.1\r\nHost: example.com\r\n\r\n
```

`tests/fuzz/corpus/parser/post_with_body.raw`:
```
POST /data HTTP/1.1\r\nHost: example.com\r\nContent-Length: 5\r\n\r\nhello
```

`tests/fuzz/corpus/http2/settings_frame.raw`:
Binary HTTP/2 connection preface + SETTINGS frame.

**Important:** Corpus files must contain actual `\r\n` bytes (0x0D 0x0A), not literal backslash-r-backslash-n. Use `printf` to create:
```bash
printf 'GET / HTTP/1.1\r\nHost: example.com\r\n\r\n' > tests/fuzz/corpus/parser/simple_get.raw
printf 'POST /data HTTP/1.1\r\nHost: example.com\r\nContent-Length: 5\r\n\r\nhello' > tests/fuzz/corpus/parser/post_with_body.raw
```

**Step 3: Create HTTP dictionary for LibFuzzer**

`tests/fuzz/dictionaries/http.dict`:

```
# HTTP methods
"GET"
"POST"
"PUT"
"DELETE"
"HEAD"
"OPTIONS"
"PATCH"

# HTTP versions
"HTTP/1.0"
"HTTP/1.1"

# Common headers
"Host:"
"Content-Length:"
"Transfer-Encoding:"
"Connection:"
"Content-Type:"
"Accept:"
"Authorization:"
"X-Request-Id:"

# Delimiters
"\r\n"
"\r\n\r\n"
": "

# Chunked encoding
"chunked"
"0\r\n\r\n"

# HTTP/2 connection preface
"PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n"
```

**Step 4: Verify coverage preset works**

```bash
cmake --preset clang-coverage && cmake --build --preset clang-coverage && ctest --preset clang-coverage
```

**Step 5: Commit**

```bash
git add CMakePresets.json tests/fuzz/corpus/ tests/fuzz/dictionaries/
git commit -m "test: add clang-coverage preset, fuzz corpus seeds, and HTTP dictionary"
```

---

## Task 6: Add SPDX license headers to source files

**Priority:** P2

**Files:**
- Modify: All `.c` and `.h` files in `src/` and `include/iohttp/`

**Step 1: Determine header format**

iohttp uses GPLv3. The SPDX header for each file:

```c
// SPDX-License-Identifier: GPL-3.0-only
```

This should be the **first line** of each `.c` and `.h` file.

**Step 2: Add SPDX headers to all source files**

Add the SPDX line as the first line of every `.c` file in `src/` and every `.h` file in `include/iohttp/`. Do NOT add to vendored files (`src/http/picohttpparser.c`, `src/http/picohttpparser.h`) — they have their own license.

List of files to modify (exclude picohttpparser):

`src/`:
- `src/core/ioh_buffer.c`, `src/core/ioh_loop.c`, `src/core/ioh_conn.c`, `src/core/ioh_ctx.c`, `src/core/ioh_log.c`, `src/core/ioh_server.c`
- `src/tls/ioh_tls.c`
- `src/http/ioh_http1.c`, `src/http/ioh_http2.c`, `src/http/ioh_http3.c`, `src/http/ioh_quic.c`, `src/http/ioh_request.c`, `src/http/ioh_response.c`, `src/http/ioh_proxy_proto.c`, `src/http/ioh_alt_svc.c`, `src/http/ioh_multipart.c`
- `src/router/ioh_router.c`, `src/router/ioh_radix.c`, `src/router/ioh_route_group.c`, `src/router/ioh_route_meta.c`, `src/router/ioh_route_inspect.c`
- `src/static/ioh_static.c`, `src/static/ioh_compress.c`, `src/static/ioh_spa.c`
- `src/ws/ioh_websocket.c`, `src/ws/ioh_sse.c`
- `src/middleware/ioh_middleware.c`, `src/middleware/ioh_cors.c`, `src/middleware/ioh_ratelimit.c`, `src/middleware/ioh_auth.c`, `src/middleware/ioh_security.c`

`include/iohttp/`:
- All `.h` files in `include/iohttp/`

**Step 3: Verify build still works**

```bash
cmake --build --preset clang-debug
```

**Step 4: Commit**

```bash
git add src/ include/
git commit -m "chore: add SPDX-License-Identifier headers to all source files"
```

---

## Task 7: Run quality pipeline and verify green

**Priority:** P2

**Goal:** Confirm the project hardening changes pass the full quality pipeline.

**Step 1: Full build and test**

```bash
cmake --preset clang-debug && cmake --build --preset clang-debug && ctest --preset clang-debug
```
Expected: All tests pass.

**Step 2: Format check**

```bash
cmake --build --preset clang-debug --target format-check
```
Expected: No formatting violations.

**Step 3: Full quality pipeline**

```bash
./scripts/quality.sh
```
Expected: `PASS: 6, FAIL: 0`

**Step 4: Commit (if any fixes needed)**

Only if quality tools require changes to the new files.

---

## Acceptance Criteria

- [ ] `.clangd` exists and provides LSP configuration
- [ ] `.editorconfig` exists with per-filetype indent rules
- [ ] `CMakeUserPresets.json` is in `.gitignore`
- [ ] `SECURITY.md` exists with vulnerability class list and reporting instructions
- [ ] `SUPPORT.md` exists with build, sanitizer, and quality pipeline instructions
- [ ] `.github/ISSUE_TEMPLATE/bug_report.yml` exists with compiler + kernel dropdown
- [ ] `.github/ISSUE_TEMPLATE/feature_request.yml` exists with API textarea
- [ ] `.github/ISSUE_TEMPLATE/config.yml` exists
- [ ] `.github/PULL_REQUEST_TEMPLATE.md` exists with C quality checklist
- [ ] `.github/CODEOWNERS` exists
- [ ] `.github/dependabot.yml` exists for GitHub Actions updates
- [ ] Both READMEs have requirements table (not bulleted list)
- [ ] Both READMEs have CMake presets table
- [ ] `clang-coverage` preset exists in `CMakePresets.json` and builds successfully
- [ ] `tests/fuzz/corpus/` populated with seed files
- [ ] `tests/fuzz/dictionaries/http.dict` exists
- [ ] SPDX `GPL-3.0-only` headers on all non-vendored source files
- [ ] `ctest --preset clang-debug` — all tests PASS
- [ ] `./scripts/quality.sh` — PASS: 6, FAIL: 0

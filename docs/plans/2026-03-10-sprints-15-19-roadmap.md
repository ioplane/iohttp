# Sprints 15–19: Feature Roadmap & liboas Integration

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Complete the remaining P0/P1/P2 features from the iohttp roadmap, culminating in liboas integration and 0.1.0 release readiness.

**Architecture:** Five sprints grouped by dependency order — P0 blockers first (health checks, per-route timeouts), then P1 framework features (cookies, Vary, streaming body, host routing), then liboas adapter integration, and finally P2 advanced features (circuit breaker, tracing hooks). Each sprint builds on the previous, with liboas integration as the capstone.

**Tech Stack:** C23, io_uring, wolfSSL, liburing, nghttp2, ngtcp2, nghttp3, yyjson, Unity, LibFuzzer.

**Skills (MANDATORY):**
- `iohttp-architecture` — naming, directory layout, ownership model
- `io-uring-patterns` — CQE handling, linked timeouts, send serialization

**Build/test:**
```bash
cmake --preset clang-debug && cmake --build --preset clang-debug && ctest --preset clang-debug
cmake --preset clang-asan && cmake --build --preset clang-asan && ctest --preset clang-asan
```

**Dependency graph:**

```
Sprint 13 (Critical Fixes) ──┐
Sprint 14 (Project Hardening)─┤
                               ├── Sprint 15 (P0: Health + Per-Route Timeout)
                               │        │
                               │        ├── Sprint 16 (P1: Cookies + Vary + Host Routing)
                               │        │        │
                               │        │        ├── Sprint 17 (P1: Streaming Body + Circuit Breaker)
                               │        │        │        │
                               │        │        │        ├── Sprint 18 (liboas Integration)
                               │        │        │        │        │
                               │        │        │        │        └── Sprint 19 (P2: Tracing + Hot Reload)
```

---

## Sprint 15: Health Checks & Per-Route Timeouts (P0)

**Goal:** Complete the remaining P0 features that block 0.1.0 release.

**Priority:** P0 — blocks production use.

### Task 15.1: Health Check Endpoint Framework

**Rationale:** Production deployments need liveness/readiness probes for orchestrators (Kubernetes, systemd watchdog, HAProxy health checks). Three endpoints: `/health` (basic liveness), `/ready` (accepting connections), `/live` (deep — io_uring ring, TLS context, buffer pools).

**Files:**
- Create: `src/core/io_health.h`
- Create: `src/core/io_health.c`
- Create: `tests/unit/test_io_health.c`
- Modify: `src/core/io_server.h` — add health config fields
- Modify: `src/core/io_server.c` — register health routes
- Modify: `CMakeLists.txt` — add source file

**Step 1: Read existing files**

Read `src/core/io_server.h`, `src/core/io_server.c`, `src/core/io_ctx.h`, `src/router/io_router.h` to understand server lifecycle and route registration.

**Step 2: Write tests**

```c
/* tests/unit/test_io_health.c */

void test_health_check_default_config(void)
{
    io_health_config_t cfg = {0};
    io_health_config_init(&cfg);
    TEST_ASSERT_EQUAL_STRING("/health", cfg.health_path);
    TEST_ASSERT_EQUAL_STRING("/ready", cfg.ready_path);
    TEST_ASSERT_EQUAL_STRING("/live", cfg.live_path);
    TEST_ASSERT_TRUE(cfg.enabled);
}

void test_health_liveness_returns_200(void)
{
    /* Setup: create server, register health routes */
    /* Dispatch GET /health */
    /* Assert: status 200, body {"status":"ok"} */
}

void test_health_readiness_returns_503_during_drain(void)
{
    /* Setup: create server, start drain */
    /* Dispatch GET /ready */
    /* Assert: status 503 */
}

void test_health_deep_check_returns_200(void)
{
    /* Setup: create server with working io_uring ring */
    /* Dispatch GET /live */
    /* Assert: status 200 with component statuses */
}

void test_health_custom_checker(void)
{
    /* Register custom health check callback */
    /* Dispatch GET /live */
    /* Assert: custom check output present */
}

void test_health_disabled(void)
{
    /* cfg.enabled = false */
    /* Assert: no health routes registered */
}
```

**Step 3: Run tests to verify they fail**

```bash
cmake --build --preset clang-debug && ctest --preset clang-debug -R test_io_health
```
Expected: FAIL (functions undefined)

**Step 4: Implement io_health.h**

```c
/* src/core/io_health.h */
#ifndef IOHTTP_CORE_HEALTH_H
#define IOHTTP_CORE_HEALTH_H

#include <stdbool.h>
#include <stdint.h>

typedef struct io_ctx io_ctx_t;
typedef struct io_router io_router_t;
typedef struct io_server io_server_t;

/* Health check callback — return 0 = healthy, negative errno = unhealthy */
typedef int (*io_health_check_fn)(const char **message, void *user_data);

constexpr uint32_t IO_HEALTH_MAX_CHECKS = 8;

typedef struct {
    const char *name;          /* e.g. "database", "cache" */
    io_health_check_fn check;
    void *user_data;
} io_health_checker_t;

typedef struct {
    bool enabled;              /* default true */
    const char *health_path;   /* default "/health" */
    const char *ready_path;    /* default "/ready" */
    const char *live_path;     /* default "/live" */
    io_health_checker_t checkers[IO_HEALTH_MAX_CHECKS];
    uint32_t checker_count;
} io_health_config_t;

void io_health_config_init(io_health_config_t *cfg);

/* Register health endpoints on a router. Server ptr used for readiness state. */
[[nodiscard]] int io_health_register(io_router_t *r, io_server_t *srv,
                                     const io_health_config_t *cfg);

/* Add a custom deep-liveness checker */
[[nodiscard]] int io_health_add_checker(io_health_config_t *cfg, const char *name,
                                        io_health_check_fn check, void *user_data);

/* Built-in handlers (registered internally, but exposed for testing) */
int io_health_handler(io_ctx_t *c);
int io_ready_handler(io_ctx_t *c);
int io_live_handler(io_ctx_t *c);

#endif
```

**Step 5: Implement io_health.c**

- `io_health_handler`: return `{"status":"ok"}` with 200
- `io_ready_handler`: check server drain state via `io_server_is_draining(srv)`, return 200 or 503
- `io_live_handler`: run all registered checkers, aggregate results into JSON, return 200 or 503
- `io_health_register`: call `io_router_get()` for each path; store server ptr via `io_route_opts_t`

**Step 6: Add io_server_is_draining() accessor**

```c
/* In io_server.h: */
bool io_server_is_draining(const io_server_t *srv);
```

**Step 7: Run tests**

```bash
cmake --build --preset clang-debug && ctest --preset clang-debug -R test_io_health
```
Expected: PASS

**Step 8: Commit**

```bash
git add src/core/io_health.h src/core/io_health.c src/core/io_server.h src/core/io_server.c \
        tests/unit/test_io_health.c CMakeLists.txt
git commit -m "feat(core): add health check endpoint framework (/health, /ready, /live)

Three built-in endpoints: liveness (200 OK), readiness (503 during drain),
deep liveness (pluggable checkers for io_uring, TLS, custom components).
Configurable paths, optional disable, custom checker registration."
```

---

### Task 15.2: Per-Route Timeout Configuration

**Rationale:** API routes handling file uploads need longer body timeouts (300s) while simple GET routes should keep the default (60s). Currently all routes share server-level timeouts.

**Files:**
- Modify: `src/router/io_router.h` — add timeout fields to `io_route_opts_t`
- Modify: `src/core/io_conn.h` — add per-request timeout override fields to `io_conn_t`
- Modify: `src/core/io_conn.c` — use per-route timeouts when set
- Create: `tests/unit/test_io_route_timeout.c`

**Step 1: Read existing timeout flow**

Read `src/core/io_conn.h` (timeout_phase enum), `src/core/io_conn.c` (how arm_recv/arm_send use timeouts), `src/core/io_loop.c` (LINK_TIMEOUT SQE submission).

**Step 2: Write tests**

```c
void test_route_timeout_defaults_to_server(void)
{
    /* Register route with no timeout override */
    /* Assert: connection uses server-level timeout values */
}

void test_route_timeout_override_body(void)
{
    /* Register route with .body_timeout_ms = 300000 */
    /* Assert: matched route opts contain timeout override */
    /* Assert: connection timeout uses 300000 for body phase */
}

void test_route_timeout_zero_means_no_override(void)
{
    /* Register route with .body_timeout_ms = 0 */
    /* Assert: falls back to server default */
}
```

**Step 3: Add timeout fields to io_route_opts_t**

```c
/* In io_router.h, add to io_route_opts_t: */
typedef struct {
    const io_route_meta_t *meta;
    void *oas_operation;
    uint32_t permissions;
    bool auth_required;
    uint32_t header_timeout_ms;  /* 0 = use server default */
    uint32_t body_timeout_ms;    /* 0 = use server default */
    uint32_t keepalive_timeout_ms; /* 0 = use server default */
} io_route_opts_t;
```

**Step 4: Apply per-route timeout after dispatch**

In connection handling (where dispatch result is used), check `match.opts->header_timeout_ms` etc., use non-zero values to override the server-level defaults for the linked timeout SQE.

**Step 5: Run tests**

```bash
cmake --build --preset clang-debug && ctest --preset clang-debug
```
Expected: All PASS including new timeout tests

**Step 6: Commit**

```bash
git add src/router/io_router.h src/core/io_conn.h src/core/io_conn.c \
        tests/unit/test_io_route_timeout.c CMakeLists.txt
git commit -m "feat(router): add per-route timeout configuration

Routes can override header_timeout_ms, body_timeout_ms, keepalive_timeout_ms
via io_route_opts_t. Zero means use server default. Enables longer timeouts
for file upload routes while keeping tight defaults elsewhere."
```

---

### Task 15.3: Quality verification

```bash
cmake --preset clang-asan && cmake --build --preset clang-asan && ctest --preset clang-asan
./scripts/quality.sh
```
Expected: All tests PASS, quality pipeline green.

---

## Sprint 16: Set-Cookie, Vary Header, Host Routing (P1)

**Goal:** Complete partial implementations (cookies, content negotiation) and add host-based routing.

**Priority:** P1 — production hardening.

### Task 16.1: Set-Cookie Response Builder (RFC 6265bis)

**Rationale:** `io_request_cookie()` exists for parsing, but there's no way to SET cookies in responses. Need `Set-Cookie` header builder with SameSite, Secure, HttpOnly, Domain, Path, Max-Age.

**Files:**
- Create: `src/http/io_cookie.h`
- Create: `src/http/io_cookie.c`
- Create: `tests/unit/test_io_cookie.c`
- Modify: `src/core/io_ctx.h` — add `io_ctx_set_cookie()` convenience

**Step 1: Read existing cookie code**

Read `src/http/io_request.c` (io_request_cookie function), `src/http/io_response.h`.

**Step 2: Write tests**

```c
void test_cookie_simple(void)
{
    /* io_cookie_t c = { .name = "sid", .value = "abc123" } */
    /* io_cookie_serialize(&c, buf, sizeof(buf)) */
    /* Assert: "sid=abc123" */
}

void test_cookie_full_attributes(void)
{
    /* .name="sid", .value="abc", .domain="example.com", .path="/",
       .max_age=3600, .secure=true, .http_only=true, .same_site=IO_SAME_SITE_LAX */
    /* Assert: "sid=abc; Domain=example.com; Path=/; Max-Age=3600; Secure; HttpOnly; SameSite=Lax" */
}

void test_cookie_same_site_strict(void) { /* Assert: "SameSite=Strict" */ }
void test_cookie_same_site_none_requires_secure(void) { /* Assert: SameSite=None forces Secure */ }
void test_cookie_name_validation(void) { /* Reject names with = ; \n */ }
void test_ctx_set_cookie(void) { /* io_ctx_set_cookie() adds Set-Cookie header */ }
```

**Step 3: Implement io_cookie.h**

```c
typedef enum : uint8_t {
    IO_SAME_SITE_DEFAULT, /* browser default */
    IO_SAME_SITE_LAX,
    IO_SAME_SITE_STRICT,
    IO_SAME_SITE_NONE,    /* requires Secure */
} io_same_site_t;

typedef struct {
    const char *name;
    const char *value;
    const char *domain;    /* nullable */
    const char *path;      /* nullable */
    int64_t max_age;       /* seconds, -1 = session, 0 = delete */
    io_same_site_t same_site;
    bool secure;
    bool http_only;
} io_cookie_t;

/* Serialize cookie into Set-Cookie header value. Returns bytes written or -ENOSPC. */
[[nodiscard]] int io_cookie_serialize(const io_cookie_t *cookie, char *buf, size_t buf_size);

/* Validate cookie name per RFC 6265bis */
[[nodiscard]] int io_cookie_validate_name(const char *name);
```

**Step 4: Add io_ctx_set_cookie()**

```c
/* In io_ctx.h: */
[[nodiscard]] int io_ctx_set_cookie(io_ctx_t *c, const io_cookie_t *cookie);
```

Implementation: serialize cookie → `io_ctx_set_header(c, "Set-Cookie", serialized)`.

**Step 5: Run tests, commit**

---

### Task 16.2: Vary Header in Content Negotiation

**Rationale:** `io_request_accepts()` and `io_compress_negotiate()` already select content variants but don't emit `Vary` header. Caches will serve wrong content without it.

**Files:**
- Modify: `src/static/io_static.c` — add `Vary: Accept-Encoding` when serving compressed
- Modify: `src/core/io_ctx.c` — add `Vary: Accept` when `io_ctx_json/text/html` uses negotiation
- Create: `tests/unit/test_io_vary.c`

**Step 1: Write tests**

```c
void test_static_compressed_sets_vary(void)
{
    /* Serve static file with Accept-Encoding negotiation */
    /* Assert: response has Vary: Accept-Encoding header */
}

void test_vary_multiple_headers(void)
{
    /* Both Accept and Accept-Encoding negotiation */
    /* Assert: Vary: Accept, Accept-Encoding */
}
```

**Step 2: Implement**

Add `io_response_add_vary()` helper that appends to existing Vary header (comma-separated). Call it from compression and content negotiation paths.

**Step 3: Run tests, commit**

---

### Task 16.3: Host-Based Routing

**Rationale:** Virtual hosting — same server, different domains, different route trees. Required for multi-tenant deployments.

**Files:**
- Create: `src/router/io_vhost.h`
- Create: `src/router/io_vhost.c`
- Create: `tests/unit/test_io_vhost.c`
- Modify: `src/core/io_server.h` — add vhost support

**Step 1: Design**

```c
typedef struct io_vhost io_vhost_t;

/* Create a virtual host router (dispatches to sub-routers by Host header) */
[[nodiscard]] io_vhost_t *io_vhost_create(void);
void io_vhost_destroy(io_vhost_t *vhost);

/* Add a host->router mapping. Wildcard: "*.example.com" */
[[nodiscard]] int io_vhost_add(io_vhost_t *v, const char *host, io_router_t *router);

/* Set default router for unmatched hosts */
void io_vhost_set_default(io_vhost_t *v, io_router_t *router);

/* Dispatch: extract Host header, find router, delegate */
[[nodiscard]] io_route_match_t io_vhost_dispatch(const io_vhost_t *v, const io_request_t *req);
```

**Step 2: Write tests**

```c
void test_vhost_exact_match(void) { /* "api.example.com" → API router */ }
void test_vhost_wildcard(void) { /* "*.example.com" → catch-all */ }
void test_vhost_default_fallback(void) { /* unknown host → default router */ }
void test_vhost_no_host_header(void) { /* HTTP/1.0 without Host → default */ }
void test_vhost_port_stripping(void) { /* "example.com:8080" → "example.com" */ }
```

**Step 3: Implement, test, commit**

---

### Task 16.4: Quality verification

```bash
cmake --preset clang-asan && cmake --build --preset clang-asan && ctest --preset clang-asan
```

---

## Sprint 17: Streaming Request Body & Circuit Breaker (P1)

**Goal:** Add streaming chunked input and circuit breaker middleware for backend protection.

**Priority:** P1 — production hardening.

### Task 17.1: Streaming Request Body (Chunked Input)

**Rationale:** Current API buffers entire request body before calling handler. Large file uploads or streaming inputs need incremental processing. This adds a pull-based body reader to `io_ctx_t`.

**Files:**
- Create: `src/http/io_body_reader.h`
- Create: `src/http/io_body_reader.c`
- Create: `tests/unit/test_io_body_reader.c`
- Modify: `src/core/io_ctx.h` — add body reader accessor
- Modify: `src/http/io_http1.c` — support streaming body delivery

**Step 1: Design pull-based reader**

```c
typedef struct io_body_reader io_body_reader_t;

typedef enum : uint8_t {
    IO_BODY_CHUNK,     /* data available in buf/len */
    IO_BODY_EOF,       /* no more data */
    IO_BODY_NEED_DATA, /* caller should re-arm recv and retry */
    IO_BODY_ERROR,     /* parsing error */
} io_body_status_t;

typedef struct {
    io_body_status_t status;
    const uint8_t *data;
    size_t len;
} io_body_chunk_t;

/* Read next chunk from body stream. Non-blocking. */
[[nodiscard]] io_body_chunk_t io_body_reader_read(io_body_reader_t *r);

/* Convenience: is body fully buffered? (small bodies skip streaming) */
bool io_body_reader_is_buffered(const io_body_reader_t *r);

/* In io_ctx.h: */
io_body_reader_t *io_ctx_body_reader(io_ctx_t *c);
```

**Step 2: Write tests**

```c
void test_body_reader_buffered_small(void)
{
    /* Body < max_body_size → fully buffered, single chunk + EOF */
}

void test_body_reader_chunked_transfer(void)
{
    /* Transfer-Encoding: chunked → multiple chunks → EOF */
}

void test_body_reader_content_length(void)
{
    /* Content-Length: N → read N bytes → EOF */
}

void test_body_reader_exceeds_limit(void)
{
    /* Body > max_body_size → IO_BODY_ERROR with 413 */
}
```

**Step 3: Implement, test, commit**

---

### Task 17.2: Circuit Breaker Middleware

**Rationale:** When a backend (database, upstream service) is failing, keep hammering it wastes resources and delays recovery. Circuit breaker stops sending requests after threshold failures, returning 503 immediately, and periodically probes to check recovery.

**Files:**
- Create: `src/middleware/io_circuit.h`
- Create: `src/middleware/io_circuit.c`
- Create: `tests/unit/test_io_circuit.c`

**Step 1: Design**

```c
typedef enum : uint8_t {
    IO_CIRCUIT_CLOSED,    /* normal operation, requests pass through */
    IO_CIRCUIT_OPEN,      /* failures exceeded threshold, requests rejected */
    IO_CIRCUIT_HALF_OPEN, /* probe one request to test recovery */
} io_circuit_state_t;

typedef struct {
    uint32_t failure_threshold;   /* failures to trip open (default 5) */
    uint32_t success_threshold;   /* successes in half-open to close (default 2) */
    uint32_t timeout_ms;          /* open → half-open transition (default 30000) */
    uint16_t open_status;         /* HTTP status when open (default 503) */
} io_circuit_config_t;

typedef struct io_circuit_breaker io_circuit_breaker_t;

[[nodiscard]] io_circuit_breaker_t *io_circuit_create(const io_circuit_config_t *cfg);
void io_circuit_destroy(io_circuit_breaker_t *cb);

/* Use as middleware: wraps handler, tracks success/failure */
io_middleware_fn io_circuit_middleware(io_circuit_breaker_t *cb);

/* Manual state inspection */
io_circuit_state_t io_circuit_state(const io_circuit_breaker_t *cb);

/* Record result manually (for non-middleware usage) */
void io_circuit_record_success(io_circuit_breaker_t *cb);
void io_circuit_record_failure(io_circuit_breaker_t *cb);
```

**Step 2: Write tests**

```c
void test_circuit_starts_closed(void) { /* state == CLOSED */ }
void test_circuit_trips_after_threshold(void)
{
    /* Record 5 failures → state == OPEN */
}
void test_circuit_rejects_when_open(void)
{
    /* Middleware returns 503 without calling handler */
}
void test_circuit_transitions_to_half_open(void)
{
    /* After timeout_ms → state == HALF_OPEN, one request passes through */
}
void test_circuit_closes_after_success_threshold(void)
{
    /* In HALF_OPEN: 2 successes → CLOSED */
}
void test_circuit_reopens_on_half_open_failure(void)
{
    /* In HALF_OPEN: 1 failure → OPEN again */
}
```

**Step 3: Implement**

State machine with `_Atomic` counters for thread safety. Timer-based open→half-open transition using monotonic clock (not io_uring timer — circuit breaker is passive, checked on next request).

**Step 4: Run tests, commit**

---

### Task 17.3: Quality verification

```bash
cmake --preset clang-asan && cmake --build --preset clang-asan && ctest --preset clang-asan
./scripts/quality.sh
```

---

## Sprint 18: liboas Integration (P1)

**Goal:** Connect iohttp's route introspection to liboas for automatic OpenAPI spec generation, request validation, and Scalar UI documentation serving.

**Priority:** P1 — key differentiator for REST framework.

**Prerequisites:** liboas project at `/opt/projects/repositories/liboas` must be installed or findable via `pkg_check_modules`. Sprint 15 health checks and per-route timeouts must be done (they add route metadata consumed by liboas).

### Task 18.1: liboas Adapter Middleware

**Rationale:** The adapter is the bridge between iohttp's route metadata and liboas's OpenAPI model. It walks the router, feeds metadata to liboas, and mounts validation + documentation endpoints. Follows the bunrouter+huma pattern.

**Files:**
- Create: `src/middleware/io_liboas.h`
- Create: `src/middleware/io_liboas.c`
- Create: `tests/unit/test_io_liboas.c`
- Modify: `CMakeLists.txt` — add optional liboas dependency

**Step 1: Read liboas public API**

Read `/opt/projects/repositories/liboas/include/liboas/oas_adapter.h`, `oas_types.h`, `oas_validator.h` to understand the liboas consumer interface.

**Step 2: Read iohttp introspection API**

Read `src/router/io_route_inspect.h` (`io_router_walk`, `io_route_info_t`), `src/router/io_route_meta.h` (`io_route_meta_t`, `io_param_meta_t`).

**Step 3: Design adapter**

```c
/* src/middleware/io_liboas.h */
#ifndef IOHTTP_MIDDLEWARE_LIBOAS_H
#define IOHTTP_MIDDLEWARE_LIBOAS_H

#include "router/io_router.h"

/* Forward declare liboas types (no #include of liboas headers in public API) */
typedef struct oas_adapter oas_adapter_t;

typedef struct {
    const char *title;              /* OpenAPI info.title */
    const char *version;            /* OpenAPI info.version */
    const char *description;        /* OpenAPI info.description (nullable) */
    const char *spec_path;          /* default "/openapi.json" */
    const char *docs_path;          /* default "/docs" (Scalar UI) */
    bool validate_requests;         /* default false */
    bool validate_responses;        /* default false (debug only) */
} io_liboas_config_t;

typedef struct io_liboas io_liboas_t;

void io_liboas_config_init(io_liboas_config_t *cfg);

/* Create adapter: walks router, builds OpenAPI spec via liboas */
[[nodiscard]] io_liboas_t *io_liboas_create(io_router_t *router,
                                            const io_liboas_config_t *cfg);
void io_liboas_destroy(io_liboas_t *adapter);

/* Mount spec + docs endpoints onto router */
[[nodiscard]] int io_liboas_mount(io_liboas_t *adapter, io_router_t *router);

/* Request validation middleware (use with io_router_use) */
io_middleware_fn io_liboas_validate_middleware(io_liboas_t *adapter);

/* Get generated spec JSON (for testing or embedding) */
const char *io_liboas_spec_json(const io_liboas_t *adapter, size_t *len);

#endif
```

**Step 4: Write tests**

```c
void test_liboas_create_with_routes(void)
{
    /* Create router with 3 routes + metadata */
    /* Create liboas adapter */
    /* Assert: adapter != nullptr */
    /* Assert: spec JSON contains all 3 paths */
}

void test_liboas_spec_endpoint(void)
{
    /* Mount adapter, dispatch GET /openapi.json */
    /* Assert: 200, Content-Type: application/json */
    /* Assert: valid OpenAPI 3.x JSON */
}

void test_liboas_docs_endpoint(void)
{
    /* Mount adapter, dispatch GET /docs */
    /* Assert: 200, Content-Type: text/html */
    /* Assert: contains Scalar UI script */
}

void test_liboas_route_metadata_mapping(void)
{
    /* Route with meta: summary, tags, params */
    /* Assert: OpenAPI path item has matching summary, tags, parameters */
}

void test_liboas_validate_request_valid(void)
{
    /* Valid request against spec → passes through to handler */
}

void test_liboas_validate_request_invalid(void)
{
    /* Missing required param → 422 with validation errors */
}
```

**Step 5: Implement adapter**

Key logic in `io_liboas_create()`:
1. Call `io_router_walk()` to enumerate all routes
2. For each `io_route_info_t`, map to liboas OpenAPI path item:
   - `info->pattern` → OpenAPI path (convert `:id` to `{id}`)
   - `info->method` → OpenAPI method
   - `info->meta->summary` → operation summary
   - `info->meta->tags` → operation tags
   - `info->meta->params` → OpenAPI parameters
   - `info->opts->oas_operation` → liboas-specific enrichment
3. Call `oas_adapter_create()` with built JSON
4. Store compiled adapter for runtime validation

**Step 6: Add CMake optional dependency**

```cmake
# In CMakeLists.txt:
option(IOHTTP_ENABLE_LIBOAS "Enable liboas OpenAPI integration" OFF)
if(IOHTTP_ENABLE_LIBOAS)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(LIBOAS REQUIRED IMPORTED_TARGET liboas)
    target_sources(iohttp PRIVATE src/middleware/io_liboas.c)
    target_link_libraries(iohttp PRIVATE PkgConfig::LIBOAS)
    target_compile_definitions(iohttp PRIVATE IOHTTP_HAS_LIBOAS=1)
endif()
```

**Step 7: Run tests, commit**

```bash
git add src/middleware/io_liboas.h src/middleware/io_liboas.c \
        tests/unit/test_io_liboas.c CMakeLists.txt
git commit -m "feat(middleware): add liboas adapter for OpenAPI spec generation and validation

Walks route introspection API, maps iohttp metadata to OpenAPI 3.x via liboas.
Mounts /openapi.json and /docs (Scalar UI) endpoints. Optional request/response
validation middleware. Enabled via -DIOHTTP_ENABLE_LIBOAS=ON."
```

---

### Task 18.2: End-to-End Integration Test

**Rationale:** Verify the full flow: register routes with metadata → create liboas adapter → serve spec → validate requests.

**Files:**
- Create: `tests/integration/test_liboas_integration.c`

**Step 1: Write integration test**

```c
void test_full_rest_api_with_liboas(void)
{
    /* 1. Create router */
    io_router_t *r = io_router_create();

    /* 2. Register routes with metadata */
    static const char *user_tags[] = {"users", nullptr};
    static const io_param_meta_t user_params[] = {
        {.name = "id", .in = IO_PARAM_PATH, .required = true, .description = "User ID"},
    };
    static const io_route_meta_t get_user_meta = {
        .summary = "Get user by ID",
        .tags = user_tags,
        .params = user_params,
        .param_count = 1,
    };
    io_router_get_with(r, "/api/users/:id", get_user_handler,
                       &(io_route_opts_t){.meta = &get_user_meta});

    /* 3. Create liboas adapter */
    io_liboas_config_t cfg;
    io_liboas_config_init(&cfg);
    cfg.title = "Test API";
    cfg.version = "1.0.0";
    cfg.validate_requests = true;

    io_liboas_t *oas = io_liboas_create(r, &cfg);
    TEST_ASSERT_NOT_NULL(oas);

    /* 4. Verify spec JSON */
    size_t spec_len = 0;
    const char *spec = io_liboas_spec_json(oas, &spec_len);
    TEST_ASSERT_NOT_NULL(spec);
    TEST_ASSERT_TRUE(spec_len > 0);
    /* Assert spec contains "/api/users/{id}" path */

    /* 5. Cleanup */
    io_liboas_destroy(oas);
    io_router_destroy(r);
}
```

**Step 2: Run, commit**

---

### Task 18.3: Quality verification

```bash
cmake --preset clang-asan -DIOHTTP_ENABLE_LIBOAS=ON && cmake --build --preset clang-asan
ctest --preset clang-asan
./scripts/quality.sh
```

---

## Sprint 19: Tracing Hooks & Configuration Hot Reload (P2)

**Goal:** Add W3C trace context propagation hooks and signal-based configuration hot reload.

**Priority:** P2 — completeness features.

### Task 19.1: W3C Trace Context Propagation

**Rationale:** Distributed tracing requires propagating `traceparent`/`tracestate` headers (W3C Trace Context). iohttp doesn't need a full OpenTelemetry SDK — just parse/generate trace headers and expose hooks for external collectors.

**Files:**
- Create: `src/core/io_trace.h`
- Create: `src/core/io_trace.c`
- Create: `tests/unit/test_io_trace.c`
- Modify: `src/core/io_ctx.h` — add trace context accessor

**Step 1: Design**

```c
/* W3C traceparent: version-trace_id-parent_id-trace_flags */
typedef struct {
    uint8_t trace_id[16];   /* 128-bit trace ID */
    uint8_t parent_id[8];   /* 64-bit span ID */
    uint8_t trace_flags;    /* 01 = sampled */
    bool valid;
} io_trace_ctx_t;

/* Trace event hook — called on request start/end */
typedef void (*io_trace_hook_fn)(const io_trace_ctx_t *trace, const io_request_t *req,
                                 uint16_t status, uint64_t duration_us, void *user_data);

/* Parse traceparent header */
[[nodiscard]] int io_trace_parse(const char *traceparent, io_trace_ctx_t *out);

/* Generate new traceparent string (for outgoing requests) */
[[nodiscard]] int io_trace_format(const io_trace_ctx_t *trace, char *buf, size_t buf_size);

/* Generate new trace context (root span) */
void io_trace_generate(io_trace_ctx_t *out);

/* Register a trace hook on the server (called per-request) */
[[nodiscard]] int io_server_set_trace_hook(io_server_t *srv, io_trace_hook_fn hook,
                                           void *user_data);

/* In io_ctx.h: */
const io_trace_ctx_t *io_ctx_trace(const io_ctx_t *c);
```

**Step 2: Write tests**

```c
void test_trace_parse_valid(void)
{
    /* "00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01" */
    /* Assert: trace_id, parent_id, flags=sampled, valid=true */
}

void test_trace_parse_invalid(void) { /* malformed → valid=false */ }

void test_trace_format_roundtrip(void)
{
    /* parse → format → compare strings */
}

void test_trace_generate_unique(void)
{
    /* Two calls → different trace_id and parent_id */
}

void test_trace_propagation_in_ctx(void)
{
    /* Request with traceparent → io_ctx_trace() returns parsed context */
}

void test_trace_hook_called(void)
{
    /* Register hook, process request → hook called with trace + status + duration */
}
```

**Step 3: Implement, test, commit**

---

### Task 19.2: Configuration Hot Reload

**Rationale:** Long-running servers need to update TLS certificates, rate limits, and route configuration without restarting. SIGHUP is the Unix convention for config reload.

**Files:**
- Create: `src/core/io_reload.h`
- Create: `src/core/io_reload.c`
- Create: `tests/unit/test_io_reload.c`
- Modify: `src/core/io_server.c` — add SIGHUP to signalfd mask

**Step 1: Design**

```c
/* Reload callback — called on SIGHUP. Return 0 to acknowledge, negative to reject. */
typedef int (*io_reload_fn)(io_server_t *srv, void *user_data);

/* Register a reload callback */
[[nodiscard]] int io_server_on_reload(io_server_t *srv, io_reload_fn fn, void *user_data);

/* Trigger reload programmatically (for testing without signals) */
[[nodiscard]] int io_server_reload(io_server_t *srv);
```

**Step 2: Write tests**

```c
void test_reload_callback_called(void)
{
    /* Register callback, trigger reload → callback called */
}

void test_reload_tls_context_swap(void)
{
    /* Callback replaces TLS context → new connections use new cert */
}

void test_reload_callback_failure(void)
{
    /* Callback returns -EINVAL → server continues with old config */
}
```

**Step 3: Implementation**

Add SIGHUP to the signalfd mask (alongside SIGTERM/SIGQUIT). When SIGHUP CQE is received, call registered reload callbacks. No atomics needed — reload runs in the event loop thread.

TLS reload pattern: create new `io_tls_ctx_t`, swap pointer with `io_server_set_tls()`, destroy old context after all active connections using it have drained.

**Step 4: Run tests, commit**

---

### Task 19.3: Quality verification + full test suite

```bash
cmake --preset clang-asan && cmake --build --preset clang-asan && ctest --preset clang-asan
cmake --preset clang-tsan && cmake --build --preset clang-tsan && ctest --preset clang-tsan
./scripts/quality.sh
```

---

## Summary: Sprint → Feature Matrix

| Sprint | Features | Priority | Status |
|--------|----------|----------|--------|
| **15** | Health checks (`/health`, `/ready`, `/live`), per-route timeouts | P0 | **DONE** |
| **16** | Set-Cookie (RFC 6265bis), Vary header, host-based routing | P1 | Planned |
| **17** | Streaming request body, circuit breaker middleware | P1 | Planned |
| **18** | liboas adapter: OpenAPI spec, Scalar UI, request validation | P1 | Planned |
| **19** | W3C trace context hooks, SIGHUP config hot reload | P2 | Planned |

## Features NOT in Sprints 15–19 (deferred to post-0.1.0)

| Feature | Priority | Rationale for deferral |
|---------|----------|----------------------|
| WebTransport / HTTP/3 Datagrams (RFC 9297) | P2 | Needs ngtcp2 datagram API maturity |
| OpenTelemetry SDK integration | P2 | Sprint 19 adds hooks; full SDK is consumer's concern |
| Dynamic connection pool scaling | P2 | Current static pool sufficient for 0.1.0 |
| HTTP caching (RFC 9111) | P1 | Large spec surface; better as Sprint 20+ |
| Content negotiation (full) | P1 | Partially done; full Vary+406 in Sprint 16 |

## Acceptance Criteria (Release Gate for 0.1.0)

After Sprint 19:

- [ ] All P0 features implemented and tested
- [ ] `ctest --preset clang-asan` — all tests PASS (zero ASan/UBSan errors)
- [ ] `ctest --preset clang-tsan` — all tests PASS (zero data races)
- [ ] `./scripts/quality.sh` — PASS: 6, FAIL: 0
- [ ] liboas integration works end-to-end (with `-DIOHTTP_ENABLE_LIBOAS=ON`)
- [ ] Health endpoints respond correctly during all server lifecycle phases
- [ ] Per-route timeouts verified with io_uring linked timeout SQEs
- [ ] Set-Cookie with SameSite/Secure/HttpOnly produces valid headers
- [ ] Host-based virtual hosting dispatches to correct routers
- [ ] Streaming body reader handles chunked TE without full buffering
- [ ] Circuit breaker state machine transitions correctly under failure
- [ ] W3C traceparent propagation round-trips correctly
- [ ] SIGHUP reload swaps TLS context without dropping connections

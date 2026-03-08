# iohttp REST Framework — Updated Sprint Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development to implement this plan task-by-task.

**Strategic pivot:** iohttp evolves from an embedded HTTP server library to a **full REST API framework** comparable to Gin (Go), Echo (Go), Axum (Rust), Actix (Rust), Express (Node.js). The foundation (io_uring, wolfSSL, router, middleware, HTTP/1.1, HTTP/2) is already built across S1–S7. This plan focuses on the framework-level features that differentiate a REST framework from a bare HTTP server.

**Core architectural change:** Unified context `io_ctx_t` replacing `fn(req, resp)` → `fn(io_ctx_t*)`, enabling middleware→handler data passing, request binding, response helpers, and per-request arena allocation.

**Current state:** S1–S7 complete (29 tests), HTTP/1.1 + HTTP/2 + TLS + router + middleware + WebSocket + SSE + compression + multipart + SPA + static files.

**liboas (separate project):** OpenAPI specification generation, validation, and documentation UI live in a separate `liboas` library — not in iohttp. iohttp provides raw route metadata via introspection API (`io_route_inspect.h`) and field descriptors (`io_field_desc_t`); liboas consumes these to generate OpenAPI 3.1 JSON, serve Scalar UI, and produce client SDKs. This mirrors the **bunrouter + huma** pattern in Go: the router/framework (iohttp) knows nothing about OpenAPI spec format; the adapter library (liboas) bridges the gap. Integration point: `io_liboas_adapter()` middleware that plugs liboas into iohttp's middleware chain.

**Build/test:**
```bash
cmake --preset clang-debug
cmake --build --preset clang-debug
ctest --preset clang-debug
```

---

## C23 Features Analysis for REST Framework

C23 (ISO/IEC 9899:2024) provides several features directly applicable to building a REST framework in C. Below is the analysis based on the C23 standard and Jens Gustedt's "Modern C" (8th ed.).

### Must-Use (already mandated in CLAUDE.md)

| Feature | REST Framework Application |
|---------|---------------------------|
| `nullptr` | Type-safe null for context values, optional params |
| `[[nodiscard]]` | All public API functions — prevents ignoring error returns |
| `constexpr` | Compile-time limits (max headers, buffer sizes, status codes) |
| `bool` keyword | Config flags, feature toggles |
| `_Static_assert` | Compile-time struct layout verification |

### High-Value for REST Framework

| Feature | Application | Example |
|---------|-------------|---------|
| `typeof` (Ch 18) | Type-safe macros for JSON binding, extractors | `#define IO_BIND_JSON(ctx, type) ((type*)io_ctx_bind_json(ctx, sizeof(type), _Alignof(type)))` |
| `_Generic` (Ch 18) | Type-dispatched response helpers, param extraction | `#define io_ctx_param(c, name, out) _Generic((out), int64_t*: io_ctx_param_i64, uint64_t*: io_ctx_param_u64, bool*: io_ctx_param_bool)(c, name, out)` |
| `__VA_OPT__` (Ch 17) | Optional middleware args in route macros | `#define IO_GET(r, path, handler, ...) io_router_get(r, path, handler __VA_OPT__(,) __VA_ARGS__)` |
| `<stdckdint.h>` (C.4) | Checked arithmetic for Content-Length, buffer sizes | `if (ckd_add(&total, header_len, body_len)) return -EOVERFLOW;` |
| `_Atomic` (Ch 20) | Lock-free Prometheus counters, connection stats | `_Atomic uint64_t requests_total;` |
| `constexpr` arrays | Static status text tables, method name tables | `constexpr char IO_STATUS_200[] = "OK";` |
| `thread_local` | Per-thread context for debugging, tracing | `thread_local io_trace_t *current_trace;` |

### Moderate-Value (use where appropriate)

| Feature | Application |
|---------|-------------|
| `[[maybe_unused]]` | Handler params in simple handlers |
| Digit separators | Large buffer constants: `constexpr size_t IO_MAX_BODY = 1'048'576;` |
| `unreachable()` | Switch exhaustiveness in method dispatch |
| `auto` type inference | Local variables in complex generic macros |
| `call_once` / `once_flag` | Singleton init for global state (now in `<stdlib.h>`) |
| `<stdbit.h>` (C.3) | Type-generic bit ops for permission bitmasks |

### C23 Macro Patterns for Declarative API

**Pattern 1: Default Arguments via `__VA_OPT__`** (from Modern C Ch 17)

```c
/* Route registration with optional middleware */
#define IO_ROUTE(method, path, handler, ...)   \
    io_router_handle_ex(r, method, path, handler \
        __VA_OPT__(, (io_middleware_fn[]){__VA_ARGS__}, \
                   IO_ALEN(__VA_ARGS__)))

/* Usage: */
IO_ROUTE(IO_METHOD_GET, "/api/users", list_users);
IO_ROUTE(IO_METHOD_POST, "/api/users", create_user, auth_mw, validate_mw);
```

**Pattern 2: Compile-Time Argument Counting** (ALEN from Modern C Ch 17)

```c
#define IO_ALEN(...) (sizeof((int[]){__VA_ARGS__}) / sizeof(int))
```

**Pattern 3: Type-Safe Extractors via `_Generic`**

```c
#define io_ctx_query(c, name, out) _Generic((out),     \
    int64_t*:  io_ctx_query_i64,                        \
    uint64_t*: io_ctx_query_u64,                        \
    double*:   io_ctx_query_f64,                        \
    bool*:     io_ctx_query_bool,                       \
    default:   io_ctx_query_str                         \
)(c, name, out)
```

**Pattern 4: `#embed` for Static Assets**

```c
/* Embed OpenAPI spec at compile time — zero runtime I/O */
constexpr unsigned char openapi_spec[] = {
    #embed "openapi.json"
};
```

### Avoid

| Feature | Reason |
|---------|--------|
| `auto` for function params | Not in C23 (C++ only) |
| `_BitInt` | No REST framework use case |
| `#embed` for non-static | Only for compile-time assets (spec files, SPA bundles) |

---

## Sprint 8: io_ctx_t — Unified Request Context (BREAKING CHANGE)

**Goal:** Replace `fn(req, resp)` handler signature with `fn(io_ctx_t*)` — the central architectural change enabling framework-level features. This is a breaking change affecting all existing handlers and middleware.

**Rationale:** Every modern REST framework (Gin, Echo, Axum, Actix, Express) uses a unified context. It enables:
1. Middleware→handler data passing (`io_ctx_set(c, "user", user)` / `io_ctx_get(c, "user")`)
2. Per-request arena allocator (zero-copy JSON binding, temp strings)
3. Chainable response helpers (`io_ctx_json(c, 200, doc)`)
4. Unified error handling (`io_ctx_error(c, 500, "db failure")`)
5. Access to request, response, route params, connection state in one pointer

### Task 8.1: io_ctx_t Core Type

**Files:**
- Create: `src/core/io_ctx.h`
- Create: `src/core/io_ctx.c`
- Create: `tests/unit/test_io_ctx.c`
- Modify: `CMakeLists.txt`

**io_ctx.h:**

```c
/**
 * @file io_ctx.h
 * @brief Unified request context — the central type of the iohttp framework.
 *
 * Replaces separate (req, resp) parameters with a single context pointer,
 * enabling middleware data passing, per-request arena allocation, and
 * chainable response helpers.
 */

#ifndef IOHTTP_CORE_CTX_H
#define IOHTTP_CORE_CTX_H

#include "http/io_request.h"
#include "http/io_response.h"
#include <stddef.h>
#include <stdint.h>

/* ---- Forward declarations ---- */
typedef struct io_ctx io_ctx_t;
typedef struct io_server io_server_t;

/* ---- New handler signature ---- */
typedef int (*io_handler_fn)(io_ctx_t *c);
typedef int (*io_middleware_fn)(io_ctx_t *c, io_handler_fn next);

/* ---- Context key-value store ---- */
constexpr uint32_t IO_CTX_MAX_VALUES = 16;

typedef struct {
    const char *key;
    void *value;
    void (*destructor)(void *);  /* optional, called on ctx reset */
} io_ctx_value_t;

/* ---- Per-request arena allocator ---- */
constexpr size_t IO_CTX_ARENA_DEFAULT = 4096;

typedef struct {
    uint8_t *base;
    size_t   size;
    size_t   used;
} io_arena_t;

/* ---- Context ---- */
struct io_ctx {
    io_request_t  *req;          /* parsed request (read-only in handlers) */
    io_response_t *resp;         /* response builder */
    io_server_t   *server;       /* back-pointer to server */
    io_arena_t     arena;        /* per-request bump allocator */
    io_ctx_value_t values[IO_CTX_MAX_VALUES];
    uint32_t       value_count;
    int            conn_fd;      /* connection file descriptor */
    bool           aborted;      /* set by io_ctx_abort() */
};

/* ---- Lifecycle (internal, called by server) ---- */

[[nodiscard]] int io_ctx_init(io_ctx_t *c, io_request_t *req,
                               io_response_t *resp, io_server_t *srv);
void io_ctx_reset(io_ctx_t *c);
void io_ctx_destroy(io_ctx_t *c);

/* ---- Context values (middleware→handler data passing) ---- */

[[nodiscard]] int io_ctx_set(io_ctx_t *c, const char *key, void *value);
[[nodiscard]] int io_ctx_set_with_destructor(io_ctx_t *c, const char *key,
                                              void *value, void (*dtor)(void *));
void *io_ctx_get(const io_ctx_t *c, const char *key);

/* ---- Request accessors (convenience wrappers) ---- */

const char *io_ctx_param(const io_ctx_t *c, const char *name);
[[nodiscard]] int io_ctx_param_i64(const io_ctx_t *c, const char *name, int64_t *out);
[[nodiscard]] int io_ctx_param_u64(const io_ctx_t *c, const char *name, uint64_t *out);
const char *io_ctx_query(const io_ctx_t *c, const char *name);
const char *io_ctx_header(const io_ctx_t *c, const char *name);
const char *io_ctx_cookie(const io_ctx_t *c, const char *name);
io_method_t io_ctx_method(const io_ctx_t *c);
const char *io_ctx_path(const io_ctx_t *c);
const uint8_t *io_ctx_body(const io_ctx_t *c, size_t *len);

/* ---- Response helpers (chainable) ---- */

[[nodiscard]] int io_ctx_status(io_ctx_t *c, uint16_t status);
[[nodiscard]] int io_ctx_set_header(io_ctx_t *c, const char *name, const char *value);
[[nodiscard]] int io_ctx_json(io_ctx_t *c, uint16_t status, const char *json);
[[nodiscard]] int io_ctx_text(io_ctx_t *c, uint16_t status, const char *text);
[[nodiscard]] int io_ctx_html(io_ctx_t *c, uint16_t status, const char *html);
[[nodiscard]] int io_ctx_blob(io_ctx_t *c, uint16_t status,
                               const char *content_type,
                               const uint8_t *data, size_t len);
[[nodiscard]] int io_ctx_no_content(io_ctx_t *c);
[[nodiscard]] int io_ctx_redirect(io_ctx_t *c, uint16_t status, const char *url);
[[nodiscard]] int io_ctx_error(io_ctx_t *c, uint16_t status, const char *message);
void io_ctx_abort(io_ctx_t *c);

/* ---- Arena allocator ---- */

void *io_ctx_alloc(io_ctx_t *c, size_t size);
void *io_ctx_alloc_aligned(io_ctx_t *c, size_t size, size_t align);
char *io_ctx_sprintf(io_ctx_t *c, const char *fmt, ...);

#endif /* IOHTTP_CORE_CTX_H */
```

**Tests (test_io_ctx.c):**

```c
void test_ctx_init_destroy(void);
void test_ctx_set_get_value(void);
void test_ctx_set_max_values(void);
void test_ctx_get_missing_returns_null(void);
void test_ctx_set_with_destructor(void);
void test_ctx_reset_calls_destructors(void);
void test_ctx_param_delegates_to_request(void);
void test_ctx_query_delegates_to_request(void);
void test_ctx_header_delegates_to_request(void);
void test_ctx_json_sets_content_type_and_body(void);
void test_ctx_text_sets_content_type(void);
void test_ctx_redirect_sets_location(void);
void test_ctx_no_content_sets_204(void);
void test_ctx_error_json_format(void);
void test_ctx_abort_sets_flag(void);
void test_ctx_arena_alloc(void);
void test_ctx_arena_alloc_aligned(void);
void test_ctx_arena_overflow_realloc(void);
void test_ctx_sprintf_arena(void);
```

**New tests: ~19**

### Task 8.2: Migrate Handler Signatures

**Files:**
- Modify: `src/router/io_router.h` — change `io_handler_fn` typedef
- Modify: `src/router/io_route_group.h` — change `io_middleware_fn` typedef
- Modify: `src/middleware/io_middleware.h` — update chain execution
- Modify: `src/middleware/io_middleware.c` — pass `io_ctx_t*` through chain
- Modify: `src/router/io_router.c` — dispatch passes `io_ctx_t*`
- Modify: `src/router/io_route_group.c` — group dispatch
- Modify: all `tests/unit/test_io_*.c` — update handler signatures

**Migration:**

Old:
```c
typedef int (*io_handler_fn)(io_request_t *req, io_response_t *resp);
typedef int (*io_middleware_fn)(io_request_t *req, io_response_t *resp, io_next_fn next);
```

New:
```c
typedef int (*io_handler_fn)(io_ctx_t *c);
typedef int (*io_middleware_fn)(io_ctx_t *c, io_handler_fn next);
```

Middleware calls `next(c)` instead of `next(req, resp)`. Handlers access request/response via `c->req` / `c->resp` or convenience wrappers.

**Migration in middleware chain (`io_chain_execute`):**

Old: `io_chain_execute(req, resp, global_mw, global_count, group_mw, group_count, handler)`
New: `io_chain_execute(io_ctx_t *c, io_middleware_fn *global_mw, ..., io_handler_fn handler)`

### Task 8.3: Migrate Built-in Middleware

**Files:**
- Modify: `src/middleware/io_cors.c` — `io_cors_middleware(io_ctx_t *c, io_handler_fn next)`
- Modify: `src/middleware/io_ratelimit.c` — same pattern
- Modify: `src/middleware/io_auth.c` — same pattern
- Modify: `src/middleware/io_security.c` — same pattern
- Modify: all middleware headers

Each middleware factory now returns `io_middleware_fn` with the new signature. Internal closures access config via static/global state or embedded struct (same pattern as current).

### Task 8.4: Update HTTP/1.1 and HTTP/2 Integration

**Files:**
- Modify: `src/http/io_http1.c` — create `io_ctx_t` per request, pass to dispatch
- Modify: `src/http/io_http2.c` — create `io_ctx_t` per stream
- Modify: `tests/integration/test_http1_server.c`
- Modify: `tests/integration/test_http2_server.c`

**New tests: ~5 (integration tests for ctx flow)**

**Sprint 8 total: ~24 new tests. Estimated: ~53 total.**

---

## Sprint 9: JSON API — Request Binding & Response Builder

**Goal:** First-class JSON support via yyjson — request body binding, structured response building, error responses, content negotiation. This is what makes iohttp a REST framework vs an HTTP library.

### Task 9.1: yyjson Integration Layer

**Files:**
- Create: `src/json/io_json.h`
- Create: `src/json/io_json.c`
- Create: `tests/unit/test_io_json.c`
- Modify: `CMakeLists.txt`

**io_json.h — JSON read/write wrappers around yyjson:**

```c
/* Parse JSON from request body (zero-copy where possible) */
[[nodiscard]] int io_json_parse(const uint8_t *data, size_t len, io_json_doc_t **doc);
void io_json_free(io_json_doc_t *doc);

/* Extract typed fields */
[[nodiscard]] int io_json_get_str(io_json_doc_t *doc, const char *path, const char **out);
[[nodiscard]] int io_json_get_i64(io_json_doc_t *doc, const char *path, int64_t *out);
[[nodiscard]] int io_json_get_u64(io_json_doc_t *doc, const char *path, uint64_t *out);
[[nodiscard]] int io_json_get_f64(io_json_doc_t *doc, const char *path, double *out);
[[nodiscard]] int io_json_get_bool(io_json_doc_t *doc, const char *path, bool *out);

/* Build JSON responses */
[[nodiscard]] io_json_writer_t *io_json_writer_create(io_arena_t *arena);
[[nodiscard]] int io_json_write_str(io_json_writer_t *w, const char *key, const char *val);
[[nodiscard]] int io_json_write_i64(io_json_writer_t *w, const char *key, int64_t val);
[[nodiscard]] int io_json_write_bool(io_json_writer_t *w, const char *key, bool val);
[[nodiscard]] int io_json_write_null(io_json_writer_t *w, const char *key);
[[nodiscard]] int io_json_write_obj_begin(io_json_writer_t *w, const char *key);
[[nodiscard]] int io_json_write_obj_end(io_json_writer_t *w);
[[nodiscard]] int io_json_write_arr_begin(io_json_writer_t *w, const char *key);
[[nodiscard]] int io_json_write_arr_end(io_json_writer_t *w);
const char *io_json_writer_finish(io_json_writer_t *w, size_t *len);
```

**Tests:**
```c
void test_json_parse_valid(void);
void test_json_parse_invalid(void);
void test_json_parse_empty(void);
void test_json_get_str(void);
void test_json_get_i64(void);
void test_json_get_bool(void);
void test_json_get_nested_path(void);
void test_json_get_missing_field(void);
void test_json_writer_simple_object(void);
void test_json_writer_nested(void);
void test_json_writer_array(void);
void test_json_writer_arena_alloc(void);
```

**New tests: ~12**

### Task 9.2: Request Binding (io_ctx_bind_json)

**Files:**
- Create: `src/json/io_bind.h`
- Create: `src/json/io_bind.c`
- Create: `tests/unit/test_io_bind.c`

**io_bind.h — Struct-to-JSON binding via field descriptors:**

```c
typedef enum : uint8_t {
    IO_FIELD_STR,
    IO_FIELD_I64,
    IO_FIELD_U64,
    IO_FIELD_F64,
    IO_FIELD_BOOL,
} io_field_type_t;

typedef struct {
    const char     *json_name;   /* JSON field name */
    io_field_type_t type;        /* field type */
    size_t          offset;      /* offsetof(struct, field) */
    bool            required;    /* validation: must be present */
} io_field_desc_t;

/* Bind JSON request body to a struct using field descriptors */
[[nodiscard]] int io_ctx_bind_json(io_ctx_t *c, void *out,
                                    const io_field_desc_t *fields, size_t field_count);

/* Convenience macro for field descriptor */
#define IO_FIELD(type_enum, struct_type, field, json_key, req) \
    { .json_name = (json_key), .type = (type_enum), \
      .offset = offsetof(struct_type, field), .required = (req) }
```

**Example usage:**
```c
typedef struct { const char *name; int64_t age; bool active; } user_t;

static const io_field_desc_t user_fields[] = {
    IO_FIELD(IO_FIELD_STR,  user_t, name,   "name",   true),
    IO_FIELD(IO_FIELD_I64,  user_t, age,    "age",    true),
    IO_FIELD(IO_FIELD_BOOL, user_t, active, "active", false),
};

int create_user(io_ctx_t *c) {
    user_t user = {0};
    int rc = io_ctx_bind_json(c, &user, user_fields, 3);
    if (rc < 0) return rc;  /* auto 400 with validation errors */
    /* ... */
    return io_ctx_json(c, 201, "{\"id\": 1}");
}
```

**Tests:**
```c
void test_bind_json_all_fields(void);
void test_bind_json_missing_required(void);
void test_bind_json_missing_optional(void);
void test_bind_json_wrong_type(void);
void test_bind_json_empty_body(void);
void test_bind_json_invalid_json(void);
void test_bind_json_extra_fields_ignored(void);
void test_bind_json_nested_not_supported(void);
```

**New tests: ~8**

### Task 9.3: Structured Error Responses

**Files:**
- Create: `src/json/io_error.h`
- Create: `src/json/io_error.c`
- Create: `tests/unit/test_io_error.c`

**RFC 9457 Problem Details format:**

```c
typedef struct {
    uint16_t    status;
    const char *type;     /* URI reference */
    const char *title;    /* short summary */
    const char *detail;   /* human-readable explanation */
    const char *instance; /* URI for specific occurrence */
} io_problem_t;

/* Send RFC 9457 Problem Details JSON response */
[[nodiscard]] int io_ctx_problem(io_ctx_t *c, const io_problem_t *problem);

/* Convenience for common errors */
[[nodiscard]] int io_ctx_bad_request(io_ctx_t *c, const char *detail);
[[nodiscard]] int io_ctx_not_found(io_ctx_t *c);
[[nodiscard]] int io_ctx_unauthorized(io_ctx_t *c, const char *detail);
[[nodiscard]] int io_ctx_forbidden(io_ctx_t *c);
[[nodiscard]] int io_ctx_internal_error(io_ctx_t *c, const char *detail);
[[nodiscard]] int io_ctx_conflict(io_ctx_t *c, const char *detail);
[[nodiscard]] int io_ctx_unprocessable(io_ctx_t *c, const char *detail);

/* Validation error collection */
typedef struct {
    const char *field;
    const char *message;
} io_validation_error_t;

[[nodiscard]] int io_ctx_validation_error(io_ctx_t *c,
                                           const io_validation_error_t *errors,
                                           size_t count);
```

**Tests:**
```c
void test_problem_json_format(void);
void test_bad_request_response(void);
void test_not_found_response(void);
void test_validation_error_multiple(void);
void test_validation_error_single(void);
void test_problem_custom_type(void);
```

**New tests: ~6**

### Task 9.4: Cookie Builder & Content Negotiation

**Files:**
- Create: `src/http/io_cookie.h`
- Create: `src/http/io_cookie.c`
- Create: `tests/unit/test_io_cookie.c`

**io_cookie.h — Set-Cookie builder (RFC 6265bis):**

```c
typedef struct {
    const char *name;
    const char *value;
    const char *domain;
    const char *path;
    uint32_t    max_age;       /* 0 = session cookie */
    bool        secure;
    bool        http_only;
    enum : uint8_t { IO_SAMESITE_NONE, IO_SAMESITE_LAX, IO_SAMESITE_STRICT } same_site;
} io_cookie_t;

[[nodiscard]] int io_ctx_set_cookie(io_ctx_t *c, const io_cookie_t *cookie);
[[nodiscard]] int io_ctx_delete_cookie(io_ctx_t *c, const char *name, const char *path);
```

**Tests:**
```c
void test_cookie_basic(void);
void test_cookie_full_options(void);
void test_cookie_secure_httponly(void);
void test_cookie_samesite_strict(void);
void test_cookie_delete(void);
void test_cookie_missing_name(void);
```

**New tests: ~6**

**Sprint 9 total: ~32 new tests. Estimated: ~85 total.**

---

## Sprint 10: HTTP/3 — QUIC Transport

**Goal:** HTTP/3 via ngtcp2 + nghttp3 with wolfSSL QUIC crypto. Container already has all deps installed. This sprint focuses on UDP transport, QUIC connection lifecycle, and HTTP/3 frame processing.

### Task 10.1: QUIC Connection Context

**Files:**
- Create: `src/http/io_quic.h`
- Create: `src/http/io_quic.c`
- Create: `tests/unit/test_io_quic.c`

**Core types:**
```c
typedef struct io_quic_conn io_quic_conn_t;

typedef struct {
    uint32_t max_streams_bidi;     /* default 100 */
    uint32_t initial_max_data;     /* default 1 MiB */
    uint32_t idle_timeout_ms;      /* default 30000 */
    uint32_t max_udp_payload;      /* default 1350 */
    bool     enable_0rtt;          /* default false */
} io_quic_config_t;

[[nodiscard]] int io_quic_config_init(io_quic_config_t *cfg);
[[nodiscard]] int io_quic_config_validate(const io_quic_config_t *cfg);
```

**Tests: ~8**

### Task 10.2: ngtcp2 + wolfSSL Crypto Integration

**Files:**
- Create: `src/tls/io_quic_crypto.h`
- Create: `src/tls/io_quic_crypto.c`
- Create: `tests/unit/test_io_quic_crypto.c`

Uses `ngtcp2_crypto_wolfssl` — the only QUIC library with native wolfSSL support. Callbacks for initial/handshake/application crypto, key updates, retry token validation.

**Tests: ~6**

### Task 10.3: UDP Listener with io_uring

**Files:**
- Create: `src/net/io_udp.h`
- Create: `src/net/io_udp.c`
- Create: `tests/unit/test_io_udp.c`

UDP recvmsg/sendmsg via io_uring with provided buffers. GRO/GSO support for packet batching. QUIC connection ID-based demuxing.

**Tests: ~6**

### Task 10.4: nghttp3 HTTP/3 Session

**Files:**
- Create: `src/http/io_http3.h`
- Create: `src/http/io_http3.c`
- Create: `tests/unit/test_io_http3.c`

nghttp3 callbacks for request headers, data, stream lifecycle. Maps HTTP/3 streams to `io_ctx_t` contexts. Integrates with existing router and middleware chain.

**Tests: ~10**

### Task 10.5: ALPN Multi-Protocol Dispatch

**Files:**
- Modify: `src/tls/io_tls.c` — add `h3` ALPN
- Modify: `src/core/io_server.c` — dual TCP+UDP listeners
- Create: `tests/integration/test_http3_server.c`

**Tests: ~6**

**Sprint 10 total: ~36 new tests. Estimated: ~121 total.**

---

## Sprint 11: Observability — Logging, Metrics, Tracing

**Goal:** Production-grade observability: structured logging, Prometheus metrics, request tracing. Lock-free where possible using `_Atomic`.

### Task 11.1: Structured Logging

**Files:**
- Create: `src/core/io_log.h`
- Create: `src/core/io_log.c`
- Create: `tests/unit/test_io_log.c`

JSON-formatted structured logging with configurable levels. Request-scoped log context via `io_ctx_t`. No external deps (custom implementation, not stumpless — keep iohttp dependency-free for logging).

```c
typedef enum : uint8_t {
    IO_LOG_TRACE, IO_LOG_DEBUG, IO_LOG_INFO,
    IO_LOG_WARN, IO_LOG_ERROR, IO_LOG_FATAL,
} io_log_level_t;

void io_log_set_level(io_log_level_t level);
void io_log_set_output(int fd);

/* Macros that include file:line */
#define IO_LOG_INFO(fmt, ...) io_log_write(IO_LOG_INFO, __FILE__, __LINE__, fmt __VA_OPT__(,) __VA_ARGS__)
#define IO_LOG_ERROR(fmt, ...) io_log_write(IO_LOG_ERROR, __FILE__, __LINE__, fmt __VA_OPT__(,) __VA_ARGS__)
```

**Tests: ~8**

### Task 11.2: Prometheus Metrics

**Files:**
- Create: `src/middleware/io_metrics.h`
- Create: `src/middleware/io_metrics.c`
- Create: `tests/unit/test_io_metrics.c`

Lock-free counters via `_Atomic uint64_t`. Histogram for request duration (pre-defined buckets). Text exposition format (`/metrics` endpoint). Middleware that auto-records per-request metrics.

```c
/* Counter */
void io_metric_inc(io_metric_t *m);
void io_metric_add(io_metric_t *m, uint64_t n);

/* Histogram */
void io_metric_observe(io_histogram_t *h, double value);

/* Built-in metrics */
typedef struct {
    io_metric_t requests_total;
    io_metric_t requests_active;
    io_metric_t request_bytes_total;
    io_metric_t response_bytes_total;
    io_histogram_t request_duration_seconds;
    io_metric_t connections_total;
    io_metric_t connections_active;
    io_metric_t tls_handshakes_total;
    io_metric_t errors_total;
} io_server_metrics_t;

/* Middleware */
[[nodiscard]] io_middleware_fn io_metrics_create(io_server_metrics_t *metrics);

/* Exposition */
[[nodiscard]] int io_metrics_handler(io_ctx_t *c);
```

**Tests: ~10**

### Task 11.3: Request Tracing

**Files:**
- Create: `src/middleware/io_trace.h`
- Create: `src/middleware/io_trace.c`
- Create: `tests/unit/test_io_trace.c`

Request ID generation (UUID v7 or monotonic), W3C Trace Context propagation (traceparent header), timing spans for middleware/handler execution.

```c
typedef struct {
    uint8_t  trace_id[16];
    uint8_t  span_id[8];
    uint64_t start_ns;    /* monotonic clock */
    uint64_t end_ns;
} io_trace_t;

/* Middleware: assigns trace ID, propagates traceparent */
[[nodiscard]] io_middleware_fn io_trace_create(void);

/* Access from handler */
const io_trace_t *io_ctx_trace(const io_ctx_t *c);
```

**Tests: ~6**

### Task 11.4: Access Log Middleware

**Files:**
- Create: `src/middleware/io_accesslog.h`
- Create: `src/middleware/io_accesslog.c`
- Create: `tests/unit/test_io_accesslog.c`

Combined/JSON format access logging with configurable output. Uses `io_ctx_t` for request/response metadata. Integrates with tracing for request ID inclusion.

**Tests: ~6**

**Sprint 11 total: ~30 new tests. Estimated: ~151 total.**

---

## Sprint 12: Route Metadata, liboas Integration & Developer Experience

**Goal:** Extend route introspection with endpoint metadata (summary, tags, field descriptors). Provide the adapter interface for liboas (separate project) to generate OpenAPI 3.1 and serve Scalar UI. iohttp does NOT generate OpenAPI spec itself — that's liboas's job. Pattern: **bunrouter + huma** (Go).

### Task 12.1: Endpoint Metadata & Extended Introspection

**Files:**
- Create: `src/router/io_endpoint_meta.h`
- Modify: `src/router/io_route_inspect.h` — extend with metadata
- Modify: `src/router/io_route_inspect.c`
- Create: `tests/unit/test_io_endpoint_meta.c`

Metadata attached to routes at registration time. iohttp stores it; liboas reads it via introspection API.

```c
typedef struct {
    const char          *summary;
    const char          *description;
    const char          *tag;
    const io_field_desc_t *request_body;    /* reuse bind descriptors from S9 */
    size_t               request_body_count;
    const io_field_desc_t *response_body;
    size_t               response_body_count;
    uint16_t             success_status;    /* default 200 */
} io_endpoint_meta_t;

/* Registration with metadata */
[[nodiscard]] int io_router_get_meta(io_router_t *r, const char *pattern,
                                      io_handler_fn h, const io_endpoint_meta_t *meta);

/* Extended introspection — returns metadata for liboas consumption */
const io_endpoint_meta_t *io_route_info_meta(const io_route_info_t *info);
```

**Declarative macro (using `__VA_OPT__`):**

```c
#define IO_ENDPOINT(method, path, handler, ...)                              \
    io_router_handle_meta(r, method, path, handler                           \
        __VA_OPT__(, &(io_endpoint_meta_t){__VA_ARGS__}))
```

**What iohttp provides (this sprint):**
- `io_endpoint_meta_t` struct + registration API
- Extended `io_route_inspect()` returning metadata
- Field descriptors (`io_field_desc_t` from S9) reused as schema source

**What liboas provides (separate project, later):**
- `io_liboas_adapter()` middleware — reads iohttp introspection → generates OpenAPI 3.1
- `GET /openapi.json` handler
- Scalar UI handler (`GET /docs`)
- Client SDK generation
- OpenAPI spec validation

**Tests: ~8**

### Task 12.2: liboas Adapter Interface

**Files:**
- Create: `src/middleware/io_liboas.h` — adapter interface (header-only)

Defines the callback interface that liboas implements. iohttp ships this header; liboas links against it. Users call `io_liboas_mount(router, liboas_instance)`.

```c
/* Adapter interface — implemented by liboas, consumed by iohttp */
typedef struct io_oas_adapter io_oas_adapter_t;

/* Mount liboas adapter: registers /openapi.json and /docs handlers */
[[nodiscard]] int io_oas_mount(io_router_t *r, io_oas_adapter_t *adapter,
                                const char *spec_path, const char *docs_path);
```

**Tests: ~4 (mock adapter)**

### Task 12.3: Graceful Shutdown & Health Checks

**Files:**
- Create: `src/core/io_health.h`
- Create: `src/core/io_health.c`
- Create: `tests/unit/test_io_health.c`

Readiness/liveness probe handlers, graceful shutdown with drain timeout, connection tracking for zero-downtime deploys.

```c
/* Health check handlers */
int io_health_ready(io_ctx_t *c);    /* GET /healthz */
int io_health_live(io_ctx_t *c);     /* GET /livez */

/* Graceful shutdown */
[[nodiscard]] int io_server_shutdown_graceful(io_server_t *srv, uint32_t drain_timeout_ms);
```

**Tests: ~6**

**Sprint 12 total: ~18 new tests. Estimated: ~169 total.**

---

## Sprint 13: Stabilization & Benchmark

**Goal:** Performance benchmarking, fuzzing targets, documentation, release preparation for v0.1.0.

### Task 13.1: Fuzz Targets

**Files:**
- Create: `tests/fuzz/fuzz_http1_parser.c`
- Create: `tests/fuzz/fuzz_json_bind.c`
- Create: `tests/fuzz/fuzz_router_dispatch.c`
- Create: `tests/fuzz/fuzz_cookie_parse.c`
- Modify: `CMakeLists.txt`

LibFuzzer targets (Clang only) covering HTTP parsing, JSON binding, router dispatch, cookie parsing.

### Task 13.2: Performance Benchmarks

**Files:**
- Create: `tests/bench/bench_router.c`
- Create: `tests/bench/bench_json.c`
- Create: `tests/bench/bench_ctx.c`

Microbenchmarks for router dispatch (10K routes), JSON parse/write (various sizes), context init/reset cycle.

### Task 13.3: Example Applications

**Files:**
- Create: `examples/hello.c` — minimal hello world
- Create: `examples/rest_api.c` — CRUD REST API with JSON binding
- Create: `examples/auth_api.c` — API with auth middleware
- Create: `examples/spa.c` — SPA + API server

### Task 13.4: Documentation

**Files:**
- Create: `docs/en/03-rest-api-guide.md` — REST framework usage guide
- Create: `docs/en/04-json-binding.md` — JSON binding reference
- Create: `docs/en/05-middleware.md` — middleware authoring guide
- Update: `README.md` — reflect REST framework positioning

### Task 13.5: Release v0.1.0

- Tag `v0.1.0`
- CHANGELOG.md
- Verify all sanitizers pass (ASan, UBSan, MSan, TSan)
- Coverage >= 80%

**Sprint 13 total: ~0 new unit tests (fuzz + bench + docs). Estimated: ~171 total.**

---

## Summary

| Sprint | Theme | Key Deliverable | New Tests | Running Total |
|--------|-------|-----------------|-----------|---------------|
| S1–S7 | Foundation | io_uring + TLS + HTTP/1.1 + HTTP/2 + router + middleware + WS + SSE | 29 | 29 |
| **S8** | **io_ctx_t** | **Unified context, handler migration (BREAKING)** | **~24** | **~53** |
| **S9** | **JSON API** | **yyjson binding, errors, cookies** | **~32** | **~85** |
| **S10** | **HTTP/3** | **ngtcp2 + nghttp3 + QUIC** | **~36** | **~121** |
| **S11** | **Observability** | **Logging, metrics, tracing** | **~30** | **~151** |
| **S12** | **Metadata + liboas + DX** | **Endpoint metadata, liboas adapter interface, health checks** | **~18** | **~169** |
| **S13** | **Stabilization** | **Fuzzing, benchmarks, docs, v0.1.0** | **—** | **~169** |

## Competitive Positioning After S13

| Feature | iohttp | Gin | Echo | Axum | Actix | Express |
|---------|--------|-----|------|------|-------|---------|
| Language | C23 | Go | Go | Rust | Rust | JS |
| Context | `io_ctx_t*` | `*gin.Context` | `echo.Context` | extractors | `HttpRequest` | `(req, res)` |
| JSON binding | field descriptors | `ShouldBindJSON` | `Bind` | `Json<T>` | `web::Json<T>` | body-parser |
| Error format | RFC 9457 | custom | `HTTPError` | custom | `ResponseError` | custom |
| OpenAPI | liboas (adapter) | swaggo | echo-swagger | utoipa | paperclip | swagger |
| Middleware | `fn(ctx, next)` | `HandlerFunc` | `MiddlewareFunc` | tower layers | middleware | `fn(req,res,next)` |
| I/O | io_uring | goroutines | goroutines | tokio | tokio/actix | libuv |
| TLS | wolfSSL native | stdlib | stdlib | rustls | rustls/native | — |
| HTTP/3 | ngtcp2+nghttp3 | quic-go | — | quinn | — | — |

**iohttp's unique position:** Only C REST framework with io_uring core, wolfSSL native TLS, HTTP/1.1+2+3, and Go/Rust-style developer ergonomics via `io_ctx_t`. OpenAPI via separate liboas project (bunrouter+huma pattern).

## Critical Implementation Notes

1. **S8 is the hardest sprint** — touches every file, breaks all existing tests. Must be done atomically.
2. **Arena allocator in io_ctx_t** is key for zero-copy JSON — strings point into arena, freed on request completion.
3. **Field descriptors** (S9) serve dual purpose: JSON binding AND OpenAPI schema generation (S12).
4. **`_Generic` macros** must work on both Clang 22 and GCC 15 — test on both compilers.
5. **QUIC (S10)** depends on S8 (io_ctx_t) being complete — HTTP/3 streams need context.
6. **Lock-free metrics (S11)** use `_Atomic` — no mutex contention on hot path.

# Sprint 15: Health Check Endpoints & Per-Route Timeouts

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Implement the two remaining P0 features that block 0.1.0 release: health check endpoint framework (`/health`, `/ready`, `/live`) and per-route timeout configuration.

**Architecture:** Health checks are a standalone module (`io_health`) that registers routes on the server's router. JSON responses via yyjson. Per-route timeouts extend `io_route_opts_t` with override fields; `io_server.c` applies them after route match, before arming body recv.

**Tech Stack:** C23, io_uring (linked timeouts), yyjson (JSON responses), Unity (tests).

**Skills (MANDATORY):**
- `iohttp-architecture` — naming (`io_module_verb_noun()`), directory layout, ownership model
- `io-uring-patterns` — linked timeout SQE pattern, CQE error handling

**Build/test:**
```bash
# Inside container (podman exec -w <worktree-path> iohttp-dev bash -c "...")
cmake --preset clang-debug && cmake --build --preset clang-debug && ctest --preset clang-debug
cmake --preset clang-asan && cmake --build --preset clang-asan && ctest --preset clang-asan
```

**Existing files (read before implementing):**
- `src/core/io_server.h` — server config, lifecycle API
- `src/core/io_server.c` — `dispatch_request()`, `arm_recv()`, `timeout_ms_for_phase()`, CQE loop
- `src/core/io_ctx.h` — per-request context, `io_ctx_json()` response helper
- `src/core/io_conn.h` — connection state, `io_timeout_phase_t`
- `src/router/io_router.h` — `io_route_opts_t`, `io_route_match_t`, handler signature
- `CMakeLists.txt` — `io_add_test()` macro, library linkage pattern

---

## Task 1: Health Check Endpoint Framework

**Priority:** P0 — orchestrators (Kubernetes, systemd, HAProxy) need health probes.

**Files:**
- Create: `src/core/io_health.h`
- Create: `src/core/io_health.c`
- Create: `tests/unit/test_io_health.c`
- Modify: `CMakeLists.txt` — add `io_health` library and test

**Step 1: Read existing files**

Read `src/core/io_server.h`, `src/core/io_server.c` (focus on `struct io_server`, `dispatch_request`, shutdown flow), `src/core/io_ctx.h` (response helpers), `src/router/io_router.h` (route registration).

**Step 2: Create io_health.h**

```c
/**
 * @file io_health.h
 * @brief Health check endpoint framework (/health, /ready, /live).
 */

#ifndef IOHTTP_CORE_HEALTH_H
#define IOHTTP_CORE_HEALTH_H

#include <stdbool.h>
#include <stdint.h>

typedef struct io_ctx io_ctx_t;
typedef struct io_router io_router_t;
typedef struct io_server io_server_t;

/**
 * Health check callback.
 * @param message  Output: human-readable status (points into static/literal storage).
 * @param user_data  Opaque user context.
 * @return 0 = healthy, negative errno = unhealthy.
 */
typedef int (*io_health_check_fn)(const char **message, void *user_data);

constexpr uint32_t IO_HEALTH_MAX_CHECKS = 8;

typedef struct {
    const char *name;
    io_health_check_fn check;
    void *user_data;
} io_health_checker_t;

typedef struct {
    bool enabled;
    const char *health_path;   /**< default "/health" */
    const char *ready_path;    /**< default "/ready" */
    const char *live_path;     /**< default "/live" */
    io_health_checker_t checkers[IO_HEALTH_MAX_CHECKS];
    uint32_t checker_count;
} io_health_config_t;

/**
 * @brief Initialize health config with defaults.
 */
void io_health_config_init(io_health_config_t *cfg);

/**
 * @brief Add a custom deep-liveness checker.
 * @return 0 on success, -ENOSPC if max checkers reached.
 */
[[nodiscard]] int io_health_add_checker(io_health_config_t *cfg, const char *name,
                                        io_health_check_fn check, void *user_data);

/**
 * @brief Register health endpoints on a router.
 * @param r    Router to register routes on.
 * @param srv  Server pointer (used by /ready to check drain state).
 * @param cfg  Health configuration.
 * @return 0 on success, negative errno on error.
 */
[[nodiscard]] int io_health_register(io_router_t *r, io_server_t *srv,
                                     const io_health_config_t *cfg);

#endif /* IOHTTP_CORE_HEALTH_H */
```

**Step 3: Create io_health.c**

Implementation details:

1. **`io_health_config_init()`**: Set `enabled=true`, paths to `"/health"`, `"/ready"`, `"/live"`, `checker_count=0`.

2. **`io_health_add_checker()`**: Append to `cfg->checkers[]`, increment count, return `-ENOSPC` if `checker_count >= IO_HEALTH_MAX_CHECKS`.

3. Three static handler functions (stored via closure pattern using `io_ctx_get()`):

   **`health_handler(io_ctx_t *c)`**:
   ```c
   return io_ctx_json(c, 200, "{\"status\":\"ok\"}");
   ```

   **`ready_handler(io_ctx_t *c)`**:
   - Get server pointer from context: `io_server_t *srv = c->server;`
   - If `srv == nullptr` or `io_server_is_draining(srv)`:
     ```c
     return io_ctx_json(c, 503, "{\"status\":\"unavailable\"}");
     ```
   - Otherwise:
     ```c
     return io_ctx_json(c, 200, "{\"status\":\"ready\"}");
     ```

   **`live_handler(io_ctx_t *c)`**:
   - Retrieve stored `io_health_config_t *` from context value `"_health_cfg"`
   - If no checkers: return `io_ctx_json(c, 200, "{\"status\":\"ok\"}")`
   - Otherwise: iterate `checkers[]`, call each, build JSON with yyjson:
     ```json
     {"status":"ok","checks":{"database":"ok","cache":"ok"}}
     ```
     or `{"status":"degraded","checks":{"database":"error: connection refused"}}` with 503
   - Use `yyjson_mut_doc` to build response, `yyjson_mut_write()` to serialize
   - Free yyjson doc after calling `io_ctx_json()`

4. **`io_health_register()`**:
   - If `!cfg->enabled`, return 0 (no routes registered)
   - Store a static copy of `cfg` (or store pointer — cfg must outlive router)
   - Call `io_router_get(r, cfg->health_path, health_handler)`
   - Call `io_router_get(r, cfg->ready_path, ready_handler)`
   - Call `io_router_get(r, cfg->live_path, live_handler)`
   - For live_handler to access checkers, use `io_router_get_with()` with opts containing user_data. **Alternative (simpler)**: use a file-static `const io_health_config_t *` pointer since health config is singleton per server.
   - Return 0 on success

5. **`io_server_is_draining()`** — add to `io_server.h` / `io_server.c`:
   ```c
   bool io_server_is_draining(const io_server_t *srv);
   ```
   Implementation: `return srv != nullptr && srv->stopped;`

**Step 4: Write tests (tests/unit/test_io_health.c)**

```c
#include "core/io_health.h"
#include "core/io_ctx.h"
#include "core/io_server.h"
#include "http/io_request.h"
#include "http/io_response.h"
#include "router/io_router.h"

#include <string.h>
#include <unity.h>

void setUp(void) {}
void tearDown(void) {}

/* ---- Config defaults ---- */

void test_health_config_defaults(void)
{
    io_health_config_t cfg;
    io_health_config_init(&cfg);
    TEST_ASSERT_TRUE(cfg.enabled);
    TEST_ASSERT_EQUAL_STRING("/health", cfg.health_path);
    TEST_ASSERT_EQUAL_STRING("/ready", cfg.ready_path);
    TEST_ASSERT_EQUAL_STRING("/live", cfg.live_path);
    TEST_ASSERT_EQUAL_UINT32(0, cfg.checker_count);
}

/* ---- Add checker ---- */

static int dummy_check(const char **msg, void *ctx)
{
    (void)ctx;
    *msg = "ok";
    return 0;
}

void test_health_add_checker(void)
{
    io_health_config_t cfg;
    io_health_config_init(&cfg);
    int rc = io_health_add_checker(&cfg, "db", dummy_check, nullptr);
    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_EQUAL_UINT32(1, cfg.checker_count);
    TEST_ASSERT_EQUAL_STRING("db", cfg.checkers[0].name);
}

void test_health_add_checker_overflow(void)
{
    io_health_config_t cfg;
    io_health_config_init(&cfg);
    for (uint32_t i = 0; i < IO_HEALTH_MAX_CHECKS; i++) {
        TEST_ASSERT_EQUAL_INT(0, io_health_add_checker(&cfg, "c", dummy_check, nullptr));
    }
    TEST_ASSERT_EQUAL_INT(-ENOSPC, io_health_add_checker(&cfg, "overflow", dummy_check, nullptr));
}

/* ---- Route registration ---- */

void test_health_register_creates_routes(void)
{
    io_router_t *r = io_router_create();
    TEST_ASSERT_NOT_NULL(r);

    io_health_config_t cfg;
    io_health_config_init(&cfg);

    int rc = io_health_register(r, nullptr, &cfg);
    TEST_ASSERT_EQUAL_INT(0, rc);

    /* Verify routes exist by dispatching */
    io_route_match_t m = io_router_dispatch(r, IO_METHOD_GET, "/health", 7);
    TEST_ASSERT_EQUAL_INT(IO_MATCH_FOUND, m.status);

    m = io_router_dispatch(r, IO_METHOD_GET, "/ready", 6);
    TEST_ASSERT_EQUAL_INT(IO_MATCH_FOUND, m.status);

    m = io_router_dispatch(r, IO_METHOD_GET, "/live", 5);
    TEST_ASSERT_EQUAL_INT(IO_MATCH_FOUND, m.status);

    io_router_destroy(r);
}

void test_health_disabled_no_routes(void)
{
    io_router_t *r = io_router_create();
    io_health_config_t cfg;
    io_health_config_init(&cfg);
    cfg.enabled = false;

    int rc = io_health_register(r, nullptr, &cfg);
    TEST_ASSERT_EQUAL_INT(0, rc);

    io_route_match_t m = io_router_dispatch(r, IO_METHOD_GET, "/health", 7);
    TEST_ASSERT_EQUAL_INT(IO_MATCH_NOT_FOUND, m.status);

    io_router_destroy(r);
}

/* ---- Health handler returns 200 ---- */

void test_health_handler_returns_200(void)
{
    io_router_t *r = io_router_create();
    io_health_config_t cfg;
    io_health_config_init(&cfg);
    (void)io_health_register(r, nullptr, &cfg);

    io_route_match_t m = io_router_dispatch(r, IO_METHOD_GET, "/health", 7);
    TEST_ASSERT_EQUAL_INT(IO_MATCH_FOUND, m.status);

    /* Call handler with test context */
    io_request_t req;
    memset(&req, 0, sizeof(req));
    io_response_t resp;
    (void)io_response_init(&resp);
    io_ctx_t ctx;
    (void)io_ctx_init(&ctx, &req, &resp, nullptr);

    int rc = m.handler(&ctx);
    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_EQUAL_UINT16(200, resp.status);

    io_ctx_destroy(&ctx);
    io_response_destroy(&resp);
    io_router_destroy(r);
}

/* ---- Custom paths ---- */

void test_health_custom_paths(void)
{
    io_router_t *r = io_router_create();
    io_health_config_t cfg;
    io_health_config_init(&cfg);
    cfg.health_path = "/healthz";
    cfg.ready_path = "/readyz";
    cfg.live_path = "/livez";

    (void)io_health_register(r, nullptr, &cfg);

    io_route_match_t m = io_router_dispatch(r, IO_METHOD_GET, "/healthz", 8);
    TEST_ASSERT_EQUAL_INT(IO_MATCH_FOUND, m.status);

    m = io_router_dispatch(r, IO_METHOD_GET, "/health", 7);
    TEST_ASSERT_EQUAL_INT(IO_MATCH_NOT_FOUND, m.status);

    io_router_destroy(r);
}

/* ---- Test runner ---- */

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_health_config_defaults);
    RUN_TEST(test_health_add_checker);
    RUN_TEST(test_health_add_checker_overflow);
    RUN_TEST(test_health_register_creates_routes);
    RUN_TEST(test_health_disabled_no_routes);
    RUN_TEST(test_health_handler_returns_200);
    RUN_TEST(test_health_custom_paths);
    return UNITY_END();
}
```

**Step 5: Add to CMakeLists.txt**

After the io_log section (~line 287), add:

```cmake
# ============================================================================
# Sprint 15: Health check endpoints
# ============================================================================

add_library(io_health STATIC src/core/io_health.c)
target_include_directories(io_health PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(io_health PUBLIC io_router io_ctx io_request io_response io_server)
if(YYJSON_INCLUDE_DIR AND YYJSON_LIBRARY)
    target_include_directories(io_health PRIVATE ${YYJSON_INCLUDE_DIR})
    target_link_libraries(io_health PRIVATE ${YYJSON_LIBRARY})
    target_compile_definitions(io_health PRIVATE IOHTTP_HAVE_YYJSON)
endif()

io_add_test(test_io_health tests/unit/test_io_health.c
    io_health io_router io_radix io_ctx io_request io_response)
```

**Note:** `io_health` depends on `io_server` for `io_server_is_draining()`. To avoid circular dependency (io_server already links io_router), consider: the health handlers access server via `ctx->server` (already available). `io_server_is_draining()` can be a standalone function in `io_server.h`/`io_server.c` that just checks the flag. The `io_health` library needs the declaration but not necessarily to link against `io_server` — the test will link both.

**Step 6: Run tests to verify they fail**

```bash
cmake --build --preset clang-debug && ctest --preset clang-debug -R test_io_health
```
Expected: FAIL (source files don't exist yet → build error)

**Step 7: Implement io_health.c**

Follow the implementation details from Step 3. Key points:
- Use file-static pointer for config: `static const io_health_config_t *s_health_cfg;`
- `io_health_register()` stores cfg pointer and server pointer in file-static variables
- Handlers access them directly (health endpoints are singleton per process)
- For `/live` JSON with checkers, use yyjson behind `#ifdef IOHTTP_HAVE_YYJSON` guard; fallback to simple string concat if yyjson unavailable

**Step 8: Run tests**

```bash
cmake --build --preset clang-debug && ctest --preset clang-debug -R test_io_health
```
Expected: PASS (7 tests)

**Step 9: Run full suite**

```bash
ctest --preset clang-debug
```
Expected: All tests PASS (no regressions)

**Step 10: Commit**

```bash
git add src/core/io_health.h src/core/io_health.c src/core/io_server.h src/core/io_server.c \
        tests/unit/test_io_health.c CMakeLists.txt
git commit -m "feat(core): add health check endpoint framework (/health, /ready, /live)

Three built-in endpoints: liveness (200 OK), readiness (503 during drain),
deep liveness (pluggable checkers with yyjson JSON aggregation).
Configurable paths, optional disable, up to 8 custom checker callbacks."
```

---

## Task 2: Per-Route Timeout Configuration

**Priority:** P0 — file upload routes need longer body timeouts than API routes.

**Files:**
- Modify: `src/router/io_router.h` — add timeout fields to `io_route_opts_t`
- Modify: `src/core/io_conn.h` — add per-request timeout overrides to `io_conn_t`
- Modify: `src/core/io_server.c` — apply route timeouts after header parse, before body recv
- Create: `tests/unit/test_io_route_timeout.c`
- Modify: `CMakeLists.txt` — add timeout test

**Step 1: Read existing timeout flow**

Read `src/core/io_server.c` focusing on:
- `timeout_ms_for_phase()` (line ~193) — returns server-level timeout by phase
- `arm_recv()` (line ~207) — creates linked timeout SQE using `timeout_ms_for_phase()`
- CQE RECV handler (line ~596) — where body recv is re-armed with `IO_TIMEOUT_BODY`
- `dispatch_request()` (line ~351) — where route match happens

**Key insight:** The body recv timeout is set BEFORE the route is matched in the current code:
```c
// Line 752-756 in io_server.c:
if (req.content_length > 0 && body_avail < req.content_length) {
    conn->timeout_phase = IO_TIMEOUT_BODY;
    (void)arm_recv(srv, conn);
}
```

But we DO have the parsed request (method + path) at this point, so we can do a quick route lookup to get timeout overrides before arming recv.

**Step 2: Add timeout fields to io_route_opts_t**

In `src/router/io_router.h`, extend `io_route_opts_t`:

```c
typedef struct {
    const io_route_meta_t *meta;
    void *oas_operation;
    uint32_t permissions;
    bool auth_required;
    uint32_t header_timeout_ms;    /**< 0 = use server default */
    uint32_t body_timeout_ms;      /**< 0 = use server default */
    uint32_t keepalive_timeout_ms; /**< 0 = use server default */
} io_route_opts_t;
```

**Step 3: Add per-request timeout overrides to io_conn_t**

In `src/core/io_conn.h`, add to `io_conn_t`:

```c
/* Per-request timeout overrides (0 = use server default) */
uint32_t route_body_timeout_ms;
uint32_t route_keepalive_timeout_ms;
```

**Step 4: Modify timeout_ms_for_phase() in io_server.c**

```c
static uint32_t timeout_ms_for_phase(const io_server_t *srv, const io_conn_t *conn,
                                     io_timeout_phase_t phase)
{
    switch (phase) {
    case IO_TIMEOUT_HEADER:
        return srv->config.header_timeout_ms;
    case IO_TIMEOUT_BODY:
        if (conn->route_body_timeout_ms > 0)
            return conn->route_body_timeout_ms;
        return srv->config.body_timeout_ms;
    case IO_TIMEOUT_KEEPALIVE:
        if (conn->route_keepalive_timeout_ms > 0)
            return conn->route_keepalive_timeout_ms;
        return srv->config.keepalive_timeout_ms;
    default:
        return 0;
    }
}
```

Update `arm_recv()` call to pass `conn`:
```c
uint32_t tmo_ms = timeout_ms_for_phase(srv, conn, conn->timeout_phase);
```

**Step 5: Apply per-route timeout before body recv**

In `io_server_run_once()`, in the RECV handler, BEFORE the body wait arm_recv, do a quick route lookup:

```c
/* After parsing headers, before waiting for body: */
if (req.content_length > 0 && body_avail < req.content_length) {
    /* Quick route lookup for timeout overrides */
    io_route_match_t m = io_router_dispatch(srv->router, req.method,
                                            req.path, req.path_len);
    if (m.status == IO_MATCH_FOUND && m.opts != nullptr) {
        conn->route_body_timeout_ms = m.opts->body_timeout_ms;
        conn->route_keepalive_timeout_ms = m.opts->keepalive_timeout_ms;
    }

    conn->timeout_phase = IO_TIMEOUT_BODY;
    (void)arm_recv(srv, conn);
    processed++;
    continue;
}
```

Also apply keepalive timeout override after dispatch in `dispatch_request()` or in the send-complete path (already done via `timeout_ms_for_phase`).

**Step 6: Reset per-route overrides on new request**

When connection is reused for keep-alive, reset the overrides. In the send-complete path where keepalive is re-armed:

```c
/* Reset per-route timeout overrides for next request */
conn->route_body_timeout_ms = 0;
conn->route_keepalive_timeout_ms = 0;
```

**Step 7: Write tests (tests/unit/test_io_route_timeout.c)**

```c
#include "core/io_ctx.h"
#include "core/io_server.h"
#include "http/io_request.h"
#include "http/io_response.h"
#include "router/io_router.h"

#include <string.h>
#include <unity.h>

void setUp(void) {}
void tearDown(void) {}

static int upload_handler(io_ctx_t *c)
{
    return io_ctx_json(c, 200, "{\"ok\":true}");
}

static int api_handler(io_ctx_t *c)
{
    return io_ctx_json(c, 200, "{\"ok\":true}");
}

void test_route_opts_timeout_defaults_zero(void)
{
    io_route_opts_t opts = {0};
    TEST_ASSERT_EQUAL_UINT32(0, opts.header_timeout_ms);
    TEST_ASSERT_EQUAL_UINT32(0, opts.body_timeout_ms);
    TEST_ASSERT_EQUAL_UINT32(0, opts.keepalive_timeout_ms);
}

void test_route_timeout_override_in_match(void)
{
    io_router_t *r = io_router_create();
    TEST_ASSERT_NOT_NULL(r);

    static const io_route_opts_t upload_opts = {
        .body_timeout_ms = 300000,
    };
    int rc = io_router_post_with(r, "/upload", upload_handler, &upload_opts);
    TEST_ASSERT_EQUAL_INT(0, rc);

    io_route_match_t m = io_router_dispatch(r, IO_METHOD_POST, "/upload", 7);
    TEST_ASSERT_EQUAL_INT(IO_MATCH_FOUND, m.status);
    TEST_ASSERT_NOT_NULL(m.opts);
    TEST_ASSERT_EQUAL_UINT32(300000, m.opts->body_timeout_ms);

    io_router_destroy(r);
}

void test_route_timeout_zero_means_server_default(void)
{
    io_router_t *r = io_router_create();
    TEST_ASSERT_NOT_NULL(r);

    static const io_route_opts_t api_opts = {
        .body_timeout_ms = 0,
    };
    (void)io_router_get_with(r, "/api/data", api_handler, &api_opts);

    io_route_match_t m = io_router_dispatch(r, IO_METHOD_GET, "/api/data", 9);
    TEST_ASSERT_EQUAL_INT(IO_MATCH_FOUND, m.status);
    TEST_ASSERT_NOT_NULL(m.opts);
    TEST_ASSERT_EQUAL_UINT32(0, m.opts->body_timeout_ms);

    io_router_destroy(r);
}

void test_route_no_opts_returns_null(void)
{
    io_router_t *r = io_router_create();
    (void)io_router_get(r, "/simple", api_handler);

    io_route_match_t m = io_router_dispatch(r, IO_METHOD_GET, "/simple", 7);
    TEST_ASSERT_EQUAL_INT(IO_MATCH_FOUND, m.status);
    /* opts may be nullptr when registered without _with() */
    /* No timeout override means server default */

    io_router_destroy(r);
}

void test_route_keepalive_timeout_override(void)
{
    io_router_t *r = io_router_create();

    static const io_route_opts_t long_ka_opts = {
        .keepalive_timeout_ms = 120000,
    };
    (void)io_router_get_with(r, "/stream", api_handler, &long_ka_opts);

    io_route_match_t m = io_router_dispatch(r, IO_METHOD_GET, "/stream", 7);
    TEST_ASSERT_EQUAL_INT(IO_MATCH_FOUND, m.status);
    TEST_ASSERT_NOT_NULL(m.opts);
    TEST_ASSERT_EQUAL_UINT32(120000, m.opts->keepalive_timeout_ms);

    io_router_destroy(r);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_route_opts_timeout_defaults_zero);
    RUN_TEST(test_route_timeout_override_in_match);
    RUN_TEST(test_route_timeout_zero_means_server_default);
    RUN_TEST(test_route_no_opts_returns_null);
    RUN_TEST(test_route_keepalive_timeout_override);
    return UNITY_END();
}
```

**Step 8: Add to CMakeLists.txt**

After the health check section:

```cmake
# Sprint 15: Per-route timeout tests
io_add_test(test_io_route_timeout tests/unit/test_io_route_timeout.c
    io_router io_radix io_ctx io_request io_response)
```

**Step 9: Run tests**

```bash
cmake --build --preset clang-debug && ctest --preset clang-debug
```
Expected: All tests PASS

**Step 10: Commit**

```bash
git add src/router/io_router.h src/core/io_conn.h src/core/io_server.c \
        tests/unit/test_io_route_timeout.c CMakeLists.txt
git commit -m "feat(router): add per-route timeout configuration

Routes can override body_timeout_ms and keepalive_timeout_ms via
io_route_opts_t. Zero means use server default. Applied after header
parse via quick route lookup before body recv arm. Enables longer
timeouts for upload routes while keeping tight defaults elsewhere."
```

---

## Task 3: Integration Test — Health Endpoints via TCP

**Priority:** P1 — verify health endpoints work end-to-end through the server pipeline.

**Files:**
- Create: `tests/integration/test_health_pipeline.c`
- Modify: `CMakeLists.txt` — add integration test

**Step 1: Write integration test**

Pattern: same as `tests/integration/test_pipeline.c` — create server, connect via TCP, send HTTP request, verify response.

```c
#include "core/io_health.h"
#include "core/io_server.h"
#include "router/io_router.h"

#include <netinet/in.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <unity.h>

static io_server_t *srv;
static io_router_t *router;

void setUp(void)
{
    io_server_config_t cfg;
    io_server_config_init(&cfg);
    cfg.listen_port = 0; /* auto-assign */
    srv = io_server_create(&cfg);
    TEST_ASSERT_NOT_NULL(srv);

    router = io_router_create();
    TEST_ASSERT_NOT_NULL(router);

    io_health_config_t health_cfg;
    io_health_config_init(&health_cfg);
    TEST_ASSERT_EQUAL_INT(0, io_health_register(router, srv, &health_cfg));
    TEST_ASSERT_EQUAL_INT(0, io_server_set_router(srv, router));
}

void tearDown(void)
{
    io_server_destroy(srv);
    io_router_destroy(router);
}

/* Helper: get port from listen fd */
static uint16_t get_bound_port(int fd)
{
    struct sockaddr_in addr;
    socklen_t len = sizeof(addr);
    getsockname(fd, (struct sockaddr *)&addr, &len);
    return ntohs(addr.sin_port);
}

/* Helper: connect to localhost:port */
static int connect_to(uint16_t port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(port),
        .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
    };
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

void test_health_endpoint_returns_200(void)
{
    int listen_fd = io_server_listen(srv);
    TEST_ASSERT_TRUE(listen_fd >= 0);
    uint16_t port = get_bound_port(listen_fd);

    int client = connect_to(port);
    TEST_ASSERT_TRUE(client >= 0);

    (void)io_server_run_once(srv, 200);

    const char *req = "GET /health HTTP/1.1\r\nHost: localhost\r\n\r\n";
    write(client, req, strlen(req));

    for (int i = 0; i < 10; i++)
        (void)io_server_run_once(srv, 100);

    char buf[1024];
    ssize_t n = read(client, buf, sizeof(buf) - 1);
    TEST_ASSERT_TRUE(n > 0);
    buf[n] = '\0';

    TEST_ASSERT_NOT_NULL(strstr(buf, "200"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"status\":\"ok\""));

    close(client);
}

void test_ready_endpoint_returns_200(void)
{
    int listen_fd = io_server_listen(srv);
    TEST_ASSERT_TRUE(listen_fd >= 0);
    uint16_t port = get_bound_port(listen_fd);

    int client = connect_to(port);
    TEST_ASSERT_TRUE(client >= 0);

    (void)io_server_run_once(srv, 200);

    const char *req = "GET /ready HTTP/1.1\r\nHost: localhost\r\n\r\n";
    write(client, req, strlen(req));

    for (int i = 0; i < 10; i++)
        (void)io_server_run_once(srv, 100);

    char buf[1024];
    ssize_t n = read(client, buf, sizeof(buf) - 1);
    TEST_ASSERT_TRUE(n > 0);
    buf[n] = '\0';

    TEST_ASSERT_NOT_NULL(strstr(buf, "200"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"status\":\"ready\""));

    close(client);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_health_endpoint_returns_200);
    RUN_TEST(test_ready_endpoint_returns_200);
    return UNITY_END();
}
```

**Step 2: Add to CMakeLists.txt**

Follow the pattern of `test_pipeline` — link against all server modules:

```cmake
    add_executable(test_health_pipeline tests/integration/test_health_pipeline.c)
    target_include_directories(test_health_pipeline PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(test_health_pipeline PRIVATE
        unity io_health io_server io_loop io_conn io_ctx
        io_http1 io_request io_response
        io_router io_radix io_middleware
        io_route_group io_route_inspect io_route_meta
        io_log
    )
    target_compile_options(test_health_pipeline PRIVATE
        -Wno-missing-prototypes -Wno-missing-declarations
    )
    add_test(NAME test_health_pipeline COMMAND test_health_pipeline)
```

If yyjson is available, also link it:
```cmake
    if(YYJSON_INCLUDE_DIR AND YYJSON_LIBRARY)
        target_include_directories(test_health_pipeline PRIVATE ${YYJSON_INCLUDE_DIR})
        target_link_libraries(test_health_pipeline PRIVATE ${YYJSON_LIBRARY})
    endif()
```

**Step 3: Run tests**

```bash
cmake --build --preset clang-debug && ctest --preset clang-debug -R test_health_pipeline
```
Expected: PASS

**Step 4: Commit**

```bash
git add tests/integration/test_health_pipeline.c CMakeLists.txt
git commit -m "test(integration): add health endpoint pipeline tests

Verifies /health and /ready endpoints return correct responses
through the full TCP → io_uring → server → router → handler pipeline."
```

---

## Task 4: Quality Verification

**Priority:** P0 — sprint must be ASan-green before merge.

**Step 1: Full ASan build + test**

```bash
cmake --preset clang-asan && cmake --build --preset clang-asan && ctest --preset clang-asan --output-on-failure
```
Expected: All tests PASS (N/N, zero ASan/UBSan errors)

**Step 2: Full debug build + test**

```bash
cmake --preset clang-debug && cmake --build --preset clang-debug && ctest --preset clang-debug
```
Expected: All tests PASS

**Step 3: Format check**

```bash
cmake --build --preset clang-debug --target format-check
```
Expected: No formatting violations. If violations exist, run `--target format` and commit.

**Step 4: Commit if any fixes needed**

Only if additional changes were required to get to green.

---

## Acceptance Criteria

- [ ] `io_health_config_init()` sets defaults: enabled=true, `/health`, `/ready`, `/live`
- [ ] `io_health_add_checker()` registers up to 8 custom deep-liveness checkers
- [ ] `io_health_register()` creates GET routes for all three endpoints
- [ ] `/health` returns `200 {"status":"ok"}`
- [ ] `/ready` returns `200 {"status":"ready"}` normally, `503 {"status":"unavailable"}` during drain
- [ ] `/live` returns `200` with aggregated checker results (or `503` if any checker fails)
- [ ] Custom paths work (e.g., `/healthz`, `/readyz`, `/livez`)
- [ ] `cfg.enabled = false` disables all health routes
- [ ] `io_route_opts_t` has `header_timeout_ms`, `body_timeout_ms`, `keepalive_timeout_ms` fields
- [ ] Zero timeout value means "use server default"
- [ ] Non-zero timeout value overrides server default for the matched route
- [ ] Route lookup for timeout happens after header parse, before body recv arm
- [ ] Per-route timeout overrides reset on keep-alive reuse
- [ ] Integration test: health endpoints respond correctly via TCP pipeline
- [ ] `ctest --preset clang-asan` — all tests PASS
- [ ] `ctest --preset clang-debug` — all tests PASS
- [ ] No regressions in existing 46+ tests

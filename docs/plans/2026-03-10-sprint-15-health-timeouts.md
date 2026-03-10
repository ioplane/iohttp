# Sprint 15: Health Check Endpoints & Per-Route Timeouts

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Implement the two remaining P0 features that block 0.1.0 release: health check endpoint framework (`/health`, `/ready`, `/live`) and per-route timeout configuration.

**Architecture:** Health checks are a standalone module (`ioh_health`) that registers routes on the server's router. JSON responses via yyjson. Per-route timeouts extend `ioh_route_opts_t` with override fields; `ioh_server.c` applies them after route match, before arming body recv.

**Tech Stack:** C23, io_uring (linked timeouts), yyjson (JSON responses), Unity (tests).

**Skills (MANDATORY):**
- `iohttp-architecture` — naming (`ioh_module_verb_noun()`), directory layout, ownership model
- `io-uring-patterns` — linked timeout SQE pattern, CQE error handling

**Build/test:**
```bash
# Inside container (podman exec -w <worktree-path> iohttp-dev bash -c "...")
cmake --preset clang-debug && cmake --build --preset clang-debug && ctest --preset clang-debug
cmake --preset clang-asan && cmake --build --preset clang-asan && ctest --preset clang-asan
```

**Existing files (read before implementing):**
- `src/core/ioh_server.h` — server config, lifecycle API
- `src/core/ioh_server.c` — `dispatch_request()`, `arm_recv()`, `timeout_ms_for_phase()`, CQE loop
- `src/core/ioh_ctx.h` — per-request context, `ioh_ctx_json()` response helper
- `src/core/ioh_conn.h` — connection state, `ioh_timeout_phase_t`
- `src/router/ioh_router.h` — `ioh_route_opts_t`, `ioh_route_match_t`, handler signature
- `CMakeLists.txt` — `ioh_add_test()` macro, library linkage pattern

---

## Task 1: Health Check Endpoint Framework

**Priority:** P0 — orchestrators (Kubernetes, systemd, HAProxy) need health probes.

**Files:**
- Create: `src/core/ioh_health.h`
- Create: `src/core/ioh_health.c`
- Create: `tests/unit/test_ioh_health.c`
- Modify: `CMakeLists.txt` — add `ioh_health` library and test

**Step 1: Read existing files**

Read `src/core/ioh_server.h`, `src/core/ioh_server.c` (focus on `struct ioh_server`, `dispatch_request`, shutdown flow), `src/core/ioh_ctx.h` (response helpers), `src/router/ioh_router.h` (route registration).

**Step 2: Create ioh_health.h**

```c
/**
 * @file ioh_health.h
 * @brief Health check endpoint framework (/health, /ready, /live).
 */

#ifndef IOHTTP_CORE_HEALTH_H
#define IOHTTP_CORE_HEALTH_H

#include <stdbool.h>
#include <stdint.h>

typedef struct ioh_ctx ioh_ctx_t;
typedef struct ioh_router ioh_router_t;
typedef struct ioh_server ioh_server_t;

/**
 * Health check callback.
 * @param message  Output: human-readable status (points into static/literal storage).
 * @param user_data  Opaque user context.
 * @return 0 = healthy, negative errno = unhealthy.
 */
typedef int (*ioh_health_check_fn)(const char **message, void *user_data);

constexpr uint32_t IOH_HEALTH_MAX_CHECKS = 8;

typedef struct {
    const char *name;
    ioh_health_check_fn check;
    void *user_data;
} ioh_health_checker_t;

typedef struct {
    bool enabled;
    const char *health_path;   /**< default "/health" */
    const char *ready_path;    /**< default "/ready" */
    const char *live_path;     /**< default "/live" */
    ioh_health_checker_t checkers[IOH_HEALTH_MAX_CHECKS];
    uint32_t checker_count;
} ioh_health_config_t;

/**
 * @brief Initialize health config with defaults.
 */
void ioh_health_config_init(ioh_health_config_t *cfg);

/**
 * @brief Add a custom deep-liveness checker.
 * @return 0 on success, -ENOSPC if max checkers reached.
 */
[[nodiscard]] int ioh_health_add_checker(ioh_health_config_t *cfg, const char *name,
                                        ioh_health_check_fn check, void *user_data);

/**
 * @brief Register health endpoints on a router.
 * @param r    Router to register routes on.
 * @param srv  Server pointer (used by /ready to check drain state).
 * @param cfg  Health configuration.
 * @return 0 on success, negative errno on error.
 */
[[nodiscard]] int ioh_health_register(ioh_router_t *r, ioh_server_t *srv,
                                     const ioh_health_config_t *cfg);

#endif /* IOHTTP_CORE_HEALTH_H */
```

**Step 3: Create ioh_health.c**

Implementation details:

1. **`ioh_health_config_init()`**: Set `enabled=true`, paths to `"/health"`, `"/ready"`, `"/live"`, `checker_count=0`.

2. **`ioh_health_add_checker()`**: Append to `cfg->checkers[]`, increment count, return `-ENOSPC` if `checker_count >= IOH_HEALTH_MAX_CHECKS`.

3. Three static handler functions (stored via closure pattern using `ioh_ctx_get()`):

   **`health_handler(ioh_ctx_t *c)`**:
   ```c
   return ioh_ctx_json(c, 200, "{\"status\":\"ok\"}");
   ```

   **`ready_handler(ioh_ctx_t *c)`**:
   - Get server pointer from context: `ioh_server_t *srv = c->server;`
   - If `srv == nullptr` or `ioh_server_is_draining(srv)`:
     ```c
     return ioh_ctx_json(c, 503, "{\"status\":\"unavailable\"}");
     ```
   - Otherwise:
     ```c
     return ioh_ctx_json(c, 200, "{\"status\":\"ready\"}");
     ```

   **`live_handler(ioh_ctx_t *c)`**:
   - Retrieve stored `ioh_health_config_t *` from context value `"_health_cfg"`
   - If no checkers: return `ioh_ctx_json(c, 200, "{\"status\":\"ok\"}")`
   - Otherwise: iterate `checkers[]`, call each, build JSON with yyjson:
     ```json
     {"status":"ok","checks":{"database":"ok","cache":"ok"}}
     ```
     or `{"status":"degraded","checks":{"database":"error: connection refused"}}` with 503
   - Use `yyjson_mut_doc` to build response, `yyjson_mut_write()` to serialize
   - Free yyjson doc after calling `ioh_ctx_json()`

4. **`ioh_health_register()`**:
   - If `!cfg->enabled`, return 0 (no routes registered)
   - Store a static copy of `cfg` (or store pointer — cfg must outlive router)
   - Call `ioh_router_get(r, cfg->health_path, health_handler)`
   - Call `ioh_router_get(r, cfg->ready_path, ready_handler)`
   - Call `ioh_router_get(r, cfg->live_path, live_handler)`
   - For live_handler to access checkers, use `ioh_router_get_with()` with opts containing user_data. **Alternative (simpler)**: use a file-static `const ioh_health_config_t *` pointer since health config is singleton per server.
   - Return 0 on success

5. **`ioh_server_is_draining()`** — add to `ioh_server.h` / `ioh_server.c`:
   ```c
   bool ioh_server_is_draining(const ioh_server_t *srv);
   ```
   Implementation: `return srv != nullptr && srv->stopped;`

**Step 4: Write tests (tests/unit/test_ioh_health.c)**

```c
#include "core/ioh_health.h"
#include "core/ioh_ctx.h"
#include "core/ioh_server.h"
#include "http/ioh_request.h"
#include "http/ioh_response.h"
#include "router/ioh_router.h"

#include <string.h>
#include <unity.h>

void setUp(void) {}
void tearDown(void) {}

/* ---- Config defaults ---- */

void test_health_config_defaults(void)
{
    ioh_health_config_t cfg;
    ioh_health_config_init(&cfg);
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
    ioh_health_config_t cfg;
    ioh_health_config_init(&cfg);
    int rc = ioh_health_add_checker(&cfg, "db", dummy_check, nullptr);
    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_EQUAL_UINT32(1, cfg.checker_count);
    TEST_ASSERT_EQUAL_STRING("db", cfg.checkers[0].name);
}

void test_health_add_checker_overflow(void)
{
    ioh_health_config_t cfg;
    ioh_health_config_init(&cfg);
    for (uint32_t i = 0; i < IOH_HEALTH_MAX_CHECKS; i++) {
        TEST_ASSERT_EQUAL_INT(0, ioh_health_add_checker(&cfg, "c", dummy_check, nullptr));
    }
    TEST_ASSERT_EQUAL_INT(-ENOSPC, ioh_health_add_checker(&cfg, "overflow", dummy_check, nullptr));
}

/* ---- Route registration ---- */

void test_health_register_creates_routes(void)
{
    ioh_router_t *r = ioh_router_create();
    TEST_ASSERT_NOT_NULL(r);

    ioh_health_config_t cfg;
    ioh_health_config_init(&cfg);

    int rc = ioh_health_register(r, nullptr, &cfg);
    TEST_ASSERT_EQUAL_INT(0, rc);

    /* Verify routes exist by dispatching */
    ioh_route_match_t m = ioh_router_dispatch(r, IOH_METHOD_GET, "/health", 7);
    TEST_ASSERT_EQUAL_INT(IOH_MATCH_FOUND, m.status);

    m = ioh_router_dispatch(r, IOH_METHOD_GET, "/ready", 6);
    TEST_ASSERT_EQUAL_INT(IOH_MATCH_FOUND, m.status);

    m = ioh_router_dispatch(r, IOH_METHOD_GET, "/live", 5);
    TEST_ASSERT_EQUAL_INT(IOH_MATCH_FOUND, m.status);

    ioh_router_destroy(r);
}

void test_health_disabled_no_routes(void)
{
    ioh_router_t *r = ioh_router_create();
    ioh_health_config_t cfg;
    ioh_health_config_init(&cfg);
    cfg.enabled = false;

    int rc = ioh_health_register(r, nullptr, &cfg);
    TEST_ASSERT_EQUAL_INT(0, rc);

    ioh_route_match_t m = ioh_router_dispatch(r, IOH_METHOD_GET, "/health", 7);
    TEST_ASSERT_EQUAL_INT(IOH_MATCH_NOT_FOUND, m.status);

    ioh_router_destroy(r);
}

/* ---- Health handler returns 200 ---- */

void test_health_handler_returns_200(void)
{
    ioh_router_t *r = ioh_router_create();
    ioh_health_config_t cfg;
    ioh_health_config_init(&cfg);
    (void)ioh_health_register(r, nullptr, &cfg);

    ioh_route_match_t m = ioh_router_dispatch(r, IOH_METHOD_GET, "/health", 7);
    TEST_ASSERT_EQUAL_INT(IOH_MATCH_FOUND, m.status);

    /* Call handler with test context */
    ioh_request_t req;
    memset(&req, 0, sizeof(req));
    ioh_response_t resp;
    (void)ioh_response_init(&resp);
    ioh_ctx_t ctx;
    (void)ioh_ctx_init(&ctx, &req, &resp, nullptr);

    int rc = m.handler(&ctx);
    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_EQUAL_UINT16(200, resp.status);

    ioh_ctx_destroy(&ctx);
    ioh_response_destroy(&resp);
    ioh_router_destroy(r);
}

/* ---- Custom paths ---- */

void test_health_custom_paths(void)
{
    ioh_router_t *r = ioh_router_create();
    ioh_health_config_t cfg;
    ioh_health_config_init(&cfg);
    cfg.health_path = "/healthz";
    cfg.ready_path = "/readyz";
    cfg.live_path = "/livez";

    (void)ioh_health_register(r, nullptr, &cfg);

    ioh_route_match_t m = ioh_router_dispatch(r, IOH_METHOD_GET, "/healthz", 8);
    TEST_ASSERT_EQUAL_INT(IOH_MATCH_FOUND, m.status);

    m = ioh_router_dispatch(r, IOH_METHOD_GET, "/health", 7);
    TEST_ASSERT_EQUAL_INT(IOH_MATCH_NOT_FOUND, m.status);

    ioh_router_destroy(r);
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

After the ioh_log section (~line 287), add:

```cmake
# ============================================================================
# Sprint 15: Health check endpoints
# ============================================================================

add_library(ioh_health STATIC src/core/ioh_health.c)
target_include_directories(ioh_health PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(ioh_health PUBLIC ioh_router ioh_ctx ioh_request ioh_response ioh_server)
if(YYJSON_INCLUDE_DIR AND YYJSON_LIBRARY)
    target_include_directories(ioh_health PRIVATE ${YYJSON_INCLUDE_DIR})
    target_link_libraries(ioh_health PRIVATE ${YYJSON_LIBRARY})
    target_compile_definitions(ioh_health PRIVATE IOHTTP_HAVE_YYJSON)
endif()

ioh_add_test(test_ioh_health tests/unit/test_ioh_health.c
    ioh_health ioh_router ioh_radix ioh_ctx ioh_request ioh_response)
```

**Note:** `ioh_health` depends on `ioh_server` for `ioh_server_is_draining()`. To avoid circular dependency (ioh_server already links ioh_router), consider: the health handlers access server via `ctx->server` (already available). `ioh_server_is_draining()` can be a standalone function in `ioh_server.h`/`ioh_server.c` that just checks the flag. The `ioh_health` library needs the declaration but not necessarily to link against `ioh_server` — the test will link both.

**Step 6: Run tests to verify they fail**

```bash
cmake --build --preset clang-debug && ctest --preset clang-debug -R test_ioh_health
```
Expected: FAIL (source files don't exist yet → build error)

**Step 7: Implement ioh_health.c**

Follow the implementation details from Step 3. Key points:
- Use file-static pointer for config: `static const ioh_health_config_t *s_health_cfg;`
- `ioh_health_register()` stores cfg pointer and server pointer in file-static variables
- Handlers access them directly (health endpoints are singleton per process)
- For `/live` JSON with checkers, use yyjson behind `#ifdef IOHTTP_HAVE_YYJSON` guard; fallback to simple string concat if yyjson unavailable

**Step 8: Run tests**

```bash
cmake --build --preset clang-debug && ctest --preset clang-debug -R test_ioh_health
```
Expected: PASS (7 tests)

**Step 9: Run full suite**

```bash
ctest --preset clang-debug
```
Expected: All tests PASS (no regressions)

**Step 10: Commit**

```bash
git add src/core/ioh_health.h src/core/ioh_health.c src/core/ioh_server.h src/core/ioh_server.c \
        tests/unit/test_ioh_health.c CMakeLists.txt
git commit -m "feat(core): add health check endpoint framework (/health, /ready, /live)

Three built-in endpoints: liveness (200 OK), readiness (503 during drain),
deep liveness (pluggable checkers with yyjson JSON aggregation).
Configurable paths, optional disable, up to 8 custom checker callbacks."
```

---

## Task 2: Per-Route Timeout Configuration

**Priority:** P0 — file upload routes need longer body timeouts than API routes.

**Files:**
- Modify: `src/router/ioh_router.h` — add timeout fields to `ioh_route_opts_t`
- Modify: `src/core/ioh_conn.h` — add per-request timeout overrides to `ioh_conn_t`
- Modify: `src/core/ioh_server.c` — apply route timeouts after header parse, before body recv
- Create: `tests/unit/test_ioh_route_timeout.c`
- Modify: `CMakeLists.txt` — add timeout test

**Step 1: Read existing timeout flow**

Read `src/core/ioh_server.c` focusing on:
- `timeout_ms_for_phase()` (line ~193) — returns server-level timeout by phase
- `arm_recv()` (line ~207) — creates linked timeout SQE using `timeout_ms_for_phase()`
- CQE RECV handler (line ~596) — where body recv is re-armed with `IOH_TIMEOUT_BODY`
- `dispatch_request()` (line ~351) — where route match happens

**Key insight:** The body recv timeout is set BEFORE the route is matched in the current code:
```c
// Line 752-756 in ioh_server.c:
if (req.content_length > 0 && body_avail < req.content_length) {
    conn->timeout_phase = IOH_TIMEOUT_BODY;
    (void)arm_recv(srv, conn);
}
```

But we DO have the parsed request (method + path) at this point, so we can do a quick route lookup to get timeout overrides before arming recv.

**Step 2: Add timeout fields to ioh_route_opts_t**

In `src/router/ioh_router.h`, extend `ioh_route_opts_t`:

```c
typedef struct {
    const ioh_route_meta_t *meta;
    void *oas_operation;
    uint32_t permissions;
    bool auth_required;
    uint32_t header_timeout_ms;    /**< 0 = use server default */
    uint32_t body_timeout_ms;      /**< 0 = use server default */
    uint32_t keepalive_timeout_ms; /**< 0 = use server default */
} ioh_route_opts_t;
```

**Step 3: Add per-request timeout overrides to ioh_conn_t**

In `src/core/ioh_conn.h`, add to `ioh_conn_t`:

```c
/* Per-request timeout overrides (0 = use server default) */
uint32_t route_body_timeout_ms;
uint32_t route_keepalive_timeout_ms;
```

**Step 4: Modify timeout_ms_for_phase() in ioh_server.c**

```c
static uint32_t timeout_ms_for_phase(const ioh_server_t *srv, const ioh_conn_t *conn,
                                     ioh_timeout_phase_t phase)
{
    switch (phase) {
    case IOH_TIMEOUT_HEADER:
        return srv->config.header_timeout_ms;
    case IOH_TIMEOUT_BODY:
        if (conn->route_body_timeout_ms > 0)
            return conn->route_body_timeout_ms;
        return srv->config.body_timeout_ms;
    case IOH_TIMEOUT_KEEPALIVE:
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

In `ioh_server_run_once()`, in the RECV handler, BEFORE the body wait arm_recv, do a quick route lookup:

```c
/* After parsing headers, before waiting for body: */
if (req.content_length > 0 && body_avail < req.content_length) {
    /* Quick route lookup for timeout overrides */
    ioh_route_match_t m = ioh_router_dispatch(srv->router, req.method,
                                            req.path, req.path_len);
    if (m.status == IOH_MATCH_FOUND && m.opts != nullptr) {
        conn->route_body_timeout_ms = m.opts->body_timeout_ms;
        conn->route_keepalive_timeout_ms = m.opts->keepalive_timeout_ms;
    }

    conn->timeout_phase = IOH_TIMEOUT_BODY;
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

**Step 7: Write tests (tests/unit/test_ioh_route_timeout.c)**

```c
#include "core/ioh_ctx.h"
#include "core/ioh_server.h"
#include "http/ioh_request.h"
#include "http/ioh_response.h"
#include "router/ioh_router.h"

#include <string.h>
#include <unity.h>

void setUp(void) {}
void tearDown(void) {}

static int upload_handler(ioh_ctx_t *c)
{
    return ioh_ctx_json(c, 200, "{\"ok\":true}");
}

static int api_handler(ioh_ctx_t *c)
{
    return ioh_ctx_json(c, 200, "{\"ok\":true}");
}

void test_route_opts_timeout_defaults_zero(void)
{
    ioh_route_opts_t opts = {0};
    TEST_ASSERT_EQUAL_UINT32(0, opts.header_timeout_ms);
    TEST_ASSERT_EQUAL_UINT32(0, opts.body_timeout_ms);
    TEST_ASSERT_EQUAL_UINT32(0, opts.keepalive_timeout_ms);
}

void test_route_timeout_override_in_match(void)
{
    ioh_router_t *r = ioh_router_create();
    TEST_ASSERT_NOT_NULL(r);

    static const ioh_route_opts_t upload_opts = {
        .body_timeout_ms = 300000,
    };
    int rc = ioh_router_post_with(r, "/upload", upload_handler, &upload_opts);
    TEST_ASSERT_EQUAL_INT(0, rc);

    ioh_route_match_t m = ioh_router_dispatch(r, IOH_METHOD_POST, "/upload", 7);
    TEST_ASSERT_EQUAL_INT(IOH_MATCH_FOUND, m.status);
    TEST_ASSERT_NOT_NULL(m.opts);
    TEST_ASSERT_EQUAL_UINT32(300000, m.opts->body_timeout_ms);

    ioh_router_destroy(r);
}

void test_route_timeout_zero_means_server_default(void)
{
    ioh_router_t *r = ioh_router_create();
    TEST_ASSERT_NOT_NULL(r);

    static const ioh_route_opts_t api_opts = {
        .body_timeout_ms = 0,
    };
    (void)ioh_router_get_with(r, "/api/data", api_handler, &api_opts);

    ioh_route_match_t m = ioh_router_dispatch(r, IOH_METHOD_GET, "/api/data", 9);
    TEST_ASSERT_EQUAL_INT(IOH_MATCH_FOUND, m.status);
    TEST_ASSERT_NOT_NULL(m.opts);
    TEST_ASSERT_EQUAL_UINT32(0, m.opts->body_timeout_ms);

    ioh_router_destroy(r);
}

void test_route_no_opts_returns_null(void)
{
    ioh_router_t *r = ioh_router_create();
    (void)ioh_router_get(r, "/simple", api_handler);

    ioh_route_match_t m = ioh_router_dispatch(r, IOH_METHOD_GET, "/simple", 7);
    TEST_ASSERT_EQUAL_INT(IOH_MATCH_FOUND, m.status);
    /* opts may be nullptr when registered without _with() */
    /* No timeout override means server default */

    ioh_router_destroy(r);
}

void test_route_keepalive_timeout_override(void)
{
    ioh_router_t *r = ioh_router_create();

    static const ioh_route_opts_t long_ka_opts = {
        .keepalive_timeout_ms = 120000,
    };
    (void)ioh_router_get_with(r, "/stream", api_handler, &long_ka_opts);

    ioh_route_match_t m = ioh_router_dispatch(r, IOH_METHOD_GET, "/stream", 7);
    TEST_ASSERT_EQUAL_INT(IOH_MATCH_FOUND, m.status);
    TEST_ASSERT_NOT_NULL(m.opts);
    TEST_ASSERT_EQUAL_UINT32(120000, m.opts->keepalive_timeout_ms);

    ioh_router_destroy(r);
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
ioh_add_test(test_ioh_route_timeout tests/unit/test_ioh_route_timeout.c
    ioh_router ioh_radix ioh_ctx ioh_request ioh_response)
```

**Step 9: Run tests**

```bash
cmake --build --preset clang-debug && ctest --preset clang-debug
```
Expected: All tests PASS

**Step 10: Commit**

```bash
git add src/router/ioh_router.h src/core/ioh_conn.h src/core/ioh_server.c \
        tests/unit/test_ioh_route_timeout.c CMakeLists.txt
git commit -m "feat(router): add per-route timeout configuration

Routes can override body_timeout_ms and keepalive_timeout_ms via
ioh_route_opts_t. Zero means use server default. Applied after header
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
#include "core/ioh_health.h"
#include "core/ioh_server.h"
#include "router/ioh_router.h"

#include <netinet/in.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <unity.h>

static ioh_server_t *srv;
static ioh_router_t *router;

void setUp(void)
{
    ioh_server_config_t cfg;
    ioh_server_config_init(&cfg);
    cfg.listen_port = 0; /* auto-assign */
    srv = ioh_server_create(&cfg);
    TEST_ASSERT_NOT_NULL(srv);

    router = ioh_router_create();
    TEST_ASSERT_NOT_NULL(router);

    ioh_health_config_t health_cfg;
    ioh_health_config_init(&health_cfg);
    TEST_ASSERT_EQUAL_INT(0, ioh_health_register(router, srv, &health_cfg));
    TEST_ASSERT_EQUAL_INT(0, ioh_server_set_router(srv, router));
}

void tearDown(void)
{
    ioh_server_destroy(srv);
    ioh_router_destroy(router);
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
    int listen_fd = ioh_server_listen(srv);
    TEST_ASSERT_TRUE(listen_fd >= 0);
    uint16_t port = get_bound_port(listen_fd);

    int client = connect_to(port);
    TEST_ASSERT_TRUE(client >= 0);

    (void)ioh_server_run_once(srv, 200);

    const char *req = "GET /health HTTP/1.1\r\nHost: localhost\r\n\r\n";
    write(client, req, strlen(req));

    for (int i = 0; i < 10; i++)
        (void)ioh_server_run_once(srv, 100);

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
    int listen_fd = ioh_server_listen(srv);
    TEST_ASSERT_TRUE(listen_fd >= 0);
    uint16_t port = get_bound_port(listen_fd);

    int client = connect_to(port);
    TEST_ASSERT_TRUE(client >= 0);

    (void)ioh_server_run_once(srv, 200);

    const char *req = "GET /ready HTTP/1.1\r\nHost: localhost\r\n\r\n";
    write(client, req, strlen(req));

    for (int i = 0; i < 10; i++)
        (void)ioh_server_run_once(srv, 100);

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
        unity ioh_health ioh_server ioh_loop ioh_conn ioh_ctx
        ioh_http1 ioh_request ioh_response
        ioh_router ioh_radix ioh_middleware
        ioh_route_group ioh_route_inspect ioh_route_meta
        ioh_log
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

- [ ] `ioh_health_config_init()` sets defaults: enabled=true, `/health`, `/ready`, `/live`
- [ ] `ioh_health_add_checker()` registers up to 8 custom deep-liveness checkers
- [ ] `ioh_health_register()` creates GET routes for all three endpoints
- [ ] `/health` returns `200 {"status":"ok"}`
- [ ] `/ready` returns `200 {"status":"ready"}` normally, `503 {"status":"unavailable"}` during drain
- [ ] `/live` returns `200` with aggregated checker results (or `503` if any checker fails)
- [ ] Custom paths work (e.g., `/healthz`, `/readyz`, `/livez`)
- [ ] `cfg.enabled = false` disables all health routes
- [ ] `ioh_route_opts_t` has `header_timeout_ms`, `body_timeout_ms`, `keepalive_timeout_ms` fields
- [ ] Zero timeout value means "use server default"
- [ ] Non-zero timeout value overrides server default for the matched route
- [ ] Route lookup for timeout happens after header parse, before body recv arm
- [ ] Per-route timeout overrides reset on keep-alive reuse
- [ ] Integration test: health endpoints respond correctly via TCP pipeline
- [ ] `ctest --preset clang-asan` — all tests PASS
- [ ] `ctest --preset clang-debug` — all tests PASS
- [ ] No regressions in existing 46+ tests

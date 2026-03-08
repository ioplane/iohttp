# S9: Route Metadata + Introspection API — Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development to implement this plan task-by-task.

**Goal:** Add typed route metadata (`io_route_meta_t`) to iohttp's router so routes carry summary, tags, params descriptions, and deprecated flags — enabling introspection and future liboas integration.

**Architecture:** New `io_route_meta_t` struct stored as pointer in `io_route_opts_t.meta`. Radix nodes gain a separate `meta` pointer (alongside existing `metadata`/opts pointer) so `set_meta()` can update meta without replacing opts. Walk API returns typed `opts` + `meta` instead of `void *metadata`.

**Tech Stack:** C23, Unity tests, CMake. ALL builds in podman: `podman run --rm --security-opt seccomp=unconfined -v /opt/projects/repositories/iohttp:/workspace:Z localhost/ringwall-dev:latest bash -c "cd /workspace && <commands>"`

**Design doc:** `docs/plans/2026-03-08-s9-route-metadata-design.md`

**Build/test commands:**
```bash
podman run --rm --security-opt seccomp=unconfined \
  -v /opt/projects/repositories/iohttp:/workspace:Z \
  localhost/ringwall-dev:latest bash -c \
  "cd /workspace && cmake --preset clang-debug && cmake --build --preset clang-debug && ctest --preset clang-debug --output-on-failure"
```

**NEVER mention "Claude" or any AI in commits, code, or comments.**

---

## Task 1: Create `io_route_meta.h` and `io_route_meta.c`

**Files:**
- Create: `src/router/io_route_meta.h`
- Create: `src/router/io_route_meta.c`
- Modify: `CMakeLists.txt` (add library)

**Step 1: Create `src/router/io_route_meta.h`**

```c
/**
 * @file io_route_meta.h
 * @brief Route metadata types for introspection and documentation.
 *
 * Provides structured per-route metadata (summary, tags, params, deprecated)
 * independent of liboas. Stored via io_route_opts_t.meta pointer.
 */

#ifndef IOHTTP_ROUTER_ROUTE_META_H
#define IOHTTP_ROUTER_ROUTE_META_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Parameter location in HTTP request */
typedef enum : uint8_t {
    IO_PARAM_PATH,   /* /users/:id */
    IO_PARAM_QUERY,  /* ?page=1 */
    IO_PARAM_HEADER, /* X-Request-Id */
    IO_PARAM_COOKIE, /* session_id */
} io_param_in_t;

/* Per-parameter metadata (name, location, description) */
typedef struct {
    const char *name;        /* parameter name, e.g. "id" */
    const char *description; /* human-readable, e.g. "User UUID" */
    io_param_in_t in;        /* where in the request */
    bool required;           /* always true for path params */
} io_param_meta_t;

/* Per-route metadata for introspection and documentation */
typedef struct {
    const char *summary;            /* short description, e.g. "Get user by ID" */
    const char *description;        /* longer text (nullable) */
    const char *const *tags;        /* nullptr-terminated array, e.g. {"users", nullptr} */
    bool deprecated;                /* route is deprecated */
    const io_param_meta_t *params;  /* array of param descriptors */
    uint32_t param_count;           /* number of params */
} io_route_meta_t;

/**
 * @brief Get string name for a parameter location.
 * @param in Parameter location enum value.
 * @return Static string: "path", "query", "header", "cookie", or "unknown".
 */
const char *io_param_in_name(io_param_in_t in);

#endif /* IOHTTP_ROUTER_ROUTE_META_H */
```

**Step 2: Create `src/router/io_route_meta.c`**

```c
/**
 * @file io_route_meta.c
 * @brief Route metadata utility functions.
 */

#include "router/io_route_meta.h"

const char *io_param_in_name(io_param_in_t in)
{
    switch (in) {
    case IO_PARAM_PATH:
        return "path";
    case IO_PARAM_QUERY:
        return "query";
    case IO_PARAM_HEADER:
        return "header";
    case IO_PARAM_COOKIE:
        return "cookie";
    default:
        return "unknown";
    }
}
```

**Step 3: Add to CMakeLists.txt**

After the `io_route_inspect` section (around line 415), add:

```cmake
# ============================================================================
# Sprint 9: Route metadata
# ============================================================================

add_library(io_route_meta STATIC src/router/io_route_meta.c)
target_include_directories(io_route_meta PUBLIC ${CMAKE_SOURCE_DIR}/src)
```

**Step 4: Build to verify compilation**

```bash
podman run --rm --security-opt seccomp=unconfined \
  -v /opt/projects/repositories/iohttp:/workspace:Z \
  localhost/ringwall-dev:latest bash -c \
  "cd /workspace && cmake --preset clang-debug && cmake --build --preset clang-debug"
```

Expected: clean build, no errors.

**Step 5: Commit**

```bash
git add src/router/io_route_meta.h src/router/io_route_meta.c CMakeLists.txt
git commit -m "feat(router): add io_route_meta_t types for route metadata"
```

---

## Task 2: Write failing tests for `io_route_meta_t`

**Files:**
- Create: `tests/unit/test_io_route_meta.c`
- Modify: `CMakeLists.txt` (add test target)

**Step 1: Create `tests/unit/test_io_route_meta.c`**

```c
/**
 * @file test_io_route_meta.c
 * @brief Unit tests for route metadata types and introspection integration.
 */

#include "router/io_route_inspect.h"
#include "router/io_route_meta.h"
#include "router/io_router.h"

#include <errno.h>
#include <string.h>

#include <unity.h>

static io_router_t *router;

void setUp(void)
{
    router = io_router_create();
    TEST_ASSERT_NOT_NULL(router);
}

void tearDown(void)
{
    io_router_destroy(router);
    router = nullptr;
}

/* ---- Stub handlers ---- */

static int handler_a(io_ctx_t *c)
{
    (void)c;
    return 0;
}

static int handler_b(io_ctx_t *c)
{
    (void)c;
    return 0;
}

/* ---- Walk helper ---- */

typedef struct {
    io_route_info_t routes[16];
    char patterns[16][256];
    uint32_t count;
} walk_ctx_t;

static int collect_routes(const io_route_info_t *info, void *ctx)
{
    walk_ctx_t *w = (walk_ctx_t *)ctx;
    if (w->count >= 16) {
        return -1;
    }
    w->routes[w->count] = *info;
    size_t len = strlen(info->pattern);
    if (len >= 256)
        len = 255;
    memcpy(w->patterns[w->count], info->pattern, len);
    w->patterns[w->count][len] = '\0';
    w->routes[w->count].pattern = w->patterns[w->count];
    w->count++;
    return 0;
}

/* ---- 1. io_param_in_name tests ---- */

void test_param_in_name_path(void)
{
    TEST_ASSERT_EQUAL_STRING("path", io_param_in_name(IO_PARAM_PATH));
}

void test_param_in_name_query(void)
{
    TEST_ASSERT_EQUAL_STRING("query", io_param_in_name(IO_PARAM_QUERY));
}

void test_param_in_name_header(void)
{
    TEST_ASSERT_EQUAL_STRING("header", io_param_in_name(IO_PARAM_HEADER));
}

void test_param_in_name_cookie(void)
{
    TEST_ASSERT_EQUAL_STRING("cookie", io_param_in_name(IO_PARAM_COOKIE));
}

/* ---- 2. Meta via opts registration ---- */

void test_meta_summary_via_opts(void)
{
    static const io_route_meta_t meta = {.summary = "List all users"};
    io_route_opts_t opts = {.meta = &meta};

    TEST_ASSERT_EQUAL_INT(0, io_router_get_with(router, "/users", handler_a, &opts));

    walk_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    TEST_ASSERT_EQUAL_INT(0, io_router_walk(router, collect_routes, &ctx));
    TEST_ASSERT_EQUAL_UINT32(1, ctx.count);
    TEST_ASSERT_NOT_NULL(ctx.routes[0].meta);
    TEST_ASSERT_EQUAL_STRING("List all users", ctx.routes[0].meta->summary);
}

void test_meta_tags_nullptr_terminated(void)
{
    static const char *tags[] = {"users", "admin", nullptr};
    static const io_route_meta_t meta = {.summary = "Get user", .tags = tags};
    io_route_opts_t opts = {.meta = &meta};

    TEST_ASSERT_EQUAL_INT(0, io_router_get_with(router, "/users/:id", handler_a, &opts));

    walk_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    TEST_ASSERT_EQUAL_INT(0, io_router_walk(router, collect_routes, &ctx));
    TEST_ASSERT_NOT_NULL(ctx.routes[0].meta);
    TEST_ASSERT_NOT_NULL(ctx.routes[0].meta->tags);
    TEST_ASSERT_EQUAL_STRING("users", ctx.routes[0].meta->tags[0]);
    TEST_ASSERT_EQUAL_STRING("admin", ctx.routes[0].meta->tags[1]);
    TEST_ASSERT_NULL(ctx.routes[0].meta->tags[2]);
}

void test_meta_deprecated_flag(void)
{
    static const io_route_meta_t meta = {.summary = "Old endpoint", .deprecated = true};
    io_route_opts_t opts = {.meta = &meta};

    TEST_ASSERT_EQUAL_INT(0, io_router_get_with(router, "/v1/users", handler_a, &opts));

    walk_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    TEST_ASSERT_EQUAL_INT(0, io_router_walk(router, collect_routes, &ctx));
    TEST_ASSERT_NOT_NULL(ctx.routes[0].meta);
    TEST_ASSERT_TRUE(ctx.routes[0].meta->deprecated);
}

void test_meta_params_path(void)
{
    static const io_param_meta_t params[] = {
        {.name = "id", .in = IO_PARAM_PATH, .required = true, .description = "User UUID"},
    };
    static const io_route_meta_t meta = {
        .summary = "Get user",
        .params = params,
        .param_count = 1,
    };
    io_route_opts_t opts = {.meta = &meta};

    TEST_ASSERT_EQUAL_INT(0, io_router_get_with(router, "/users/:id", handler_a, &opts));

    walk_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    TEST_ASSERT_EQUAL_INT(0, io_router_walk(router, collect_routes, &ctx));
    TEST_ASSERT_NOT_NULL(ctx.routes[0].meta);
    TEST_ASSERT_EQUAL_UINT32(1, ctx.routes[0].meta->param_count);
    TEST_ASSERT_EQUAL_STRING("id", ctx.routes[0].meta->params[0].name);
    TEST_ASSERT_EQUAL_INT(IO_PARAM_PATH, ctx.routes[0].meta->params[0].in);
    TEST_ASSERT_TRUE(ctx.routes[0].meta->params[0].required);
    TEST_ASSERT_EQUAL_STRING("User UUID", ctx.routes[0].meta->params[0].description);
}

void test_meta_params_multiple(void)
{
    static const io_param_meta_t params[] = {
        {.name = "id", .in = IO_PARAM_PATH, .required = true},
        {.name = "fields", .in = IO_PARAM_QUERY, .required = false, .description = "Comma-separated field names"},
    };
    static const io_route_meta_t meta = {
        .summary = "Get user with field selection",
        .params = params,
        .param_count = 2,
    };
    io_route_opts_t opts = {.meta = &meta};

    TEST_ASSERT_EQUAL_INT(0, io_router_get_with(router, "/users/:id", handler_a, &opts));

    walk_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    TEST_ASSERT_EQUAL_INT(0, io_router_walk(router, collect_routes, &ctx));
    TEST_ASSERT_NOT_NULL(ctx.routes[0].meta);
    TEST_ASSERT_EQUAL_UINT32(2, ctx.routes[0].meta->param_count);
    TEST_ASSERT_EQUAL_STRING("id", ctx.routes[0].meta->params[0].name);
    TEST_ASSERT_EQUAL_STRING("fields", ctx.routes[0].meta->params[1].name);
    TEST_ASSERT_EQUAL_INT(IO_PARAM_QUERY, ctx.routes[0].meta->params[1].in);
    TEST_ASSERT_FALSE(ctx.routes[0].meta->params[1].required);
}

void test_meta_null_when_no_opts(void)
{
    TEST_ASSERT_EQUAL_INT(0, io_router_get(router, "/health", handler_a));

    walk_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    TEST_ASSERT_EQUAL_INT(0, io_router_walk(router, collect_routes, &ctx));
    TEST_ASSERT_EQUAL_UINT32(1, ctx.count);
    TEST_ASSERT_NULL(ctx.routes[0].meta);
}

void test_meta_set_meta_after_registration(void)
{
    /* Register without meta */
    io_route_opts_t opts = {.auth_required = true, .permissions = 0xFF};
    TEST_ASSERT_EQUAL_INT(0, io_router_get_with(router, "/users", handler_a, &opts));

    /* Initially no meta */
    walk_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    TEST_ASSERT_EQUAL_INT(0, io_router_walk(router, collect_routes, &ctx));
    TEST_ASSERT_NULL(ctx.routes[0].meta);

    /* Set meta after registration */
    static const io_route_meta_t meta = {.summary = "List users"};
    TEST_ASSERT_EQUAL_INT(0, io_router_set_meta(router, IO_METHOD_GET, "/users", &meta));

    /* Now meta is visible */
    memset(&ctx, 0, sizeof(ctx));
    TEST_ASSERT_EQUAL_INT(0, io_router_walk(router, collect_routes, &ctx));
    TEST_ASSERT_NOT_NULL(ctx.routes[0].meta);
    TEST_ASSERT_EQUAL_STRING("List users", ctx.routes[0].meta->summary);

    /* opts still accessible via dispatch */
    io_route_match_t m = io_router_dispatch(router, IO_METHOD_GET, "/users", strlen("/users"));
    TEST_ASSERT_EQUAL_INT(IO_MATCH_FOUND, m.status);
    TEST_ASSERT_NOT_NULL(m.opts);
    TEST_ASSERT_TRUE(m.opts->auth_required);
    TEST_ASSERT_EQUAL_UINT32(0xFF, m.opts->permissions);
}

void test_meta_opts_and_meta_combined(void)
{
    static const io_route_meta_t meta = {.summary = "Delete user"};
    io_route_opts_t opts = {.meta = &meta, .auth_required = true, .permissions = 0x01};

    TEST_ASSERT_EQUAL_INT(0, io_router_delete_with(router, "/users/:id", handler_a, &opts));

    /* Walk sees meta */
    walk_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    TEST_ASSERT_EQUAL_INT(0, io_router_walk(router, collect_routes, &ctx));
    TEST_ASSERT_NOT_NULL(ctx.routes[0].meta);
    TEST_ASSERT_EQUAL_STRING("Delete user", ctx.routes[0].meta->summary);

    /* Dispatch sees opts */
    io_route_match_t m = io_router_dispatch(router, IO_METHOD_DELETE, "/users/42", strlen("/users/42"));
    TEST_ASSERT_EQUAL_INT(IO_MATCH_FOUND, m.status);
    TEST_ASSERT_NOT_NULL(m.opts);
    TEST_ASSERT_TRUE(m.opts->auth_required);
    TEST_ASSERT_EQUAL_UINT32(0x01, m.opts->permissions);
    /* opts->meta also accessible */
    TEST_ASSERT_NOT_NULL(m.opts->meta);
    TEST_ASSERT_EQUAL_STRING("Delete user", m.opts->meta->summary);
}

/* ---- Main ---- */

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_param_in_name_path);
    RUN_TEST(test_param_in_name_query);
    RUN_TEST(test_param_in_name_header);
    RUN_TEST(test_param_in_name_cookie);
    RUN_TEST(test_meta_summary_via_opts);
    RUN_TEST(test_meta_tags_nullptr_terminated);
    RUN_TEST(test_meta_deprecated_flag);
    RUN_TEST(test_meta_params_path);
    RUN_TEST(test_meta_params_multiple);
    RUN_TEST(test_meta_null_when_no_opts);
    RUN_TEST(test_meta_set_meta_after_registration);
    RUN_TEST(test_meta_opts_and_meta_combined);
    return UNITY_END();
}
```

**Step 2: Add test target to CMakeLists.txt**

After the `io_route_meta` library definition, add:

```cmake
io_add_test(test_io_route_meta tests/unit/test_io_route_meta.c io_route_meta io_route_inspect io_router io_radix io_request io_response io_ctx)
```

**Step 3: Build to verify tests fail**

```bash
podman run --rm --security-opt seccomp=unconfined \
  -v /opt/projects/repositories/iohttp:/workspace:Z \
  localhost/ringwall-dev:latest bash -c \
  "cd /workspace && cmake --preset clang-debug && cmake --build --preset clang-debug 2>&1"
```

Expected: compilation errors — `io_route_info_t` has no `meta` field, `io_route_opts_t` has no `meta` field, `io_router_set_meta` undefined, `io_router_delete_with` undefined.

**Step 4: Commit (failing tests)**

```bash
git add tests/unit/test_io_route_meta.c CMakeLists.txt
git commit -m "test(router): add failing tests for route metadata (12 tests)"
```

---

## Task 3: Modify `io_route_opts_t` and `io_radix_node_t` to support meta

**Files:**
- Modify: `src/router/io_router.h` (add `meta` field to `io_route_opts_t`, add include, add `_with` variants for remaining methods)
- Modify: `src/router/io_radix.h` (add `meta` field to `io_radix_node_t`)

**Step 1: Modify `src/router/io_router.h`**

Add include at the top (after existing includes):
```c
#include "router/io_route_meta.h"
```

Change `io_route_opts_t`:
```c
/* Route options -- extensible metadata per-route */
typedef struct {
    const io_route_meta_t *meta; /* route metadata for introspection */
    void *oas_operation;         /* for liboas binding */
    uint32_t permissions;        /* bitmask for auth */
    bool auth_required;
} io_route_opts_t;
```

Add missing `_with` method variants (currently only `io_router_get_with` exists):
```c
[[nodiscard]] int io_router_post_with(io_router_t *r, const char *pattern, io_handler_fn h,
                                      const io_route_opts_t *opts);
[[nodiscard]] int io_router_put_with(io_router_t *r, const char *pattern, io_handler_fn h,
                                     const io_route_opts_t *opts);
[[nodiscard]] int io_router_delete_with(io_router_t *r, const char *pattern, io_handler_fn h,
                                        const io_route_opts_t *opts);
[[nodiscard]] int io_router_patch_with(io_router_t *r, const char *pattern, io_handler_fn h,
                                       const io_route_opts_t *opts);
```

**Step 2: Modify `src/router/io_radix.h`**

Add forward declaration at top (before `io_radix_node_t`):
```c
#include "router/io_route_meta.h"
```

Add `meta` field to `io_radix_node_t`:
```c
typedef struct io_radix_node {
    char *prefix;
    io_node_type_t type;
    char *param_name;
    void *handler;
    void *metadata;                  /* route opts pointer */
    const io_route_meta_t *meta;     /* route metadata (separate from opts) */
    struct io_radix_node **children;
    uint32_t child_count;
    uint32_t child_capacity;
    uint32_t priority;
} io_radix_node_t;
```

**Step 3: Build to verify header changes compile**

```bash
podman run --rm --security-opt seccomp=unconfined \
  -v /opt/projects/repositories/iohttp:/workspace:Z \
  localhost/ringwall-dev:latest bash -c \
  "cd /workspace && cmake --preset clang-debug && cmake --build --preset clang-debug 2>&1"
```

Expected: may still fail on missing `io_router_delete_with` implementation and `io_route_info_t` changes. That's expected — those are in Tasks 4 and 5.

**Step 4: Commit**

```bash
git add src/router/io_router.h src/router/io_radix.h
git commit -m "refactor(router): add meta field to io_route_opts_t and io_radix_node_t"
```

---

## Task 4: Implement meta propagation in router and radix

**Files:**
- Modify: `src/router/io_router.c` (extract meta from opts, implement `_with` methods)
- Modify: `src/router/io_radix.c` (propagate meta on insert)

**Step 1: Modify `src/router/io_router.c`**

In `router_add_route()`, extract meta from opts and store on node. Currently the function passes opts as metadata to `io_radix_insert`. After insert, we need to set meta on the node. But `io_radix_insert` is recursive and we don't get the node back.

**Better approach**: modify the lines in `io_radix.c` where `node->metadata = metadata` to also set `node->meta`. Add a new parameter to `io_radix_insert`:

Actually, simplest approach — after `io_radix_insert` succeeds, call a new internal function `io_radix_set_meta` to find the node and set meta. BUT this duplicates the lookup.

**Cleanest approach**: pass meta as separate parameter through `io_radix_insert`. Add `const io_route_meta_t *meta` parameter.

Modify `src/router/io_radix.h` — change `io_radix_insert` signature:
```c
[[nodiscard]] int io_radix_insert(io_radix_tree_t *tree, const char *pattern, void *handler,
                                  void *metadata, const io_route_meta_t *meta);
```

Modify `src/router/io_radix.c`:

In `insert_recursive`, add `const io_route_meta_t *meta` parameter. At each point where `node->metadata = metadata` is set, also set `node->meta = meta`:

```c
static int insert_recursive(io_radix_node_t *node, const char *pattern, size_t pos,
                            size_t pattern_len, void *handler, void *metadata,
                            const io_route_meta_t *meta)
```

At handler assignment points (3 places in `insert_recursive` + `insert_param_or_wildcard`):
```c
node->handler = handler;
node->metadata = metadata;
node->meta = meta;
```

Update all recursive calls to pass `meta` through.

In `io_radix_insert`:
```c
int io_radix_insert(io_radix_tree_t *tree, const char *pattern, void *handler,
                    void *metadata, const io_route_meta_t *meta)
{
    // ... validation ...
    return insert_recursive(tree->root, pattern, 0, pattern_len, handler, metadata, meta);
}
```

Modify `src/router/io_router.c`:

In `router_add_route`:
```c
static int router_add_route(io_router_t *r, io_method_t method, const char *pattern,
                            io_handler_fn h, const io_route_opts_t *opts)
{
    // ... existing validation and tree creation ...

    const io_route_meta_t *meta = (opts != nullptr) ? opts->meta : nullptr;
    return io_radix_insert(r->trees[method], pattern, (void *)(uintptr_t)h,
                           (void *)(uintptr_t)opts, meta);
}
```

Add missing `_with` methods:
```c
int io_router_post_with(io_router_t *r, const char *pattern, io_handler_fn h,
                        const io_route_opts_t *opts)
{
    return router_add_route(r, IO_METHOD_POST, pattern, h, opts);
}

int io_router_put_with(io_router_t *r, const char *pattern, io_handler_fn h,
                       const io_route_opts_t *opts)
{
    return router_add_route(r, IO_METHOD_PUT, pattern, h, opts);
}

int io_router_delete_with(io_router_t *r, const char *pattern, io_handler_fn h,
                          const io_route_opts_t *opts)
{
    return router_add_route(r, IO_METHOD_DELETE, pattern, h, opts);
}

int io_router_patch_with(io_router_t *r, const char *pattern, io_handler_fn h,
                         const io_route_opts_t *opts)
{
    return router_add_route(r, IO_METHOD_PATCH, pattern, h, opts);
}
```

Also update `src/router/io_route_group.c` — it calls `io_radix_insert` via `io_router_handle_with` which goes through `router_add_route`, so no change needed there.

**Step 2: Fix all callers of `io_radix_insert`**

Search for direct calls to `io_radix_insert` outside `io_router.c`. Check `io_route_group.c` — it uses `io_router_handle_with`, not direct `io_radix_insert`. Good.

Check tests — `test_io_radix.c` calls `io_radix_insert` directly. Update those calls to add `nullptr` as the last parameter.

**Step 3: Build and verify**

```bash
podman run --rm --security-opt seccomp=unconfined \
  -v /opt/projects/repositories/iohttp:/workspace:Z \
  localhost/ringwall-dev:latest bash -c \
  "cd /workspace && cmake --preset clang-debug && cmake --build --preset clang-debug 2>&1"
```

Expected: may still fail on `io_route_info_t` changes (Task 5). Router/radix code should compile.

**Step 4: Commit**

```bash
git add src/router/io_radix.c src/router/io_radix.h src/router/io_router.c src/router/io_router.h tests/unit/test_io_radix.c
git commit -m "feat(router): propagate meta through radix insert, add _with methods"
```

---

## Task 5: Update `io_route_inspect` — typed walk + `set_meta()`

**Files:**
- Modify: `src/router/io_route_inspect.h` (change `io_route_info_t`, rename `set_metadata`)
- Modify: `src/router/io_route_inspect.c` (fill opts/meta, implement `set_meta`)

**Step 1: Modify `src/router/io_route_inspect.h`**

Add include:
```c
#include "router/io_route_meta.h"
```

Replace `io_route_info_t`:
```c
/* Route info returned during walk */
typedef struct {
    io_method_t method;
    const char *pattern;            /* reconstructed pattern string from trie */
    io_handler_fn handler;
    const io_route_opts_t *opts;    /* full route options (may be nullptr) */
    const io_route_meta_t *meta;    /* route metadata (may be nullptr) */
} io_route_info_t;
```

Replace `io_router_set_metadata` declaration:
```c
/**
 * @brief Attach metadata to an existing route (without replacing opts).
 * @param r       Router.
 * @param method  HTTP method of the route.
 * @param pattern Original route pattern (e.g. "/users/:id").
 * @param meta    Metadata pointer to attach. Caller ensures lifetime.
 * @return 0 on success, -EINVAL on bad args, -ENOENT if route not found.
 */
[[nodiscard]] int io_router_set_meta(io_router_t *r, io_method_t method, const char *pattern,
                                     const io_route_meta_t *meta);
```

**Step 2: Modify `src/router/io_route_inspect.c`**

In `walk_node`, change the `io_route_info_t` construction:
```c
io_route_info_t info = {
    .method = state->method,
    .pattern = pattern_copy,
    .handler = (io_handler_fn)(uintptr_t)node->handler,
    .opts = (const io_route_opts_t *)(uintptr_t)node->metadata,
    .meta = node->meta,
};
```

Rename `find_and_set_metadata` to `find_and_set_meta`. Change the function to set `node->meta` instead of `node->metadata`:

```c
static int find_and_set_meta(io_radix_node_t *node, const char *pattern, size_t pos,
                             size_t pattern_len, const io_route_meta_t *meta)
```

At each place where `node->metadata = metadata` was set, change to `node->meta = meta`.

Rename the public function:
```c
int io_router_set_meta(io_router_t *r, io_method_t method, const char *pattern,
                       const io_route_meta_t *meta)
{
    if (!r || !pattern) {
        return -EINVAL;
    }

    io_radix_tree_t *tree = io_router_get_tree(r, method);
    if (!tree || !tree->root) {
        return -ENOENT;
    }

    size_t pattern_len = strlen(pattern);
    return find_and_set_meta(tree->root, pattern, 0, pattern_len, meta);
}
```

**Step 3: Update `tests/unit/test_io_route_inspect.c`**

In `collect_routes`, the callback now receives `opts` and `meta` instead of `metadata`. Update all assertions:

- `ctx.routes[i].metadata` → check `ctx.routes[i].opts` or `ctx.routes[i].meta` as appropriate
- `test_route_set_metadata` → rename to `test_route_set_meta`, use `io_router_set_meta`, check `ctx.routes[i].meta`
- `test_route_metadata_in_match` → update to use new field names

Key changes in `test_route_set_metadata` → `test_route_set_meta`:
```c
void test_route_set_meta(void)
{
    TEST_ASSERT_EQUAL_INT(0, io_router_get(router, "/users", handler_a));
    TEST_ASSERT_EQUAL_INT(0, io_router_get(router, "/users/:id", handler_b));

    /* Initially no meta */
    walk_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    int rc = io_router_walk(router, collect_routes, &ctx);
    TEST_ASSERT_EQUAL_INT(0, rc);
    for (uint32_t i = 0; i < ctx.count; i++) {
        TEST_ASSERT_NULL(ctx.routes[i].meta);
    }

    /* Attach meta to /users */
    static const io_route_meta_t meta1 = {.summary = "List users"};
    rc = io_router_set_meta(router, IO_METHOD_GET, "/users", &meta1);
    TEST_ASSERT_EQUAL_INT(0, rc);

    /* Attach meta to /users/:id */
    static const io_route_meta_t meta2 = {.summary = "Get user"};
    rc = io_router_set_meta(router, IO_METHOD_GET, "/users/:id", &meta2);
    TEST_ASSERT_EQUAL_INT(0, rc);

    /* Walk and verify */
    memset(&ctx, 0, sizeof(ctx));
    rc = io_router_walk(router, collect_routes, &ctx);
    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_EQUAL_UINT32(2, ctx.count);

    for (uint32_t i = 0; i < ctx.count; i++) {
        TEST_ASSERT_NOT_NULL(ctx.routes[i].meta);
        if (ctx.routes[i].handler == handler_a) {
            TEST_ASSERT_EQUAL_STRING("List users", ctx.routes[i].meta->summary);
        } else if (ctx.routes[i].handler == handler_b) {
            TEST_ASSERT_EQUAL_STRING("Get user", ctx.routes[i].meta->summary);
        }
    }

    /* Non-existent route returns -ENOENT */
    static const io_route_meta_t meta3 = {.summary = "nope"};
    rc = io_router_set_meta(router, IO_METHOD_GET, "/nope", &meta3);
    TEST_ASSERT_EQUAL_INT(-ENOENT, rc);

    /* Wrong method returns -ENOENT */
    rc = io_router_set_meta(router, IO_METHOD_POST, "/users", &meta3);
    TEST_ASSERT_EQUAL_INT(-ENOENT, rc);
}
```

Update `test_route_metadata_in_match`:
```c
void test_route_metadata_in_match(void)
{
    static const io_route_meta_t meta = {.summary = "Secure route"};
    io_route_opts_t opts = {
        .meta = &meta,
        .auth_required = true,
        .permissions = 0xFF,
    };

    TEST_ASSERT_EQUAL_INT(0, io_router_get_with(router, "/secure", handler_a, &opts));

    /* Dispatch: opts accessible */
    io_route_match_t m = io_router_dispatch(router, IO_METHOD_GET, "/secure", strlen("/secure"));
    TEST_ASSERT_EQUAL_INT(IO_MATCH_FOUND, m.status);
    TEST_ASSERT_NOT_NULL(m.opts);
    TEST_ASSERT_TRUE(m.opts->auth_required);
    TEST_ASSERT_EQUAL_UINT32(0xFF, m.opts->permissions);
    TEST_ASSERT_NOT_NULL(m.opts->meta);
    TEST_ASSERT_EQUAL_STRING("Secure route", m.opts->meta->summary);

    /* Walk: meta accessible */
    walk_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    int rc = io_router_walk(router, collect_routes, &ctx);
    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_EQUAL_UINT32(1, ctx.count);
    TEST_ASSERT_NOT_NULL(ctx.routes[0].meta);
    TEST_ASSERT_EQUAL_STRING("Secure route", ctx.routes[0].meta->summary);
}
```

Update `main()` to use renamed test.

**Step 4: Build and run ALL tests**

```bash
podman run --rm --security-opt seccomp=unconfined \
  -v /opt/projects/repositories/iohttp:/workspace:Z \
  localhost/ringwall-dev:latest bash -c \
  "cd /workspace && cmake --preset clang-debug && cmake --build --preset clang-debug && ctest --preset clang-debug --output-on-failure"
```

Expected: all tests pass, including the 12 new tests in `test_io_route_meta` and the 6 updated tests in `test_io_route_inspect`.

**Step 5: Commit**

```bash
git add src/router/io_route_inspect.h src/router/io_route_inspect.c tests/unit/test_io_route_inspect.c
git commit -m "feat(router): typed walk API with opts/meta, rename set_metadata to set_meta"
```

---

## Task 6: Run full test suite, format, finalize

**Step 1: Run full test suite**

```bash
podman run --rm --security-opt seccomp=unconfined \
  -v /opt/projects/repositories/iohttp:/workspace:Z \
  localhost/ringwall-dev:latest bash -c \
  "cd /workspace && cmake --preset clang-debug && cmake --build --preset clang-debug && ctest --preset clang-debug --output-on-failure 2>&1"
```

Expected: all tests pass. Count should be previous total + 12 new.

**Step 2: Run clang-format**

```bash
podman run --rm --security-opt seccomp=unconfined \
  -v /opt/projects/repositories/iohttp:/workspace:Z \
  localhost/ringwall-dev:latest bash -c \
  "cd /workspace && cmake --build --preset clang-debug --target format 2>&1"
```

**Step 3: Verify test count**

```bash
podman run --rm --security-opt seccomp=unconfined \
  -v /opt/projects/repositories/iohttp:/workspace:Z \
  localhost/ringwall-dev:latest bash -c \
  "cd /workspace && ctest --preset clang-debug -N 2>&1 | tail -1"
```

**Step 4: Final commit (if format changed anything)**

```bash
git diff --stat
# If changes:
git add -u
git commit -m "style: format route metadata files"
```

---

## Summary

| Task | What | New Tests | Files |
|------|------|-----------|-------|
| 1 | Create `io_route_meta.h/.c` | 0 | 3 (2 new, 1 modify) |
| 2 | Write failing tests | 12 | 2 (1 new, 1 modify) |
| 3 | Add meta to `io_route_opts_t` and `io_radix_node_t` | 0 | 2 modify |
| 4 | Propagate meta in router/radix, add `_with` methods | 0 | 4 modify |
| 5 | Typed walk API + `set_meta()`, fix existing tests | 0 | 3 modify |
| 6 | Full suite, format, finalize | 0 | 0 |

**New tests: 12. Modified tests: 2 (in test_io_route_inspect.c).**

## Critical Files Reference

| File | Role |
|------|------|
| `src/router/io_route_meta.h` | NEW: types (`io_param_in_t`, `io_param_meta_t`, `io_route_meta_t`) |
| `src/router/io_route_meta.c` | NEW: `io_param_in_name()` |
| `src/router/io_router.h` | MODIFY: `meta` in `io_route_opts_t`, `_with` declarations |
| `src/router/io_router.c` | MODIFY: extract meta, implement `_with` methods |
| `src/router/io_radix.h` | MODIFY: `meta` in `io_radix_node_t` |
| `src/router/io_radix.c` | MODIFY: pass meta through insert |
| `src/router/io_route_inspect.h` | MODIFY: typed `io_route_info_t`, `set_meta` |
| `src/router/io_route_inspect.c` | MODIFY: fill opts/meta in walk, `set_meta` impl |
| `tests/unit/test_io_route_meta.c` | NEW: 12 tests |
| `tests/unit/test_io_route_inspect.c` | MODIFY: adapt to new API |
| `tests/unit/test_io_radix.c` | MODIFY: add nullptr param to `io_radix_insert` calls |

## Verification

After all tasks:
1. `ctest --preset clang-debug --output-on-failure` — all tests pass
2. `cmake --build --preset clang-debug --target format` — formatting clean
3. New test count: previous + 12

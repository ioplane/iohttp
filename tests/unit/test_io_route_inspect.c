/**
 * @file test_io_route_inspect.c
 * @brief Unit tests for route introspection and metadata binding.
 */

#include "router/io_route_group.h"
#include "router/io_route_inspect.h"
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

/* ---- Stub handlers (unique addresses for identification) ---- */

static int handler_a(io_ctx_t *c)
{
    (void)c;
    return 0;
}

static int handler_b(io_ctx_t *c)
{
    (void)c;
    return 1;
}

static int handler_c(io_ctx_t *c)
{
    (void)c;
    return 2;
}

static int handler_d(io_ctx_t *c)
{
    (void)c;
    return 3;
}

static int handler_e(io_ctx_t *c)
{
    (void)c;
    return 4;
}

/* ---- Walk context for collecting results ---- */

typedef struct {
    io_route_info_t routes[32];
    char patterns[32][256]; /* owned copies of pattern strings */
    uint32_t count;
} walk_ctx_t;

static int collect_routes(const io_route_info_t *info, void *ctx)
{
    walk_ctx_t *w = (walk_ctx_t *)ctx;
    if (w->count >= 32) {
        return -1;
    }
    w->routes[w->count] = *info;
    /* Copy pattern string since it lives on the stack in walk_node */
    size_t len = strlen(info->pattern);
    if (len >= 256) {
        len = 255;
    }
    memcpy(w->patterns[w->count], info->pattern, len);
    w->patterns[w->count][len] = '\0';
    w->routes[w->count].pattern = w->patterns[w->count];
    w->count++;
    return 0;
}

/* ---- 1. test_route_walk_all ---- */

void test_route_walk_all(void)
{
    /* Register 5 routes across different methods */
    TEST_ASSERT_EQUAL_INT(0, io_router_get(router, "/users", handler_a));
    TEST_ASSERT_EQUAL_INT(0, io_router_post(router, "/users", handler_b));
    TEST_ASSERT_EQUAL_INT(0, io_router_get(router, "/posts", handler_c));
    TEST_ASSERT_EQUAL_INT(0, io_router_put(router, "/posts/:id", handler_d));
    TEST_ASSERT_EQUAL_INT(0, io_router_delete(router, "/posts/:id", handler_e));

    walk_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));

    int rc = io_router_walk(router, collect_routes, &ctx);
    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_EQUAL_UINT32(5, ctx.count);

    /* Verify all handlers were found */
    bool found[5] = {false};
    for (uint32_t i = 0; i < ctx.count; i++) {
        if (ctx.routes[i].handler == handler_a) {
            found[0] = true;
        }
        if (ctx.routes[i].handler == handler_b) {
            found[1] = true;
        }
        if (ctx.routes[i].handler == handler_c) {
            found[2] = true;
        }
        if (ctx.routes[i].handler == handler_d) {
            found[3] = true;
        }
        if (ctx.routes[i].handler == handler_e) {
            found[4] = true;
        }
    }
    for (int i = 0; i < 5; i++) {
        TEST_ASSERT_TRUE_MESSAGE(found[i], "Missing handler in walk");
    }
}

/* ---- 2. test_route_walk_order ---- */

void test_route_walk_order(void)
{
    /* Register routes in reverse method order */
    TEST_ASSERT_EQUAL_INT(0, io_router_delete(router, "/items", handler_e));
    TEST_ASSERT_EQUAL_INT(0, io_router_post(router, "/items", handler_b));
    TEST_ASSERT_EQUAL_INT(0, io_router_get(router, "/items", handler_a));

    walk_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));

    int rc = io_router_walk(router, collect_routes, &ctx);
    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_EQUAL_UINT32(3, ctx.count);

    /* Walk should iterate methods in order: GET(0), POST(1), DELETE(3) */
    TEST_ASSERT_EQUAL_INT(IO_METHOD_GET, ctx.routes[0].method);
    TEST_ASSERT_EQUAL_INT(IO_METHOD_POST, ctx.routes[1].method);
    TEST_ASSERT_EQUAL_INT(IO_METHOD_DELETE, ctx.routes[2].method);
}

/* ---- 3. test_route_walk_includes_groups ---- */

void test_route_walk_includes_groups(void)
{
    /* Register a direct route */
    TEST_ASSERT_EQUAL_INT(0, io_router_get(router, "/health", handler_a));

    /* Register routes via a group */
    io_group_t *api = io_router_group(router, "/api");
    TEST_ASSERT_NOT_NULL(api);

    TEST_ASSERT_EQUAL_INT(0, io_group_get(api, "/users", handler_b));
    TEST_ASSERT_EQUAL_INT(0, io_group_post(api, "/users", handler_c));

    walk_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));

    int rc = io_router_walk(router, collect_routes, &ctx);
    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_EQUAL_UINT32(3, ctx.count);

    /* All 3 routes should appear (group routes are registered on the router) */
    bool found_health = false;
    bool found_api_users_get = false;
    bool found_api_users_post = false;

    for (uint32_t i = 0; i < ctx.count; i++) {
        if (ctx.routes[i].handler == handler_a && strcmp(ctx.routes[i].pattern, "/health") == 0) {
            found_health = true;
        }
        if (ctx.routes[i].handler == handler_b &&
            strcmp(ctx.routes[i].pattern, "/api/users") == 0) {
            found_api_users_get = true;
        }
        if (ctx.routes[i].handler == handler_c &&
            strcmp(ctx.routes[i].pattern, "/api/users") == 0 &&
            ctx.routes[i].method == IO_METHOD_POST) {
            found_api_users_post = true;
        }
    }

    TEST_ASSERT_TRUE(found_health);
    TEST_ASSERT_TRUE(found_api_users_get);
    TEST_ASSERT_TRUE(found_api_users_post);
}

/* ---- 4. test_route_count ---- */

void test_route_count(void)
{
    TEST_ASSERT_EQUAL_UINT32(0, io_router_route_count(router));

    TEST_ASSERT_EQUAL_INT(0, io_router_get(router, "/a", handler_a));
    TEST_ASSERT_EQUAL_UINT32(1, io_router_route_count(router));

    TEST_ASSERT_EQUAL_INT(0, io_router_post(router, "/b", handler_b));
    TEST_ASSERT_EQUAL_UINT32(2, io_router_route_count(router));

    TEST_ASSERT_EQUAL_INT(0, io_router_get(router, "/c", handler_c));
    TEST_ASSERT_EQUAL_UINT32(3, io_router_route_count(router));

    /* Same path, different method counts as separate route */
    TEST_ASSERT_EQUAL_INT(0, io_router_post(router, "/a", handler_d));
    TEST_ASSERT_EQUAL_UINT32(4, io_router_route_count(router));

    /* nullptr router returns 0 */
    TEST_ASSERT_EQUAL_UINT32(0, io_router_route_count(nullptr));
}

/* ---- 5. test_route_set_metadata ---- */

void test_route_set_metadata(void)
{
    TEST_ASSERT_EQUAL_INT(0, io_router_get(router, "/users", handler_a));
    TEST_ASSERT_EQUAL_INT(0, io_router_get(router, "/users/:id", handler_b));

    /* Initially no metadata */
    walk_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    int rc = io_router_walk(router, collect_routes, &ctx);
    TEST_ASSERT_EQUAL_INT(0, rc);
    for (uint32_t i = 0; i < ctx.count; i++) {
        TEST_ASSERT_NULL(ctx.routes[i].metadata);
    }

    /* Attach metadata to /users */
    int tag1 = 42;
    rc = io_router_set_metadata(router, IO_METHOD_GET, "/users", &tag1);
    TEST_ASSERT_EQUAL_INT(0, rc);

    /* Attach metadata to /users/:id */
    int tag2 = 99;
    rc = io_router_set_metadata(router, IO_METHOD_GET, "/users/:id", &tag2);
    TEST_ASSERT_EQUAL_INT(0, rc);

    /* Walk again and verify metadata */
    memset(&ctx, 0, sizeof(ctx));
    rc = io_router_walk(router, collect_routes, &ctx);
    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_EQUAL_UINT32(2, ctx.count);

    for (uint32_t i = 0; i < ctx.count; i++) {
        if (ctx.routes[i].handler == handler_a) {
            TEST_ASSERT_EQUAL_PTR(&tag1, ctx.routes[i].metadata);
        } else if (ctx.routes[i].handler == handler_b) {
            TEST_ASSERT_EQUAL_PTR(&tag2, ctx.routes[i].metadata);
        }
    }

    /* Non-existent route returns -ENOENT */
    rc = io_router_set_metadata(router, IO_METHOD_GET, "/nope", &tag1);
    TEST_ASSERT_EQUAL_INT(-ENOENT, rc);

    /* Wrong method returns -ENOENT */
    rc = io_router_set_metadata(router, IO_METHOD_POST, "/users", &tag1);
    TEST_ASSERT_EQUAL_INT(-ENOENT, rc);
}

/* ---- 6. test_route_metadata_in_match ---- */

void test_route_metadata_in_match(void)
{
    io_route_opts_t opts = {
        .auth_required = true,
        .permissions = 0xFF,
        .oas_operation = nullptr,
    };

    TEST_ASSERT_EQUAL_INT(0, io_router_get_with(router, "/secure", handler_a, &opts));

    /* Dispatch and verify opts are returned */
    io_route_match_t m = io_router_dispatch(router, IO_METHOD_GET, "/secure", strlen("/secure"));
    TEST_ASSERT_EQUAL_INT(IO_MATCH_FOUND, m.status);
    TEST_ASSERT_EQUAL_PTR(handler_a, m.handler);
    TEST_ASSERT_NOT_NULL(m.opts);
    TEST_ASSERT_TRUE(m.opts->auth_required);
    TEST_ASSERT_EQUAL_UINT32(0xFF, m.opts->permissions);

    /* Now use set_metadata to attach additional metadata */
    int extra_tag = 777;
    int rc = io_router_set_metadata(router, IO_METHOD_GET, "/secure", &extra_tag);
    TEST_ASSERT_EQUAL_INT(0, rc);

    /* Walk and verify metadata was updated */
    walk_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    rc = io_router_walk(router, collect_routes, &ctx);
    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_EQUAL_UINT32(1, ctx.count);
    TEST_ASSERT_EQUAL_PTR(&extra_tag, ctx.routes[0].metadata);
}

/* ---- Main ---- */

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_route_walk_all);
    RUN_TEST(test_route_walk_order);
    RUN_TEST(test_route_walk_includes_groups);
    RUN_TEST(test_route_count);
    RUN_TEST(test_route_set_metadata);
    RUN_TEST(test_route_metadata_in_match);
    return UNITY_END();
}

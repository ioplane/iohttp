/**
 * @file test_io_route_meta.c
 * @brief Unit tests for route metadata types and introspection integration.
 */

#include "router/ioh_route_inspect.h"
#include "router/ioh_route_meta.h"
#include "router/ioh_router.h"

#include <errno.h>
#include <string.h>

#include <unity.h>

static ioh_router_t *router;

void setUp(void)
{
    router = ioh_router_create();
    TEST_ASSERT_NOT_NULL(router);
}

void tearDown(void)
{
    ioh_router_destroy(router);
    router = nullptr;
}

static int handler_a(ioh_ctx_t *c)
{
    (void)c;
    return 0;
}

typedef struct {
    ioh_route_info_t routes[16];
    char patterns[16][256];
    uint32_t count;
} walk_ctx_t;

static int collect_routes(const ioh_route_info_t *info, void *ctx)
{
    walk_ctx_t *w = (walk_ctx_t *)ctx;
    if (w->count >= 16) {
        return -1;
    }
    w->routes[w->count] = *info;
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

void test_param_in_name_path(void)
{
    TEST_ASSERT_EQUAL_STRING("path", ioh_param_in_name(IOH_PARAM_PATH));
}

void test_param_in_name_query(void)
{
    TEST_ASSERT_EQUAL_STRING("query", ioh_param_in_name(IOH_PARAM_QUERY));
}

void test_param_in_name_header(void)
{
    TEST_ASSERT_EQUAL_STRING("header", ioh_param_in_name(IOH_PARAM_HEADER));
}

void test_param_in_name_cookie(void)
{
    TEST_ASSERT_EQUAL_STRING("cookie", ioh_param_in_name(IOH_PARAM_COOKIE));
}

void test_meta_summary_via_opts(void)
{
    static const ioh_route_meta_t meta = {.summary = "List all users"};
    ioh_route_opts_t opts = {.meta = &meta};

    TEST_ASSERT_EQUAL_INT(0, ioh_router_get_with(router, "/users", handler_a, &opts));

    walk_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    TEST_ASSERT_EQUAL_INT(0, ioh_router_walk(router, collect_routes, &ctx));
    TEST_ASSERT_EQUAL_UINT32(1, ctx.count);
    TEST_ASSERT_NOT_NULL(ctx.routes[0].meta);
    TEST_ASSERT_EQUAL_STRING("List all users", ctx.routes[0].meta->summary);
}

void test_meta_tags_nullptr_terminated(void)
{
    static const char *tags[] = {"users", "admin", nullptr};
    static const ioh_route_meta_t meta = {.summary = "Get user", .tags = tags};
    ioh_route_opts_t opts = {.meta = &meta};

    TEST_ASSERT_EQUAL_INT(0, ioh_router_get_with(router, "/users/:id", handler_a, &opts));

    walk_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    TEST_ASSERT_EQUAL_INT(0, ioh_router_walk(router, collect_routes, &ctx));
    TEST_ASSERT_NOT_NULL(ctx.routes[0].meta);
    TEST_ASSERT_NOT_NULL(ctx.routes[0].meta->tags);
    TEST_ASSERT_EQUAL_STRING("users", ctx.routes[0].meta->tags[0]);
    TEST_ASSERT_EQUAL_STRING("admin", ctx.routes[0].meta->tags[1]);
    TEST_ASSERT_NULL(ctx.routes[0].meta->tags[2]);
}

void test_meta_deprecated_flag(void)
{
    static const ioh_route_meta_t meta = {.summary = "Old endpoint", .deprecated = true};
    ioh_route_opts_t opts = {.meta = &meta};

    TEST_ASSERT_EQUAL_INT(0, ioh_router_get_with(router, "/v1/users", handler_a, &opts));

    walk_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    TEST_ASSERT_EQUAL_INT(0, ioh_router_walk(router, collect_routes, &ctx));
    TEST_ASSERT_NOT_NULL(ctx.routes[0].meta);
    TEST_ASSERT_TRUE(ctx.routes[0].meta->deprecated);
}

void test_meta_params_path(void)
{
    static const ioh_param_meta_t params[] = {
        {.name = "id", .in = IOH_PARAM_PATH, .required = true, .description = "User UUID"},
    };
    static const ioh_route_meta_t meta = {
        .summary = "Get user",
        .params = params,
        .param_count = 1,
    };
    ioh_route_opts_t opts = {.meta = &meta};

    TEST_ASSERT_EQUAL_INT(0, ioh_router_get_with(router, "/users/:id", handler_a, &opts));

    walk_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    TEST_ASSERT_EQUAL_INT(0, ioh_router_walk(router, collect_routes, &ctx));
    TEST_ASSERT_NOT_NULL(ctx.routes[0].meta);
    TEST_ASSERT_EQUAL_UINT32(1, ctx.routes[0].meta->param_count);
    TEST_ASSERT_EQUAL_STRING("id", ctx.routes[0].meta->params[0].name);
    TEST_ASSERT_EQUAL_INT(IOH_PARAM_PATH, ctx.routes[0].meta->params[0].in);
    TEST_ASSERT_TRUE(ctx.routes[0].meta->params[0].required);
    TEST_ASSERT_EQUAL_STRING("User UUID", ctx.routes[0].meta->params[0].description);
}

void test_meta_params_multiple(void)
{
    static const ioh_param_meta_t params[] = {
        {.name = "id", .in = IOH_PARAM_PATH, .required = true},
        {.name = "fields",
         .in = IOH_PARAM_QUERY,
         .required = false,
         .description = "Comma-separated field names"},
    };
    static const ioh_route_meta_t meta = {
        .summary = "Get user with field selection",
        .params = params,
        .param_count = 2,
    };
    ioh_route_opts_t opts = {.meta = &meta};

    TEST_ASSERT_EQUAL_INT(0, ioh_router_get_with(router, "/users/:id", handler_a, &opts));

    walk_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    TEST_ASSERT_EQUAL_INT(0, ioh_router_walk(router, collect_routes, &ctx));
    TEST_ASSERT_NOT_NULL(ctx.routes[0].meta);
    TEST_ASSERT_EQUAL_UINT32(2, ctx.routes[0].meta->param_count);
    TEST_ASSERT_EQUAL_STRING("id", ctx.routes[0].meta->params[0].name);
    TEST_ASSERT_EQUAL_STRING("fields", ctx.routes[0].meta->params[1].name);
    TEST_ASSERT_EQUAL_INT(IOH_PARAM_QUERY, ctx.routes[0].meta->params[1].in);
    TEST_ASSERT_FALSE(ctx.routes[0].meta->params[1].required);
}

void test_meta_null_when_no_opts(void)
{
    TEST_ASSERT_EQUAL_INT(0, ioh_router_get(router, "/health", handler_a));

    walk_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    TEST_ASSERT_EQUAL_INT(0, ioh_router_walk(router, collect_routes, &ctx));
    TEST_ASSERT_EQUAL_UINT32(1, ctx.count);
    TEST_ASSERT_NULL(ctx.routes[0].meta);
}

void test_meta_set_meta_after_registration(void)
{
    ioh_route_opts_t opts = {.auth_required = true, .permissions = 0xFF};
    TEST_ASSERT_EQUAL_INT(0, ioh_router_get_with(router, "/users", handler_a, &opts));

    walk_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    TEST_ASSERT_EQUAL_INT(0, ioh_router_walk(router, collect_routes, &ctx));
    TEST_ASSERT_NULL(ctx.routes[0].meta);

    static const ioh_route_meta_t meta = {.summary = "List users"};
    TEST_ASSERT_EQUAL_INT(0, ioh_router_set_meta(router, IOH_METHOD_GET, "/users", &meta));

    memset(&ctx, 0, sizeof(ctx));
    TEST_ASSERT_EQUAL_INT(0, ioh_router_walk(router, collect_routes, &ctx));
    TEST_ASSERT_NOT_NULL(ctx.routes[0].meta);
    TEST_ASSERT_EQUAL_STRING("List users", ctx.routes[0].meta->summary);

    /* Dispatch also sees updated meta */
    ioh_route_match_t m = ioh_router_dispatch(router, IOH_METHOD_GET, "/users", strlen("/users"));
    TEST_ASSERT_EQUAL_INT(IOH_MATCH_FOUND, m.status);
    TEST_ASSERT_NOT_NULL(m.meta);
    TEST_ASSERT_EQUAL_STRING("List users", m.meta->summary);
    /* opts unchanged */
    TEST_ASSERT_NOT_NULL(m.opts);
    TEST_ASSERT_TRUE(m.opts->auth_required);
    TEST_ASSERT_EQUAL_UINT32(0xFF, m.opts->permissions);
}

void test_meta_opts_and_meta_combined(void)
{
    static const ioh_route_meta_t meta = {.summary = "Delete user"};
    ioh_route_opts_t opts = {.meta = &meta, .auth_required = true, .permissions = 0x01};

    TEST_ASSERT_EQUAL_INT(0, ioh_router_delete_with(router, "/users/:id", handler_a, &opts));

    walk_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    TEST_ASSERT_EQUAL_INT(0, ioh_router_walk(router, collect_routes, &ctx));
    TEST_ASSERT_NOT_NULL(ctx.routes[0].meta);
    TEST_ASSERT_EQUAL_STRING("Delete user", ctx.routes[0].meta->summary);

    ioh_route_match_t m =
        ioh_router_dispatch(router, IOH_METHOD_DELETE, "/users/42", strlen("/users/42"));
    TEST_ASSERT_EQUAL_INT(IOH_MATCH_FOUND, m.status);
    TEST_ASSERT_NOT_NULL(m.opts);
    TEST_ASSERT_TRUE(m.opts->auth_required);
    TEST_ASSERT_EQUAL_UINT32(0x01, m.opts->permissions);
    TEST_ASSERT_NOT_NULL(m.meta);
    TEST_ASSERT_EQUAL_STRING("Delete user", m.meta->summary);
}

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

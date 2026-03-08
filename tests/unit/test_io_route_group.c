/**
 * @file test_io_route_group.c
 * @brief Unit tests for route groups with prefix composition and middleware.
 */

#include "router/io_route_group.h"
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

static int handler_users(io_ctx_t *c)
{
    (void)c;
    return 0;
}

static int handler_posts(io_ctx_t *c)
{
    (void)c;
    return 1;
}

static int handler_admin(io_ctx_t *c)
{
    (void)c;
    return 2;
}

/* ---- Stub middleware ---- */

static int stub_mw_a(io_ctx_t *c, io_handler_fn next)
{
    (void)c;
    (void)next;
    return 0;
}

static int stub_mw_b(io_ctx_t *c, io_handler_fn next)
{
    (void)c;
    (void)next;
    return 0;
}

/* ---- 1. test_group_create ---- */

void test_group_create(void)
{
    io_group_t *g = io_router_group(router, "/api");
    TEST_ASSERT_NOT_NULL(g);
    TEST_ASSERT_EQUAL_STRING("/api", io_group_prefix(g));
    TEST_ASSERT_NULL(io_group_parent(g));
}

/* ---- 2. test_group_prefix_composition ---- */

void test_group_prefix_composition(void)
{
    io_group_t *api = io_router_group(router, "/api");
    TEST_ASSERT_NOT_NULL(api);

    io_group_t *v1 = io_group_subgroup(api, "/v1");
    TEST_ASSERT_NOT_NULL(v1);

    io_group_t *users = io_group_subgroup(v1, "/users");
    TEST_ASSERT_NOT_NULL(users);

    TEST_ASSERT_EQUAL_STRING("/api/v1/users", io_group_prefix(users));
}

/* ---- 3. test_group_nested_subgroup ---- */

void test_group_nested_subgroup(void)
{
    io_group_t *api = io_router_group(router, "/api");
    TEST_ASSERT_NOT_NULL(api);

    io_group_t *v1 = io_group_subgroup(api, "/v1");
    TEST_ASSERT_NOT_NULL(v1);

    io_group_t *admin = io_group_subgroup(v1, "/admin");
    TEST_ASSERT_NOT_NULL(admin);

    TEST_ASSERT_EQUAL_STRING("/api", io_group_prefix(api));
    TEST_ASSERT_EQUAL_STRING("/api/v1", io_group_prefix(v1));
    TEST_ASSERT_EQUAL_STRING("/api/v1/admin", io_group_prefix(admin));

    /* Verify parent chain */
    TEST_ASSERT_NULL(io_group_parent(api));
    TEST_ASSERT_EQUAL_PTR(api, io_group_parent(v1));
    TEST_ASSERT_EQUAL_PTR(v1, io_group_parent(admin));
}

/* ---- 4. test_group_method_registration ---- */

void test_group_method_registration(void)
{
    io_group_t *api = io_router_group(router, "/api");
    TEST_ASSERT_NOT_NULL(api);

    int rc = io_group_get(api, "/users", handler_users);
    TEST_ASSERT_EQUAL_INT(0, rc);

    rc = io_group_post(api, "/posts", handler_posts);
    TEST_ASSERT_EQUAL_INT(0, rc);

    /* Verify routes are registered on the router with full prefix */
    io_route_match_t m =
        io_router_dispatch(router, IO_METHOD_GET, "/api/users", strlen("/api/users"));
    TEST_ASSERT_EQUAL_INT(IO_MATCH_FOUND, m.status);
    TEST_ASSERT_EQUAL_PTR(handler_users, m.handler);

    m = io_router_dispatch(router, IO_METHOD_POST, "/api/posts", strlen("/api/posts"));
    TEST_ASSERT_EQUAL_INT(IO_MATCH_FOUND, m.status);
    TEST_ASSERT_EQUAL_PTR(handler_posts, m.handler);

    /* Verify the route does NOT exist without the prefix */
    m = io_router_dispatch(router, IO_METHOD_GET, "/users", strlen("/users"));
    TEST_ASSERT_NOT_EQUAL(IO_MATCH_FOUND, m.status);
}

/* ---- 5. test_group_middleware_applied ---- */

void test_group_middleware_applied(void)
{
    io_group_t *api = io_router_group(router, "/api");
    TEST_ASSERT_NOT_NULL(api);

    TEST_ASSERT_EQUAL_UINT32(0, io_group_middleware_count(api));

    int rc = io_group_use(api, stub_mw_a);
    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_EQUAL_UINT32(1, io_group_middleware_count(api));

    rc = io_group_use(api, stub_mw_b);
    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_EQUAL_UINT32(2, io_group_middleware_count(api));
}

/* ---- 6. test_group_middleware_not_leaked ---- */

void test_group_middleware_not_leaked(void)
{
    io_group_t *api = io_router_group(router, "/api");
    io_group_t *admin = io_router_group(router, "/admin");
    TEST_ASSERT_NOT_NULL(api);
    TEST_ASSERT_NOT_NULL(admin);

    int rc = io_group_use(api, stub_mw_a);
    TEST_ASSERT_EQUAL_INT(0, rc);

    rc = io_group_use(api, stub_mw_b);
    TEST_ASSERT_EQUAL_INT(0, rc);

    /* Sibling group must NOT have api's middleware */
    TEST_ASSERT_EQUAL_UINT32(2, io_group_middleware_count(api));
    TEST_ASSERT_EQUAL_UINT32(0, io_group_middleware_count(admin));
}

/* ---- 7. test_group_middleware_inheritance ---- */

void test_group_middleware_inheritance(void)
{
    io_group_t *api = io_router_group(router, "/api");
    TEST_ASSERT_NOT_NULL(api);

    int rc = io_group_use(api, stub_mw_a);
    TEST_ASSERT_EQUAL_INT(0, rc);

    io_group_t *v1 = io_group_subgroup(api, "/v1");
    TEST_ASSERT_NOT_NULL(v1);

    /* Subgroup itself has no middleware yet */
    TEST_ASSERT_EQUAL_UINT32(0, io_group_middleware_count(v1));

    /* But the parent chain can be walked to find parent middleware */
    const io_group_t *parent = io_group_parent(v1);
    TEST_ASSERT_NOT_NULL(parent);
    TEST_ASSERT_EQUAL_UINT32(1, io_group_middleware_count(parent));
    TEST_ASSERT_EQUAL_PTR(stub_mw_a, io_group_middleware_at(parent, 0));
}

/* ---- 8. test_group_middleware_order ---- */

void test_group_middleware_order(void)
{
    io_group_t *api = io_router_group(router, "/api");
    TEST_ASSERT_NOT_NULL(api);

    int rc = io_group_use(api, stub_mw_a);
    TEST_ASSERT_EQUAL_INT(0, rc);

    rc = io_group_use(api, stub_mw_b);
    TEST_ASSERT_EQUAL_INT(0, rc);

    /* First use() -> mw[0], second use() -> mw[1] */
    TEST_ASSERT_EQUAL_PTR(stub_mw_a, io_group_middleware_at(api, 0));
    TEST_ASSERT_EQUAL_PTR(stub_mw_b, io_group_middleware_at(api, 1));

    /* Out of range returns nullptr */
    TEST_ASSERT_NULL(io_group_middleware_at(api, 2));
}

/* ---- 9. test_group_empty ---- */

void test_group_empty(void)
{
    /* Create a group with no routes and no middleware; destroy works */
    io_group_t *empty = io_router_group(router, "/empty");
    TEST_ASSERT_NOT_NULL(empty);
    TEST_ASSERT_EQUAL_STRING("/empty", io_group_prefix(empty));
    TEST_ASSERT_EQUAL_UINT32(0, io_group_middleware_count(empty));

    /* nullptr is safe */
    io_group_destroy(nullptr);
}

/* ---- 10. test_group_with_route_opts ---- */

void test_group_with_route_opts(void)
{
    io_group_t *api = io_router_group(router, "/api");
    TEST_ASSERT_NOT_NULL(api);

    io_route_opts_t opts = {
        .auth_required = true,
        .permissions = 0x0F,
        .oas_operation = nullptr,
    };

    int rc = io_group_get_with(api, "/secure", handler_admin, &opts);
    TEST_ASSERT_EQUAL_INT(0, rc);

    io_route_match_t m =
        io_router_dispatch(router, IO_METHOD_GET, "/api/secure", strlen("/api/secure"));
    TEST_ASSERT_EQUAL_INT(IO_MATCH_FOUND, m.status);
    TEST_ASSERT_EQUAL_PTR(handler_admin, m.handler);
    TEST_ASSERT_NOT_NULL(m.opts);
    TEST_ASSERT_TRUE(m.opts->auth_required);
    TEST_ASSERT_EQUAL_UINT32(0x0F, m.opts->permissions);
}

/* ---- Main ---- */

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_group_create);
    RUN_TEST(test_group_prefix_composition);
    RUN_TEST(test_group_nested_subgroup);
    RUN_TEST(test_group_method_registration);
    RUN_TEST(test_group_middleware_applied);
    RUN_TEST(test_group_middleware_not_leaked);
    RUN_TEST(test_group_middleware_inheritance);
    RUN_TEST(test_group_middleware_order);
    RUN_TEST(test_group_empty);
    RUN_TEST(test_group_with_route_opts);
    return UNITY_END();
}

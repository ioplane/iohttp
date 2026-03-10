/**
 * @file test_io_route_group.c
 * @brief Unit tests for route groups with prefix composition and middleware.
 */

#include "router/ioh_route_group.h"
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

/* ---- Stub handlers ---- */

static int handler_users(ioh_ctx_t *c)
{
    (void)c;
    return 0;
}

static int handler_posts(ioh_ctx_t *c)
{
    (void)c;
    return 1;
}

static int handler_admin(ioh_ctx_t *c)
{
    (void)c;
    return 2;
}

/* ---- Stub middleware ---- */

static int stub_mw_a(ioh_ctx_t *c, ioh_handler_fn next)
{
    (void)c;
    (void)next;
    return 0;
}

static int stub_mw_b(ioh_ctx_t *c, ioh_handler_fn next)
{
    (void)c;
    (void)next;
    return 0;
}

/* ---- 1. test_group_create ---- */

void test_group_create(void)
{
    ioh_group_t *g = ioh_router_group(router, "/api");
    TEST_ASSERT_NOT_NULL(g);
    TEST_ASSERT_EQUAL_STRING("/api", ioh_group_prefix(g));
    TEST_ASSERT_NULL(ioh_group_parent(g));
}

/* ---- 2. test_group_prefix_composition ---- */

void test_group_prefix_composition(void)
{
    ioh_group_t *api = ioh_router_group(router, "/api");
    TEST_ASSERT_NOT_NULL(api);

    ioh_group_t *v1 = ioh_group_subgroup(api, "/v1");
    TEST_ASSERT_NOT_NULL(v1);

    ioh_group_t *users = ioh_group_subgroup(v1, "/users");
    TEST_ASSERT_NOT_NULL(users);

    TEST_ASSERT_EQUAL_STRING("/api/v1/users", ioh_group_prefix(users));
}

/* ---- 3. test_group_nested_subgroup ---- */

void test_group_nested_subgroup(void)
{
    ioh_group_t *api = ioh_router_group(router, "/api");
    TEST_ASSERT_NOT_NULL(api);

    ioh_group_t *v1 = ioh_group_subgroup(api, "/v1");
    TEST_ASSERT_NOT_NULL(v1);

    ioh_group_t *admin = ioh_group_subgroup(v1, "/admin");
    TEST_ASSERT_NOT_NULL(admin);

    TEST_ASSERT_EQUAL_STRING("/api", ioh_group_prefix(api));
    TEST_ASSERT_EQUAL_STRING("/api/v1", ioh_group_prefix(v1));
    TEST_ASSERT_EQUAL_STRING("/api/v1/admin", ioh_group_prefix(admin));

    /* Verify parent chain */
    TEST_ASSERT_NULL(ioh_group_parent(api));
    TEST_ASSERT_EQUAL_PTR(api, ioh_group_parent(v1));
    TEST_ASSERT_EQUAL_PTR(v1, ioh_group_parent(admin));
}

/* ---- 4. test_group_method_registration ---- */

void test_group_method_registration(void)
{
    ioh_group_t *api = ioh_router_group(router, "/api");
    TEST_ASSERT_NOT_NULL(api);

    int rc = ioh_group_get(api, "/users", handler_users);
    TEST_ASSERT_EQUAL_INT(0, rc);

    rc = ioh_group_post(api, "/posts", handler_posts);
    TEST_ASSERT_EQUAL_INT(0, rc);

    /* Verify routes are registered on the router with full prefix */
    ioh_route_match_t m =
        ioh_router_dispatch(router, IOH_METHOD_GET, "/api/users", strlen("/api/users"));
    TEST_ASSERT_EQUAL_INT(IOH_MATCH_FOUND, m.status);
    TEST_ASSERT_EQUAL_PTR(handler_users, m.handler);

    m = ioh_router_dispatch(router, IOH_METHOD_POST, "/api/posts", strlen("/api/posts"));
    TEST_ASSERT_EQUAL_INT(IOH_MATCH_FOUND, m.status);
    TEST_ASSERT_EQUAL_PTR(handler_posts, m.handler);

    /* Verify the route does NOT exist without the prefix */
    m = ioh_router_dispatch(router, IOH_METHOD_GET, "/users", strlen("/users"));
    TEST_ASSERT_NOT_EQUAL(IOH_MATCH_FOUND, m.status);
}

/* ---- 5. test_group_middleware_applied ---- */

void test_group_middleware_applied(void)
{
    ioh_group_t *api = ioh_router_group(router, "/api");
    TEST_ASSERT_NOT_NULL(api);

    TEST_ASSERT_EQUAL_UINT32(0, ioh_group_middleware_count(api));

    int rc = ioh_group_use(api, stub_mw_a);
    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_EQUAL_UINT32(1, ioh_group_middleware_count(api));

    rc = ioh_group_use(api, stub_mw_b);
    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_EQUAL_UINT32(2, ioh_group_middleware_count(api));
}

/* ---- 6. test_group_middleware_not_leaked ---- */

void test_group_middleware_not_leaked(void)
{
    ioh_group_t *api = ioh_router_group(router, "/api");
    ioh_group_t *admin = ioh_router_group(router, "/admin");
    TEST_ASSERT_NOT_NULL(api);
    TEST_ASSERT_NOT_NULL(admin);

    int rc = ioh_group_use(api, stub_mw_a);
    TEST_ASSERT_EQUAL_INT(0, rc);

    rc = ioh_group_use(api, stub_mw_b);
    TEST_ASSERT_EQUAL_INT(0, rc);

    /* Sibling group must NOT have api's middleware */
    TEST_ASSERT_EQUAL_UINT32(2, ioh_group_middleware_count(api));
    TEST_ASSERT_EQUAL_UINT32(0, ioh_group_middleware_count(admin));
}

/* ---- 7. test_group_middleware_inheritance ---- */

void test_group_middleware_inheritance(void)
{
    ioh_group_t *api = ioh_router_group(router, "/api");
    TEST_ASSERT_NOT_NULL(api);

    int rc = ioh_group_use(api, stub_mw_a);
    TEST_ASSERT_EQUAL_INT(0, rc);

    ioh_group_t *v1 = ioh_group_subgroup(api, "/v1");
    TEST_ASSERT_NOT_NULL(v1);

    /* Subgroup itself has no middleware yet */
    TEST_ASSERT_EQUAL_UINT32(0, ioh_group_middleware_count(v1));

    /* But the parent chain can be walked to find parent middleware */
    const ioh_group_t *parent = ioh_group_parent(v1);
    TEST_ASSERT_NOT_NULL(parent);
    TEST_ASSERT_EQUAL_UINT32(1, ioh_group_middleware_count(parent));
    TEST_ASSERT_EQUAL_PTR(stub_mw_a, ioh_group_middleware_at(parent, 0));
}

/* ---- 8. test_group_middleware_order ---- */

void test_group_middleware_order(void)
{
    ioh_group_t *api = ioh_router_group(router, "/api");
    TEST_ASSERT_NOT_NULL(api);

    int rc = ioh_group_use(api, stub_mw_a);
    TEST_ASSERT_EQUAL_INT(0, rc);

    rc = ioh_group_use(api, stub_mw_b);
    TEST_ASSERT_EQUAL_INT(0, rc);

    /* First use() -> mw[0], second use() -> mw[1] */
    TEST_ASSERT_EQUAL_PTR(stub_mw_a, ioh_group_middleware_at(api, 0));
    TEST_ASSERT_EQUAL_PTR(stub_mw_b, ioh_group_middleware_at(api, 1));

    /* Out of range returns nullptr */
    TEST_ASSERT_NULL(ioh_group_middleware_at(api, 2));
}

/* ---- 9. test_group_empty ---- */

void test_group_empty(void)
{
    /* Create a group with no routes and no middleware; destroy works */
    ioh_group_t *empty = ioh_router_group(router, "/empty");
    TEST_ASSERT_NOT_NULL(empty);
    TEST_ASSERT_EQUAL_STRING("/empty", ioh_group_prefix(empty));
    TEST_ASSERT_EQUAL_UINT32(0, ioh_group_middleware_count(empty));

    /* nullptr is safe */
    ioh_group_destroy(nullptr);
}

/* ---- 10. test_group_with_route_opts ---- */

void test_group_with_route_opts(void)
{
    ioh_group_t *api = ioh_router_group(router, "/api");
    TEST_ASSERT_NOT_NULL(api);

    ioh_route_opts_t opts = {
        .auth_required = true,
        .permissions = 0x0F,
        .oas_operation = nullptr,
    };

    int rc = ioh_group_get_with(api, "/secure", handler_admin, &opts);
    TEST_ASSERT_EQUAL_INT(0, rc);

    ioh_route_match_t m =
        ioh_router_dispatch(router, IOH_METHOD_GET, "/api/secure", strlen("/api/secure"));
    TEST_ASSERT_EQUAL_INT(IOH_MATCH_FOUND, m.status);
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

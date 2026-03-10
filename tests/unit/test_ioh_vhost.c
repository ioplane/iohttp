/**
 * @file test_io_vhost.c
 * @brief Unit tests for host-based virtual routing dispatcher.
 */

#include "router/ioh_router.h"
#include "router/ioh_vhost.h"

#include <errno.h>
#include <string.h>

#include <unity.h>

/* ---- Stub handler ---- */

static int handler_ok(ioh_ctx_t *c)
{
    (void)c;
    return 0;
}

/* ---- Helpers ---- */

static ioh_router_t *make_router(void)
{
    ioh_router_t *r = ioh_router_create();
    TEST_ASSERT_NOT_NULL(r);
    int rc = ioh_router_get(r, "/", handler_ok);
    TEST_ASSERT_EQUAL_INT(0, rc);
    return r;
}

/* ---- Test fixtures ---- */

void setUp(void)
{
}

void tearDown(void)
{
}

/* ---- Tests ---- */

void test_vhost_create_destroy(void)
{
    ioh_vhost_t *v = ioh_vhost_create();
    TEST_ASSERT_NOT_NULL(v);
    TEST_ASSERT_EQUAL_UINT32(0, ioh_vhost_count(v));
    ioh_vhost_destroy(v);
}

void test_vhost_destroy_null(void)
{
    /* Must not crash */
    ioh_vhost_destroy(nullptr);
}

void test_vhost_exact_match(void)
{
    ioh_vhost_t *v = ioh_vhost_create();
    ioh_router_t *r = make_router();

    int rc = ioh_vhost_add(v, "api.example.com", r);
    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_EQUAL_UINT32(1, ioh_vhost_count(v));

    ioh_route_match_t m = ioh_vhost_dispatch(v, IOH_METHOD_GET, "api.example.com", "/", 1);
    TEST_ASSERT_EQUAL_INT(IOH_MATCH_FOUND, m.status);

    ioh_router_destroy(r);
    ioh_vhost_destroy(v);
}

void test_vhost_wildcard_match(void)
{
    ioh_vhost_t *v = ioh_vhost_create();
    ioh_router_t *r = make_router();

    int rc = ioh_vhost_add(v, "*.example.com", r);
    TEST_ASSERT_EQUAL_INT(0, rc);

    ioh_route_match_t m1 = ioh_vhost_dispatch(v, IOH_METHOD_GET, "foo.example.com", "/", 1);
    TEST_ASSERT_EQUAL_INT(IOH_MATCH_FOUND, m1.status);

    ioh_route_match_t m2 = ioh_vhost_dispatch(v, IOH_METHOD_GET, "bar.example.com", "/", 1);
    TEST_ASSERT_EQUAL_INT(IOH_MATCH_FOUND, m2.status);

    ioh_router_destroy(r);
    ioh_vhost_destroy(v);
}

void test_vhost_wildcard_no_match_bare_domain(void)
{
    ioh_vhost_t *v = ioh_vhost_create();
    ioh_router_t *r = make_router();

    int rc = ioh_vhost_add(v, "*.example.com", r);
    TEST_ASSERT_EQUAL_INT(0, rc);

    /* "*.example.com" does NOT match "example.com" */
    ioh_route_match_t m = ioh_vhost_dispatch(v, IOH_METHOD_GET, "example.com", "/", 1);
    TEST_ASSERT_EQUAL_INT(IOH_MATCH_NOT_FOUND, m.status);

    ioh_router_destroy(r);
    ioh_vhost_destroy(v);
}

void test_vhost_default_fallback(void)
{
    ioh_vhost_t *v = ioh_vhost_create();
    ioh_router_t *r_api = make_router();
    ioh_router_t *r_default = make_router();

    int rc = ioh_vhost_add(v, "api.example.com", r_api);
    TEST_ASSERT_EQUAL_INT(0, rc);
    ioh_vhost_set_default(v, r_default);

    /* Unmatched host falls back to default */
    ioh_route_match_t m = ioh_vhost_dispatch(v, IOH_METHOD_GET, "unknown.example.com", "/", 1);
    TEST_ASSERT_EQUAL_INT(IOH_MATCH_FOUND, m.status);

    ioh_router_destroy(r_api);
    ioh_router_destroy(r_default);
    ioh_vhost_destroy(v);
}

void test_vhost_no_host_no_default(void)
{
    ioh_vhost_t *v = ioh_vhost_create();
    ioh_router_t *r = make_router();

    int rc = ioh_vhost_add(v, "api.example.com", r);
    TEST_ASSERT_EQUAL_INT(0, rc);

    /* nullptr host and no default router */
    ioh_route_match_t m = ioh_vhost_dispatch(v, IOH_METHOD_GET, nullptr, "/", 1);
    TEST_ASSERT_EQUAL_INT(IOH_MATCH_NOT_FOUND, m.status);

    ioh_router_destroy(r);
    ioh_vhost_destroy(v);
}

void test_vhost_port_stripping(void)
{
    ioh_vhost_t *v = ioh_vhost_create();
    ioh_router_t *r = make_router();

    int rc = ioh_vhost_add(v, "api.example.com", r);
    TEST_ASSERT_EQUAL_INT(0, rc);

    ioh_route_match_t m = ioh_vhost_dispatch(v, IOH_METHOD_GET, "api.example.com:8080", "/", 1);
    TEST_ASSERT_EQUAL_INT(IOH_MATCH_FOUND, m.status);

    ioh_router_destroy(r);
    ioh_vhost_destroy(v);
}

void test_vhost_port_stripping_ipv6(void)
{
    ioh_vhost_t *v = ioh_vhost_create();
    ioh_router_t *r = make_router();

    int rc = ioh_vhost_add(v, "[::1]", r);
    TEST_ASSERT_EQUAL_INT(0, rc);

    /* IPv6 with port: [::1]:8080 -> [::1] */
    ioh_route_match_t m = ioh_vhost_dispatch(v, IOH_METHOD_GET, "[::1]:8080", "/", 1);
    TEST_ASSERT_EQUAL_INT(IOH_MATCH_FOUND, m.status);

    ioh_router_destroy(r);
    ioh_vhost_destroy(v);
}

void test_vhost_case_insensitive(void)
{
    ioh_vhost_t *v = ioh_vhost_create();
    ioh_router_t *r = make_router();

    int rc = ioh_vhost_add(v, "api.example.com", r);
    TEST_ASSERT_EQUAL_INT(0, rc);

    ioh_route_match_t m = ioh_vhost_dispatch(v, IOH_METHOD_GET, "API.Example.COM", "/", 1);
    TEST_ASSERT_EQUAL_INT(IOH_MATCH_FOUND, m.status);

    ioh_router_destroy(r);
    ioh_vhost_destroy(v);
}

void test_vhost_exact_before_wildcard(void)
{
    ioh_vhost_t *v = ioh_vhost_create();
    ioh_router_t *r_exact = make_router();
    ioh_router_t *r_wild = make_router();

    /* Register wildcard first, then exact — exact should still win */
    int rc = ioh_vhost_add(v, "*.example.com", r_wild);
    TEST_ASSERT_EQUAL_INT(0, rc);
    rc = ioh_vhost_add(v, "api.example.com", r_exact);
    TEST_ASSERT_EQUAL_INT(0, rc);

    ioh_route_match_t m = ioh_vhost_dispatch(v, IOH_METHOD_GET, "api.example.com", "/", 1);
    TEST_ASSERT_EQUAL_INT(IOH_MATCH_FOUND, m.status);

    /* Wildcard should still work for non-exact hosts */
    ioh_route_match_t m2 = ioh_vhost_dispatch(v, IOH_METHOD_GET, "other.example.com", "/", 1);
    TEST_ASSERT_EQUAL_INT(IOH_MATCH_FOUND, m2.status);

    ioh_router_destroy(r_exact);
    ioh_router_destroy(r_wild);
    ioh_vhost_destroy(v);
}

void test_vhost_max_hosts(void)
{
    ioh_vhost_t *v = ioh_vhost_create();
    ioh_router_t *r = make_router();

    char hostname[64];
    for (uint32_t i = 0; i < IOH_VHOST_MAX_HOSTS; i++) {
        snprintf(hostname, sizeof(hostname), "host%u.example.com", i);
        int rc = ioh_vhost_add(v, hostname, r);
        TEST_ASSERT_EQUAL_INT(0, rc);
    }

    TEST_ASSERT_EQUAL_UINT32(IOH_VHOST_MAX_HOSTS, ioh_vhost_count(v));

    /* Next add should fail with -ENOSPC */
    int rc = ioh_vhost_add(v, "overflow.example.com", r);
    TEST_ASSERT_EQUAL_INT(-ENOSPC, rc);

    ioh_router_destroy(r);
    ioh_vhost_destroy(v);
}

void test_vhost_null_inputs(void)
{
    ioh_router_t *r = make_router();

    /* nullptr vhost */
    int rc = ioh_vhost_add(nullptr, "host.example.com", r);
    TEST_ASSERT_EQUAL_INT(-EINVAL, rc);

    /* nullptr host */
    ioh_vhost_t *v = ioh_vhost_create();
    rc = ioh_vhost_add(v, nullptr, r);
    TEST_ASSERT_EQUAL_INT(-EINVAL, rc);

    /* nullptr router */
    rc = ioh_vhost_add(v, "host.example.com", nullptr);
    TEST_ASSERT_EQUAL_INT(-EINVAL, rc);

    /* Empty host */
    rc = ioh_vhost_add(v, "", r);
    TEST_ASSERT_EQUAL_INT(-EINVAL, rc);

    /* nullptr vhost count */
    TEST_ASSERT_EQUAL_UINT32(0, ioh_vhost_count(nullptr));

    /* nullptr vhost dispatch */
    ioh_route_match_t m = ioh_vhost_dispatch(nullptr, IOH_METHOD_GET, "host", "/", 1);
    TEST_ASSERT_EQUAL_INT(IOH_MATCH_NOT_FOUND, m.status);

    ioh_router_destroy(r);
    ioh_vhost_destroy(v);
}

void test_vhost_dispatch_not_found_path(void)
{
    ioh_vhost_t *v = ioh_vhost_create();
    ioh_router_t *r = make_router();

    int rc = ioh_vhost_add(v, "api.example.com", r);
    TEST_ASSERT_EQUAL_INT(0, rc);

    /* Host matches but path does not exist in router */
    ioh_route_match_t m = ioh_vhost_dispatch(v, IOH_METHOD_GET, "api.example.com", "/nonexistent", 12);
    TEST_ASSERT_EQUAL_INT(IOH_MATCH_NOT_FOUND, m.status);

    ioh_router_destroy(r);
    ioh_vhost_destroy(v);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_vhost_create_destroy);
    RUN_TEST(test_vhost_destroy_null);
    RUN_TEST(test_vhost_exact_match);
    RUN_TEST(test_vhost_wildcard_match);
    RUN_TEST(test_vhost_wildcard_no_match_bare_domain);
    RUN_TEST(test_vhost_default_fallback);
    RUN_TEST(test_vhost_no_host_no_default);
    RUN_TEST(test_vhost_port_stripping);
    RUN_TEST(test_vhost_port_stripping_ipv6);
    RUN_TEST(test_vhost_case_insensitive);
    RUN_TEST(test_vhost_exact_before_wildcard);
    RUN_TEST(test_vhost_max_hosts);
    RUN_TEST(test_vhost_null_inputs);
    RUN_TEST(test_vhost_dispatch_not_found_path);
    return UNITY_END();
}

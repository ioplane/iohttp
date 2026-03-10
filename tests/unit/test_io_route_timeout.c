/**
 * @file test_io_route_timeout.c
 * @brief Unit tests for per-route timeout configuration.
 */

#include "core/io_ctx.h"
#include "http/io_request.h"
#include "http/io_response.h"
#include "router/io_router.h"

#include <string.h>
#include <unity.h>

void setUp(void) {}
void tearDown(void) {}

static int upload_handler(io_ctx_t *c)
{
    (void)c;
    return 0;
}

static int api_handler(io_ctx_t *c)
{
    (void)c;
    return 0;
}

void test_route_opts_timeout_defaults_zero(void)
{
    io_route_opts_t opts = {0};
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
    int rc = io_router_get_with(r, "/api/data", api_handler, &api_opts);
    TEST_ASSERT_EQUAL_INT(0, rc);

    io_route_match_t m = io_router_dispatch(r, IO_METHOD_GET, "/api/data", 9);
    TEST_ASSERT_EQUAL_INT(IO_MATCH_FOUND, m.status);
    TEST_ASSERT_NOT_NULL(m.opts);
    TEST_ASSERT_EQUAL_UINT32(0, m.opts->body_timeout_ms);

    io_router_destroy(r);
}

void test_route_no_opts_returns_null(void)
{
    io_router_t *r = io_router_create();
    TEST_ASSERT_NOT_NULL(r);

    int rc = io_router_get(r, "/simple", api_handler);
    TEST_ASSERT_EQUAL_INT(0, rc);

    io_route_match_t m = io_router_dispatch(r, IO_METHOD_GET, "/simple", 7);
    TEST_ASSERT_EQUAL_INT(IO_MATCH_FOUND, m.status);
    /* opts may be nullptr when registered without _with() */

    io_router_destroy(r);
}

void test_route_keepalive_timeout_override(void)
{
    io_router_t *r = io_router_create();
    TEST_ASSERT_NOT_NULL(r);

    static const io_route_opts_t long_ka_opts = {
        .keepalive_timeout_ms = 120000,
    };
    int rc = io_router_get_with(r, "/stream", api_handler, &long_ka_opts);
    TEST_ASSERT_EQUAL_INT(0, rc);

    io_route_match_t m = io_router_dispatch(r, IO_METHOD_GET, "/stream", 7);
    TEST_ASSERT_EQUAL_INT(IO_MATCH_FOUND, m.status);
    TEST_ASSERT_NOT_NULL(m.opts);
    TEST_ASSERT_EQUAL_UINT32(120000, m.opts->keepalive_timeout_ms);

    io_router_destroy(r);
}

void test_route_all_timeouts_override(void)
{
    io_router_t *r = io_router_create();
    TEST_ASSERT_NOT_NULL(r);

    static const io_route_opts_t opts = {
        .body_timeout_ms = 10000,
        .keepalive_timeout_ms = 60000,
    };
    int rc = io_router_post_with(r, "/all-timeouts", upload_handler, &opts);
    TEST_ASSERT_EQUAL_INT(0, rc);

    io_route_match_t m = io_router_dispatch(r, IO_METHOD_POST, "/all-timeouts", 13);
    TEST_ASSERT_EQUAL_INT(IO_MATCH_FOUND, m.status);
    TEST_ASSERT_NOT_NULL(m.opts);
    TEST_ASSERT_EQUAL_UINT32(10000, m.opts->body_timeout_ms);
    TEST_ASSERT_EQUAL_UINT32(60000, m.opts->keepalive_timeout_ms);

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
    RUN_TEST(test_route_all_timeouts_override);
    return UNITY_END();
}

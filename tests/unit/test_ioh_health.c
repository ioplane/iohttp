/**
 * @file test_io_health.c
 * @brief Unit tests for health check endpoint framework.
 */

#include "core/ioh_health.h"

#include "core/ioh_ctx.h"
#include "core/ioh_server.h"
#include "http/ioh_request.h"
#include "http/ioh_response.h"
#include "router/ioh_router.h"

#include <errno.h>
#include <string.h>

#include <unity.h>

static ioh_router_t *router;

void setUp(void)
{
    router = ioh_router_create();
}

void tearDown(void)
{
    ioh_router_destroy(router);
    router = nullptr;
}

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

    /* nullptr is safe */
    ioh_health_config_init(nullptr);
}

/* ---- Add checker ---- */

static int dummy_check(const char **message, void *user_data)
{
    (void)user_data;
    *message = "all good";
    return 0;
}

void test_health_add_checker(void)
{
    ioh_health_config_t cfg;
    ioh_health_config_init(&cfg);

    int rc = ioh_health_add_checker(&cfg, "test_check", dummy_check, nullptr);
    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_EQUAL_UINT32(1, cfg.checker_count);
    TEST_ASSERT_EQUAL_STRING("test_check", cfg.checkers[0].name);
    TEST_ASSERT_EQUAL_PTR(dummy_check, cfg.checkers[0].check);
    TEST_ASSERT_NULL(cfg.checkers[0].user_data);

    /* nullptr args */
    TEST_ASSERT_EQUAL_INT(-EINVAL, ioh_health_add_checker(nullptr, "x", dummy_check, nullptr));
    TEST_ASSERT_EQUAL_INT(-EINVAL, ioh_health_add_checker(&cfg, nullptr, dummy_check, nullptr));
    TEST_ASSERT_EQUAL_INT(-EINVAL, ioh_health_add_checker(&cfg, "x", nullptr, nullptr));
}

void test_health_add_checker_overflow(void)
{
    ioh_health_config_t cfg;
    ioh_health_config_init(&cfg);

    for (uint32_t i = 0; i < IOH_HEALTH_MAX_CHECKS; i++) {
        int rc = ioh_health_add_checker(&cfg, "chk", dummy_check, nullptr);
        TEST_ASSERT_EQUAL_INT(0, rc);
    }

    /* 9th should fail */
    int rc = ioh_health_add_checker(&cfg, "overflow", dummy_check, nullptr);
    TEST_ASSERT_EQUAL_INT(-ENOSPC, rc);
    TEST_ASSERT_EQUAL_UINT32(IOH_HEALTH_MAX_CHECKS, cfg.checker_count);
}

/* ---- Register creates routes ---- */

void test_health_register_creates_routes(void)
{
    ioh_health_config_t cfg;
    ioh_health_config_init(&cfg);

    int rc = ioh_health_register(router, nullptr, &cfg);
    TEST_ASSERT_EQUAL_INT(0, rc);

    /* Verify routes exist by dispatching */
    ioh_route_match_t m;
    m = ioh_router_dispatch(router, IOH_METHOD_GET, "/health", 7);
    TEST_ASSERT_EQUAL_INT(IOH_MATCH_FOUND, m.status);
    TEST_ASSERT_NOT_NULL(m.handler);

    m = ioh_router_dispatch(router, IOH_METHOD_GET, "/ready", 6);
    TEST_ASSERT_EQUAL_INT(IOH_MATCH_FOUND, m.status);
    TEST_ASSERT_NOT_NULL(m.handler);

    m = ioh_router_dispatch(router, IOH_METHOD_GET, "/live", 5);
    TEST_ASSERT_EQUAL_INT(IOH_MATCH_FOUND, m.status);
    TEST_ASSERT_NOT_NULL(m.handler);
}

/* ---- Disabled config skips registration ---- */

void test_health_disabled_no_routes(void)
{
    ioh_health_config_t cfg;
    ioh_health_config_init(&cfg);
    cfg.enabled = false;

    int rc = ioh_health_register(router, nullptr, &cfg);
    TEST_ASSERT_EQUAL_INT(0, rc);

    ioh_route_match_t m = ioh_router_dispatch(router, IOH_METHOD_GET, "/health", 7);
    TEST_ASSERT_NOT_EQUAL(IOH_MATCH_FOUND, m.status);
}

/* ---- Handler returns 200 ---- */

void test_health_handler_returns_200(void)
{
    ioh_health_config_t cfg;
    ioh_health_config_init(&cfg);

    int rc = ioh_health_register(router, nullptr, &cfg);
    TEST_ASSERT_EQUAL_INT(0, rc);

    ioh_route_match_t m = ioh_router_dispatch(router, IOH_METHOD_GET, "/health", 7);
    TEST_ASSERT_EQUAL_INT(IOH_MATCH_FOUND, m.status);
    TEST_ASSERT_NOT_NULL(m.handler);

    /* Create a minimal context and call the handler */
    ioh_request_t req;
    ioh_request_init(&req);
    ioh_response_t resp;
    TEST_ASSERT_EQUAL_INT(0, ioh_response_init(&resp));

    ioh_ctx_t ctx;
    TEST_ASSERT_EQUAL_INT(0, ioh_ctx_init(&ctx, &req, &resp, nullptr));

    rc = m.handler(&ctx);
    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_EQUAL_UINT16(200, resp.status);

    /* Verify JSON body contains "ok" */
    TEST_ASSERT_GREATER_THAN(0, resp.body_len);
    char body[256];
    size_t copy_len = resp.body_len < sizeof(body) - 1 ? resp.body_len : sizeof(body) - 1;
    memcpy(body, resp.body, copy_len);
    body[copy_len] = '\0';
    TEST_ASSERT_NOT_NULL(strstr(body, "\"ok\""));

    ioh_ctx_destroy(&ctx);
    ioh_response_destroy(&resp);
}

/* ---- Custom paths ---- */

void test_health_custom_paths(void)
{
    ioh_health_config_t cfg;
    ioh_health_config_init(&cfg);
    cfg.health_path = "/healthz";
    cfg.ready_path = "/readyz";
    cfg.live_path = "/livez";

    int rc = ioh_health_register(router, nullptr, &cfg);
    TEST_ASSERT_EQUAL_INT(0, rc);

    ioh_route_match_t m;
    m = ioh_router_dispatch(router, IOH_METHOD_GET, "/healthz", 8);
    TEST_ASSERT_EQUAL_INT(IOH_MATCH_FOUND, m.status);

    m = ioh_router_dispatch(router, IOH_METHOD_GET, "/readyz", 7);
    TEST_ASSERT_EQUAL_INT(IOH_MATCH_FOUND, m.status);

    m = ioh_router_dispatch(router, IOH_METHOD_GET, "/livez", 6);
    TEST_ASSERT_EQUAL_INT(IOH_MATCH_FOUND, m.status);

    /* Default paths should NOT exist */
    m = ioh_router_dispatch(router, IOH_METHOD_GET, "/health", 7);
    TEST_ASSERT_NOT_EQUAL(IOH_MATCH_FOUND, m.status);
}

/* ---- Register with nullptr args ---- */

void test_health_register_null_args(void)
{
    ioh_health_config_t cfg;
    ioh_health_config_init(&cfg);

    TEST_ASSERT_EQUAL_INT(-EINVAL, ioh_health_register(nullptr, nullptr, &cfg));
    TEST_ASSERT_EQUAL_INT(-EINVAL, ioh_health_register(router, nullptr, nullptr));
}

/* ---- Live handler with checker ---- */

static int failing_check(const char **message, void *user_data)
{
    (void)user_data;
    *message = "connection refused";
    return -EIO;
}

void test_health_live_with_checkers(void)
{
    ioh_health_config_t cfg;
    ioh_health_config_init(&cfg);

    TEST_ASSERT_EQUAL_INT(0, ioh_health_add_checker(&cfg, "db", dummy_check, nullptr));
    TEST_ASSERT_EQUAL_INT(0, ioh_health_add_checker(&cfg, "cache", failing_check, nullptr));

    int rc = ioh_health_register(router, nullptr, &cfg);
    TEST_ASSERT_EQUAL_INT(0, rc);

    ioh_route_match_t m = ioh_router_dispatch(router, IOH_METHOD_GET, "/live", 5);
    TEST_ASSERT_EQUAL_INT(IOH_MATCH_FOUND, m.status);

    ioh_request_t req;
    ioh_request_init(&req);
    ioh_response_t resp;
    TEST_ASSERT_EQUAL_INT(0, ioh_response_init(&resp));

    ioh_ctx_t ctx;
    TEST_ASSERT_EQUAL_INT(0, ioh_ctx_init(&ctx, &req, &resp, nullptr));

    rc = m.handler(&ctx);
    TEST_ASSERT_EQUAL_INT(0, rc);
    /* One checker fails, so status should be 503 */
    TEST_ASSERT_EQUAL_UINT16(503, resp.status);

    /* Verify body contains "degraded" */
    char body[1024];
    size_t copy_len = resp.body_len < sizeof(body) - 1 ? resp.body_len : sizeof(body) - 1;
    memcpy(body, resp.body, copy_len);
    body[copy_len] = '\0';
    TEST_ASSERT_NOT_NULL(strstr(body, "degraded"));

    ioh_ctx_destroy(&ctx);
    ioh_response_destroy(&resp);
}

/* ---- Ready handler returns 503 when draining ---- */

void test_health_ready_returns_503_when_draining(void)
{
    ioh_server_config_t srv_cfg;
    ioh_server_config_init(&srv_cfg);
    srv_cfg.listen_port = 9999; /* validation requires > 0 */

    ioh_server_t *srv = ioh_server_create(&srv_cfg);
    TEST_ASSERT_NOT_NULL(srv);

    ioh_health_config_t cfg;
    ioh_health_config_init(&cfg);

    int rc = ioh_health_register(router, srv, &cfg);
    TEST_ASSERT_EQUAL_INT(0, rc);

    /* Before draining: should return 200 */
    ioh_route_match_t m = ioh_router_dispatch(router, IOH_METHOD_GET, "/ready", 6);
    TEST_ASSERT_EQUAL_INT(IOH_MATCH_FOUND, m.status);
    TEST_ASSERT_NOT_NULL(m.handler);

    ioh_request_t req;
    ioh_request_init(&req);
    ioh_response_t resp;
    TEST_ASSERT_EQUAL_INT(0, ioh_response_init(&resp));

    ioh_ctx_t ctx;
    TEST_ASSERT_EQUAL_INT(0, ioh_ctx_init(&ctx, &req, &resp, nullptr));

    rc = m.handler(&ctx);
    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_EQUAL_UINT16(200, resp.status);

    ioh_ctx_destroy(&ctx);
    ioh_response_destroy(&resp);

    /* Set server to draining state */
    ioh_server_stop(srv);
    TEST_ASSERT_TRUE(ioh_server_is_draining(srv));

    /* After draining: should return 503 */
    TEST_ASSERT_EQUAL_INT(0, ioh_response_init(&resp));
    TEST_ASSERT_EQUAL_INT(0, ioh_ctx_init(&ctx, &req, &resp, nullptr));

    rc = m.handler(&ctx);
    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_EQUAL_UINT16(503, resp.status);

    /* Verify body contains "unavailable" */
    char body[256];
    size_t copy_len = resp.body_len < sizeof(body) - 1 ? resp.body_len : sizeof(body) - 1;
    memcpy(body, resp.body, copy_len);
    body[copy_len] = '\0';
    TEST_ASSERT_NOT_NULL(strstr(body, "\"unavailable\""));

    ioh_ctx_destroy(&ctx);
    ioh_response_destroy(&resp);
    ioh_server_destroy(srv);
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
    RUN_TEST(test_health_register_null_args);
    RUN_TEST(test_health_live_with_checkers);
    RUN_TEST(test_health_ready_returns_503_when_draining);

    return UNITY_END();
}

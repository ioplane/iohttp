/**
 * @file test_io_cors.c
 * @brief Unit tests for CORS middleware.
 */

#include "core/ioh_ctx.h"
#include "middleware/ioh_cors.h"

#include <string.h>

#include <unity.h>

/* ---- Test helpers ---- */

static const char *resp_header(const ioh_response_t *resp, const char *name)
{
    size_t name_len = strlen(name);
    for (uint32_t i = 0; i < resp->header_count; i++) {
        if (resp->headers[i].name_len == name_len &&
            strncasecmp(resp->headers[i].name, name, name_len) == 0) {
            return resp->headers[i].value;
        }
    }
    return nullptr;
}

static bool next_called;

static int dummy_next(ioh_ctx_t *c)
{
    (void)c;
    next_called = true;
    return 0;
}

static void set_request_header(ioh_request_t *req, const char *name, const char *value)
{
    uint32_t idx = req->header_count;
    req->headers[idx].name = name;
    req->headers[idx].name_len = strlen(name);
    req->headers[idx].value = value;
    req->headers[idx].value_len = strlen(value);
    req->header_count++;
}

void setUp(void)
{
    next_called = false;
}

void tearDown(void)
{
    ioh_cors_destroy();
}

/* ---- Tests ---- */

void test_cors_preflight_allowed(void)
{
    const char *origins[] = {"https://example.com"};
    ioh_cors_config_t cfg;
    ioh_cors_config_init(&cfg);
    cfg.allowed_origins = origins;
    cfg.origin_count = 1;

    ioh_middleware_fn mw = ioh_cors_create(&cfg);
    TEST_ASSERT_NOT_NULL(mw);

    ioh_request_t req;
    ioh_request_init(&req);
    req.method = IOH_METHOD_OPTIONS;
    set_request_header(&req, "Origin", "https://example.com");

    ioh_response_t resp;
    TEST_ASSERT_EQUAL_INT(0, ioh_response_init(&resp));

    ioh_ctx_t c;
    TEST_ASSERT_EQUAL_INT(0, ioh_ctx_init(&c, &req, &resp, nullptr));

    int rc = mw(&c, dummy_next);
    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_EQUAL_UINT16(204, resp.status);

    const char *acao = resp_header(&resp, "Access-Control-Allow-Origin");
    TEST_ASSERT_NOT_NULL(acao);
    TEST_ASSERT_EQUAL_STRING("https://example.com", acao);

    const char *acam = resp_header(&resp, "Access-Control-Allow-Methods");
    TEST_ASSERT_NOT_NULL(acam);

    const char *acah = resp_header(&resp, "Access-Control-Allow-Headers");
    TEST_ASSERT_NOT_NULL(acah);

    TEST_ASSERT_FALSE(next_called);

    ioh_ctx_destroy(&c);
    ioh_response_destroy(&resp);
}

void test_cors_preflight_denied(void)
{
    const char *origins[] = {"https://allowed.com"};
    ioh_cors_config_t cfg;
    ioh_cors_config_init(&cfg);
    cfg.allowed_origins = origins;
    cfg.origin_count = 1;

    ioh_middleware_fn mw = ioh_cors_create(&cfg);
    TEST_ASSERT_NOT_NULL(mw);

    ioh_request_t req;
    ioh_request_init(&req);
    req.method = IOH_METHOD_OPTIONS;
    set_request_header(&req, "Origin", "https://evil.com");

    ioh_response_t resp;
    TEST_ASSERT_EQUAL_INT(0, ioh_response_init(&resp));

    ioh_ctx_t c;
    TEST_ASSERT_EQUAL_INT(0, ioh_ctx_init(&c, &req, &resp, nullptr));

    int rc = mw(&c, dummy_next);
    TEST_ASSERT_EQUAL_INT(0, rc);

    /* no CORS headers set for denied origin */
    const char *acao = resp_header(&resp, "Access-Control-Allow-Origin");
    TEST_ASSERT_NULL(acao);

    ioh_ctx_destroy(&c);
    ioh_response_destroy(&resp);
}

void test_cors_simple_request_headers(void)
{
    const char *origins[] = {"https://example.com"};
    ioh_cors_config_t cfg;
    ioh_cors_config_init(&cfg);
    cfg.allowed_origins = origins;
    cfg.origin_count = 1;

    ioh_middleware_fn mw = ioh_cors_create(&cfg);
    TEST_ASSERT_NOT_NULL(mw);

    ioh_request_t req;
    ioh_request_init(&req);
    req.method = IOH_METHOD_GET;
    set_request_header(&req, "Origin", "https://example.com");

    ioh_response_t resp;
    TEST_ASSERT_EQUAL_INT(0, ioh_response_init(&resp));

    ioh_ctx_t c;
    TEST_ASSERT_EQUAL_INT(0, ioh_ctx_init(&c, &req, &resp, nullptr));

    int rc = mw(&c, dummy_next);
    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_TRUE(next_called);

    const char *acao = resp_header(&resp, "Access-Control-Allow-Origin");
    TEST_ASSERT_NOT_NULL(acao);
    TEST_ASSERT_EQUAL_STRING("https://example.com", acao);

    ioh_ctx_destroy(&c);
    ioh_response_destroy(&resp);
}

void test_cors_wildcard_origin(void)
{
    ioh_cors_config_t cfg;
    ioh_cors_config_init(&cfg);
    /* no origins → wildcard */

    ioh_middleware_fn mw = ioh_cors_create(&cfg);
    TEST_ASSERT_NOT_NULL(mw);

    ioh_request_t req;
    ioh_request_init(&req);
    req.method = IOH_METHOD_GET;
    set_request_header(&req, "Origin", "https://any-origin.com");

    ioh_response_t resp;
    TEST_ASSERT_EQUAL_INT(0, ioh_response_init(&resp));

    ioh_ctx_t c;
    TEST_ASSERT_EQUAL_INT(0, ioh_ctx_init(&c, &req, &resp, nullptr));

    int rc = mw(&c, dummy_next);
    TEST_ASSERT_EQUAL_INT(0, rc);

    const char *acao = resp_header(&resp, "Access-Control-Allow-Origin");
    TEST_ASSERT_NOT_NULL(acao);
    TEST_ASSERT_EQUAL_STRING("*", acao);

    ioh_ctx_destroy(&c);
    ioh_response_destroy(&resp);
}

void test_cors_credentials(void)
{
    const char *origins[] = {"https://example.com"};
    ioh_cors_config_t cfg;
    ioh_cors_config_init(&cfg);
    cfg.allowed_origins = origins;
    cfg.origin_count = 1;
    cfg.allow_credentials = true;

    ioh_middleware_fn mw = ioh_cors_create(&cfg);
    TEST_ASSERT_NOT_NULL(mw);

    ioh_request_t req;
    ioh_request_init(&req);
    req.method = IOH_METHOD_OPTIONS;
    set_request_header(&req, "Origin", "https://example.com");

    ioh_response_t resp;
    TEST_ASSERT_EQUAL_INT(0, ioh_response_init(&resp));

    ioh_ctx_t c;
    TEST_ASSERT_EQUAL_INT(0, ioh_ctx_init(&c, &req, &resp, nullptr));

    int rc = mw(&c, dummy_next);
    TEST_ASSERT_EQUAL_INT(0, rc);

    /* credentials: echo origin, never "*" */
    const char *acao = resp_header(&resp, "Access-Control-Allow-Origin");
    TEST_ASSERT_NOT_NULL(acao);
    TEST_ASSERT_EQUAL_STRING("https://example.com", acao);

    const char *acac = resp_header(&resp, "Access-Control-Allow-Credentials");
    TEST_ASSERT_NOT_NULL(acac);
    TEST_ASSERT_EQUAL_STRING("true", acac);

    ioh_ctx_destroy(&c);
    ioh_response_destroy(&resp);
}

void test_cors_disabled(void)
{
    /* no CORS middleware installed — just call handler directly */
    ioh_request_t req;
    ioh_request_init(&req);
    req.method = IOH_METHOD_GET;

    ioh_response_t resp;
    TEST_ASSERT_EQUAL_INT(0, ioh_response_init(&resp));

    ioh_ctx_t c;
    TEST_ASSERT_EQUAL_INT(0, ioh_ctx_init(&c, &req, &resp, nullptr));

    int rc = dummy_next(&c);
    TEST_ASSERT_EQUAL_INT(0, rc);

    const char *acao = resp_header(&resp, "Access-Control-Allow-Origin");
    TEST_ASSERT_NULL(acao);

    ioh_ctx_destroy(&c);
    ioh_response_destroy(&resp);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_cors_preflight_allowed);
    RUN_TEST(test_cors_preflight_denied);
    RUN_TEST(test_cors_simple_request_headers);
    RUN_TEST(test_cors_wildcard_origin);
    RUN_TEST(test_cors_credentials);
    RUN_TEST(test_cors_disabled);
    return UNITY_END();
}

/**
 * @file test_io_ratelimit.c
 * @brief Unit tests for rate limiting middleware.
 */

#include "core/ioh_ctx.h"
#include "middleware/ioh_ratelimit.h"

#include <string.h>
#include <time.h>

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
    ioh_ratelimit_destroy();
}

/* ---- Tests ---- */

void test_ratelimit_under_limit(void)
{
    ioh_ratelimit_config_t cfg;
    ioh_ratelimit_config_init(&cfg);
    cfg.requests_per_second = 10;
    cfg.burst = 10;

    ioh_middleware_fn mw = ioh_ratelimit_create(&cfg);
    TEST_ASSERT_NOT_NULL(mw);

    ioh_request_t req;
    ioh_request_init(&req);
    req.method = IOH_METHOD_GET;
    set_request_header(&req, "Host", "localhost");

    ioh_response_t resp;
    TEST_ASSERT_EQUAL_INT(0, ioh_response_init(&resp));

    ioh_ctx_t c;
    TEST_ASSERT_EQUAL_INT(0, ioh_ctx_init(&c, &req, &resp, nullptr));

    int rc = mw(&c, dummy_next);
    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_TRUE(next_called);
    TEST_ASSERT_NOT_EQUAL(429, resp.status);

    ioh_ctx_destroy(&c);
    ioh_response_destroy(&resp);
}

void test_ratelimit_at_limit(void)
{
    ioh_ratelimit_config_t cfg;
    ioh_ratelimit_config_init(&cfg);
    cfg.requests_per_second = 5;
    cfg.burst = 5;

    ioh_middleware_fn mw = ioh_ratelimit_create(&cfg);
    TEST_ASSERT_NOT_NULL(mw);

    /* consume exactly burst tokens */
    for (uint32_t i = 0; i < 5; i++) {
        ioh_request_t req;
        ioh_request_init(&req);
        req.method = IOH_METHOD_GET;
        set_request_header(&req, "Host", "testhost");

        ioh_response_t resp;
        TEST_ASSERT_EQUAL_INT(0, ioh_response_init(&resp));

        ioh_ctx_t c;
        TEST_ASSERT_EQUAL_INT(0, ioh_ctx_init(&c, &req, &resp, nullptr));

        int rc = mw(&c, dummy_next);
        TEST_ASSERT_EQUAL_INT(0, rc);
        TEST_ASSERT_NOT_EQUAL(429, resp.status);

        ioh_ctx_destroy(&c);
        ioh_response_destroy(&resp);
    }
}

void test_ratelimit_over_limit_429(void)
{
    ioh_ratelimit_config_t cfg;
    ioh_ratelimit_config_init(&cfg);
    cfg.requests_per_second = 1;
    cfg.burst = 2;

    ioh_middleware_fn mw = ioh_ratelimit_create(&cfg);
    TEST_ASSERT_NOT_NULL(mw);

    /* exhaust 2 tokens */
    for (int i = 0; i < 2; i++) {
        ioh_request_t req;
        ioh_request_init(&req);
        req.method = IOH_METHOD_GET;
        set_request_header(&req, "Host", "testhost2");

        ioh_response_t resp;
        TEST_ASSERT_EQUAL_INT(0, ioh_response_init(&resp));

        ioh_ctx_t c;
        TEST_ASSERT_EQUAL_INT(0, ioh_ctx_init(&c, &req, &resp, nullptr));

        (void)mw(&c, dummy_next);
        ioh_ctx_destroy(&c);
        ioh_response_destroy(&resp);
    }

    /* third request should be rate limited */
    next_called = false;
    ioh_request_t req;
    ioh_request_init(&req);
    req.method = IOH_METHOD_GET;
    set_request_header(&req, "Host", "testhost2");

    ioh_response_t resp;
    TEST_ASSERT_EQUAL_INT(0, ioh_response_init(&resp));

    ioh_ctx_t c;
    TEST_ASSERT_EQUAL_INT(0, ioh_ctx_init(&c, &req, &resp, nullptr));

    int rc = mw(&c, dummy_next);
    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_EQUAL_UINT16(429, resp.status);
    TEST_ASSERT_FALSE(next_called);

    const char *retry = resp_header(&resp, "Retry-After");
    TEST_ASSERT_NOT_NULL(retry);

    ioh_ctx_destroy(&c);
    ioh_response_destroy(&resp);
}

void test_ratelimit_burst(void)
{
    ioh_ratelimit_config_t cfg;
    ioh_ratelimit_config_init(&cfg);
    cfg.requests_per_second = 1;
    cfg.burst = 5;

    ioh_middleware_fn mw = ioh_ratelimit_create(&cfg);
    TEST_ASSERT_NOT_NULL(mw);

    /* burst allows 5 rapid requests */
    for (int i = 0; i < 5; i++) {
        ioh_request_t req;
        ioh_request_init(&req);
        req.method = IOH_METHOD_GET;
        set_request_header(&req, "Host", "bursthost");

        ioh_response_t resp;
        TEST_ASSERT_EQUAL_INT(0, ioh_response_init(&resp));

        ioh_ctx_t c;
        TEST_ASSERT_EQUAL_INT(0, ioh_ctx_init(&c, &req, &resp, nullptr));

        next_called = false;
        int rc = mw(&c, dummy_next);
        TEST_ASSERT_EQUAL_INT(0, rc);
        TEST_ASSERT_TRUE(next_called);

        ioh_ctx_destroy(&c);
        ioh_response_destroy(&resp);
    }

    /* 6th should fail */
    ioh_request_t req;
    ioh_request_init(&req);
    req.method = IOH_METHOD_GET;
    set_request_header(&req, "Host", "bursthost");

    ioh_response_t resp;
    TEST_ASSERT_EQUAL_INT(0, ioh_response_init(&resp));

    ioh_ctx_t c;
    TEST_ASSERT_EQUAL_INT(0, ioh_ctx_init(&c, &req, &resp, nullptr));

    next_called = false;
    int rc = mw(&c, dummy_next);
    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_EQUAL_UINT16(429, resp.status);
    TEST_ASSERT_FALSE(next_called);

    ioh_ctx_destroy(&c);
    ioh_response_destroy(&resp);
}

void test_ratelimit_refill(void)
{
    ioh_ratelimit_config_t cfg;
    ioh_ratelimit_config_init(&cfg);
    cfg.requests_per_second = 1000; /* high rate for fast refill */
    cfg.burst = 1;

    ioh_middleware_fn mw = ioh_ratelimit_create(&cfg);
    TEST_ASSERT_NOT_NULL(mw);

    /* consume the 1 token */
    ioh_request_t req;
    ioh_request_init(&req);
    req.method = IOH_METHOD_GET;
    set_request_header(&req, "Host", "refillhost");

    ioh_response_t resp;
    TEST_ASSERT_EQUAL_INT(0, ioh_response_init(&resp));

    ioh_ctx_t c;
    TEST_ASSERT_EQUAL_INT(0, ioh_ctx_init(&c, &req, &resp, nullptr));
    (void)mw(&c, dummy_next);
    ioh_ctx_destroy(&c);
    ioh_response_destroy(&resp);

    /* wait for refill (2ms should give 2 tokens at 1000/s) */
    struct timespec ts = {.tv_sec = 0, .tv_nsec = 2000000};
    nanosleep(&ts, nullptr);

    /* should work now */
    ioh_request_init(&req);
    req.method = IOH_METHOD_GET;
    set_request_header(&req, "Host", "refillhost");

    TEST_ASSERT_EQUAL_INT(0, ioh_response_init(&resp));

    TEST_ASSERT_EQUAL_INT(0, ioh_ctx_init(&c, &req, &resp, nullptr));

    next_called = false;
    int rc = mw(&c, dummy_next);
    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_TRUE(next_called);

    ioh_ctx_destroy(&c);
    ioh_response_destroy(&resp);
}

void test_ratelimit_check_api(void)
{
    ioh_ratelimit_config_t cfg;
    ioh_ratelimit_config_init(&cfg);
    cfg.requests_per_second = 1;
    cfg.burst = 3;

    (void)ioh_ratelimit_create(&cfg);

    TEST_ASSERT_TRUE(ioh_ratelimit_check("testkey"));
    TEST_ASSERT_TRUE(ioh_ratelimit_check("testkey"));
    TEST_ASSERT_TRUE(ioh_ratelimit_check("testkey"));
    TEST_ASSERT_FALSE(ioh_ratelimit_check("testkey"));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_ratelimit_under_limit);
    RUN_TEST(test_ratelimit_at_limit);
    RUN_TEST(test_ratelimit_over_limit_429);
    RUN_TEST(test_ratelimit_burst);
    RUN_TEST(test_ratelimit_refill);
    RUN_TEST(test_ratelimit_check_api);
    return UNITY_END();
}

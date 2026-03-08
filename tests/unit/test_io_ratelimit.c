/**
 * @file test_io_ratelimit.c
 * @brief Unit tests for rate limiting middleware.
 */

#include "middleware/io_ratelimit.h"

#include <string.h>
#include <time.h>

#include <unity.h>

/* ---- Test helpers ---- */

static const char *resp_header(const io_response_t *resp, const char *name)
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

static int dummy_next(io_request_t *req, io_response_t *resp)
{
    (void)req;
    (void)resp;
    next_called = true;
    return 0;
}

static void set_request_header(io_request_t *req, const char *name,
                               const char *value)
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
    io_ratelimit_destroy();
}

/* ---- Tests ---- */

void test_ratelimit_under_limit(void)
{
    io_ratelimit_config_t cfg;
    io_ratelimit_config_init(&cfg);
    cfg.requests_per_second = 10;
    cfg.burst = 10;

    io_middleware_fn mw = io_ratelimit_create(&cfg);
    TEST_ASSERT_NOT_NULL(mw);

    io_request_t req;
    io_request_init(&req);
    req.method = IO_METHOD_GET;
    set_request_header(&req, "Host", "localhost");

    io_response_t resp;
    TEST_ASSERT_EQUAL_INT(0, io_response_init(&resp));

    int rc = mw(&req, &resp, dummy_next);
    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_TRUE(next_called);
    TEST_ASSERT_NOT_EQUAL(429, resp.status);

    io_response_destroy(&resp);
}

void test_ratelimit_at_limit(void)
{
    io_ratelimit_config_t cfg;
    io_ratelimit_config_init(&cfg);
    cfg.requests_per_second = 5;
    cfg.burst = 5;

    io_middleware_fn mw = io_ratelimit_create(&cfg);
    TEST_ASSERT_NOT_NULL(mw);

    /* consume exactly burst tokens */
    for (uint32_t i = 0; i < 5; i++) {
        io_request_t req;
        io_request_init(&req);
        req.method = IO_METHOD_GET;
        set_request_header(&req, "Host", "testhost");

        io_response_t resp;
        TEST_ASSERT_EQUAL_INT(0, io_response_init(&resp));

        int rc = mw(&req, &resp, dummy_next);
        TEST_ASSERT_EQUAL_INT(0, rc);
        TEST_ASSERT_NOT_EQUAL(429, resp.status);

        io_response_destroy(&resp);
    }
}

void test_ratelimit_over_limit_429(void)
{
    io_ratelimit_config_t cfg;
    io_ratelimit_config_init(&cfg);
    cfg.requests_per_second = 1;
    cfg.burst = 2;

    io_middleware_fn mw = io_ratelimit_create(&cfg);
    TEST_ASSERT_NOT_NULL(mw);

    /* exhaust 2 tokens */
    for (int i = 0; i < 2; i++) {
        io_request_t req;
        io_request_init(&req);
        req.method = IO_METHOD_GET;
        set_request_header(&req, "Host", "testhost2");

        io_response_t resp;
        TEST_ASSERT_EQUAL_INT(0, io_response_init(&resp));

        (void)mw(&req, &resp, dummy_next);
        io_response_destroy(&resp);
    }

    /* third request should be rate limited */
    next_called = false;
    io_request_t req;
    io_request_init(&req);
    req.method = IO_METHOD_GET;
    set_request_header(&req, "Host", "testhost2");

    io_response_t resp;
    TEST_ASSERT_EQUAL_INT(0, io_response_init(&resp));

    int rc = mw(&req, &resp, dummy_next);
    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_EQUAL_UINT16(429, resp.status);
    TEST_ASSERT_FALSE(next_called);

    const char *retry = resp_header(&resp, "Retry-After");
    TEST_ASSERT_NOT_NULL(retry);

    io_response_destroy(&resp);
}

void test_ratelimit_burst(void)
{
    io_ratelimit_config_t cfg;
    io_ratelimit_config_init(&cfg);
    cfg.requests_per_second = 1;
    cfg.burst = 5;

    io_middleware_fn mw = io_ratelimit_create(&cfg);
    TEST_ASSERT_NOT_NULL(mw);

    /* burst allows 5 rapid requests */
    for (int i = 0; i < 5; i++) {
        io_request_t req;
        io_request_init(&req);
        req.method = IO_METHOD_GET;
        set_request_header(&req, "Host", "bursthost");

        io_response_t resp;
        TEST_ASSERT_EQUAL_INT(0, io_response_init(&resp));

        next_called = false;
        int rc = mw(&req, &resp, dummy_next);
        TEST_ASSERT_EQUAL_INT(0, rc);
        TEST_ASSERT_TRUE(next_called);

        io_response_destroy(&resp);
    }

    /* 6th should fail */
    io_request_t req;
    io_request_init(&req);
    req.method = IO_METHOD_GET;
    set_request_header(&req, "Host", "bursthost");

    io_response_t resp;
    TEST_ASSERT_EQUAL_INT(0, io_response_init(&resp));

    next_called = false;
    int rc = mw(&req, &resp, dummy_next);
    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_EQUAL_UINT16(429, resp.status);
    TEST_ASSERT_FALSE(next_called);

    io_response_destroy(&resp);
}

void test_ratelimit_refill(void)
{
    io_ratelimit_config_t cfg;
    io_ratelimit_config_init(&cfg);
    cfg.requests_per_second = 1000;  /* high rate for fast refill */
    cfg.burst = 1;

    io_middleware_fn mw = io_ratelimit_create(&cfg);
    TEST_ASSERT_NOT_NULL(mw);

    /* consume the 1 token */
    io_request_t req;
    io_request_init(&req);
    req.method = IO_METHOD_GET;
    set_request_header(&req, "Host", "refillhost");

    io_response_t resp;
    TEST_ASSERT_EQUAL_INT(0, io_response_init(&resp));
    (void)mw(&req, &resp, dummy_next);
    io_response_destroy(&resp);

    /* wait for refill (2ms should give 2 tokens at 1000/s) */
    struct timespec ts = {.tv_sec = 0, .tv_nsec = 2000000};
    nanosleep(&ts, nullptr);

    /* should work now */
    io_request_init(&req);
    req.method = IO_METHOD_GET;
    set_request_header(&req, "Host", "refillhost");

    TEST_ASSERT_EQUAL_INT(0, io_response_init(&resp));

    next_called = false;
    int rc = mw(&req, &resp, dummy_next);
    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_TRUE(next_called);

    io_response_destroy(&resp);
}

void test_ratelimit_check_api(void)
{
    io_ratelimit_config_t cfg;
    io_ratelimit_config_init(&cfg);
    cfg.requests_per_second = 1;
    cfg.burst = 3;

    (void)io_ratelimit_create(&cfg);

    TEST_ASSERT_TRUE(io_ratelimit_check("testkey"));
    TEST_ASSERT_TRUE(io_ratelimit_check("testkey"));
    TEST_ASSERT_TRUE(io_ratelimit_check("testkey"));
    TEST_ASSERT_FALSE(io_ratelimit_check("testkey"));
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

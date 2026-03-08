/**
 * @file test_io_security.c
 * @brief Unit tests for security headers middleware.
 */

#include "middleware/io_security.h"

#include <string.h>

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

static int dummy_next(io_request_t *req, io_response_t *resp)
{
    (void)req;
    (void)resp;
    return 0;
}

void setUp(void)
{
}

void tearDown(void)
{
    io_security_destroy();
}

/* ---- Tests ---- */

void test_security_csp_header(void)
{
    io_security_config_t cfg;
    io_security_config_init(&cfg);
    cfg.csp = "default-src 'self'";
    cfg.hsts = false;
    cfg.frame_options = nullptr;
    cfg.referrer_policy = nullptr;
    cfg.nosniff = false;

    io_middleware_fn mw = io_security_create(&cfg);
    TEST_ASSERT_NOT_NULL(mw);

    io_request_t req;
    io_request_init(&req);

    io_response_t resp;
    TEST_ASSERT_EQUAL_INT(0, io_response_init(&resp));

    int rc = mw(&req, &resp, dummy_next);
    TEST_ASSERT_EQUAL_INT(0, rc);

    const char *csp = resp_header(&resp, "Content-Security-Policy");
    TEST_ASSERT_NOT_NULL(csp);
    TEST_ASSERT_EQUAL_STRING("default-src 'self'", csp);

    io_response_destroy(&resp);
}

void test_security_hsts_header(void)
{
    io_security_config_t cfg;
    io_security_config_init(&cfg);
    cfg.csp = nullptr;
    cfg.hsts = true;
    cfg.hsts_max_age = 31536000;
    cfg.frame_options = nullptr;
    cfg.referrer_policy = nullptr;
    cfg.nosniff = false;

    io_middleware_fn mw = io_security_create(&cfg);
    TEST_ASSERT_NOT_NULL(mw);

    io_request_t req;
    io_request_init(&req);

    io_response_t resp;
    TEST_ASSERT_EQUAL_INT(0, io_response_init(&resp));

    int rc = mw(&req, &resp, dummy_next);
    TEST_ASSERT_EQUAL_INT(0, rc);

    const char *hsts = resp_header(&resp, "Strict-Transport-Security");
    TEST_ASSERT_NOT_NULL(hsts);
    TEST_ASSERT_EQUAL_STRING("max-age=31536000; includeSubDomains", hsts);

    io_response_destroy(&resp);
}

void test_security_frame_options(void)
{
    io_security_config_t cfg;
    io_security_config_init(&cfg);
    cfg.csp = nullptr;
    cfg.hsts = false;
    cfg.frame_options = "DENY";
    cfg.referrer_policy = nullptr;
    cfg.nosniff = false;

    io_middleware_fn mw = io_security_create(&cfg);
    TEST_ASSERT_NOT_NULL(mw);

    io_request_t req;
    io_request_init(&req);

    io_response_t resp;
    TEST_ASSERT_EQUAL_INT(0, io_response_init(&resp));

    int rc = mw(&req, &resp, dummy_next);
    TEST_ASSERT_EQUAL_INT(0, rc);

    const char *fo = resp_header(&resp, "X-Frame-Options");
    TEST_ASSERT_NOT_NULL(fo);
    TEST_ASSERT_EQUAL_STRING("DENY", fo);

    io_response_destroy(&resp);
}

void test_security_nosniff(void)
{
    io_security_config_t cfg;
    io_security_config_init(&cfg);
    cfg.csp = nullptr;
    cfg.hsts = false;
    cfg.frame_options = nullptr;
    cfg.referrer_policy = nullptr;
    cfg.nosniff = true;

    io_middleware_fn mw = io_security_create(&cfg);
    TEST_ASSERT_NOT_NULL(mw);

    io_request_t req;
    io_request_init(&req);

    io_response_t resp;
    TEST_ASSERT_EQUAL_INT(0, io_response_init(&resp));

    int rc = mw(&req, &resp, dummy_next);
    TEST_ASSERT_EQUAL_INT(0, rc);

    const char *ns = resp_header(&resp, "X-Content-Type-Options");
    TEST_ASSERT_NOT_NULL(ns);
    TEST_ASSERT_EQUAL_STRING("nosniff", ns);

    io_response_destroy(&resp);
}

void test_security_referrer_policy(void)
{
    io_security_config_t cfg;
    io_security_config_init(&cfg);
    cfg.csp = nullptr;
    cfg.hsts = false;
    cfg.frame_options = nullptr;
    cfg.referrer_policy = "no-referrer";
    cfg.nosniff = false;

    io_middleware_fn mw = io_security_create(&cfg);
    TEST_ASSERT_NOT_NULL(mw);

    io_request_t req;
    io_request_init(&req);

    io_response_t resp;
    TEST_ASSERT_EQUAL_INT(0, io_response_init(&resp));

    int rc = mw(&req, &resp, dummy_next);
    TEST_ASSERT_EQUAL_INT(0, rc);

    const char *rp = resp_header(&resp, "Referrer-Policy");
    TEST_ASSERT_NOT_NULL(rp);
    TEST_ASSERT_EQUAL_STRING("no-referrer", rp);

    io_response_destroy(&resp);
}

void test_security_all_headers(void)
{
    io_security_config_t cfg;
    io_security_config_init(&cfg);
    cfg.csp = "default-src 'none'";
    /* defaults already have hsts, frame_options, referrer_policy, nosniff */

    io_middleware_fn mw = io_security_create(&cfg);
    TEST_ASSERT_NOT_NULL(mw);

    io_request_t req;
    io_request_init(&req);

    io_response_t resp;
    TEST_ASSERT_EQUAL_INT(0, io_response_init(&resp));

    int rc = mw(&req, &resp, dummy_next);
    TEST_ASSERT_EQUAL_INT(0, rc);

    TEST_ASSERT_NOT_NULL(resp_header(&resp, "Content-Security-Policy"));
    TEST_ASSERT_NOT_NULL(resp_header(&resp, "Strict-Transport-Security"));
    TEST_ASSERT_NOT_NULL(resp_header(&resp, "X-Frame-Options"));
    TEST_ASSERT_NOT_NULL(resp_header(&resp, "Referrer-Policy"));
    TEST_ASSERT_NOT_NULL(resp_header(&resp, "X-Content-Type-Options"));

    io_response_destroy(&resp);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_security_csp_header);
    RUN_TEST(test_security_hsts_header);
    RUN_TEST(test_security_frame_options);
    RUN_TEST(test_security_nosniff);
    RUN_TEST(test_security_referrer_policy);
    RUN_TEST(test_security_all_headers);
    return UNITY_END();
}

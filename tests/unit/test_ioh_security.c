/**
 * @file test_io_security.c
 * @brief Unit tests for security headers middleware.
 */

#include "core/ioh_ctx.h"
#include "middleware/ioh_security.h"

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

static int dummy_next(ioh_ctx_t *c)
{
    (void)c;
    return 0;
}

void setUp(void)
{
}

void tearDown(void)
{
    ioh_security_destroy();
}

/* ---- Tests ---- */

void test_security_csp_header(void)
{
    ioh_security_config_t cfg;
    ioh_security_config_init(&cfg);
    cfg.csp = "default-src 'self'";
    cfg.hsts = false;
    cfg.frame_options = nullptr;
    cfg.referrer_policy = nullptr;
    cfg.nosniff = false;

    ioh_middleware_fn mw = ioh_security_create(&cfg);
    TEST_ASSERT_NOT_NULL(mw);

    ioh_request_t req;
    ioh_request_init(&req);

    ioh_response_t resp;
    TEST_ASSERT_EQUAL_INT(0, ioh_response_init(&resp));

    ioh_ctx_t c;
    TEST_ASSERT_EQUAL_INT(0, ioh_ctx_init(&c, &req, &resp, nullptr));

    int rc = mw(&c, dummy_next);
    TEST_ASSERT_EQUAL_INT(0, rc);

    const char *csp = resp_header(&resp, "Content-Security-Policy");
    TEST_ASSERT_NOT_NULL(csp);
    TEST_ASSERT_EQUAL_STRING("default-src 'self'", csp);

    ioh_ctx_destroy(&c);
    ioh_response_destroy(&resp);
}

void test_security_hsts_header(void)
{
    ioh_security_config_t cfg;
    ioh_security_config_init(&cfg);
    cfg.csp = nullptr;
    cfg.hsts = true;
    cfg.hsts_max_age = 31536000;
    cfg.frame_options = nullptr;
    cfg.referrer_policy = nullptr;
    cfg.nosniff = false;

    ioh_middleware_fn mw = ioh_security_create(&cfg);
    TEST_ASSERT_NOT_NULL(mw);

    ioh_request_t req;
    ioh_request_init(&req);

    ioh_response_t resp;
    TEST_ASSERT_EQUAL_INT(0, ioh_response_init(&resp));

    ioh_ctx_t c;
    TEST_ASSERT_EQUAL_INT(0, ioh_ctx_init(&c, &req, &resp, nullptr));

    int rc = mw(&c, dummy_next);
    TEST_ASSERT_EQUAL_INT(0, rc);

    const char *hsts = resp_header(&resp, "Strict-Transport-Security");
    TEST_ASSERT_NOT_NULL(hsts);
    TEST_ASSERT_EQUAL_STRING("max-age=31536000; includeSubDomains", hsts);

    ioh_ctx_destroy(&c);
    ioh_response_destroy(&resp);
}

void test_security_frame_options(void)
{
    ioh_security_config_t cfg;
    ioh_security_config_init(&cfg);
    cfg.csp = nullptr;
    cfg.hsts = false;
    cfg.frame_options = "DENY";
    cfg.referrer_policy = nullptr;
    cfg.nosniff = false;

    ioh_middleware_fn mw = ioh_security_create(&cfg);
    TEST_ASSERT_NOT_NULL(mw);

    ioh_request_t req;
    ioh_request_init(&req);

    ioh_response_t resp;
    TEST_ASSERT_EQUAL_INT(0, ioh_response_init(&resp));

    ioh_ctx_t c;
    TEST_ASSERT_EQUAL_INT(0, ioh_ctx_init(&c, &req, &resp, nullptr));

    int rc = mw(&c, dummy_next);
    TEST_ASSERT_EQUAL_INT(0, rc);

    const char *fo = resp_header(&resp, "X-Frame-Options");
    TEST_ASSERT_NOT_NULL(fo);
    TEST_ASSERT_EQUAL_STRING("DENY", fo);

    ioh_ctx_destroy(&c);
    ioh_response_destroy(&resp);
}

void test_security_nosniff(void)
{
    ioh_security_config_t cfg;
    ioh_security_config_init(&cfg);
    cfg.csp = nullptr;
    cfg.hsts = false;
    cfg.frame_options = nullptr;
    cfg.referrer_policy = nullptr;
    cfg.nosniff = true;

    ioh_middleware_fn mw = ioh_security_create(&cfg);
    TEST_ASSERT_NOT_NULL(mw);

    ioh_request_t req;
    ioh_request_init(&req);

    ioh_response_t resp;
    TEST_ASSERT_EQUAL_INT(0, ioh_response_init(&resp));

    ioh_ctx_t c;
    TEST_ASSERT_EQUAL_INT(0, ioh_ctx_init(&c, &req, &resp, nullptr));

    int rc = mw(&c, dummy_next);
    TEST_ASSERT_EQUAL_INT(0, rc);

    const char *ns = resp_header(&resp, "X-Content-Type-Options");
    TEST_ASSERT_NOT_NULL(ns);
    TEST_ASSERT_EQUAL_STRING("nosniff", ns);

    ioh_ctx_destroy(&c);
    ioh_response_destroy(&resp);
}

void test_security_referrer_policy(void)
{
    ioh_security_config_t cfg;
    ioh_security_config_init(&cfg);
    cfg.csp = nullptr;
    cfg.hsts = false;
    cfg.frame_options = nullptr;
    cfg.referrer_policy = "no-referrer";
    cfg.nosniff = false;

    ioh_middleware_fn mw = ioh_security_create(&cfg);
    TEST_ASSERT_NOT_NULL(mw);

    ioh_request_t req;
    ioh_request_init(&req);

    ioh_response_t resp;
    TEST_ASSERT_EQUAL_INT(0, ioh_response_init(&resp));

    ioh_ctx_t c;
    TEST_ASSERT_EQUAL_INT(0, ioh_ctx_init(&c, &req, &resp, nullptr));

    int rc = mw(&c, dummy_next);
    TEST_ASSERT_EQUAL_INT(0, rc);

    const char *rp = resp_header(&resp, "Referrer-Policy");
    TEST_ASSERT_NOT_NULL(rp);
    TEST_ASSERT_EQUAL_STRING("no-referrer", rp);

    ioh_ctx_destroy(&c);
    ioh_response_destroy(&resp);
}

void test_security_all_headers(void)
{
    ioh_security_config_t cfg;
    ioh_security_config_init(&cfg);
    cfg.csp = "default-src 'none'";
    /* defaults already have hsts, frame_options, referrer_policy, nosniff */

    ioh_middleware_fn mw = ioh_security_create(&cfg);
    TEST_ASSERT_NOT_NULL(mw);

    ioh_request_t req;
    ioh_request_init(&req);

    ioh_response_t resp;
    TEST_ASSERT_EQUAL_INT(0, ioh_response_init(&resp));

    ioh_ctx_t c;
    TEST_ASSERT_EQUAL_INT(0, ioh_ctx_init(&c, &req, &resp, nullptr));

    int rc = mw(&c, dummy_next);
    TEST_ASSERT_EQUAL_INT(0, rc);

    TEST_ASSERT_NOT_NULL(resp_header(&resp, "Content-Security-Policy"));
    TEST_ASSERT_NOT_NULL(resp_header(&resp, "Strict-Transport-Security"));
    TEST_ASSERT_NOT_NULL(resp_header(&resp, "X-Frame-Options"));
    TEST_ASSERT_NOT_NULL(resp_header(&resp, "Referrer-Policy"));
    TEST_ASSERT_NOT_NULL(resp_header(&resp, "X-Content-Type-Options"));

    ioh_ctx_destroy(&c);
    ioh_response_destroy(&resp);
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

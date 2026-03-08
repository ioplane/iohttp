/**
 * @file test_io_auth.c
 * @brief Unit tests for auth middleware (Basic and Bearer).
 */

#include "core/io_ctx.h"
#include "middleware/io_auth.h"

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

static bool next_called;

static int dummy_next(io_ctx_t *c)
{
    (void)c;
    next_called = true;
    return 0;
}

static void set_request_header(io_request_t *req, const char *name, const char *value)
{
    uint32_t idx = req->header_count;
    req->headers[idx].name = name;
    req->headers[idx].name_len = strlen(name);
    req->headers[idx].value = value;
    req->headers[idx].value_len = strlen(value);
    req->header_count++;
}

/* verify callback: accepts "admin:secret" for basic, "valid-token" for bearer */
static bool basic_verifier(const char *credentials, void *ctx)
{
    (void)ctx;
    return strcmp(credentials, "admin:secret") == 0;
}

static bool bearer_verifier(const char *token, void *ctx)
{
    (void)ctx;
    return strcmp(token, "valid-token") == 0;
}

void setUp(void)
{
    next_called = false;
}

void tearDown(void)
{
    io_auth_destroy();
}

/* ---- Tests ---- */

void test_auth_basic_valid(void)
{
    io_middleware_fn mw = io_auth_basic_create(basic_verifier, nullptr);
    TEST_ASSERT_NOT_NULL(mw);

    io_request_t req;
    io_request_init(&req);
    req.method = IO_METHOD_GET;
    /* "admin:secret" -> base64 "YWRtaW46c2VjcmV0" */
    set_request_header(&req, "Authorization", "Basic YWRtaW46c2VjcmV0");

    io_response_t resp;
    TEST_ASSERT_EQUAL_INT(0, io_response_init(&resp));

    io_ctx_t c;
    TEST_ASSERT_EQUAL_INT(0, io_ctx_init(&c, &req, &resp, nullptr));

    int rc = mw(&c, dummy_next);
    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_TRUE(next_called);

    io_ctx_destroy(&c);
    io_response_destroy(&resp);
}

void test_auth_basic_invalid(void)
{
    io_middleware_fn mw = io_auth_basic_create(basic_verifier, nullptr);
    TEST_ASSERT_NOT_NULL(mw);

    io_request_t req;
    io_request_init(&req);
    req.method = IO_METHOD_GET;
    /* "admin:wrong" -> base64 "YWRtaW46d3Jvbmc=" */
    set_request_header(&req, "Authorization", "Basic YWRtaW46d3Jvbmc=");

    io_response_t resp;
    TEST_ASSERT_EQUAL_INT(0, io_response_init(&resp));

    io_ctx_t c;
    TEST_ASSERT_EQUAL_INT(0, io_ctx_init(&c, &req, &resp, nullptr));

    int rc = mw(&c, dummy_next);
    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_EQUAL_UINT16(401, resp.status);
    TEST_ASSERT_FALSE(next_called);

    const char *www = resp_header(&resp, "WWW-Authenticate");
    TEST_ASSERT_NOT_NULL(www);

    io_ctx_destroy(&c);
    io_response_destroy(&resp);
}

void test_auth_basic_missing(void)
{
    io_middleware_fn mw = io_auth_basic_create(basic_verifier, nullptr);
    TEST_ASSERT_NOT_NULL(mw);

    io_request_t req;
    io_request_init(&req);
    req.method = IO_METHOD_GET;
    /* no Authorization header */

    io_response_t resp;
    TEST_ASSERT_EQUAL_INT(0, io_response_init(&resp));

    io_ctx_t c;
    TEST_ASSERT_EQUAL_INT(0, io_ctx_init(&c, &req, &resp, nullptr));

    int rc = mw(&c, dummy_next);
    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_EQUAL_UINT16(401, resp.status);
    TEST_ASSERT_FALSE(next_called);

    const char *www = resp_header(&resp, "WWW-Authenticate");
    TEST_ASSERT_NOT_NULL(www);
    TEST_ASSERT_EQUAL_STRING("Basic realm=\"iohttp\"", www);

    io_ctx_destroy(&c);
    io_response_destroy(&resp);
}

void test_auth_bearer_valid(void)
{
    io_middleware_fn mw = io_auth_bearer_create(bearer_verifier, nullptr);
    TEST_ASSERT_NOT_NULL(mw);

    io_request_t req;
    io_request_init(&req);
    req.method = IO_METHOD_GET;
    set_request_header(&req, "Authorization", "Bearer valid-token");

    io_response_t resp;
    TEST_ASSERT_EQUAL_INT(0, io_response_init(&resp));

    io_ctx_t c;
    TEST_ASSERT_EQUAL_INT(0, io_ctx_init(&c, &req, &resp, nullptr));

    int rc = mw(&c, dummy_next);
    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_TRUE(next_called);

    io_ctx_destroy(&c);
    io_response_destroy(&resp);
}

void test_auth_bearer_invalid(void)
{
    io_middleware_fn mw = io_auth_bearer_create(bearer_verifier, nullptr);
    TEST_ASSERT_NOT_NULL(mw);

    io_request_t req;
    io_request_init(&req);
    req.method = IO_METHOD_GET;
    set_request_header(&req, "Authorization", "Bearer bad-token");

    io_response_t resp;
    TEST_ASSERT_EQUAL_INT(0, io_response_init(&resp));

    io_ctx_t c;
    TEST_ASSERT_EQUAL_INT(0, io_ctx_init(&c, &req, &resp, nullptr));

    int rc = mw(&c, dummy_next);
    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_EQUAL_UINT16(401, resp.status);
    TEST_ASSERT_FALSE(next_called);

    io_ctx_destroy(&c);
    io_response_destroy(&resp);
}

void test_auth_bearer_missing(void)
{
    io_middleware_fn mw = io_auth_bearer_create(bearer_verifier, nullptr);
    TEST_ASSERT_NOT_NULL(mw);

    io_request_t req;
    io_request_init(&req);
    req.method = IO_METHOD_GET;

    io_response_t resp;
    TEST_ASSERT_EQUAL_INT(0, io_response_init(&resp));

    io_ctx_t c;
    TEST_ASSERT_EQUAL_INT(0, io_ctx_init(&c, &req, &resp, nullptr));

    int rc = mw(&c, dummy_next);
    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_EQUAL_UINT16(401, resp.status);
    TEST_ASSERT_FALSE(next_called);

    const char *www = resp_header(&resp, "WWW-Authenticate");
    TEST_ASSERT_NOT_NULL(www);
    TEST_ASSERT_EQUAL_STRING("Bearer", www);

    io_ctx_destroy(&c);
    io_response_destroy(&resp);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_auth_basic_valid);
    RUN_TEST(test_auth_basic_invalid);
    RUN_TEST(test_auth_basic_missing);
    RUN_TEST(test_auth_bearer_valid);
    RUN_TEST(test_auth_bearer_invalid);
    RUN_TEST(test_auth_bearer_missing);
    return UNITY_END();
}

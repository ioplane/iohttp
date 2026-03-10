/**
 * @file test_io_cookie.c
 * @brief Unit tests for Set-Cookie response builder (RFC 6265bis).
 */

#include "core/ioh_ctx.h"
#include "http/ioh_cookie.h"

#include <errno.h>
#include <string.h>

#include <unity.h>

static ioh_request_t req;
static ioh_response_t resp;

void setUp(void)
{
    ioh_request_init(&req);
    TEST_ASSERT_EQUAL_INT(0, ioh_response_init(&resp));
}

void tearDown(void)
{
    ioh_response_destroy(&resp);
}

/* ---- ioh_cookie_serialize ---- */

void test_cookie_serialize_simple(void)
{
    ioh_cookie_t cookie = {
        .name = "sid",
        .value = "abc123",
        .max_age = -1,
        .same_site = IOH_SAME_SITE_DEFAULT,
    };

    char buf[256];
    int n = ioh_cookie_serialize(&cookie, buf, sizeof(buf));
    TEST_ASSERT_GREATER_THAN(0, n);
    TEST_ASSERT_EQUAL_STRING("sid=abc123", buf);
}

void test_cookie_serialize_full_attributes(void)
{
    ioh_cookie_t cookie = {
        .name = "session",
        .value = "xyz789",
        .domain = "example.com",
        .path = "/app",
        .max_age = 3600,
        .same_site = IOH_SAME_SITE_LAX,
        .secure = true,
        .http_only = true,
    };

    char buf[512];
    int n = ioh_cookie_serialize(&cookie, buf, sizeof(buf));
    TEST_ASSERT_GREATER_THAN(0, n);

    TEST_ASSERT_NOT_NULL(strstr(buf, "session=xyz789"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "; Domain=example.com"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "; Path=/app"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "; Max-Age=3600"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "; SameSite=Lax"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "; Secure"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "; HttpOnly"));
}

void test_cookie_serialize_same_site_lax(void)
{
    ioh_cookie_t cookie = {
        .name = "t",
        .value = "1",
        .max_age = -1,
        .same_site = IOH_SAME_SITE_LAX,
    };

    char buf[256];
    int n = ioh_cookie_serialize(&cookie, buf, sizeof(buf));
    TEST_ASSERT_GREATER_THAN(0, n);
    TEST_ASSERT_NOT_NULL(strstr(buf, "; SameSite=Lax"));
}

void test_cookie_serialize_same_site_strict(void)
{
    ioh_cookie_t cookie = {
        .name = "t",
        .value = "1",
        .max_age = -1,
        .same_site = IOH_SAME_SITE_STRICT,
    };

    char buf[256];
    int n = ioh_cookie_serialize(&cookie, buf, sizeof(buf));
    TEST_ASSERT_GREATER_THAN(0, n);
    TEST_ASSERT_NOT_NULL(strstr(buf, "; SameSite=Strict"));
}

void test_cookie_serialize_same_site_none_forces_secure(void)
{
    ioh_cookie_t cookie = {
        .name = "t",
        .value = "1",
        .max_age = -1,
        .same_site = IOH_SAME_SITE_NONE,
        .secure = false, /* must be forced to true */
    };

    char buf[256];
    int n = ioh_cookie_serialize(&cookie, buf, sizeof(buf));
    TEST_ASSERT_GREATER_THAN(0, n);
    TEST_ASSERT_NOT_NULL(strstr(buf, "; SameSite=None"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "; Secure"));
}

void test_cookie_serialize_max_age_zero(void)
{
    ioh_cookie_t cookie = {
        .name = "sid",
        .value = "",
        .max_age = 0,
        .same_site = IOH_SAME_SITE_DEFAULT,
    };

    char buf[256];
    int n = ioh_cookie_serialize(&cookie, buf, sizeof(buf));
    TEST_ASSERT_GREATER_THAN(0, n);
    TEST_ASSERT_NOT_NULL(strstr(buf, "; Max-Age=0"));
}

void test_cookie_serialize_max_age_session(void)
{
    ioh_cookie_t cookie = {
        .name = "sid",
        .value = "abc",
        .max_age = -1,
        .same_site = IOH_SAME_SITE_DEFAULT,
    };

    char buf[256];
    int n = ioh_cookie_serialize(&cookie, buf, sizeof(buf));
    TEST_ASSERT_GREATER_THAN(0, n);
    /* no Max-Age attribute for session cookies */
    TEST_ASSERT_NULL(strstr(buf, "Max-Age"));
}

/* ---- ioh_cookie_validate_name ---- */

void test_cookie_validate_name_valid(void)
{
    TEST_ASSERT_EQUAL_INT(0, ioh_cookie_validate_name("session"));
    TEST_ASSERT_EQUAL_INT(0, ioh_cookie_validate_name("__Host-id"));
    TEST_ASSERT_EQUAL_INT(0, ioh_cookie_validate_name("my_cookie_123"));
}

void test_cookie_validate_name_rejects_empty(void)
{
    TEST_ASSERT_EQUAL_INT(-EINVAL, ioh_cookie_validate_name(nullptr));
    TEST_ASSERT_EQUAL_INT(-EINVAL, ioh_cookie_validate_name(""));
}

void test_cookie_validate_name_rejects_special_chars(void)
{
    TEST_ASSERT_EQUAL_INT(-EINVAL, ioh_cookie_validate_name("a=b"));
    TEST_ASSERT_EQUAL_INT(-EINVAL, ioh_cookie_validate_name("a;b"));
    TEST_ASSERT_EQUAL_INT(-EINVAL, ioh_cookie_validate_name("a b"));
    TEST_ASSERT_EQUAL_INT(-EINVAL, ioh_cookie_validate_name("a\tb"));
    TEST_ASSERT_EQUAL_INT(-EINVAL, ioh_cookie_validate_name("a\nb"));
    TEST_ASSERT_EQUAL_INT(-EINVAL, ioh_cookie_validate_name("a\rb"));
    TEST_ASSERT_EQUAL_INT(-EINVAL, ioh_cookie_validate_name("\x01name"));
    TEST_ASSERT_EQUAL_INT(-EINVAL, ioh_cookie_validate_name("del\x7F"));
}

/* ---- Edge cases ---- */

void test_cookie_serialize_buffer_too_small(void)
{
    ioh_cookie_t cookie = {
        .name = "session",
        .value = "abc123",
        .max_age = -1,
        .same_site = IOH_SAME_SITE_DEFAULT,
    };

    char buf[5]; /* too small for "session=abc123" */
    int n = ioh_cookie_serialize(&cookie, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_INT(-ENOSPC, n);
}

void test_cookie_serialize_null_cookie(void)
{
    char buf[256];
    TEST_ASSERT_EQUAL_INT(-EINVAL, ioh_cookie_serialize(nullptr, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_INT(-EINVAL, ioh_cookie_serialize(nullptr, nullptr, 0));
}

/* ---- ioh_ctx_set_cookie ---- */

void test_ctx_set_cookie(void)
{
    ioh_ctx_t ctx;
    TEST_ASSERT_EQUAL_INT(0, ioh_ctx_init(&ctx, &req, &resp, nullptr));

    ioh_cookie_t cookie = {
        .name = "sid",
        .value = "abc123",
        .path = "/",
        .max_age = 3600,
        .same_site = IOH_SAME_SITE_LAX,
        .secure = true,
        .http_only = true,
    };

    TEST_ASSERT_EQUAL_INT(0, ioh_ctx_set_cookie(&ctx, &cookie));

    /* verify the Set-Cookie header was added */
    bool found = false;
    for (uint32_t i = 0; i < resp.header_count; i++) {
        if (strncasecmp(resp.headers[i].name, "Set-Cookie", resp.headers[i].name_len) == 0) {
            found = true;
            TEST_ASSERT_NOT_NULL(strstr(resp.headers[i].value, "sid=abc123"));
            TEST_ASSERT_NOT_NULL(strstr(resp.headers[i].value, "; Path=/"));
            TEST_ASSERT_NOT_NULL(strstr(resp.headers[i].value, "; Secure"));
            TEST_ASSERT_NOT_NULL(strstr(resp.headers[i].value, "; HttpOnly"));
            break;
        }
    }
    TEST_ASSERT_TRUE(found);

    /* set a second cookie -- both should exist (add, not replace) */
    ioh_cookie_t cookie2 = {
        .name = "lang",
        .value = "en",
        .max_age = -1,
        .same_site = IOH_SAME_SITE_DEFAULT,
    };
    TEST_ASSERT_EQUAL_INT(0, ioh_ctx_set_cookie(&ctx, &cookie2));

    uint32_t set_cookie_count = 0;
    for (uint32_t i = 0; i < resp.header_count; i++) {
        if (strncasecmp(resp.headers[i].name, "Set-Cookie", resp.headers[i].name_len) == 0) {
            set_cookie_count++;
        }
    }
    TEST_ASSERT_EQUAL_UINT32(2, set_cookie_count);

    ioh_ctx_destroy(&ctx);
}

/* ---- Runner ---- */

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_cookie_serialize_simple);
    RUN_TEST(test_cookie_serialize_full_attributes);
    RUN_TEST(test_cookie_serialize_same_site_lax);
    RUN_TEST(test_cookie_serialize_same_site_strict);
    RUN_TEST(test_cookie_serialize_same_site_none_forces_secure);
    RUN_TEST(test_cookie_serialize_max_age_zero);
    RUN_TEST(test_cookie_serialize_max_age_session);
    RUN_TEST(test_cookie_validate_name_valid);
    RUN_TEST(test_cookie_validate_name_rejects_empty);
    RUN_TEST(test_cookie_validate_name_rejects_special_chars);
    RUN_TEST(test_cookie_serialize_buffer_too_small);
    RUN_TEST(test_cookie_serialize_null_cookie);
    RUN_TEST(test_ctx_set_cookie);

    return UNITY_END();
}

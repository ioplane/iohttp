/**
 * @file test_io_alt_svc.c
 * @brief Unit tests for Alt-Svc header generation (RFC 7838).
 */

#include <unity/unity.h>

#include "http/io_alt_svc.h"
#include "http/io_response.h"

#include <errno.h>
#include <string.h>

void setUp(void)
{
}
void tearDown(void)
{
}

void test_alt_svc_format_default_port(void)
{
    char buf[64];
    int len = io_alt_svc_format(buf, sizeof(buf), 443, 0);
    TEST_ASSERT_GREATER_THAN(0, len);
    TEST_ASSERT_EQUAL_STRING("h3=\":443\"", buf);
}

void test_alt_svc_format_custom_port(void)
{
    char buf[64];
    int len = io_alt_svc_format(buf, sizeof(buf), 8443, 0);
    TEST_ASSERT_GREATER_THAN(0, len);
    TEST_ASSERT_EQUAL_STRING("h3=\":8443\"", buf);
}

void test_alt_svc_format_with_max_age(void)
{
    char buf[128];
    int len = io_alt_svc_format(buf, sizeof(buf), 443, 86400);
    TEST_ASSERT_GREATER_THAN(0, len);
    TEST_ASSERT_EQUAL_STRING("h3=\":443\"; ma=86400", buf);
}

void test_alt_svc_format_buffer_too_small(void)
{
    char buf[5];
    int len = io_alt_svc_format(buf, sizeof(buf), 443, 0);
    TEST_ASSERT_EQUAL_INT(-ENOSPC, len);
}

void test_alt_svc_format_null_buffer(void)
{
    int len = io_alt_svc_format(nullptr, 0, 443, 0);
    TEST_ASSERT_EQUAL_INT(-EINVAL, len);
}

void test_alt_svc_add_header(void)
{
    io_response_t resp;
    int rc = io_response_init(&resp);
    TEST_ASSERT_EQUAL_INT(0, rc);

    rc = io_alt_svc_add_header(&resp, 443, 0);
    TEST_ASSERT_EQUAL_INT(0, rc);

    /* Verify header was added */
    bool found = false;
    for (uint32_t i = 0; i < resp.header_count; i++) {
        if (resp.headers[i].name_len == 7 && memcmp(resp.headers[i].name, "alt-svc", 7) == 0) {
            found = true;
        }
    }
    TEST_ASSERT_TRUE(found);
    io_response_destroy(&resp);
}

void test_alt_svc_add_header_null(void)
{
    TEST_ASSERT_EQUAL_INT(-EINVAL, io_alt_svc_add_header(nullptr, 443, 0));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_alt_svc_format_default_port);
    RUN_TEST(test_alt_svc_format_custom_port);
    RUN_TEST(test_alt_svc_format_with_max_age);
    RUN_TEST(test_alt_svc_format_buffer_too_small);
    RUN_TEST(test_alt_svc_format_null_buffer);
    RUN_TEST(test_alt_svc_add_header);
    RUN_TEST(test_alt_svc_add_header_null);
    return UNITY_END();
}

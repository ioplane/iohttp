/**
 * @file test_io_response.c
 * @brief Unit tests for ioh_response HTTP response builder.
 */

#include "http/ioh_response.h"

#include <errno.h>
#include <string.h>

#include <unity.h>

void setUp(void)
{
}
void tearDown(void)
{
}

/* ---- Lifecycle ---- */

void test_response_init_destroy(void)
{
    ioh_response_t resp;

    TEST_ASSERT_EQUAL_INT(0, ioh_response_init(&resp));
    TEST_ASSERT_EQUAL_UINT16(200, resp.status);
    TEST_ASSERT_NOT_NULL(resp.headers);
    TEST_ASSERT_EQUAL_UINT32(0, resp.header_count);
    TEST_ASSERT_GREATER_THAN(0, resp.header_capacity);
    TEST_ASSERT_NOT_NULL(resp.body);
    TEST_ASSERT_EQUAL_size_t(0, resp.body_len);
    TEST_ASSERT_GREATER_THAN(0, resp.body_capacity);
    TEST_ASSERT_FALSE(resp.headers_sent);
    TEST_ASSERT_FALSE(resp.chunked);

    ioh_response_destroy(&resp);

    /* nullptr is safe */
    ioh_response_destroy(nullptr);

    /* nullptr init returns error */
    TEST_ASSERT_EQUAL_INT(-EINVAL, ioh_response_init(nullptr));
}

/* ---- Set header ---- */

void test_response_set_header(void)
{
    ioh_response_t resp;
    TEST_ASSERT_EQUAL_INT(0, ioh_response_init(&resp));

    /* add a header */
    TEST_ASSERT_EQUAL_INT(0, ioh_response_set_header(&resp, "X-Test", "value1"));
    TEST_ASSERT_EQUAL_UINT32(1, resp.header_count);
    TEST_ASSERT_EQUAL_STRING("X-Test", resp.headers[0].name);
    TEST_ASSERT_EQUAL_STRING("value1", resp.headers[0].value);

    /* add a second header */
    TEST_ASSERT_EQUAL_INT(0, ioh_response_set_header(&resp, "X-Other", "value2"));
    TEST_ASSERT_EQUAL_UINT32(2, resp.header_count);

    /* replace existing header (case-insensitive) */
    TEST_ASSERT_EQUAL_INT(0, ioh_response_set_header(&resp, "x-test", "replaced"));
    TEST_ASSERT_EQUAL_UINT32(2, resp.header_count);
    TEST_ASSERT_EQUAL_STRING("replaced", resp.headers[0].value);

    /* bad input */
    TEST_ASSERT_EQUAL_INT(-EINVAL, ioh_response_set_header(nullptr, "a", "b"));
    TEST_ASSERT_EQUAL_INT(-EINVAL, ioh_response_set_header(&resp, nullptr, "b"));
    TEST_ASSERT_EQUAL_INT(-EINVAL, ioh_response_set_header(&resp, "a", nullptr));

    ioh_response_destroy(&resp);
}

/* ---- JSON convenience ---- */

void test_response_json(void)
{
    ioh_response_t resp;
    TEST_ASSERT_EQUAL_INT(0, ioh_response_init(&resp));

    const char *json = "{\"status\":\"ok\"}";
    TEST_ASSERT_EQUAL_INT(0, ioh_respond_json(&resp, 200, json));

    TEST_ASSERT_EQUAL_UINT16(200, resp.status);
    TEST_ASSERT_EQUAL_UINT32(1, resp.header_count);
    TEST_ASSERT_EQUAL_STRING("Content-Type", resp.headers[0].name);
    TEST_ASSERT_EQUAL_STRING("application/json", resp.headers[0].value);
    TEST_ASSERT_EQUAL_size_t(strnlen(json, 256), resp.body_len);
    TEST_ASSERT_EQUAL_MEMORY(json, resp.body, resp.body_len);

    ioh_response_destroy(&resp);
}

/* ---- Status text ---- */

void test_response_status_text(void)
{
    TEST_ASSERT_EQUAL_STRING("OK", ioh_status_text(200));
    TEST_ASSERT_EQUAL_STRING("Created", ioh_status_text(201));
    TEST_ASSERT_EQUAL_STRING("No Content", ioh_status_text(204));
    TEST_ASSERT_EQUAL_STRING("Moved Permanently", ioh_status_text(301));
    TEST_ASSERT_EQUAL_STRING("Bad Request", ioh_status_text(400));
    TEST_ASSERT_EQUAL_STRING("Unauthorized", ioh_status_text(401));
    TEST_ASSERT_EQUAL_STRING("Forbidden", ioh_status_text(403));
    TEST_ASSERT_EQUAL_STRING("Not Found", ioh_status_text(404));
    TEST_ASSERT_EQUAL_STRING("Method Not Allowed", ioh_status_text(405));
    TEST_ASSERT_EQUAL_STRING("Internal Server Error", ioh_status_text(500));
    TEST_ASSERT_EQUAL_STRING("Unknown", ioh_status_text(999));
}

/* ---- Test runner ---- */

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_response_init_destroy);
    RUN_TEST(test_response_set_header);
    RUN_TEST(test_response_json);
    RUN_TEST(test_response_status_text);

    return UNITY_END();
}

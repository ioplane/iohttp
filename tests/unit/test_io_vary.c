/**
 * @file test_io_vary.c
 * @brief Unit tests for io_response_add_vary() Vary header accumulator.
 */

#include "http/io_response.h"

#include <errno.h>
#include <string.h>
#include <unity.h>

static io_response_t resp;

void setUp(void)
{
    TEST_ASSERT_EQUAL_INT(0, io_response_init(&resp));
}

void tearDown(void)
{
    io_response_destroy(&resp);
}

/* ---- Helpers ---- */

static const char *find_vary_value(const io_response_t *r)
{
    for (uint32_t i = 0; i < r->header_count; i++) {
        if (r->headers[i].name_len == 4 && strncasecmp(r->headers[i].name, "Vary", 4) == 0) {
            return r->headers[i].value;
        }
    }
    return nullptr;
}

/* ---- Tests ---- */

void test_vary_single_token(void)
{
    int rc = io_response_add_vary(&resp, "Accept-Encoding");
    TEST_ASSERT_EQUAL_INT(0, rc);

    const char *val = find_vary_value(&resp);
    TEST_ASSERT_NOT_NULL(val);
    TEST_ASSERT_EQUAL_STRING("Accept-Encoding", val);
}

void test_vary_multiple_tokens(void)
{
    int rc = io_response_add_vary(&resp, "Accept-Encoding");
    TEST_ASSERT_EQUAL_INT(0, rc);

    rc = io_response_add_vary(&resp, "Accept");
    TEST_ASSERT_EQUAL_INT(0, rc);

    const char *val = find_vary_value(&resp);
    TEST_ASSERT_NOT_NULL(val);
    TEST_ASSERT_EQUAL_STRING("Accept-Encoding, Accept", val);
}

void test_vary_no_duplicate(void)
{
    int rc = io_response_add_vary(&resp, "Accept-Encoding");
    TEST_ASSERT_EQUAL_INT(0, rc);

    rc = io_response_add_vary(&resp, "Accept-Encoding");
    TEST_ASSERT_EQUAL_INT(0, rc);

    const char *val = find_vary_value(&resp);
    TEST_ASSERT_NOT_NULL(val);
    TEST_ASSERT_EQUAL_STRING("Accept-Encoding", val);
}

void test_vary_case_insensitive_dedup(void)
{
    int rc = io_response_add_vary(&resp, "accept-encoding");
    TEST_ASSERT_EQUAL_INT(0, rc);

    rc = io_response_add_vary(&resp, "Accept-Encoding");
    TEST_ASSERT_EQUAL_INT(0, rc);

    const char *val = find_vary_value(&resp);
    TEST_ASSERT_NOT_NULL(val);
    /* should keep the original casing, no duplicate */
    TEST_ASSERT_EQUAL_STRING("accept-encoding", val);
}

void test_vary_null_inputs(void)
{
    TEST_ASSERT_EQUAL_INT(-EINVAL, io_response_add_vary(nullptr, "Accept"));
    TEST_ASSERT_EQUAL_INT(-EINVAL, io_response_add_vary(&resp, nullptr));
}

void test_vary_empty_token(void)
{
    TEST_ASSERT_EQUAL_INT(-EINVAL, io_response_add_vary(&resp, ""));
}

void test_vary_three_tokens(void)
{
    int rc = io_response_add_vary(&resp, "Accept-Encoding");
    TEST_ASSERT_EQUAL_INT(0, rc);

    rc = io_response_add_vary(&resp, "Accept");
    TEST_ASSERT_EQUAL_INT(0, rc);

    rc = io_response_add_vary(&resp, "Origin");
    TEST_ASSERT_EQUAL_INT(0, rc);

    const char *val = find_vary_value(&resp);
    TEST_ASSERT_NOT_NULL(val);
    TEST_ASSERT_EQUAL_STRING("Accept-Encoding, Accept, Origin", val);
}

void test_vary_no_false_substring_match(void)
{
    /* "Accept" should not match "Accept-Encoding" as a substring */
    int rc = io_response_add_vary(&resp, "Accept-Encoding");
    TEST_ASSERT_EQUAL_INT(0, rc);

    rc = io_response_add_vary(&resp, "Accept");
    TEST_ASSERT_EQUAL_INT(0, rc);

    const char *val = find_vary_value(&resp);
    TEST_ASSERT_NOT_NULL(val);
    TEST_ASSERT_EQUAL_STRING("Accept-Encoding, Accept", val);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_vary_single_token);
    RUN_TEST(test_vary_multiple_tokens);
    RUN_TEST(test_vary_no_duplicate);
    RUN_TEST(test_vary_case_insensitive_dedup);
    RUN_TEST(test_vary_null_inputs);
    RUN_TEST(test_vary_empty_token);
    RUN_TEST(test_vary_three_tokens);
    RUN_TEST(test_vary_no_false_substring_match);
    return UNITY_END();
}

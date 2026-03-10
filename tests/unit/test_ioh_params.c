/**
 * @file test_io_params.c
 * @brief Unit tests for typed parameter extraction from ioh_request_t.
 */

#include "http/ioh_request.h"

#include <errno.h>
#include <stdint.h>
#include <string.h>

#include <unity.h>

void setUp(void)
{
}
void tearDown(void)
{
}

static void setup_param(ioh_request_t *req, uint32_t idx, const char *name, const char *value)
{
    req->params[idx].name = name;
    req->params[idx].name_len = strlen(name);
    req->params[idx].value = value;
    req->params[idx].value_len = strlen(value);
    if (idx >= req->param_count) {
        req->param_count = idx + 1;
    }
}

/* ---- String param lookup ---- */

void test_param_string(void)
{
    ioh_request_t req;
    ioh_request_init(&req);

    setup_param(&req, 0, "id", "alice");

    const char *val = ioh_request_param(&req, "id");
    TEST_ASSERT_NOT_NULL(val);
    TEST_ASSERT_EQUAL_STRING("alice", val);

    /* nonexistent key */
    TEST_ASSERT_NULL(ioh_request_param(&req, "missing"));

    /* nullptr safety */
    TEST_ASSERT_NULL(ioh_request_param(nullptr, "id"));
    TEST_ASSERT_NULL(ioh_request_param(&req, nullptr));
}

/* ---- i64 ---- */

void test_param_i64_valid(void)
{
    ioh_request_t req;
    ioh_request_init(&req);
    setup_param(&req, 0, "id", "42");

    int64_t out = 0;
    int rc = ioh_request_param_i64(&req, "id", &out);
    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_EQUAL_INT64(42, out);
}

void test_param_i64_negative(void)
{
    ioh_request_t req;
    ioh_request_init(&req);
    setup_param(&req, 0, "offset", "-5");

    int64_t out = 0;
    int rc = ioh_request_param_i64(&req, "offset", &out);
    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_EQUAL_INT64(-5, out);
}

void test_param_i64_invalid(void)
{
    ioh_request_t req;
    ioh_request_init(&req);
    setup_param(&req, 0, "id", "abc");

    int64_t out = 0;
    int rc = ioh_request_param_i64(&req, "id", &out);
    TEST_ASSERT_EQUAL_INT(-EINVAL, rc);
}

void test_param_i64_overflow(void)
{
    ioh_request_t req;
    ioh_request_init(&req);
    setup_param(&req, 0, "big", "99999999999999999999");

    int64_t out = 0;
    int rc = ioh_request_param_i64(&req, "big", &out);
    TEST_ASSERT_EQUAL_INT(-ERANGE, rc);
}

/* ---- u64 ---- */

void test_param_u64_valid(void)
{
    ioh_request_t req;
    ioh_request_init(&req);
    setup_param(&req, 0, "size", "18446744073709551615");

    uint64_t out = 0;
    int rc = ioh_request_param_u64(&req, "size", &out);
    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_EQUAL_UINT64(UINT64_MAX, out);
}

/* ---- bool ---- */

void test_param_bool_true(void)
{
    ioh_request_t req;
    ioh_request_init(&req);

    const char *true_vals[] = {"true", "1", "yes"};
    bool out = false;

    for (size_t i = 0; i < 3; i++) {
        ioh_request_init(&req);
        setup_param(&req, 0, "flag", true_vals[i]);
        out = false;
        int rc = ioh_request_param_bool(&req, "flag", &out);
        TEST_ASSERT_EQUAL_INT(0, rc);
        TEST_ASSERT_TRUE(out);
    }
}

void test_param_bool_false(void)
{
    ioh_request_t req;
    ioh_request_init(&req);

    const char *false_vals[] = {"false", "0", "no"};
    bool out = true;

    for (size_t i = 0; i < 3; i++) {
        ioh_request_init(&req);
        setup_param(&req, 0, "flag", false_vals[i]);
        out = true;
        int rc = ioh_request_param_bool(&req, "flag", &out);
        TEST_ASSERT_EQUAL_INT(0, rc);
        TEST_ASSERT_FALSE(out);
    }
}

/* ---- Missing param ---- */

void test_param_missing(void)
{
    ioh_request_t req;
    ioh_request_init(&req);

    /* no params set at all */
    TEST_ASSERT_NULL(ioh_request_param(&req, "id"));

    int64_t i64 = 0;
    TEST_ASSERT_EQUAL_INT(-EINVAL, ioh_request_param_i64(&req, "id", &i64));

    uint64_t u64 = 0;
    TEST_ASSERT_EQUAL_INT(-EINVAL, ioh_request_param_u64(&req, "id", &u64));

    bool b = false;
    TEST_ASSERT_EQUAL_INT(-EINVAL, ioh_request_param_bool(&req, "id", &b));
}

/* ---- Param count ---- */

void test_param_count(void)
{
    ioh_request_t req;
    ioh_request_init(&req);

    TEST_ASSERT_EQUAL_UINT32(0, ioh_request_param_count(&req));
    TEST_ASSERT_EQUAL_UINT32(0, ioh_request_param_count(nullptr));

    setup_param(&req, 0, "a", "1");
    TEST_ASSERT_EQUAL_UINT32(1, ioh_request_param_count(&req));

    setup_param(&req, 1, "b", "2");
    TEST_ASSERT_EQUAL_UINT32(2, ioh_request_param_count(&req));

    setup_param(&req, 2, "c", "3");
    TEST_ASSERT_EQUAL_UINT32(3, ioh_request_param_count(&req));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_param_string);
    RUN_TEST(test_param_i64_valid);
    RUN_TEST(test_param_i64_negative);
    RUN_TEST(test_param_i64_invalid);
    RUN_TEST(test_param_i64_overflow);
    RUN_TEST(test_param_u64_valid);
    RUN_TEST(test_param_bool_true);
    RUN_TEST(test_param_bool_false);
    RUN_TEST(test_param_missing);
    RUN_TEST(test_param_count);
    return UNITY_END();
}

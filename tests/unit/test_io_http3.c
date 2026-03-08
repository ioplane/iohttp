#include <unity/unity.h>

#include "http/io_http3.h"

#include <errno.h>
#include <string.h>

void setUp(void)
{
}
void tearDown(void)
{
}

/* ---- Config tests ---- */

void test_http3_config_init_defaults(void)
{
    io_http3_config_t cfg;
    io_http3_config_init(&cfg);
    TEST_ASSERT_EQUAL_UINT32(8192, cfg.max_header_list_size);
    TEST_ASSERT_EQUAL_UINT32(0, cfg.qpack_max_dtable_capacity);
    TEST_ASSERT_EQUAL_UINT32(0, cfg.qpack_blocked_streams);
}

/* ---- Session lifecycle null safety ---- */

void test_http3_session_create_null_quic(void)
{
    TEST_ASSERT_NULL(io_http3_session_create(nullptr, nullptr, nullptr, nullptr));
}

void test_http3_session_destroy_null(void)
{
    /* Must not crash */
    io_http3_session_destroy(nullptr);
}

/* ---- Data processing null safety ---- */

void test_http3_on_stream_data_null(void)
{
    uint8_t data[] = "test";
    TEST_ASSERT_EQUAL_INT(-EINVAL, io_http3_on_stream_data(nullptr, 0, data, 4, false));
}

void test_http3_on_stream_open_null(void)
{
    TEST_ASSERT_EQUAL_INT(-EINVAL, io_http3_on_stream_open(nullptr, 0));
}

void test_http3_submit_response_null(void)
{
    io_response_t resp = {0};
    TEST_ASSERT_EQUAL_INT(-EINVAL, io_http3_submit_response(nullptr, 0, &resp));
}

/* ---- State queries ---- */

void test_http3_is_shutdown_null(void)
{
    TEST_ASSERT_TRUE(io_http3_is_shutdown(nullptr)); /* null = shut down */
}

void test_http3_shutdown_null(void)
{
    TEST_ASSERT_EQUAL_INT(-EINVAL, io_http3_shutdown(nullptr));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_http3_config_init_defaults);
    RUN_TEST(test_http3_session_create_null_quic);
    RUN_TEST(test_http3_session_destroy_null);
    RUN_TEST(test_http3_on_stream_data_null);
    RUN_TEST(test_http3_on_stream_open_null);
    RUN_TEST(test_http3_submit_response_null);
    RUN_TEST(test_http3_is_shutdown_null);
    RUN_TEST(test_http3_shutdown_null);
    return UNITY_END();
}

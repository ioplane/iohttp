/**
 * @file test_io_sse.c
 * @brief Unit tests for Server-Sent Events (SSE) formatting.
 */

#include "ws/io_sse.h"

#include <errno.h>
#include <string.h>

#include <unity.h>

static io_sse_stream_t stream;

void setUp(void)
{
    int rc = io_sse_stream_init(&stream);
    TEST_ASSERT_EQUAL_INT(0, rc);
}

void tearDown(void)
{
    io_sse_stream_destroy(&stream);
}

/* ---- test_sse_format_headers ---- */

void test_sse_format_headers(void)
{
    uint8_t buf[256];
    int rc = io_sse_format_headers(buf, sizeof(buf));
    TEST_ASSERT_GREATER_THAN(0, rc);

    const char *output = (const char *)buf;
    TEST_ASSERT_NOT_NULL(strstr(output, "Content-Type: text/event-stream; charset=utf-8\r\n"));
    TEST_ASSERT_NOT_NULL(strstr(output, "Cache-Control: no-cache\r\n"));
    TEST_ASSERT_NOT_NULL(strstr(output, "Connection: keep-alive\r\n"));

    /* buffer too small */
    int rc2 = io_sse_format_headers(buf, 10);
    TEST_ASSERT_EQUAL_INT(-ENOSPC, rc2);
}

/* ---- test_sse_format_data_only ---- */

void test_sse_format_data_only(void)
{
    io_sse_event_t event = {
        .event = nullptr,
        .data = "hello",
        .id = nullptr,
        .retry_ms = 0,
    };

    int rc = io_sse_format_event(&stream, &event);
    TEST_ASSERT_EQUAL_INT(0, rc);

    const uint8_t *out;
    size_t len;
    io_sse_stream_flush(&stream, &out, &len);

    const char *expected = "data: hello\n\n";
    TEST_ASSERT_EQUAL_size_t(strlen(expected), len);
    TEST_ASSERT_EQUAL_INT(0, memcmp(out, expected, len));
}

/* ---- test_sse_format_with_event ---- */

void test_sse_format_with_event(void)
{
    io_sse_event_t event = {
        .event = "update",
        .data = "new data",
        .id = nullptr,
        .retry_ms = 0,
    };

    int rc = io_sse_format_event(&stream, &event);
    TEST_ASSERT_EQUAL_INT(0, rc);

    const uint8_t *out;
    size_t len;
    io_sse_stream_flush(&stream, &out, &len);

    const char *expected = "event: update\ndata: new data\n\n";
    TEST_ASSERT_EQUAL_size_t(strlen(expected), len);
    TEST_ASSERT_EQUAL_INT(0, memcmp(out, expected, len));
}

/* ---- test_sse_format_with_id ---- */

void test_sse_format_with_id(void)
{
    io_sse_event_t event = {
        .event = nullptr,
        .data = "payload",
        .id = "42",
        .retry_ms = 0,
    };

    int rc = io_sse_format_event(&stream, &event);
    TEST_ASSERT_EQUAL_INT(0, rc);

    const uint8_t *out;
    size_t len;
    io_sse_stream_flush(&stream, &out, &len);

    const char *expected = "id: 42\ndata: payload\n\n";
    TEST_ASSERT_EQUAL_size_t(strlen(expected), len);
    TEST_ASSERT_EQUAL_INT(0, memcmp(out, expected, len));

    /* verify last_event_id was tracked */
    TEST_ASSERT_NOT_NULL(stream.last_event_id);
    TEST_ASSERT_EQUAL_STRING("42", stream.last_event_id);
}

/* ---- test_sse_format_with_retry ---- */

void test_sse_format_with_retry(void)
{
    io_sse_event_t event = {
        .event = nullptr,
        .data = "msg",
        .id = nullptr,
        .retry_ms = 3000,
    };

    int rc = io_sse_format_event(&stream, &event);
    TEST_ASSERT_EQUAL_INT(0, rc);

    const uint8_t *out;
    size_t len;
    io_sse_stream_flush(&stream, &out, &len);

    const char *expected = "retry: 3000\ndata: msg\n\n";
    TEST_ASSERT_EQUAL_size_t(strlen(expected), len);
    TEST_ASSERT_EQUAL_INT(0, memcmp(out, expected, len));
}

/* ---- test_sse_format_comment ---- */

void test_sse_format_comment(void)
{
    int rc = io_sse_format_comment(&stream, "ping");
    TEST_ASSERT_EQUAL_INT(0, rc);

    const uint8_t *out;
    size_t len;
    io_sse_stream_flush(&stream, &out, &len);

    const char *expected = ": ping\n\n";
    TEST_ASSERT_EQUAL_size_t(strlen(expected), len);
    TEST_ASSERT_EQUAL_INT(0, memcmp(out, expected, len));
}

/* ---- test_sse_format_multiline_data ---- */

void test_sse_format_multiline_data(void)
{
    io_sse_event_t event = {
        .event = nullptr,
        .data = "line1\nline2\nline3",
        .id = nullptr,
        .retry_ms = 0,
    };

    int rc = io_sse_format_event(&stream, &event);
    TEST_ASSERT_EQUAL_INT(0, rc);

    const uint8_t *out;
    size_t len;
    io_sse_stream_flush(&stream, &out, &len);

    const char *expected = "data: line1\ndata: line2\ndata: line3\n\n";
    TEST_ASSERT_EQUAL_size_t(strlen(expected), len);
    TEST_ASSERT_EQUAL_INT(0, memcmp(out, expected, len));
}

/* ---- test_sse_stream_flush ---- */

void test_sse_stream_flush(void)
{
    /* format two events, flush after each */
    io_sse_event_t event1 = {
        .event = nullptr,
        .data = "first",
        .id = nullptr,
        .retry_ms = 0,
    };

    int rc = io_sse_format_event(&stream, &event1);
    TEST_ASSERT_EQUAL_INT(0, rc);

    const uint8_t *out;
    size_t len;
    io_sse_stream_flush(&stream, &out, &len);

    TEST_ASSERT_GREATER_THAN(0, len);
    TEST_ASSERT_NOT_NULL(out);

    /* after flush, buf_len should be 0 */
    TEST_ASSERT_EQUAL_size_t(0, stream.buf_len);

    /* buffer is still allocated (capacity preserved) */
    TEST_ASSERT_TRUE(stream.buf_capacity > 0);
    TEST_ASSERT_NOT_NULL(stream.buf);

    /* format second event into the same stream */
    io_sse_event_t event2 = {
        .event = nullptr,
        .data = "second",
        .id = nullptr,
        .retry_ms = 0,
    };

    rc = io_sse_format_event(&stream, &event2);
    TEST_ASSERT_EQUAL_INT(0, rc);

    io_sse_stream_flush(&stream, &out, &len);

    const char *expected = "data: second\n\n";
    TEST_ASSERT_EQUAL_size_t(strlen(expected), len);
    TEST_ASSERT_EQUAL_INT(0, memcmp(out, expected, len));
}

/* ---- Test runner ---- */

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_sse_format_headers);
    RUN_TEST(test_sse_format_data_only);
    RUN_TEST(test_sse_format_with_event);
    RUN_TEST(test_sse_format_with_id);
    RUN_TEST(test_sse_format_with_retry);
    RUN_TEST(test_sse_format_comment);
    RUN_TEST(test_sse_format_multiline_data);
    RUN_TEST(test_sse_stream_flush);

    return UNITY_END();
}

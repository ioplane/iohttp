/**
 * @file test_io_http1.c
 * @brief Unit tests for HTTP/1.1 parser with request smuggling protection.
 */

#include "http/io_http1.h"

#include <errno.h>
#include <string.h>

#include <unity.h>

void setUp(void)
{
}
void tearDown(void)
{
}

/* ---- Basic parsing ---- */

void test_http1_parse_get(void)
{
    const char *raw = "GET /path HTTP/1.1\r\nHost: localhost\r\n\r\n";
    io_request_t req;

    int rc = io_http1_parse_request((const uint8_t *)raw, strlen(raw), &req);
    TEST_ASSERT_GREATER_THAN(0, rc);
    TEST_ASSERT_EQUAL_UINT8(IO_METHOD_GET, req.method);
    TEST_ASSERT_EQUAL_size_t(5, req.path_len);
    TEST_ASSERT_EQUAL_INT(0, memcmp(req.path, "/path", 5));
    TEST_ASSERT_NULL(req.query);
    TEST_ASSERT_EQUAL_UINT8(1, req.http_version_major);
    TEST_ASSERT_EQUAL_UINT8(1, req.http_version_minor);
    TEST_ASSERT_NOT_NULL(req.host);
}

void test_http1_parse_post_body(void)
{
    const char *raw = "POST /submit HTTP/1.1\r\n"
                      "Host: example.com\r\n"
                      "Content-Length: 11\r\n"
                      "Content-Type: text/plain\r\n"
                      "\r\n"
                      "hello world";
    io_request_t req;

    int rc = io_http1_parse_request((const uint8_t *)raw, strlen(raw), &req);
    TEST_ASSERT_GREATER_THAN(0, rc);
    TEST_ASSERT_EQUAL_UINT8(IO_METHOD_POST, req.method);
    TEST_ASSERT_EQUAL_size_t(11, req.content_length);
    TEST_ASSERT_NOT_NULL(req.content_type);
    TEST_ASSERT_EQUAL_INT(0, strncmp(req.content_type, "text/plain", 10));
}

void test_http1_parse_headers(void)
{
    const char *raw = "GET / HTTP/1.1\r\n"
                      "Host: localhost\r\n"
                      "Accept: text/html\r\n"
                      "X-Custom: foobar\r\n"
                      "\r\n";
    io_request_t req;

    int rc = io_http1_parse_request((const uint8_t *)raw, strlen(raw), &req);
    TEST_ASSERT_GREATER_THAN(0, rc);
    TEST_ASSERT_EQUAL_UINT32(3, req.header_count);

    /* Verify headers are accessible */
    const char *host = io_request_header(&req, "Host");
    TEST_ASSERT_NOT_NULL(host);
    TEST_ASSERT_EQUAL_INT(0, strncmp(host, "localhost", 9));

    const char *accept = io_request_header(&req, "Accept");
    TEST_ASSERT_NOT_NULL(accept);
    TEST_ASSERT_EQUAL_INT(0, strncmp(accept, "text/html", 9));

    const char *custom = io_request_header(&req, "X-Custom");
    TEST_ASSERT_NOT_NULL(custom);
    TEST_ASSERT_EQUAL_INT(0, strncmp(custom, "foobar", 6));
}

void test_http1_parse_incomplete(void)
{
    const char *raw = "GET /path HTTP/1.1\r\nHost: loc";
    io_request_t req;

    int rc = io_http1_parse_request((const uint8_t *)raw, strlen(raw), &req);
    TEST_ASSERT_EQUAL_INT(-EAGAIN, rc);
}

void test_http1_parse_malformed(void)
{
    const char *raw = "BADREQUEST\r\n\r\n";
    io_request_t req;

    int rc = io_http1_parse_request((const uint8_t *)raw, strlen(raw), &req);
    TEST_ASSERT_LESS_THAN(0, rc);
}

void test_http1_parse_oversized_uri(void)
{
    /* Build a request with URI > IO_HTTP1_MAX_URI_SIZE (4096) */
    char raw[8192 + 128];
    memcpy(raw, "GET /", 5);
    memset(raw + 5, 'A', 4200);
    const char *suffix = " HTTP/1.1\r\nHost: localhost\r\n\r\n";
    memcpy(raw + 5 + 4200, suffix, strlen(suffix) + 1);

    io_request_t req;
    int rc = io_http1_parse_request((const uint8_t *)raw, 5 + 4200 + strlen(suffix), &req);
    TEST_ASSERT_EQUAL_INT(-E2BIG, rc);
}

void test_http1_parse_oversized_headers(void)
{
    /* Build a request with more headers than IO_HTTP1_MAX_HEADERS (64) */
    char raw[32768];
    int pos = 0;
    pos += snprintf(raw + pos, sizeof(raw) - (size_t)pos, "GET / HTTP/1.1\r\nHost: localhost\r\n");

    for (int i = 0; i < 65; i++) {
        pos += snprintf(raw + pos, sizeof(raw) - (size_t)pos, "X-H%d: v%d\r\n", i, i);
    }
    pos += snprintf(raw + pos, sizeof(raw) - (size_t)pos, "\r\n");

    io_request_t req;
    int rc = io_http1_parse_request((const uint8_t *)raw, (size_t)pos, &req);
    /* picohttpparser returns -1 when num_headers exceeds the limit */
    TEST_ASSERT_LESS_THAN(0, rc);
}

/* ---- Chunked decoding ---- */

void test_http1_chunked_decode(void)
{
    io_chunked_decoder_t dec;
    io_http1_chunked_init(&dec);

    char data[] = "4\r\nWiki\r\n0\r\n\r\n";
    size_t len = strlen(data);
    uint8_t *buf = (uint8_t *)data;

    int rc = io_http1_chunked_decode(&dec, buf, &len);
    TEST_ASSERT_GREATER_OR_EQUAL(0, rc);
    TEST_ASSERT_EQUAL_size_t(4, len);
    TEST_ASSERT_EQUAL_INT(0, memcmp(buf, "Wiki", 4));
}

void test_http1_chunked_decode_incomplete(void)
{
    io_chunked_decoder_t dec;
    io_http1_chunked_init(&dec);

    char data[] = "4\r\nWi";
    size_t len = strlen(data);
    uint8_t *buf = (uint8_t *)data;

    int rc = io_http1_chunked_decode(&dec, buf, &len);
    TEST_ASSERT_EQUAL_INT(-EAGAIN, rc);
}

/* ---- Response serialization ---- */

void test_http1_serialize_200(void)
{
    io_response_t resp;
    int rc = io_response_init(&resp);
    TEST_ASSERT_EQUAL_INT(0, rc);

    resp.status = 200;
    const char *body = "Hello, World!";
    rc = io_response_set_body(&resp, (const uint8_t *)body, strlen(body));
    TEST_ASSERT_EQUAL_INT(0, rc);

    uint8_t buf[4096];
    rc = io_http1_serialize_response(&resp, buf, sizeof(buf));
    TEST_ASSERT_GREATER_THAN(0, rc);

    /* Check status line */
    TEST_ASSERT_EQUAL_INT(0, memcmp(buf, "HTTP/1.1 200 OK\r\n", 17));

    /* Check body appears */
    const char *found = strstr((const char *)buf, "Hello, World!");
    TEST_ASSERT_NOT_NULL(found);

    /* Check Content-Length header was added */
    const char *cl = strstr((const char *)buf, "Content-Length: 13\r\n");
    TEST_ASSERT_NOT_NULL(cl);

    io_response_destroy(&resp);
}

void test_http1_serialize_headers(void)
{
    io_response_t resp;
    int rc = io_response_init(&resp);
    TEST_ASSERT_EQUAL_INT(0, rc);

    resp.status = 404;
    rc = io_response_set_header(&resp, "X-Custom", "value123");
    TEST_ASSERT_EQUAL_INT(0, rc);
    rc = io_response_set_header(&resp, "Content-Type", "text/plain");
    TEST_ASSERT_EQUAL_INT(0, rc);

    uint8_t buf[4096];
    rc = io_http1_serialize_response(&resp, buf, sizeof(buf));
    TEST_ASSERT_GREATER_THAN(0, rc);

    TEST_ASSERT_EQUAL_INT(0, memcmp(buf, "HTTP/1.1 404 Not Found\r\n", 24));

    const char *custom = strstr((const char *)buf, "X-Custom: value123\r\n");
    TEST_ASSERT_NOT_NULL(custom);

    const char *ct = strstr((const char *)buf, "Content-Type: text/plain\r\n");
    TEST_ASSERT_NOT_NULL(ct);

    io_response_destroy(&resp);
}

/* ---- Keep-alive ---- */

void test_http1_keepalive_11(void)
{
    const char *raw = "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n";
    io_request_t req;

    int rc = io_http1_parse_request((const uint8_t *)raw, strlen(raw), &req);
    TEST_ASSERT_GREATER_THAN(0, rc);
    TEST_ASSERT_TRUE(req.keep_alive);
}

void test_http1_keepalive_10(void)
{
    const char *raw = "GET / HTTP/1.0\r\nHost: localhost\r\n\r\n";
    io_request_t req;

    int rc = io_http1_parse_request((const uint8_t *)raw, strlen(raw), &req);
    TEST_ASSERT_GREATER_THAN(0, rc);
    TEST_ASSERT_FALSE(req.keep_alive);
}

/* ---- Request smuggling protection ---- */

void test_http1_reject_duplicate_content_length(void)
{
    const char *raw = "POST /x HTTP/1.1\r\n"
                      "Host: localhost\r\n"
                      "Content-Length: 5\r\n"
                      "Content-Length: 10\r\n"
                      "\r\n"
                      "hello";
    io_request_t req;

    int rc = io_http1_parse_request((const uint8_t *)raw, strlen(raw), &req);
    TEST_ASSERT_EQUAL_INT(-EINVAL, rc);
}

void test_http1_reject_cl_and_te(void)
{
    const char *raw = "POST /x HTTP/1.1\r\n"
                      "Host: localhost\r\n"
                      "Content-Length: 5\r\n"
                      "Transfer-Encoding: chunked\r\n"
                      "\r\n";
    io_request_t req;

    int rc = io_http1_parse_request((const uint8_t *)raw, strlen(raw), &req);
    TEST_ASSERT_EQUAL_INT(-EINVAL, rc);
}

void test_http1_reject_obs_fold(void)
{
    /* obs-fold: continuation line (header with no name, value starts with
     * space/tab). picohttpparser sets name=NULL for continuation lines. */
    const char *raw = "GET / HTTP/1.1\r\n"
                      "Host: localhost\r\n"
                      "X-Test: value1\r\n"
                      " continued\r\n"
                      "\r\n";
    io_request_t req;

    int rc = io_http1_parse_request((const uint8_t *)raw, strlen(raw), &req);
    TEST_ASSERT_EQUAL_INT(-EINVAL, rc);
}

void test_http1_reject_bad_cl_value(void)
{
    const char *raw = "POST /x HTTP/1.1\r\n"
                      "Host: localhost\r\n"
                      "Content-Length: 12abc\r\n"
                      "\r\n";
    io_request_t req;

    int rc = io_http1_parse_request((const uint8_t *)raw, strlen(raw), &req);
    TEST_ASSERT_EQUAL_INT(-EINVAL, rc);
}

void test_http1_require_host_11(void)
{
    const char *raw = "GET / HTTP/1.1\r\n"
                      "Accept: text/html\r\n"
                      "\r\n";
    io_request_t req;

    int rc = io_http1_parse_request((const uint8_t *)raw, strlen(raw), &req);
    TEST_ASSERT_EQUAL_INT(-EINVAL, rc);
}

/* ---- Test runner ---- */

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_http1_parse_get);
    RUN_TEST(test_http1_parse_post_body);
    RUN_TEST(test_http1_parse_headers);
    RUN_TEST(test_http1_parse_incomplete);
    RUN_TEST(test_http1_parse_malformed);
    RUN_TEST(test_http1_parse_oversized_uri);
    RUN_TEST(test_http1_parse_oversized_headers);
    RUN_TEST(test_http1_chunked_decode);
    RUN_TEST(test_http1_chunked_decode_incomplete);
    RUN_TEST(test_http1_serialize_200);
    RUN_TEST(test_http1_serialize_headers);
    RUN_TEST(test_http1_keepalive_11);
    RUN_TEST(test_http1_keepalive_10);
    RUN_TEST(test_http1_reject_duplicate_content_length);
    RUN_TEST(test_http1_reject_cl_and_te);
    RUN_TEST(test_http1_reject_obs_fold);
    RUN_TEST(test_http1_reject_bad_cl_value);
    RUN_TEST(test_http1_require_host_11);

    return UNITY_END();
}

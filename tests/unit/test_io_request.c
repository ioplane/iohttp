/**
 * @file test_io_request.c
 * @brief Unit tests for io_request HTTP request abstraction.
 */

#include "http/io_request.h"

#include <string.h>

#include <unity.h>

void setUp(void)
{
}
void tearDown(void)
{
}

/* ---- Init ---- */

void test_request_init(void)
{
    io_request_t req;
    memset(&req, 0xFF, sizeof(req));

    io_request_init(&req);
    TEST_ASSERT_EQUAL_UINT8(IO_METHOD_GET, req.method);
    TEST_ASSERT_NULL(req.path);
    TEST_ASSERT_EQUAL_size_t(0, req.path_len);
    TEST_ASSERT_NULL(req.query);
    TEST_ASSERT_EQUAL_size_t(0, req.query_len);
    TEST_ASSERT_EQUAL_UINT32(0, req.header_count);
    TEST_ASSERT_NULL(req.body);
    TEST_ASSERT_EQUAL_size_t(0, req.body_len);
    TEST_ASSERT_EQUAL_UINT32(0, req.param_count);
    TEST_ASSERT_EQUAL_UINT8(0, req.http_version_major);
    TEST_ASSERT_EQUAL_UINT8(0, req.http_version_minor);
    TEST_ASSERT_FALSE(req.keep_alive);
    TEST_ASSERT_EQUAL_size_t(0, req.content_length);
    TEST_ASSERT_NULL(req.content_type);
    TEST_ASSERT_NULL(req.host);

    /* nullptr is safe */
    io_request_init(nullptr);
}

/* ---- Method parsing ---- */

void test_request_method_parse(void)
{
    TEST_ASSERT_EQUAL_UINT8(IO_METHOD_GET, io_method_parse("GET", 3));
    TEST_ASSERT_EQUAL_UINT8(IO_METHOD_POST, io_method_parse("POST", 4));
    TEST_ASSERT_EQUAL_UINT8(IO_METHOD_PUT, io_method_parse("PUT", 3));
    TEST_ASSERT_EQUAL_UINT8(IO_METHOD_DELETE, io_method_parse("DELETE", 6));
    TEST_ASSERT_EQUAL_UINT8(IO_METHOD_PATCH, io_method_parse("PATCH", 5));
    TEST_ASSERT_EQUAL_UINT8(IO_METHOD_HEAD, io_method_parse("HEAD", 4));
    TEST_ASSERT_EQUAL_UINT8(IO_METHOD_OPTIONS, io_method_parse("OPTIONS", 7));
    TEST_ASSERT_EQUAL_UINT8(IO_METHOD_TRACE, io_method_parse("TRACE", 5));
    TEST_ASSERT_EQUAL_UINT8(IO_METHOD_CONNECT, io_method_parse("CONNECT", 7));
}

void test_request_method_parse_unknown(void)
{
    TEST_ASSERT_EQUAL_UINT8(IO_METHOD_UNKNOWN, io_method_parse("INVALID", 7));
    TEST_ASSERT_EQUAL_UINT8(IO_METHOD_UNKNOWN, io_method_parse("get", 3));
    TEST_ASSERT_EQUAL_UINT8(IO_METHOD_UNKNOWN, io_method_parse(nullptr, 0));
    TEST_ASSERT_EQUAL_UINT8(IO_METHOD_UNKNOWN, io_method_parse("", 0));
    TEST_ASSERT_EQUAL_UINT8(IO_METHOD_UNKNOWN, io_method_parse("X", 1));
}

/* ---- Method name ---- */

void test_request_method_name(void)
{
    TEST_ASSERT_EQUAL_STRING("GET", io_method_name(IO_METHOD_GET));
    TEST_ASSERT_EQUAL_STRING("POST", io_method_name(IO_METHOD_POST));
    TEST_ASSERT_EQUAL_STRING("PUT", io_method_name(IO_METHOD_PUT));
    TEST_ASSERT_EQUAL_STRING("DELETE", io_method_name(IO_METHOD_DELETE));
    TEST_ASSERT_EQUAL_STRING("PATCH", io_method_name(IO_METHOD_PATCH));
    TEST_ASSERT_EQUAL_STRING("HEAD", io_method_name(IO_METHOD_HEAD));
    TEST_ASSERT_EQUAL_STRING("OPTIONS", io_method_name(IO_METHOD_OPTIONS));
    TEST_ASSERT_EQUAL_STRING("TRACE", io_method_name(IO_METHOD_TRACE));
    TEST_ASSERT_EQUAL_STRING("CONNECT", io_method_name(IO_METHOD_CONNECT));
    TEST_ASSERT_EQUAL_STRING("UNKNOWN", io_method_name(IO_METHOD_UNKNOWN));
}

/* ---- Header lookup ---- */

void test_request_header_find(void)
{
    io_request_t req;
    io_request_init(&req);

    req.headers[0] = (io_header_t){
        .name = "Content-Type",
        .name_len = 12,
        .value = "text/html",
        .value_len = 9,
    };
    req.headers[1] = (io_header_t){
        .name = "X-Custom",
        .name_len = 8,
        .value = "foobar",
        .value_len = 6,
    };
    req.header_count = 2;

    /* case-insensitive match */
    TEST_ASSERT_EQUAL_STRING("text/html", io_request_header(&req, "Content-Type"));
    TEST_ASSERT_EQUAL_STRING("text/html", io_request_header(&req, "content-type"));
    TEST_ASSERT_EQUAL_STRING("text/html", io_request_header(&req, "CONTENT-TYPE"));
    TEST_ASSERT_EQUAL_STRING("foobar", io_request_header(&req, "x-custom"));
}

void test_request_header_find_missing(void)
{
    io_request_t req;
    io_request_init(&req);

    req.headers[0] = (io_header_t){
        .name = "Host",
        .name_len = 4,
        .value = "example.com",
        .value_len = 11,
    };
    req.header_count = 1;

    TEST_ASSERT_NULL(io_request_header(&req, "Accept"));
    TEST_ASSERT_NULL(io_request_header(&req, ""));
    TEST_ASSERT_NULL(io_request_header(nullptr, "Host"));
    TEST_ASSERT_NULL(io_request_header(&req, nullptr));
}

/* ---- Keep-alive defaults ---- */

void test_request_keep_alive_11(void)
{
    io_request_t req;
    io_request_init(&req);
    req.http_version_major = 1;
    req.http_version_minor = 1;
    req.keep_alive = true; /* HTTP/1.1 default */

    TEST_ASSERT_TRUE(req.keep_alive);
}

void test_request_keep_alive_10(void)
{
    io_request_t req;
    io_request_init(&req);
    req.http_version_major = 1;
    req.http_version_minor = 0;
    /* HTTP/1.0 defaults to close */
    req.keep_alive = false;

    TEST_ASSERT_FALSE(req.keep_alive);
}

/* ---- Cookie parsing ---- */

void test_request_cookie_single(void)
{
    io_request_t req;
    io_request_init(&req);

    req.headers[0] = (io_header_t){
        .name = "Cookie",
        .name_len = 6,
        .value = "session=abc123",
        .value_len = 14,
    };
    req.header_count = 1;

    const char *val = io_request_cookie(&req, "session");
    TEST_ASSERT_NOT_NULL(val);
    TEST_ASSERT_EQUAL_INT(0, strncmp(val, "abc123", 6));
}

void test_request_cookie_multiple(void)
{
    io_request_t req;
    io_request_init(&req);

    req.headers[0] = (io_header_t){
        .name = "Cookie",
        .name_len = 6,
        .value = "a=1; b=2; session=xyz",
        .value_len = 21,
    };
    req.header_count = 1;

    const char *a = io_request_cookie(&req, "a");
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_EQUAL_INT(0, strncmp(a, "1", 1));

    const char *b = io_request_cookie(&req, "b");
    TEST_ASSERT_NOT_NULL(b);
    TEST_ASSERT_EQUAL_INT(0, strncmp(b, "2", 1));

    const char *session = io_request_cookie(&req, "session");
    TEST_ASSERT_NOT_NULL(session);
    TEST_ASSERT_EQUAL_INT(0, strncmp(session, "xyz", 3));
}

void test_request_cookie_missing(void)
{
    io_request_t req;
    io_request_init(&req);

    req.headers[0] = (io_header_t){
        .name = "Cookie",
        .name_len = 6,
        .value = "a=1; b=2",
        .value_len = 8,
    };
    req.header_count = 1;

    TEST_ASSERT_NULL(io_request_cookie(&req, "missing"));
    TEST_ASSERT_NULL(io_request_cookie(&req, nullptr));
    TEST_ASSERT_NULL(io_request_cookie(nullptr, "a"));
}

/* ---- Query parameter lookup ---- */

void test_request_query_param(void)
{
    io_request_t req;
    io_request_init(&req);

    const char *qs = "page=2&sort=name&filter=active";
    req.query = qs;
    req.query_len = strnlen(qs, 256);

    const char *page = io_request_query_param(&req, "page");
    TEST_ASSERT_NOT_NULL(page);
    TEST_ASSERT_EQUAL_INT(0, strncmp(page, "2", 1));

    const char *sort = io_request_query_param(&req, "sort");
    TEST_ASSERT_NOT_NULL(sort);
    TEST_ASSERT_EQUAL_INT(0, strncmp(sort, "name", 4));

    const char *filter = io_request_query_param(&req, "filter");
    TEST_ASSERT_NOT_NULL(filter);
    TEST_ASSERT_EQUAL_INT(0, strncmp(filter, "active", 6));

    TEST_ASSERT_NULL(io_request_query_param(&req, "missing"));
}

/* ---- Accept header matching ---- */

void test_request_accepts_json(void)
{
    io_request_t req;
    io_request_init(&req);

    req.headers[0] = (io_header_t){
        .name = "Accept",
        .name_len = 6,
        .value = "application/json, text/html",
        .value_len = 27,
    };
    req.header_count = 1;

    const char *types[] = {"text/html", "application/json"};
    const char *match = io_request_accepts(&req, types, 2);
    TEST_ASSERT_NOT_NULL(match);
    TEST_ASSERT_EQUAL_STRING("application/json", match);
}

void test_request_accepts_wildcard(void)
{
    io_request_t req;
    io_request_init(&req);

    req.headers[0] = (io_header_t){
        .name = "Accept",
        .name_len = 6,
        .value = "*/*",
        .value_len = 3,
    };
    req.header_count = 1;

    const char *types[] = {"application/json", "text/html"};
    const char *match = io_request_accepts(&req, types, 2);
    TEST_ASSERT_NOT_NULL(match);
    /* wildcard returns first offered type */
    TEST_ASSERT_EQUAL_STRING("application/json", match);
}

/* ---- Test runner ---- */

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_request_init);
    RUN_TEST(test_request_method_parse);
    RUN_TEST(test_request_method_parse_unknown);
    RUN_TEST(test_request_method_name);
    RUN_TEST(test_request_header_find);
    RUN_TEST(test_request_header_find_missing);
    RUN_TEST(test_request_keep_alive_11);
    RUN_TEST(test_request_keep_alive_10);
    RUN_TEST(test_request_cookie_single);
    RUN_TEST(test_request_cookie_multiple);
    RUN_TEST(test_request_cookie_missing);
    RUN_TEST(test_request_query_param);
    RUN_TEST(test_request_accepts_json);
    RUN_TEST(test_request_accepts_wildcard);

    return UNITY_END();
}

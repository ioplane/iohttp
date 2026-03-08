/**
 * @file test_io_multipart.c
 * @brief Unit tests for io_multipart RFC 7578 parser.
 */

#include "http/io_multipart.h"

#include <errno.h>
#include <string.h>

#include <unity.h>

void setUp(void)
{
}
void tearDown(void)
{
}

/* ---- helpers ---- */

static const char *BOUNDARY = "----TestBoundary";

/* ---- test_multipart_single_field ---- */

void test_multipart_single_field(void)
{
    const char *body = "------TestBoundary\r\n"
                       "Content-Disposition: form-data; name=\"field1\"\r\n"
                       "\r\n"
                       "hello world\r\n"
                       "------TestBoundary--\r\n";

    io_multipart_config_t cfg;
    io_multipart_config_init(&cfg);

    io_multipart_part_t parts[8];
    uint32_t count = 8;

    int rc = io_multipart_parse((const uint8_t *)body, strlen(body), BOUNDARY, strlen(BOUNDARY),
                                &cfg, parts, &count);
    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_EQUAL_UINT32(1, count);

    TEST_ASSERT_EQUAL_size_t(6, parts[0].name_len);
    TEST_ASSERT_EQUAL_STRING_LEN("field1", parts[0].name, 6);
    TEST_ASSERT_NULL(parts[0].filename);
    TEST_ASSERT_NULL(parts[0].content_type);
    TEST_ASSERT_EQUAL_size_t(11, parts[0].data_len);
    TEST_ASSERT_EQUAL_STRING_LEN("hello world", (const char *)parts[0].data, 11);
}

/* ---- test_multipart_file_upload ---- */

void test_multipart_file_upload(void)
{
    const char *body = "------TestBoundary\r\n"
                       "Content-Disposition: form-data; name=\"file1\";"
                       " filename=\"test.txt\"\r\n"
                       "Content-Type: text/plain\r\n"
                       "\r\n"
                       "file contents here\r\n"
                       "------TestBoundary--\r\n";

    io_multipart_config_t cfg;
    io_multipart_config_init(&cfg);

    io_multipart_part_t parts[8];
    uint32_t count = 8;

    int rc = io_multipart_parse((const uint8_t *)body, strlen(body), BOUNDARY, strlen(BOUNDARY),
                                &cfg, parts, &count);
    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_EQUAL_UINT32(1, count);

    TEST_ASSERT_EQUAL_STRING_LEN("file1", parts[0].name, parts[0].name_len);
    TEST_ASSERT_NOT_NULL(parts[0].filename);
    TEST_ASSERT_EQUAL_STRING_LEN("test.txt", parts[0].filename, parts[0].filename_len);
    TEST_ASSERT_NOT_NULL(parts[0].content_type);
    TEST_ASSERT_EQUAL_STRING_LEN("text/plain", parts[0].content_type, parts[0].content_type_len);
    TEST_ASSERT_EQUAL_size_t(18, parts[0].data_len);
    TEST_ASSERT_EQUAL_STRING_LEN("file contents here", (const char *)parts[0].data, 18);
}

/* ---- test_multipart_multiple_parts ---- */

void test_multipart_multiple_parts(void)
{
    const char *body = "------TestBoundary\r\n"
                       "Content-Disposition: form-data; name=\"name\"\r\n"
                       "\r\n"
                       "Alice\r\n"
                       "------TestBoundary\r\n"
                       "Content-Disposition: form-data; name=\"age\"\r\n"
                       "\r\n"
                       "30\r\n"
                       "------TestBoundary\r\n"
                       "Content-Disposition: form-data; name=\"city\"\r\n"
                       "\r\n"
                       "Berlin\r\n"
                       "------TestBoundary\r\n"
                       "Content-Disposition: form-data; name=\"avatar\";"
                       " filename=\"pic.png\"\r\n"
                       "Content-Type: image/png\r\n"
                       "\r\n"
                       "PNG_DATA\r\n"
                       "------TestBoundary--\r\n";

    io_multipart_config_t cfg;
    io_multipart_config_init(&cfg);

    io_multipart_part_t parts[8];
    uint32_t count = 8;

    int rc = io_multipart_parse((const uint8_t *)body, strlen(body), BOUNDARY, strlen(BOUNDARY),
                                &cfg, parts, &count);
    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_EQUAL_UINT32(4, count);

    TEST_ASSERT_EQUAL_STRING_LEN("name", parts[0].name, parts[0].name_len);
    TEST_ASSERT_EQUAL_STRING_LEN("Alice", (const char *)parts[0].data, parts[0].data_len);

    TEST_ASSERT_EQUAL_STRING_LEN("age", parts[1].name, parts[1].name_len);
    TEST_ASSERT_EQUAL_STRING_LEN("30", (const char *)parts[1].data, parts[1].data_len);

    TEST_ASSERT_EQUAL_STRING_LEN("city", parts[2].name, parts[2].name_len);
    TEST_ASSERT_EQUAL_STRING_LEN("Berlin", (const char *)parts[2].data, parts[2].data_len);

    TEST_ASSERT_EQUAL_STRING_LEN("avatar", parts[3].name, parts[3].name_len);
    TEST_ASSERT_NOT_NULL(parts[3].filename);
    TEST_ASSERT_EQUAL_STRING_LEN("pic.png", parts[3].filename, parts[3].filename_len);
    TEST_ASSERT_EQUAL_STRING_LEN("PNG_DATA", (const char *)parts[3].data, parts[3].data_len);
}

/* ---- test_multipart_empty_body ---- */

void test_multipart_empty_body(void)
{
    io_multipart_config_t cfg;
    io_multipart_config_init(&cfg);

    io_multipart_part_t parts[8];
    uint32_t count = 8;

    int rc =
        io_multipart_parse((const uint8_t *)"", 0, BOUNDARY, strlen(BOUNDARY), &cfg, parts, &count);
    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_EQUAL_UINT32(0, count);
}

/* ---- test_multipart_missing_boundary ---- */

void test_multipart_missing_boundary(void)
{
    const char **boundary = nullptr;
    size_t boundary_len = 0;
    const char *bptr = nullptr;

    /* no boundary parameter */
    int rc = io_multipart_boundary("multipart/form-data", &bptr, &boundary_len);
    TEST_ASSERT_EQUAL_INT(-EINVAL, rc);

    /* not multipart at all */
    rc = io_multipart_boundary("application/json", &bptr, &boundary_len);
    TEST_ASSERT_EQUAL_INT(-EINVAL, rc);

    /* nullptr input */
    rc = io_multipart_boundary(nullptr, &bptr, &boundary_len);
    TEST_ASSERT_EQUAL_INT(-EINVAL, rc);

    /* valid boundary extraction */
    rc = io_multipart_boundary("multipart/form-data; boundary=----TestBoundary", &bptr,
                               &boundary_len);
    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_EQUAL_size_t(strlen("----TestBoundary"), boundary_len);
    TEST_ASSERT_EQUAL_STRING_LEN("----TestBoundary", bptr, boundary_len);

    (void)boundary; /* suppress unused */
}

/* ---- test_multipart_oversized_part ---- */

void test_multipart_oversized_part(void)
{
    /* build a body with a part larger than max_part_size */
    const char *header = "------TestBoundary\r\n"
                         "Content-Disposition: form-data; name=\"big\"\r\n"
                         "\r\n";
    const char *footer = "\r\n------TestBoundary--\r\n";

    size_t data_size = 1024; /* will set max to smaller */
    uint8_t buf[2048];
    size_t off = 0;
    memcpy(buf, header, strlen(header));
    off += strlen(header);
    memset(buf + off, 'X', data_size);
    off += data_size;
    memcpy(buf + off, footer, strlen(footer));
    off += strlen(footer);

    io_multipart_config_t cfg;
    io_multipart_config_init(&cfg);
    cfg.max_part_size = 512; /* smaller than data */

    io_multipart_part_t parts[8];
    uint32_t count = 8;

    int rc = io_multipart_parse(buf, off, BOUNDARY, strlen(BOUNDARY), &cfg, parts, &count);
    TEST_ASSERT_EQUAL_INT(-E2BIG, rc);
}

/* ---- test_multipart_too_many_parts ---- */

void test_multipart_too_many_parts(void)
{
    /* build body with 3 parts but set max to 2 */
    const char *body = "------TestBoundary\r\n"
                       "Content-Disposition: form-data; name=\"a\"\r\n"
                       "\r\n"
                       "1\r\n"
                       "------TestBoundary\r\n"
                       "Content-Disposition: form-data; name=\"b\"\r\n"
                       "\r\n"
                       "2\r\n"
                       "------TestBoundary\r\n"
                       "Content-Disposition: form-data; name=\"c\"\r\n"
                       "\r\n"
                       "3\r\n"
                       "------TestBoundary--\r\n";

    io_multipart_config_t cfg;
    io_multipart_config_init(&cfg);
    cfg.max_parts = 2;

    io_multipart_part_t parts[8];
    uint32_t count = 8;

    int rc = io_multipart_parse((const uint8_t *)body, strlen(body), BOUNDARY, strlen(BOUNDARY),
                                &cfg, parts, &count);
    TEST_ASSERT_EQUAL_INT(-E2BIG, rc);
}

/* ---- test_multipart_malformed ---- */

void test_multipart_malformed(void)
{
    io_multipart_config_t cfg;
    io_multipart_config_init(&cfg);

    io_multipart_part_t parts[8];
    uint32_t count = 8;

    /* no boundary delimiter at all */
    const char *garbage = "this is not multipart data at all";
    int rc = io_multipart_parse((const uint8_t *)garbage, strlen(garbage), BOUNDARY,
                                strlen(BOUNDARY), &cfg, parts, &count);
    TEST_ASSERT_EQUAL_INT(-EINVAL, rc);

    /* missing Content-Disposition header */
    count = 8;
    const char *no_disposition = "------TestBoundary\r\n"
                                 "Content-Type: text/plain\r\n"
                                 "\r\n"
                                 "data\r\n"
                                 "------TestBoundary--\r\n";
    rc = io_multipart_parse((const uint8_t *)no_disposition, strlen(no_disposition), BOUNDARY,
                            strlen(BOUNDARY), &cfg, parts, &count);
    TEST_ASSERT_EQUAL_INT(-EINVAL, rc);

    /* truncated — no closing boundary */
    count = 8;
    const char *truncated = "------TestBoundary\r\n"
                            "Content-Disposition: form-data; name=\"x\"\r\n"
                            "\r\n"
                            "data without closing boundary";
    rc = io_multipart_parse((const uint8_t *)truncated, strlen(truncated), BOUNDARY,
                            strlen(BOUNDARY), &cfg, parts, &count);
    TEST_ASSERT_EQUAL_INT(-EINVAL, rc);
}

/* ---- Test runner ---- */

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_multipart_single_field);
    RUN_TEST(test_multipart_file_upload);
    RUN_TEST(test_multipart_multiple_parts);
    RUN_TEST(test_multipart_empty_body);
    RUN_TEST(test_multipart_missing_boundary);
    RUN_TEST(test_multipart_oversized_part);
    RUN_TEST(test_multipart_too_many_parts);
    RUN_TEST(test_multipart_malformed);

    return UNITY_END();
}

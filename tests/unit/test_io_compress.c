/**
 * @file test_io_compress.c
 * @brief Unit tests for compression middleware.
 */

#include "static/io_compress.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <unity.h>

/* ---- Test helpers ---- */

static char tmp_dir[256];
static char tmp_file[512];
static char tmp_gz[512];
static char tmp_br[512];

static const char *resp_header(const io_response_t *resp, const char *name)
{
    size_t name_len = strlen(name);
    for (uint32_t i = 0; i < resp->header_count; i++) {
        if (resp->headers[i].name_len == name_len &&
            strncasecmp(resp->headers[i].name, name, name_len) == 0) {
            return resp->headers[i].value;
        }
    }
    return nullptr;
}

static void set_request_header(io_request_t *req, const char *name,
                               const char *value)
{
    uint32_t idx = req->header_count;
    req->headers[idx].name = name;
    req->headers[idx].name_len = strlen(name);
    req->headers[idx].value = value;
    req->headers[idx].value_len = strlen(value);
    req->header_count++;
}

static void create_file(const char *path, const char *content)
{
    FILE *f = fopen(path, "w");
    if (f != nullptr) {
        fputs(content, f);
        fclose(f);
    }
}

void setUp(void)
{
    snprintf(tmp_dir, sizeof(tmp_dir), "/tmp/iohttp_compress_test_XXXXXX");
    char *result = mkdtemp(tmp_dir);
    TEST_ASSERT_NOT_NULL(result);

    snprintf(tmp_file, sizeof(tmp_file), "%s/test.js", tmp_dir);
    snprintf(tmp_gz, sizeof(tmp_gz), "%s/test.js.gz", tmp_dir);
    snprintf(tmp_br, sizeof(tmp_br), "%s/test.js.br", tmp_dir);

    create_file(tmp_file, "console.log('hello');");
    create_file(tmp_gz, "fake gzip data");
    create_file(tmp_br, "fake brotli data");
}

void tearDown(void)
{
    unlink(tmp_gz);
    unlink(tmp_br);
    unlink(tmp_file);
    rmdir(tmp_dir);
}

/* ---- Negotiation tests ---- */

void test_compress_negotiate_gzip(void)
{
    io_encoding_t enc = io_compress_negotiate("gzip");
    TEST_ASSERT_EQUAL(IO_ENCODING_GZIP, enc);
}

void test_compress_negotiate_br(void)
{
    io_encoding_t enc = io_compress_negotiate("br");
    TEST_ASSERT_EQUAL(IO_ENCODING_BROTLI, enc);
}

void test_compress_negotiate_both(void)
{
    io_encoding_t enc = io_compress_negotiate("gzip, br");
    TEST_ASSERT_EQUAL(IO_ENCODING_BROTLI, enc);
}

void test_compress_negotiate_none(void)
{
    io_encoding_t enc = io_compress_negotiate("identity");
    TEST_ASSERT_EQUAL(IO_ENCODING_NONE, enc);
}

void test_compress_negotiate_qvalue(void)
{
    io_encoding_t enc = io_compress_negotiate("gzip;q=0.5, br;q=1.0");
    TEST_ASSERT_EQUAL(IO_ENCODING_BROTLI, enc);
}

/* ---- Precompressed file tests ---- */

void test_compress_precompressed_gz(void)
{
    char out[512];
    bool found = io_compress_precompressed(tmp_file, IO_ENCODING_GZIP,
                                           out, sizeof(out));
    TEST_ASSERT_TRUE(found);
    TEST_ASSERT_EQUAL_STRING(tmp_gz, out);
}

void test_compress_precompressed_br(void)
{
    char out[512];
    bool found = io_compress_precompressed(tmp_file, IO_ENCODING_BROTLI,
                                           out, sizeof(out));
    TEST_ASSERT_TRUE(found);
    TEST_ASSERT_EQUAL_STRING(tmp_br, out);
}

/* ---- Compress response test ---- */

void test_compress_below_min_size(void)
{
    io_compress_config_t cfg;
    io_compress_config_init(&cfg);
    cfg.min_size = 1024; /* default, body will be smaller */

    io_request_t req;
    io_request_init(&req);
    set_request_header(&req, "Accept-Encoding", "gzip, br");

    io_response_t resp;
    int rc = io_response_init(&resp);
    TEST_ASSERT_EQUAL(0, rc);

    /* set a small body (below min_size) */
    const char *small = "hello world";
    rc = io_response_set_body(&resp, (const uint8_t *)small,
                              strlen(small));
    TEST_ASSERT_EQUAL(0, rc);

    rc = io_compress_response(&cfg, &req, &resp);
    TEST_ASSERT_EQUAL(0, rc);

    /* body should NOT be compressed — no Content-Encoding set */
    const char *ce = resp_header(&resp, "Content-Encoding");
    TEST_ASSERT_NULL(ce);

    /* Vary should still be set */
    const char *vary = resp_header(&resp, "Vary");
    TEST_ASSERT_NOT_NULL(vary);
    TEST_ASSERT_EQUAL_STRING("Accept-Encoding", vary);

    /* body unchanged */
    TEST_ASSERT_EQUAL(strlen(small), resp.body_len);

    io_response_destroy(&resp);
}

/* ---- Runner ---- */

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_compress_negotiate_gzip);
    RUN_TEST(test_compress_negotiate_br);
    RUN_TEST(test_compress_negotiate_both);
    RUN_TEST(test_compress_negotiate_none);
    RUN_TEST(test_compress_negotiate_qvalue);
    RUN_TEST(test_compress_precompressed_gz);
    RUN_TEST(test_compress_precompressed_br);
    RUN_TEST(test_compress_below_min_size);
    return UNITY_END();
}

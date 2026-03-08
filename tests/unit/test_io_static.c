/**
 * @file test_io_static.c
 * @brief Unit tests for static file serving.
 */

#include "static/io_static.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <unity.h>

static char test_dir[256];

static void create_test_file(const char *name, const char *content)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", test_dir, name);
    FILE *fp = fopen(path, "wb");
    if (fp != nullptr) {
        fwrite(content, 1, strnlen(content, 65536), fp);
        fclose(fp);
    }
}

static void create_test_file_bin(const char *name, const uint8_t *data, size_t len)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", test_dir, name);
    FILE *fp = fopen(path, "wb");
    if (fp != nullptr) {
        fwrite(data, 1, len, fp);
        fclose(fp);
    }
}

static void rmrf(const char *dir)
{
    DIR *d = opendir(dir);
    if (d == nullptr) {
        return;
    }
    struct dirent *ent;
    while ((ent = readdir(d)) != nullptr) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
            continue;
        }
        char path[512];
        snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name);
        unlink(path);
    }
    closedir(d);
    rmdir(dir);
}

void setUp(void)
{
    snprintf(test_dir, sizeof(test_dir), "/tmp/iohttp_test_%d", getpid());
    mkdir(test_dir, 0755);
    create_test_file("index.html", "<html>test</html>");
    create_test_file("style.css", "body { color: red; }");
    create_test_file("app.js", "console.log('hello');");
    create_test_file("data.json", "{\"key\":\"value\"}");
    uint8_t wasm_data[] = {0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00};
    create_test_file_bin("image.wasm", wasm_data, sizeof(wasm_data));
}

void tearDown(void)
{
    rmrf(test_dir);
}

/* ---- Helper to set up a request with a path ---- */

static void init_request(io_request_t *req, const char *path)
{
    io_request_init(req);
    req->method = IO_METHOD_GET;
    req->path = path;
    req->path_len = strnlen(path, 4096);
}

static void add_header(io_request_t *req, const char *name, const char *value)
{
    uint32_t idx = req->header_count;
    if (idx >= IO_MAX_HEADERS) {
        return;
    }
    req->headers[idx].name = name;
    req->headers[idx].name_len = strnlen(name, 256);
    req->headers[idx].value = value;
    req->headers[idx].value_len = strnlen(value, 4096);
    req->header_count++;
}

/* ---- Tests ---- */

void test_static_serve_file(void)
{
    io_static_config_t cfg;
    io_static_config_init(&cfg);
    cfg.root_dir = test_dir;

    io_request_t req;
    init_request(&req, "/index.html");

    io_response_t resp;
    TEST_ASSERT_EQUAL_INT(0, io_response_init(&resp));

    int rc = io_static_serve(&cfg, &req, &resp);
    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_EQUAL_UINT16(200, resp.status);
    TEST_ASSERT_EQUAL_size_t(17, resp.body_len);
    TEST_ASSERT_EQUAL_MEMORY("<html>test</html>", resp.body, resp.body_len);

    io_response_destroy(&resp);
}

void test_static_mime_html(void)
{
    const char *mime = io_mime_type("/index.html", 11);
    TEST_ASSERT_EQUAL_STRING("text/html", mime);
}

void test_static_mime_js(void)
{
    const char *mime = io_mime_type("/app.js", 7);
    TEST_ASSERT_EQUAL_STRING("text/javascript", mime);
}

void test_static_mime_css(void)
{
    const char *mime = io_mime_type("/style.css", 10);
    TEST_ASSERT_EQUAL_STRING("text/css", mime);
}

void test_static_mime_wasm(void)
{
    const char *mime = io_mime_type("/image.wasm", 11);
    TEST_ASSERT_EQUAL_STRING("application/wasm", mime);
}

void test_static_etag_match(void)
{
    io_static_config_t cfg;
    io_static_config_init(&cfg);
    cfg.root_dir = test_dir;

    io_request_t req;
    init_request(&req, "/index.html");

    io_response_t resp;
    TEST_ASSERT_EQUAL_INT(0, io_response_init(&resp));

    /* first serve to get the ETag */
    int rc = io_static_serve(&cfg, &req, &resp);
    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_EQUAL_UINT16(200, resp.status);

    /* find the ETag header */
    const char *etag_val = nullptr;
    for (uint32_t i = 0; i < resp.header_count; i++) {
        if (strncasecmp(resp.headers[i].name, "ETag", 4) == 0) {
            etag_val = resp.headers[i].value;
            break;
        }
    }
    TEST_ASSERT_NOT_NULL(etag_val);

    /* copy etag for the second request */
    char etag_copy[64];
    snprintf(etag_copy, sizeof(etag_copy), "%s", etag_val);

    io_response_destroy(&resp);

    /* second request with If-None-Match */
    io_request_t req2;
    init_request(&req2, "/index.html");
    add_header(&req2, "If-None-Match", etag_copy);

    io_response_t resp2;
    TEST_ASSERT_EQUAL_INT(0, io_response_init(&resp2));

    rc = io_static_serve(&cfg, &req2, &resp2);
    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_EQUAL_UINT16(304, resp2.status);

    io_response_destroy(&resp2);
}

void test_static_etag_mismatch(void)
{
    io_static_config_t cfg;
    io_static_config_init(&cfg);
    cfg.root_dir = test_dir;

    io_request_t req;
    init_request(&req, "/index.html");
    add_header(&req, "If-None-Match", "\"bogus-etag\"");

    io_response_t resp;
    TEST_ASSERT_EQUAL_INT(0, io_response_init(&resp));

    int rc = io_static_serve(&cfg, &req, &resp);
    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_EQUAL_UINT16(200, resp.status);

    /* verify ETag header is present */
    bool found_etag = false;
    for (uint32_t i = 0; i < resp.header_count; i++) {
        if (strncasecmp(resp.headers[i].name, "ETag", 4) == 0) {
            found_etag = true;
            break;
        }
    }
    TEST_ASSERT_TRUE(found_etag);

    io_response_destroy(&resp);
}

void test_static_range_request(void)
{
    io_static_config_t cfg;
    io_static_config_init(&cfg);
    cfg.root_dir = test_dir;

    io_request_t req;
    init_request(&req, "/index.html");
    add_header(&req, "Range", "bytes=0-4");

    io_response_t resp;
    TEST_ASSERT_EQUAL_INT(0, io_response_init(&resp));

    int rc = io_static_serve(&cfg, &req, &resp);
    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_EQUAL_UINT16(206, resp.status);
    TEST_ASSERT_EQUAL_size_t(5, resp.body_len);
    TEST_ASSERT_EQUAL_MEMORY("<html", resp.body, 5);

    /* check Content-Range header */
    bool found_cr = false;
    for (uint32_t i = 0; i < resp.header_count; i++) {
        if (strncasecmp(resp.headers[i].name, "Content-Range", 13) == 0) {
            found_cr = true;
            TEST_ASSERT_EQUAL_STRING("bytes 0-4/17", resp.headers[i].value);
            break;
        }
    }
    TEST_ASSERT_TRUE(found_cr);

    io_response_destroy(&resp);
}

void test_static_path_traversal(void)
{
    io_static_config_t cfg;
    io_static_config_init(&cfg);
    cfg.root_dir = test_dir;

    io_request_t req;
    init_request(&req, "/../../etc/passwd");

    io_response_t resp;
    TEST_ASSERT_EQUAL_INT(0, io_response_init(&resp));

    int rc = io_static_serve(&cfg, &req, &resp);
    TEST_ASSERT_EQUAL_INT(-EACCES, rc);

    io_response_destroy(&resp);
}

void test_static_not_found(void)
{
    io_static_config_t cfg;
    io_static_config_init(&cfg);
    cfg.root_dir = test_dir;

    io_request_t req;
    init_request(&req, "/nonexistent.txt");

    io_response_t resp;
    TEST_ASSERT_EQUAL_INT(0, io_response_init(&resp));

    int rc = io_static_serve(&cfg, &req, &resp);
    TEST_ASSERT_EQUAL_INT(-ENOENT, rc);

    io_response_destroy(&resp);
}

/* ---- Test runner ---- */

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_static_serve_file);
    RUN_TEST(test_static_mime_html);
    RUN_TEST(test_static_mime_js);
    RUN_TEST(test_static_mime_css);
    RUN_TEST(test_static_mime_wasm);
    RUN_TEST(test_static_etag_match);
    RUN_TEST(test_static_etag_mismatch);
    RUN_TEST(test_static_range_request);
    RUN_TEST(test_static_path_traversal);
    RUN_TEST(test_static_not_found);

    return UNITY_END();
}

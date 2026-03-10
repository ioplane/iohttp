/**
 * @file test_io_spa.c
 * @brief Unit tests for SPA fallback serving.
 */

#include "static/ioh_spa.h"

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
    snprintf(test_dir, sizeof(test_dir), "/tmp/iohttp_spa_%d", getpid());
    mkdir(test_dir, 0755);
    create_test_file("index.html", "<html>SPA</html>");
    create_test_file("style.css", "body { color: red; }");
    create_test_file("app.abc12345.js", "console.log('hashed');");
    create_test_file("logo.png", "PNG_DATA");
}

void tearDown(void)
{
    rmrf(test_dir);
}

/* ---- Helper ---- */

static void init_request(ioh_request_t *req, const char *path)
{
    ioh_request_init(req);
    req->method = IOH_METHOD_GET;
    req->path = path;
    req->path_len = strnlen(path, 4096);
}

static const char *find_header(const ioh_response_t *resp, const char *name)
{
    for (uint32_t i = 0; i < resp->header_count; i++) {
        if (strncasecmp(resp->headers[i].name, name, resp->headers[i].name_len) == 0) {
            return resp->headers[i].value;
        }
    }
    return nullptr;
}

/* ---- Tests ---- */

void test_spa_existing_file(void)
{
    ioh_spa_config_t cfg;
    ioh_spa_config_init(&cfg);
    cfg.root_dir = test_dir;

    ioh_request_t req;
    init_request(&req, "/style.css");

    ioh_response_t resp;
    TEST_ASSERT_EQUAL_INT(0, ioh_response_init(&resp));

    int rc = ioh_spa_serve(&cfg, &req, &resp);
    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_EQUAL_UINT16(200, resp.status);

    const char *ct = find_header(&resp, "Content-Type");
    TEST_ASSERT_NOT_NULL(ct);
    TEST_ASSERT_EQUAL_STRING("text/css", ct);

    ioh_response_destroy(&resp);
}

void test_spa_fallback_to_index(void)
{
    ioh_spa_config_t cfg;
    ioh_spa_config_init(&cfg);
    cfg.root_dir = test_dir;

    ioh_request_t req;
    init_request(&req, "/app/route");

    ioh_response_t resp;
    TEST_ASSERT_EQUAL_INT(0, ioh_response_init(&resp));

    int rc = ioh_spa_serve(&cfg, &req, &resp);
    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_EQUAL_UINT16(200, resp.status);
    TEST_ASSERT_EQUAL_MEMORY("<html>SPA</html>", resp.body, resp.body_len);

    ioh_response_destroy(&resp);
}

void test_spa_api_prefix_excluded(void)
{
    const char *prefixes[] = {"/api/"};

    ioh_spa_config_t cfg;
    ioh_spa_config_init(&cfg);
    cfg.root_dir = test_dir;
    cfg.api_prefixes = prefixes;
    cfg.api_prefix_count = 1;

    ioh_request_t req;
    init_request(&req, "/api/data");

    ioh_response_t resp;
    TEST_ASSERT_EQUAL_INT(0, ioh_response_init(&resp));

    int rc = ioh_spa_serve(&cfg, &req, &resp);
    TEST_ASSERT_EQUAL_INT(-ENOENT, rc);

    ioh_response_destroy(&resp);
}

void test_spa_hashed_asset_immutable(void)
{
    ioh_spa_config_t cfg;
    ioh_spa_config_init(&cfg);
    cfg.root_dir = test_dir;

    ioh_request_t req;
    init_request(&req, "/app.abc12345.js");

    ioh_response_t resp;
    TEST_ASSERT_EQUAL_INT(0, ioh_response_init(&resp));

    int rc = ioh_spa_serve(&cfg, &req, &resp);
    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_EQUAL_UINT16(200, resp.status);

    const char *cc = find_header(&resp, "Cache-Control");
    TEST_ASSERT_NOT_NULL(cc);
    TEST_ASSERT_NOT_NULL(strstr(cc, "immutable"));
    TEST_ASSERT_NOT_NULL(strstr(cc, "31536000"));

    ioh_response_destroy(&resp);
}

void test_spa_index_no_cache(void)
{
    ioh_spa_config_t cfg;
    ioh_spa_config_init(&cfg);
    cfg.root_dir = test_dir;

    ioh_request_t req;
    init_request(&req, "/some/page");

    ioh_response_t resp;
    TEST_ASSERT_EQUAL_INT(0, ioh_response_init(&resp));

    int rc = ioh_spa_serve(&cfg, &req, &resp);
    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_EQUAL_UINT16(200, resp.status);

    const char *cc = find_header(&resp, "Cache-Control");
    TEST_ASSERT_NOT_NULL(cc);
    TEST_ASSERT_EQUAL_STRING("no-cache", cc);

    ioh_response_destroy(&resp);
}

void test_spa_nested_route(void)
{
    ioh_spa_config_t cfg;
    ioh_spa_config_init(&cfg);
    cfg.root_dir = test_dir;

    ioh_request_t req;
    init_request(&req, "/app/a/b/c");

    ioh_response_t resp;
    TEST_ASSERT_EQUAL_INT(0, ioh_response_init(&resp));

    int rc = ioh_spa_serve(&cfg, &req, &resp);
    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_EQUAL_UINT16(200, resp.status);
    TEST_ASSERT_EQUAL_MEMORY("<html>SPA</html>", resp.body, resp.body_len);

    ioh_response_destroy(&resp);
}

void test_spa_multiple_api_prefixes(void)
{
    const char *prefixes[] = {"/api/", "/ws/"};

    ioh_spa_config_t cfg;
    ioh_spa_config_init(&cfg);
    cfg.root_dir = test_dir;
    cfg.api_prefixes = prefixes;
    cfg.api_prefix_count = 2;

    ioh_request_t req;
    init_request(&req, "/ws/connect");

    ioh_response_t resp;
    TEST_ASSERT_EQUAL_INT(0, ioh_response_init(&resp));

    int rc = ioh_spa_serve(&cfg, &req, &resp);
    TEST_ASSERT_EQUAL_INT(-ENOENT, rc);

    /* also verify /api/ is excluded */
    ioh_response_reset(&resp);
    init_request(&req, "/api/users");

    rc = ioh_spa_serve(&cfg, &req, &resp);
    TEST_ASSERT_EQUAL_INT(-ENOENT, rc);

    ioh_response_destroy(&resp);
}

void test_spa_file_with_extension(void)
{
    ioh_spa_config_t cfg;
    ioh_spa_config_init(&cfg);
    cfg.root_dir = test_dir;

    ioh_request_t req;
    init_request(&req, "/missing.css");

    ioh_response_t resp;
    TEST_ASSERT_EQUAL_INT(0, ioh_response_init(&resp));

    int rc = ioh_spa_serve(&cfg, &req, &resp);
    TEST_ASSERT_EQUAL_INT(-ENOENT, rc);

    ioh_response_destroy(&resp);
}

/* ---- Test runner ---- */

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_spa_existing_file);
    RUN_TEST(test_spa_fallback_to_index);
    RUN_TEST(test_spa_api_prefix_excluded);
    RUN_TEST(test_spa_hashed_asset_immutable);
    RUN_TEST(test_spa_index_no_cache);
    RUN_TEST(test_spa_nested_route);
    RUN_TEST(test_spa_multiple_api_prefixes);
    RUN_TEST(test_spa_file_with_extension);

    return UNITY_END();
}

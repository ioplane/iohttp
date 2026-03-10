/**
 * @file test_io_router.c
 * @brief Unit tests for public router API.
 */

#include "router/ioh_router.h"

#include <errno.h>
#include <string.h>

#include <unity.h>

void setUp(void)
{
}

void tearDown(void)
{
}

/* ---- Stub handlers with distinct return values for identity checks ---- */

static int handler_a(ioh_ctx_t *c)
{
    (void)c;
    return 0;
}

static int handler_b(ioh_ctx_t *c)
{
    (void)c;
    return 1;
}

/* ---- 1. Core routing ---- */

void test_router_create_destroy(void)
{
    ioh_router_t *r = ioh_router_create();
    TEST_ASSERT_NOT_NULL(r);
    ioh_router_destroy(r);

    /* nullptr is safe */
    ioh_router_destroy(nullptr);
}

void test_router_get_exact(void)
{
    ioh_router_t *r = ioh_router_create();
    TEST_ASSERT_NOT_NULL(r);

    int rc = ioh_router_get(r, "/health", handler_a);
    TEST_ASSERT_EQUAL_INT(0, rc);

    ioh_route_match_t m = ioh_router_dispatch(r, IOH_METHOD_GET, "/health", strlen("/health"));
    TEST_ASSERT_EQUAL_INT(IOH_MATCH_FOUND, m.status);
    TEST_ASSERT_EQUAL_PTR(handler_a, m.handler);

    ioh_router_destroy(r);
}

void test_router_post_exact(void)
{
    ioh_router_t *r = ioh_router_create();
    TEST_ASSERT_NOT_NULL(r);

    int rc = ioh_router_post(r, "/users", handler_b);
    TEST_ASSERT_EQUAL_INT(0, rc);

    ioh_route_match_t m = ioh_router_dispatch(r, IOH_METHOD_POST, "/users", strlen("/users"));
    TEST_ASSERT_EQUAL_INT(IOH_MATCH_FOUND, m.status);
    TEST_ASSERT_EQUAL_PTR(handler_b, m.handler);

    ioh_router_destroy(r);
}

void test_router_path_param(void)
{
    ioh_router_t *r = ioh_router_create();
    TEST_ASSERT_NOT_NULL(r);

    int rc = ioh_router_get(r, "/users/:id", handler_a);
    TEST_ASSERT_EQUAL_INT(0, rc);

    ioh_route_match_t m = ioh_router_dispatch(r, IOH_METHOD_GET, "/users/42", strlen("/users/42"));
    TEST_ASSERT_EQUAL_INT(IOH_MATCH_FOUND, m.status);
    TEST_ASSERT_EQUAL_PTR(handler_a, m.handler);
    TEST_ASSERT_EQUAL_UINT32(1, m.param_count);
    TEST_ASSERT_EQUAL_STRING_LEN("id", m.params[0].name, m.params[0].name_len);
    TEST_ASSERT_EQUAL_STRING_LEN("42", m.params[0].value, m.params[0].value_len);

    ioh_router_destroy(r);
}

void test_router_multiple_params(void)
{
    ioh_router_t *r = ioh_router_create();
    TEST_ASSERT_NOT_NULL(r);

    int rc = ioh_router_get(r, "/users/:uid/posts/:pid", handler_a);
    TEST_ASSERT_EQUAL_INT(0, rc);

    ioh_route_match_t m =
        ioh_router_dispatch(r, IOH_METHOD_GET, "/users/7/posts/99", strlen("/users/7/posts/99"));
    TEST_ASSERT_EQUAL_INT(IOH_MATCH_FOUND, m.status);
    TEST_ASSERT_EQUAL_UINT32(2, m.param_count);
    TEST_ASSERT_EQUAL_STRING_LEN("uid", m.params[0].name, m.params[0].name_len);
    TEST_ASSERT_EQUAL_STRING_LEN("7", m.params[0].value, m.params[0].value_len);
    TEST_ASSERT_EQUAL_STRING_LEN("pid", m.params[1].name, m.params[1].name_len);
    TEST_ASSERT_EQUAL_STRING_LEN("99", m.params[1].value, m.params[1].value_len);

    ioh_router_destroy(r);
}

void test_router_wildcard(void)
{
    ioh_router_t *r = ioh_router_create();
    TEST_ASSERT_NOT_NULL(r);

    int rc = ioh_router_get(r, "/static/*path", handler_a);
    TEST_ASSERT_EQUAL_INT(0, rc);

    ioh_route_match_t m =
        ioh_router_dispatch(r, IOH_METHOD_GET, "/static/css/app.css", strlen("/static/css/app.css"));
    TEST_ASSERT_EQUAL_INT(IOH_MATCH_FOUND, m.status);
    TEST_ASSERT_EQUAL_UINT32(1, m.param_count);
    TEST_ASSERT_EQUAL_STRING_LEN("path", m.params[0].name, m.params[0].name_len);
    TEST_ASSERT_EQUAL_STRING_LEN("css/app.css", m.params[0].value, m.params[0].value_len);

    ioh_router_destroy(r);
}

void test_router_method_dispatch(void)
{
    ioh_router_t *r = ioh_router_create();
    TEST_ASSERT_NOT_NULL(r);

    int rc = ioh_router_get(r, "/x", handler_a);
    TEST_ASSERT_EQUAL_INT(0, rc);
    rc = ioh_router_post(r, "/x", handler_b);
    TEST_ASSERT_EQUAL_INT(0, rc);

    ioh_route_match_t m = ioh_router_dispatch(r, IOH_METHOD_GET, "/x", strlen("/x"));
    TEST_ASSERT_EQUAL_INT(IOH_MATCH_FOUND, m.status);
    TEST_ASSERT_EQUAL_PTR(handler_a, m.handler);

    m = ioh_router_dispatch(r, IOH_METHOD_POST, "/x", strlen("/x"));
    TEST_ASSERT_EQUAL_INT(IOH_MATCH_FOUND, m.status);
    TEST_ASSERT_EQUAL_PTR(handler_b, m.handler);

    ioh_router_destroy(r);
}

/* ---- 2. Priority ---- */

void test_router_priority_static_over_param(void)
{
    ioh_router_t *r = ioh_router_create();
    TEST_ASSERT_NOT_NULL(r);

    int rc = ioh_router_get(r, "/users/:id", handler_a);
    TEST_ASSERT_EQUAL_INT(0, rc);
    rc = ioh_router_get(r, "/users/me", handler_b);
    TEST_ASSERT_EQUAL_INT(0, rc);

    ioh_route_match_t m = ioh_router_dispatch(r, IOH_METHOD_GET, "/users/me", strlen("/users/me"));
    TEST_ASSERT_EQUAL_INT(IOH_MATCH_FOUND, m.status);
    TEST_ASSERT_EQUAL_PTR(handler_b, m.handler);
    TEST_ASSERT_EQUAL_UINT32(0, m.param_count);

    ioh_router_destroy(r);
}

void test_router_priority_param_over_wildcard(void)
{
    ioh_router_t *r = ioh_router_create();
    TEST_ASSERT_NOT_NULL(r);

    int rc = ioh_router_get(r, "/files/*path", handler_a);
    TEST_ASSERT_EQUAL_INT(0, rc);
    rc = ioh_router_get(r, "/files/:name", handler_b);
    TEST_ASSERT_EQUAL_INT(0, rc);

    ioh_route_match_t m =
        ioh_router_dispatch(r, IOH_METHOD_GET, "/files/readme", strlen("/files/readme"));
    TEST_ASSERT_EQUAL_INT(IOH_MATCH_FOUND, m.status);
    TEST_ASSERT_EQUAL_PTR(handler_b, m.handler);

    ioh_router_destroy(r);
}

/* ---- 3. Auto-behaviors ---- */

void test_router_auto_405(void)
{
    ioh_router_t *r = ioh_router_create();
    TEST_ASSERT_NOT_NULL(r);

    int rc = ioh_router_get(r, "/health", handler_a);
    TEST_ASSERT_EQUAL_INT(0, rc);

    ioh_route_match_t m = ioh_router_dispatch(r, IOH_METHOD_DELETE, "/health", strlen("/health"));
    TEST_ASSERT_EQUAL_INT(IOH_MATCH_METHOD_NOT_ALLOWED, m.status);
    TEST_ASSERT_NOT_EQUAL(0, strlen(m.allowed_methods));
    /* "GET" should appear in allowed_methods */
    TEST_ASSERT_NOT_NULL(strstr(m.allowed_methods, "GET"));

    ioh_router_destroy(r);
}

void test_router_auto_head(void)
{
    ioh_router_t *r = ioh_router_create();
    TEST_ASSERT_NOT_NULL(r);

    int rc = ioh_router_get(r, "/health", handler_a);
    TEST_ASSERT_EQUAL_INT(0, rc);

    ioh_route_match_t m = ioh_router_dispatch(r, IOH_METHOD_HEAD, "/health", strlen("/health"));
    TEST_ASSERT_EQUAL_INT(IOH_MATCH_FOUND, m.status);
    TEST_ASSERT_EQUAL_PTR(handler_a, m.handler);

    ioh_router_destroy(r);
}

void test_router_trailing_slash_redirect(void)
{
    ioh_router_t *r = ioh_router_create();
    TEST_ASSERT_NOT_NULL(r);

    int rc = ioh_router_get(r, "/users", handler_a);
    TEST_ASSERT_EQUAL_INT(0, rc);

    /* Request /users/ (trailing slash) when /users exists */
    ioh_route_match_t m = ioh_router_dispatch(r, IOH_METHOD_GET, "/users/", strlen("/users/"));
    TEST_ASSERT_EQUAL_INT(IOH_MATCH_REDIRECT, m.status);
    TEST_ASSERT_EQUAL_STRING("/users", m.redirect_path);

    ioh_router_destroy(r);
}

void test_router_path_correction(void)
{
    ioh_router_t *r = ioh_router_create();
    TEST_ASSERT_NOT_NULL(r);

    int rc = ioh_router_get(r, "/foo", handler_a);
    TEST_ASSERT_EQUAL_INT(0, rc);

    /* //foo should normalize to /foo and match */
    ioh_route_match_t m = ioh_router_dispatch(r, IOH_METHOD_GET, "//foo", strlen("//foo"));
    TEST_ASSERT_EQUAL_INT(IOH_MATCH_FOUND, m.status);
    TEST_ASSERT_EQUAL_PTR(handler_a, m.handler);

    ioh_router_destroy(r);
}

/* ---- 4. Security ---- */

void test_router_path_normalization(void)
{
    /* //foo/../bar should normalize to /bar */
    char out[256];
    size_t out_len = 0;
    int rc = ioh_path_normalize("//foo/../bar", strlen("//foo/../bar"), out, sizeof(out), &out_len);
    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_EQUAL_STRING("/bar", out);
    TEST_ASSERT_EQUAL_size_t(4, out_len);
}

void test_router_path_traversal_blocked(void)
{
    char out[256];
    size_t out_len = 0;
    int rc =
        ioh_path_normalize("/../etc/passwd", strlen("/../etc/passwd"), out, sizeof(out), &out_len);
    TEST_ASSERT_EQUAL_INT(-EINVAL, rc);
}

void test_router_null_byte_blocked(void)
{
    ioh_router_t *r = ioh_router_create();
    TEST_ASSERT_NOT_NULL(r);

    int rc = ioh_router_get(r, "/secret", handler_a);
    TEST_ASSERT_EQUAL_INT(0, rc);

    /* Path with embedded NUL byte -- pass raw bytes, length includes NUL */
    char evil_path[] = "/sec\0ret";
    ioh_route_match_t m = ioh_router_dispatch(r, IOH_METHOD_GET, evil_path, sizeof(evil_path) - 1);
    TEST_ASSERT_EQUAL_INT(IOH_MATCH_NOT_FOUND, m.status);

    /* Path with URL-encoded NUL */
    ioh_route_match_t m2 = ioh_router_dispatch(r, IOH_METHOD_GET, "/sec%00ret", strlen("/sec%00ret"));
    TEST_ASSERT_EQUAL_INT(IOH_MATCH_NOT_FOUND, m2.status);

    ioh_router_destroy(r);
}

/* ---- 5. Param lifetime (regression: stack-use-after-return) ---- */

void test_router_param_lifetime_after_dispatch(void)
{
    ioh_router_t *r = ioh_router_create();
    TEST_ASSERT_NOT_NULL(r);

    int rc = ioh_router_get(r, "/users/:id", handler_a);
    TEST_ASSERT_EQUAL_INT(0, rc);

    ioh_route_match_t m = ioh_router_dispatch(r, IOH_METHOD_GET, "/users/42", strlen("/users/42"));
    TEST_ASSERT_EQUAL_INT(IOH_MATCH_FOUND, m.status);
    TEST_ASSERT_EQUAL_UINT32(1, m.param_count);

    /*
     * After dispatch returns, the normalized[] stack buffer is gone.
     * Param values must still be valid (stored in param_storage).
     */
    TEST_ASSERT_EQUAL_size_t(2, m.params[0].value_len);
    TEST_ASSERT_EQUAL_STRING_LEN("42", m.params[0].value, m.params[0].value_len);

    /* Verify NUL termination in param_storage */
    TEST_ASSERT_EQUAL_CHAR('\0', m.params[0].value[m.params[0].value_len]);

    ioh_router_destroy(r);
}

void test_router_multi_param_lifetime_after_dispatch(void)
{
    ioh_router_t *r = ioh_router_create();
    TEST_ASSERT_NOT_NULL(r);

    int rc = ioh_router_get(r, "/users/:uid/posts/:pid", handler_a);
    TEST_ASSERT_EQUAL_INT(0, rc);

    ioh_route_match_t m =
        ioh_router_dispatch(r, IOH_METHOD_GET, "/users/7/posts/99", strlen("/users/7/posts/99"));
    TEST_ASSERT_EQUAL_INT(IOH_MATCH_FOUND, m.status);
    TEST_ASSERT_EQUAL_UINT32(2, m.param_count);

    /* Both param values must survive after dispatch returns */
    TEST_ASSERT_EQUAL_STRING_LEN("7", m.params[0].value, m.params[0].value_len);
    TEST_ASSERT_EQUAL_STRING_LEN("99", m.params[1].value, m.params[1].value_len);

    /* Values must be NUL-terminated */
    TEST_ASSERT_EQUAL_CHAR('\0', m.params[0].value[m.params[0].value_len]);
    TEST_ASSERT_EQUAL_CHAR('\0', m.params[1].value[m.params[1].value_len]);

    /* Values must be stored in param_storage, not pointing at external memory */
    TEST_ASSERT_TRUE(m.params[0].value >= m.param_storage &&
                     m.params[0].value < m.param_storage + IOH_MAX_PARAM_STORAGE);
    TEST_ASSERT_TRUE(m.params[1].value >= m.param_storage &&
                     m.params[1].value < m.param_storage + IOH_MAX_PARAM_STORAGE);

    ioh_router_destroy(r);
}

void test_router_wildcard_param_lifetime_after_dispatch(void)
{
    ioh_router_t *r = ioh_router_create();
    TEST_ASSERT_NOT_NULL(r);

    int rc = ioh_router_get(r, "/static/*path", handler_a);
    TEST_ASSERT_EQUAL_INT(0, rc);

    ioh_route_match_t m =
        ioh_router_dispatch(r, IOH_METHOD_GET, "/static/css/app.css", strlen("/static/css/app.css"));
    TEST_ASSERT_EQUAL_INT(IOH_MATCH_FOUND, m.status);
    TEST_ASSERT_EQUAL_UINT32(1, m.param_count);

    /* Wildcard value must survive after dispatch returns */
    TEST_ASSERT_EQUAL_STRING_LEN("css/app.css", m.params[0].value, m.params[0].value_len);
    TEST_ASSERT_EQUAL_CHAR('\0', m.params[0].value[m.params[0].value_len]);

    ioh_router_destroy(r);
}

/* ---- 6. Conflict detection ---- */

void test_router_conflict_same_level(void)
{
    ioh_router_t *r = ioh_router_create();
    TEST_ASSERT_NOT_NULL(r);

    int rc = ioh_router_get(r, "/:id", handler_a);
    TEST_ASSERT_EQUAL_INT(0, rc);

    /* Different param name at same level in same method tree -> -EEXIST */
    rc = ioh_router_get(r, "/:name", handler_b);
    TEST_ASSERT_EQUAL_INT(-EEXIST, rc);

    ioh_router_destroy(r);
}

void test_router_no_conflict_diff_method(void)
{
    ioh_router_t *r = ioh_router_create();
    TEST_ASSERT_NOT_NULL(r);

    int rc = ioh_router_get(r, "/:id", handler_a);
    TEST_ASSERT_EQUAL_INT(0, rc);

    /* Same pattern with different param name in different method -> OK */
    rc = ioh_router_post(r, "/:name", handler_b);
    TEST_ASSERT_EQUAL_INT(0, rc);

    ioh_router_destroy(r);
}

/* ---- main ---- */

int main(void)
{
    UNITY_BEGIN();

    /* Core routing */
    RUN_TEST(test_router_create_destroy);
    RUN_TEST(test_router_get_exact);
    RUN_TEST(test_router_post_exact);
    RUN_TEST(test_router_path_param);
    RUN_TEST(test_router_multiple_params);
    RUN_TEST(test_router_wildcard);
    RUN_TEST(test_router_method_dispatch);

    /* Priority */
    RUN_TEST(test_router_priority_static_over_param);
    RUN_TEST(test_router_priority_param_over_wildcard);

    /* Auto-behaviors */
    RUN_TEST(test_router_auto_405);
    RUN_TEST(test_router_auto_head);
    RUN_TEST(test_router_trailing_slash_redirect);
    RUN_TEST(test_router_path_correction);

    /* Security */
    RUN_TEST(test_router_path_normalization);
    RUN_TEST(test_router_path_traversal_blocked);
    RUN_TEST(test_router_null_byte_blocked);

    /* Param lifetime (regression) */
    RUN_TEST(test_router_param_lifetime_after_dispatch);
    RUN_TEST(test_router_multi_param_lifetime_after_dispatch);
    RUN_TEST(test_router_wildcard_param_lifetime_after_dispatch);

    /* Conflict detection */
    RUN_TEST(test_router_conflict_same_level);
    RUN_TEST(test_router_no_conflict_diff_method);

    return UNITY_END();
}

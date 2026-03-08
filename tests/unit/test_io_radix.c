/**
 * @file test_io_radix.c
 * @brief Unit tests for internal radix trie URL pattern matching.
 */

#include "router/io_radix.h"

#include <errno.h>
#include <string.h>

#include <unity.h>

void setUp(void)
{
}

void tearDown(void)
{
}

/* ---- Dummy handlers (unique addresses for identity checks) ---- */

static int handler_a;
static int handler_b;
static int handler_c;
static int metadata_a;
static int metadata_b;

/* ---- 1. Lifecycle ---- */

void test_radix_create_destroy(void)
{
    io_radix_tree_t *tree = io_radix_create();
    TEST_ASSERT_NOT_NULL(tree);
    TEST_ASSERT_NOT_NULL(tree->root);
    io_radix_destroy(tree);

    /* nullptr is safe */
    io_radix_destroy(nullptr);
}

/* ---- 2. Insert static ---- */

void test_radix_insert_static(void)
{
    io_radix_tree_t *tree = io_radix_create();
    TEST_ASSERT_NOT_NULL(tree);

    int rc = io_radix_insert(tree, "/users/list", &handler_a, nullptr);
    TEST_ASSERT_EQUAL_INT(0, rc);

    io_radix_match_t match;
    rc = io_radix_lookup(tree, "/users/list", strlen("/users/list"), &match);
    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_EQUAL_PTR(&handler_a, match.handler);
    TEST_ASSERT_EQUAL_UINT32(0, match.param_count);

    io_radix_destroy(tree);
}

/* ---- 3. Insert param ---- */

void test_radix_insert_param(void)
{
    io_radix_tree_t *tree = io_radix_create();
    TEST_ASSERT_NOT_NULL(tree);

    int rc = io_radix_insert(tree, "/users/:id", &handler_a, nullptr);
    TEST_ASSERT_EQUAL_INT(0, rc);

    io_radix_match_t match;
    rc = io_radix_lookup(tree, "/users/42", strlen("/users/42"), &match);
    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_EQUAL_PTR(&handler_a, match.handler);
    TEST_ASSERT_EQUAL_UINT32(1, match.param_count);

    io_radix_destroy(tree);
}

/* ---- 4. Insert wildcard ---- */

void test_radix_insert_wildcard(void)
{
    io_radix_tree_t *tree = io_radix_create();
    TEST_ASSERT_NOT_NULL(tree);

    int rc = io_radix_insert(tree, "/static/*path", &handler_a, nullptr);
    TEST_ASSERT_EQUAL_INT(0, rc);

    io_radix_match_t match;
    rc = io_radix_lookup(tree, "/static/js/app.js",
                         strlen("/static/js/app.js"), &match);
    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_EQUAL_PTR(&handler_a, match.handler);
    TEST_ASSERT_EQUAL_UINT32(1, match.param_count);

    io_radix_destroy(tree);
}

/* ---- 5. Lookup static exact match ---- */

void test_radix_lookup_static(void)
{
    io_radix_tree_t *tree = io_radix_create();
    TEST_ASSERT_NOT_NULL(tree);

    int rc = io_radix_insert(tree, "/api/health", &handler_a, &metadata_a);
    TEST_ASSERT_EQUAL_INT(0, rc);
    rc = io_radix_insert(tree, "/api/version", &handler_b, &metadata_b);
    TEST_ASSERT_EQUAL_INT(0, rc);

    io_radix_match_t match;
    rc = io_radix_lookup(tree, "/api/health", strlen("/api/health"), &match);
    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_EQUAL_PTR(&handler_a, match.handler);
    TEST_ASSERT_EQUAL_PTR(&metadata_a, match.metadata);

    rc = io_radix_lookup(tree, "/api/version", strlen("/api/version"), &match);
    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_EQUAL_PTR(&handler_b, match.handler);
    TEST_ASSERT_EQUAL_PTR(&metadata_b, match.metadata);

    io_radix_destroy(tree);
}

/* ---- 6. Lookup param extraction ---- */

void test_radix_lookup_param_extract(void)
{
    io_radix_tree_t *tree = io_radix_create();
    TEST_ASSERT_NOT_NULL(tree);

    int rc = io_radix_insert(tree, "/users/:id", &handler_a, nullptr);
    TEST_ASSERT_EQUAL_INT(0, rc);

    io_radix_match_t match;
    rc = io_radix_lookup(tree, "/users/42", strlen("/users/42"), &match);
    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_EQUAL_UINT32(1, match.param_count);
    TEST_ASSERT_EQUAL_STRING_LEN("id", match.params[0].name,
                                 match.params[0].name_len);
    TEST_ASSERT_EQUAL_STRING_LEN("42", match.params[0].value,
                                 match.params[0].value_len);
    TEST_ASSERT_EQUAL_size_t(2, match.params[0].name_len);
    TEST_ASSERT_EQUAL_size_t(2, match.params[0].value_len);

    io_radix_destroy(tree);
}

/* ---- 7. Lookup wildcard extraction ---- */

void test_radix_lookup_wildcard_extract(void)
{
    io_radix_tree_t *tree = io_radix_create();
    TEST_ASSERT_NOT_NULL(tree);

    int rc = io_radix_insert(tree, "/static/*path", &handler_a, nullptr);
    TEST_ASSERT_EQUAL_INT(0, rc);

    io_radix_match_t match;
    rc = io_radix_lookup(tree, "/static/js/app.js",
                         strlen("/static/js/app.js"), &match);
    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_EQUAL_UINT32(1, match.param_count);
    TEST_ASSERT_EQUAL_STRING_LEN("path", match.params[0].name,
                                 match.params[0].name_len);
    TEST_ASSERT_EQUAL_STRING_LEN("js/app.js", match.params[0].value,
                                 match.params[0].value_len);
    TEST_ASSERT_EQUAL_size_t(4, match.params[0].name_len);
    TEST_ASSERT_EQUAL_size_t(9, match.params[0].value_len);

    io_radix_destroy(tree);
}

/* ---- 8. Priority: static over param ---- */

void test_radix_priority_static_over_param(void)
{
    io_radix_tree_t *tree = io_radix_create();
    TEST_ASSERT_NOT_NULL(tree);

    /* Insert param first, then static */
    int rc = io_radix_insert(tree, "/users/:id", &handler_a, nullptr);
    TEST_ASSERT_EQUAL_INT(0, rc);
    rc = io_radix_insert(tree, "/users/list", &handler_b, nullptr);
    TEST_ASSERT_EQUAL_INT(0, rc);

    io_radix_match_t match;
    /* /users/list should match static handler, not param */
    rc = io_radix_lookup(tree, "/users/list", strlen("/users/list"), &match);
    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_EQUAL_PTR(&handler_b, match.handler);
    TEST_ASSERT_EQUAL_UINT32(0, match.param_count);

    /* /users/42 should still match param handler */
    rc = io_radix_lookup(tree, "/users/42", strlen("/users/42"), &match);
    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_EQUAL_PTR(&handler_a, match.handler);
    TEST_ASSERT_EQUAL_UINT32(1, match.param_count);

    io_radix_destroy(tree);
}

/* ---- 9. Priority: param over wildcard ---- */

void test_radix_priority_param_over_wildcard(void)
{
    io_radix_tree_t *tree = io_radix_create();
    TEST_ASSERT_NOT_NULL(tree);

    int rc = io_radix_insert(tree, "/files/*path", &handler_a, nullptr);
    TEST_ASSERT_EQUAL_INT(0, rc);
    rc = io_radix_insert(tree, "/files/:name", &handler_b, nullptr);
    TEST_ASSERT_EQUAL_INT(0, rc);

    io_radix_match_t match;
    /* /files/readme should match param, not wildcard */
    rc = io_radix_lookup(tree, "/files/readme", strlen("/files/readme"), &match);
    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_EQUAL_PTR(&handler_b, match.handler);
    TEST_ASSERT_EQUAL_UINT32(1, match.param_count);
    TEST_ASSERT_EQUAL_STRING_LEN("name", match.params[0].name,
                                 match.params[0].name_len);

    /* /files/dir/file should match wildcard (multi-segment) */
    rc = io_radix_lookup(tree, "/files/dir/file",
                         strlen("/files/dir/file"), &match);
    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_EQUAL_PTR(&handler_a, match.handler);

    io_radix_destroy(tree);
}

/* ---- 10. Conflict detection ---- */

void test_radix_conflict_detection(void)
{
    io_radix_tree_t *tree = io_radix_create();
    TEST_ASSERT_NOT_NULL(tree);

    int rc = io_radix_insert(tree, "/:id", &handler_a, nullptr);
    TEST_ASSERT_EQUAL_INT(0, rc);

    /* Different param name at same position -> -EEXIST */
    rc = io_radix_insert(tree, "/:name", &handler_b, nullptr);
    TEST_ASSERT_EQUAL_INT(-EEXIST, rc);

    /* Same param name is allowed (but same handler -> -EEXIST) */
    rc = io_radix_insert(tree, "/:id", &handler_c, nullptr);
    TEST_ASSERT_EQUAL_INT(-EEXIST, rc);

    io_radix_destroy(tree);
}

/* ---- 11. Compressed prefix ---- */

void test_radix_compressed_prefix(void)
{
    io_radix_tree_t *tree = io_radix_create();
    TEST_ASSERT_NOT_NULL(tree);

    int rc = io_radix_insert(tree, "/api/users", &handler_a, nullptr);
    TEST_ASSERT_EQUAL_INT(0, rc);
    rc = io_radix_insert(tree, "/api/posts", &handler_b, nullptr);
    TEST_ASSERT_EQUAL_INT(0, rc);

    /* Both should be found */
    io_radix_match_t match;
    rc = io_radix_lookup(tree, "/api/users", strlen("/api/users"), &match);
    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_EQUAL_PTR(&handler_a, match.handler);

    rc = io_radix_lookup(tree, "/api/posts", strlen("/api/posts"), &match);
    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_EQUAL_PTR(&handler_b, match.handler);

    /* Verify prefix compression happened: root should have one STATIC child
     * with prefix containing shared "api" portion, not two separate children
     * with full "api/users" and "api/posts" prefixes. */
    TEST_ASSERT_EQUAL_UINT32(1, tree->root->child_count);

    io_radix_destroy(tree);
}

/* ---- 12. No match ---- */

void test_radix_no_match(void)
{
    io_radix_tree_t *tree = io_radix_create();
    TEST_ASSERT_NOT_NULL(tree);

    int rc = io_radix_insert(tree, "/users", &handler_a, nullptr);
    TEST_ASSERT_EQUAL_INT(0, rc);

    io_radix_match_t match;
    rc = io_radix_lookup(tree, "/unknown", strlen("/unknown"), &match);
    TEST_ASSERT_EQUAL_INT(-ENOENT, rc);
    TEST_ASSERT_NULL(match.handler);

    io_radix_destroy(tree);
}

/* ---- main ---- */

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_radix_create_destroy);
    RUN_TEST(test_radix_insert_static);
    RUN_TEST(test_radix_insert_param);
    RUN_TEST(test_radix_insert_wildcard);
    RUN_TEST(test_radix_lookup_static);
    RUN_TEST(test_radix_lookup_param_extract);
    RUN_TEST(test_radix_lookup_wildcard_extract);
    RUN_TEST(test_radix_priority_static_over_param);
    RUN_TEST(test_radix_priority_param_over_wildcard);
    RUN_TEST(test_radix_conflict_detection);
    RUN_TEST(test_radix_compressed_prefix);
    RUN_TEST(test_radix_no_match);
    return UNITY_END();
}

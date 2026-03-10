/**
 * @file test_io_ctx.c
 * @brief Unit tests for ioh_ctx unified request context.
 */

#include "core/ioh_ctx.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <unity.h>

static ioh_request_t req;
static ioh_response_t resp;

void setUp(void)
{
    ioh_request_init(&req);
    TEST_ASSERT_EQUAL_INT(0, ioh_response_init(&resp));
}

void tearDown(void)
{
    ioh_response_destroy(&resp);
}

/* ---- Lifecycle ---- */

void test_ctx_init_destroy(void)
{
    ioh_ctx_t ctx;
    TEST_ASSERT_EQUAL_INT(0, ioh_ctx_init(&ctx, &req, &resp, nullptr));

    TEST_ASSERT_NOT_NULL(ctx.arena.base);
    TEST_ASSERT_EQUAL_size_t(IOH_CTX_ARENA_DEFAULT, ctx.arena.size);
    TEST_ASSERT_EQUAL_size_t(0, ctx.arena.used);
    TEST_ASSERT_EQUAL_UINT32(0, ctx.value_count);
    TEST_ASSERT_FALSE(ctx.aborted);
    TEST_ASSERT_EQUAL_INT(-1, ctx.conn_fd);
    TEST_ASSERT_EQUAL_PTR(&req, ctx.req);
    TEST_ASSERT_EQUAL_PTR(&resp, ctx.resp);
    TEST_ASSERT_NULL(ctx.server);

    /* nullptr inputs */
    TEST_ASSERT_EQUAL_INT(-EINVAL, ioh_ctx_init(nullptr, &req, &resp, nullptr));
    TEST_ASSERT_EQUAL_INT(-EINVAL, ioh_ctx_init(&ctx, nullptr, &resp, nullptr));
    TEST_ASSERT_EQUAL_INT(-EINVAL, ioh_ctx_init(&ctx, &req, nullptr, nullptr));

    ioh_ctx_destroy(&ctx);

    /* destroying nullptr is safe */
    ioh_ctx_destroy(nullptr);
}

/* ---- Context values ---- */

void test_ctx_set_get_value(void)
{
    ioh_ctx_t ctx;
    TEST_ASSERT_EQUAL_INT(0, ioh_ctx_init(&ctx, &req, &resp, nullptr));

    int data = 42;
    TEST_ASSERT_EQUAL_INT(0, ioh_ctx_set(&ctx, "user", &data));

    void *got = ioh_ctx_get(&ctx, "user");
    TEST_ASSERT_EQUAL_PTR(&data, got);
    TEST_ASSERT_EQUAL_INT(42, *(int *)got);

    ioh_ctx_destroy(&ctx);
}

void test_ctx_set_max_values(void)
{
    ioh_ctx_t ctx;
    TEST_ASSERT_EQUAL_INT(0, ioh_ctx_init(&ctx, &req, &resp, nullptr));

    /* keys must persist — use static strings */
    static const char *keys[] = {
        "k0", "k1", "k2",  "k3",  "k4",  "k5",  "k6",  "k7",
        "k8", "k9", "k10", "k11", "k12", "k13", "k14", "k15",
    };
    int vals[IOH_CTX_MAX_VALUES];

    for (uint32_t i = 0; i < IOH_CTX_MAX_VALUES; i++) {
        vals[i] = (int)i;
        TEST_ASSERT_EQUAL_INT(0, ioh_ctx_set(&ctx, keys[i], &vals[i]));
    }

    /* 17th should fail */
    int extra = 99;
    TEST_ASSERT_EQUAL_INT(-ENOSPC, ioh_ctx_set(&ctx, "overflow", &extra));

    /* verify all 16 are still accessible */
    for (uint32_t i = 0; i < IOH_CTX_MAX_VALUES; i++) {
        TEST_ASSERT_EQUAL_PTR(&vals[i], ioh_ctx_get(&ctx, keys[i]));
    }

    ioh_ctx_destroy(&ctx);
}

void test_ctx_get_missing_returns_null(void)
{
    ioh_ctx_t ctx;
    TEST_ASSERT_EQUAL_INT(0, ioh_ctx_init(&ctx, &req, &resp, nullptr));

    TEST_ASSERT_NULL(ioh_ctx_get(&ctx, "nonexistent"));
    TEST_ASSERT_NULL(ioh_ctx_get(nullptr, "key"));
    TEST_ASSERT_NULL(ioh_ctx_get(&ctx, nullptr));

    ioh_ctx_destroy(&ctx);
}

void test_ctx_set_replaces_existing(void)
{
    ioh_ctx_t ctx;
    TEST_ASSERT_EQUAL_INT(0, ioh_ctx_init(&ctx, &req, &resp, nullptr));

    int v1 = 1, v2 = 2;
    TEST_ASSERT_EQUAL_INT(0, ioh_ctx_set(&ctx, "key", &v1));
    TEST_ASSERT_EQUAL_PTR(&v1, ioh_ctx_get(&ctx, "key"));

    TEST_ASSERT_EQUAL_INT(0, ioh_ctx_set(&ctx, "key", &v2));
    TEST_ASSERT_EQUAL_PTR(&v2, ioh_ctx_get(&ctx, "key"));

    /* value_count should still be 1 (replaced, not appended) */
    TEST_ASSERT_EQUAL_UINT32(1, ctx.value_count);

    ioh_ctx_destroy(&ctx);
}

void test_ctx_set_with_destructor(void)
{
    ioh_ctx_t ctx;
    TEST_ASSERT_EQUAL_INT(0, ioh_ctx_init(&ctx, &req, &resp, nullptr));

    int *data = malloc(sizeof(*data));
    TEST_ASSERT_NOT_NULL(data);
    *data = 42;

    TEST_ASSERT_EQUAL_INT(0, ioh_ctx_set_with_destructor(&ctx, "heap", data, free));

    void *got = ioh_ctx_get(&ctx, "heap");
    TEST_ASSERT_EQUAL_PTR(data, got);

    /* destroy will call free(data) via destructor */
    ioh_ctx_destroy(&ctx);
}

static int dtor_call_count;

static void counting_dtor(void *ptr)
{
    (void)ptr;
    dtor_call_count++;
}

void test_ctx_reset_calls_destructors(void)
{
    ioh_ctx_t ctx;
    TEST_ASSERT_EQUAL_INT(0, ioh_ctx_init(&ctx, &req, &resp, nullptr));

    dtor_call_count = 0;
    int v1 = 1, v2 = 2;

    TEST_ASSERT_EQUAL_INT(0, ioh_ctx_set_with_destructor(&ctx, "a", &v1, counting_dtor));
    TEST_ASSERT_EQUAL_INT(0, ioh_ctx_set_with_destructor(&ctx, "b", &v2, counting_dtor));

    ioh_ctx_reset(&ctx);

    TEST_ASSERT_EQUAL_INT(2, dtor_call_count);
    TEST_ASSERT_EQUAL_UINT32(0, ctx.value_count);
    TEST_ASSERT_FALSE(ctx.aborted);
    TEST_ASSERT_EQUAL_size_t(0, ctx.arena.used);

    /* arena memory is still allocated (reuse) */
    TEST_ASSERT_NOT_NULL(ctx.arena.base);
    TEST_ASSERT_GREATER_THAN(0, ctx.arena.size);

    ioh_ctx_destroy(&ctx);
}

/* ---- Request accessor delegation ---- */

void test_ctx_param_delegates(void)
{
    ioh_ctx_t ctx;
    TEST_ASSERT_EQUAL_INT(0, ioh_ctx_init(&ctx, &req, &resp, nullptr));

    /* set up a param on the request directly */
    req.params[0].name = "id";
    req.params[0].name_len = 2;
    req.params[0].value = "42";
    req.params[0].value_len = 2;
    req.param_count = 1;

    const char *val = ioh_ctx_param(&ctx, "id");
    TEST_ASSERT_NOT_NULL(val);
    TEST_ASSERT_EQUAL_STRING("42", val);

    /* typed extraction */
    int64_t i64val;
    TEST_ASSERT_EQUAL_INT(0, ioh_ctx_param_i64(&ctx, "id", &i64val));
    TEST_ASSERT_EQUAL_INT64(42, i64val);

    uint64_t u64val;
    TEST_ASSERT_EQUAL_INT(0, ioh_ctx_param_u64(&ctx, "id", &u64val));
    TEST_ASSERT_EQUAL_UINT64(42, u64val);

    ioh_ctx_destroy(&ctx);
}

void test_ctx_query_delegates(void)
{
    ioh_ctx_t ctx;
    TEST_ASSERT_EQUAL_INT(0, ioh_ctx_init(&ctx, &req, &resp, nullptr));

    req.query = "page=3&limit=10";
    req.query_len = 15;

    const char *val = ioh_ctx_query(&ctx, "page");
    TEST_ASSERT_NOT_NULL(val);
    /* query_param returns pointer into the query string at the value */
    TEST_ASSERT_EQUAL_CHAR('3', val[0]);

    ioh_ctx_destroy(&ctx);
}

void test_ctx_header_delegates(void)
{
    ioh_ctx_t ctx;
    TEST_ASSERT_EQUAL_INT(0, ioh_ctx_init(&ctx, &req, &resp, nullptr));

    req.headers[0].name = "Content-Type";
    req.headers[0].name_len = 12;
    req.headers[0].value = "application/json";
    req.headers[0].value_len = 16;
    req.header_count = 1;

    const char *val = ioh_ctx_header(&ctx, "Content-Type");
    TEST_ASSERT_NOT_NULL(val);
    TEST_ASSERT_EQUAL_STRING("application/json", val);

    ioh_ctx_destroy(&ctx);
}

/* ---- Response helpers ---- */

void test_ctx_json_sets_content_type(void)
{
    ioh_ctx_t ctx;
    TEST_ASSERT_EQUAL_INT(0, ioh_ctx_init(&ctx, &req, &resp, nullptr));

    TEST_ASSERT_EQUAL_INT(0, ioh_ctx_json(&ctx, 200, "{\"ok\":true}"));

    TEST_ASSERT_EQUAL_UINT16(200, resp.status);

    /* find Content-Type header */
    bool found = false;
    for (uint32_t i = 0; i < resp.header_count; i++) {
        if (strncasecmp(resp.headers[i].name, "Content-Type", 12) == 0) {
            TEST_ASSERT_EQUAL_STRING("application/json", resp.headers[i].value);
            found = true;
            break;
        }
    }
    TEST_ASSERT_TRUE(found);

    ioh_ctx_destroy(&ctx);
}

void test_ctx_text_sets_content_type(void)
{
    ioh_ctx_t ctx;
    TEST_ASSERT_EQUAL_INT(0, ioh_ctx_init(&ctx, &req, &resp, nullptr));

    TEST_ASSERT_EQUAL_INT(0, ioh_ctx_text(&ctx, 200, "hello world"));

    TEST_ASSERT_EQUAL_UINT16(200, resp.status);

    bool found = false;
    for (uint32_t i = 0; i < resp.header_count; i++) {
        if (strncasecmp(resp.headers[i].name, "Content-Type", 12) == 0) {
            TEST_ASSERT_EQUAL_STRING("text/plain", resp.headers[i].value);
            found = true;
            break;
        }
    }
    TEST_ASSERT_TRUE(found);

    ioh_ctx_destroy(&ctx);
}

void test_ctx_redirect_sets_location(void)
{
    ioh_ctx_t ctx;
    TEST_ASSERT_EQUAL_INT(0, ioh_ctx_init(&ctx, &req, &resp, nullptr));

    TEST_ASSERT_EQUAL_INT(0, ioh_ctx_redirect(&ctx, 302, "https://example.com/new"));

    TEST_ASSERT_EQUAL_UINT16(302, resp.status);

    bool found = false;
    for (uint32_t i = 0; i < resp.header_count; i++) {
        if (strncasecmp(resp.headers[i].name, "Location", 8) == 0) {
            TEST_ASSERT_EQUAL_STRING("https://example.com/new", resp.headers[i].value);
            found = true;
            break;
        }
    }
    TEST_ASSERT_TRUE(found);

    ioh_ctx_destroy(&ctx);
}

void test_ctx_no_content_sets_204(void)
{
    ioh_ctx_t ctx;
    TEST_ASSERT_EQUAL_INT(0, ioh_ctx_init(&ctx, &req, &resp, nullptr));

    TEST_ASSERT_EQUAL_INT(0, ioh_ctx_no_content(&ctx));
    TEST_ASSERT_EQUAL_UINT16(204, resp.status);

    ioh_ctx_destroy(&ctx);
}

void test_ctx_error_json_format(void)
{
    ioh_ctx_t ctx;
    TEST_ASSERT_EQUAL_INT(0, ioh_ctx_init(&ctx, &req, &resp, nullptr));

    TEST_ASSERT_EQUAL_INT(0, ioh_ctx_error(&ctx, 404, "not found"));

    TEST_ASSERT_EQUAL_UINT16(404, resp.status);

    /* verify body is valid JSON with error and status fields */
    TEST_ASSERT_GREATER_THAN(0, resp.body_len);
    char body[512];
    size_t copy_len = resp.body_len < sizeof(body) - 1 ? resp.body_len : sizeof(body) - 1;
    memcpy(body, resp.body, copy_len);
    body[copy_len] = '\0';

    TEST_ASSERT_NOT_NULL(strstr(body, "\"error\""));
    TEST_ASSERT_NOT_NULL(strstr(body, "\"not found\""));
    TEST_ASSERT_NOT_NULL(strstr(body, "\"status\""));
    TEST_ASSERT_NOT_NULL(strstr(body, "404"));

    ioh_ctx_destroy(&ctx);
}

void test_ctx_abort_sets_flag(void)
{
    ioh_ctx_t ctx;
    TEST_ASSERT_EQUAL_INT(0, ioh_ctx_init(&ctx, &req, &resp, nullptr));

    TEST_ASSERT_FALSE(ctx.aborted);
    ioh_ctx_abort(&ctx);
    TEST_ASSERT_TRUE(ctx.aborted);

    /* aborting nullptr is safe */
    ioh_ctx_abort(nullptr);

    ioh_ctx_destroy(&ctx);
}

/* ---- Arena allocator ---- */

void test_ctx_arena_alloc(void)
{
    ioh_ctx_t ctx;
    TEST_ASSERT_EQUAL_INT(0, ioh_ctx_init(&ctx, &req, &resp, nullptr));

    void *p1 = ioh_ctx_alloc(&ctx, 64);
    TEST_ASSERT_NOT_NULL(p1);

    void *p2 = ioh_ctx_alloc(&ctx, 128);
    TEST_ASSERT_NOT_NULL(p2);

    /* p2 should come right after p1 */
    TEST_ASSERT_EQUAL_PTR((uint8_t *)p1 + 64, p2);
    TEST_ASSERT_EQUAL_size_t(192, ctx.arena.used);

    /* zero size returns nullptr */
    TEST_ASSERT_NULL(ioh_ctx_alloc(&ctx, 0));

    ioh_ctx_destroy(&ctx);
}

void test_ctx_arena_alloc_aligned(void)
{
    ioh_ctx_t ctx;
    TEST_ASSERT_EQUAL_INT(0, ioh_ctx_init(&ctx, &req, &resp, nullptr));

    /* burn 1 byte so alignment matters */
    void *p0 = ioh_ctx_alloc(&ctx, 1);
    TEST_ASSERT_NOT_NULL(p0);
    TEST_ASSERT_EQUAL_size_t(1, ctx.arena.used);

    /* align to 16 */
    void *p1 = ioh_ctx_alloc_aligned(&ctx, 32, 16);
    TEST_ASSERT_NOT_NULL(p1);
    TEST_ASSERT_EQUAL_UINT(0, (uintptr_t)p1 % 16);

    /* align to 64 */
    void *p2 = ioh_ctx_alloc_aligned(&ctx, 8, 64);
    TEST_ASSERT_NOT_NULL(p2);
    TEST_ASSERT_EQUAL_UINT(0, (uintptr_t)p2 % 64);

    /* non-power-of-2 alignment is rejected */
    TEST_ASSERT_NULL(ioh_ctx_alloc_aligned(&ctx, 8, 3));

    ioh_ctx_destroy(&ctx);
}

void test_ctx_arena_overflow_realloc(void)
{
    ioh_ctx_t ctx;
    TEST_ASSERT_EQUAL_INT(0, ioh_ctx_init(&ctx, &req, &resp, nullptr));

    TEST_ASSERT_EQUAL_size_t(IOH_CTX_ARENA_DEFAULT, ctx.arena.size);

    /* allocate more than the default arena */
    void *p = ioh_ctx_alloc(&ctx, IOH_CTX_ARENA_DEFAULT + 1);
    TEST_ASSERT_NOT_NULL(p);
    TEST_ASSERT_GREATER_THAN(IOH_CTX_ARENA_DEFAULT, ctx.arena.size);

    /* can still allocate more */
    void *p2 = ioh_ctx_alloc(&ctx, 64);
    TEST_ASSERT_NOT_NULL(p2);

    ioh_ctx_destroy(&ctx);
}

void test_ctx_sprintf_arena(void)
{
    ioh_ctx_t ctx;
    TEST_ASSERT_EQUAL_INT(0, ioh_ctx_init(&ctx, &req, &resp, nullptr));

    char *s = ioh_ctx_sprintf(&ctx, "hello %s, id=%d", "world", 42);
    TEST_ASSERT_NOT_NULL(s);
    TEST_ASSERT_EQUAL_STRING("hello world, id=42", s);

    /* verify it came from the arena */
    TEST_ASSERT_TRUE((uint8_t *)s >= ctx.arena.base &&
                     (uint8_t *)s < ctx.arena.base + ctx.arena.size);

    ioh_ctx_destroy(&ctx);
}

/* ---- Test runner ---- */

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_ctx_init_destroy);
    RUN_TEST(test_ctx_set_get_value);
    RUN_TEST(test_ctx_set_max_values);
    RUN_TEST(test_ctx_get_missing_returns_null);
    RUN_TEST(test_ctx_set_replaces_existing);
    RUN_TEST(test_ctx_set_with_destructor);
    RUN_TEST(test_ctx_reset_calls_destructors);
    RUN_TEST(test_ctx_param_delegates);
    RUN_TEST(test_ctx_query_delegates);
    RUN_TEST(test_ctx_header_delegates);
    RUN_TEST(test_ctx_json_sets_content_type);
    RUN_TEST(test_ctx_text_sets_content_type);
    RUN_TEST(test_ctx_redirect_sets_location);
    RUN_TEST(test_ctx_no_content_sets_204);
    RUN_TEST(test_ctx_error_json_format);
    RUN_TEST(test_ctx_abort_sets_flag);
    RUN_TEST(test_ctx_arena_alloc);
    RUN_TEST(test_ctx_arena_alloc_aligned);
    RUN_TEST(test_ctx_arena_overflow_realloc);
    RUN_TEST(test_ctx_sprintf_arena);

    return UNITY_END();
}

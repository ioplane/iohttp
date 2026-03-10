/**
 * @file test_io_middleware.c
 * @brief Unit tests for middleware chain execution.
 */

#include "core/ioh_ctx.h"
#include "middleware/ioh_middleware.h"
#include "router/ioh_route_group.h"
#include "router/ioh_router.h"

#include <errno.h>
#include <string.h>

#include <unity.h>

static ioh_router_t *router;

/* ---- Call tracking ---- */

static int call_order[16];
static int call_count;

void setUp(void)
{
    router = ioh_router_create();
    TEST_ASSERT_NOT_NULL(router);
    memset(call_order, 0, sizeof(call_order));
    call_count = 0;
}

void tearDown(void)
{
    ioh_router_destroy(router);
    router = nullptr;
}

/* ---- Stub middleware ---- */

static int mw_a(ioh_ctx_t *c, ioh_handler_fn next)
{
    (void)c;
    call_order[call_count++] = 'A';
    return next(c);
}

static int mw_b(ioh_ctx_t *c, ioh_handler_fn next)
{
    (void)c;
    call_order[call_count++] = 'B';
    return next(c);
}

static int mw_c(ioh_ctx_t *c, ioh_handler_fn next)
{
    (void)c;
    call_order[call_count++] = 'C';
    return next(c);
}

static int mw_d(ioh_ctx_t *c, ioh_handler_fn next)
{
    (void)c;
    call_order[call_count++] = 'D';
    return next(c);
}

static int mw_block(ioh_ctx_t *c, ioh_handler_fn next)
{
    (void)c;
    (void)next;
    call_order[call_count++] = 'X';
    return -EPERM; /* short-circuit: never calls next */
}

/** Middleware that adds an X-Before header before calling next */
static int mw_add_req_header(ioh_ctx_t *c, ioh_handler_fn next)
{
    /* Simulate adding a header to the request by setting content_type */
    c->req->content_type = "application/json";
    return next(c);
}

/** Middleware that adds an X-After header after calling next */
static int mw_add_resp_header(ioh_ctx_t *c, ioh_handler_fn next)
{
    int rc = next(c);
    if (rc == 0) {
        (void)ioh_response_set_header(c->resp, "X-After", "middleware");
    }
    return rc;
}

/* ---- Stub handlers ---- */

static int handler_ok(ioh_ctx_t *c)
{
    (void)c;
    call_order[call_count++] = 'H';
    return 0;
}

static int handler_error(ioh_ctx_t *c)
{
    (void)c;
    return -EIO;
}

static int handler_check_content_type(ioh_ctx_t *c)
{
    call_order[call_count++] = 'H';
    /* Verify middleware set content_type */
    if (c->req->content_type && strcmp(c->req->content_type, "application/json") == 0) {
        call_order[call_count++] = '!'; /* success marker */
    }
    return 0;
}

static int error_handler_called;
static int error_handler_code;

static int test_error_handler(ioh_ctx_t *c, int error)
{
    (void)c;
    error_handler_called = 1;
    error_handler_code = error;
    return 0;
}

static int custom_404_called;

static int custom_404_handler(ioh_ctx_t *c)
{
    (void)c;
    custom_404_called = 1;
    return 0;
}

static int custom_405_called;

static int custom_405_handler(ioh_ctx_t *c)
{
    (void)c;
    custom_405_called = 1;
    return 0;
}

/* ---- Tests ---- */

/** Test that middleware executes in order: A -> B -> handler */
void test_middleware_chain_order(void)
{
    ioh_request_t req;
    ioh_response_t resp;
    ioh_request_init(&req);
    TEST_ASSERT_EQUAL_INT(0, ioh_response_init(&resp));

    ioh_ctx_t c;
    TEST_ASSERT_EQUAL_INT(0, ioh_ctx_init(&c, &req, &resp, nullptr));

    ioh_middleware_fn mws[] = {mw_a, mw_b};
    int rc = ioh_chain_execute(&c, mws, 2, nullptr, 0, handler_ok);

    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_EQUAL_INT(3, call_count);
    TEST_ASSERT_EQUAL_INT('A', call_order[0]);
    TEST_ASSERT_EQUAL_INT('B', call_order[1]);
    TEST_ASSERT_EQUAL_INT('H', call_order[2]);

    ioh_ctx_destroy(&c);
    ioh_response_destroy(&resp);
}

/** Test short-circuit: blocking middleware returns, handler never called */
void test_middleware_short_circuit(void)
{
    ioh_request_t req;
    ioh_response_t resp;
    ioh_request_init(&req);
    TEST_ASSERT_EQUAL_INT(0, ioh_response_init(&resp));

    ioh_ctx_t c;
    TEST_ASSERT_EQUAL_INT(0, ioh_ctx_init(&c, &req, &resp, nullptr));

    ioh_middleware_fn mws[] = {mw_a, mw_block, mw_b};
    int rc = ioh_chain_execute(&c, mws, 3, nullptr, 0, handler_ok);

    TEST_ASSERT_EQUAL_INT(-EPERM, rc);
    TEST_ASSERT_EQUAL_INT(2, call_count); /* A, X — B and H never called */
    TEST_ASSERT_EQUAL_INT('A', call_order[0]);
    TEST_ASSERT_EQUAL_INT('X', call_order[1]);

    ioh_ctx_destroy(&c);
    ioh_response_destroy(&resp);
}

/** Test middleware modifying request before handler sees it */
void test_middleware_modify_request(void)
{
    ioh_request_t req;
    ioh_response_t resp;
    ioh_request_init(&req);
    TEST_ASSERT_EQUAL_INT(0, ioh_response_init(&resp));

    ioh_ctx_t c;
    TEST_ASSERT_EQUAL_INT(0, ioh_ctx_init(&c, &req, &resp, nullptr));

    ioh_middleware_fn mws[] = {mw_add_req_header};
    int rc = ioh_chain_execute(&c, mws, 1, nullptr, 0, handler_check_content_type);

    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_EQUAL_INT(2, call_count); /* H, ! */
    TEST_ASSERT_EQUAL_INT('H', call_order[0]);
    TEST_ASSERT_EQUAL_INT('!', call_order[1]);

    ioh_ctx_destroy(&c);
    ioh_response_destroy(&resp);
}

/** Test middleware modifying response after handler returns */
void test_middleware_modify_response(void)
{
    ioh_request_t req;
    ioh_response_t resp;
    ioh_request_init(&req);
    TEST_ASSERT_EQUAL_INT(0, ioh_response_init(&resp));

    ioh_ctx_t c;
    TEST_ASSERT_EQUAL_INT(0, ioh_ctx_init(&c, &req, &resp, nullptr));

    ioh_middleware_fn mws[] = {mw_add_resp_header};
    int rc = ioh_chain_execute(&c, mws, 1, nullptr, 0, handler_ok);

    TEST_ASSERT_EQUAL_INT(0, rc);

    /* Check that X-After header was added by middleware */
    bool found = false;
    for (uint32_t i = 0; i < resp.header_count; i++) {
        if (resp.headers[i].name && strcmp(resp.headers[i].name, "X-After") == 0) {
            TEST_ASSERT_EQUAL_STRING("middleware", resp.headers[i].value);
            found = true;
            break;
        }
    }
    TEST_ASSERT_TRUE(found);

    ioh_ctx_destroy(&c);
    ioh_response_destroy(&resp);
}

/** Test per-group middleware: only runs for that group's routes */
void test_middleware_per_group(void)
{
    ioh_request_t req;
    ioh_response_t resp;
    ioh_request_init(&req);
    TEST_ASSERT_EQUAL_INT(0, ioh_response_init(&resp));

    ioh_ctx_t c;
    TEST_ASSERT_EQUAL_INT(0, ioh_ctx_init(&c, &req, &resp, nullptr));

    /* Group middleware only */
    ioh_middleware_fn group_mws[] = {mw_c, mw_d};
    int rc = ioh_chain_execute(&c, nullptr, 0, group_mws, 2, handler_ok);

    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_EQUAL_INT(3, call_count);
    TEST_ASSERT_EQUAL_INT('C', call_order[0]);
    TEST_ASSERT_EQUAL_INT('D', call_order[1]);
    TEST_ASSERT_EQUAL_INT('H', call_order[2]);

    ioh_ctx_destroy(&c);
    ioh_response_destroy(&resp);
}

/** Test empty chain: handler called directly with no middleware */
void test_middleware_empty_chain(void)
{
    ioh_request_t req;
    ioh_response_t resp;
    ioh_request_init(&req);
    TEST_ASSERT_EQUAL_INT(0, ioh_response_init(&resp));

    ioh_ctx_t c;
    TEST_ASSERT_EQUAL_INT(0, ioh_ctx_init(&c, &req, &resp, nullptr));

    int rc = ioh_chain_execute(&c, nullptr, 0, nullptr, 0, handler_ok);

    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_EQUAL_INT(1, call_count);
    TEST_ASSERT_EQUAL_INT('H', call_order[0]);

    ioh_ctx_destroy(&c);
    ioh_response_destroy(&resp);
}

/** Test error handler invocation when handler returns error */
void test_middleware_error_handler(void)
{
    error_handler_called = 0;
    error_handler_code = 0;

    ioh_router_set_error_handler(router, test_error_handler);
    ioh_error_handler_fn eh = ioh_router_error_handler(router);
    TEST_ASSERT_NOT_NULL(eh);

    ioh_request_t req;
    ioh_response_t resp;
    ioh_request_init(&req);
    TEST_ASSERT_EQUAL_INT(0, ioh_response_init(&resp));

    ioh_ctx_t c;
    TEST_ASSERT_EQUAL_INT(0, ioh_ctx_init(&c, &req, &resp, nullptr));

    /* Execute chain — handler returns error */
    int rc = ioh_chain_execute(&c, nullptr, 0, nullptr, 0, handler_error);
    TEST_ASSERT_EQUAL_INT(-EIO, rc);

    /* Simulate what the server loop would do: call the error handler */
    if (rc != 0 && eh) {
        int erc = eh(&c, rc);
        TEST_ASSERT_EQUAL_INT(0, erc);
    }

    TEST_ASSERT_EQUAL_INT(1, error_handler_called);
    TEST_ASSERT_EQUAL_INT(-EIO, error_handler_code);

    ioh_ctx_destroy(&c);
    ioh_response_destroy(&resp);
}

/** Test custom 404 handler registration and retrieval */
void test_middleware_custom_404(void)
{
    custom_404_called = 0;

    ioh_router_set_not_found(router, custom_404_handler);
    ioh_handler_fn nf = ioh_router_not_found_handler(router);
    TEST_ASSERT_NOT_NULL(nf);

    ioh_request_t req;
    ioh_response_t resp;
    ioh_request_init(&req);
    TEST_ASSERT_EQUAL_INT(0, ioh_response_init(&resp));

    ioh_ctx_t c;
    TEST_ASSERT_EQUAL_INT(0, ioh_ctx_init(&c, &req, &resp, nullptr));

    /* Simulate dispatch miss */
    ioh_route_match_t match = ioh_router_dispatch(router, IOH_METHOD_GET, "/nope", 5);
    TEST_ASSERT_EQUAL_INT(IOH_MATCH_NOT_FOUND, match.status);

    /* Server loop would call the custom not-found handler */
    int rc = nf(&c);
    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_EQUAL_INT(1, custom_404_called);

    ioh_ctx_destroy(&c);
    ioh_response_destroy(&resp);
}

/** Test custom 405 handler registration and retrieval */
void test_middleware_custom_405(void)
{
    custom_405_called = 0;

    /* Register a GET route so that POST to same path yields 405 */
    TEST_ASSERT_EQUAL_INT(0, ioh_router_get(router, "/resource", handler_ok));

    ioh_router_set_method_not_allowed(router, custom_405_handler);
    ioh_handler_fn ma = ioh_router_method_not_allowed_handler(router);
    TEST_ASSERT_NOT_NULL(ma);

    ioh_request_t req;
    ioh_response_t resp;
    ioh_request_init(&req);
    TEST_ASSERT_EQUAL_INT(0, ioh_response_init(&resp));

    ioh_ctx_t c;
    TEST_ASSERT_EQUAL_INT(0, ioh_ctx_init(&c, &req, &resp, nullptr));

    /* Dispatch POST to a GET-only route */
    ioh_route_match_t match = ioh_router_dispatch(router, IOH_METHOD_POST, "/resource", 9);
    TEST_ASSERT_EQUAL_INT(IOH_MATCH_METHOD_NOT_ALLOWED, match.status);

    /* Server loop would call the custom 405 handler */
    int rc = ma(&c);
    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_EQUAL_INT(1, custom_405_called);

    ioh_ctx_destroy(&c);
    ioh_response_destroy(&resp);
}

/** Test global + group middleware execute in correct order */
void test_middleware_global_plus_group(void)
{
    ioh_request_t req;
    ioh_response_t resp;
    ioh_request_init(&req);
    TEST_ASSERT_EQUAL_INT(0, ioh_response_init(&resp));

    ioh_ctx_t c;
    TEST_ASSERT_EQUAL_INT(0, ioh_ctx_init(&c, &req, &resp, nullptr));

    /* Global middleware: A, B */
    ioh_middleware_fn global_mws[] = {mw_a, mw_b};

    /* Group middleware: C, D */
    ioh_middleware_fn group_mws[] = {mw_c, mw_d};

    int rc = ioh_chain_execute(&c, global_mws, 2, group_mws, 2, handler_ok);

    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_EQUAL_INT(5, call_count);
    TEST_ASSERT_EQUAL_INT('A', call_order[0]);
    TEST_ASSERT_EQUAL_INT('B', call_order[1]);
    TEST_ASSERT_EQUAL_INT('C', call_order[2]);
    TEST_ASSERT_EQUAL_INT('D', call_order[3]);
    TEST_ASSERT_EQUAL_INT('H', call_order[4]);

    ioh_ctx_destroy(&c);
    ioh_response_destroy(&resp);
}

/* ---- Runner ---- */

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_middleware_chain_order);
    RUN_TEST(test_middleware_short_circuit);
    RUN_TEST(test_middleware_modify_request);
    RUN_TEST(test_middleware_modify_response);
    RUN_TEST(test_middleware_per_group);
    RUN_TEST(test_middleware_empty_chain);
    RUN_TEST(test_middleware_error_handler);
    RUN_TEST(test_middleware_custom_404);
    RUN_TEST(test_middleware_custom_405);
    RUN_TEST(test_middleware_global_plus_group);
    return UNITY_END();
}

/**
 * @file test_io_conn.c
 * @brief Unit tests for ioh_conn connection state machine and pool.
 */

#include "core/ioh_conn.h"

#include <errno.h>
#include <string.h>

#include <unity.h>

void setUp(void)
{
}
void tearDown(void)
{
}

/* ---- Pool lifecycle ---- */

void test_conn_pool_create_destroy(void)
{
    ioh_conn_pool_t *pool = ioh_conn_pool_create(64);
    TEST_ASSERT_NOT_NULL(pool);
    TEST_ASSERT_EQUAL_UINT32(0, ioh_conn_pool_active(pool));
    TEST_ASSERT_EQUAL_UINT32(64, ioh_conn_pool_capacity(pool));

    ioh_conn_pool_destroy(pool);

    /* Destroying nullptr should not crash */
    ioh_conn_pool_destroy(nullptr);

    /* Zero capacity returns nullptr */
    TEST_ASSERT_NULL(ioh_conn_pool_create(0));
}

/* ---- Alloc returns valid conn ---- */

void test_conn_alloc_returns_conn(void)
{
    ioh_conn_pool_t *pool = ioh_conn_pool_create(8);
    TEST_ASSERT_NOT_NULL(pool);

    ioh_conn_t *conn = ioh_conn_alloc(pool);
    TEST_ASSERT_NOT_NULL(conn);
    TEST_ASSERT_EQUAL_UINT8(IOH_CONN_ACCEPTING, conn->state);
    TEST_ASSERT_EQUAL_INT(-1, conn->fd);
    TEST_ASSERT_GREATER_THAN(0, conn->id);
    TEST_ASSERT_EQUAL_UINT32(1, ioh_conn_pool_active(pool));

    ioh_conn_pool_destroy(pool);
}

/* ---- Pool full ---- */

void test_conn_alloc_pool_full(void)
{
    ioh_conn_pool_t *pool = ioh_conn_pool_create(2);
    TEST_ASSERT_NOT_NULL(pool);

    ioh_conn_t *c1 = ioh_conn_alloc(pool);
    TEST_ASSERT_NOT_NULL(c1);

    ioh_conn_t *c2 = ioh_conn_alloc(pool);
    TEST_ASSERT_NOT_NULL(c2);

    /* Pool is full — should return nullptr */
    ioh_conn_t *c3 = ioh_conn_alloc(pool);
    TEST_ASSERT_NULL(c3);
    TEST_ASSERT_EQUAL_UINT32(2, ioh_conn_pool_active(pool));

    ioh_conn_pool_destroy(pool);
}

/* ---- Free reuses slot ---- */

void test_conn_free_reuses_slot(void)
{
    ioh_conn_pool_t *pool = ioh_conn_pool_create(1);
    TEST_ASSERT_NOT_NULL(pool);

    ioh_conn_t *conn = ioh_conn_alloc(pool);
    TEST_ASSERT_NOT_NULL(conn);
    TEST_ASSERT_EQUAL_UINT32(1, ioh_conn_pool_active(pool));

    ioh_conn_free(pool, conn);
    TEST_ASSERT_EQUAL_UINT32(0, ioh_conn_pool_active(pool));

    /* Should be able to allocate again */
    ioh_conn_t *conn2 = ioh_conn_alloc(pool);
    TEST_ASSERT_NOT_NULL(conn2);
    TEST_ASSERT_EQUAL_UINT8(IOH_CONN_ACCEPTING, conn2->state);
    TEST_ASSERT_EQUAL_UINT32(1, ioh_conn_pool_active(pool));

    ioh_conn_pool_destroy(pool);
}

/* ---- Find by fd ---- */

void test_conn_find_by_fd(void)
{
    ioh_conn_pool_t *pool = ioh_conn_pool_create(8);
    TEST_ASSERT_NOT_NULL(pool);

    ioh_conn_t *c1 = ioh_conn_alloc(pool);
    TEST_ASSERT_NOT_NULL(c1);
    c1->fd = 10;

    ioh_conn_t *c2 = ioh_conn_alloc(pool);
    TEST_ASSERT_NOT_NULL(c2);
    c2->fd = 20;

    ioh_conn_t *found = ioh_conn_find(pool, 10);
    TEST_ASSERT_NOT_NULL(found);
    TEST_ASSERT_EQUAL_PTR(c1, found);
    TEST_ASSERT_EQUAL_INT(10, found->fd);

    found = ioh_conn_find(pool, 20);
    TEST_ASSERT_NOT_NULL(found);
    TEST_ASSERT_EQUAL_PTR(c2, found);

    ioh_conn_pool_destroy(pool);
}

/* ---- Find missing ---- */

void test_conn_find_missing(void)
{
    ioh_conn_pool_t *pool = ioh_conn_pool_create(4);
    TEST_ASSERT_NOT_NULL(pool);

    ioh_conn_t *conn = ioh_conn_alloc(pool);
    TEST_ASSERT_NOT_NULL(conn);
    conn->fd = 42;

    /* fd 99 does not exist */
    TEST_ASSERT_NULL(ioh_conn_find(pool, 99));

    /* Negative fd returns nullptr */
    TEST_ASSERT_NULL(ioh_conn_find(pool, -1));

    /* nullptr pool returns nullptr */
    TEST_ASSERT_NULL(ioh_conn_find(nullptr, 42));

    ioh_conn_pool_destroy(pool);
}

/* ---- Valid state transitions ---- */

void test_conn_state_valid_transition(void)
{
    ioh_conn_pool_t *pool = ioh_conn_pool_create(4);
    TEST_ASSERT_NOT_NULL(pool);

    ioh_conn_t *conn = ioh_conn_alloc(pool);
    TEST_ASSERT_NOT_NULL(conn);
    TEST_ASSERT_EQUAL_UINT8(IOH_CONN_ACCEPTING, conn->state);

    /* ACCEPTING → TLS_HANDSHAKE */
    TEST_ASSERT_EQUAL_INT(0, ioh_conn_transition(conn, IOH_CONN_TLS_HANDSHAKE));
    TEST_ASSERT_EQUAL_UINT8(IOH_CONN_TLS_HANDSHAKE, conn->state);

    /* TLS_HANDSHAKE → HTTP_ACTIVE */
    TEST_ASSERT_EQUAL_INT(0, ioh_conn_transition(conn, IOH_CONN_HTTP_ACTIVE));
    TEST_ASSERT_EQUAL_UINT8(IOH_CONN_HTTP_ACTIVE, conn->state);

    /* HTTP_ACTIVE → DRAINING */
    TEST_ASSERT_EQUAL_INT(0, ioh_conn_transition(conn, IOH_CONN_DRAINING));
    TEST_ASSERT_EQUAL_UINT8(IOH_CONN_DRAINING, conn->state);

    /* DRAINING → CLOSING */
    TEST_ASSERT_EQUAL_INT(0, ioh_conn_transition(conn, IOH_CONN_CLOSING));
    TEST_ASSERT_EQUAL_UINT8(IOH_CONN_CLOSING, conn->state);

    /* Verify state names while we're at it */
    TEST_ASSERT_EQUAL_STRING("CLOSING", ioh_conn_state_name(IOH_CONN_CLOSING));
    TEST_ASSERT_EQUAL_STRING("FREE", ioh_conn_state_name(IOH_CONN_FREE));
    TEST_ASSERT_EQUAL_STRING("HTTP_ACTIVE", ioh_conn_state_name(IOH_CONN_HTTP_ACTIVE));

    ioh_conn_pool_destroy(pool);
}

/* ---- Invalid state transitions ---- */

void test_conn_state_invalid_transition(void)
{
    ioh_conn_pool_t *pool = ioh_conn_pool_create(4);
    TEST_ASSERT_NOT_NULL(pool);

    ioh_conn_t *conn = ioh_conn_alloc(pool);
    TEST_ASSERT_NOT_NULL(conn);

    /* Force state to FREE to test invalid transition */
    conn->state = IOH_CONN_FREE;

    /* FREE → HTTP_ACTIVE is invalid */
    TEST_ASSERT_EQUAL_INT(-EINVAL, ioh_conn_transition(conn, IOH_CONN_HTTP_ACTIVE));
    TEST_ASSERT_EQUAL_UINT8(IOH_CONN_FREE, conn->state);

    /* FREE → CLOSING is invalid */
    TEST_ASSERT_EQUAL_INT(-EINVAL, ioh_conn_transition(conn, IOH_CONN_CLOSING));

    /* nullptr conn */
    TEST_ASSERT_EQUAL_INT(-EINVAL, ioh_conn_transition(nullptr, IOH_CONN_ACCEPTING));

    /* CLOSING → anything except FREE (which is only via ioh_conn_free) */
    conn->state = IOH_CONN_CLOSING;
    TEST_ASSERT_EQUAL_INT(-EINVAL, ioh_conn_transition(conn, IOH_CONN_HTTP_ACTIVE));
    TEST_ASSERT_EQUAL_INT(-EINVAL, ioh_conn_transition(conn, IOH_CONN_ACCEPTING));

    ioh_conn_pool_destroy(pool);
}

/* ---- Recv buffer tests ---- */

void test_conn_alloc_has_recv_buffer(void)
{
    ioh_conn_pool_t *pool = ioh_conn_pool_create(4);
    TEST_ASSERT_NOT_NULL(pool);

    ioh_conn_t *conn = ioh_conn_alloc(pool);
    TEST_ASSERT_NOT_NULL(conn);
    TEST_ASSERT_NOT_NULL(conn->recv_buf);
    TEST_ASSERT_GREATER_THAN(0, (int)conn->recv_buf_size);
    TEST_ASSERT_EQUAL(0, conn->recv_len);
    TEST_ASSERT_FALSE(conn->send_active);

    ioh_conn_free(pool, conn);
    ioh_conn_pool_destroy(pool);
}

/* ---- Pool get tests ---- */

void test_conn_pool_get_valid(void)
{
    ioh_conn_pool_t *pool = ioh_conn_pool_create(4);
    TEST_ASSERT_NOT_NULL(pool);

    ioh_conn_t *conn = ioh_conn_alloc(pool);
    TEST_ASSERT_NOT_NULL(conn);

    /* Index 0 should return the first slot */
    ioh_conn_t *got = ioh_conn_pool_get(pool, 0);
    TEST_ASSERT_NOT_NULL(got);
    TEST_ASSERT_EQUAL_PTR(conn, got);

    /* Out of range returns nullptr */
    TEST_ASSERT_NULL(ioh_conn_pool_get(pool, 999));

    /* nullptr pool returns nullptr */
    TEST_ASSERT_NULL(ioh_conn_pool_get(nullptr, 0));

    ioh_conn_pool_destroy(pool);
}

/* ---- Test runner ---- */

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_conn_pool_create_destroy);
    RUN_TEST(test_conn_alloc_returns_conn);
    RUN_TEST(test_conn_alloc_pool_full);
    RUN_TEST(test_conn_free_reuses_slot);
    RUN_TEST(test_conn_find_by_fd);
    RUN_TEST(test_conn_find_missing);
    RUN_TEST(test_conn_state_valid_transition);
    RUN_TEST(test_conn_state_invalid_transition);
    RUN_TEST(test_conn_alloc_has_recv_buffer);
    RUN_TEST(test_conn_pool_get_valid);

    return UNITY_END();
}

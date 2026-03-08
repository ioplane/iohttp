/**
 * @file test_io_conn.c
 * @brief Unit tests for io_conn connection state machine and pool.
 */

#include "core/io_conn.h"

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
    io_conn_pool_t *pool = io_conn_pool_create(64);
    TEST_ASSERT_NOT_NULL(pool);
    TEST_ASSERT_EQUAL_UINT32(0, io_conn_pool_active(pool));
    TEST_ASSERT_EQUAL_UINT32(64, io_conn_pool_capacity(pool));

    io_conn_pool_destroy(pool);

    /* Destroying nullptr should not crash */
    io_conn_pool_destroy(nullptr);

    /* Zero capacity returns nullptr */
    TEST_ASSERT_NULL(io_conn_pool_create(0));
}

/* ---- Alloc returns valid conn ---- */

void test_conn_alloc_returns_conn(void)
{
    io_conn_pool_t *pool = io_conn_pool_create(8);
    TEST_ASSERT_NOT_NULL(pool);

    io_conn_t *conn = io_conn_alloc(pool);
    TEST_ASSERT_NOT_NULL(conn);
    TEST_ASSERT_EQUAL_UINT8(IO_CONN_ACCEPTING, conn->state);
    TEST_ASSERT_EQUAL_INT(-1, conn->fd);
    TEST_ASSERT_GREATER_THAN(0, conn->id);
    TEST_ASSERT_EQUAL_UINT32(1, io_conn_pool_active(pool));

    io_conn_pool_destroy(pool);
}

/* ---- Pool full ---- */

void test_conn_alloc_pool_full(void)
{
    io_conn_pool_t *pool = io_conn_pool_create(2);
    TEST_ASSERT_NOT_NULL(pool);

    io_conn_t *c1 = io_conn_alloc(pool);
    TEST_ASSERT_NOT_NULL(c1);

    io_conn_t *c2 = io_conn_alloc(pool);
    TEST_ASSERT_NOT_NULL(c2);

    /* Pool is full — should return nullptr */
    io_conn_t *c3 = io_conn_alloc(pool);
    TEST_ASSERT_NULL(c3);
    TEST_ASSERT_EQUAL_UINT32(2, io_conn_pool_active(pool));

    io_conn_pool_destroy(pool);
}

/* ---- Free reuses slot ---- */

void test_conn_free_reuses_slot(void)
{
    io_conn_pool_t *pool = io_conn_pool_create(1);
    TEST_ASSERT_NOT_NULL(pool);

    io_conn_t *conn = io_conn_alloc(pool);
    TEST_ASSERT_NOT_NULL(conn);
    TEST_ASSERT_EQUAL_UINT32(1, io_conn_pool_active(pool));

    io_conn_free(pool, conn);
    TEST_ASSERT_EQUAL_UINT32(0, io_conn_pool_active(pool));

    /* Should be able to allocate again */
    io_conn_t *conn2 = io_conn_alloc(pool);
    TEST_ASSERT_NOT_NULL(conn2);
    TEST_ASSERT_EQUAL_UINT8(IO_CONN_ACCEPTING, conn2->state);
    TEST_ASSERT_EQUAL_UINT32(1, io_conn_pool_active(pool));

    io_conn_pool_destroy(pool);
}

/* ---- Find by fd ---- */

void test_conn_find_by_fd(void)
{
    io_conn_pool_t *pool = io_conn_pool_create(8);
    TEST_ASSERT_NOT_NULL(pool);

    io_conn_t *c1 = io_conn_alloc(pool);
    TEST_ASSERT_NOT_NULL(c1);
    c1->fd = 10;

    io_conn_t *c2 = io_conn_alloc(pool);
    TEST_ASSERT_NOT_NULL(c2);
    c2->fd = 20;

    io_conn_t *found = io_conn_find(pool, 10);
    TEST_ASSERT_NOT_NULL(found);
    TEST_ASSERT_EQUAL_PTR(c1, found);
    TEST_ASSERT_EQUAL_INT(10, found->fd);

    found = io_conn_find(pool, 20);
    TEST_ASSERT_NOT_NULL(found);
    TEST_ASSERT_EQUAL_PTR(c2, found);

    io_conn_pool_destroy(pool);
}

/* ---- Find missing ---- */

void test_conn_find_missing(void)
{
    io_conn_pool_t *pool = io_conn_pool_create(4);
    TEST_ASSERT_NOT_NULL(pool);

    io_conn_t *conn = io_conn_alloc(pool);
    TEST_ASSERT_NOT_NULL(conn);
    conn->fd = 42;

    /* fd 99 does not exist */
    TEST_ASSERT_NULL(io_conn_find(pool, 99));

    /* Negative fd returns nullptr */
    TEST_ASSERT_NULL(io_conn_find(pool, -1));

    /* nullptr pool returns nullptr */
    TEST_ASSERT_NULL(io_conn_find(nullptr, 42));

    io_conn_pool_destroy(pool);
}

/* ---- Valid state transitions ---- */

void test_conn_state_valid_transition(void)
{
    io_conn_pool_t *pool = io_conn_pool_create(4);
    TEST_ASSERT_NOT_NULL(pool);

    io_conn_t *conn = io_conn_alloc(pool);
    TEST_ASSERT_NOT_NULL(conn);
    TEST_ASSERT_EQUAL_UINT8(IO_CONN_ACCEPTING, conn->state);

    /* ACCEPTING → TLS_HANDSHAKE */
    TEST_ASSERT_EQUAL_INT(0, io_conn_transition(conn, IO_CONN_TLS_HANDSHAKE));
    TEST_ASSERT_EQUAL_UINT8(IO_CONN_TLS_HANDSHAKE, conn->state);

    /* TLS_HANDSHAKE → HTTP_ACTIVE */
    TEST_ASSERT_EQUAL_INT(0, io_conn_transition(conn, IO_CONN_HTTP_ACTIVE));
    TEST_ASSERT_EQUAL_UINT8(IO_CONN_HTTP_ACTIVE, conn->state);

    /* HTTP_ACTIVE → DRAINING */
    TEST_ASSERT_EQUAL_INT(0, io_conn_transition(conn, IO_CONN_DRAINING));
    TEST_ASSERT_EQUAL_UINT8(IO_CONN_DRAINING, conn->state);

    /* DRAINING → CLOSING */
    TEST_ASSERT_EQUAL_INT(0, io_conn_transition(conn, IO_CONN_CLOSING));
    TEST_ASSERT_EQUAL_UINT8(IO_CONN_CLOSING, conn->state);

    /* Verify state names while we're at it */
    TEST_ASSERT_EQUAL_STRING("CLOSING", io_conn_state_name(IO_CONN_CLOSING));
    TEST_ASSERT_EQUAL_STRING("FREE", io_conn_state_name(IO_CONN_FREE));
    TEST_ASSERT_EQUAL_STRING("HTTP_ACTIVE", io_conn_state_name(IO_CONN_HTTP_ACTIVE));

    io_conn_pool_destroy(pool);
}

/* ---- Invalid state transitions ---- */

void test_conn_state_invalid_transition(void)
{
    io_conn_pool_t *pool = io_conn_pool_create(4);
    TEST_ASSERT_NOT_NULL(pool);

    io_conn_t *conn = io_conn_alloc(pool);
    TEST_ASSERT_NOT_NULL(conn);

    /* Force state to FREE to test invalid transition */
    conn->state = IO_CONN_FREE;

    /* FREE → HTTP_ACTIVE is invalid */
    TEST_ASSERT_EQUAL_INT(-EINVAL, io_conn_transition(conn, IO_CONN_HTTP_ACTIVE));
    TEST_ASSERT_EQUAL_UINT8(IO_CONN_FREE, conn->state);

    /* FREE → CLOSING is invalid */
    TEST_ASSERT_EQUAL_INT(-EINVAL, io_conn_transition(conn, IO_CONN_CLOSING));

    /* nullptr conn */
    TEST_ASSERT_EQUAL_INT(-EINVAL, io_conn_transition(nullptr, IO_CONN_ACCEPTING));

    /* CLOSING → anything except FREE (which is only via io_conn_free) */
    conn->state = IO_CONN_CLOSING;
    TEST_ASSERT_EQUAL_INT(-EINVAL, io_conn_transition(conn, IO_CONN_HTTP_ACTIVE));
    TEST_ASSERT_EQUAL_INT(-EINVAL, io_conn_transition(conn, IO_CONN_ACCEPTING));

    io_conn_pool_destroy(pool);
}

/* ---- Recv buffer tests ---- */

void test_conn_alloc_has_recv_buffer(void)
{
    io_conn_pool_t *pool = io_conn_pool_create(4);
    TEST_ASSERT_NOT_NULL(pool);

    io_conn_t *conn = io_conn_alloc(pool);
    TEST_ASSERT_NOT_NULL(conn);
    TEST_ASSERT_NOT_NULL(conn->recv_buf);
    TEST_ASSERT_GREATER_THAN(0, (int)conn->recv_buf_size);
    TEST_ASSERT_EQUAL(0, conn->recv_len);
    TEST_ASSERT_FALSE(conn->send_active);

    io_conn_free(pool, conn);
    io_conn_pool_destroy(pool);
}

/* ---- Pool get tests ---- */

void test_conn_pool_get_valid(void)
{
    io_conn_pool_t *pool = io_conn_pool_create(4);
    TEST_ASSERT_NOT_NULL(pool);

    io_conn_t *conn = io_conn_alloc(pool);
    TEST_ASSERT_NOT_NULL(conn);

    /* Index 0 should return the first slot */
    io_conn_t *got = io_conn_pool_get(pool, 0);
    TEST_ASSERT_NOT_NULL(got);
    TEST_ASSERT_EQUAL_PTR(conn, got);

    /* Out of range returns nullptr */
    TEST_ASSERT_NULL(io_conn_pool_get(pool, 999));

    /* nullptr pool returns nullptr */
    TEST_ASSERT_NULL(io_conn_pool_get(nullptr, 0));

    io_conn_pool_destroy(pool);
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

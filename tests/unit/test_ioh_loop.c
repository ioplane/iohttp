/**
 * @file test_io_loop.c
 * @brief Unit tests for ioh_loop ring management wrapper.
 */

#include "core/ioh_loop.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/uio.h>
#include <unistd.h>

#include <unity.h>

void setUp(void)
{
}
void tearDown(void)
{
}

/* ---- Config tests ---- */

void test_loop_config_defaults(void)
{
    ioh_loop_config_t cfg;
    ioh_loop_config_init(&cfg);

    TEST_ASSERT_EQUAL_UINT32(256, cfg.queue_depth);
    TEST_ASSERT_EQUAL_UINT32(128, cfg.buf_ring_size);
    TEST_ASSERT_EQUAL_UINT32(4096, cfg.buf_size);
    TEST_ASSERT_TRUE(cfg.defer_taskrun);
}

void test_loop_config_validate_valid(void)
{
    ioh_loop_config_t cfg;
    ioh_loop_config_init(&cfg);

    TEST_ASSERT_EQUAL_INT(0, ioh_loop_config_validate(&cfg));
}

void test_loop_config_validate_invalid(void)
{
    ioh_loop_config_t cfg;
    ioh_loop_config_init(&cfg);

    /* queue_depth = 0 is invalid */
    cfg.queue_depth = 0;
    TEST_ASSERT_EQUAL_INT(-EINVAL, ioh_loop_config_validate(&cfg));

    /* non-power-of-2 queue_depth */
    cfg.queue_depth = 100;
    TEST_ASSERT_EQUAL_INT(-EINVAL, ioh_loop_config_validate(&cfg));

    /* nullptr config */
    TEST_ASSERT_EQUAL_INT(-EINVAL, ioh_loop_config_validate(nullptr));
}

/* ---- Lifecycle tests ---- */

void test_loop_create_destroy(void)
{
    ioh_loop_config_t cfg;
    ioh_loop_config_init(&cfg);

    ioh_loop_t *loop = ioh_loop_create(&cfg);
    TEST_ASSERT_NOT_NULL(loop);
    TEST_ASSERT_NOT_NULL(ioh_loop_ring(loop));

    ioh_loop_destroy(loop);

    /* Destroying nullptr should not crash */
    ioh_loop_destroy(nullptr);
}

/* ---- NOP test ---- */

void test_loop_nop_submit(void)
{
    ioh_loop_config_t cfg;
    ioh_loop_config_init(&cfg);

    ioh_loop_t *loop = ioh_loop_create(&cfg);
    TEST_ASSERT_NOT_NULL(loop);

    int ret = ioh_loop_submit_nop(loop);
    TEST_ASSERT_EQUAL_INT(0, ret);

    /* Run once to drain the CQE */
    ret = ioh_loop_run_once(loop, 1000);
    TEST_ASSERT_GREATER_THAN(0, ret);

    ioh_loop_destroy(loop);
}

/* ---- Timer tests ---- */

static bool timer_fired_flag = false;
static void timer_callback(void *data)
{
    bool *flag = data;
    *flag = true;
}

void test_loop_timer_fires(void)
{
    ioh_loop_config_t cfg;
    ioh_loop_config_init(&cfg);

    ioh_loop_t *loop = ioh_loop_create(&cfg);
    TEST_ASSERT_NOT_NULL(loop);

    timer_fired_flag = false;
    int timer_id = ioh_loop_add_timer(loop, 10, timer_callback, &timer_fired_flag);
    TEST_ASSERT_GREATER_OR_EQUAL(0, timer_id);

    /* Run loop — timer should fire within 10ms + some slack */
    int ret = ioh_loop_run_once(loop, 1000);
    TEST_ASSERT_GREATER_THAN(0, ret);
    TEST_ASSERT_TRUE(timer_fired_flag);

    ioh_loop_destroy(loop);
}

void test_loop_timer_cancel(void)
{
    ioh_loop_config_t cfg;
    ioh_loop_config_init(&cfg);

    ioh_loop_t *loop = ioh_loop_create(&cfg);
    TEST_ASSERT_NOT_NULL(loop);

    timer_fired_flag = false;
    int timer_id = ioh_loop_add_timer(loop, 5000, timer_callback, &timer_fired_flag);
    TEST_ASSERT_GREATER_OR_EQUAL(0, timer_id);

    int ret = ioh_loop_cancel_timer(loop, timer_id);
    TEST_ASSERT_EQUAL_INT(0, ret);

    /* Run once to process the cancel + timeout CQEs */
    ret = ioh_loop_run_once(loop, 500);
    TEST_ASSERT_GREATER_THAN(0, ret);

    /* Callback should NOT have been called since we cancelled */
    TEST_ASSERT_FALSE(timer_fired_flag);

    ioh_loop_destroy(loop);
}

/* ---- Linked timeout tests ---- */

void test_loop_linked_timeout_fires(void)
{
    ioh_loop_config_t cfg;
    ioh_loop_config_init(&cfg);

    ioh_loop_t *loop = ioh_loop_create(&cfg);
    TEST_ASSERT_NOT_NULL(loop);

    struct io_uring *ring = ioh_loop_ring(loop);

    /*
     * Create a recv on an invalid fd — it will stall until the linked
     * timeout fires.  We use a pipe fd that nobody writes to.
     */
    int pipefd[2];
    TEST_ASSERT_EQUAL_INT(0, pipe(pipefd));

    /* SQE 1: recv on read end (will block) */
    struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
    TEST_ASSERT_NOT_NULL(sqe);
    char buf[64];
    io_uring_prep_recv(sqe, pipefd[0], buf, sizeof(buf), 0);
    io_uring_sqe_set_data64(sqe, IOH_ENCODE_USERDATA(1, IOH_OP_RECV));
    sqe->flags |= IOSQE_IO_LINK;

    /* SQE 2: linked timeout — 50ms */
    sqe = io_uring_get_sqe(ring);
    TEST_ASSERT_NOT_NULL(sqe);
    struct __kernel_timespec ts = {.tv_sec = 0, .tv_nsec = 50000000LL};
    io_uring_prep_link_timeout(sqe, &ts, 0);
    io_uring_sqe_set_data64(sqe, IOH_ENCODE_USERDATA(1, IOH_OP_TIMEOUT));

    /* Run — expect 2 CQEs total (cancelled recv + fired timeout).
     * They may arrive across multiple run_once calls. */
    int total = 0;
    for (int i = 0; i < 3 && total < 2; i++) {
        int ret = ioh_loop_run_once(loop, 2000);
        TEST_ASSERT_GREATER_THAN(0, ret);
        total += ret;
    }
    TEST_ASSERT_GREATER_OR_EQUAL(2, total);

    close(pipefd[0]);
    close(pipefd[1]);
    ioh_loop_destroy(loop);
}

void test_loop_linked_timeout_no_fire(void)
{
    ioh_loop_config_t cfg;
    ioh_loop_config_init(&cfg);

    ioh_loop_t *loop = ioh_loop_create(&cfg);
    TEST_ASSERT_NOT_NULL(loop);

    struct io_uring *ring = ioh_loop_ring(loop);

    /*
     * NOP completes instantly, so the linked timeout should be cancelled.
     */
    struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
    TEST_ASSERT_NOT_NULL(sqe);
    io_uring_prep_nop(sqe);
    io_uring_sqe_set_data64(sqe, IOH_ENCODE_USERDATA(2, IOH_OP_NOP));
    sqe->flags |= IOSQE_IO_LINK;

    sqe = io_uring_get_sqe(ring);
    TEST_ASSERT_NOT_NULL(sqe);
    struct __kernel_timespec ts = {.tv_sec = 5, .tv_nsec = 0};
    io_uring_prep_link_timeout(sqe, &ts, 0);
    io_uring_sqe_set_data64(sqe, IOH_ENCODE_USERDATA(2, IOH_OP_TIMEOUT));

    /* Expect 2 CQEs total: NOP success + timeout cancelled.
     * They may arrive across multiple run_once calls. */
    int total = 0;
    for (int i = 0; i < 3 && total < 2; i++) {
        int ret = ioh_loop_run_once(loop, 2000);
        TEST_ASSERT_GREATER_THAN(0, ret);
        total += ret;
    }
    TEST_ASSERT_GREATER_OR_EQUAL(2, total);

    ioh_loop_destroy(loop);
}

/* ---- Stop test ---- */

void test_loop_stop(void)
{
    ioh_loop_config_t cfg;
    ioh_loop_config_init(&cfg);

    ioh_loop_t *loop = ioh_loop_create(&cfg);
    TEST_ASSERT_NOT_NULL(loop);

    ioh_loop_stop(loop);

    /* run_once should return 0 immediately when stopped */
    int ret = ioh_loop_run_once(loop, 1000);
    TEST_ASSERT_EQUAL_INT(0, ret);

    ioh_loop_destroy(loop);
}

/* ---- Provided buffer ring test ---- */

void test_loop_provided_buffer_ring(void)
{
    ioh_loop_config_t cfg;
    ioh_loop_config_init(&cfg);

    ioh_loop_t *loop = ioh_loop_create(&cfg);
    TEST_ASSERT_NOT_NULL(loop);

    int ret = ioh_loop_setup_buf_ring(loop, 0, 16, 4096);
    TEST_ASSERT_EQUAL_INT(0, ret);

    /* Verify we can get buffer pointers */
    uint8_t *b0 = ioh_loop_get_buf(loop, 0, 0);
    TEST_ASSERT_NOT_NULL(b0);

    uint8_t *b1 = ioh_loop_get_buf(loop, 0, 1);
    TEST_ASSERT_NOT_NULL(b1);
    TEST_ASSERT_NOT_EQUAL(b0, b1);

    /* Buffers should be 4096 bytes apart */
    TEST_ASSERT_EQUAL_PTR(b0 + 4096, b1);

    /* Out of range buffer returns nullptr */
    TEST_ASSERT_NULL(ioh_loop_get_buf(loop, 0, 16));

    /* Wrong bgid returns nullptr */
    TEST_ASSERT_NULL(ioh_loop_get_buf(loop, 1, 0));

    /* Return a buffer and verify no crash */
    ioh_loop_return_buf(loop, 0, 0, b0, 4096);

    ioh_loop_destroy(loop);
}

/* ---- Registered buffers test ---- */

void test_loop_register_buffers(void)
{
    ioh_loop_config_t cfg;
    ioh_loop_config_init(&cfg);

    ioh_loop_t *loop = ioh_loop_create(&cfg);
    TEST_ASSERT_NOT_NULL(loop);

    char buf1[4096];
    char buf2[4096];
    struct iovec iovs[2] = {
        {.iov_base = buf1, .iov_len = sizeof(buf1)},
        {.iov_base = buf2, .iov_len = sizeof(buf2)},
    };

    int ret = ioh_loop_register_buffers(loop, iovs, 2);
    TEST_ASSERT_EQUAL_INT(0, ret);

    ret = ioh_loop_unregister_buffers(loop);
    TEST_ASSERT_EQUAL_INT(0, ret);

    ioh_loop_destroy(loop);
}

/* ---- Registered files test ---- */

void test_loop_register_files(void)
{
    ioh_loop_config_t cfg;
    ioh_loop_config_init(&cfg);

    ioh_loop_t *loop = ioh_loop_create(&cfg);
    TEST_ASSERT_NOT_NULL(loop);

    int pipefd[2];
    TEST_ASSERT_EQUAL_INT(0, pipe(pipefd));

    int ret = ioh_loop_register_files(loop, pipefd, 2);
    TEST_ASSERT_EQUAL_INT(0, ret);

    ret = ioh_loop_unregister_files(loop);
    TEST_ASSERT_EQUAL_INT(0, ret);

    close(pipefd[0]);
    close(pipefd[1]);
    ioh_loop_destroy(loop);
}

/* ---- Userdata encoding test ---- */

void test_loop_userdata_encoding(void)
{
    /* Basic encoding/decoding */
    uint64_t ud = IOH_ENCODE_USERDATA(42, IOH_OP_RECV);
    TEST_ASSERT_EQUAL_UINT8(IOH_OP_RECV, IOH_DECODE_OP(ud));
    TEST_ASSERT_EQUAL_UINT64(42, IOH_DECODE_ID(ud));

    /* Zero values */
    ud = IOH_ENCODE_USERDATA(0, IOH_OP_NOP);
    TEST_ASSERT_EQUAL_UINT8(IOH_OP_NOP, IOH_DECODE_OP(ud));
    TEST_ASSERT_EQUAL_UINT64(0, IOH_DECODE_ID(ud));

    /* Large ID */
    ud = IOH_ENCODE_USERDATA(0xFFFFFFFFULL, IOH_OP_SEND_ZC);
    TEST_ASSERT_EQUAL_UINT8(IOH_OP_SEND_ZC, IOH_DECODE_OP(ud));
    TEST_ASSERT_EQUAL_UINT64(0xFFFFFFFFULL, IOH_DECODE_ID(ud));

    /* All op types round-trip correctly */
    for (uint8_t op = 0; op <= IOH_OP_CANCEL; op++) {
        ud = IOH_ENCODE_USERDATA(100, op);
        TEST_ASSERT_EQUAL_UINT8(op, IOH_DECODE_OP(ud));
        TEST_ASSERT_EQUAL_UINT64(100, IOH_DECODE_ID(ud));
    }
}

/* ---- Test runner ---- */

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_loop_config_defaults);
    RUN_TEST(test_loop_config_validate_valid);
    RUN_TEST(test_loop_config_validate_invalid);
    RUN_TEST(test_loop_create_destroy);
    RUN_TEST(test_loop_nop_submit);
    RUN_TEST(test_loop_timer_fires);
    RUN_TEST(test_loop_timer_cancel);
    RUN_TEST(test_loop_linked_timeout_fires);
    RUN_TEST(test_loop_linked_timeout_no_fire);
    RUN_TEST(test_loop_stop);
    RUN_TEST(test_loop_provided_buffer_ring);
    RUN_TEST(test_loop_register_buffers);
    RUN_TEST(test_loop_register_files);
    RUN_TEST(test_loop_userdata_encoding);

    return UNITY_END();
}

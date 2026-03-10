/**
 * @file test_io_buffer.c
 * @brief Unit tests for ioh_buffer pool management.
 */

#include "core/ioh_buffer.h"

#include <errno.h>
#include <string.h>
#include <unistd.h>

#include <unity.h>

static struct io_uring test_ring;

void setUp(void)
{
    struct io_uring_params params = {0};
    int ret = io_uring_queue_init_params(64, &test_ring, &params);
    TEST_ASSERT_EQUAL_INT(0, ret);
}

void tearDown(void)
{
    io_uring_queue_exit(&test_ring);
}

/* ---- Config tests ---- */

void test_bufpool_config_defaults(void)
{
    ioh_bufpool_config_t cfg;
    ioh_bufpool_config_init(&cfg);

    TEST_ASSERT_EQUAL_UINT32(128, cfg.ring_size);
    TEST_ASSERT_EQUAL_UINT32(4096, cfg.buf_size);
    TEST_ASSERT_EQUAL_UINT16(0, cfg.bgid);
    TEST_ASSERT_EQUAL_UINT32(0, cfg.reg_buf_count);
    TEST_ASSERT_EQUAL_UINT32(16384, cfg.reg_buf_size);
    TEST_ASSERT_EQUAL_UINT32(0, cfg.reg_file_count);
}

void test_bufpool_config_validate_valid(void)
{
    ioh_bufpool_config_t cfg;
    ioh_bufpool_config_init(&cfg);

    TEST_ASSERT_EQUAL_INT(0, ioh_bufpool_config_validate(&cfg));

    /* With registered buffers enabled */
    cfg.reg_buf_count = 4;
    cfg.reg_buf_size = 16384;
    TEST_ASSERT_EQUAL_INT(0, ioh_bufpool_config_validate(&cfg));
}

void test_bufpool_config_validate_invalid(void)
{
    ioh_bufpool_config_t cfg;
    ioh_bufpool_config_init(&cfg);

    /* ring_size not power of 2 */
    cfg.ring_size = 100;
    TEST_ASSERT_EQUAL_INT(-EINVAL, ioh_bufpool_config_validate(&cfg));

    /* ring_size = 0 */
    cfg.ring_size = 0;
    TEST_ASSERT_EQUAL_INT(-EINVAL, ioh_bufpool_config_validate(&cfg));

    /* buf_size = 0 */
    ioh_bufpool_config_init(&cfg);
    cfg.buf_size = 0;
    TEST_ASSERT_EQUAL_INT(-EINVAL, ioh_bufpool_config_validate(&cfg));

    /* reg_buf_count > 0 but reg_buf_size = 0 */
    ioh_bufpool_config_init(&cfg);
    cfg.reg_buf_count = 4;
    cfg.reg_buf_size = 0;
    TEST_ASSERT_EQUAL_INT(-EINVAL, ioh_bufpool_config_validate(&cfg));

    /* nullptr config */
    TEST_ASSERT_EQUAL_INT(-EINVAL, ioh_bufpool_config_validate(nullptr));
}

/* ---- Lifecycle tests ---- */

void test_bufpool_create_destroy(void)
{
    ioh_bufpool_config_t cfg;
    ioh_bufpool_config_init(&cfg);

    ioh_bufpool_t *pool = ioh_bufpool_create(&cfg);
    TEST_ASSERT_NOT_NULL(pool);

    TEST_ASSERT_EQUAL_UINT32(128, ioh_bufpool_ring_size(pool));
    TEST_ASSERT_EQUAL_UINT32(4096, ioh_bufpool_buf_size(pool));
    TEST_ASSERT_EQUAL_UINT32(0, ioh_bufpool_reg_buf_count(pool));
    TEST_ASSERT_EQUAL_UINT32(0, ioh_bufpool_reg_file_count(pool));

    /* Destroy without registration — no ring needed */
    ioh_bufpool_destroy(pool, nullptr);

    /* Destroying nullptr should not crash */
    ioh_bufpool_destroy(nullptr, nullptr);
}

/* ---- Buffer access tests ---- */

void test_bufpool_get_buf_valid_id(void)
{
    ioh_bufpool_config_t cfg;
    ioh_bufpool_config_init(&cfg);
    cfg.ring_size = 16;
    cfg.buf_size = 4096;

    ioh_bufpool_t *pool = ioh_bufpool_create(&cfg);
    TEST_ASSERT_NOT_NULL(pool);

    uint8_t *b0 = ioh_bufpool_get_buf(pool, 0);
    TEST_ASSERT_NOT_NULL(b0);

    uint8_t *b1 = ioh_bufpool_get_buf(pool, 1);
    TEST_ASSERT_NOT_NULL(b1);
    TEST_ASSERT_NOT_EQUAL(b0, b1);

    /* Buffers should be buf_size bytes apart */
    TEST_ASSERT_EQUAL_PTR(b0 + 4096, b1);

    /* Last valid buffer */
    uint8_t *b15 = ioh_bufpool_get_buf(pool, 15);
    TEST_ASSERT_NOT_NULL(b15);

    ioh_bufpool_destroy(pool, nullptr);
}

void test_bufpool_get_buf_invalid_id(void)
{
    ioh_bufpool_config_t cfg;
    ioh_bufpool_config_init(&cfg);
    cfg.ring_size = 16;

    ioh_bufpool_t *pool = ioh_bufpool_create(&cfg);
    TEST_ASSERT_NOT_NULL(pool);

    /* Out of range */
    TEST_ASSERT_NULL(ioh_bufpool_get_buf(pool, 16));
    TEST_ASSERT_NULL(ioh_bufpool_get_buf(pool, 1000));

    /* nullptr pool */
    TEST_ASSERT_NULL(ioh_bufpool_get_buf(nullptr, 0));

    ioh_bufpool_destroy(pool, nullptr);
}

/* ---- Registration tests ---- */

void test_bufpool_register_ring(void)
{
    ioh_bufpool_config_t cfg;
    ioh_bufpool_config_init(&cfg);
    cfg.ring_size = 16;
    cfg.buf_size = 4096;

    ioh_bufpool_t *pool = ioh_bufpool_create(&cfg);
    TEST_ASSERT_NOT_NULL(pool);

    int ret = ioh_bufpool_register_ring(pool, &test_ring);
    TEST_ASSERT_EQUAL_INT(0, ret);

    /* Get a buffer pointer */
    uint8_t *buf = ioh_bufpool_get_buf(pool, 0);
    TEST_ASSERT_NOT_NULL(buf);

    /* Write something to it and return */
    memset(buf, 0xAB, 64);
    ioh_bufpool_return_buf(pool, 0);

    /* Double registration should fail */
    ret = ioh_bufpool_register_ring(pool, &test_ring);
    TEST_ASSERT_EQUAL_INT(-EALREADY, ret);

    ioh_bufpool_destroy(pool, &test_ring);
}

void test_bufpool_register_bufs(void)
{
    ioh_bufpool_config_t cfg;
    ioh_bufpool_config_init(&cfg);
    cfg.reg_buf_count = 4;
    cfg.reg_buf_size = 8192;

    ioh_bufpool_t *pool = ioh_bufpool_create(&cfg);
    TEST_ASSERT_NOT_NULL(pool);

    int ret = ioh_bufpool_register_bufs(pool, &test_ring);
    TEST_ASSERT_EQUAL_INT(0, ret);

    /* Access registered buffers */
    uint8_t *rb0 = ioh_bufpool_get_reg_buf(pool, 0);
    TEST_ASSERT_NOT_NULL(rb0);

    uint8_t *rb3 = ioh_bufpool_get_reg_buf(pool, 3);
    TEST_ASSERT_NOT_NULL(rb3);

    /* Out of range */
    TEST_ASSERT_NULL(ioh_bufpool_get_reg_buf(pool, 4));

    /* Buffers should be reg_buf_size apart */
    TEST_ASSERT_EQUAL_PTR(rb0 + 8192, ioh_bufpool_get_reg_buf(pool, 1));

    /* Double registration should fail */
    ret = ioh_bufpool_register_bufs(pool, &test_ring);
    TEST_ASSERT_EQUAL_INT(-EALREADY, ret);

    ioh_bufpool_destroy(pool, &test_ring);
}

void test_bufpool_register_files(void)
{
    ioh_bufpool_config_t cfg;
    ioh_bufpool_config_init(&cfg);
    cfg.reg_file_count = 8;

    ioh_bufpool_t *pool = ioh_bufpool_create(&cfg);
    TEST_ASSERT_NOT_NULL(pool);

    int ret = ioh_bufpool_register_files(pool, &test_ring);
    TEST_ASSERT_EQUAL_INT(0, ret);

    TEST_ASSERT_EQUAL_UINT32(8, ioh_bufpool_reg_file_count(pool));

    /* Double registration should fail */
    ret = ioh_bufpool_register_files(pool, &test_ring);
    TEST_ASSERT_EQUAL_INT(-EALREADY, ret);

    ioh_bufpool_destroy(pool, &test_ring);
}

void test_bufpool_register_fd_slot(void)
{
    ioh_bufpool_config_t cfg;
    ioh_bufpool_config_init(&cfg);
    cfg.reg_file_count = 4;

    ioh_bufpool_t *pool = ioh_bufpool_create(&cfg);
    TEST_ASSERT_NOT_NULL(pool);

    int ret = ioh_bufpool_register_files(pool, &test_ring);
    TEST_ASSERT_EQUAL_INT(0, ret);

    /* Create pipe fds to register */
    int pipefd[2];
    TEST_ASSERT_EQUAL_INT(0, pipe(pipefd));

    /* Register read end */
    int slot0 = ioh_bufpool_register_fd(pool, &test_ring, pipefd[0]);
    TEST_ASSERT_GREATER_OR_EQUAL(0, slot0);

    /* Register write end */
    int slot1 = ioh_bufpool_register_fd(pool, &test_ring, pipefd[1]);
    TEST_ASSERT_GREATER_OR_EQUAL(0, slot1);
    TEST_ASSERT_NOT_EQUAL(slot0, slot1);

    /* Unregister read end */
    ioh_bufpool_unregister_fd(pool, &test_ring, slot0);

    /* Re-register should reuse the freed slot */
    int slot2 = ioh_bufpool_register_fd(pool, &test_ring, pipefd[0]);
    TEST_ASSERT_EQUAL_INT(slot0, slot2);

    close(pipefd[0]);
    close(pipefd[1]);
    ioh_bufpool_destroy(pool, &test_ring);
}

/* ---- Test runner ---- */

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_bufpool_config_defaults);
    RUN_TEST(test_bufpool_config_validate_valid);
    RUN_TEST(test_bufpool_config_validate_invalid);
    RUN_TEST(test_bufpool_create_destroy);
    RUN_TEST(test_bufpool_get_buf_valid_id);
    RUN_TEST(test_bufpool_get_buf_invalid_id);
    RUN_TEST(test_bufpool_register_ring);
    RUN_TEST(test_bufpool_register_bufs);
    RUN_TEST(test_bufpool_register_files);
    RUN_TEST(test_bufpool_register_fd_slot);

    return UNITY_END();
}

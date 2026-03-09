/**
 * @file test_signal.c
 * @brief Integration tests for signalfd-based SIGTERM/SIGQUIT shutdown.
 */

#include "core/io_server.h"

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>

#include <unity.h>

/* Block SIGTERM + SIGQUIT process-wide so spawned threads inherit the mask.
 * io_server_run() creates a signalfd to consume these signals via io_uring. */
static sigset_t saved_mask;

void setUp(void)
{
}
void tearDown(void)
{
}

/* ---- Handler ---- */

static int on_request_cb(io_ctx_t *c, void *user_data)
{
    (void)user_data;
    (void)c;
    return 0;
}

/* ---- Signal sender thread ---- */

typedef struct {
    int signo;
    uint32_t delay_ms;
} signal_args_t;

static void *signal_sender(void *arg)
{
    signal_args_t *sa = (signal_args_t *)arg;
    struct timespec ts = {
        .tv_sec = (long)(sa->delay_ms / 1000),
        .tv_nsec = (long)(sa->delay_ms % 1000) * 1000000L,
    };
    nanosleep(&ts, nullptr);
    kill(getpid(), sa->signo);
    return nullptr;
}

/* ---- Test 1: SIGTERM triggers graceful shutdown ---- */

void test_sigterm_triggers_graceful_shutdown(void)
{
    io_server_config_t cfg;
    io_server_config_init(&cfg);
    cfg.listen_port = 19300;
    cfg.max_connections = 16;
    cfg.queue_depth = 32;
    cfg.keepalive_timeout_ms = 500;

    io_server_t *srv = io_server_create(&cfg);
    TEST_ASSERT_NOT_NULL(srv);
    TEST_ASSERT_EQUAL_INT(0, io_server_set_on_request(srv, on_request_cb, nullptr));

    /* Launch thread to send SIGTERM after 200ms */
    signal_args_t sa = {.signo = SIGTERM, .delay_ms = 200};
    pthread_t tid;
    int prc = pthread_create(&tid, nullptr, signal_sender, &sa);
    TEST_ASSERT_EQUAL_INT(0, prc);

    /* io_server_run() should return 0 after receiving SIGTERM */
    int ret = io_server_run(srv);
    TEST_ASSERT_EQUAL_INT(0, ret);

    pthread_join(tid, nullptr);
    io_server_destroy(srv);
}

/* ---- Test 2: SIGQUIT triggers immediate shutdown ---- */

void test_sigquit_triggers_immediate_shutdown(void)
{
    io_server_config_t cfg;
    io_server_config_init(&cfg);
    cfg.listen_port = 19301;
    cfg.max_connections = 16;
    cfg.queue_depth = 32;
    cfg.keepalive_timeout_ms = 500;

    io_server_t *srv = io_server_create(&cfg);
    TEST_ASSERT_NOT_NULL(srv);
    TEST_ASSERT_EQUAL_INT(0, io_server_set_on_request(srv, on_request_cb, nullptr));

    /* Launch thread to send SIGQUIT after 200ms */
    signal_args_t sa = {.signo = SIGQUIT, .delay_ms = 200};
    pthread_t tid;
    int prc = pthread_create(&tid, nullptr, signal_sender, &sa);
    TEST_ASSERT_EQUAL_INT(0, prc);

    /* io_server_run() should return 0 after receiving SIGQUIT */
    int ret = io_server_run(srv);
    TEST_ASSERT_EQUAL_INT(0, ret);

    pthread_join(tid, nullptr);
    io_server_destroy(srv);
}

int main(void)
{
    /* Block SIGTERM + SIGQUIT in all threads so only signalfd receives them */
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGTERM);
    sigaddset(&mask, SIGQUIT);
    pthread_sigmask(SIG_BLOCK, &mask, &saved_mask);

    UNITY_BEGIN();
    RUN_TEST(test_sigterm_triggers_graceful_shutdown);
    RUN_TEST(test_sigquit_triggers_immediate_shutdown);
    int result = UNITY_END();

    /* Restore original signal mask */
    pthread_sigmask(SIG_SETMASK, &saved_mask, nullptr);

    return result;
}

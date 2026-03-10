/**
 * @file test_io_server.c
 * @brief Unit tests for ioh_server lifecycle management.
 */

#include "core/ioh_server.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <unity.h>

void setUp(void)
{
}
void tearDown(void)
{
}

/* ---- Helpers ---- */

static ioh_server_config_t make_config(uint16_t port, uint32_t max_conns)
{
    ioh_server_config_t cfg;
    ioh_server_config_init(&cfg);
    cfg.listen_addr = "127.0.0.1";
    cfg.listen_port = port;
    if (max_conns > 0) {
        cfg.max_connections = max_conns;
    }
    return cfg;
}

static uint16_t get_bound_port(int listen_fd)
{
    struct sockaddr_in addr;
    socklen_t len = sizeof(addr);
    int ret = getsockname(listen_fd, (struct sockaddr *)&addr, &len);
    TEST_ASSERT_EQUAL_INT(0, ret);
    return ntohs(addr.sin_port);
}

static int connect_client(uint16_t port)
{
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    TEST_ASSERT_TRUE(fd >= 0);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    int ret = connect(fd, (struct sockaddr *)&addr, sizeof(addr));
    TEST_ASSERT_EQUAL_INT(0, ret);

    return fd;
}

/* ---- Config tests ---- */

void test_server_config_defaults(void)
{
    ioh_server_config_t cfg;
    ioh_server_config_init(&cfg);

    TEST_ASSERT_NOT_NULL(cfg.listen_addr);
    TEST_ASSERT_EQUAL_STRING("0.0.0.0", cfg.listen_addr);
    TEST_ASSERT_EQUAL_UINT16(0, cfg.listen_port);
    TEST_ASSERT_EQUAL_UINT32(1024, cfg.max_connections);
    TEST_ASSERT_EQUAL_UINT32(256, cfg.queue_depth);
    TEST_ASSERT_EQUAL_UINT32(65000, cfg.keepalive_timeout_ms);
    TEST_ASSERT_EQUAL_UINT32(30000, cfg.header_timeout_ms);
    TEST_ASSERT_EQUAL_UINT32(60000, cfg.body_timeout_ms);
    TEST_ASSERT_EQUAL_UINT32(8192, cfg.max_header_size);
    TEST_ASSERT_EQUAL_UINT32(1048576, cfg.max_body_size);
    TEST_ASSERT_FALSE(cfg.proxy_protocol);
}

void test_server_config_validate_valid(void)
{
    ioh_server_config_t cfg;
    ioh_server_config_init(&cfg);
    cfg.listen_port = 8080;

    TEST_ASSERT_EQUAL_INT(0, ioh_server_config_validate(&cfg));
}

void test_server_config_validate_zero_port(void)
{
    ioh_server_config_t cfg;
    ioh_server_config_init(&cfg);

    TEST_ASSERT_EQUAL_INT(-EINVAL, ioh_server_config_validate(&cfg));
}

void test_server_config_validate_zero_conns(void)
{
    ioh_server_config_t cfg;
    ioh_server_config_init(&cfg);
    cfg.listen_port = 8080;
    cfg.max_connections = 0;

    TEST_ASSERT_EQUAL_INT(-EINVAL, ioh_server_config_validate(&cfg));
}

/* ---- Lifecycle tests ---- */

void test_server_create_destroy(void)
{
    ioh_server_config_t cfg = make_config(9999, 0);
    ioh_server_t *srv = ioh_server_create(&cfg);
    TEST_ASSERT_NOT_NULL(srv);

    TEST_ASSERT_NOT_NULL(ioh_server_loop(srv));
    TEST_ASSERT_NOT_NULL(ioh_server_pool(srv));
    TEST_ASSERT_EQUAL_INT(-1, ioh_server_listen_fd(srv));

    ioh_server_destroy(srv);

    /* nullptr destroy should not crash */
    ioh_server_destroy(nullptr);
}

/* ---- Listen test ---- */

void test_server_listen_binds(void)
{
    ioh_server_config_t cfg = make_config(18080, 16);
    ioh_server_t *srv = ioh_server_create(&cfg);
    TEST_ASSERT_NOT_NULL(srv);

    int fd = ioh_server_listen(srv);
    TEST_ASSERT_GREATER_THAN(0, fd);
    TEST_ASSERT_EQUAL_INT(fd, ioh_server_listen_fd(srv));

    uint16_t port = get_bound_port(fd);
    TEST_ASSERT_EQUAL_UINT16(18080, port);

    ioh_server_destroy(srv);
}

/* ---- Accept test ---- */

void test_server_accept_connection(void)
{
    ioh_server_config_t cfg = make_config(18081, 16);
    ioh_server_t *srv = ioh_server_create(&cfg);
    TEST_ASSERT_NOT_NULL(srv);

    int fd = ioh_server_listen(srv);
    TEST_ASSERT_GREATER_THAN(0, fd);

    uint16_t port = get_bound_port(fd);

    /* Connect a client */
    int client_fd = connect_client(port);
    TEST_ASSERT_TRUE(client_fd >= 0);

    /* Run server once to process the accept CQE */
    int ret = ioh_server_run_once(srv, 1000);
    TEST_ASSERT_GREATER_THAN(0, ret);

    /* Verify a connection was allocated in the pool */
    TEST_ASSERT_EQUAL_UINT32(1, ioh_conn_pool_active(ioh_server_pool(srv)));

    close(client_fd);
    ioh_server_destroy(srv);
}

/* ---- Stop test ---- */

void test_server_stop(void)
{
    ioh_server_config_t cfg = make_config(18082, 16);
    ioh_server_t *srv = ioh_server_create(&cfg);
    TEST_ASSERT_NOT_NULL(srv);

    int fd = ioh_server_listen(srv);
    TEST_ASSERT_GREATER_THAN(0, fd);

    ioh_server_stop(srv);

    /* run_once should return 0 immediately when stopped */
    int ret = ioh_server_run_once(srv, 100);
    TEST_ASSERT_EQUAL_INT(0, ret);

    ioh_server_destroy(srv);
}

/* ---- Shutdown immediate test ---- */

void test_server_shutdown_immediate(void)
{
    ioh_server_config_t cfg = make_config(18083, 16);
    ioh_server_t *srv = ioh_server_create(&cfg);
    TEST_ASSERT_NOT_NULL(srv);

    int fd = ioh_server_listen(srv);
    TEST_ASSERT_GREATER_THAN(0, fd);

    uint16_t port = get_bound_port(fd);

    /* Connect a client */
    int client_fd = connect_client(port);
    TEST_ASSERT_TRUE(client_fd >= 0);

    /* Process accept */
    int ret = ioh_server_run_once(srv, 1000);
    TEST_ASSERT_GREATER_THAN(0, ret);
    TEST_ASSERT_EQUAL_UINT32(1, ioh_conn_pool_active(ioh_server_pool(srv)));

    /* Shutdown immediate — closes listen fd, stops server */
    ret = ioh_server_shutdown(srv, IOH_SHUTDOWN_IMMEDIATE);
    TEST_ASSERT_EQUAL_INT(0, ret);

    /* Listen fd should be closed */
    TEST_ASSERT_EQUAL_INT(-1, ioh_server_listen_fd(srv));

    /* Server should be stopped */
    ret = ioh_server_run_once(srv, 100);
    TEST_ASSERT_EQUAL_INT(0, ret);

    close(client_fd);
    ioh_server_destroy(srv);
}

/* ---- Backpressure test ---- */

void test_server_accept_backpressure(void)
{
    /* Pool with max_connections = 1 */
    ioh_server_config_t cfg = make_config(18084, 1);
    ioh_server_t *srv = ioh_server_create(&cfg);
    TEST_ASSERT_NOT_NULL(srv);

    int fd = ioh_server_listen(srv);
    TEST_ASSERT_GREATER_THAN(0, fd);

    uint16_t port = get_bound_port(fd);

    /* First client fills the pool */
    int client1 = connect_client(port);
    TEST_ASSERT_TRUE(client1 >= 0);

    int ret = ioh_server_run_once(srv, 1000);
    TEST_ASSERT_GREATER_THAN(0, ret);
    TEST_ASSERT_EQUAL_UINT32(1, ioh_conn_pool_active(ioh_server_pool(srv)));

    /* Second client — pool full, accepted fd closed by server (backpressure) */
    int client2 = connect_client(port);
    TEST_ASSERT_TRUE(client2 >= 0);

    ret = ioh_server_run_once(srv, 1000);
    TEST_ASSERT_GREATER_THAN(0, ret);

    /* Pool should still have exactly 1 active connection */
    TEST_ASSERT_EQUAL_UINT32(1, ioh_conn_pool_active(ioh_server_pool(srv)));

    close(client1);
    close(client2);
    ioh_server_destroy(srv);
}

/* ---- Configuration extension tests ---- */

void test_server_set_router(void)
{
    ioh_server_config_t cfg;
    ioh_server_config_init(&cfg);
    cfg.listen_port = 19001;
    ioh_server_t *srv = ioh_server_create(&cfg);
    TEST_ASSERT_NOT_NULL(srv);

    TEST_ASSERT_EQUAL_INT(-EINVAL, ioh_server_set_router(nullptr, nullptr));
    TEST_ASSERT_EQUAL_INT(0, ioh_server_set_router(srv, nullptr));

    ioh_server_destroy(srv);
}

void test_server_set_on_request(void)
{
    ioh_server_config_t cfg;
    ioh_server_config_init(&cfg);
    cfg.listen_port = 19002;
    ioh_server_t *srv = ioh_server_create(&cfg);
    TEST_ASSERT_NOT_NULL(srv);

    TEST_ASSERT_EQUAL_INT(-EINVAL, ioh_server_set_on_request(nullptr, nullptr, nullptr));
    TEST_ASSERT_EQUAL_INT(0, ioh_server_set_on_request(srv, nullptr, nullptr));

    ioh_server_destroy(srv);
}

void test_server_set_tls(void)
{
    ioh_server_config_t cfg;
    ioh_server_config_init(&cfg);
    cfg.listen_port = 19003;
    ioh_server_t *srv = ioh_server_create(&cfg);
    TEST_ASSERT_NOT_NULL(srv);

    TEST_ASSERT_EQUAL_INT(-EINVAL, ioh_server_set_tls(nullptr, nullptr));
    TEST_ASSERT_EQUAL_INT(0, ioh_server_set_tls(srv, nullptr));

    ioh_server_destroy(srv);
}

/* ---- Accept arms recv test ---- */

void test_server_accept_arms_recv(void)
{
    ioh_server_config_t cfg = make_config(19010, 16);
    ioh_server_t *srv = ioh_server_create(&cfg);
    TEST_ASSERT_NOT_NULL(srv);

    int fd = ioh_server_listen(srv);
    TEST_ASSERT_GREATER_THAN(0, fd);
    uint16_t port = get_bound_port(fd);

    int client_fd = connect_client(port);
    TEST_ASSERT_TRUE(client_fd >= 0);

    int ret = ioh_server_run_once(srv, 1000);
    TEST_ASSERT_GREATER_THAN(0, ret);
    TEST_ASSERT_EQUAL_UINT32(1, ioh_conn_pool_active(ioh_server_pool(srv)));

    close(client_fd);
    ioh_server_destroy(srv);
}

/* ---- Run + stop test ---- */

void test_server_run_null(void)
{
    TEST_ASSERT_EQUAL_INT(-EINVAL, ioh_server_run(nullptr));
}

void test_server_run_stop(void)
{
    ioh_server_config_t cfg = make_config(19020, 16);
    ioh_server_t *srv = ioh_server_create(&cfg);
    TEST_ASSERT_NOT_NULL(srv);

    /* Stop the server first, then call run — it should return immediately */
    ioh_server_stop(srv);
    int ret = ioh_server_run(srv);
    TEST_ASSERT_EQUAL_INT(0, ret);

    ioh_server_destroy(srv);
}

/* ---- Timeout phase test ---- */

void test_server_conn_timeout_phase_default(void)
{
    ioh_server_config_t cfg = make_config(19030, 16);
    ioh_server_t *srv = ioh_server_create(&cfg);
    TEST_ASSERT_NOT_NULL(srv);

    int fd = ioh_server_listen(srv);
    TEST_ASSERT_GREATER_THAN(0, fd);
    uint16_t port = get_bound_port(fd);

    int client_fd = connect_client(port);
    TEST_ASSERT_TRUE(client_fd >= 0);

    /* Process accept — connection should get IOH_TIMEOUT_HEADER */
    int ret = ioh_server_run_once(srv, 1000);
    TEST_ASSERT_GREATER_THAN(0, ret);
    TEST_ASSERT_EQUAL_UINT32(1, ioh_conn_pool_active(ioh_server_pool(srv)));

    /* Verify the connection has HEADER timeout phase */
    ioh_conn_t *conn = ioh_conn_pool_get(ioh_server_pool(srv), 0);
    TEST_ASSERT_NOT_NULL(conn);
    TEST_ASSERT_EQUAL_UINT8(IOH_TIMEOUT_HEADER, conn->timeout_phase);

    close(client_fd);
    ioh_server_destroy(srv);
}

/* ---- Test runner ---- */

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_server_config_defaults);
    RUN_TEST(test_server_config_validate_valid);
    RUN_TEST(test_server_config_validate_zero_port);
    RUN_TEST(test_server_config_validate_zero_conns);
    RUN_TEST(test_server_create_destroy);
    RUN_TEST(test_server_listen_binds);
    RUN_TEST(test_server_accept_connection);
    RUN_TEST(test_server_stop);
    RUN_TEST(test_server_shutdown_immediate);
    RUN_TEST(test_server_accept_backpressure);
    RUN_TEST(test_server_set_router);
    RUN_TEST(test_server_set_on_request);
    RUN_TEST(test_server_set_tls);
    RUN_TEST(test_server_accept_arms_recv);
    RUN_TEST(test_server_run_null);
    RUN_TEST(test_server_run_stop);
    RUN_TEST(test_server_conn_timeout_phase_default);

    return UNITY_END();
}

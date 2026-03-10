/**
 * @file test_timeout.c
 * @brief Integration tests for linked recv timeouts (header/body/keepalive).
 */

#include "core/ioh_ctx.h"
#include "core/ioh_server.h"
#include "http/ioh_request.h"

#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <unity.h>

void setUp(void)
{
}
void tearDown(void)
{
}

/* ---- Helpers ---- */

static uint16_t get_bound_port(int fd)
{
    struct sockaddr_in addr;
    socklen_t len = sizeof(addr);
    int ret = getsockname(fd, (struct sockaddr *)&addr, &len);
    TEST_ASSERT_EQUAL_INT(0, ret);
    return ntohs(addr.sin_port);
}

static int connect_to(uint16_t port)
{
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return -errno;
    }
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -errno;
    }
    return fd;
}

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

/* ---- Handler ---- */

static int on_request_cb(ioh_ctx_t *c, void *user_data)
{
    (void)user_data;
    return ioh_ctx_text(c, 200, "OK");
}

/* ---- Test helpers ---- */

static uint16_t next_port = 19200;

static ioh_server_t *make_timeout_server(uint32_t header_ms, uint32_t keepalive_ms)
{
    ioh_server_config_t cfg;
    ioh_server_config_init(&cfg);
    cfg.listen_port = next_port++;
    cfg.max_connections = 16;
    cfg.queue_depth = 64;
    cfg.header_timeout_ms = header_ms;
    cfg.keepalive_timeout_ms = keepalive_ms;
    cfg.body_timeout_ms = 60000;
    return ioh_server_create(&cfg);
}

/* ---- Test 1: Header timeout closes idle connection ---- */

void test_header_timeout_closes_idle_connection(void)
{
    ioh_server_t *srv = make_timeout_server(500, 65000);
    TEST_ASSERT_NOT_NULL(srv);
    TEST_ASSERT_EQUAL_INT(0, ioh_server_set_on_request(srv, on_request_cb, nullptr));

    int listen_fd = ioh_server_listen(srv);
    TEST_ASSERT_GREATER_THAN(0, listen_fd);
    uint16_t port = get_bound_port(listen_fd);

    /* Connect but send nothing — should trigger header timeout */
    int client = connect_to(port);
    TEST_ASSERT_TRUE(client >= 0);

    /* Let server accept */
    (void)ioh_server_run_once(srv, 200);
    TEST_ASSERT_EQUAL_UINT32(1, ioh_conn_pool_active(ioh_server_pool(srv)));

    /* Poll the event loop until the connection closes or we exceed max wait */
    uint64_t start = now_ms();
    uint64_t max_wait_ms = 3000;

    while (ioh_conn_pool_active(ioh_server_pool(srv)) > 0 && (now_ms() - start) < max_wait_ms) {
        (void)ioh_server_run_once(srv, 100);
    }

    uint64_t elapsed = now_ms() - start;

    /* Connection should have been closed by the header timeout */
    TEST_ASSERT_EQUAL_UINT32(0, ioh_conn_pool_active(ioh_server_pool(srv)));

    /* Should have taken approximately 500ms (allow 300ms–2000ms range) */
    TEST_ASSERT_GREATER_THAN(300, elapsed);
    TEST_ASSERT_LESS_THAN(2000, elapsed);

    close(client);
    ioh_server_destroy(srv);
}

/* ---- Test 2: Keepalive timeout closes after response ---- */

void test_keepalive_timeout_closes_after_response(void)
{
    ioh_server_t *srv = make_timeout_server(5000, 500);
    TEST_ASSERT_NOT_NULL(srv);
    TEST_ASSERT_EQUAL_INT(0, ioh_server_set_on_request(srv, on_request_cb, nullptr));

    int listen_fd = ioh_server_listen(srv);
    TEST_ASSERT_GREATER_THAN(0, listen_fd);
    uint16_t port = get_bound_port(listen_fd);

    int client = connect_to(port);
    TEST_ASSERT_TRUE(client >= 0);

    /* Let server accept */
    (void)ioh_server_run_once(srv, 200);
    TEST_ASSERT_EQUAL_UINT32(1, ioh_conn_pool_active(ioh_server_pool(srv)));

    /* Send a valid HTTP request with Connection: keep-alive */
    const char *http_req = "GET / HTTP/1.1\r\nHost: localhost\r\nConnection: keep-alive\r\n\r\n";
    ssize_t sent = write(client, http_req, strlen(http_req));
    TEST_ASSERT_GREATER_THAN(0, sent);

    /* Process the request and send response */
    for (int i = 0; i < 5; i++) {
        (void)ioh_server_run_once(srv, 100);
    }

    /* Read the response */
    char buf[4096];
    struct pollfd pfd = {.fd = client, .events = POLLIN};
    if (poll(&pfd, 1, 500) > 0) {
        (void)read(client, buf, sizeof(buf));
    }

    /* Now idle — keepalive timeout should close the connection */
    uint64_t start = now_ms();
    uint64_t max_wait_ms = 3000;

    while (ioh_conn_pool_active(ioh_server_pool(srv)) > 0 && (now_ms() - start) < max_wait_ms) {
        (void)ioh_server_run_once(srv, 100);
    }

    uint64_t elapsed = now_ms() - start;

    /* Connection should have been closed by the keepalive timeout */
    TEST_ASSERT_EQUAL_UINT32(0, ioh_conn_pool_active(ioh_server_pool(srv)));

    /* Should have taken approximately 500ms (allow 200ms–2000ms range) */
    TEST_ASSERT_GREATER_THAN(200, elapsed);
    TEST_ASSERT_LESS_THAN(2000, elapsed);

    close(client);
    ioh_server_destroy(srv);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_header_timeout_closes_idle_connection);
    RUN_TEST(test_keepalive_timeout_closes_after_response);
    return UNITY_END();
}

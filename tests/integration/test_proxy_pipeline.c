/**
 * @file test_proxy_pipeline.c
 * @brief Integration tests for PROXY protocol in the server pipeline.
 */

#include "core/ioh_ctx.h"
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

static ioh_server_config_t make_config(uint16_t port)
{
    ioh_server_config_t cfg;
    ioh_server_config_init(&cfg);
    cfg.listen_addr = "127.0.0.1";
    cfg.listen_port = port;
    cfg.max_connections = 16;
    cfg.header_timeout_ms = 5000;
    cfg.proxy_protocol = true;
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

static int on_request_echo(ioh_ctx_t *c, void *user_data)
{
    (void)user_data;
    return ioh_ctx_text(c, 200, "OK");
}

void test_proxy_v1_tcp4_pipeline(void)
{
    ioh_server_config_t cfg = make_config(19500);
    ioh_server_t *srv = ioh_server_create(&cfg);
    TEST_ASSERT_NOT_NULL(srv);
    (void)ioh_server_set_on_request(srv, on_request_echo, nullptr);

    int fd = ioh_server_listen(srv);
    TEST_ASSERT_GREATER_THAN(0, fd);
    uint16_t port = get_bound_port(fd);

    int client_fd = connect_client(port);
    TEST_ASSERT_TRUE(client_fd >= 0);

    /* Send PROXY v1 header followed by HTTP request */
    const char *proxy_header = "PROXY TCP4 192.168.1.1 192.168.1.2 12345 80\r\n";
    const char *http_req = "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n";

    send(client_fd, proxy_header, strlen(proxy_header), 0);
    send(client_fd, http_req, strlen(http_req), 0);

    for (int i = 0; i < 15; i++) {
        (void)ioh_server_run_once(srv, 200);
    }

    char resp[4096] = {0};
    recv(client_fd, resp, sizeof(resp) - 1, 0);

    /* Should get a 200 response */
    TEST_ASSERT_NOT_NULL(strstr(resp, "200"));

    close(client_fd);
    ioh_server_destroy(srv);
}

void test_proxy_invalid_header_closes_connection(void)
{
    ioh_server_config_t cfg = make_config(19501);
    ioh_server_t *srv = ioh_server_create(&cfg);
    TEST_ASSERT_NOT_NULL(srv);
    (void)ioh_server_set_on_request(srv, on_request_echo, nullptr);

    int fd = ioh_server_listen(srv);
    TEST_ASSERT_GREATER_THAN(0, fd);
    uint16_t port = get_bound_port(fd);

    int client_fd = connect_client(port);
    TEST_ASSERT_TRUE(client_fd >= 0);

    /* Send garbage instead of PROXY header */
    const char *garbage = "NOT_A_PROXY_HEADER\r\n";
    send(client_fd, garbage, strlen(garbage), 0);

    for (int i = 0; i < 10; i++) {
        (void)ioh_server_run_once(srv, 200);
    }

    /* Connection should be closed */
    TEST_ASSERT_EQUAL_UINT32(0, ioh_conn_pool_active(ioh_server_pool(srv)));

    close(client_fd);
    ioh_server_destroy(srv);
}

void test_non_proxy_listener_ignores_proxy_headers(void)
{
    ioh_server_config_t cfg = make_config(19502);
    cfg.proxy_protocol = false; /* NOT a PROXY listener */
    ioh_server_t *srv = ioh_server_create(&cfg);
    TEST_ASSERT_NOT_NULL(srv);
    (void)ioh_server_set_on_request(srv, on_request_echo, nullptr);

    int fd = ioh_server_listen(srv);
    TEST_ASSERT_GREATER_THAN(0, fd);
    uint16_t port = get_bound_port(fd);

    int client_fd = connect_client(port);
    TEST_ASSERT_TRUE(client_fd >= 0);

    /* Send normal HTTP request (no PROXY) */
    const char *http_req = "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n";
    send(client_fd, http_req, strlen(http_req), 0);

    for (int i = 0; i < 10; i++) {
        (void)ioh_server_run_once(srv, 200);
    }

    char resp[4096] = {0};
    recv(client_fd, resp, sizeof(resp) - 1, 0);
    TEST_ASSERT_NOT_NULL(strstr(resp, "200"));

    close(client_fd);
    ioh_server_destroy(srv);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_proxy_v1_tcp4_pipeline);
    RUN_TEST(test_proxy_invalid_header_closes_connection);
    RUN_TEST(test_non_proxy_listener_ignores_proxy_headers);
    return UNITY_END();
}

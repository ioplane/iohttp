/**
 * @file test_limits.c
 * @brief Integration tests for request header size (431) and body size (413) limits.
 */

#include "core/ioh_ctx.h"
#include "core/ioh_server.h"
#include "http/ioh_request.h"

#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
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

static uint16_t get_bound_port(int fd)
{
    struct sockaddr_in addr;
    socklen_t len = sizeof(addr);
    int ret = getsockname(fd, (struct sockaddr *)&addr, &len);
    TEST_ASSERT_EQUAL_INT(0, ret);
    return ntohs(addr.sin_port);
}

/* ---- Handler ---- */

static int on_request_cb(ioh_ctx_t *c, void *user_data)
{
    (void)user_data;
    return ioh_ctx_text(c, 200, "OK");
}

/* ---- Test 1: Oversized headers return 431 ---- */

void test_oversized_header_returns_431(void)
{
    ioh_server_config_t cfg;
    ioh_server_config_init(&cfg);
    cfg.listen_port = 19200;
    cfg.max_connections = 16;
    cfg.queue_depth = 64;
    cfg.max_header_size = 256;

    ioh_server_t *srv = ioh_server_create(&cfg);
    TEST_ASSERT_NOT_NULL(srv);
    TEST_ASSERT_EQUAL_INT(0, ioh_server_set_on_request(srv, on_request_cb, nullptr));

    int listen_fd = ioh_server_listen(srv);
    TEST_ASSERT_GREATER_THAN(0, listen_fd);
    uint16_t port = get_bound_port(listen_fd);

    int client = connect_to(port);
    TEST_ASSERT_TRUE(client >= 0);

    /* Let server accept */
    (void)ioh_server_run_once(srv, 200);

    /* Build a request with headers exceeding 256 bytes */
    char big_request[512];
    memset(big_request, 0, sizeof(big_request));
    int off = snprintf(big_request, sizeof(big_request),
                       "GET / HTTP/1.1\r\nHost: localhost\r\n"
                       "X-Padding: ");
    /* Fill until we exceed 256 bytes total */
    while (off < 300) {
        big_request[off++] = 'A';
    }
    big_request[off++] = '\r';
    big_request[off++] = '\n';
    big_request[off++] = '\r';
    big_request[off++] = '\n';

    ssize_t sent = write(client, big_request, (size_t)off);
    TEST_ASSERT_GREATER_THAN(0, sent);

    /* Process request */
    for (int i = 0; i < 10; i++) {
        (void)ioh_server_run_once(srv, 100);
    }

    /* Read response */
    char buf[4096];
    memset(buf, 0, sizeof(buf));
    struct pollfd pfd = {.fd = client, .events = POLLIN};
    if (poll(&pfd, 1, 1000) > 0) {
        (void)read(client, buf, sizeof(buf) - 1);
    }

    /* Verify 431 status */
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(buf, "431"), "Expected 431 in response");

    close(client);
    ioh_server_destroy(srv);
}

/* ---- Test 2: Oversized body returns 413 ---- */

void test_oversized_body_returns_413(void)
{
    ioh_server_config_t cfg;
    ioh_server_config_init(&cfg);
    cfg.listen_port = 19201;
    cfg.max_connections = 16;
    cfg.queue_depth = 64;
    cfg.max_body_size = 64;

    ioh_server_t *srv = ioh_server_create(&cfg);
    TEST_ASSERT_NOT_NULL(srv);
    TEST_ASSERT_EQUAL_INT(0, ioh_server_set_on_request(srv, on_request_cb, nullptr));

    int listen_fd = ioh_server_listen(srv);
    TEST_ASSERT_GREATER_THAN(0, listen_fd);
    uint16_t port = get_bound_port(listen_fd);

    int client = connect_to(port);
    TEST_ASSERT_TRUE(client >= 0);

    /* Let server accept */
    (void)ioh_server_run_once(srv, 200);

    /* Send a POST with Content-Length exceeding max_body_size */
    const char *http_req = "POST / HTTP/1.1\r\n"
                           "Host: localhost\r\n"
                           "Content-Length: 128\r\n"
                           "\r\n";
    ssize_t sent = write(client, http_req, strlen(http_req));
    TEST_ASSERT_GREATER_THAN(0, sent);

    /* Process request */
    for (int i = 0; i < 10; i++) {
        (void)ioh_server_run_once(srv, 100);
    }

    /* Read response */
    char buf[4096];
    memset(buf, 0, sizeof(buf));
    struct pollfd pfd = {.fd = client, .events = POLLIN};
    if (poll(&pfd, 1, 1000) > 0) {
        (void)read(client, buf, sizeof(buf) - 1);
    }

    /* Verify 413 status */
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(buf, "413"), "Expected 413 in response");

    close(client);
    ioh_server_destroy(srv);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_oversized_header_returns_431);
    RUN_TEST(test_oversized_body_returns_413);
    return UNITY_END();
}

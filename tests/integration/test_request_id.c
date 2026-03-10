/**
 * @file test_request_id.c
 * @brief Integration tests: X-Request-Id generation and propagation.
 */

#include "core/ioh_ctx.h"
#include "core/ioh_server.h"
#include "router/ioh_router.h"

#include <ctype.h>
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

static int send_all(int fd, const char *data, size_t len)
{
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(fd, data + sent, len - sent, MSG_NOSIGNAL);
        if (n <= 0) {
            return -1;
        }
        sent += (size_t)n;
    }
    return 0;
}

static ssize_t recv_response(int fd, char *buf, size_t cap)
{
    struct timeval tv = {.tv_sec = 2, .tv_usec = 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    size_t total = 0;
    while (total < cap) {
        ssize_t n = recv(fd, buf + total, cap - total, 0);
        if (n <= 0) {
            break;
        }
        total += (size_t)n;
        if (total > 4 && memmem(buf, total, "\r\n\r\n", 4) != nullptr) {
            break;
        }
    }
    return (ssize_t)total;
}

/* ---- Handler ---- */

static int dummy_handler(ioh_ctx_t *c)
{
    return ioh_ctx_text(c, 200, "OK");
}

/* ---- Test 1: Response contains a generated 32-hex-char request ID ---- */

void test_response_contains_generated_request_id(void)
{
    ioh_server_config_t cfg;
    ioh_server_config_init(&cfg);
    cfg.listen_port = 19400;
    cfg.max_connections = 16;
    cfg.queue_depth = 32;

    ioh_server_t *srv = ioh_server_create(&cfg);
    TEST_ASSERT_NOT_NULL(srv);

    ioh_router_t *router = ioh_router_create();
    TEST_ASSERT_NOT_NULL(router);
    TEST_ASSERT_EQUAL_INT(0, ioh_router_get(router, "/", dummy_handler));
    TEST_ASSERT_EQUAL_INT(0, ioh_server_set_router(srv, router));

    int listen_fd = ioh_server_listen(srv);
    TEST_ASSERT_GREATER_THAN(0, listen_fd);
    uint16_t port = get_bound_port(listen_fd);

    int client = connect_to(port);
    TEST_ASSERT_TRUE(client >= 0);

    const char *req = "GET / HTTP/1.1\r\n"
                      "Host: localhost\r\n"
                      "Connection: close\r\n"
                      "\r\n";
    TEST_ASSERT_EQUAL_INT(0, send_all(client, req, strlen(req)));

    for (int i = 0; i < 10; i++) {
        (void)ioh_server_run_once(srv, 100);
    }

    char resp[4096];
    ssize_t resp_len = recv_response(client, resp, sizeof(resp));
    TEST_ASSERT_GREATER_THAN(0, resp_len);
    TEST_ASSERT_NOT_NULL(memmem(resp, (size_t)resp_len, "200", 3));

    /* Find X-Request-Id header */
    const char *hdr = "X-Request-Id: ";
    char *pos = memmem(resp, (size_t)resp_len, hdr, strlen(hdr));
    TEST_ASSERT_NOT_NULL_MESSAGE(pos, "Expected X-Request-Id header in response");

    /* Extract the value (until \r\n) */
    const char *val_start = pos + strlen(hdr);
    const char *val_end = strstr(val_start, "\r\n");
    TEST_ASSERT_NOT_NULL(val_end);

    size_t val_len = (size_t)(val_end - val_start);
    TEST_ASSERT_EQUAL_UINT(32, val_len);

    /* Verify all characters are hex digits */
    for (size_t i = 0; i < val_len; i++) {
        TEST_ASSERT_TRUE_MESSAGE(isxdigit((unsigned char)val_start[i]),
                                 "Request ID should contain only hex digits");
    }

    close(client);
    ioh_server_destroy(srv);
    ioh_router_destroy(router);
}

/* ---- Test 2: Incoming X-Request-Id is propagated ---- */

void test_propagates_incoming_request_id(void)
{
    ioh_server_config_t cfg;
    ioh_server_config_init(&cfg);
    cfg.listen_port = 19401;
    cfg.max_connections = 16;
    cfg.queue_depth = 32;

    ioh_server_t *srv = ioh_server_create(&cfg);
    TEST_ASSERT_NOT_NULL(srv);

    ioh_router_t *router = ioh_router_create();
    TEST_ASSERT_NOT_NULL(router);
    TEST_ASSERT_EQUAL_INT(0, ioh_router_get(router, "/", dummy_handler));
    TEST_ASSERT_EQUAL_INT(0, ioh_server_set_router(srv, router));

    int listen_fd = ioh_server_listen(srv);
    TEST_ASSERT_GREATER_THAN(0, listen_fd);
    uint16_t port = get_bound_port(listen_fd);

    int client = connect_to(port);
    TEST_ASSERT_TRUE(client >= 0);

    const char *req = "GET / HTTP/1.1\r\n"
                      "Host: localhost\r\n"
                      "X-Request-Id: abc123\r\n"
                      "Connection: close\r\n"
                      "\r\n";
    TEST_ASSERT_EQUAL_INT(0, send_all(client, req, strlen(req)));

    for (int i = 0; i < 10; i++) {
        (void)ioh_server_run_once(srv, 100);
    }

    char resp[4096];
    ssize_t resp_len = recv_response(client, resp, sizeof(resp));
    TEST_ASSERT_GREATER_THAN(0, resp_len);
    TEST_ASSERT_NOT_NULL(memmem(resp, (size_t)resp_len, "200", 3));
    TEST_ASSERT_NOT_NULL_MESSAGE(memmem(resp, (size_t)resp_len, "X-Request-Id: abc123", 20),
                                 "Expected propagated X-Request-Id: abc123 in response");

    close(client);
    ioh_server_destroy(srv);
    ioh_router_destroy(router);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_response_contains_generated_request_id);
    RUN_TEST(test_propagates_incoming_request_id);
    return UNITY_END();
}

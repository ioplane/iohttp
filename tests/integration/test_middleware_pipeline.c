/**
 * @file test_middleware_pipeline.c
 * @brief Integration tests: middleware execution through full TCP pipeline.
 */

#include "core/ioh_ctx.h"
#include "core/ioh_server.h"
#include "http/ioh_request.h"
#include "middleware/ioh_middleware.h"
#include "router/ioh_router.h"

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
    getsockname(fd, (struct sockaddr *)&addr, &len);
    return ntohs(addr.sin_port);
}

static int connect_to(uint16_t port)
{
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return -errno;
    }
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(port),
        .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
    };
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

/* ---- Handlers ---- */

static int hello_handler(ioh_ctx_t *c)
{
    return ioh_ctx_text(c, 200, "Hello, World!");
}

/* ---- Middleware ---- */

static int custom_header_mw(ioh_ctx_t *c, ioh_handler_fn next)
{
    (void)ioh_ctx_set_header(c, "X-Custom", "applied");
    return next(c);
}

/* ---- Test helpers ---- */

static uint16_t next_port = 19080;

static ioh_server_t *make_server(void)
{
    ioh_server_config_t cfg;
    ioh_server_config_init(&cfg);
    cfg.listen_port = next_port++;
    cfg.max_connections = 16;
    cfg.queue_depth = 32;
    return ioh_server_create(&cfg);
}

/* ---- Test 1: Custom middleware adds X-Custom header ---- */

void test_middleware_custom_header(void)
{
    ioh_server_t *srv = make_server();
    TEST_ASSERT_NOT_NULL(srv);

    ioh_router_t *router = ioh_router_create();
    TEST_ASSERT_NOT_NULL(router);
    TEST_ASSERT_EQUAL_INT(0, ioh_router_use(router, custom_header_mw));
    TEST_ASSERT_EQUAL_INT(0, ioh_router_get(router, "/hello", hello_handler));
    TEST_ASSERT_EQUAL_INT(0, ioh_server_set_router(srv, router));

    int listen_fd = ioh_server_listen(srv);
    TEST_ASSERT_GREATER_THAN(0, listen_fd);
    uint16_t port = get_bound_port(listen_fd);

    int client = connect_to(port);
    TEST_ASSERT_TRUE(client >= 0);

    const char *req = "GET /hello HTTP/1.1\r\n"
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
    TEST_ASSERT_NOT_NULL(memmem(resp, (size_t)resp_len, "Hello, World!", 13));
    TEST_ASSERT_NOT_NULL_MESSAGE(memmem(resp, (size_t)resp_len, "X-Custom: applied", 17),
                                 "Expected X-Custom: applied header in response");

    close(client);
    ioh_server_destroy(srv);
    ioh_router_destroy(router);
}

/* ---- Test 2: Middleware short-circuit without calling next ---- */

static int short_circuit_mw(ioh_ctx_t *c, ioh_handler_fn next)
{
    (void)next;
    (void)ioh_ctx_set_header(c, "X-Short-Circuit", "yes");
    return ioh_ctx_text(c, 204, "");
}

void test_middleware_short_circuit(void)
{
    ioh_server_t *srv = make_server();
    TEST_ASSERT_NOT_NULL(srv);

    ioh_router_t *router = ioh_router_create();
    TEST_ASSERT_NOT_NULL(router);
    TEST_ASSERT_EQUAL_INT(0, ioh_router_use(router, short_circuit_mw));
    TEST_ASSERT_EQUAL_INT(0, ioh_router_get(router, "/hello", hello_handler));
    TEST_ASSERT_EQUAL_INT(0, ioh_server_set_router(srv, router));

    int listen_fd = ioh_server_listen(srv);
    TEST_ASSERT_GREATER_THAN(0, listen_fd);
    uint16_t port = get_bound_port(listen_fd);

    int client = connect_to(port);
    TEST_ASSERT_TRUE(client >= 0);

    const char *req = "GET /hello HTTP/1.1\r\n"
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
    TEST_ASSERT_NOT_NULL(memmem(resp, (size_t)resp_len, "204", 3));
    TEST_ASSERT_NOT_NULL_MESSAGE(memmem(resp, (size_t)resp_len, "X-Short-Circuit: yes", 20),
                                 "Expected X-Short-Circuit header from short-circuit middleware");
    /* Verify handler was NOT called (no "Hello, World!" in response) */
    TEST_ASSERT_NULL_MESSAGE(memmem(resp, (size_t)resp_len, "Hello, World!", 13),
                             "Handler should not have been called when middleware short-circuits");

    close(client);
    ioh_server_destroy(srv);
    ioh_router_destroy(router);
}

/* ---- Test 3: Multiple middleware execute in order ---- */

static int first_mw(ioh_ctx_t *c, ioh_handler_fn next)
{
    (void)ioh_ctx_set_header(c, "X-First", "1");
    return next(c);
}

static int second_mw(ioh_ctx_t *c, ioh_handler_fn next)
{
    (void)ioh_ctx_set_header(c, "X-Second", "2");
    return next(c);
}

void test_middleware_chain_order(void)
{
    ioh_server_t *srv = make_server();
    TEST_ASSERT_NOT_NULL(srv);

    ioh_router_t *router = ioh_router_create();
    TEST_ASSERT_NOT_NULL(router);
    TEST_ASSERT_EQUAL_INT(0, ioh_router_use(router, first_mw));
    TEST_ASSERT_EQUAL_INT(0, ioh_router_use(router, second_mw));
    TEST_ASSERT_EQUAL_INT(0, ioh_router_get(router, "/hello", hello_handler));
    TEST_ASSERT_EQUAL_INT(0, ioh_server_set_router(srv, router));

    int listen_fd = ioh_server_listen(srv);
    TEST_ASSERT_GREATER_THAN(0, listen_fd);
    uint16_t port = get_bound_port(listen_fd);

    int client = connect_to(port);
    TEST_ASSERT_TRUE(client >= 0);

    const char *req = "GET /hello HTTP/1.1\r\n"
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
    TEST_ASSERT_NOT_NULL_MESSAGE(memmem(resp, (size_t)resp_len, "X-First: 1", 10),
                                 "Expected X-First header from first middleware");
    TEST_ASSERT_NOT_NULL_MESSAGE(memmem(resp, (size_t)resp_len, "X-Second: 2", 11),
                                 "Expected X-Second header from second middleware");

    close(client);
    ioh_server_destroy(srv);
    ioh_router_destroy(router);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_middleware_custom_header);
    RUN_TEST(test_middleware_short_circuit);
    RUN_TEST(test_middleware_chain_order);
    return UNITY_END();
}

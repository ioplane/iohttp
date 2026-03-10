/**
 * @file test_pipeline.c
 * @brief End-to-end integration tests: real TCP connections through server pipeline.
 */

#include "core/ioh_ctx.h"
#include "core/ioh_server.h"
#include "http/ioh_request.h"
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

static int echo_handler(ioh_ctx_t *c)
{
    size_t body_len = 0;
    const uint8_t *body = ioh_ctx_body(c, &body_len);
    return ioh_ctx_blob(c, 200, "application/octet-stream", body, body_len);
}

static int callback_handler(ioh_ctx_t *c, void *user_data)
{
    (void)user_data;
    return ioh_ctx_text(c, 200, "callback");
}

/* ---- Test helpers ---- */

static uint16_t next_port = 18080;

static ioh_server_t *make_server(void)
{
    ioh_server_config_t cfg;
    ioh_server_config_init(&cfg);
    cfg.listen_port = next_port++;
    cfg.max_connections = 16;
    cfg.queue_depth = 32;
    return ioh_server_create(&cfg);
}

/* ---- Test 1: Simple GET ---- */

void test_pipeline_simple_get(void)
{
    ioh_server_t *srv = make_server();
    TEST_ASSERT_NOT_NULL(srv);

    ioh_router_t *router = ioh_router_create();
    TEST_ASSERT_NOT_NULL(router);
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

    for (int i = 0; i < 5; i++) {
        (void)ioh_server_run_once(srv, 100);
    }

    char resp[4096];
    ssize_t resp_len = recv_response(client, resp, sizeof(resp));
    TEST_ASSERT_GREATER_THAN(0, resp_len);
    TEST_ASSERT_NOT_NULL(memmem(resp, (size_t)resp_len, "200", 3));
    TEST_ASSERT_NOT_NULL(memmem(resp, (size_t)resp_len, "Hello, World!", 13));

    close(client);
    ioh_server_destroy(srv);
    ioh_router_destroy(router);
}

/* ---- Test 2: 404 Not Found ---- */

void test_pipeline_not_found(void)
{
    ioh_server_t *srv = make_server();
    TEST_ASSERT_NOT_NULL(srv);

    ioh_router_t *router = ioh_router_create();
    TEST_ASSERT_NOT_NULL(router);
    TEST_ASSERT_EQUAL_INT(0, ioh_router_get(router, "/hello", hello_handler));
    TEST_ASSERT_EQUAL_INT(0, ioh_server_set_router(srv, router));

    int listen_fd = ioh_server_listen(srv);
    uint16_t port = get_bound_port(listen_fd);

    int client = connect_to(port);
    TEST_ASSERT_TRUE(client >= 0);

    const char *req = "GET /nonexistent HTTP/1.1\r\n"
                      "Host: localhost\r\n"
                      "Connection: close\r\n"
                      "\r\n";
    (void)send_all(client, req, strlen(req));

    for (int i = 0; i < 5; i++) {
        (void)ioh_server_run_once(srv, 100);
    }

    char resp[4096];
    ssize_t resp_len = recv_response(client, resp, sizeof(resp));
    TEST_ASSERT_GREATER_THAN(0, resp_len);
    TEST_ASSERT_NOT_NULL(memmem(resp, (size_t)resp_len, "404", 3));

    close(client);
    ioh_server_destroy(srv);
    ioh_router_destroy(router);
}

/* ---- Test 3: POST with body ---- */

void test_pipeline_post_with_body(void)
{
    ioh_server_t *srv = make_server();
    TEST_ASSERT_NOT_NULL(srv);

    ioh_router_t *router = ioh_router_create();
    TEST_ASSERT_NOT_NULL(router);
    TEST_ASSERT_EQUAL_INT(0, ioh_router_post(router, "/echo", echo_handler));
    TEST_ASSERT_EQUAL_INT(0, ioh_server_set_router(srv, router));

    int listen_fd = ioh_server_listen(srv);
    uint16_t port = get_bound_port(listen_fd);

    int client = connect_to(port);
    TEST_ASSERT_TRUE(client >= 0);

    const char *req = "POST /echo HTTP/1.1\r\n"
                      "Host: localhost\r\n"
                      "Content-Length: 5\r\n"
                      "Connection: close\r\n"
                      "\r\n"
                      "hello";
    (void)send_all(client, req, strlen(req));

    for (int i = 0; i < 5; i++) {
        (void)ioh_server_run_once(srv, 100);
    }

    char resp[4096];
    ssize_t resp_len = recv_response(client, resp, sizeof(resp));
    TEST_ASSERT_GREATER_THAN(0, resp_len);
    TEST_ASSERT_NOT_NULL(memmem(resp, (size_t)resp_len, "200", 3));
    TEST_ASSERT_NOT_NULL(memmem(resp, (size_t)resp_len, "hello", 5));

    close(client);
    ioh_server_destroy(srv);
    ioh_router_destroy(router);
}

/* ---- Test 4: on_request callback ---- */

void test_pipeline_on_request_callback(void)
{
    ioh_server_t *srv = make_server();
    TEST_ASSERT_NOT_NULL(srv);
    TEST_ASSERT_EQUAL_INT(0, ioh_server_set_on_request(srv, callback_handler, nullptr));

    int listen_fd = ioh_server_listen(srv);
    uint16_t port = get_bound_port(listen_fd);

    int client = connect_to(port);
    TEST_ASSERT_TRUE(client >= 0);

    const char *req = "GET /anything HTTP/1.1\r\n"
                      "Host: localhost\r\n"
                      "Connection: close\r\n"
                      "\r\n";
    (void)send_all(client, req, strlen(req));

    for (int i = 0; i < 10; i++) {
        (void)ioh_server_run_once(srv, 100);
    }

    char resp[4096];
    ssize_t resp_len = recv_response(client, resp, sizeof(resp));
    TEST_ASSERT_GREATER_THAN(0, resp_len);
    TEST_ASSERT_NOT_NULL(memmem(resp, (size_t)resp_len, "200", 3));
    TEST_ASSERT_NOT_NULL(memmem(resp, (size_t)resp_len, "callback", 8));

    close(client);
    ioh_server_destroy(srv);
}

/* ---- Test 5: Bad request ---- */

void test_pipeline_bad_request(void)
{
    ioh_server_t *srv = make_server();
    TEST_ASSERT_NOT_NULL(srv);
    TEST_ASSERT_EQUAL_INT(0, ioh_server_set_on_request(srv, callback_handler, nullptr));

    int listen_fd = ioh_server_listen(srv);
    uint16_t port = get_bound_port(listen_fd);

    int client = connect_to(port);
    TEST_ASSERT_TRUE(client >= 0);

    const char *req = "INVALID GARBAGE\r\n\r\n";
    (void)send_all(client, req, strlen(req));

    for (int i = 0; i < 5; i++) {
        (void)ioh_server_run_once(srv, 100);
    }

    char resp[4096];
    ssize_t resp_len = recv_response(client, resp, sizeof(resp));
    TEST_ASSERT_GREATER_THAN(0, resp_len);
    TEST_ASSERT_NOT_NULL(memmem(resp, (size_t)resp_len, "400", 3));

    close(client);
    ioh_server_destroy(srv);
}

/* ---- Test 6: Client disconnect ---- */

void test_pipeline_client_disconnect(void)
{
    ioh_server_t *srv = make_server();
    TEST_ASSERT_NOT_NULL(srv);
    TEST_ASSERT_EQUAL_INT(0, ioh_server_set_on_request(srv, callback_handler, nullptr));

    int listen_fd = ioh_server_listen(srv);
    uint16_t port = get_bound_port(listen_fd);

    int client = connect_to(port);
    TEST_ASSERT_TRUE(client >= 0);

    for (int i = 0; i < 3; i++) {
        (void)ioh_server_run_once(srv, 100);
    }
    TEST_ASSERT_EQUAL_UINT32(1, ioh_conn_pool_active(ioh_server_pool(srv)));

    close(client);

    for (int i = 0; i < 5; i++) {
        (void)ioh_server_run_once(srv, 100);
    }

    TEST_ASSERT_EQUAL_UINT32(0, ioh_conn_pool_active(ioh_server_pool(srv)));

    ioh_server_destroy(srv);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_pipeline_simple_get);
    RUN_TEST(test_pipeline_not_found);
    RUN_TEST(test_pipeline_post_with_body);
    RUN_TEST(test_pipeline_on_request_callback);
    RUN_TEST(test_pipeline_bad_request);
    RUN_TEST(test_pipeline_client_disconnect);
    return UNITY_END();
}

/**
 * @file test_keepalive.c
 * @brief Integration tests for HTTP/1.1 keep-alive connection reuse.
 */

#include "core/io_server.h"
#include "core/io_conn.h"
#include "core/io_ctx.h"
#include "router/io_router.h"

#include <errno.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <unity.h>

void setUp(void) {}
void tearDown(void) {}

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

static int hello_handler(io_ctx_t *c)
{
	return io_ctx_text(c, 200, "OK");
}

/* ---- Test helpers ---- */

static uint16_t next_port = 19080;

static io_server_t *make_server(void)
{
	io_server_config_t cfg;
	io_server_config_init(&cfg);
	cfg.listen_port = next_port++;
	cfg.max_connections = 16;
	cfg.queue_depth = 32;
	return io_server_create(&cfg);
}

/* ---- Test 1: Two requests on same connection ---- */

void test_keepalive_two_requests(void)
{
	io_server_t *srv = make_server();
	TEST_ASSERT_NOT_NULL(srv);

	io_router_t *router = io_router_create();
	TEST_ASSERT_NOT_NULL(router);
	TEST_ASSERT_EQUAL_INT(0, io_router_get(router, "/hello", hello_handler));
	TEST_ASSERT_EQUAL_INT(0, io_server_set_router(srv, router));

	int listen_fd = io_server_listen(srv);
	TEST_ASSERT_GREATER_THAN(0, listen_fd);
	uint16_t port = get_bound_port(listen_fd);

	int client = connect_to(port);
	TEST_ASSERT_TRUE(client >= 0);

	/* First request: default keep-alive (HTTP/1.1 implicit) */
	const char *req1 = "GET /hello HTTP/1.1\r\n"
			   "Host: localhost\r\n"
			   "\r\n";
	TEST_ASSERT_EQUAL_INT(0, send_all(client, req1, strlen(req1)));

	for (int i = 0; i < 10; i++) {
		(void)io_server_run_once(srv, 100);
	}

	char resp[4096];
	ssize_t resp_len = recv_response(client, resp, sizeof(resp));
	TEST_ASSERT_GREATER_THAN(0, resp_len);
	TEST_ASSERT_NOT_NULL(memmem(resp, (size_t)resp_len, "200", 3));
	TEST_ASSERT_NOT_NULL(memmem(resp, (size_t)resp_len, "OK", 2));

	/* Second request on SAME connection with Connection: close */
	const char *req2 = "GET /hello HTTP/1.1\r\n"
			   "Host: localhost\r\n"
			   "Connection: close\r\n"
			   "\r\n";
	TEST_ASSERT_EQUAL_INT(0, send_all(client, req2, strlen(req2)));

	for (int i = 0; i < 10; i++) {
		(void)io_server_run_once(srv, 100);
	}

	resp_len = recv_response(client, resp, sizeof(resp));
	TEST_ASSERT_GREATER_THAN(0, resp_len);
	TEST_ASSERT_NOT_NULL(memmem(resp, (size_t)resp_len, "200", 3));
	TEST_ASSERT_NOT_NULL(memmem(resp, (size_t)resp_len, "OK", 2));

	close(client);
	io_server_destroy(srv);
	io_router_destroy(router);
}

/* ---- Test 2: Connection: close closes after first request ---- */

void test_keepalive_connection_close(void)
{
	io_server_t *srv = make_server();
	TEST_ASSERT_NOT_NULL(srv);

	io_router_t *router = io_router_create();
	TEST_ASSERT_NOT_NULL(router);
	TEST_ASSERT_EQUAL_INT(0, io_router_get(router, "/hello", hello_handler));
	TEST_ASSERT_EQUAL_INT(0, io_server_set_router(srv, router));

	int listen_fd = io_server_listen(srv);
	TEST_ASSERT_GREATER_THAN(0, listen_fd);
	uint16_t port = get_bound_port(listen_fd);

	int client = connect_to(port);
	TEST_ASSERT_TRUE(client >= 0);

	/* Send request with Connection: close */
	const char *req = "GET /hello HTTP/1.1\r\n"
			  "Host: localhost\r\n"
			  "Connection: close\r\n"
			  "\r\n";
	TEST_ASSERT_EQUAL_INT(0, send_all(client, req, strlen(req)));

	for (int i = 0; i < 10; i++) {
		(void)io_server_run_once(srv, 100);
	}

	char resp[4096];
	ssize_t resp_len = recv_response(client, resp, sizeof(resp));
	TEST_ASSERT_GREATER_THAN(0, resp_len);
	TEST_ASSERT_NOT_NULL(memmem(resp, (size_t)resp_len, "200", 3));

	/* Run more iterations to process close */
	for (int i = 0; i < 5; i++) {
		(void)io_server_run_once(srv, 100);
	}

	/* Verify pool has 0 active connections */
	TEST_ASSERT_EQUAL_UINT32(
		0, io_conn_pool_active(io_server_pool(srv)));

	close(client);
	io_server_destroy(srv);
	io_router_destroy(router);
}

int main(void)
{
	UNITY_BEGIN();
	RUN_TEST(test_keepalive_two_requests);
	RUN_TEST(test_keepalive_connection_close);
	return UNITY_END();
}

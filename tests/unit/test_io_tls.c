/**
 * @file test_io_tls.c
 * @brief Unit tests for wolfSSL TLS context management.
 */

#include "tls/io_tls.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <wolfssl/options.h>
#include <wolfssl/ssl.h>

#include <wolfssl/wolfio.h>

#include <unity.h>

/* Path to test certificates (relative to build directory) */
static const char *TEST_CA_CERT;
static const char *TEST_SERVER_CERT;
static const char *TEST_SERVER_KEY;
static const char *TEST_CLIENT_CERT;
static const char *TEST_CLIENT_KEY;

static void init_cert_paths(void)
{
	/* Paths relative to project root — set via SOURCE_DIR at compile time */
#ifdef TEST_CERTS_DIR
	static char ca_cert[512];
	static char server_cert[512];
	static char server_key[512];
	static char client_cert[512];
	static char client_key[512];

	snprintf(ca_cert, sizeof(ca_cert), "%s/ca-cert.pem", TEST_CERTS_DIR);
	snprintf(server_cert, sizeof(server_cert), "%s/server-cert.pem", TEST_CERTS_DIR);
	snprintf(server_key, sizeof(server_key), "%s/server-key.pem", TEST_CERTS_DIR);
	snprintf(client_cert, sizeof(client_cert), "%s/client-cert.pem", TEST_CERTS_DIR);
	snprintf(client_key, sizeof(client_key), "%s/client-key.pem", TEST_CERTS_DIR);

	TEST_CA_CERT = ca_cert;
	TEST_SERVER_CERT = server_cert;
	TEST_SERVER_KEY = server_key;
	TEST_CLIENT_CERT = client_cert;
	TEST_CLIENT_KEY = client_key;
#else
	TEST_CA_CERT = "tests/certs/ca-cert.pem";
	TEST_SERVER_CERT = "tests/certs/server-cert.pem";
	TEST_SERVER_KEY = "tests/certs/server-key.pem";
	TEST_CLIENT_CERT = "tests/certs/client-cert.pem";
	TEST_CLIENT_KEY = "tests/certs/client-key.pem";
#endif
}

void setUp(void)
{
}

void tearDown(void)
{
}

/* ---- Config tests ---- */

void test_tls_config_init_defaults(void)
{
	io_tls_config_t cfg;
	io_tls_config_init(&cfg);

	TEST_ASSERT_NULL(cfg.cert_file);
	TEST_ASSERT_NULL(cfg.key_file);
	TEST_ASSERT_NULL(cfg.ca_file);
	TEST_ASSERT_FALSE(cfg.require_client_cert);
	TEST_ASSERT_FALSE(cfg.enable_session_tickets);
	TEST_ASSERT_EQUAL_UINT32(256, cfg.session_cache_size);
	TEST_ASSERT_NULL(cfg.alpn);

	/* nullptr is safe */
	io_tls_config_init(nullptr);
}

void test_tls_config_validate_no_cert(void)
{
	io_tls_config_t cfg;
	io_tls_config_init(&cfg);
	cfg.key_file = TEST_SERVER_KEY;

	TEST_ASSERT_EQUAL_INT(-EINVAL, io_tls_config_validate(&cfg));
}

void test_tls_config_validate_no_key(void)
{
	io_tls_config_t cfg;
	io_tls_config_init(&cfg);
	cfg.cert_file = TEST_SERVER_CERT;

	TEST_ASSERT_EQUAL_INT(-EINVAL, io_tls_config_validate(&cfg));
}

/* ---- Context lifecycle tests ---- */

void test_tls_ctx_create_destroy(void)
{
	io_tls_config_t cfg;
	io_tls_config_init(&cfg);
	cfg.cert_file = TEST_SERVER_CERT;
	cfg.key_file = TEST_SERVER_KEY;

	io_tls_ctx_t *ctx = io_tls_ctx_create(&cfg);
	TEST_ASSERT_NOT_NULL(ctx);

	io_tls_ctx_destroy(ctx);

	/* nullptr is safe */
	io_tls_ctx_destroy(nullptr);
}

void test_tls_ctx_create_invalid_cert(void)
{
	io_tls_config_t cfg;
	io_tls_config_init(&cfg);
	cfg.cert_file = "/nonexistent/cert.pem";
	cfg.key_file = "/nonexistent/key.pem";

	io_tls_ctx_t *ctx = io_tls_ctx_create(&cfg);
	TEST_ASSERT_NULL(ctx);
}

/* ---- Per-connection lifecycle tests ---- */

void test_tls_conn_create_destroy(void)
{
	io_tls_config_t cfg;
	io_tls_config_init(&cfg);
	cfg.cert_file = TEST_SERVER_CERT;
	cfg.key_file = TEST_SERVER_KEY;

	io_tls_ctx_t *ctx = io_tls_ctx_create(&cfg);
	TEST_ASSERT_NOT_NULL(ctx);

	io_tls_conn_t *conn = io_tls_conn_create(ctx, -1);
	TEST_ASSERT_NOT_NULL(conn);
	TEST_ASSERT_NOT_NULL(conn->ssl);
	TEST_ASSERT_NOT_NULL(conn->cipher_in_buf);
	TEST_ASSERT_NOT_NULL(conn->cipher_out_buf);
	TEST_ASSERT_EQUAL_size_t(IO_TLS_CIPHER_BUF_SIZE, conn->cipher_in_cap);
	TEST_ASSERT_EQUAL_size_t(IO_TLS_CIPHER_BUF_SIZE, conn->cipher_out_cap);
	TEST_ASSERT_FALSE(conn->handshake_done);

	io_tls_conn_destroy(conn);
	io_tls_ctx_destroy(ctx);

	/* nullptr is safe */
	io_tls_conn_destroy(nullptr);

	/* nullptr ctx returns nullptr */
	TEST_ASSERT_NULL(io_tls_conn_create(nullptr, -1));
}

/* ---- Feed input buffer test ---- */

void test_tls_feed_input_basic(void)
{
	io_tls_config_t cfg;
	io_tls_config_init(&cfg);
	cfg.cert_file = TEST_SERVER_CERT;
	cfg.key_file = TEST_SERVER_KEY;

	io_tls_ctx_t *ctx = io_tls_ctx_create(&cfg);
	TEST_ASSERT_NOT_NULL(ctx);

	io_tls_conn_t *conn = io_tls_conn_create(ctx, -1);
	TEST_ASSERT_NOT_NULL(conn);

	/* Feed some data */
	uint8_t data[] = {0x16, 0x03, 0x01, 0x00, 0x05, 'h', 'e', 'l', 'l', 'o'};
	int ret = io_tls_feed_input(conn, data, sizeof(data));
	TEST_ASSERT_EQUAL_INT(0, ret);
	TEST_ASSERT_EQUAL_size_t(sizeof(data), conn->cipher_in_len);
	TEST_ASSERT_EQUAL_size_t(0, conn->cipher_in_pos);

	/* Feed more data */
	uint8_t more[] = {0x01, 0x02, 0x03};
	ret = io_tls_feed_input(conn, more, sizeof(more));
	TEST_ASSERT_EQUAL_INT(0, ret);
	TEST_ASSERT_EQUAL_size_t(sizeof(data) + sizeof(more), conn->cipher_in_len);

	/* Null args */
	TEST_ASSERT_EQUAL_INT(-EINVAL, io_tls_feed_input(nullptr, data, sizeof(data)));
	TEST_ASSERT_EQUAL_INT(-EINVAL, io_tls_feed_input(conn, nullptr, sizeof(data)));

	/* Zero length is ok */
	TEST_ASSERT_EQUAL_INT(0, io_tls_feed_input(conn, data, 0));

	io_tls_conn_destroy(conn);
	io_tls_ctx_destroy(ctx);
}

/* ---- Helper: set fd non-blocking ---- */

static void set_nonblock(int fd)
{
	int flags = fcntl(fd, F_GETFL, 0);
	fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

/* ---- Handshake test using socketpair + wolfSSL_set_fd ---- */

void test_tls_handshake_self_signed(void)
{
	int sv[2];
	TEST_ASSERT_EQUAL_INT(0, socketpair(AF_UNIX, SOCK_STREAM, 0, sv));

	/* Server side: use our io_tls_ctx */
	io_tls_config_t scfg;
	io_tls_config_init(&scfg);
	scfg.cert_file = TEST_SERVER_CERT;
	scfg.key_file = TEST_SERVER_KEY;

	io_tls_ctx_t *sctx = io_tls_ctx_create(&scfg);
	TEST_ASSERT_NOT_NULL(sctx);

	io_tls_conn_t *sconn = io_tls_conn_create(sctx, sv[0]);
	TEST_ASSERT_NOT_NULL(sconn);

	/* Use non-blocking sockets + reset to default socket I/O callbacks
	 * since CTX-level callbacks are our custom buffer-based ones. */
	set_nonblock(sv[0]);
	set_nonblock(sv[1]);
	wolfSSL_set_fd(sconn->ssl, sv[0]);
	wolfSSL_SSLSetIORecv(sconn->ssl, EmbedReceive);
	wolfSSL_SSLSetIOSend(sconn->ssl, EmbedSend);
	wolfSSL_set_using_nonblock(sconn->ssl, 1);

	/* Client side: create a separate wolfSSL client context */
	WOLFSSL_CTX *cctx = wolfSSL_CTX_new(wolfTLSv1_3_client_method());
	TEST_ASSERT_NOT_NULL(cctx);

	int rc = wolfSSL_CTX_load_verify_locations(cctx, TEST_CA_CERT, nullptr);
	TEST_ASSERT_EQUAL_INT(WOLFSSL_SUCCESS, rc);

	WOLFSSL *cssl = wolfSSL_new(cctx);
	TEST_ASSERT_NOT_NULL(cssl);
	wolfSSL_set_fd(cssl, sv[1]);
	wolfSSL_set_using_nonblock(cssl, 1);

	/* Non-blocking handshake: interleave accept/connect */
	int sret = WOLFSSL_FAILURE;
	int cret = WOLFSSL_FAILURE;
	for (int i = 0; i < 100; i++) {
		if (sret != WOLFSSL_SUCCESS) {
			sret = wolfSSL_accept(sconn->ssl);
			if (sret != WOLFSSL_SUCCESS) {
				int serr = wolfSSL_get_error(sconn->ssl, sret);
				if (serr != WOLFSSL_ERROR_WANT_READ &&
				    serr != WOLFSSL_ERROR_WANT_WRITE) {
					TEST_FAIL_MESSAGE("wolfSSL_accept failed");
				}
			}
		}
		if (cret != WOLFSSL_SUCCESS) {
			cret = wolfSSL_connect(cssl);
			if (cret != WOLFSSL_SUCCESS) {
				int cerr = wolfSSL_get_error(cssl, cret);
				if (cerr != WOLFSSL_ERROR_WANT_READ &&
				    cerr != WOLFSSL_ERROR_WANT_WRITE) {
					TEST_FAIL_MESSAGE("wolfSSL_connect failed");
				}
			}
		}
		if (sret == WOLFSSL_SUCCESS && cret == WOLFSSL_SUCCESS) {
			break;
		}
	}

	TEST_ASSERT_EQUAL_INT(WOLFSSL_SUCCESS, sret);
	TEST_ASSERT_EQUAL_INT(WOLFSSL_SUCCESS, cret);

	wolfSSL_free(cssl);
	wolfSSL_CTX_free(cctx);
	io_tls_conn_destroy(sconn);
	io_tls_ctx_destroy(sctx);
	close(sv[0]);
	close(sv[1]);
}

/* ---- Read/write roundtrip ---- */

void test_tls_read_write_roundtrip(void)
{
	int sv[2];
	TEST_ASSERT_EQUAL_INT(0, socketpair(AF_UNIX, SOCK_STREAM, 0, sv));

	/* Server */
	io_tls_config_t scfg;
	io_tls_config_init(&scfg);
	scfg.cert_file = TEST_SERVER_CERT;
	scfg.key_file = TEST_SERVER_KEY;

	io_tls_ctx_t *sctx = io_tls_ctx_create(&scfg);
	TEST_ASSERT_NOT_NULL(sctx);

	io_tls_conn_t *sconn = io_tls_conn_create(sctx, sv[0]);
	TEST_ASSERT_NOT_NULL(sconn);
	set_nonblock(sv[0]);
	set_nonblock(sv[1]);
	wolfSSL_set_fd(sconn->ssl, sv[0]);
	wolfSSL_SSLSetIORecv(sconn->ssl, EmbedReceive);
	wolfSSL_SSLSetIOSend(sconn->ssl, EmbedSend);
	wolfSSL_set_using_nonblock(sconn->ssl, 1);

	/* Client */
	WOLFSSL_CTX *cctx = wolfSSL_CTX_new(wolfTLSv1_3_client_method());
	TEST_ASSERT_NOT_NULL(cctx);
	wolfSSL_CTX_load_verify_locations(cctx, TEST_CA_CERT, nullptr);

	WOLFSSL *cssl = wolfSSL_new(cctx);
	TEST_ASSERT_NOT_NULL(cssl);
	wolfSSL_set_fd(cssl, sv[1]);
	wolfSSL_set_using_nonblock(cssl, 1);

	/* Non-blocking handshake — interleave accept/connect */
	int sret = WOLFSSL_FAILURE;
	int cret = WOLFSSL_FAILURE;
	for (int i = 0; i < 100; i++) {
		if (sret != WOLFSSL_SUCCESS) {
			sret = wolfSSL_accept(sconn->ssl);
		}
		if (cret != WOLFSSL_SUCCESS) {
			cret = wolfSSL_connect(cssl);
		}
		if (sret == WOLFSSL_SUCCESS && cret == WOLFSSL_SUCCESS) {
			break;
		}
	}
	TEST_ASSERT_EQUAL_INT(WOLFSSL_SUCCESS, sret);
	TEST_ASSERT_EQUAL_INT(WOLFSSL_SUCCESS, cret);

	/* Client writes, server reads */
	const char *msg = "Hello from client";
	int wret = wolfSSL_write(cssl, msg, (int)strlen(msg));
	TEST_ASSERT_EQUAL_INT((int)strlen(msg), wret);

	char buf[256];
	int rret = wolfSSL_read(sconn->ssl, buf, (int)sizeof(buf));
	TEST_ASSERT_EQUAL_INT((int)strlen(msg), rret);
	TEST_ASSERT_EQUAL_STRING_LEN(msg, buf, (size_t)rret);

	/* Server writes, client reads */
	const char *reply = "Hello from server";
	wret = wolfSSL_write(sconn->ssl, reply, (int)strlen(reply));
	TEST_ASSERT_EQUAL_INT((int)strlen(reply), wret);

	rret = wolfSSL_read(cssl, buf, (int)sizeof(buf));
	TEST_ASSERT_EQUAL_INT((int)strlen(reply), rret);
	TEST_ASSERT_EQUAL_STRING_LEN(reply, buf, (size_t)rret);

	wolfSSL_free(cssl);
	wolfSSL_CTX_free(cctx);
	io_tls_conn_destroy(sconn);
	io_tls_ctx_destroy(sctx);
	close(sv[0]);
	close(sv[1]);
}

/* ---- ALPN tests ---- */

static void do_alpn_test(const char *server_alpn, const char *client_alpn,
			 const char *expected_proto)
{
	int sv[2];
	TEST_ASSERT_EQUAL_INT(0, socketpair(AF_UNIX, SOCK_STREAM, 0, sv));

	/* Server with ALPN */
	io_tls_config_t scfg;
	io_tls_config_init(&scfg);
	scfg.cert_file = TEST_SERVER_CERT;
	scfg.key_file = TEST_SERVER_KEY;
	scfg.alpn = server_alpn;

	io_tls_ctx_t *sctx = io_tls_ctx_create(&scfg);
	TEST_ASSERT_NOT_NULL(sctx);

	io_tls_conn_t *sconn = io_tls_conn_create(sctx, sv[0]);
	TEST_ASSERT_NOT_NULL(sconn);
	set_nonblock(sv[0]);
	set_nonblock(sv[1]);
	wolfSSL_set_fd(sconn->ssl, sv[0]);
	wolfSSL_SSLSetIORecv(sconn->ssl, EmbedReceive);
	wolfSSL_SSLSetIOSend(sconn->ssl, EmbedSend);
	wolfSSL_set_using_nonblock(sconn->ssl, 1);

	/* Client with ALPN */
	WOLFSSL_CTX *cctx = wolfSSL_CTX_new(wolfTLSv1_3_client_method());
	TEST_ASSERT_NOT_NULL(cctx);
	wolfSSL_CTX_load_verify_locations(cctx, TEST_CA_CERT, nullptr);

	WOLFSSL *cssl = wolfSSL_new(cctx);
	TEST_ASSERT_NOT_NULL(cssl);
	wolfSSL_set_fd(cssl, sv[1]);
	wolfSSL_set_using_nonblock(cssl, 1);

	/* Set client ALPN — wolfSSL takes comma-separated protocol names */
	int rc = wolfSSL_UseALPN(cssl, (char *)client_alpn,
				 (unsigned int)strlen(client_alpn),
				 WOLFSSL_ALPN_FAILED_ON_MISMATCH);
	TEST_ASSERT_EQUAL_INT(WOLFSSL_SUCCESS, rc);

	/* Handshake */
	int sret = WOLFSSL_FAILURE;
	int cret = WOLFSSL_FAILURE;
	for (int i = 0; i < 100; i++) {
		if (sret != WOLFSSL_SUCCESS) {
			sret = wolfSSL_accept(sconn->ssl);
		}
		if (cret != WOLFSSL_SUCCESS) {
			cret = wolfSSL_connect(cssl);
		}
		if (sret == WOLFSSL_SUCCESS && cret == WOLFSSL_SUCCESS) {
			break;
		}
	}
	TEST_ASSERT_EQUAL_INT(WOLFSSL_SUCCESS, sret);
	TEST_ASSERT_EQUAL_INT(WOLFSSL_SUCCESS, cret);

	/* Check negotiated ALPN */
	const char *proto = io_tls_get_alpn(sconn);
	TEST_ASSERT_NOT_NULL(proto);
	TEST_ASSERT_EQUAL_STRING(expected_proto, proto);

	wolfSSL_free(cssl);
	wolfSSL_CTX_free(cctx);
	io_tls_conn_destroy(sconn);
	io_tls_ctx_destroy(sctx);
	close(sv[0]);
	close(sv[1]);
}

void test_tls_alpn_h2(void)
{
	do_alpn_test("h2,http/1.1", "h2", "h2");
}

void test_tls_alpn_http11(void)
{
	do_alpn_test("h2,http/1.1", "http/1.1", "http/1.1");
}

/* ---- Test runner ---- */

int main(void)
{
	init_cert_paths();

	UNITY_BEGIN();

	RUN_TEST(test_tls_config_init_defaults);
	RUN_TEST(test_tls_config_validate_no_cert);
	RUN_TEST(test_tls_config_validate_no_key);
	RUN_TEST(test_tls_ctx_create_destroy);
	RUN_TEST(test_tls_ctx_create_invalid_cert);
	RUN_TEST(test_tls_conn_create_destroy);
	RUN_TEST(test_tls_handshake_self_signed);
	RUN_TEST(test_tls_read_write_roundtrip);
	RUN_TEST(test_tls_alpn_h2);
	RUN_TEST(test_tls_alpn_http11);
	RUN_TEST(test_tls_feed_input_basic);

	return UNITY_END();
}

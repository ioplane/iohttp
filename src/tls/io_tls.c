/**
 * @file io_tls.c
 * @brief wolfSSL TLS context management with buffer-based I/O.
 *
 * Custom I/O callbacks read/write from internal cipher buffers instead of
 * sockets, enabling non-blocking TLS over io_uring.
 */

#include "tls/io_tls.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <wolfssl/options.h>
#include <wolfssl/ssl.h>

/* ---- Internal context structure ---- */

struct io_tls_ctx {
	WOLFSSL_CTX *wolf_ctx;
	io_tls_config_t config;
	char *alpn_copy;       /**< comma-separated ALPN string, owned by ctx */
	uint32_t alpn_copy_len;
};

/* ---- wolfSSL library init (thread-safe via atomic) ---- */

static _Atomic bool wolfssl_initialized = false;

static void ensure_wolfssl_init(void)
{
	bool expected = false;
	if (atomic_compare_exchange_strong(&wolfssl_initialized, &expected, true)) {
		wolfSSL_Init();
	}
}

/* ---- Custom I/O callbacks ---- */

/**
 * Recv callback: wolfSSL calls this to read ciphertext.
 * Reads from conn->cipher_in_buf.
 */
static int tls_io_recv_cb(WOLFSSL *ssl, char *buf, int sz, void *ctx)
{
	(void)ssl;
	io_tls_conn_t *conn = ctx;

	size_t avail = conn->cipher_in_len - conn->cipher_in_pos;
	if (avail == 0) {
		return WOLFSSL_CBIO_ERR_WANT_READ;
	}

	size_t to_copy = (size_t)sz < avail ? (size_t)sz : avail;
	memcpy(buf, conn->cipher_in_buf + conn->cipher_in_pos, to_copy);
	conn->cipher_in_pos += to_copy;

	/* Compact buffer when fully consumed */
	if (conn->cipher_in_pos == conn->cipher_in_len) {
		conn->cipher_in_pos = 0;
		conn->cipher_in_len = 0;
	}

	return (int)to_copy;
}

/**
 * Send callback: wolfSSL calls this to write ciphertext.
 * Writes to conn->cipher_out_buf.
 */
static int tls_io_send_cb(WOLFSSL *ssl, char *buf, int sz, void *ctx)
{
	(void)ssl;
	io_tls_conn_t *conn = ctx;

	size_t space = conn->cipher_out_cap - conn->cipher_out_len;
	if (space == 0) {
		return WOLFSSL_CBIO_ERR_WANT_WRITE;
	}

	size_t to_copy = (size_t)sz < space ? (size_t)sz : space;
	memcpy(conn->cipher_out_buf + conn->cipher_out_len, buf, to_copy);
	conn->cipher_out_len += to_copy;

	return (int)to_copy;
}

/* ---- Config ---- */

void io_tls_config_init(io_tls_config_t *cfg)
{
	if (cfg == nullptr) {
		return;
	}

	*cfg = (io_tls_config_t){
		.cert_file = nullptr,
		.key_file = nullptr,
		.ca_file = nullptr,
		.require_client_cert = false,
		.enable_session_tickets = false,
		.session_cache_size = 256,
		.alpn = nullptr,
	};
}

int io_tls_config_validate(const io_tls_config_t *cfg)
{
	if (cfg == nullptr) {
		return -EINVAL;
	}
	if (cfg->cert_file == nullptr) {
		return -EINVAL;
	}
	if (cfg->key_file == nullptr) {
		return -EINVAL;
	}
	return 0;
}

/* ---- Context ---- */

io_tls_ctx_t *io_tls_ctx_create(const io_tls_config_t *cfg)
{
	if (io_tls_config_validate(cfg) != 0) {
		return nullptr;
	}

	ensure_wolfssl_init();

	io_tls_ctx_t *ctx = calloc(1, sizeof(*ctx));
	if (ctx == nullptr) {
		return nullptr;
	}

	ctx->config = *cfg;

	/* TLS 1.2 minimum, TLS 1.3 primary */
	ctx->wolf_ctx = wolfSSL_CTX_new(wolfSSLv23_server_method());
	if (ctx->wolf_ctx == nullptr) {
		goto fail;
	}

	/* Set minimum version to TLS 1.2 */
	if (wolfSSL_CTX_SetMinVersion(ctx->wolf_ctx, WOLFSSL_TLSV1_2) !=
	    WOLFSSL_SUCCESS) {
		goto fail_ctx;
	}

	/* Load server certificate */
	if (wolfSSL_CTX_use_certificate_file(ctx->wolf_ctx, cfg->cert_file,
					     WOLFSSL_FILETYPE_PEM) !=
	    WOLFSSL_SUCCESS) {
		goto fail_ctx;
	}

	/* Load server private key */
	if (wolfSSL_CTX_use_PrivateKey_file(ctx->wolf_ctx, cfg->key_file,
					    WOLFSSL_FILETYPE_PEM) !=
	    WOLFSSL_SUCCESS) {
		goto fail_ctx;
	}

	/* mTLS: load CA and require client cert */
	if (cfg->ca_file != nullptr) {
		if (wolfSSL_CTX_load_verify_locations(ctx->wolf_ctx,
						      cfg->ca_file,
						      nullptr) !=
		    WOLFSSL_SUCCESS) {
			goto fail_ctx;
		}
	}

	if (cfg->require_client_cert) {
		wolfSSL_CTX_set_verify(ctx->wolf_ctx,
				       WOLFSSL_VERIFY_PEER |
					       WOLFSSL_VERIFY_FAIL_IF_NO_PEER_CERT,
				       nullptr);
	}

	/* Session cache */
	if (cfg->session_cache_size == 0) {
		wolfSSL_CTX_set_session_cache_mode(ctx->wolf_ctx,
						   WOLFSSL_SESS_CACHE_OFF);
	}

	/* Session tickets */
	if (!cfg->enable_session_tickets) {
		wolfSSL_CTX_set_options(ctx->wolf_ctx,
					WOLFSSL_OP_NO_TICKET);
	}

	/* ALPN: store copy of comma-separated string; applied per-SSL in conn_create */
	if (cfg->alpn != nullptr) {
		size_t alen = strlen(cfg->alpn);
		ctx->alpn_copy = malloc(alen + 1);
		if (ctx->alpn_copy == nullptr) {
			goto fail_ctx;
		}
		memcpy(ctx->alpn_copy, cfg->alpn, alen + 1);
		ctx->alpn_copy_len = (uint32_t)alen;
	}

	/* Set custom I/O callbacks */
	wolfSSL_CTX_SetIORecv(ctx->wolf_ctx, tls_io_recv_cb);
	wolfSSL_CTX_SetIOSend(ctx->wolf_ctx, tls_io_send_cb);

	/* Enable partial writes */
	wolfSSL_CTX_set_mode(ctx->wolf_ctx, WOLFSSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);

	return ctx;

fail_ctx:
	wolfSSL_CTX_free(ctx->wolf_ctx);
fail:
	free(ctx->alpn_copy);
	free(ctx);
	return nullptr;
}

void io_tls_ctx_destroy(io_tls_ctx_t *ctx)
{
	if (ctx == nullptr) {
		return;
	}

	wolfSSL_CTX_free(ctx->wolf_ctx);
	free(ctx->alpn_copy);
	free(ctx);
}

/* ---- Per-connection ---- */

io_tls_conn_t *io_tls_conn_create(io_tls_ctx_t *ctx, int fd)
{
	if (ctx == nullptr) {
		return nullptr;
	}

	io_tls_conn_t *conn = calloc(1, sizeof(*conn));
	if (conn == nullptr) {
		return nullptr;
	}

	conn->cipher_in_buf = malloc(IO_TLS_CIPHER_BUF_SIZE);
	if (conn->cipher_in_buf == nullptr) {
		goto fail;
	}
	conn->cipher_in_cap = IO_TLS_CIPHER_BUF_SIZE;

	conn->cipher_out_buf = malloc(IO_TLS_CIPHER_BUF_SIZE);
	if (conn->cipher_out_buf == nullptr) {
		goto fail;
	}
	conn->cipher_out_cap = IO_TLS_CIPHER_BUF_SIZE;

	conn->ssl = wolfSSL_new(ctx->wolf_ctx);
	if (conn->ssl == nullptr) {
		goto fail;
	}

	/* Store conn pointer for custom I/O callbacks */
	wolfSSL_SetIOReadCtx(conn->ssl, conn);
	wolfSSL_SetIOWriteCtx(conn->ssl, conn);

	/* Apply ALPN if configured (must be per-SSL, not per-CTX).
	 * wolfSSL_UseALPN takes comma-separated protocol names. */
	if (ctx->alpn_copy != nullptr && ctx->alpn_copy_len > 0) {
		if (wolfSSL_UseALPN(conn->ssl, ctx->alpn_copy,
				    ctx->alpn_copy_len,
				    WOLFSSL_ALPN_FAILED_ON_MISMATCH) !=
		    WOLFSSL_SUCCESS) {
			wolfSSL_free(conn->ssl);
			conn->ssl = nullptr;
			goto fail;
		}
	}

	(void)fd;

	return conn;

fail:
	free(conn->cipher_out_buf);
	free(conn->cipher_in_buf);
	free(conn);
	return nullptr;
}

void io_tls_conn_destroy(io_tls_conn_t *conn)
{
	if (conn == nullptr) {
		return;
	}

	if (conn->ssl != nullptr) {
		wolfSSL_free(conn->ssl);
	}
	free(conn->cipher_in_buf);
	free(conn->cipher_out_buf);
	free(conn);
}

/* ---- Buffer I/O ---- */

int io_tls_feed_input(io_tls_conn_t *conn, const uint8_t *data, size_t len)
{
	if (conn == nullptr || data == nullptr) {
		return -EINVAL;
	}

	if (len == 0) {
		return 0;
	}

	/* Compact buffer if we have consumed data at the front */
	if (conn->cipher_in_pos > 0 && conn->cipher_in_len > conn->cipher_in_pos) {
		size_t remaining = conn->cipher_in_len - conn->cipher_in_pos;
		memmove(conn->cipher_in_buf, conn->cipher_in_buf + conn->cipher_in_pos,
			remaining);
		conn->cipher_in_len = remaining;
		conn->cipher_in_pos = 0;
	} else if (conn->cipher_in_pos > 0) {
		conn->cipher_in_len = 0;
		conn->cipher_in_pos = 0;
	}

	size_t space = conn->cipher_in_cap - conn->cipher_in_len;
	if (len > space) {
		return -ENOMEM;
	}

	memcpy(conn->cipher_in_buf + conn->cipher_in_len, data, len);
	conn->cipher_in_len += len;

	return 0;
}

int io_tls_handshake(io_tls_conn_t *conn)
{
	if (conn == nullptr) {
		return -EINVAL;
	}

	if (conn->handshake_done) {
		return 0;
	}

	int ret = wolfSSL_accept(conn->ssl);
	if (ret == WOLFSSL_SUCCESS) {
		conn->handshake_done = true;
		return 0;
	}

	int err = wolfSSL_get_error(conn->ssl, ret);
	if (err == WOLFSSL_ERROR_WANT_READ || err == WOLFSSL_ERROR_WANT_WRITE) {
		return -EAGAIN;
	}

	return -EIO;
}

int io_tls_read(io_tls_conn_t *conn, uint8_t *buf, size_t len)
{
	if (conn == nullptr || buf == nullptr) {
		return -EINVAL;
	}

	int ret = wolfSSL_read(conn->ssl, buf, (int)len);
	if (ret > 0) {
		return ret;
	}

	int err = wolfSSL_get_error(conn->ssl, ret);
	if (err == WOLFSSL_ERROR_WANT_READ || err == WOLFSSL_ERROR_WANT_WRITE) {
		return -EAGAIN;
	}

	return -EIO;
}

int io_tls_write(io_tls_conn_t *conn, const uint8_t *buf, size_t len)
{
	if (conn == nullptr || buf == nullptr) {
		return -EINVAL;
	}

	int ret = wolfSSL_write(conn->ssl, buf, (int)len);
	if (ret > 0) {
		return ret;
	}

	int err = wolfSSL_get_error(conn->ssl, ret);
	if (err == WOLFSSL_ERROR_WANT_READ || err == WOLFSSL_ERROR_WANT_WRITE) {
		return -EAGAIN;
	}

	return -EIO;
}

int io_tls_get_output(io_tls_conn_t *conn, const uint8_t **data, size_t *len)
{
	if (conn == nullptr || data == nullptr || len == nullptr) {
		return -EINVAL;
	}

	*data = conn->cipher_out_buf;
	*len = conn->cipher_out_len;
	return 0;
}

void io_tls_consume_output(io_tls_conn_t *conn, size_t len)
{
	if (conn == nullptr || len == 0) {
		return;
	}

	if (len >= conn->cipher_out_len) {
		conn->cipher_out_len = 0;
		return;
	}

	size_t remaining = conn->cipher_out_len - len;
	memmove(conn->cipher_out_buf, conn->cipher_out_buf + len, remaining);
	conn->cipher_out_len = remaining;
}

int io_tls_shutdown(io_tls_conn_t *conn)
{
	if (conn == nullptr) {
		return -EINVAL;
	}

	int ret = wolfSSL_shutdown(conn->ssl);
	if (ret == WOLFSSL_SUCCESS) {
		return 0;
	}

	int err = wolfSSL_get_error(conn->ssl, ret);
	if (err == WOLFSSL_ERROR_WANT_READ || err == WOLFSSL_ERROR_WANT_WRITE) {
		return -EAGAIN;
	}

	/* wolfSSL_shutdown returns 0 for "sent close_notify, waiting for peer" */
	if (ret == 0) {
		return -EAGAIN;
	}

	return -EIO;
}

const char *io_tls_get_alpn(io_tls_conn_t *conn)
{
	if (conn == nullptr || conn->ssl == nullptr) {
		return nullptr;
	}

	char *proto = nullptr;
	word16 proto_len = 0;

	int ret = wolfSSL_ALPN_GetProtocol(conn->ssl, &proto, &proto_len);
	if (ret != WOLFSSL_SUCCESS || proto == nullptr || proto_len == 0) {
		return nullptr;
	}

	return proto;
}

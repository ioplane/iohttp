/**
 * @file ioh_tls.h
 * @brief wolfSSL TLS context management and per-connection state.
 *
 * Buffer-based I/O layer: ciphertext flows through internal buffers
 * rather than directly on sockets, enabling integration with io_uring.
 */

#ifndef IOHTTP_TLS_IO_TLS_H
#define IOHTTP_TLS_IO_TLS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <wolfssl/options.h>
#include <wolfssl/ssl.h>

/* ---- TLS cipher buffer size (one TLS record) ---- */

constexpr size_t IOH_TLS_CIPHER_BUF_SIZE = 16384;

/* ---- Configuration ---- */

typedef struct {
    const char *cert_file;
    const char *key_file;
    const char *ca_file;         /**< CA certificate for mTLS verification */
    bool require_client_cert;    /**< require client certificate (mTLS) */
    bool enable_session_tickets; /**< enable TLS session tickets */
    uint32_t session_cache_size; /**< server-side session cache entries */
    const char *alpn;            /**< comma-separated, e.g. "h2,http/1.1" */
} ioh_tls_config_t;

/* ---- Opaque context ---- */

typedef struct ioh_tls_ctx ioh_tls_ctx_t;

/* ---- Per-connection TLS state ---- */

typedef struct {
    WOLFSSL *ssl;
    uint8_t *cipher_in_buf; /**< io_uring recv -> here */
    size_t cipher_in_len;
    size_t cipher_in_pos;
    size_t cipher_in_cap;
    uint8_t *cipher_out_buf; /**< wolfSSL_write -> here -> io_uring send */
    size_t cipher_out_len;
    size_t cipher_out_cap;
    bool handshake_done;
} ioh_tls_conn_t;

/* ---- Config lifecycle ---- */

/**
 * @brief Initialize a TLS config with safe defaults.
 * @param cfg Config to initialize.
 */
void ioh_tls_config_init(ioh_tls_config_t *cfg);

/**
 * @brief Validate a TLS config.
 * @param cfg Config to validate.
 * @return 0 on success, -EINVAL if required fields are missing.
 */
[[nodiscard]] int ioh_tls_config_validate(const ioh_tls_config_t *cfg);

/* ---- Context lifecycle ---- */

/**
 * @brief Create a TLS context from config.
 * @param cfg Validated config.
 * @return Non-null on success; nullptr on failure.
 */
[[nodiscard]] ioh_tls_ctx_t *ioh_tls_ctx_create(const ioh_tls_config_t *cfg);

/**
 * @brief Destroy a TLS context. nullptr-safe.
 * @param ctx Context to destroy.
 */
void ioh_tls_ctx_destroy(ioh_tls_ctx_t *ctx);

/* ---- Per-connection lifecycle ---- */

/**
 * @brief Create a per-connection TLS state.
 * @param ctx  TLS context (shared).
 * @param fd   Socket file descriptor.
 * @return Non-null on success; nullptr on failure.
 */
[[nodiscard]] ioh_tls_conn_t *ioh_tls_conn_create(ioh_tls_ctx_t *ctx, int fd);

/**
 * @brief Destroy a per-connection TLS state. nullptr-safe.
 * @param conn Connection to destroy.
 */
void ioh_tls_conn_destroy(ioh_tls_conn_t *conn);

/* ---- Buffer-based I/O ---- */

/**
 * @brief Feed ciphertext data from io_uring recv into the TLS connection.
 * @param conn TLS connection.
 * @param data Ciphertext bytes.
 * @param len  Number of bytes.
 * @return 0 on success, -ENOMEM if buffer full, -EINVAL on null args.
 */
[[nodiscard]] int ioh_tls_feed_input(ioh_tls_conn_t *conn, const uint8_t *data, size_t len);

/**
 * @brief Perform one TLS handshake step (call repeatedly).
 * @param conn TLS connection.
 * @return 0 = done, -EAGAIN = need more data, <0 = error.
 */
[[nodiscard]] int ioh_tls_handshake(ioh_tls_conn_t *conn);

/**
 * @brief Read plaintext after handshake completes.
 * @param conn TLS connection.
 * @param buf  Output buffer.
 * @param len  Buffer capacity.
 * @return >0 = bytes read, -EAGAIN = need more ciphertext, <0 = error.
 */
[[nodiscard]] int ioh_tls_read(ioh_tls_conn_t *conn, uint8_t *buf, size_t len);

/**
 * @brief Write plaintext (wolfSSL encrypts to cipher_out_buf).
 * @param conn TLS connection.
 * @param buf  Plaintext input.
 * @param len  Input length.
 * @return >0 = bytes written, -EAGAIN = cipher_out full, <0 = error.
 */
[[nodiscard]] int ioh_tls_write(ioh_tls_conn_t *conn, const uint8_t *buf, size_t len);

/**
 * @brief Get pending ciphertext output to send via io_uring.
 * @param conn TLS connection.
 * @param data Output pointer to ciphertext.
 * @param len  Output length.
 * @return 0 on success, -EINVAL on null args.
 */
[[nodiscard]] int ioh_tls_get_output(ioh_tls_conn_t *conn, const uint8_t **data, size_t *len);

/**
 * @brief Mark ciphertext bytes as sent (advance output pointer).
 * @param conn TLS connection.
 * @param len  Number of bytes consumed.
 */
void ioh_tls_consume_output(ioh_tls_conn_t *conn, size_t len);

/**
 * @brief Initiate TLS shutdown.
 * @param conn TLS connection.
 * @return 0 = done, -EAGAIN = need to send/recv more, <0 = error.
 */
[[nodiscard]] int ioh_tls_shutdown(ioh_tls_conn_t *conn);

/**
 * @brief Get the negotiated ALPN protocol string.
 * @param conn TLS connection.
 * @return Protocol string (e.g. "h2") or nullptr if none negotiated.
 */
const char *ioh_tls_get_alpn(ioh_tls_conn_t *conn);

#endif /* IOHTTP_TLS_IO_TLS_H */

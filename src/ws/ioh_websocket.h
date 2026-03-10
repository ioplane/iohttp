/**
 * @file ioh_websocket.h
 * @brief WebSocket (RFC 6455) — wslay-backed event API with iohttp handshake.
 *
 * Handshake (ioh_ws_compute_accept, ioh_ws_validate_upgrade) is iohttp's own.
 * Framing, masking, close handshake, ping/pong — delegated to wslay.
 */

#ifndef IOHTTP_WS_WEBSOCKET_H
#define IOHTTP_WS_WEBSOCKET_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <wslay/wslay.h>

/* ---- Constants ---- */

constexpr size_t IOH_WS_ACCEPT_KEY_LEN = 28;          /* base64(SHA-1) */
constexpr size_t IOH_WS_DEFAULT_MAX_MSG = (1U << 20); /* 1 MiB */

/* ---- Callbacks (application-provided) ---- */

/**
 * @brief Called when a complete WebSocket message is received.
 * @param opcode  WSLAY_TEXT_FRAME or WSLAY_BINARY_FRAME.
 * @param msg     Message payload (reassembled from fragments by wslay).
 * @param len     Message length in bytes.
 * @param ctx     User context from ioh_ws_ctx_t.user_data.
 */
typedef void (*ioh_ws_on_msg_fn)(uint8_t opcode, const uint8_t *msg, size_t len, void *ctx);

/**
 * @brief Called when peer sends a close frame.
 * @param status_code  RFC 6455 close code (1000, 1001, ...).
 * @param ctx          User context.
 */
typedef void (*ioh_ws_on_close_fn)(uint16_t status_code, void *ctx);

/* ---- I/O adapters (caller must implement) ---- */

/**
 * @brief Read callback — must read up to len bytes from the peer.
 * @return Bytes read (>0), or -1 on error. Set *wouldblock=true on EAGAIN.
 */
typedef ssize_t (*ioh_ws_recv_fn)(uint8_t *buf, size_t len, bool *wouldblock, void *ctx);

/**
 * @brief Write callback — must send up to len bytes to the peer.
 * @return Bytes sent (>0), or -1 on error. Set *wouldblock=true on EAGAIN.
 */
typedef ssize_t (*ioh_ws_send_fn)(const uint8_t *data, size_t len, bool *wouldblock, void *ctx);

/* ---- WebSocket context ---- */

typedef struct {
    ioh_ws_on_msg_fn on_msg;
    ioh_ws_on_close_fn on_close;
    ioh_ws_recv_fn recv;
    ioh_ws_send_fn send;
    void *user_data;
    wslay_event_context_ptr wslay_ctx;
    uint64_t max_recv_msg_len;
} ioh_ws_ctx_t;

/* ---- Upgrade handshake (iohttp's own, uses wolfSSL SHA-1) ---- */

/**
 * @brief Compute the Sec-WebSocket-Accept value from client key.
 * @param client_key  The Sec-WebSocket-Key header value (24 chars base64).
 * @param accept_out  Output buffer (at least IOH_WS_ACCEPT_KEY_LEN + 1 bytes).
 * @return 0 on success, -EINVAL if client_key is nullptr.
 */
[[nodiscard]] int ioh_ws_compute_accept(const char *client_key, char *accept_out);

/**
 * @brief Validate an HTTP upgrade request for WebSocket.
 * @param method       HTTP method string (must be "GET").
 * @param upgrade_hdr  Upgrade header value (must contain "websocket").
 * @param conn_hdr     Connection header value (must contain "Upgrade").
 * @param ws_key       Sec-WebSocket-Key value (must be non-empty).
 * @param ws_version   Sec-WebSocket-Version value (must be "13").
 * @return 0 if valid, -EINVAL if invalid.
 */
[[nodiscard]] int ioh_ws_validate_upgrade(const char *method, const char *upgrade_hdr,
                                         const char *conn_hdr, const char *ws_key,
                                         const char *ws_version);

/* ---- wslay-backed session lifecycle ---- */

/**
 * @brief Initialize a server-side WebSocket context backed by wslay.
 * @param ws  Context to initialize.
 * @return 0 on success, -EINVAL on bad args, -ENOMEM on allocation failure.
 */
[[nodiscard]] int ioh_ws_ctx_init(ioh_ws_ctx_t *ws);

/**
 * @brief Destroy the WebSocket context, freeing wslay resources.
 */
void ioh_ws_ctx_destroy(ioh_ws_ctx_t *ws);

/* ---- Event processing ---- */

/**
 * @brief Process incoming data — calls recv callback, dispatches messages.
 * @return 0 on success, -EIO on fatal wslay error.
 */
[[nodiscard]] int ioh_ws_recv(ioh_ws_ctx_t *ws);

/**
 * @brief Send all queued messages — calls send callback.
 * @return 0 on success, -EIO on fatal wslay error.
 */
[[nodiscard]] int ioh_ws_send(ioh_ws_ctx_t *ws);

/* ---- Message queueing ---- */

/**
 * @brief Queue a text message for sending.
 * @return 0 on success, negative errno on error.
 */
[[nodiscard]] int ioh_ws_queue_text(ioh_ws_ctx_t *ws, const char *msg, size_t len);

/**
 * @brief Queue a binary message for sending.
 * @return 0 on success, negative errno on error.
 */
[[nodiscard]] int ioh_ws_queue_binary(ioh_ws_ctx_t *ws, const uint8_t *data, size_t len);

/**
 * @brief Queue a close frame.
 * @return 0 on success, negative errno on error.
 */
[[nodiscard]] int ioh_ws_queue_close(ioh_ws_ctx_t *ws, uint16_t code, const char *reason,
                                    size_t reason_len);

/**
 * @brief Queue a ping frame.
 * @return 0 on success, negative errno on error.
 */
[[nodiscard]] int ioh_ws_queue_ping(ioh_ws_ctx_t *ws);

/* ---- State queries ---- */

/** @return true if the context wants to read more data. */
bool ioh_ws_want_read(const ioh_ws_ctx_t *ws);

/** @return true if the context has data to write. */
bool ioh_ws_want_write(const ioh_ws_ctx_t *ws);

/** @return true if close frame has been received. */
bool ioh_ws_close_received(const ioh_ws_ctx_t *ws);

/** @return true if close frame has been sent. */
bool ioh_ws_close_sent(const ioh_ws_ctx_t *ws);

#endif /* IOHTTP_WS_WEBSOCKET_H */

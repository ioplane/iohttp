/**
 * @file io_http3.h
 * @brief HTTP/3 session management — nghttp3-backed, buffer-based I/O.
 *
 * Sits on top of io_quic.h (QUIC transport). Application feeds QUIC stream
 * data via io_http3_on_stream_data(), retrieves output via QUIC layer.
 * Same dispatch pattern as io_http2: callback fires on complete request.
 */

#ifndef IOHTTP_HTTP_HTTP3_H
#define IOHTTP_HTTP_HTTP3_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "core/io_ctx.h"
#include "http/io_quic.h"
#include "http/io_request.h"
#include "http/io_response.h"

/* ---- Forward declaration (opaque) ---- */

typedef struct io_http3_session io_http3_session_t;

/* ---- Configuration ---- */

typedef struct {
    uint32_t max_header_list_size; /* default 8192 */
    uint32_t qpack_max_dtable_capacity; /* default 0 (no dynamic table, simpler) */
    uint32_t qpack_blocked_streams; /* default 0 */
} io_http3_config_t;

/* ---- Callback types ---- */

/**
 * @brief Called when a complete HTTP/3 request is received.
 * @param c         Unified context. Handler fills c->resp.
 * @param stream_id QUIC stream ID.
 * @param user_data User context from io_http3_session_create().
 * @return 0 on success, negative errno on failure.
 */
typedef int (*io_http3_on_request_fn)(io_ctx_t *c, int64_t stream_id, void *user_data);

/* ---- Session lifecycle ---- */

/**
 * @brief Initialize HTTP/3 config with defaults.
 */
void io_http3_config_init(io_http3_config_t *cfg);

/**
 * @brief Create an HTTP/3 session bound to a QUIC connection.
 * @param cfg       Configuration (nullptr for defaults).
 * @param quic_conn QUIC connection (must be established).
 * @param on_req    Callback for completed requests.
 * @param user_data Passed to on_req callback.
 * @return New session, or nullptr on failure.
 */
[[nodiscard]] io_http3_session_t *io_http3_session_create(const io_http3_config_t *cfg,
                                                           io_quic_conn_t *quic_conn,
                                                           io_http3_on_request_fn on_req,
                                                           void *user_data);

/**
 * @brief Destroy an HTTP/3 session and free all resources.
 */
void io_http3_session_destroy(io_http3_session_t *session);

/* ---- Data processing ---- */

/**
 * @brief Feed QUIC stream data into the HTTP/3 session.
 * Called from the QUIC stream_data callback.
 * @param session   HTTP/3 session.
 * @param stream_id QUIC stream ID.
 * @param data      Stream data bytes.
 * @param len       Number of bytes.
 * @param fin       End of stream flag.
 * @return 0 on success, negative errno on error.
 */
[[nodiscard]] int io_http3_on_stream_data(io_http3_session_t *session, int64_t stream_id,
                                           const uint8_t *data, size_t len, bool fin);

/**
 * @brief Notify HTTP/3 session that a new stream was opened.
 * Called from the QUIC stream_open callback.
 * @return 0 on success, negative errno on error.
 */
[[nodiscard]] int io_http3_on_stream_open(io_http3_session_t *session, int64_t stream_id);

/**
 * @brief Submit response for a stream.
 * Called by the request handler after processing.
 * @param session   HTTP/3 session.
 * @param stream_id Stream to respond on.
 * @param resp      Response data.
 * @return 0 on success, negative errno on error.
 */
[[nodiscard]] int io_http3_submit_response(io_http3_session_t *session, int64_t stream_id,
                                            const io_response_t *resp);

/* ---- State queries ---- */

/** @return true if the session has been shut down. */
[[nodiscard]] bool io_http3_is_shutdown(const io_http3_session_t *session);

/**
 * @brief Initiate HTTP/3 GOAWAY.
 * @return 0 on success, negative errno on error.
 */
[[nodiscard]] int io_http3_shutdown(io_http3_session_t *session);

#endif /* IOHTTP_HTTP_HTTP3_H */

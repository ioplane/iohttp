/**
 * @file ioh_http2.h
 * @brief HTTP/2 session management — nghttp2-backed, buffer-based I/O.
 *
 * No socket I/O — application feeds raw bytes via ioh_http2_on_recv(),
 * retrieves output via ioh_http2_flush(). Same pattern as WebSocket/SSE.
 */

#ifndef IOHTTP_HTTP_HTTP2_H
#define IOHTTP_HTTP_HTTP2_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "core/ioh_ctx.h"
#include "http/ioh_request.h"
#include "http/ioh_response.h"

/* ---- Forward declaration (opaque) ---- */

typedef struct ioh_http2_session ioh_http2_session_t;

/* ---- Configuration ---- */

typedef struct {
    uint32_t max_concurrent_streams; /* default 100 — CVE-2023-44487 Rapid Reset protection */
    uint32_t initial_window_size;    /* default 65535 */
    uint32_t max_frame_size;         /* default 16384 */
    uint32_t max_header_list_size;   /* default 8192 */
    uint32_t max_rst_stream_per_sec; /* default 100, Rapid Reset rate limit */
} ioh_http2_config_t;

/* ---- Callback types ---- */

/**
 * @brief Called when a complete HTTP/2 request is received (END_STREAM).
 * @param c         Unified context with req/resp pointers. Handler fills c->resp.
 * @param stream_id The HTTP/2 stream ID.
 * @param user_data User context from ioh_http2_session_create().
 * @return 0 on success, negative errno on failure.
 */
typedef int (*ioh_http2_on_request_fn)(ioh_ctx_t *c, int32_t stream_id, void *user_data);

/* ---- Session lifecycle ---- */

/**
 * @brief Create an HTTP/2 server session.
 * @param cfg       Configuration (nullptr for defaults).
 * @param on_req    Callback for completed requests.
 * @param user_data Passed to on_req callback.
 * @return New session, or nullptr on failure.
 */
[[nodiscard]] ioh_http2_session_t *ioh_http2_session_create(const ioh_http2_config_t *cfg,
                                                          ioh_http2_on_request_fn on_req,
                                                          void *user_data);

/**
 * @brief Destroy an HTTP/2 session and free all resources.
 * @param session Session to destroy (nullptr is safe).
 */
void ioh_http2_session_destroy(ioh_http2_session_t *session);

/* ---- Data processing ---- */

/**
 * @brief Feed received data into the HTTP/2 session.
 * @param session Session to process data for.
 * @param data    Raw bytes received from peer.
 * @param len     Number of bytes.
 * @return Number of bytes consumed (>= 0), or negative errno on error.
 */
[[nodiscard]] ssize_t ioh_http2_on_recv(ioh_http2_session_t *session, const uint8_t *data,
                                       size_t len);

/**
 * @brief Get pending output data from the session.
 * @param session  Session to flush.
 * @param out_data Set to point to output buffer (valid until next flush/recv call).
 * @param out_len  Set to output length.
 * @return 0 on success (may set out_len=0 if nothing to send), negative errno on error.
 */
[[nodiscard]] int ioh_http2_flush(ioh_http2_session_t *session, const uint8_t **out_data,
                                 size_t *out_len);

/* ---- GOAWAY ---- */

/**
 * @brief Initiate graceful shutdown with two-phase GOAWAY (RFC 9113 §5.1.1).
 * @param session Session to shut down.
 * @return 0 on success, negative errno on error.
 */
[[nodiscard]] int ioh_http2_goaway(ioh_http2_session_t *session);

/* ---- State queries ---- */

/** @return true if the session wants to read more data. */
[[nodiscard]] bool ioh_http2_want_read(const ioh_http2_session_t *session);

/** @return true if the session has data to write. */
[[nodiscard]] bool ioh_http2_want_write(const ioh_http2_session_t *session);

/** @return true if GOAWAY has been sent. */
[[nodiscard]] bool ioh_http2_goaway_sent(const ioh_http2_session_t *session);

/** @return true if the session is draining (GOAWAY sent, no more streams). */
[[nodiscard]] bool ioh_http2_is_draining(const ioh_http2_session_t *session);

#endif /* IOHTTP_HTTP_HTTP2_H */

/**
 * @file ioh_sse.h
 * @brief Server-Sent Events (SSE) formatting and stream management.
 */

#ifndef IOHTTP_WS_SSE_H
#define IOHTTP_WS_SSE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ---- SSE event structure ---- */

typedef struct {
    const char *event; /* event type (optional, nullptr to omit) */
    const char *data;  /* event data (required) */
    const char *id;    /* event ID (optional, nullptr to omit) */
    uint32_t retry_ms; /* reconnect interval hint (0 = omit) */
} ioh_sse_event_t;

/* ---- SSE stream context ---- */

typedef struct {
    uint8_t *buf;
    size_t buf_len;
    size_t buf_capacity;
    char *last_event_id;
    bool headers_sent;
} ioh_sse_stream_t;

/* ---- SSE stream lifecycle ---- */

/**
 * @brief Initialize an SSE stream.
 * @param stream  Stream to initialize.
 * @return 0 on success, -EINVAL if stream is nullptr.
 */
[[nodiscard]] int ioh_sse_stream_init(ioh_sse_stream_t *stream);

/**
 * @brief Destroy an SSE stream, freeing all resources.
 * @param stream  Stream to destroy.
 */
void ioh_sse_stream_destroy(ioh_sse_stream_t *stream);

/* ---- SSE formatting ---- */

/**
 * @brief Format SSE response headers into buffer.
 * @param buf       Output buffer.
 * @param buf_size  Buffer capacity.
 * @return Bytes written on success, -EINVAL if buf is nullptr, -ENOSPC if too small.
 */
[[nodiscard]] int ioh_sse_format_headers(uint8_t *buf, size_t buf_size);

/**
 * @brief Format an SSE event into the stream buffer.
 * @param stream  SSE stream context.
 * @param event   Event to format.
 * @return 0 on success, -EINVAL if data is nullptr, -ENOMEM if buffer grow fails.
 */
[[nodiscard]] int ioh_sse_format_event(ioh_sse_stream_t *stream, const ioh_sse_event_t *event);

/**
 * @brief Format a comment (heartbeat) into the stream buffer.
 * @param stream   SSE stream context.
 * @param comment  Comment text (without leading colon).
 * @return 0 on success, -EINVAL if comment is nullptr, -ENOMEM if buffer grow fails.
 */
[[nodiscard]] int ioh_sse_format_comment(ioh_sse_stream_t *stream, const char *comment);

/**
 * @brief Get the formatted data and reset the stream buffer.
 * @param stream    SSE stream context.
 * @param data_out  Output pointer to formatted data.
 * @param len_out   Output data length.
 */
void ioh_sse_stream_flush(ioh_sse_stream_t *stream, const uint8_t **data_out, size_t *len_out);

#endif /* IOHTTP_WS_SSE_H */

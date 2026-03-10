/**
 * @file ioh_response.h
 * @brief Protocol-independent HTTP response builder.
 */

#ifndef IOHTTP_HTTP_RESPONSE_H
#define IOHTTP_HTTP_RESPONSE_H

#include "http/ioh_request.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ---- Response ---- */

typedef struct {
    uint16_t status;
    ioh_header_t *headers;
    uint32_t header_count;
    uint32_t header_capacity;
    uint8_t *body;
    size_t body_len;
    size_t body_capacity;
    bool headers_sent;
    bool chunked;
} ioh_response_t;

/* ---- Response lifecycle ---- */

/**
 * @brief Initialize a response with default allocations.
 * @param resp Response to initialize.
 * @return 0 on success, -ENOMEM on allocation failure.
 */
[[nodiscard]] int ioh_response_init(ioh_response_t *resp);

/**
 * @brief Reset a response for reuse (keeps allocated buffers).
 * @param resp Response to reset.
 */
void ioh_response_reset(ioh_response_t *resp);

/**
 * @brief Free all memory owned by a response.
 * @param resp Response to destroy (nullptr is safe).
 */
void ioh_response_destroy(ioh_response_t *resp);

/* ---- Header and body manipulation ---- */

/**
 * @brief Set (or replace) a response header.
 * @param resp  Response to modify.
 * @param name  Header name.
 * @param value Header value.
 * @return 0 on success, -EINVAL on bad input, -ENOMEM on allocation failure.
 */
[[nodiscard]] int ioh_response_set_header(ioh_response_t *resp, const char *name, const char *value);

/**
 * @brief Add a response header (allows duplicates, e.g. Set-Cookie).
 * @param resp  Response to modify.
 * @param name  Header name.
 * @param value Header value.
 * @return 0 on success, -EINVAL on bad input, -ENOMEM on allocation failure.
 */
[[nodiscard]] int ioh_response_add_header(ioh_response_t *resp, const char *name, const char *value);

/**
 * @brief Set the response body (copies data).
 * @param resp Response to modify.
 * @param body Body data.
 * @param len  Body length in bytes.
 * @return 0 on success, -EINVAL on bad input, -ENOMEM on allocation failure.
 */
[[nodiscard]] int ioh_response_set_body(ioh_response_t *resp, const uint8_t *body, size_t len);

/**
 * @brief Append a token to the Vary header (avoids duplicates).
 * @param resp  Response to modify.
 * @param token Vary token (e.g., "Accept-Encoding").
 * @return 0 on success, negative errno on error.
 */
[[nodiscard]] int ioh_response_add_vary(ioh_response_t *resp, const char *token);

/* ---- Convenience builders ---- */

/**
 * @brief Set status, content-type, and body in one call.
 * @param resp         Response to modify.
 * @param status       HTTP status code.
 * @param content_type Content-Type header value.
 * @param body         Body data (may be nullptr if body_len is 0).
 * @param body_len     Body length in bytes.
 * @return 0 on success, negative errno on failure.
 */
[[nodiscard]] int ioh_respond(ioh_response_t *resp, uint16_t status, const char *content_type,
                             const uint8_t *body, size_t body_len);

/**
 * @brief Convenience for JSON responses.
 * @param resp   Response to modify.
 * @param status HTTP status code.
 * @param json   NUL-terminated JSON string.
 * @return 0 on success, negative errno on failure.
 */
[[nodiscard]] int ioh_respond_json(ioh_response_t *resp, uint16_t status, const char *json);

/* ---- Status text ---- */

/**
 * @brief Return the reason phrase for an HTTP status code.
 * @param status HTTP status code.
 * @return Static string (e.g. "OK"), or "Unknown" for unrecognized codes.
 */
const char *ioh_status_text(uint16_t status);

#endif /* IOHTTP_HTTP_RESPONSE_H */

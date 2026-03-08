/**
 * @file io_request.h
 * @brief Protocol-independent HTTP request abstraction.
 */

#ifndef IOHTTP_HTTP_REQUEST_H
#define IOHTTP_HTTP_REQUEST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ---- HTTP methods ---- */

typedef enum : uint8_t {
    IO_METHOD_GET = 0,
    IO_METHOD_POST,
    IO_METHOD_PUT,
    IO_METHOD_DELETE,
    IO_METHOD_PATCH,
    IO_METHOD_HEAD,
    IO_METHOD_OPTIONS,
    IO_METHOD_TRACE,
    IO_METHOD_CONNECT,
    IO_METHOD_UNKNOWN,
} io_method_t;

/* ---- Header key-value pair ---- */

typedef struct {
    const char *name;
    size_t name_len;
    const char *value;
    size_t value_len;
} io_header_t;

/* ---- Path/query parameter ---- */

typedef struct {
    const char *name;
    size_t name_len;
    const char *value;
    size_t value_len;
} io_param_t;

/* ---- Limits ---- */

constexpr uint32_t IO_MAX_PATH_PARAMS = 8;
constexpr uint32_t IO_MAX_HEADERS = 64;
constexpr size_t IO_MAX_HEADER_SIZE = 8192;
constexpr size_t IO_MAX_URI_SIZE = 4096;

/* ---- Request ---- */

typedef struct {
    io_method_t method;
    const char *path;
    size_t path_len;
    const char *query;
    size_t query_len;
    io_header_t headers[IO_MAX_HEADERS];
    uint32_t header_count;
    const uint8_t *body;
    size_t body_len;
    io_param_t params[IO_MAX_PATH_PARAMS];
    uint32_t param_count;
    uint8_t http_version_major;
    uint8_t http_version_minor;
    bool keep_alive;
    size_t content_length;
    const char *content_type;
    const char *host;
} io_request_t;

/* ---- Request functions ---- */

/**
 * @brief Initialize a request to zero state.
 * @param req Request to initialize.
 */
void io_request_init(io_request_t *req);

/**
 * @brief Reset a request for reuse (same as init).
 * @param req Request to reset.
 */
void io_request_reset(io_request_t *req);

/**
 * @brief Find a header value by name (case-insensitive).
 * @param req  Request to search.
 * @param name Header name to find.
 * @return Header value pointer, or nullptr if not found.
 */
const char *io_request_header(const io_request_t *req, const char *name);

/**
 * @brief Parse an HTTP method string to enum (case-sensitive per RFC 9110).
 * @param method Method string (e.g. "GET").
 * @param len    Length of method string.
 * @return Parsed method, or IO_METHOD_UNKNOWN.
 */
io_method_t io_method_parse(const char *method, size_t len);

/**
 * @brief Return the canonical name string for a method enum.
 * @param method Method enum value.
 * @return Static string (e.g. "GET"), or "UNKNOWN".
 */
const char *io_method_name(io_method_t method);

/**
 * @brief Look up a cookie value by name from the Cookie header.
 * @param req  Request to search.
 * @param name Cookie name to find.
 * @return Cookie value pointer (within the header), or nullptr.
 */
const char *io_request_cookie(const io_request_t *req, const char *name);

/**
 * @brief Look up a query parameter value by name.
 * @param req  Request to search.
 * @param name Parameter name to find.
 * @return Parameter value pointer (within the query string), or nullptr.
 */
const char *io_request_query_param(const io_request_t *req, const char *name);

/**
 * @brief Check Accept header against provided content types.
 * @param req   Request to check.
 * @param types Array of MIME type strings.
 * @param count Number of types in array.
 * @return Best matching type string, or nullptr if none match.
 */
const char *io_request_accepts(const io_request_t *req, const char **types, uint32_t count);

#endif /* IOHTTP_HTTP_REQUEST_H */

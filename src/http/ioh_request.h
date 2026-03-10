/**
 * @file ioh_request.h
 * @brief Protocol-independent HTTP request abstraction.
 */

#ifndef IOHTTP_HTTP_REQUEST_H
#define IOHTTP_HTTP_REQUEST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ---- HTTP methods ---- */

typedef enum : uint8_t {
    IOH_METHOD_GET = 0,
    IOH_METHOD_POST,
    IOH_METHOD_PUT,
    IOH_METHOD_DELETE,
    IOH_METHOD_PATCH,
    IOH_METHOD_HEAD,
    IOH_METHOD_OPTIONS,
    IOH_METHOD_TRACE,
    IOH_METHOD_CONNECT,
    IOH_METHOD_UNKNOWN,
} ioh_method_t;

/* ---- Header key-value pair ---- */

typedef struct {
    const char *name;
    size_t name_len;
    const char *value;
    size_t value_len;
} ioh_header_t;

/* ---- Path/query parameter ---- */

typedef struct {
    const char *name;
    size_t name_len;
    const char *value;
    size_t value_len;
} ioh_param_t;

/* ---- Limits ---- */

constexpr uint32_t IOH_MAX_PATH_PARAMS = 8;
constexpr uint32_t IOH_MAX_HEADERS = 64;
constexpr size_t IOH_MAX_HEADER_SIZE = 8192;
constexpr size_t IOH_MAX_URI_SIZE = 4096;

/* ---- Request ---- */

typedef struct {
    ioh_method_t method;
    const char *path;
    size_t path_len;
    const char *query;
    size_t query_len;
    ioh_header_t headers[IOH_MAX_HEADERS];
    uint32_t header_count;
    const uint8_t *body;
    size_t body_len;
    ioh_param_t params[IOH_MAX_PATH_PARAMS];
    uint32_t param_count;
    uint8_t http_version_major;
    uint8_t http_version_minor;
    bool keep_alive;
    size_t content_length;
    const char *content_type;
    const char *host;
} ioh_request_t;

/* ---- Request functions ---- */

/**
 * @brief Initialize a request to zero state.
 * @param req Request to initialize.
 */
void ioh_request_init(ioh_request_t *req);

/**
 * @brief Reset a request for reuse (same as init).
 * @param req Request to reset.
 */
void ioh_request_reset(ioh_request_t *req);

/**
 * @brief Find a header value by name (case-insensitive).
 * @param req  Request to search.
 * @param name Header name to find.
 * @return Header value pointer, or nullptr if not found.
 */
const char *ioh_request_header(const ioh_request_t *req, const char *name);

/**
 * @brief Parse an HTTP method string to enum (case-sensitive per RFC 9110).
 * @param method Method string (e.g. "GET").
 * @param len    Length of method string.
 * @return Parsed method, or IOH_METHOD_UNKNOWN.
 */
ioh_method_t ioh_method_parse(const char *method, size_t len);

/**
 * @brief Return the canonical name string for a method enum.
 * @param method Method enum value.
 * @return Static string (e.g. "GET"), or "UNKNOWN".
 */
const char *ioh_method_name(ioh_method_t method);

/**
 * @brief Look up a path parameter value by name.
 * @param req  Request to search.
 * @param name Parameter name to find.
 * @return Parameter value pointer, or nullptr if not found.
 */
const char *ioh_request_param(const ioh_request_t *req, const char *name);

/**
 * @brief Extract a path parameter as int64_t.
 * @param req  Request to search.
 * @param name Parameter name to find.
 * @param out  Output value on success.
 * @return 0 on success, -EINVAL on missing/invalid, -ERANGE on overflow.
 */
[[nodiscard]] int ioh_request_param_i64(const ioh_request_t *req, const char *name, int64_t *out);

/**
 * @brief Extract a path parameter as uint64_t.
 * @param req  Request to search.
 * @param name Parameter name to find.
 * @param out  Output value on success.
 * @return 0 on success, -EINVAL on missing/invalid, -ERANGE on overflow.
 */
[[nodiscard]] int ioh_request_param_u64(const ioh_request_t *req, const char *name, uint64_t *out);

/**
 * @brief Extract a path parameter as bool.
 * @param req  Request to search.
 * @param name Parameter name to find.
 * @param out  Output value on success.
 * @return 0 on success, -EINVAL on missing/unrecognized value.
 */
[[nodiscard]] int ioh_request_param_bool(const ioh_request_t *req, const char *name, bool *out);

/**
 * @brief Return the number of path parameters.
 * @param req Request to query.
 * @return Parameter count.
 */
uint32_t ioh_request_param_count(const ioh_request_t *req);

/**
 * @brief Look up a cookie value by name from the Cookie header.
 * @param req  Request to search.
 * @param name Cookie name to find.
 * @return Cookie value pointer (within the header), or nullptr.
 */
const char *ioh_request_cookie(const ioh_request_t *req, const char *name);

/**
 * @brief Look up a query parameter value by name.
 * @param req  Request to search.
 * @param name Parameter name to find.
 * @return Parameter value pointer (within the query string), or nullptr.
 */
const char *ioh_request_query_param(const ioh_request_t *req, const char *name);

/**
 * @brief Check Accept header against provided content types.
 * @param req   Request to check.
 * @param types Array of MIME type strings.
 * @param count Number of types in array.
 * @return Best matching type string, or nullptr if none match.
 */
const char *ioh_request_accepts(const ioh_request_t *req, const char **types, uint32_t count);

#endif /* IOHTTP_HTTP_REQUEST_H */

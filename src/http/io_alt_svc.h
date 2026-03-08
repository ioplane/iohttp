/**
 * @file io_alt_svc.h
 * @brief Alt-Svc header generation for HTTP/3 discovery (RFC 7838).
 */

#ifndef IOHTTP_HTTP_ALT_SVC_H
#define IOHTTP_HTTP_ALT_SVC_H

#include "http/io_response.h"

#include <stddef.h>
#include <stdint.h>

/**
 * @brief Format an Alt-Svc header value.
 * @param buf     Output buffer.
 * @param buf_len Buffer capacity.
 * @param port    UDP port for HTTP/3.
 * @param max_age Max-age in seconds (0 = omit).
 * @return Length of formatted string, or -ENOSPC / -EINVAL on error.
 */
[[nodiscard]] int io_alt_svc_format(char *buf, size_t buf_len, uint16_t port, uint32_t max_age);

/**
 * @brief Add Alt-Svc header to a response.
 * @param resp    Response to add header to.
 * @param port    UDP port for HTTP/3.
 * @param max_age Max-age in seconds (0 = omit).
 * @return 0 on success, negative errno on error.
 */
[[nodiscard]] int io_alt_svc_add_header(io_response_t *resp, uint16_t port, uint32_t max_age);

#endif /* IOHTTP_HTTP_ALT_SVC_H */

/**
 * @file ioh_multipart.h
 * @brief RFC 7578 multipart/form-data parser for file uploads.
 */

#ifndef IOHTTP_HTTP_MULTIPART_H
#define IOHTTP_HTTP_MULTIPART_H

#include "http/ioh_request.h"

#include <stddef.h>
#include <stdint.h>

typedef struct {
    const char *name; /* form field name */
    size_t name_len;
    const char *filename; /* original filename (nullptr if not file) */
    size_t filename_len;
    const char *content_type; /* part content-type (nullptr if not specified) */
    size_t content_type_len;
    const uint8_t *data; /* part body (zero-copy pointer into request body) */
    size_t data_len;
} ioh_multipart_part_t;

typedef struct {
    uint32_t max_parts;    /* default 64 */
    size_t max_part_size;  /* default 10MB */
    size_t max_total_size; /* default 50MB */
} ioh_multipart_config_t;

constexpr uint32_t IOH_MULTIPART_DEFAULT_MAX_PARTS = 64;
constexpr size_t IOH_MULTIPART_DEFAULT_MAX_PART_SIZE = 10 * 1024 * 1024;
constexpr size_t IOH_MULTIPART_DEFAULT_MAX_TOTAL_SIZE = 50 * 1024 * 1024;

/**
 * @brief Initialize config with defaults.
 * @param cfg Config to initialize.
 */
void ioh_multipart_config_init(ioh_multipart_config_t *cfg);

/**
 * @brief Extract boundary string from Content-Type header.
 * @param content_type The Content-Type header value.
 * @param boundary     Output pointer to boundary string (points into content_type).
 * @param boundary_len Output boundary length.
 * @return 0 on success, -EINVAL if not multipart or boundary missing.
 */
[[nodiscard]] int ioh_multipart_boundary(const char *content_type, const char **boundary,
                                        size_t *boundary_len);

/**
 * @brief Parse multipart body into parts array.
 * @param body         Request body buffer.
 * @param body_len     Body length.
 * @param boundary     Boundary string (without leading --).
 * @param boundary_len Boundary length.
 * @param cfg          Config with limits.
 * @param parts        Output parts array (caller provides).
 * @param part_count   Input: max parts array size. Output: actual parts parsed.
 * @return 0 on success, -EINVAL malformed, -E2BIG too many/large parts.
 */
[[nodiscard]] int ioh_multipart_parse(const uint8_t *body, size_t body_len, const char *boundary,
                                     size_t boundary_len, const ioh_multipart_config_t *cfg,
                                     ioh_multipart_part_t *parts, uint32_t *part_count);

#endif /* IOHTTP_HTTP_MULTIPART_H */

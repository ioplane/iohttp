/**
 * @file ioh_static.h
 * @brief Static file serving with MIME detection, ETag, and range requests.
 */

#ifndef IOHTTP_STATIC_STATIC_H
#define IOHTTP_STATIC_STATIC_H

#include "http/ioh_request.h"
#include "http/ioh_response.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    const char *root_dir;     /* document root */
    bool directory_listing;   /* default false */
    bool etag;                /* ETag generation, default true */
    uint32_t max_age_default; /* Cache-Control max-age in seconds, default 3600 */
} ioh_static_config_t;

/**
 * @brief Initialize config with defaults.
 * @param cfg Config to initialize.
 */
void ioh_static_config_init(ioh_static_config_t *cfg);

/**
 * @brief Serve a static file for the given request path.
 * @param cfg   Static serving config.
 * @param req   HTTP request (uses path for file lookup).
 * @param resp  HTTP response to populate.
 * @return 0 on success, -ENOENT if file not found, -EACCES for path traversal, <0 on error.
 */
[[nodiscard]] int ioh_static_serve(const ioh_static_config_t *cfg, const ioh_request_t *req,
                                  ioh_response_t *resp);

/**
 * @brief Get MIME type for a file extension.
 * @param path  File path or name.
 * @param len   Path length.
 * @return MIME type string, or "application/octet-stream" for unknown.
 */
const char *ioh_mime_type(const char *path, size_t len);

/**
 * @brief Generate ETag from file stat info.
 * @param mtime  File modification time (seconds since epoch).
 * @param size   File size in bytes.
 * @param buf    Output buffer for ETag string (including quotes).
 * @param buf_size  Buffer size (at least 32 bytes).
 * @return Bytes written, or -ENOSPC.
 */
[[nodiscard]] int ioh_etag_generate(uint64_t mtime, uint64_t size, char *buf, size_t buf_size);

#endif /* IOHTTP_STATIC_STATIC_H */

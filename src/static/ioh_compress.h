/**
 * @file ioh_compress.h
 * @brief Compression middleware: gzip/brotli negotiation and precompressed file serving.
 */

#ifndef IOHTTP_STATIC_COMPRESS_H
#define IOHTTP_STATIC_COMPRESS_H

#include "http/ioh_request.h"
#include "http/ioh_response.h"

#include <stdbool.h>
#include <stdint.h>

typedef enum : uint8_t {
    IOH_ENCODING_NONE = 0,
    IOH_ENCODING_GZIP,
    IOH_ENCODING_BROTLI,
} ioh_encoding_t;

typedef struct {
    bool enable_gzip;
    bool enable_brotli;
    bool enable_precompressed; /* serve .gz/.br files if they exist */
    uint32_t min_size;         /* don't compress below this (default 1024) */
    int gzip_level;            /* 1-9, default 6 */
    int brotli_quality;        /* 0-11, default 4 */
} ioh_compress_config_t;

/**
 * @brief Initialize compression config with sensible defaults.
 * @param cfg Config to initialize.
 */
void ioh_compress_config_init(ioh_compress_config_t *cfg);

/**
 * @brief Parse Accept-Encoding header and select best encoding.
 * @param accept_encoding Accept-Encoding header value.
 * @return Best supported encoding.
 */
ioh_encoding_t ioh_compress_negotiate(const char *accept_encoding);

/**
 * @brief Compress response body in-place.
 *
 * If body is below min_size, does nothing.
 * Sets Content-Encoding and Vary headers on success.
 *
 * @param cfg  Compression config.
 * @param req  Request (for Accept-Encoding).
 * @param resp Response to compress.
 * @return 0 on success (or skipped), <0 on error.
 */
[[nodiscard]] int ioh_compress_response(const ioh_compress_config_t *cfg, const ioh_request_t *req,
                                       ioh_response_t *resp);

/**
 * @brief Check for precompressed file variant.
 * @param path      Original file path.
 * @param encoding  Negotiated encoding.
 * @param out_path  Buffer for precompressed path (e.g., "file.js.gz").
 * @param out_size  Buffer size.
 * @return true if precompressed file exists.
 */
bool ioh_compress_precompressed(const char *path, ioh_encoding_t encoding, char *out_path,
                               size_t out_size);

#endif /* IOHTTP_STATIC_COMPRESS_H */

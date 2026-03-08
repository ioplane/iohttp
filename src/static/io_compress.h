/**
 * @file io_compress.h
 * @brief Compression middleware: gzip/brotli negotiation and precompressed file serving.
 */

#ifndef IOHTTP_STATIC_COMPRESS_H
#define IOHTTP_STATIC_COMPRESS_H

#include "http/io_request.h"
#include "http/io_response.h"

#include <stdbool.h>
#include <stdint.h>

typedef enum : uint8_t {
    IO_ENCODING_NONE = 0,
    IO_ENCODING_GZIP,
    IO_ENCODING_BROTLI,
} io_encoding_t;

typedef struct {
    bool enable_gzip;
    bool enable_brotli;
    bool enable_precompressed; /* serve .gz/.br files if they exist */
    uint32_t min_size;         /* don't compress below this (default 1024) */
    int gzip_level;            /* 1-9, default 6 */
    int brotli_quality;        /* 0-11, default 4 */
} io_compress_config_t;

/**
 * @brief Initialize compression config with sensible defaults.
 * @param cfg Config to initialize.
 */
void io_compress_config_init(io_compress_config_t *cfg);

/**
 * @brief Parse Accept-Encoding header and select best encoding.
 * @param accept_encoding Accept-Encoding header value.
 * @return Best supported encoding.
 */
io_encoding_t io_compress_negotiate(const char *accept_encoding);

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
[[nodiscard]] int io_compress_response(const io_compress_config_t *cfg,
                                       const io_request_t *req,
                                       io_response_t *resp);

/**
 * @brief Check for precompressed file variant.
 * @param path      Original file path.
 * @param encoding  Negotiated encoding.
 * @param out_path  Buffer for precompressed path (e.g., "file.js.gz").
 * @param out_size  Buffer size.
 * @return true if precompressed file exists.
 */
bool io_compress_precompressed(const char *path, io_encoding_t encoding,
                               char *out_path, size_t out_size);

#endif /* IOHTTP_STATIC_COMPRESS_H */

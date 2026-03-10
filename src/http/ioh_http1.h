/**
 * @file ioh_http1.h
 * @brief HTTP/1.1 parser wrapping picohttpparser with smuggling protection.
 */

#ifndef IOHTTP_HTTP_HTTP1_H
#define IOHTTP_HTTP_HTTP1_H

#include "http/ioh_request.h"
#include "http/ioh_response.h"

#include "http/picohttpparser.h"

#include <stddef.h>
#include <stdint.h>

/* Parse limits */
constexpr size_t IOH_HTTP1_MAX_HEADERS = 64;
constexpr size_t IOH_HTTP1_MAX_HEADER_SIZE = 8192;
constexpr size_t IOH_HTTP1_MAX_URI_SIZE = 4096;

/* Chunked decoder wrapper */
typedef struct {
    struct phr_chunked_decoder decoder;
    bool consume_trailer;
} ioh_chunked_decoder_t;

/**
 * Parse HTTP/1.1 request from buffer into ioh_request_t.
 * @param buf Raw request bytes.
 * @param len Number of bytes in buf.
 * @param req Request structure to populate.
 * @return >0 bytes consumed, -EAGAIN need more data, <0 error
 *         (-EINVAL malformed, -E2BIG too large).
 */
[[nodiscard]] int ioh_http1_parse_request(const uint8_t *buf, size_t len, ioh_request_t *req);

/**
 * Serialize HTTP/1.1 response to buffer.
 * @param resp Response to serialize.
 * @param buf  Output buffer.
 * @param buf_size Size of output buffer.
 * @return >0 bytes written, -ENOSPC buffer too small, <0 error.
 */
[[nodiscard]] int ioh_http1_serialize_response(const ioh_response_t *resp, uint8_t *buf,
                                              size_t buf_size);

/**
 * Initialize chunked decoder.
 * @param dec Decoder to initialize.
 */
void ioh_http1_chunked_init(ioh_chunked_decoder_t *dec);

/**
 * Decode chunked transfer encoding in-place.
 * @param dec Chunked decoder state.
 * @param buf Buffer containing chunked data (decoded in-place).
 * @param len In: bytes available, Out: bytes decoded.
 * @return >0 complete (trailing bytes), -EAGAIN need more, -EINVAL error.
 */
[[nodiscard]] int ioh_http1_chunked_decode(ioh_chunked_decoder_t *dec, uint8_t *buf, size_t *len);

#endif /* IOHTTP_HTTP_HTTP1_H */

/**
 * @file io_compress.c
 * @brief Compression middleware implementation.
 *
 * Supports gzip (via zlib) and brotli (via libbrotlienc) when available
 * at compile time. Always provides encoding negotiation and precompressed
 * file detection regardless of library availability.
 */

#include "static/io_compress.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#ifdef IOHTTP_HAVE_ZLIB
#include <zlib.h>
#endif

#ifdef IOHTTP_HAVE_BROTLI
#include <brotli/encode.h>
#endif

/* ---- Config defaults ---- */

void io_compress_config_init(io_compress_config_t *cfg)
{
    if (cfg == nullptr) {
        return;
    }
    cfg->enable_gzip = true;
    cfg->enable_brotli = true;
    cfg->enable_precompressed = true;
    cfg->min_size = 1024;
    cfg->gzip_level = 6;
    cfg->brotli_quality = 4;
}

/* ---- Accept-Encoding negotiation ---- */

/**
 * Parse a single encoding token and its q-value from a comma-separated list.
 * Advances *pos past the token (and trailing comma/whitespace).
 */
static bool parse_encoding_token(const char *header, size_t len, size_t *pos,
                                 const char **token, size_t *token_len,
                                 double *qvalue)
{
    /* skip leading whitespace and commas */
    while (*pos < len && (header[*pos] == ' ' || header[*pos] == ','
                          || header[*pos] == '\t')) {
        (*pos)++;
    }
    if (*pos >= len) {
        return false;
    }

    /* extract token name */
    *token = &header[*pos];
    size_t start = *pos;
    while (*pos < len && header[*pos] != ',' && header[*pos] != ';'
           && header[*pos] != ' ' && header[*pos] != '\t') {
        (*pos)++;
    }
    *token_len = *pos - start;
    *qvalue = 1.0;

    /* look for ;q= parameter */
    while (*pos < len && (header[*pos] == ' ' || header[*pos] == '\t')) {
        (*pos)++;
    }
    if (*pos < len && header[*pos] == ';') {
        (*pos)++;
        /* skip whitespace */
        while (*pos < len && (header[*pos] == ' ' || header[*pos] == '\t')) {
            (*pos)++;
        }
        if (*pos + 1 < len && (header[*pos] == 'q' || header[*pos] == 'Q')
            && header[*pos + 1] == '=') {
            *pos += 2;
            char buf[16];
            size_t bi = 0;
            while (*pos < len && header[*pos] != ',' && header[*pos] != ' '
                   && bi < sizeof(buf) - 1) {
                buf[bi++] = header[(*pos)++];
            }
            buf[bi] = '\0';
            *qvalue = strtod(buf, nullptr);
        }
    }

    /* skip to next comma or end */
    while (*pos < len && header[*pos] != ',') {
        (*pos)++;
    }

    return *token_len > 0;
}

io_encoding_t io_compress_negotiate(const char *accept_encoding)
{
    if (accept_encoding == nullptr || accept_encoding[0] == '\0') {
        return IO_ENCODING_NONE;
    }

    size_t len = strlen(accept_encoding);
    size_t pos = 0;

    double gzip_q = -1.0;
    double br_q = -1.0;
    double star_q = -1.0;
    bool identity_excluded = false;

    const char *token = nullptr;
    size_t token_len = 0;
    double qvalue = 1.0;

    while (parse_encoding_token(accept_encoding, len, &pos,
                                &token, &token_len, &qvalue)) {
        if (token_len == 4 && strncasecmp(token, "gzip", 4) == 0) {
            gzip_q = qvalue;
        } else if (token_len == 2 && strncasecmp(token, "br", 2) == 0) {
            br_q = qvalue;
        } else if (token_len == 1 && token[0] == '*') {
            star_q = qvalue;
        } else if (token_len == 8
                   && strncasecmp(token, "identity", 8) == 0) {
            if (qvalue == 0.0) {
                identity_excluded = true;
            }
        }
    }

    /* wildcard provides a default q for unmentioned encodings */
    if (star_q >= 0.0) {
        if (gzip_q < 0.0) {
            gzip_q = star_q;
        }
        if (br_q < 0.0) {
            br_q = star_q;
        }
    }

    /* exclude q=0 encodings */
    if (gzip_q == 0.0) {
        gzip_q = -1.0;
    }
    if (br_q == 0.0) {
        br_q = -1.0;
    }

    /* prefer brotli over gzip at equal q (higher compression ratio) */
    if (br_q > 0.0 && br_q >= gzip_q) {
        return IO_ENCODING_BROTLI;
    }
    if (gzip_q > 0.0) {
        return IO_ENCODING_GZIP;
    }

    (void)identity_excluded;
    return IO_ENCODING_NONE;
}

/* ---- Precompressed file detection ---- */

bool io_compress_precompressed(const char *path, io_encoding_t encoding,
                               char *out_path, size_t out_size)
{
    if (path == nullptr || out_path == nullptr || out_size == 0) {
        return false;
    }

    const char *suffix = nullptr;
    switch (encoding) {
    case IO_ENCODING_GZIP:
        suffix = ".gz";
        break;
    case IO_ENCODING_BROTLI:
        suffix = ".br";
        break;
    default:
        return false;
    }

    int written = snprintf(out_path, out_size, "%s%s", path, suffix);
    if (written < 0 || (size_t)written >= out_size) {
        return false;
    }

    struct stat st;
    return stat(out_path, &st) == 0 && S_ISREG(st.st_mode);
}

/* ---- Dynamic compression ---- */

#ifdef IOHTTP_HAVE_ZLIB
static int compress_gzip(const uint8_t *in, size_t in_len, int level,
                         uint8_t **out, size_t *out_len)
{
    /* worst case: deflateBound */
    uLong bound = deflateBound(nullptr, (uLong)in_len);
    uint8_t *buf = malloc(bound);
    if (buf == nullptr) {
        return -ENOMEM;
    }

    z_stream strm;
    memset(&strm, 0, sizeof(strm));

    /* MAX_WBITS + 16 = gzip format */
    int ret = deflateInit2(&strm, level, Z_DEFLATED, MAX_WBITS + 16, 8,
                           Z_DEFAULT_STRATEGY);
    if (ret != Z_OK) {
        free(buf);
        return -EIO;
    }

    strm.next_in = (Bytef *)(uintptr_t)in;
    strm.avail_in = (uInt)in_len;
    strm.next_out = buf;
    strm.avail_out = (uInt)bound;

    ret = deflate(&strm, Z_FINISH);
    if (ret != Z_STREAM_END) {
        deflateEnd(&strm);
        free(buf);
        return -EIO;
    }

    *out_len = strm.total_out;
    *out = buf;
    deflateEnd(&strm);
    return 0;
}
#endif /* IOHTTP_HAVE_ZLIB */

#ifdef IOHTTP_HAVE_BROTLI
static int compress_brotli(const uint8_t *in, size_t in_len, int quality,
                           uint8_t **out, size_t *out_len)
{
    size_t max_len = BrotliEncoderMaxCompressedSize(in_len);
    if (max_len == 0) {
        max_len = in_len + 1024;
    }
    uint8_t *buf = malloc(max_len);
    if (buf == nullptr) {
        return -ENOMEM;
    }

    size_t encoded_size = max_len;
    BROTLI_BOOL ok = BrotliEncoderCompress(
        quality, BROTLI_DEFAULT_WINDOW, BROTLI_DEFAULT_MODE,
        in_len, in, &encoded_size, buf);
    if (!ok) {
        free(buf);
        return -EIO;
    }

    *out = buf;
    *out_len = encoded_size;
    return 0;
}
#endif /* IOHTTP_HAVE_BROTLI */

int io_compress_response(const io_compress_config_t *cfg,
                         const io_request_t *req,
                         io_response_t *resp)
{
    if (cfg == nullptr || req == nullptr || resp == nullptr) {
        return -EINVAL;
    }

    const char *ae = io_request_header(req, "Accept-Encoding");
    io_encoding_t enc = io_compress_negotiate(ae);

    /* always set Vary so caches know about encoding negotiation */
    int rc = io_response_set_header(resp, "Vary", "Accept-Encoding");
    if (rc < 0) {
        return rc;
    }

    /* skip if body too small */
    if (resp->body_len < cfg->min_size) {
        return 0;
    }

    if (enc == IO_ENCODING_NONE) {
        return 0;
    }

    uint8_t *compressed = nullptr;
    size_t compressed_len = 0;

    if (enc == IO_ENCODING_GZIP && cfg->enable_gzip) {
#ifdef IOHTTP_HAVE_ZLIB
        rc = compress_gzip(resp->body, resp->body_len, cfg->gzip_level,
                           &compressed, &compressed_len);
        if (rc < 0) {
            return rc;
        }
        rc = io_response_set_header(resp, "Content-Encoding", "gzip");
#else
        return 0; /* no zlib — skip silently */
#endif
    } else if (enc == IO_ENCODING_BROTLI && cfg->enable_brotli) {
#ifdef IOHTTP_HAVE_BROTLI
        rc = compress_brotli(resp->body, resp->body_len, cfg->brotli_quality,
                             &compressed, &compressed_len);
        if (rc < 0) {
            return rc;
        }
        rc = io_response_set_header(resp, "Content-Encoding", "br");
#else
        return 0; /* no brotli — skip silently */
#endif
    } else {
        return 0;
    }

    if (rc < 0) {
        free(compressed);
        return rc;
    }

    /* replace body with compressed data */
    rc = io_response_set_body(resp, compressed, compressed_len);
    free(compressed);
    return rc;
}

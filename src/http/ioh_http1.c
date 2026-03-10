/**
 * @file ioh_http1.c
 * @brief HTTP/1.1 parser implementation wrapping picohttpparser.
 */

#include "http/ioh_http1.h"
#include "http/picohttpparser.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

/**
 * Check if a Content-Length value contains only digits.
 * Returns true if valid, false if non-digit characters present.
 */
static bool is_valid_content_length(const char *value, size_t len)
{
    if (len == 0) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        if (value[i] < '0' || value[i] > '9') {
            return false;
        }
    }
    return true;
}

/**
 * Parse a Content-Length value to size_t.
 * Assumes the value has already been validated with is_valid_content_length().
 */
static size_t parse_content_length(const char *value, size_t len)
{
    size_t result = 0;
    for (size_t i = 0; i < len; i++) {
        result = result * 10 + (size_t)(value[i] - '0');
    }
    return result;
}

int ioh_http1_parse_request(const uint8_t *buf, size_t len, ioh_request_t *req)
{
    if (buf == nullptr || req == nullptr) {
        return -EINVAL;
    }

    if (len == 0) {
        return -EAGAIN;
    }

    const char *method = nullptr;
    size_t method_len = 0;
    const char *path = nullptr;
    size_t path_len = 0;
    int minor_version = 0;
    struct phr_header headers[IOH_HTTP1_MAX_HEADERS];
    size_t num_headers = IOH_HTTP1_MAX_HEADERS;

    int rc = phr_parse_request((const char *)buf, len, &method, &method_len, &path, &path_len,
                               &minor_version, headers, &num_headers, 0);

    if (rc == -2) {
        return -EAGAIN;
    }
    if (rc == -1) {
        return -EINVAL;
    }

    /* Check URI size limit */
    if (path_len > IOH_HTTP1_MAX_URI_SIZE) {
        return -E2BIG;
    }

    /* Check header count limit */
    if (num_headers > IOH_HTTP1_MAX_HEADERS) {
        return -E2BIG;
    }

    /* Initialize request */
    ioh_request_init(req);

    /* Parse method */
    req->method = ioh_method_parse(method, method_len);

    /* HTTP version */
    req->http_version_major = 1;
    req->http_version_minor = (uint8_t)minor_version;

    /* Default keep-alive: true for HTTP/1.1, false for HTTP/1.0 */
    req->keep_alive = (minor_version >= 1);

    /* Split path from query string at '?' */
    const char *qmark = memchr(path, '?', path_len);
    if (qmark != nullptr) {
        req->path = path;
        req->path_len = (size_t)(qmark - path);
        req->query = qmark + 1;
        req->query_len = path_len - req->path_len - 1;
    } else {
        req->path = path;
        req->path_len = path_len;
    }

    /*
     * Request smuggling protection: scan headers for violations.
     * We do a single pass detecting issues before populating req->headers.
     */
    bool has_content_length = false;
    bool has_transfer_encoding = false;
    bool has_host = false;

    for (size_t i = 0; i < num_headers; i++) {
        /* obs-fold: name==NULL means continuation line */
        if (headers[i].name == nullptr) {
            return -EINVAL;
        }

        size_t nlen = headers[i].name_len;
        const char *name = headers[i].name;
        const char *value = headers[i].value;
        size_t vlen = headers[i].value_len;

        if (nlen == 14 && strncasecmp(name, "Content-Length", 14) == 0) {
            /* Reject duplicate Content-Length */
            if (has_content_length) {
                return -EINVAL;
            }
            has_content_length = true;

            /* Reject non-digit characters in Content-Length */
            if (!is_valid_content_length(value, vlen)) {
                return -EINVAL;
            }

            req->content_length = parse_content_length(value, vlen);
        }

        if (nlen == 17 && strncasecmp(name, "Transfer-Encoding", 17) == 0) {
            has_transfer_encoding = true;

            /*
             * Reject chunked TE not as last encoding.
             * If "chunked" appears but is not the final token, reject.
             */
            const char *chunked = "chunked";
            size_t chunked_len = 7;
            if (vlen >= chunked_len) {
                /* Find "chunked" in the value */
                const char *found = nullptr;
                for (size_t j = 0; j + chunked_len <= vlen; j++) {
                    if (strncasecmp(value + j, chunked, chunked_len) == 0) {
                        found = value + j;
                        break;
                    }
                }
                if (found != nullptr) {
                    /* chunked must be at the end (after trimming whitespace) */
                    const char *after = found + chunked_len;
                    const char *vend = value + vlen;
                    while (after < vend && (*after == ' ' || *after == '\t')) {
                        after++;
                    }
                    if (after != vend) {
                        return -EINVAL;
                    }
                }
            }
        }

        if (nlen == 4 && strncasecmp(name, "Host", 4) == 0) {
            has_host = true;
        }
    }

    /* Reject requests with both Content-Length and Transfer-Encoding */
    if (has_content_length && has_transfer_encoding) {
        return -EINVAL;
    }

    /* Require Host header for HTTP/1.1 */
    if (minor_version >= 1 && !has_host) {
        return -EINVAL;
    }

    /* Copy headers to request and extract special headers */
    for (size_t i = 0; i < num_headers; i++) {
        if (i >= IOH_MAX_HEADERS) {
            break;
        }

        req->headers[i].name = headers[i].name;
        req->headers[i].name_len = headers[i].name_len;
        req->headers[i].value = headers[i].value;
        req->headers[i].value_len = headers[i].value_len;
        req->header_count++;

        size_t nlen = headers[i].name_len;
        const char *name = headers[i].name;
        const char *value = headers[i].value;

        /* Extract Content-Type */
        if (nlen == 12 && strncasecmp(name, "Content-Type", 12) == 0) {
            req->content_type = value;
        }

        /* Extract Host */
        if (nlen == 4 && strncasecmp(name, "Host", 4) == 0) {
            req->host = value;
        }

        /* Check Connection header for keep-alive override */
        if (nlen == 10 && strncasecmp(name, "Connection", 10) == 0) {
            if (headers[i].value_len == 5 && strncasecmp(value, "close", 5) == 0) {
                req->keep_alive = false;
            } else if (headers[i].value_len == 10 && strncasecmp(value, "keep-alive", 10) == 0) {
                req->keep_alive = true;
            }
        }
    }

    return rc;
}

int ioh_http1_serialize_response(const ioh_response_t *resp, uint8_t *buf, size_t buf_size)
{
    if (resp == nullptr || buf == nullptr || buf_size == 0) {
        return -EINVAL;
    }

    const char *status_text = ioh_status_text(resp->status);
    char *out = (char *)buf;
    size_t remaining = buf_size;
    int written = 0;

    /* Status line: HTTP/1.1 STATUS TEXT\r\n */
    written = snprintf(out, remaining, "HTTP/1.1 %u %s\r\n", resp->status, status_text);
    if (written < 0 || (size_t)written >= remaining) {
        return -ENOSPC;
    }
    out += written;
    remaining -= (size_t)written;

    /* Headers */
    for (uint32_t i = 0; i < resp->header_count; i++) {
        written = snprintf(out, remaining, "%.*s: %.*s\r\n", (int)resp->headers[i].name_len,
                           resp->headers[i].name, (int)resp->headers[i].value_len,
                           resp->headers[i].value);
        if (written < 0 || (size_t)written >= remaining) {
            return -ENOSPC;
        }
        out += written;
        remaining -= (size_t)written;
    }

    /* Auto-add Content-Length if body present and not chunked */
    if (resp->body_len > 0 && !resp->chunked) {
        written = snprintf(out, remaining, "Content-Length: %zu\r\n", resp->body_len);
        if (written < 0 || (size_t)written >= remaining) {
            return -ENOSPC;
        }
        out += written;
        remaining -= (size_t)written;
    }

    /* Header/body separator */
    if (remaining < 2) {
        return -ENOSPC;
    }
    memcpy(out, "\r\n", 2);
    out += 2;
    remaining -= 2;

    /* Body */
    if (resp->body_len > 0 && resp->body != nullptr) {
        if (resp->body_len > remaining) {
            return -ENOSPC;
        }
        memcpy(out, resp->body, resp->body_len);
        out += resp->body_len;
    }

    return (int)(out - (char *)buf);
}

void ioh_http1_chunked_init(ioh_chunked_decoder_t *dec)
{
    if (dec == nullptr) {
        return;
    }
    memset(dec, 0, sizeof(*dec));
    dec->decoder.consume_trailer = 1;
    dec->consume_trailer = true;
}

int ioh_http1_chunked_decode(ioh_chunked_decoder_t *dec, uint8_t *buf, size_t *len)
{
    if (dec == nullptr || buf == nullptr || len == nullptr) {
        return -EINVAL;
    }

    ssize_t rc = phr_decode_chunked(&dec->decoder, (char *)buf, len);

    if (rc >= 0) {
        return (int)rc; /* complete, rc = trailing bytes */
    }
    if (rc == -2) {
        return -EAGAIN; /* need more data */
    }
    return -EINVAL; /* error */
}

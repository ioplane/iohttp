/**
 * @file io_response.c
 * @brief Protocol-independent HTTP response builder implementation.
 */

#include "http/io_response.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

constexpr uint32_t IO_RESPONSE_INITIAL_HEADERS = 16;
constexpr size_t IO_RESPONSE_INITIAL_BODY = 1024;

int io_response_init(io_response_t *resp)
{
    if (resp == nullptr) {
        return -EINVAL;
    }

    memset(resp, 0, sizeof(*resp));
    resp->status = 200;

    resp->headers = calloc(IO_RESPONSE_INITIAL_HEADERS, sizeof(*resp->headers));
    if (resp->headers == nullptr) {
        return -ENOMEM;
    }
    resp->header_capacity = IO_RESPONSE_INITIAL_HEADERS;

    resp->body = malloc(IO_RESPONSE_INITIAL_BODY);
    if (resp->body == nullptr) {
        free(resp->headers);
        resp->headers = nullptr;
        return -ENOMEM;
    }
    resp->body_capacity = IO_RESPONSE_INITIAL_BODY;

    return 0;
}

void io_response_reset(io_response_t *resp)
{
    if (resp == nullptr) {
        return;
    }

    /* free copied header name/value strings */
    for (uint32_t i = 0; i < resp->header_count; i++) {
        free((void *)resp->headers[i].name);
        free((void *)resp->headers[i].value);
    }

    resp->status = 200;
    resp->header_count = 0;
    resp->body_len = 0;
    resp->headers_sent = false;
    resp->chunked = false;
}

void io_response_destroy(io_response_t *resp)
{
    if (resp == nullptr) {
        return;
    }

    for (uint32_t i = 0; i < resp->header_count; i++) {
        free((void *)resp->headers[i].name);
        free((void *)resp->headers[i].value);
    }

    free(resp->headers);
    free(resp->body);
    memset(resp, 0, sizeof(*resp));
}

int io_response_set_header(io_response_t *resp, const char *name, const char *value)
{
    if (resp == nullptr || name == nullptr || value == nullptr) {
        return -EINVAL;
    }

    size_t name_len = strnlen(name, IO_MAX_HEADER_SIZE);
    size_t value_len = strnlen(value, IO_MAX_HEADER_SIZE);

    /* check if header already exists (replace) */
    for (uint32_t i = 0; i < resp->header_count; i++) {
        if (resp->headers[i].name_len == name_len &&
            strncasecmp(resp->headers[i].name, name, name_len) == 0) {
            char *new_value = strndup(value, value_len);
            if (new_value == nullptr) {
                return -ENOMEM;
            }
            free((void *)resp->headers[i].value);
            resp->headers[i].value = new_value;
            resp->headers[i].value_len = value_len;
            return 0;
        }
    }

    /* grow if needed */
    if (resp->header_count >= resp->header_capacity) {
        uint32_t new_cap = resp->header_capacity * 2;
        io_header_t *tmp = realloc(resp->headers, new_cap * sizeof(*tmp));
        if (tmp == nullptr) {
            return -ENOMEM;
        }
        resp->headers = tmp;
        resp->header_capacity = new_cap;
    }

    char *dup_name = strndup(name, name_len);
    if (dup_name == nullptr) {
        return -ENOMEM;
    }

    char *dup_value = strndup(value, value_len);
    if (dup_value == nullptr) {
        free(dup_name);
        return -ENOMEM;
    }

    io_header_t *hdr = &resp->headers[resp->header_count];
    hdr->name = dup_name;
    hdr->name_len = name_len;
    hdr->value = dup_value;
    hdr->value_len = value_len;
    resp->header_count++;

    return 0;
}

int io_response_add_header(io_response_t *resp, const char *name, const char *value)
{
    if (resp == nullptr || name == nullptr || value == nullptr) {
        return -EINVAL;
    }

    size_t name_len = strnlen(name, IO_MAX_HEADER_SIZE);
    size_t value_len = strnlen(value, IO_MAX_HEADER_SIZE);

    /* grow if needed */
    if (resp->header_count >= resp->header_capacity) {
        uint32_t new_cap = resp->header_capacity * 2;
        io_header_t *tmp = realloc(resp->headers, new_cap * sizeof(*tmp));
        if (tmp == nullptr) {
            return -ENOMEM;
        }
        resp->headers = tmp;
        resp->header_capacity = new_cap;
    }

    char *dup_name = strndup(name, name_len);
    if (dup_name == nullptr) {
        return -ENOMEM;
    }

    char *dup_value = strndup(value, value_len);
    if (dup_value == nullptr) {
        free(dup_name);
        return -ENOMEM;
    }

    io_header_t *hdr = &resp->headers[resp->header_count];
    hdr->name = dup_name;
    hdr->name_len = name_len;
    hdr->value = dup_value;
    hdr->value_len = value_len;
    resp->header_count++;

    return 0;
}

int io_response_set_body(io_response_t *resp, const uint8_t *body, size_t len)
{
    if (resp == nullptr) {
        return -EINVAL;
    }
    if (len == 0) {
        resp->body_len = 0;
        return 0;
    }
    if (body == nullptr) {
        return -EINVAL;
    }

    /* grow body buffer if needed */
    if (len > resp->body_capacity) {
        size_t new_cap = len;
        /* round up to next power of two for reuse */
        if (new_cap < resp->body_capacity * 2) {
            new_cap = resp->body_capacity * 2;
        }
        uint8_t *tmp = realloc(resp->body, new_cap);
        if (tmp == nullptr) {
            return -ENOMEM;
        }
        resp->body = tmp;
        resp->body_capacity = new_cap;
    }

    memcpy(resp->body, body, len);
    resp->body_len = len;
    return 0;
}

int io_respond(io_response_t *resp, uint16_t status, const char *content_type, const uint8_t *body,
               size_t body_len)
{
    if (resp == nullptr) {
        return -EINVAL;
    }

    resp->status = status;

    if (content_type != nullptr) {
        int rc = io_response_set_header(resp, "Content-Type", content_type);
        if (rc != 0) {
            return rc;
        }
    }

    if (body != nullptr && body_len > 0) {
        int rc = io_response_set_body(resp, body, body_len);
        if (rc != 0) {
            return rc;
        }
    }

    return 0;
}

int io_respond_json(io_response_t *resp, uint16_t status, const char *json)
{
    if (resp == nullptr || json == nullptr) {
        return -EINVAL;
    }

    size_t len = strnlen(json, 1024 * 1024); /* 1 MiB sanity limit */
    return io_respond(resp, status, "application/json", (const uint8_t *)json, len);
}

const char *io_status_text(uint16_t status)
{
    switch (status) {
    case 100:
        return "Continue";
    case 101:
        return "Switching Protocols";
    case 200:
        return "OK";
    case 201:
        return "Created";
    case 202:
        return "Accepted";
    case 204:
        return "No Content";
    case 206:
        return "Partial Content";
    case 301:
        return "Moved Permanently";
    case 302:
        return "Found";
    case 304:
        return "Not Modified";
    case 307:
        return "Temporary Redirect";
    case 308:
        return "Permanent Redirect";
    case 400:
        return "Bad Request";
    case 401:
        return "Unauthorized";
    case 403:
        return "Forbidden";
    case 404:
        return "Not Found";
    case 405:
        return "Method Not Allowed";
    case 406:
        return "Not Acceptable";
    case 408:
        return "Request Timeout";
    case 409:
        return "Conflict";
    case 411:
        return "Length Required";
    case 413:
        return "Content Too Large";
    case 414:
        return "URI Too Long";
    case 415:
        return "Unsupported Media Type";
    case 416:
        return "Range Not Satisfiable";
    case 422:
        return "Unprocessable Content";
    case 429:
        return "Too Many Requests";
    case 500:
        return "Internal Server Error";
    case 501:
        return "Not Implemented";
    case 502:
        return "Bad Gateway";
    case 503:
        return "Service Unavailable";
    case 504:
        return "Gateway Timeout";
    default:
        return "Unknown";
    }
}

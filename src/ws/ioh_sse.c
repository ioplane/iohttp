/**
 * @file ioh_sse.c
 * @brief Server-Sent Events (SSE) formatting and stream management.
 */

#include "ws/ioh_sse.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

constexpr size_t IOH_SSE_INITIAL_BUF = 1024;

static const char IOH_SSE_HEADERS[] = "Content-Type: text/event-stream; charset=utf-8\r\n"
                                     "Cache-Control: no-cache\r\n"
                                     "Connection: keep-alive\r\n"
                                     "\r\n";

/* ---- Internal helpers ---- */

static int sse_ensure_capacity(ioh_sse_stream_t *stream, size_t additional)
{
    size_t needed = stream->buf_len + additional;
    if (needed <= stream->buf_capacity) {
        return 0;
    }

    size_t new_cap = stream->buf_capacity;
    if (new_cap == 0) {
        new_cap = IOH_SSE_INITIAL_BUF;
    }
    while (new_cap < needed) {
        new_cap *= 2;
    }

    uint8_t *tmp = realloc(stream->buf, new_cap);
    if (tmp == nullptr) {
        return -ENOMEM;
    }
    stream->buf = tmp;
    stream->buf_capacity = new_cap;
    return 0;
}

static int sse_append(ioh_sse_stream_t *stream, const char *data, size_t len)
{
    int rc = sse_ensure_capacity(stream, len);
    if (rc != 0) {
        return rc;
    }
    memcpy(stream->buf + stream->buf_len, data, len);
    stream->buf_len += len;
    return 0;
}

static int sse_append_str(ioh_sse_stream_t *stream, const char *str)
{
    return sse_append(stream, str, strlen(str));
}

/* ---- SSE stream lifecycle ---- */

int ioh_sse_stream_init(ioh_sse_stream_t *stream)
{
    if (stream == nullptr) {
        return -EINVAL;
    }

    memset(stream, 0, sizeof(*stream));

    stream->buf = malloc(IOH_SSE_INITIAL_BUF);
    if (stream->buf == nullptr) {
        return -ENOMEM;
    }
    stream->buf_capacity = IOH_SSE_INITIAL_BUF;

    return 0;
}

void ioh_sse_stream_destroy(ioh_sse_stream_t *stream)
{
    if (stream == nullptr) {
        return;
    }

    free(stream->buf);
    free(stream->last_event_id);
    memset(stream, 0, sizeof(*stream));
}

/* ---- SSE formatting ---- */

int ioh_sse_format_headers(uint8_t *buf, size_t buf_size)
{
    if (buf == nullptr) {
        return -EINVAL;
    }

    size_t len = sizeof(IOH_SSE_HEADERS) - 1;
    if (buf_size < len) {
        return -ENOSPC;
    }

    memcpy(buf, IOH_SSE_HEADERS, len);
    return (int)len;
}

int ioh_sse_format_event(ioh_sse_stream_t *stream, const ioh_sse_event_t *event)
{
    if (stream == nullptr || event == nullptr || event->data == nullptr) {
        return -EINVAL;
    }

    int rc;

    /* event type field */
    if (event->event != nullptr) {
        rc = sse_append_str(stream, "event: ");
        if (rc != 0) {
            return rc;
        }
        rc = sse_append_str(stream, event->event);
        if (rc != 0) {
            return rc;
        }
        rc = sse_append(stream, "\n", 1);
        if (rc != 0) {
            return rc;
        }
    }

    /* id field */
    if (event->id != nullptr) {
        rc = sse_append_str(stream, "id: ");
        if (rc != 0) {
            return rc;
        }
        rc = sse_append_str(stream, event->id);
        if (rc != 0) {
            return rc;
        }
        rc = sse_append(stream, "\n", 1);
        if (rc != 0) {
            return rc;
        }

        /* track last event ID */
        free(stream->last_event_id);
        stream->last_event_id = strdup(event->id);
        if (stream->last_event_id == nullptr) {
            return -ENOMEM;
        }
    }

    /* retry field */
    if (event->retry_ms > 0) {
        char retry_buf[32];
        int n = snprintf(retry_buf, sizeof(retry_buf), "retry: %u\n", (unsigned)event->retry_ms);
        if (n < 0) {
            return -EINVAL;
        }
        rc = sse_append(stream, retry_buf, (size_t)n);
        if (rc != 0) {
            return rc;
        }
    }

    /* data field — split on newlines */
    const char *p = event->data;
    const char *end = p + strlen(p);

    while (p < end) {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        size_t line_len = (nl != nullptr) ? (size_t)(nl - p) : (size_t)(end - p);

        rc = sse_append_str(stream, "data: ");
        if (rc != 0) {
            return rc;
        }
        rc = sse_append(stream, p, line_len);
        if (rc != 0) {
            return rc;
        }
        rc = sse_append(stream, "\n", 1);
        if (rc != 0) {
            return rc;
        }

        p += line_len;
        if (nl != nullptr) {
            p++; /* skip the newline character */
        }
    }

    /* empty data case (data is "") */
    if (p == end && strlen(event->data) == 0) {
        rc = sse_append_str(stream, "data: \n");
        if (rc != 0) {
            return rc;
        }
    }

    /* terminating blank line */
    rc = sse_append(stream, "\n", 1);
    if (rc != 0) {
        return rc;
    }

    return 0;
}

int ioh_sse_format_comment(ioh_sse_stream_t *stream, const char *comment)
{
    if (stream == nullptr || comment == nullptr) {
        return -EINVAL;
    }

    int rc;

    rc = sse_append_str(stream, ": ");
    if (rc != 0) {
        return rc;
    }
    rc = sse_append_str(stream, comment);
    if (rc != 0) {
        return rc;
    }
    rc = sse_append(stream, "\n\n", 2);
    if (rc != 0) {
        return rc;
    }

    return 0;
}

void ioh_sse_stream_flush(ioh_sse_stream_t *stream, const uint8_t **data_out, size_t *len_out)
{
    if (stream == nullptr) {
        if (data_out != nullptr) {
            *data_out = nullptr;
        }
        if (len_out != nullptr) {
            *len_out = 0;
        }
        return;
    }

    if (data_out != nullptr) {
        *data_out = stream->buf;
    }
    if (len_out != nullptr) {
        *len_out = stream->buf_len;
    }

    stream->buf_len = 0;
}

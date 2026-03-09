/**
 * @file io_http2.c
 * @brief HTTP/2 session management — nghttp2-backed implementation.
 *
 * Buffer-based I/O: no sockets, no threads. Application feeds raw bytes
 * via io_http2_on_recv(), retrieves output via io_http2_flush().
 */

#include "http/io_http2.h"

#include "core/io_ctx.h"

#include <errno.h>
#include <stdckdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <nghttp2/nghttp2.h>

/* ---- Constants ---- */

constexpr uint32_t H2_DEFAULT_MAX_CONCURRENT_STREAMS = 100;
constexpr uint32_t H2_DEFAULT_INITIAL_WINDOW_SIZE = 65535;
constexpr uint32_t H2_DEFAULT_MAX_FRAME_SIZE = 16384;
constexpr uint32_t H2_DEFAULT_MAX_HEADER_LIST_SIZE = 8192;
constexpr uint32_t H2_DEFAULT_MAX_RST_PER_SEC = 100;
constexpr size_t H2_OUTPUT_BUF_SIZE = 65536;

/* ---- Per-stream string arena ---- */

/* nghttp2 header name/value pointers are only valid during callbacks.
 * We copy all strings into a simple arena that lives with the stream. */
typedef struct {
    char *buf;
    size_t len;
    size_t cap;
} h2_arena_t;

static char *arena_copy(h2_arena_t *a, const char *src, size_t len)
{
    /* +1 for NUL terminator */
    if (a->len + len + 1 > a->cap) {
        size_t new_cap = (a->cap == 0) ? 4096 : a->cap * 2;
        while (new_cap < a->len + len + 1) {
            new_cap *= 2;
        }
        char *new_buf = realloc(a->buf, new_cap);
        if (new_buf == nullptr) {
            return nullptr;
        }
        a->buf = new_buf;
        a->cap = new_cap;
    }
    char *dst = a->buf + a->len;
    memcpy(dst, src, len);
    dst[len] = '\0';
    a->len += len + 1;
    return dst;
}

/* ---- Response data provider ---- */

typedef struct h2_resp_data {
    uint8_t *body; /* owned — transferred from io_response_t in submit_response() */
    size_t body_len;
    size_t offset;
} h2_resp_data_t;

/* ---- Per-stream data ---- */

typedef struct h2_stream_data h2_stream_data_t;

struct h2_stream_data {
    io_request_t request;
    h2_arena_t arena;  /* owns all string copies for header names/values/path/etc */
    uint8_t *body_buf; /* dynamically allocated body accumulator */
    size_t body_cap;
    size_t body_len;
    int32_t stream_id;
    bool headers_done;         /* HEADERS frame fully received */
    h2_resp_data_t *resp_data; /* response data provider — freed with stream */
    h2_stream_data_t *next;    /* intrusive list for session-level cleanup */
};

/* ---- Session (opaque) ---- */

struct io_http2_session {
    nghttp2_session *ng_session;
    io_http2_config_t config;
    io_http2_on_request_fn on_request;
    void *user_data;

    /* output buffer for flush() */
    uint8_t *out_buf;
    size_t out_len;
    size_t out_cap;

    /* RST_STREAM rate tracking (Rapid Reset protection) */
    uint32_t rst_count;
    time_t rst_window_start;

    /* GOAWAY state */
    bool goaway_sent;

    /* All active streams — for cleanup on session destroy */
    h2_stream_data_t *streams;
};

/* ---- Helpers ---- */

static h2_stream_data_t *stream_data_new(int32_t stream_id)
{
    h2_stream_data_t *sd = calloc(1, sizeof(*sd));
    if (sd == nullptr) {
        return nullptr;
    }
    io_request_init(&sd->request);
    sd->stream_id = stream_id;
    sd->request.http_version_major = 2;
    sd->request.http_version_minor = 0;
    sd->request.keep_alive = true; /* HTTP/2 is always persistent */
    return sd;
}

static void stream_data_free(h2_stream_data_t *sd)
{
    if (sd == nullptr) {
        return;
    }
    if (sd->resp_data != nullptr) {
        free((void *)sd->resp_data->body); /* owns the transferred response body */
        free(sd->resp_data);
    }
    free(sd->arena.buf);
    free(sd->body_buf);
    free(sd);
}

static io_method_t parse_method(const char *method, size_t len)
{
    return io_method_parse(method, len);
}

static int output_buf_append(io_http2_session_t *session, const uint8_t *data, size_t len)
{
    if (session->out_len + len > session->out_cap) {
        size_t new_cap = (session->out_cap == 0) ? H2_OUTPUT_BUF_SIZE : session->out_cap * 2;
        while (new_cap < session->out_len + len) {
            new_cap *= 2;
        }
        uint8_t *new_buf = realloc(session->out_buf, new_cap);
        if (new_buf == nullptr) {
            return -ENOMEM;
        }
        session->out_buf = new_buf;
        session->out_cap = new_cap;
    }
    memcpy(session->out_buf + session->out_len, data, len);
    session->out_len += len;
    return 0;
}

/* ---- Response data provider callback ---- */

static nghttp2_ssize resp_data_read_cb(nghttp2_session *session, int32_t stream_id, uint8_t *buf,
                                       size_t length, uint32_t *data_flags,
                                       nghttp2_data_source *source, void *user_data)
{
    (void)session;
    (void)stream_id;
    (void)user_data;

    h2_resp_data_t *rd = source->ptr;
    size_t remaining = rd->body_len - rd->offset;
    size_t n = (length < remaining) ? length : remaining;

    if (n > 0) {
        memcpy(buf, rd->body + rd->offset, n);
        rd->offset += n;
    }

    if (rd->offset >= rd->body_len) {
        *data_flags |= NGHTTP2_DATA_FLAG_EOF;
    }

    return (nghttp2_ssize)n;
}

/* Helper: submit a response for a stream */
static int submit_response(io_http2_session_t *session, int32_t stream_id, io_response_t *resp)
{
    /* Build :status pseudo-header */
    char status_str[4];
    int slen = snprintf(status_str, sizeof(status_str), "%u", resp->status);
    if (slen < 0 || (size_t)slen >= sizeof(status_str)) {
        return -EINVAL;
    }

    /* Count total headers: :status + response headers */
    size_t nva_count = 1 + resp->header_count;
    nghttp2_nv *nva = calloc(nva_count, sizeof(*nva));
    if (nva == nullptr) {
        return -ENOMEM;
    }

    /* :status */
    nva[0] = (nghttp2_nv){
        .name = (uint8_t *)":status",
        .namelen = 7,
        .value = (uint8_t *)status_str,
        .valuelen = (size_t)slen,
        .flags = NGHTTP2_NV_FLAG_NONE,
    };

    /* Copy response headers — HTTP/2 requires lowercase header names.
     * We lowercase in-place in a temporary buffer and let nghttp2 copy. */
    char **lc_names = nullptr;
    if (resp->header_count > 0) {
        lc_names = calloc(resp->header_count, sizeof(*lc_names));
        if (lc_names == nullptr) {
            free(nva);
            return -ENOMEM;
        }
    }

    for (uint32_t i = 0; i < resp->header_count; i++) {
        lc_names[i] = malloc(resp->headers[i].name_len + 1);
        if (lc_names[i] == nullptr) {
            for (uint32_t j = 0; j < i; j++) {
                free(lc_names[j]);
            }
            free(lc_names);
            free(nva);
            return -ENOMEM;
        }
        for (size_t c = 0; c < resp->headers[i].name_len; c++) {
            char ch = resp->headers[i].name[c];
            lc_names[i][c] = (ch >= 'A' && ch <= 'Z') ? (char)(ch + 32) : ch;
        }
        lc_names[i][resp->headers[i].name_len] = '\0';

        nva[1 + i] = (nghttp2_nv){
            .name = (uint8_t *)lc_names[i],
            .namelen = resp->headers[i].name_len,
            .value = (uint8_t *)resp->headers[i].value,
            .valuelen = resp->headers[i].value_len,
            .flags = NGHTTP2_NV_FLAG_NONE, /* let nghttp2 copy everything */
        };
    }

    int rv;
    if (resp->body != nullptr && resp->body_len > 0) {
        /* Allocate response data context — freed when stream closes */
        h2_resp_data_t *rd = calloc(1, sizeof(*rd));
        if (rd == nullptr) {
            rv = -ENOMEM;
            goto cleanup;
        }
        /* Transfer body ownership: rd takes over resp's body pointer
         * so it remains valid when nghttp2 calls resp_data_read_cb
         * after io_response_destroy() frees resp. */
        rd->body = resp->body;
        rd->body_len = resp->body_len;
        rd->offset = 0;
        resp->body = nullptr; /* prevent double-free in io_response_destroy */
        resp->body_len = 0;
        resp->body_capacity = 0;

        nghttp2_data_provider2 data_prd = {
            .source = {.ptr = rd},
            .read_callback = resp_data_read_cb,
        };

        rv = nghttp2_submit_response2(session->ng_session, stream_id, nva, nva_count, &data_prd);
        if (rv != 0) {
            free(rd->body);
            free(rd);
        } else {
            /* Store rd in stream data so it is freed when the stream closes */
            h2_stream_data_t *sd =
                nghttp2_session_get_stream_user_data(session->ng_session, stream_id);
            if (sd == nullptr) {
                free(rd->body);
                free(rd);
            } else {
                sd->resp_data = rd;
            }
        }
    } else {
        rv = nghttp2_submit_response2(session->ng_session, stream_id, nva, nva_count, nullptr);
    }

cleanup:
    /* Free temporary lowercase name copies */
    if (lc_names != nullptr) {
        for (uint32_t i = 0; i < resp->header_count; i++) {
            free(lc_names[i]);
        }
        free(lc_names);
    }
    free(nva);
    return (rv == 0) ? 0 : ((rv == -ENOMEM) ? -ENOMEM : -EIO);
}

/* ---- nghttp2 callbacks ---- */

static int on_begin_headers_cb(nghttp2_session *session, const nghttp2_frame *frame,
                               void *user_data)
{
    io_http2_session_t *h2 = user_data;

    if (frame->hd.type != NGHTTP2_HEADERS || frame->headers.cat != NGHTTP2_HCAT_REQUEST) {
        return 0;
    }

    h2_stream_data_t *sd = stream_data_new(frame->hd.stream_id);
    if (sd == nullptr) {
        return NGHTTP2_ERR_CALLBACK_FAILURE;
    }

    /* Track in session list for cleanup on destroy */
    sd->next = h2->streams;
    h2->streams = sd;

    nghttp2_session_set_stream_user_data(session, frame->hd.stream_id, sd);
    return 0;
}

static int on_header_cb(nghttp2_session *session, const nghttp2_frame *frame, const uint8_t *name,
                        size_t namelen, const uint8_t *value, size_t valuelen, uint8_t flags,
                        void *user_data)
{
    (void)flags;
    (void)user_data;

    if (frame->hd.type != NGHTTP2_HEADERS || frame->headers.cat != NGHTTP2_HCAT_REQUEST) {
        return 0;
    }

    h2_stream_data_t *sd = nghttp2_session_get_stream_user_data(session, frame->hd.stream_id);
    if (sd == nullptr) {
        return 0;
    }

    io_request_t *req = &sd->request;

    /* Copy value into arena (all headers need owned copies) */
    char *val_copy = arena_copy(&sd->arena, (const char *)value, valuelen);
    if (val_copy == nullptr) {
        return NGHTTP2_ERR_CALLBACK_FAILURE;
    }

    /* Pseudo-headers */
    if (namelen > 0 && name[0] == ':') {
        if (namelen == 7 && memcmp(name, ":method", 7) == 0) {
            req->method = parse_method(val_copy, valuelen);
        } else if (namelen == 5 && memcmp(name, ":path", 5) == 0) {
            req->path = val_copy;
            req->path_len = valuelen;
            /* Split query string */
            char *q = memchr(val_copy, '?', valuelen);
            if (q != nullptr) {
                req->path_len = (size_t)(q - val_copy);
                req->query = q + 1;
                req->query_len = valuelen - req->path_len - 1;
            }
        } else if (namelen == 10 && memcmp(name, ":authority", 10) == 0) {
            req->host = val_copy;
        }
        return 0;
    }

    /* Regular headers — copy name too */
    if (req->header_count < IO_MAX_HEADERS) {
        char *name_copy = arena_copy(&sd->arena, (const char *)name, namelen);
        if (name_copy == nullptr) {
            return NGHTTP2_ERR_CALLBACK_FAILURE;
        }

        req->headers[req->header_count] = (io_header_t){
            .name = name_copy,
            .name_len = namelen,
            .value = val_copy,
            .value_len = valuelen,
        };
        req->header_count++;

        /* Track content-type and content-length */
        if (namelen == 12 && memcmp(name, "content-type", 12) == 0) {
            req->content_type = val_copy;
        } else if (namelen == 14 && memcmp(name, "content-length", 14) == 0) {
            req->content_length = 0;
            for (size_t i = 0; i < valuelen; i++) {
                if (value[i] < '0' || value[i] > '9') {
                    return NGHTTP2_ERR_CALLBACK_FAILURE;
                }
                size_t next;
                if (ckd_mul(&next, req->content_length, 10) ||
                    ckd_add(&req->content_length, next, (size_t)(value[i] - '0'))) {
                    return NGHTTP2_ERR_CALLBACK_FAILURE;
                }
            }
        }
    }

    return 0;
}

static int on_data_chunk_cb(nghttp2_session *session, uint8_t flags, int32_t stream_id,
                            const uint8_t *data, size_t len, void *user_data)
{
    (void)flags;
    (void)user_data;

    h2_stream_data_t *sd = nghttp2_session_get_stream_user_data(session, stream_id);
    if (sd == nullptr) {
        return 0;
    }

    /* Grow body buffer */
    if (sd->body_len + len > sd->body_cap) {
        size_t new_cap = (sd->body_cap == 0) ? 1024 : sd->body_cap * 2;
        while (new_cap < sd->body_len + len) {
            new_cap *= 2;
        }
        uint8_t *new_buf = realloc(sd->body_buf, new_cap);
        if (new_buf == nullptr) {
            return NGHTTP2_ERR_CALLBACK_FAILURE;
        }
        sd->body_buf = new_buf;
        sd->body_cap = new_cap;
    }

    memcpy(sd->body_buf + sd->body_len, data, len);
    sd->body_len += len;
    sd->request.body = sd->body_buf;
    sd->request.body_len = sd->body_len;

    return 0;
}

static int on_frame_recv_cb(nghttp2_session *session, const nghttp2_frame *frame, void *user_data)
{
    io_http2_session_t *h2 = user_data;

    /* Track RST_STREAM for Rapid Reset protection */
    if (frame->hd.type == NGHTTP2_RST_STREAM) {
        time_t now = time(nullptr);
        if (now != h2->rst_window_start) {
            h2->rst_count = 1;
            h2->rst_window_start = now;
        } else {
            h2->rst_count++;
        }

        if (h2->rst_count > h2->config.max_rst_stream_per_sec && !h2->goaway_sent) {
            nghttp2_submit_goaway(session, NGHTTP2_FLAG_NONE, 0, NGHTTP2_ENHANCE_YOUR_CALM,
                                  (const uint8_t *)"rapid reset", 11);
            h2->goaway_sent = true;
        }
        return 0;
    }

    if (frame->hd.type != NGHTTP2_HEADERS && frame->hd.type != NGHTTP2_DATA) {
        return 0;
    }

    /* Dispatch request when END_STREAM is received */
    if ((frame->hd.flags & NGHTTP2_FLAG_END_STREAM) == 0) {
        return 0;
    }

    h2_stream_data_t *sd = nghttp2_session_get_stream_user_data(session, frame->hd.stream_id);
    if (sd == nullptr) {
        return 0;
    }

    if (h2->on_request != nullptr) {
        io_response_t resp;
        int rc = io_response_init(&resp);
        if (rc != 0) {
            return 0;
        }
        io_ctx_t ctx;
        rc = io_ctx_init(&ctx, &sd->request, &resp, nullptr);
        if (rc == 0) {
            rc = h2->on_request(&ctx, frame->hd.stream_id, h2->user_data);
            if (rc == 0 && !ctx.aborted) {
                submit_response(h2, frame->hd.stream_id, &resp);
            }
            io_ctx_destroy(&ctx);
        }
        io_response_destroy(&resp);
    }

    return 0;
}

static int on_stream_close_cb(nghttp2_session *session, int32_t stream_id, uint32_t error_code,
                              void *user_data)
{
    (void)error_code;
    io_http2_session_t *h2 = user_data;

    h2_stream_data_t *sd = nghttp2_session_get_stream_user_data(session, stream_id);
    if (sd != nullptr) {
        nghttp2_session_set_stream_user_data(session, stream_id, nullptr);

        /* Remove from session stream list */
        h2_stream_data_t **pp = &h2->streams;
        while (*pp != nullptr) {
            if (*pp == sd) {
                *pp = sd->next;
                break;
            }
            pp = &(*pp)->next;
        }

        stream_data_free(sd);
    }

    return 0;
}

/* ---- Public API ---- */

io_http2_session_t *io_http2_session_create(const io_http2_config_t *cfg,
                                            io_http2_on_request_fn on_req, void *user_data)
{
    io_http2_session_t *session = calloc(1, sizeof(*session));
    if (session == nullptr) {
        return nullptr;
    }

    /* Apply config with defaults */
    if (cfg != nullptr) {
        session->config = *cfg;
    }
    if (session->config.max_concurrent_streams == 0) {
        session->config.max_concurrent_streams = H2_DEFAULT_MAX_CONCURRENT_STREAMS;
    }
    if (session->config.initial_window_size == 0) {
        session->config.initial_window_size = H2_DEFAULT_INITIAL_WINDOW_SIZE;
    }
    if (session->config.max_frame_size == 0) {
        session->config.max_frame_size = H2_DEFAULT_MAX_FRAME_SIZE;
    }
    if (session->config.max_header_list_size == 0) {
        session->config.max_header_list_size = H2_DEFAULT_MAX_HEADER_LIST_SIZE;
    }
    if (session->config.max_rst_stream_per_sec == 0) {
        session->config.max_rst_stream_per_sec = H2_DEFAULT_MAX_RST_PER_SEC;
    }

    session->on_request = on_req;
    session->user_data = user_data;

    /* Allocate output buffer */
    session->out_buf = malloc(H2_OUTPUT_BUF_SIZE);
    if (session->out_buf == nullptr) {
        free(session);
        return nullptr;
    }
    session->out_cap = H2_OUTPUT_BUF_SIZE;

    /* Setup nghttp2 callbacks */
    nghttp2_session_callbacks *callbacks;
    if (nghttp2_session_callbacks_new(&callbacks) != 0) {
        free(session->out_buf);
        free(session);
        return nullptr;
    }

    nghttp2_session_callbacks_set_on_begin_headers_callback(callbacks, on_begin_headers_cb);
    nghttp2_session_callbacks_set_on_header_callback(callbacks, on_header_cb);
    nghttp2_session_callbacks_set_on_data_chunk_recv_callback(callbacks, on_data_chunk_cb);
    nghttp2_session_callbacks_set_on_frame_recv_callback(callbacks, on_frame_recv_cb);
    nghttp2_session_callbacks_set_on_stream_close_callback(callbacks, on_stream_close_cb);

    int rv = nghttp2_session_server_new(&session->ng_session, callbacks, session);
    nghttp2_session_callbacks_del(callbacks);

    if (rv != 0) {
        free(session->out_buf);
        free(session);
        return nullptr;
    }

    /* Submit initial SETTINGS */
    nghttp2_settings_entry iv[] = {
        {NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS, session->config.max_concurrent_streams},
        {NGHTTP2_SETTINGS_INITIAL_WINDOW_SIZE, session->config.initial_window_size},
        {NGHTTP2_SETTINGS_MAX_FRAME_SIZE, session->config.max_frame_size},
        {NGHTTP2_SETTINGS_MAX_HEADER_LIST_SIZE, session->config.max_header_list_size},
    };
    nghttp2_submit_settings(session->ng_session, NGHTTP2_FLAG_NONE, iv, sizeof(iv) / sizeof(iv[0]));

    return session;
}

void io_http2_session_destroy(io_http2_session_t *session)
{
    if (session == nullptr) {
        return;
    }
    if (session->ng_session != nullptr) {
        nghttp2_session_del(session->ng_session);
    }

    /* Free any streams not closed via on_stream_close_cb
     * (nghttp2_session_del does not trigger close callbacks) */
    h2_stream_data_t *sd = session->streams;
    while (sd != nullptr) {
        h2_stream_data_t *next = sd->next;
        stream_data_free(sd);
        sd = next;
    }

    free(session->out_buf);
    free(session);
}

ssize_t io_http2_on_recv(io_http2_session_t *session, const uint8_t *data, size_t len)
{
    if (session == nullptr || data == nullptr) {
        return -EINVAL;
    }

    nghttp2_ssize consumed = nghttp2_session_mem_recv2(session->ng_session, data, len);
    if (consumed < 0) {
        return -EIO;
    }

    return (ssize_t)consumed;
}

int io_http2_flush(io_http2_session_t *session, const uint8_t **out_data, size_t *out_len)
{
    if (session == nullptr || out_data == nullptr || out_len == nullptr) {
        return -EINVAL;
    }

    /* Reset output buffer */
    session->out_len = 0;

    for (;;) {
        const uint8_t *data;
        nghttp2_ssize n = nghttp2_session_mem_send2(session->ng_session, &data);
        if (n < 0) {
            return -EIO;
        }
        if (n == 0) {
            break;
        }

        int rv = output_buf_append(session, data, (size_t)n);
        if (rv != 0) {
            return rv;
        }
    }

    *out_data = session->out_buf;
    *out_len = session->out_len;
    return 0;
}

int io_http2_goaway(io_http2_session_t *session)
{
    if (session == nullptr) {
        return -EINVAL;
    }

    /* Two-phase GOAWAY per RFC 9113 §5.1.1:
     * First: GOAWAY with last_stream_id = 2^31-1 (accept no more)
     * After draining: GOAWAY with real last_stream_id */
    int32_t last_id = session->goaway_sent
                          ? (int32_t)nghttp2_session_get_last_proc_stream_id(session->ng_session)
                          : (int32_t)((1U << 31) - 1);

    int rv = nghttp2_submit_goaway(session->ng_session, NGHTTP2_FLAG_NONE, last_id,
                                   NGHTTP2_NO_ERROR, nullptr, 0);
    if (rv != 0) {
        return -EIO;
    }

    session->goaway_sent = true;
    return 0;
}

bool io_http2_want_read(const io_http2_session_t *session)
{
    if (session == nullptr || session->ng_session == nullptr) {
        return false;
    }
    return nghttp2_session_want_read(session->ng_session) != 0;
}

bool io_http2_want_write(const io_http2_session_t *session)
{
    if (session == nullptr || session->ng_session == nullptr) {
        return false;
    }
    return nghttp2_session_want_write(session->ng_session) != 0;
}

bool io_http2_goaway_sent(const io_http2_session_t *session)
{
    if (session == nullptr) {
        return false;
    }
    return session->goaway_sent;
}

bool io_http2_is_draining(const io_http2_session_t *session)
{
    if (session == nullptr) {
        return false;
    }
    return session->goaway_sent && !io_http2_want_read(session) && !io_http2_want_write(session);
}

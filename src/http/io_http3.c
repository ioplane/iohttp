/**
 * @file io_http3.c
 * @brief HTTP/3 session management — nghttp3-backed implementation.
 *
 * Sits on top of io_quic (ngtcp2 QUIC transport). Processes HTTP/3
 * frames from QUIC stream data, dispatches requests via io_ctx_t.
 */

#include "http/io_http3.h"

#include "core/io_ctx.h"

#include <errno.h>
#include <stdckdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <nghttp3/nghttp3.h>

/* ---- Constants ---- */

constexpr uint32_t H3_DEFAULT_MAX_HEADER_LIST_SIZE = 8192;

/* ---- Per-stream string arena ---- */

/* nghttp3 header name/value rcbufs are only valid during callbacks.
 * We copy all strings into a simple arena that lives with the stream. */
typedef struct {
    char *buf;
    size_t len;
    size_t cap;
} h3_arena_t;

static char *arena_copy(h3_arena_t *a, const char *src, size_t len)
{
    /* +1 for NUL terminator */
    if (a->len + len + 1 > a->cap) {
        size_t new_cap = (a->cap == 0) ? 4096 : a->cap * 2;
        while (new_cap < a->len + len + 1) {
            new_cap *= 2;
        }
        char *new_buf = realloc(a->buf, new_cap);
        if (new_buf == nullptr) {
            return nullptr; //-V773
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

/* ---- Per-stream data ---- */

typedef struct {
    io_request_t request;
    h3_arena_t arena; /* owns all string copies for header names/values/path/etc */
    uint8_t *body_buf;
    size_t body_cap;
    size_t body_len;
    int64_t stream_id;
    bool headers_done;
} h3_stream_data_t;

/* ---- Session (opaque) ---- */

struct io_http3_session {
    nghttp3_conn *ng_conn;
    io_quic_conn_t *quic_conn;
    io_http3_config_t config;
    io_http3_on_request_fn on_request;
    void *user_data;
    bool shutdown_initiated;
    bool control_streams_bound;
};

/* ---- Helpers ---- */

static h3_stream_data_t *stream_data_new(int64_t stream_id)
{
    h3_stream_data_t *sd = calloc(1, sizeof(*sd));
    if (sd == nullptr) {
        return nullptr; //-V773
    }
    io_request_init(&sd->request); //-V522
    sd->stream_id = stream_id;
    sd->request.http_version_major = 3;
    sd->request.http_version_minor = 0;
    sd->request.keep_alive = true; /* HTTP/3 is always persistent */
    return sd;
}

static void stream_data_free(h3_stream_data_t *sd)
{
    if (sd == nullptr) {
        return;
    }
    free(sd->arena.buf);
    free(sd->body_buf);
    free(sd);
}

static io_method_t parse_method(const char *method, size_t len)
{
    return io_method_parse(method, len);
}

/* ---- nghttp3 callbacks ---- */

static int h3_begin_headers_cb(nghttp3_conn *conn, int64_t stream_id, void *conn_user_data,
                               void *stream_user_data)
{
    (void)conn;
    (void)conn_user_data;

    if (stream_user_data != nullptr) {
        return 0; /* already have stream data */
    }

    h3_stream_data_t *sd = stream_data_new(stream_id);
    if (sd == nullptr) {
        return NGHTTP3_ERR_CALLBACK_FAILURE; //-V773
    }

    int rv = nghttp3_conn_set_stream_user_data(conn, stream_id, sd);
    if (rv != 0) {
        stream_data_free(sd);
        return NGHTTP3_ERR_CALLBACK_FAILURE;
    }

    return 0;
}

static int h3_recv_header_cb(nghttp3_conn *conn, int64_t stream_id, int32_t token,
                             nghttp3_rcbuf *name, nghttp3_rcbuf *value, uint8_t flags,
                             void *conn_user_data, void *stream_user_data)
{
    (void)conn;
    (void)stream_id;
    (void)flags;
    (void)conn_user_data;

    h3_stream_data_t *sd = stream_user_data;
    if (sd == nullptr) {
        return 0;
    }

    nghttp3_vec name_vec = nghttp3_rcbuf_get_buf(name);
    nghttp3_vec val_vec = nghttp3_rcbuf_get_buf(value);

    io_request_t *req = &sd->request;

    /* Copy value into arena (all headers need owned copies) */
    char *val_copy = arena_copy(&sd->arena, (const char *)val_vec.base, val_vec.len);
    if (val_copy == nullptr) {
        return NGHTTP3_ERR_CALLBACK_FAILURE;
    }

    /* Pseudo-headers (identified by token or name prefix) */
    if (name_vec.len > 0 && name_vec.base[0] == ':') {
        if (token == NGHTTP3_QPACK_TOKEN__METHOD) {
            req->method = parse_method(val_copy, val_vec.len);
        } else if (token == NGHTTP3_QPACK_TOKEN__PATH) {
            req->path = val_copy;
            req->path_len = val_vec.len;
            /* Split query string */
            char *q = memchr(val_copy, '?', val_vec.len);
            if (q != nullptr) {
                req->path_len = (size_t)(q - val_copy);
                req->query = q + 1;
                req->query_len = val_vec.len - req->path_len - 1;
            }
        } else if (token == NGHTTP3_QPACK_TOKEN__AUTHORITY) {
            req->host = val_copy;
        }
        return 0;
    }

    /* Regular headers — copy name too */
    if (req->header_count < IO_MAX_HEADERS) {
        char *name_copy = arena_copy(&sd->arena, (const char *)name_vec.base, name_vec.len);
        if (name_copy == nullptr) {
            return NGHTTP3_ERR_CALLBACK_FAILURE;
        }

        req->headers[req->header_count] = (io_header_t){
            .name = name_copy,
            .name_len = name_vec.len,
            .value = val_copy,
            .value_len = val_vec.len,
        };
        req->header_count++;

        /* Track content-type and content-length */
        if (token == NGHTTP3_QPACK_TOKEN_CONTENT_TYPE) {
            req->content_type = val_copy;
        } else if (token == NGHTTP3_QPACK_TOKEN_CONTENT_LENGTH) {
            req->content_length = 0;
            for (size_t i = 0; i < val_vec.len; i++) {
                if (val_vec.base[i] < '0' || val_vec.base[i] > '9') {
                    return NGHTTP3_ERR_CALLBACK_FAILURE;
                }
                size_t next;
                if (ckd_mul(&next, req->content_length, 10) ||
                    ckd_add(&req->content_length, next, (size_t)(val_vec.base[i] - '0'))) {
                    return NGHTTP3_ERR_CALLBACK_FAILURE;
                }
            }
        }
    }

    return 0;
}

static int h3_end_headers_cb(nghttp3_conn *conn, int64_t stream_id, int fin, void *conn_user_data,
                             void *stream_user_data)
{
    (void)conn;
    (void)stream_id;
    (void)conn_user_data;

    h3_stream_data_t *sd = stream_user_data;
    if (sd != nullptr) {
        sd->headers_done = true;
    }

    /* If fin is set, end_stream_cb will handle dispatch */
    (void)fin;
    return 0;
}

static int h3_recv_data_cb(nghttp3_conn *conn, int64_t stream_id, const uint8_t *data,
                           size_t datalen, void *conn_user_data, void *stream_user_data)
{
    (void)conn;
    (void)stream_id;
    (void)conn_user_data;

    h3_stream_data_t *sd = stream_user_data;
    if (sd == nullptr) {
        return 0;
    }

    /* Grow body buffer */
    if (sd->body_len + datalen > sd->body_cap) {
        size_t new_cap = (sd->body_cap == 0) ? 1024 : sd->body_cap * 2;
        while (new_cap < sd->body_len + datalen) {
            new_cap *= 2;
        }
        uint8_t *new_buf = realloc(sd->body_buf, new_cap);
        if (new_buf == nullptr) {
            return NGHTTP3_ERR_CALLBACK_FAILURE; //-V773
        }
        sd->body_buf = new_buf;
        sd->body_cap = new_cap;
    }

    memcpy(sd->body_buf + sd->body_len, data, datalen);
    sd->body_len += datalen;
    sd->request.body = sd->body_buf;
    sd->request.body_len = sd->body_len;

    return 0;
}

static int h3_end_stream_cb(nghttp3_conn *conn, int64_t stream_id, void *conn_user_data,
                            void *stream_user_data)
{
    (void)conn;

    io_http3_session_t *h3 = conn_user_data;
    h3_stream_data_t *sd = stream_user_data;
    if (sd == nullptr || h3 == nullptr) {
        return 0;
    }

    if (h3->on_request != nullptr) {
        io_response_t resp;
        int rc = io_response_init(&resp);
        if (rc != 0) {
            return 0;
        }
        io_ctx_t ctx;
        rc = io_ctx_init(&ctx, &sd->request, &resp, nullptr);
        if (rc == 0) {
            rc = h3->on_request(&ctx, stream_id, h3->user_data);
            if (rc == 0 && !ctx.aborted) {
                (void)io_http3_submit_response(h3, stream_id, &resp);
            }
            io_ctx_destroy(&ctx);
        }
        io_response_destroy(&resp);
    }

    return 0;
}

static int h3_stream_close_cb(nghttp3_conn *conn, int64_t stream_id, uint64_t app_error_code,
                              void *conn_user_data, void *stream_user_data)
{
    (void)conn;
    (void)stream_id;
    (void)app_error_code;
    (void)conn_user_data;

    h3_stream_data_t *sd = stream_user_data;
    stream_data_free(sd);

    return 0;
}

static int h3_deferred_consume_cb(nghttp3_conn *conn, int64_t stream_id, size_t consumed,
                                  void *conn_user_data, void *stream_user_data)
{
    (void)conn;
    (void)stream_id;
    (void)consumed;
    (void)conn_user_data;
    (void)stream_user_data;

    /* In a full implementation, we would extend QUIC flow control credit
     * via ngtcp2_conn_extend_max_stream_offset / ngtcp2_conn_extend_max_offset.
     * For now, this is a no-op. */
    return 0;
}

/* ---- Response data provider callback ---- */

typedef struct {
    const uint8_t *body;
    size_t body_len;
    size_t offset;
} h3_resp_data_t;

static nghttp3_ssize resp_data_read_cb(nghttp3_conn *conn, int64_t stream_id, nghttp3_vec *vec,
                                       size_t veccnt, uint32_t *pflags, void *conn_user_data,
                                       void *stream_user_data)
{
    (void)conn;
    (void)stream_id;
    (void)vec;
    (void)conn_user_data;
    (void)stream_user_data;

    if (veccnt == 0) {
        return 0;
    }

    /* Placeholder: signal EOF. Full body streaming requires deeper
     * integration with the QUIC layer (future sprint). */
    *pflags |= NGHTTP3_DATA_FLAG_EOF;
    return 0;
}

/* ---- Public API ---- */

void io_http3_config_init(io_http3_config_t *cfg)
{
    if (cfg == nullptr) {
        return;
    }
    *cfg = (io_http3_config_t){
        .max_header_list_size = H3_DEFAULT_MAX_HEADER_LIST_SIZE,
        .qpack_max_dtable_capacity = 0,
        .qpack_blocked_streams = 0,
    };
}

io_http3_session_t *io_http3_session_create(const io_http3_config_t *cfg, io_quic_conn_t *quic_conn,
                                            io_http3_on_request_fn on_req, void *user_data)
{
    if (quic_conn == nullptr) {
        return nullptr;
    }

    io_http3_session_t *session = calloc(1, sizeof(*session));
    if (session == nullptr) {
        return nullptr; //-V773
    }

    /* Apply config with defaults */
    if (cfg != nullptr) {
        session->config = *cfg; //-V522
    } else {
        io_http3_config_init(&session->config);
    }

    session->quic_conn = quic_conn;
    session->on_request = on_req;
    session->user_data = user_data;

    /* Setup nghttp3 callbacks */
    nghttp3_callbacks callbacks = {
        .stream_close = h3_stream_close_cb,
        .recv_data = h3_recv_data_cb,
        .deferred_consume = h3_deferred_consume_cb,
        .begin_headers = h3_begin_headers_cb,
        .recv_header = h3_recv_header_cb,
        .end_headers = h3_end_headers_cb,
        .end_stream = h3_end_stream_cb,
    };

    /* Setup nghttp3 settings */
    nghttp3_settings settings;
    nghttp3_settings_default(&settings);
    settings.max_field_section_size = session->config.max_header_list_size;
    settings.qpack_max_dtable_capacity = session->config.qpack_max_dtable_capacity;
    settings.qpack_blocked_streams = session->config.qpack_blocked_streams;

    int rv = nghttp3_conn_server_new(&session->ng_conn, &callbacks, &settings, nullptr, session);
    if (rv != 0) {
        free(session);
        return nullptr;
    }

    /* Try to bind HTTP/3 control streams (control, QPACK encoder, QPACK decoder).
     * The server must open 3 unidirectional QUIC streams and bind them to
     * the nghttp3 connection. This may fail if QUIC handshake is not yet
     * complete (peer hasn't granted uni stream credits). In that case,
     * binding is deferred — call io_http3_bind_control_streams() after
     * handshake completes. */
    (void)io_http3_bind_control_streams(session);

    return session;
}

void io_http3_session_destroy(io_http3_session_t *session)
{
    if (session == nullptr) {
        return;
    }
    if (session->ng_conn != nullptr) {
        nghttp3_conn_del(session->ng_conn);
    }
    free(session);
}

int io_http3_bind_control_streams(io_http3_session_t *session)
{
    if (session == nullptr) {
        return -EINVAL;
    }
    if (session->control_streams_bound) {
        return 0;
    }

    int64_t ctrl_stream_id;
    int64_t qenc_stream_id;
    int64_t qdec_stream_id;

    if (io_quic_open_uni_stream(session->quic_conn, &ctrl_stream_id) != 0 ||
        io_quic_open_uni_stream(session->quic_conn, &qenc_stream_id) != 0 ||
        io_quic_open_uni_stream(session->quic_conn, &qdec_stream_id) != 0) {
        return -EAGAIN;
    }

    int rv = nghttp3_conn_bind_control_stream(session->ng_conn, ctrl_stream_id);
    if (rv != 0) {
        return -EIO;
    }

    rv = nghttp3_conn_bind_qpack_streams(session->ng_conn, qenc_stream_id, qdec_stream_id);
    if (rv != 0) {
        return -EIO;
    }

    session->control_streams_bound = true;
    return 0;
}

int io_http3_on_stream_data(io_http3_session_t *session, int64_t stream_id, const uint8_t *data,
                            size_t len, bool fin)
{
    if (session == nullptr) {
        return -EINVAL;
    }

    nghttp3_ssize consumed =
        nghttp3_conn_read_stream(session->ng_conn, stream_id, data, len, (int)fin);
    if (consumed < 0) {
        return -EIO;
    }

    return 0;
}

int io_http3_on_stream_open(io_http3_session_t *session, int64_t stream_id)
{
    if (session == nullptr) {
        return -EINVAL;
    }

    /* For bidi streams opened by the client, nghttp3 will handle them
     * when data arrives (via begin_headers callback). For uni streams
     * (control, QPACK), they need to be bound separately.
     * This function is a hook for the QUIC layer to notify HTTP/3. */
    (void)stream_id;
    return 0;
}

int io_http3_submit_response(io_http3_session_t *session, int64_t stream_id,
                             const io_response_t *resp)
{
    if (session == nullptr || resp == nullptr) {
        return -EINVAL;
    }

    /* Build :status pseudo-header */
    char status_str[4];
    int slen = snprintf(status_str, sizeof(status_str), "%u", resp->status);
    if (slen < 0 || (size_t)slen >= sizeof(status_str)) {
        return -EINVAL;
    }

    /* Count total headers: :status + response headers */
    size_t nva_count = 1 + resp->header_count;
    nghttp3_nv *nva = calloc(nva_count, sizeof(*nva));
    if (nva == nullptr) {
        return -ENOMEM; //-V773
    }

    /* :status */
    nva[0] = (nghttp3_nv){ //-V522
        .name = (uint8_t *)":status",
        .namelen = 7,
        .value = (uint8_t *)status_str,
        .valuelen = (size_t)slen,
        .flags = NGHTTP3_NV_FLAG_NO_COPY_NAME,
    };

    /* Copy response headers — HTTP/3 requires lowercase header names.
     * We lowercase in-place in a temporary buffer. */
    char **lc_names = nullptr;
    if (resp->header_count > 0) {
        lc_names = calloc(resp->header_count, sizeof(*lc_names));
        if (lc_names == nullptr) {
            free(nva);
            return -ENOMEM; //-V773
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
            lc_names[i][c] = (ch >= 'A' && ch <= 'Z') ? (char)(ch + 32) : ch; //-V522
        }
        lc_names[i][resp->headers[i].name_len] = '\0';

        nva[1 + i] = (nghttp3_nv){
            .name = (uint8_t *)lc_names[i],
            .namelen = resp->headers[i].name_len,
            .value = (uint8_t *)resp->headers[i].value,
            .valuelen = resp->headers[i].value_len,
            .flags = NGHTTP3_NV_FLAG_NONE,
        };
    }

    int rv;
    if (resp->body != nullptr && resp->body_len > 0) {
        nghttp3_data_reader dr = {
            .read_data = resp_data_read_cb,
        };
        rv = nghttp3_conn_submit_response(session->ng_conn, stream_id, nva, nva_count, &dr);
    } else {
        rv = nghttp3_conn_submit_response(session->ng_conn, stream_id, nva, nva_count, nullptr);
    }

    /* Free temporary lowercase name copies */
    if (lc_names != nullptr) {
        for (uint32_t i = 0; i < resp->header_count; i++) {
            free(lc_names[i]);
        }
        free(lc_names);
    }
    free(nva);

    return (rv == 0) ? 0 : -EIO;
}

bool io_http3_is_shutdown(const io_http3_session_t *session)
{
    if (session == nullptr) {
        return true;
    }
    return session->shutdown_initiated;
}

int io_http3_shutdown(io_http3_session_t *session)
{
    if (session == nullptr) {
        return -EINVAL;
    }

    /* nghttp3_conn_shutdown requires control streams to be bound.
     * If not yet bound, just mark shutdown and skip the GOAWAY. */
    if (session->control_streams_bound) {
        int rv = nghttp3_conn_shutdown(session->ng_conn);
        if (rv != 0) {
            return -EIO;
        }
    }

    session->shutdown_initiated = true;
    return 0;
}

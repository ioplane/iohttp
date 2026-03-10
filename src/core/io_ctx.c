/**
 * @file io_ctx.c
 * @brief Unified per-request context implementation.
 */

#include "core/io_ctx.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- Lifecycle ---- */

int io_ctx_init(io_ctx_t *c, io_request_t *req, io_response_t *resp, io_server_t *srv)
{
    if (c == nullptr || req == nullptr || resp == nullptr) {
        return -EINVAL;
    }

    memset(c, 0, sizeof(*c));
    c->req = req;
    c->resp = resp;
    c->server = srv;
    c->conn_fd = -1;
    c->aborted = false;
    c->value_count = 0;

    c->arena.base = malloc(IO_CTX_ARENA_DEFAULT);
    if (c->arena.base == nullptr) {
        return -ENOMEM;
    }
    c->arena.size = IO_CTX_ARENA_DEFAULT;
    c->arena.used = 0;

    return 0;
}

void io_ctx_reset(io_ctx_t *c)
{
    if (c == nullptr) {
        return;
    }

    /* call destructors for all values */
    for (uint32_t i = 0; i < c->value_count; i++) {
        if (c->values[i].destructor != nullptr && c->values[i].value != nullptr) {
            c->values[i].destructor(c->values[i].value);
        }
        c->values[i].key = nullptr;
        c->values[i].value = nullptr;
        c->values[i].destructor = nullptr;
    }

    c->value_count = 0;
    c->arena.used = 0;
    c->aborted = false;
}

void io_ctx_destroy(io_ctx_t *c)
{
    if (c == nullptr) {
        return;
    }

    io_ctx_reset(c);
    free(c->arena.base);
    c->arena.base = nullptr;
    c->arena.size = 0;
}

/* ---- Context values ---- */

int io_ctx_set(io_ctx_t *c, const char *key, void *value)
{
    return io_ctx_set_with_destructor(c, key, value, nullptr);
}

int io_ctx_set_with_destructor(io_ctx_t *c, const char *key, void *value, void (*dtor)(void *))
{
    if (c == nullptr || key == nullptr) {
        return -EINVAL;
    }

    /* scan for existing key */
    for (uint32_t i = 0; i < c->value_count; i++) {
        if (c->values[i].key != nullptr && strcmp(c->values[i].key, key) == 0) {
            c->values[i].value = value;
            c->values[i].destructor = dtor;
            return 0;
        }
    }

    /* append new */
    if (c->value_count >= IO_CTX_MAX_VALUES) {
        return -ENOSPC;
    }

    c->values[c->value_count].key = key;
    c->values[c->value_count].value = value;
    c->values[c->value_count].destructor = dtor;
    c->value_count++;

    return 0;
}

void *io_ctx_get(const io_ctx_t *c, const char *key)
{
    if (c == nullptr || key == nullptr) {
        return nullptr;
    }

    for (uint32_t i = 0; i < c->value_count; i++) {
        if (c->values[i].key != nullptr && strcmp(c->values[i].key, key) == 0) {
            return c->values[i].value;
        }
    }

    return nullptr;
}

/* ---- Request accessors ---- */

const char *io_ctx_param(const io_ctx_t *c, const char *name)
{
    if (c == nullptr || c->req == nullptr) {
        return nullptr;
    }
    return io_request_param(c->req, name);
}

int io_ctx_param_i64(const io_ctx_t *c, const char *name, int64_t *out)
{
    if (c == nullptr || c->req == nullptr) {
        return -EINVAL;
    }
    return io_request_param_i64(c->req, name, out);
}

int io_ctx_param_u64(const io_ctx_t *c, const char *name, uint64_t *out)
{
    if (c == nullptr || c->req == nullptr) {
        return -EINVAL;
    }
    return io_request_param_u64(c->req, name, out);
}

const char *io_ctx_query(const io_ctx_t *c, const char *name)
{
    if (c == nullptr || c->req == nullptr) {
        return nullptr;
    }
    return io_request_query_param(c->req, name);
}

const char *io_ctx_header(const io_ctx_t *c, const char *name)
{
    if (c == nullptr || c->req == nullptr) {
        return nullptr;
    }
    return io_request_header(c->req, name);
}

const char *io_ctx_cookie(const io_ctx_t *c, const char *name)
{
    if (c == nullptr || c->req == nullptr) {
        return nullptr;
    }
    return io_request_cookie(c->req, name);
}

io_method_t io_ctx_method(const io_ctx_t *c)
{
    if (c == nullptr || c->req == nullptr) {
        return IO_METHOD_UNKNOWN;
    }
    return c->req->method;
}

const char *io_ctx_path(const io_ctx_t *c)
{
    if (c == nullptr || c->req == nullptr) {
        return nullptr;
    }
    return c->req->path;
}

const uint8_t *io_ctx_body(const io_ctx_t *c, size_t *len)
{
    if (c == nullptr || c->req == nullptr) {
        if (len != nullptr) {
            *len = 0;
        }
        return nullptr;
    }
    if (len != nullptr) {
        *len = c->req->body_len;
    }
    return c->req->body;
}

/* ---- Response helpers ---- */

int io_ctx_status(io_ctx_t *c, uint16_t status)
{
    if (c == nullptr || c->resp == nullptr) {
        return -EINVAL;
    }
    c->resp->status = status;
    return 0;
}

int io_ctx_set_header(io_ctx_t *c, const char *name, const char *value)
{
    if (c == nullptr || c->resp == nullptr) {
        return -EINVAL;
    }
    return io_response_set_header(c->resp, name, value);
}

int io_ctx_set_cookie(io_ctx_t *c, const io_cookie_t *cookie)
{
    if (c == nullptr || c->resp == nullptr || cookie == nullptr) {
        return -EINVAL;
    }

    char buf[512];
    int len = io_cookie_serialize(cookie, buf, sizeof(buf));
    if (len < 0) {
        return len;
    }

    return io_response_add_header(c->resp, "Set-Cookie", buf);
}

int io_ctx_json(io_ctx_t *c, uint16_t status, const char *json)
{
    if (c == nullptr || c->resp == nullptr) {
        return -EINVAL;
    }
    return io_respond_json(c->resp, status, json);
}

int io_ctx_text(io_ctx_t *c, uint16_t status, const char *text)
{
    if (c == nullptr || c->resp == nullptr || text == nullptr) {
        return -EINVAL;
    }
    size_t len = strnlen(text, 1024 * 1024);
    return io_respond(c->resp, status, "text/plain", (const uint8_t *)text, len);
}

int io_ctx_html(io_ctx_t *c, uint16_t status, const char *html)
{
    if (c == nullptr || c->resp == nullptr || html == nullptr) {
        return -EINVAL;
    }
    size_t len = strnlen(html, 1024 * 1024);
    return io_respond(c->resp, status, "text/html", (const uint8_t *)html, len);
}

int io_ctx_blob(io_ctx_t *c, uint16_t status, const char *content_type, const uint8_t *data,
                size_t len)
{
    if (c == nullptr || c->resp == nullptr) {
        return -EINVAL;
    }
    return io_respond(c->resp, status, content_type, data, len);
}

int io_ctx_no_content(io_ctx_t *c)
{
    if (c == nullptr || c->resp == nullptr) {
        return -EINVAL;
    }
    c->resp->status = 204;
    return 0;
}

int io_ctx_redirect(io_ctx_t *c, uint16_t status, const char *url)
{
    if (c == nullptr || c->resp == nullptr || url == nullptr) {
        return -EINVAL;
    }
    c->resp->status = status;
    return io_response_set_header(c->resp, "Location", url);
}

int io_ctx_error(io_ctx_t *c, uint16_t status, const char *message)
{
    if (c == nullptr || c->resp == nullptr || message == nullptr) {
        return -EINVAL;
    }

    char buf[512];
    int n = snprintf(buf, sizeof(buf), "{\"error\":\"%s\",\"status\":%u}", message, status);
    if (n < 0 || (size_t)n >= sizeof(buf)) {
        return -ENOMEM;
    }

    return io_respond_json(c->resp, status, buf);
}

void io_ctx_abort(io_ctx_t *c)
{
    if (c != nullptr) {
        c->aborted = true;
    }
}

/* ---- Arena allocator ---- */

static int arena_grow(io_arena_t *a, size_t needed)
{
    size_t new_size = a->size;
    while (new_size < needed) {
        new_size *= 2;
    }

    uint8_t *new_base = realloc(a->base, new_size);
    if (new_base == nullptr) {
        return -ENOMEM;
    }

    a->base = new_base;
    a->size = new_size;
    return 0;
}

void *io_ctx_alloc(io_ctx_t *c, size_t size)
{
    if (c == nullptr || size == 0) {
        return nullptr;
    }

    size_t needed = c->arena.used + size;
    if (needed > c->arena.size) {
        if (arena_grow(&c->arena, needed) != 0) {
            return nullptr;
        }
    }

    void *ptr = c->arena.base + c->arena.used;
    c->arena.used += size;
    return ptr;
}

void *io_ctx_alloc_aligned(io_ctx_t *c, size_t size, size_t align)
{
    if (c == nullptr || size == 0 || align == 0) {
        return nullptr;
    }

    /* align must be power of 2 */
    if ((align & (align - 1)) != 0) {
        return nullptr;
    }

    /* align based on absolute pointer address */
    uintptr_t cur = (uintptr_t)(c->arena.base + c->arena.used);
    size_t mask = align - 1;
    uintptr_t aligned = (cur + mask) & ~mask;
    size_t padding = (size_t)(aligned - cur);
    size_t aligned_used = c->arena.used + padding;

    size_t needed = aligned_used + size;
    if (needed > c->arena.size) {
        if (arena_grow(&c->arena, needed) != 0) {
            return nullptr;
        }
        /* recompute after potential realloc (base may change) */
        cur = (uintptr_t)(c->arena.base + c->arena.used);
        aligned = (cur + mask) & ~mask;
        padding = (size_t)(aligned - cur);
        aligned_used = c->arena.used + padding;
    }

    void *ptr = c->arena.base + aligned_used;
    c->arena.used = aligned_used + size;
    return ptr;
}

char *io_ctx_sprintf(io_ctx_t *c, const char *fmt, ...)
{
    if (c == nullptr || fmt == nullptr) {
        return nullptr;
    }

    /* first pass: determine length */
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(nullptr, 0, fmt, ap);
    va_end(ap);

    if (n < 0) {
        return nullptr;
    }

    size_t len = (size_t)n + 1; /* +1 for NUL */
    char *buf = io_ctx_alloc(c, len);
    if (buf == nullptr) {
        return nullptr;
    }

    /* second pass: format into arena */
    va_start(ap, fmt);
    vsnprintf(buf, len, fmt, ap);
    va_end(ap);

    return buf;
}

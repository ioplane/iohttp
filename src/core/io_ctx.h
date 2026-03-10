/**
 * @file io_ctx.h
 * @brief Unified per-request context with arena allocator and key-value store.
 */

#ifndef IOHTTP_CORE_CTX_H
#define IOHTTP_CORE_CTX_H

#include "http/io_cookie.h"
#include "http/io_request.h"
#include "http/io_response.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ---- Forward declarations ---- */

typedef struct io_ctx io_ctx_t;
typedef struct io_server io_server_t;

/* ---- Context key-value store ---- */

constexpr uint32_t IO_CTX_MAX_VALUES = 16;

typedef struct {
    const char *key;
    void *value;
    void (*destructor)(void *);
} io_ctx_value_t;

/* ---- Per-request arena allocator ---- */

constexpr size_t IO_CTX_ARENA_DEFAULT = 4096;

typedef struct {
    uint8_t *base;
    size_t size;
    size_t used;
} io_arena_t;

/* ---- Unified request context ---- */

struct io_ctx {
    io_request_t *req;
    io_response_t *resp;
    io_server_t *server; /* may be nullptr in tests */
    io_arena_t arena;
    io_ctx_value_t values[IO_CTX_MAX_VALUES];
    uint32_t value_count;
    int conn_fd;
    bool aborted;
};

/* ---- Lifecycle (called by server, not by users) ---- */

/**
 * @brief Initialize a context, allocating the arena.
 * @param c    Context to initialize.
 * @param req  Request pointer.
 * @param resp Response pointer.
 * @param srv  Server pointer (may be nullptr in tests).
 * @return 0 on success, -EINVAL on bad input, -ENOMEM on allocation failure.
 */
[[nodiscard]] int io_ctx_init(io_ctx_t *c, io_request_t *req, io_response_t *resp,
                              io_server_t *srv);

/**
 * @brief Reset context for reuse. Calls destructors, resets arena. Keeps memory.
 * @param c Context to reset.
 */
void io_ctx_reset(io_ctx_t *c);

/**
 * @brief Destroy context. Calls reset, then frees arena.
 * @param c Context to destroy (nullptr is safe).
 */
void io_ctx_destroy(io_ctx_t *c);

/* ---- Context values ---- */

/**
 * @brief Store a value by key. Replaces existing key.
 * @param c     Context.
 * @param key   Key string (not copied, must outlive context).
 * @param value Value pointer.
 * @return 0 on success, -EINVAL on bad input, -ENOSPC if full.
 */
[[nodiscard]] int io_ctx_set(io_ctx_t *c, const char *key, void *value);

/**
 * @brief Store a value with destructor. Replaces existing key.
 * @param c     Context.
 * @param key   Key string (not copied).
 * @param value Value pointer.
 * @param dtor  Destructor called on reset/destroy (may be nullptr).
 * @return 0 on success, -EINVAL on bad input, -ENOSPC if full.
 */
[[nodiscard]] int io_ctx_set_with_destructor(io_ctx_t *c, const char *key, void *value,
                                             void (*dtor)(void *));

/**
 * @brief Retrieve a value by key.
 * @param c   Context.
 * @param key Key to look up.
 * @return Value pointer, or nullptr if not found.
 */
void *io_ctx_get(const io_ctx_t *c, const char *key);

/* ---- Request accessors (convenience wrappers) ---- */

const char *io_ctx_param(const io_ctx_t *c, const char *name);
[[nodiscard]] int io_ctx_param_i64(const io_ctx_t *c, const char *name, int64_t *out);
[[nodiscard]] int io_ctx_param_u64(const io_ctx_t *c, const char *name, uint64_t *out);
const char *io_ctx_query(const io_ctx_t *c, const char *name);
const char *io_ctx_header(const io_ctx_t *c, const char *name);
const char *io_ctx_cookie(const io_ctx_t *c, const char *name);
io_method_t io_ctx_method(const io_ctx_t *c);
const char *io_ctx_path(const io_ctx_t *c);
const uint8_t *io_ctx_body(const io_ctx_t *c, size_t *len);

/* ---- Response helpers ---- */

[[nodiscard]] int io_ctx_status(io_ctx_t *c, uint16_t status);
[[nodiscard]] int io_ctx_set_header(io_ctx_t *c, const char *name, const char *value);
[[nodiscard]] int io_ctx_set_cookie(io_ctx_t *c, const io_cookie_t *cookie);
[[nodiscard]] int io_ctx_json(io_ctx_t *c, uint16_t status, const char *json);
[[nodiscard]] int io_ctx_text(io_ctx_t *c, uint16_t status, const char *text);
[[nodiscard]] int io_ctx_html(io_ctx_t *c, uint16_t status, const char *html);
[[nodiscard]] int io_ctx_blob(io_ctx_t *c, uint16_t status, const char *content_type,
                              const uint8_t *data, size_t len);
[[nodiscard]] int io_ctx_no_content(io_ctx_t *c);
[[nodiscard]] int io_ctx_redirect(io_ctx_t *c, uint16_t status, const char *url);
[[nodiscard]] int io_ctx_error(io_ctx_t *c, uint16_t status, const char *message);
void io_ctx_abort(io_ctx_t *c);

/* ---- Arena allocator ---- */

/**
 * @brief Bump-allocate from the per-request arena. Grows if needed.
 * @param c    Context.
 * @param size Bytes to allocate.
 * @return Pointer to allocated memory, or nullptr on OOM.
 */
void *io_ctx_alloc(io_ctx_t *c, size_t size);

/**
 * @brief Aligned bump-allocate from the per-request arena.
 * @param c     Context.
 * @param size  Bytes to allocate.
 * @param align Alignment (must be power of 2).
 * @return Pointer to aligned memory, or nullptr on OOM.
 */
void *io_ctx_alloc_aligned(io_ctx_t *c, size_t size, size_t align);

/**
 * @brief Printf into the arena, returning the formatted string.
 * @param c   Context.
 * @param fmt Printf format string.
 * @return NUL-terminated string in arena, or nullptr on OOM.
 */
char *io_ctx_sprintf(io_ctx_t *c, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

#endif /* IOHTTP_CORE_CTX_H */

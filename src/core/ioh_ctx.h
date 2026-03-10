/**
 * @file ioh_ctx.h
 * @brief Unified per-request context with arena allocator and key-value store.
 */

#ifndef IOHTTP_CORE_CTX_H
#define IOHTTP_CORE_CTX_H

#include "http/ioh_cookie.h"
#include "http/ioh_request.h"
#include "http/ioh_response.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ---- Forward declarations ---- */

typedef struct ioh_ctx ioh_ctx_t;
typedef struct ioh_server ioh_server_t;

/* ---- Context key-value store ---- */

constexpr uint32_t IOH_CTX_MAX_VALUES = 16;

typedef struct {
    const char *key;
    void *value;
    void (*destructor)(void *);
} ioh_ctx_value_t;

/* ---- Per-request arena allocator ---- */

constexpr size_t IOH_CTX_ARENA_DEFAULT = 4096;

typedef struct {
    uint8_t *base;
    size_t size;
    size_t used;
} ioh_arena_t;

/* ---- Unified request context ---- */

struct ioh_ctx {
    ioh_request_t *req;
    ioh_response_t *resp;
    ioh_server_t *server; /* may be nullptr in tests */
    ioh_arena_t arena;
    ioh_ctx_value_t values[IOH_CTX_MAX_VALUES];
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
[[nodiscard]] int ioh_ctx_init(ioh_ctx_t *c, ioh_request_t *req, ioh_response_t *resp,
                              ioh_server_t *srv);

/**
 * @brief Reset context for reuse. Calls destructors, resets arena. Keeps memory.
 * @param c Context to reset.
 */
void ioh_ctx_reset(ioh_ctx_t *c);

/**
 * @brief Destroy context. Calls reset, then frees arena.
 * @param c Context to destroy (nullptr is safe).
 */
void ioh_ctx_destroy(ioh_ctx_t *c);

/* ---- Context values ---- */

/**
 * @brief Store a value by key. Replaces existing key.
 * @param c     Context.
 * @param key   Key string (not copied, must outlive context).
 * @param value Value pointer.
 * @return 0 on success, -EINVAL on bad input, -ENOSPC if full.
 */
[[nodiscard]] int ioh_ctx_set(ioh_ctx_t *c, const char *key, void *value);

/**
 * @brief Store a value with destructor. Replaces existing key.
 * @param c     Context.
 * @param key   Key string (not copied).
 * @param value Value pointer.
 * @param dtor  Destructor called on reset/destroy (may be nullptr).
 * @return 0 on success, -EINVAL on bad input, -ENOSPC if full.
 */
[[nodiscard]] int ioh_ctx_set_with_destructor(ioh_ctx_t *c, const char *key, void *value,
                                             void (*dtor)(void *));

/**
 * @brief Retrieve a value by key.
 * @param c   Context.
 * @param key Key to look up.
 * @return Value pointer, or nullptr if not found.
 */
void *ioh_ctx_get(const ioh_ctx_t *c, const char *key);

/* ---- Request accessors (convenience wrappers) ---- */

const char *ioh_ctx_param(const ioh_ctx_t *c, const char *name);
[[nodiscard]] int ioh_ctx_param_i64(const ioh_ctx_t *c, const char *name, int64_t *out);
[[nodiscard]] int ioh_ctx_param_u64(const ioh_ctx_t *c, const char *name, uint64_t *out);
const char *ioh_ctx_query(const ioh_ctx_t *c, const char *name);
const char *ioh_ctx_header(const ioh_ctx_t *c, const char *name);
const char *ioh_ctx_cookie(const ioh_ctx_t *c, const char *name);
ioh_method_t ioh_ctx_method(const ioh_ctx_t *c);
const char *ioh_ctx_path(const ioh_ctx_t *c);
const uint8_t *ioh_ctx_body(const ioh_ctx_t *c, size_t *len);

/* ---- Response helpers ---- */

[[nodiscard]] int ioh_ctx_status(ioh_ctx_t *c, uint16_t status);
[[nodiscard]] int ioh_ctx_set_header(ioh_ctx_t *c, const char *name, const char *value);
[[nodiscard]] int ioh_ctx_set_cookie(ioh_ctx_t *c, const ioh_cookie_t *cookie);
[[nodiscard]] int ioh_ctx_json(ioh_ctx_t *c, uint16_t status, const char *json);
[[nodiscard]] int ioh_ctx_text(ioh_ctx_t *c, uint16_t status, const char *text);
[[nodiscard]] int ioh_ctx_html(ioh_ctx_t *c, uint16_t status, const char *html);
[[nodiscard]] int ioh_ctx_blob(ioh_ctx_t *c, uint16_t status, const char *content_type,
                              const uint8_t *data, size_t len);
[[nodiscard]] int ioh_ctx_no_content(ioh_ctx_t *c);
[[nodiscard]] int ioh_ctx_redirect(ioh_ctx_t *c, uint16_t status, const char *url);
[[nodiscard]] int ioh_ctx_error(ioh_ctx_t *c, uint16_t status, const char *message);
void ioh_ctx_abort(ioh_ctx_t *c);

/* ---- Arena allocator ---- */

/**
 * @brief Bump-allocate from the per-request arena. Grows if needed.
 * @param c    Context.
 * @param size Bytes to allocate.
 * @return Pointer to allocated memory, or nullptr on OOM.
 */
void *ioh_ctx_alloc(ioh_ctx_t *c, size_t size);

/**
 * @brief Aligned bump-allocate from the per-request arena.
 * @param c     Context.
 * @param size  Bytes to allocate.
 * @param align Alignment (must be power of 2).
 * @return Pointer to aligned memory, or nullptr on OOM.
 */
void *ioh_ctx_alloc_aligned(ioh_ctx_t *c, size_t size, size_t align);

/**
 * @brief Printf into the arena, returning the formatted string.
 * @param c   Context.
 * @param fmt Printf format string.
 * @return NUL-terminated string in arena, or nullptr on OOM.
 */
char *ioh_ctx_sprintf(ioh_ctx_t *c, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

#endif /* IOHTTP_CORE_CTX_H */

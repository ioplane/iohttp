/**
 * @file io_router.h
 * @brief Public router API wrapping radix trie with per-method dispatch.
 *
 * Provides automatic 405 Method Not Allowed, auto-HEAD from GET,
 * trailing-slash redirects, and path normalization with security checks.
 */

#ifndef IOHTTP_ROUTER_ROUTER_H
#define IOHTTP_ROUTER_ROUTER_H

#include "core/io_ctx.h"
#include "http/io_request.h"
#include "http/io_response.h"
#include "router/io_radix.h"
#include "router/io_route_meta.h"

/* Handler function -- returns 0 on success, negative errno on error */
typedef int (*io_handler_fn)(io_ctx_t *c);

/* Route options -- extensible metadata per-route */
typedef struct {
    const io_route_meta_t *meta; /* route metadata for introspection */
    void *oas_operation;         /* for liboas binding */
    uint32_t permissions;        /* bitmask for auth */
    bool auth_required;
    uint32_t header_timeout_ms;    /**< 0 = use server default */
    uint32_t body_timeout_ms;      /**< 0 = use server default */
    uint32_t keepalive_timeout_ms; /**< 0 = use server default */
} io_route_opts_t;

/* Match result from dispatch */
typedef enum : uint8_t {
    IO_MATCH_FOUND,
    IO_MATCH_NOT_FOUND,
    IO_MATCH_METHOD_NOT_ALLOWED,
    IO_MATCH_REDIRECT,
} io_match_status_t;

/* Total storage for all param values copied from normalized path */
constexpr size_t IO_MAX_PARAM_STORAGE = 512;

typedef struct {
    io_match_status_t status;
    io_handler_fn handler;
    const io_route_opts_t *opts;
    const io_route_meta_t *meta; /* route metadata (latest, post-set_meta) */
    io_param_t params[IO_MAX_PATH_PARAMS];
    uint32_t param_count;
    char allowed_methods[128];                /* "GET, POST, DELETE" for 405 */
    char redirect_path[IO_MAX_URI_SIZE];      /* for trailing slash redirect */
    char param_storage[IO_MAX_PARAM_STORAGE]; /* stable storage for param values */
    size_t param_storage_used;
} io_route_match_t;

/* Opaque router type */
typedef struct io_router io_router_t;

/**
 * @brief Create a new router with per-method radix trees.
 * @return New router, or nullptr on allocation failure.
 */
[[nodiscard]] io_router_t *io_router_create(void);

/**
 * @brief Destroy a router and all its radix trees.
 * @param router Router to destroy (nullptr safe).
 */
void io_router_destroy(io_router_t *router);

/* ---- Method-specific registration ---- */

[[nodiscard]] int io_router_get(io_router_t *r, const char *pattern, io_handler_fn h);
[[nodiscard]] int io_router_post(io_router_t *r, const char *pattern, io_handler_fn h);
[[nodiscard]] int io_router_put(io_router_t *r, const char *pattern, io_handler_fn h);
[[nodiscard]] int io_router_delete(io_router_t *r, const char *pattern, io_handler_fn h);
[[nodiscard]] int io_router_patch(io_router_t *r, const char *pattern, io_handler_fn h);
[[nodiscard]] int io_router_head(io_router_t *r, const char *pattern, io_handler_fn h);
[[nodiscard]] int io_router_options(io_router_t *r, const char *pattern, io_handler_fn h);

/* ---- Generic method registration ---- */

[[nodiscard]] int io_router_handle(io_router_t *r, io_method_t method, const char *pattern,
                                   io_handler_fn h);

/* ---- Registration with route options ---- */

[[nodiscard]] int io_router_get_with(io_router_t *r, const char *pattern, io_handler_fn h,
                                     const io_route_opts_t *opts);

[[nodiscard]] int io_router_handle_with(io_router_t *r, io_method_t method, const char *pattern,
                                        io_handler_fn h, const io_route_opts_t *opts);

[[nodiscard]] int io_router_post_with(io_router_t *r, const char *pattern, io_handler_fn h,
                                      const io_route_opts_t *opts);
[[nodiscard]] int io_router_put_with(io_router_t *r, const char *pattern, io_handler_fn h,
                                     const io_route_opts_t *opts);
[[nodiscard]] int io_router_delete_with(io_router_t *r, const char *pattern, io_handler_fn h,
                                        const io_route_opts_t *opts);
[[nodiscard]] int io_router_patch_with(io_router_t *r, const char *pattern, io_handler_fn h,
                                       const io_route_opts_t *opts);

/* ---- Dispatch ---- */

/**
 * @brief Match a request path to a handler.
 * @param r        Router.
 * @param method   HTTP method.
 * @param path     URL path.
 * @param path_len Length of path.
 * @return Match result (check status field).
 */
[[nodiscard]] io_route_match_t io_router_dispatch(const io_router_t *r, io_method_t method,
                                                  const char *path, size_t path_len);

/* ---- Group ownership (used by io_route_group.c) ---- */

/**
 * @brief Register a group as owned by this router (freed on destroy).
 * @param r       Router.
 * @param group   Group pointer (opaque).
 * @param destroy Destructor function for the group.
 * @return 0 on success, negative errno on error.
 */
[[nodiscard]] int io_router_own_group(io_router_t *r, void *group, void (*destroy)(void *));

/* ---- Internal accessors (used by io_route_inspect.c) ---- */

/**
 * @brief Get the radix tree for a specific HTTP method.
 * @param r      Router.
 * @param method HTTP method index.
 * @return Radix tree pointer, or nullptr if none registered.
 */
io_radix_tree_t *io_router_get_tree(const io_router_t *r, io_method_t method);

/**
 * @brief Get the number of method slots in the router.
 * @return Method count (always 9).
 */
uint32_t io_router_method_count(void);

/* ---- Path normalization (public for testing) ---- */

/**
 * @brief Normalize a URL path: collapse //, resolve .., reject traversal.
 * @param path     Input path.
 * @param path_len Input path length.
 * @param out      Output buffer.
 * @param out_size Output buffer size.
 * @param out_len  Resulting normalized path length.
 * @return 0 on success, -EINVAL on traversal above root or bad input.
 */
[[nodiscard]] int io_path_normalize(const char *path, size_t path_len, char *out, size_t out_size,
                                    size_t *out_len);

#endif /* IOHTTP_ROUTER_ROUTER_H */

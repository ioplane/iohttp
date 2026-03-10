/**
 * @file ioh_router.h
 * @brief Public router API wrapping radix trie with per-method dispatch.
 *
 * Provides automatic 405 Method Not Allowed, auto-HEAD from GET,
 * trailing-slash redirects, and path normalization with security checks.
 */

#ifndef IOHTTP_ROUTER_ROUTER_H
#define IOHTTP_ROUTER_ROUTER_H

#include "core/ioh_ctx.h"
#include "http/ioh_request.h"
#include "http/ioh_response.h"
#include "router/ioh_radix.h"
#include "router/ioh_route_meta.h"

/* Handler function -- returns 0 on success, negative errno on error */
typedef int (*ioh_handler_fn)(ioh_ctx_t *c);

/* Route options -- extensible metadata per-route */
typedef struct {
    const ioh_route_meta_t *meta; /* route metadata for introspection */
    void *oas_operation;         /* for liboas binding */
    uint32_t permissions;        /* bitmask for auth */
    bool auth_required;
    uint32_t body_timeout_ms;      /**< 0 = use server default */
    uint32_t keepalive_timeout_ms; /**< 0 = use server default */
} ioh_route_opts_t;

/* Match result from dispatch */
typedef enum : uint8_t {
    IOH_MATCH_FOUND,
    IOH_MATCH_NOT_FOUND,
    IOH_MATCH_METHOD_NOT_ALLOWED,
    IOH_MATCH_REDIRECT,
} ioh_match_status_t;

/* Total storage for all param values copied from normalized path */
constexpr size_t IOH_MAX_PARAM_STORAGE = 512;

typedef struct {
    ioh_match_status_t status;
    ioh_handler_fn handler;
    const ioh_route_opts_t *opts;
    const ioh_route_meta_t *meta; /* route metadata (latest, post-set_meta) */
    ioh_param_t params[IOH_MAX_PATH_PARAMS];
    uint32_t param_count;
    char allowed_methods[128];                /* "GET, POST, DELETE" for 405 */
    char redirect_path[IOH_MAX_URI_SIZE];      /* for trailing slash redirect */
    char param_storage[IOH_MAX_PARAM_STORAGE]; /* stable storage for param values */
    size_t param_storage_used;
} ioh_route_match_t;

/* Opaque router type */
typedef struct ioh_router ioh_router_t;

/**
 * @brief Create a new router with per-method radix trees.
 * @return New router, or nullptr on allocation failure.
 */
[[nodiscard]] ioh_router_t *ioh_router_create(void);

/**
 * @brief Destroy a router and all its radix trees.
 * @param router Router to destroy (nullptr safe).
 */
void ioh_router_destroy(ioh_router_t *router);

/* ---- Method-specific registration ---- */

[[nodiscard]] int ioh_router_get(ioh_router_t *r, const char *pattern, ioh_handler_fn h);
[[nodiscard]] int ioh_router_post(ioh_router_t *r, const char *pattern, ioh_handler_fn h);
[[nodiscard]] int ioh_router_put(ioh_router_t *r, const char *pattern, ioh_handler_fn h);
[[nodiscard]] int ioh_router_delete(ioh_router_t *r, const char *pattern, ioh_handler_fn h);
[[nodiscard]] int ioh_router_patch(ioh_router_t *r, const char *pattern, ioh_handler_fn h);
[[nodiscard]] int ioh_router_head(ioh_router_t *r, const char *pattern, ioh_handler_fn h);
[[nodiscard]] int ioh_router_options(ioh_router_t *r, const char *pattern, ioh_handler_fn h);

/* ---- Generic method registration ---- */

[[nodiscard]] int ioh_router_handle(ioh_router_t *r, ioh_method_t method, const char *pattern,
                                   ioh_handler_fn h);

/* ---- Registration with route options ---- */

[[nodiscard]] int ioh_router_get_with(ioh_router_t *r, const char *pattern, ioh_handler_fn h,
                                     const ioh_route_opts_t *opts);

[[nodiscard]] int ioh_router_handle_with(ioh_router_t *r, ioh_method_t method, const char *pattern,
                                        ioh_handler_fn h, const ioh_route_opts_t *opts);

[[nodiscard]] int ioh_router_post_with(ioh_router_t *r, const char *pattern, ioh_handler_fn h,
                                      const ioh_route_opts_t *opts);
[[nodiscard]] int ioh_router_put_with(ioh_router_t *r, const char *pattern, ioh_handler_fn h,
                                     const ioh_route_opts_t *opts);
[[nodiscard]] int ioh_router_delete_with(ioh_router_t *r, const char *pattern, ioh_handler_fn h,
                                        const ioh_route_opts_t *opts);
[[nodiscard]] int ioh_router_patch_with(ioh_router_t *r, const char *pattern, ioh_handler_fn h,
                                       const ioh_route_opts_t *opts);

/* ---- Dispatch ---- */

/**
 * @brief Match a request path to a handler.
 * @param r        Router.
 * @param method   HTTP method.
 * @param path     URL path.
 * @param path_len Length of path.
 * @return Match result (check status field).
 */
[[nodiscard]] ioh_route_match_t ioh_router_dispatch(const ioh_router_t *r, ioh_method_t method,
                                                  const char *path, size_t path_len);

/* ---- Group ownership (used by ioh_route_group.c) ---- */

/**
 * @brief Register a group as owned by this router (freed on destroy).
 * @param r       Router.
 * @param group   Group pointer (opaque).
 * @param destroy Destructor function for the group.
 * @return 0 on success, negative errno on error.
 */
[[nodiscard]] int ioh_router_own_group(ioh_router_t *r, void *group, void (*destroy)(void *));

/* ---- Internal accessors (used by ioh_route_inspect.c) ---- */

/**
 * @brief Get the radix tree for a specific HTTP method.
 * @param r      Router.
 * @param method HTTP method index.
 * @return Radix tree pointer, or nullptr if none registered.
 */
ioh_radix_tree_t *ioh_router_get_tree(const ioh_router_t *r, ioh_method_t method);

/**
 * @brief Get the number of method slots in the router.
 * @return Method count (always 9).
 */
uint32_t ioh_router_method_count(void);

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
[[nodiscard]] int ioh_path_normalize(const char *path, size_t path_len, char *out, size_t out_size,
                                    size_t *out_len);

#endif /* IOHTTP_ROUTER_ROUTER_H */

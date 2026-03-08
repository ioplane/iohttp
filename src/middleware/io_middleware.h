/**
 * @file io_middleware.h
 * @brief Middleware chain execution: global -> group -> handler.
 *
 * Provides chain-based middleware execution with short-circuit support,
 * custom error handling, and custom 404/405 handlers on the router.
 */

#ifndef IOHTTP_MIDDLEWARE_MIDDLEWARE_H
#define IOHTTP_MIDDLEWARE_MIDDLEWARE_H

#include "http/io_request.h"
#include "http/io_response.h"
#include "router/io_route_group.h"
#include "router/io_router.h"

constexpr uint32_t IO_MAX_GLOBAL_MIDDLEWARE = 32;

/* ---- Router-level middleware registration ---- */

/**
 * @brief Add global middleware to a router (runs for ALL routes).
 * @param r  Router.
 * @param mw Middleware function.
 * @return 0 on success, -EINVAL or -ENOSPC on error.
 */
[[nodiscard]] int io_router_use(io_router_t *r, io_middleware_fn mw);

/* ---- Error and fallback handlers on router ---- */

/** Error handler — called when handler or middleware returns non-zero. */
typedef int (*io_error_handler_fn)(io_ctx_t *c, int error);

/**
 * @brief Set a custom error handler on the router.
 * @param r Router.
 * @param h Error handler function.
 */
void io_router_set_error_handler(io_router_t *r, io_error_handler_fn h);

/**
 * @brief Set a custom 404 Not Found handler.
 * @param r Router.
 * @param h Handler function.
 */
void io_router_set_not_found(io_router_t *r, io_handler_fn h);

/**
 * @brief Set a custom 405 Method Not Allowed handler.
 * @param r Router.
 * @param h Handler function.
 */
void io_router_set_method_not_allowed(io_router_t *r, io_handler_fn h);

/* ---- Internal accessors (used by io_middleware.c) ---- */

/**
 * @brief Get the global middleware array from a router.
 * @param r     Router.
 * @param count Output: number of global middleware functions.
 * @return Pointer to the middleware array, or nullptr.
 */
io_middleware_fn *io_router_global_middleware(const io_router_t *r, uint32_t *count);

/**
 * @brief Get the error handler from a router.
 * @param r Router.
 * @return Error handler, or nullptr if none set.
 */
io_error_handler_fn io_router_error_handler(const io_router_t *r);

/**
 * @brief Get the custom not-found handler from a router.
 * @param r Router.
 * @return Handler, or nullptr if none set.
 */
io_handler_fn io_router_not_found_handler(const io_router_t *r);

/**
 * @brief Get the custom method-not-allowed handler from a router.
 * @param r Router.
 * @return Handler, or nullptr if none set.
 */
io_handler_fn io_router_method_not_allowed_handler(const io_router_t *r);

/* ---- Middleware chain context ---- */

typedef struct {
    io_middleware_fn *middleware; /* array of middleware fns */
    uint32_t count;               /* total middleware count */
    uint32_t current;             /* current position in chain */
    io_handler_fn handler;        /* final handler */
} io_chain_t;

/**
 * @brief Build and execute a middleware chain for a matched route.
 *
 * Execution order: global_mw[0..N] -> group_mw[0..M] -> handler.
 * Any middleware may short-circuit by returning without calling next.
 *
 * @param c            Request context.
 * @param global_mw    Global middleware array (may be nullptr if count is 0).
 * @param global_count Number of global middleware functions.
 * @param group_mw     Group middleware array (may be nullptr if count is 0).
 * @param group_count  Number of group middleware functions.
 * @param handler      Final route handler.
 * @return 0 on success, negative errno on error.
 */
[[nodiscard]] int io_chain_execute(io_ctx_t *c,
                                   io_middleware_fn *global_mw, uint32_t global_count,
                                   io_middleware_fn *group_mw, uint32_t group_count,
                                   io_handler_fn handler);

#endif /* IOHTTP_MIDDLEWARE_MIDDLEWARE_H */

/**
 * @file ioh_route_group.h
 * @brief Route groups with prefix composition and per-group middleware slots.
 *
 * Implements an Express.Router-style group API: routes registered on a group
 * have the group's full prefix prepended automatically. Groups can be nested
 * to compose prefixes (e.g. /api + /v1 + /users -> /api/v1/users).
 *
 * Middleware slots are stored per-group but not executed until Sprint 4.6.
 */

#ifndef IOHTTP_ROUTER_ROUTE_GROUP_H
#define IOHTTP_ROUTER_ROUTE_GROUP_H

#include "router/ioh_router.h"

/**
 * Middleware function type -- wraps handler with pre/post processing.
 */
typedef int (*ioh_middleware_fn)(ioh_ctx_t *c, ioh_handler_fn next);

constexpr uint32_t IOH_MAX_GROUP_MIDDLEWARE = 16;
constexpr uint32_t IOH_MAX_GROUP_PREFIX = 256;

typedef struct ioh_group ioh_group_t;

/**
 * @brief Create a route group on a router.
 * @param r      Router that owns this group.
 * @param prefix URL prefix (e.g. "/api").
 * @return New group, or nullptr on error.
 */
[[nodiscard]] ioh_group_t *ioh_router_group(ioh_router_t *r, const char *prefix);

/**
 * @brief Create a nested subgroup under an existing group.
 * @param g      Parent group.
 * @param prefix Additional prefix (e.g. "/v1").
 * @return New subgroup, or nullptr on error.
 */
[[nodiscard]] ioh_group_t *ioh_group_subgroup(ioh_group_t *g, const char *prefix);

/* ---- Method-specific registration on groups ---- */

[[nodiscard]] int ioh_group_get(ioh_group_t *g, const char *pattern, ioh_handler_fn h);
[[nodiscard]] int ioh_group_post(ioh_group_t *g, const char *pattern, ioh_handler_fn h);
[[nodiscard]] int ioh_group_put(ioh_group_t *g, const char *pattern, ioh_handler_fn h);
[[nodiscard]] int ioh_group_delete(ioh_group_t *g, const char *pattern, ioh_handler_fn h);
[[nodiscard]] int ioh_group_patch(ioh_group_t *g, const char *pattern, ioh_handler_fn h);

/**
 * @brief Attach a middleware function to a group.
 * @param g  Group.
 * @param mw Middleware function pointer.
 * @return 0 on success, -EINVAL or -ENOSPC on error.
 */
[[nodiscard]] int ioh_group_use(ioh_group_t *g, ioh_middleware_fn mw);

/**
 * @brief Get the full prefix (group + all parent prefixes).
 * @param g Group.
 * @return Full prefix string.
 */
const char *ioh_group_prefix(const ioh_group_t *g);

/**
 * @brief Get the parent group (nullptr for top-level groups).
 * @param g Group.
 * @return Parent group, or nullptr.
 */
const ioh_group_t *ioh_group_parent(const ioh_group_t *g);

/**
 * @brief Get the middleware count for a group.
 * @param g Group.
 * @return Number of middleware functions registered.
 */
uint32_t ioh_group_middleware_count(const ioh_group_t *g);

/**
 * @brief Get a middleware function by index.
 * @param g   Group.
 * @param idx Index (0-based).
 * @return Middleware function pointer, or nullptr if out of range.
 */
ioh_middleware_fn ioh_group_middleware_at(const ioh_group_t *g, uint32_t idx);

/**
 * @brief Register a GET route with route options.
 * @param g       Group.
 * @param pattern Route pattern (appended to group prefix).
 * @param h       Handler function.
 * @param opts    Route options (may be nullptr).
 * @return 0 on success, negative errno on error.
 */
[[nodiscard]] int ioh_group_get_with(ioh_group_t *g, const char *pattern, ioh_handler_fn h,
                                    const ioh_route_opts_t *opts);

/**
 * @brief Destroy a group and all its subgroups.
 * @param g Group to destroy (nullptr safe).
 */
void ioh_group_destroy(ioh_group_t *g);

#endif /* IOHTTP_ROUTER_ROUTE_GROUP_H */

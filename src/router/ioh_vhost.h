/**
 * @file ioh_vhost.h
 * @brief Host-based virtual routing dispatcher.
 *
 * Routes requests to per-host routers based on the Host header.
 * Supports exact match, wildcard (*.example.com), port stripping,
 * and case-insensitive matching. Exact matches take priority over wildcards.
 */

#ifndef IOHTTP_ROUTER_VHOST_H
#define IOHTTP_ROUTER_VHOST_H

#include "http/ioh_request.h"
#include "router/ioh_router.h"

constexpr uint32_t IOH_VHOST_MAX_HOSTS = 32;

typedef struct ioh_vhost ioh_vhost_t;

/**
 * @brief Create a virtual host dispatcher.
 * @return New vhost, or nullptr on allocation failure.
 */
[[nodiscard]] ioh_vhost_t *ioh_vhost_create(void);

/**
 * @brief Destroy a vhost dispatcher and all owned routers.
 * @param v Vhost to destroy (nullptr safe).
 */
void ioh_vhost_destroy(ioh_vhost_t *v);

/**
 * @brief Add a host -> router mapping.
 * Supports exact match ("api.example.com") and wildcard ("*.example.com").
 * @param v      Vhost dispatcher.
 * @param host   Host pattern (copied internally).
 * @param router Router for this host (ownership NOT transferred).
 * @return 0 on success, -EINVAL on bad input, -ENOSPC if max hosts reached, -ENOMEM on OOM.
 */
[[nodiscard]] int ioh_vhost_add(ioh_vhost_t *v, const char *host, ioh_router_t *router);

/**
 * @brief Set the default router for unmatched hosts.
 * @param v      Vhost dispatcher.
 * @param router Default router (ownership NOT transferred).
 */
void ioh_vhost_set_default(ioh_vhost_t *v, ioh_router_t *router);

/**
 * @brief Dispatch a request to the appropriate router by Host header.
 * Extracts Host header, strips port if present, matches against registered hosts.
 * Falls back to default router if no match.
 * @param v        Vhost dispatcher.
 * @param method   HTTP method.
 * @param host     Host header value (may include port).
 * @param path     URL path.
 * @param path_len Path length.
 * @return Route match result (check status field).
 */
[[nodiscard]] ioh_route_match_t ioh_vhost_dispatch(const ioh_vhost_t *v, ioh_method_t method,
                                                 const char *host, const char *path,
                                                 size_t path_len);

/**
 * @brief Get the number of registered hosts.
 * @param v Vhost dispatcher.
 * @return Number of registered host entries.
 */
uint32_t ioh_vhost_count(const ioh_vhost_t *v);

#endif /* IOHTTP_ROUTER_VHOST_H */

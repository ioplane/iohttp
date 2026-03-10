/**
 * @file io_vhost.h
 * @brief Host-based virtual routing dispatcher.
 *
 * Routes requests to per-host routers based on the Host header.
 * Supports exact match, wildcard (*.example.com), port stripping,
 * and case-insensitive matching. Exact matches take priority over wildcards.
 */

#ifndef IOHTTP_ROUTER_VHOST_H
#define IOHTTP_ROUTER_VHOST_H

#include "http/io_request.h"
#include "router/io_router.h"

constexpr uint32_t IO_VHOST_MAX_HOSTS = 32;

typedef struct io_vhost io_vhost_t;

/**
 * @brief Create a virtual host dispatcher.
 * @return New vhost, or nullptr on allocation failure.
 */
[[nodiscard]] io_vhost_t *io_vhost_create(void);

/**
 * @brief Destroy a vhost dispatcher and all owned routers.
 * @param v Vhost to destroy (nullptr safe).
 */
void io_vhost_destroy(io_vhost_t *v);

/**
 * @brief Add a host -> router mapping.
 * Supports exact match ("api.example.com") and wildcard ("*.example.com").
 * @param v      Vhost dispatcher.
 * @param host   Host pattern (copied internally).
 * @param router Router for this host (ownership NOT transferred).
 * @return 0 on success, -EINVAL on bad input, -ENOSPC if max hosts reached, -ENOMEM on OOM.
 */
[[nodiscard]] int io_vhost_add(io_vhost_t *v, const char *host, io_router_t *router);

/**
 * @brief Set the default router for unmatched hosts.
 * @param v      Vhost dispatcher.
 * @param router Default router (ownership NOT transferred).
 */
void io_vhost_set_default(io_vhost_t *v, io_router_t *router);

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
[[nodiscard]] io_route_match_t io_vhost_dispatch(const io_vhost_t *v, io_method_t method,
                                                 const char *host, const char *path,
                                                 size_t path_len);

/**
 * @brief Get the number of registered hosts.
 * @param v Vhost dispatcher.
 * @return Number of registered host entries.
 */
uint32_t io_vhost_count(const io_vhost_t *v);

#endif /* IOHTTP_ROUTER_VHOST_H */

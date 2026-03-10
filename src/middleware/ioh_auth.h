/**
 * @file ioh_auth.h
 * @brief Basic and Bearer authentication middleware.
 */

#ifndef IOHTTP_MIDDLEWARE_AUTH_H
#define IOHTTP_MIDDLEWARE_AUTH_H

#include "router/ioh_route_group.h"

#include <stdbool.h>

/**
 * Verification callback: returns true if credentials are valid.
 * @param credentials Decoded "user:pass" (Basic) or token (Bearer).
 * @param ctx         User-provided context.
 */
typedef bool (*ioh_auth_verify_fn)(const char *credentials, void *ctx);

/**
 * @brief Create HTTP Basic authentication middleware.
 * @param verify Verification callback.
 * @param ctx    User context passed to verify.
 * @return Middleware function, or nullptr on error.
 */
[[nodiscard]] ioh_middleware_fn ioh_auth_basic_create(ioh_auth_verify_fn verify, void *ctx);

/**
 * @brief Create HTTP Bearer authentication middleware.
 * @param verify Verification callback.
 * @param ctx    User context passed to verify.
 * @return Middleware function, or nullptr on error.
 */
[[nodiscard]] ioh_middleware_fn ioh_auth_bearer_create(ioh_auth_verify_fn verify, void *ctx);

/**
 * @brief Destroy auth middleware state.
 */
void ioh_auth_destroy(void);

#endif /* IOHTTP_MIDDLEWARE_AUTH_H */

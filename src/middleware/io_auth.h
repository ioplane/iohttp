/**
 * @file io_auth.h
 * @brief Basic and Bearer authentication middleware.
 */

#ifndef IOHTTP_MIDDLEWARE_AUTH_H
#define IOHTTP_MIDDLEWARE_AUTH_H

#include "router/io_route_group.h"

#include <stdbool.h>

/**
 * Verification callback: returns true if credentials are valid.
 * @param credentials Decoded "user:pass" (Basic) or token (Bearer).
 * @param ctx         User-provided context.
 */
typedef bool (*io_auth_verify_fn)(const char *credentials, void *ctx);

/**
 * @brief Create HTTP Basic authentication middleware.
 * @param verify Verification callback.
 * @param ctx    User context passed to verify.
 * @return Middleware function, or nullptr on error.
 */
[[nodiscard]] io_middleware_fn io_auth_basic_create(io_auth_verify_fn verify,
                                                    void *ctx);

/**
 * @brief Create HTTP Bearer authentication middleware.
 * @param verify Verification callback.
 * @param ctx    User context passed to verify.
 * @return Middleware function, or nullptr on error.
 */
[[nodiscard]] io_middleware_fn io_auth_bearer_create(io_auth_verify_fn verify,
                                                     void *ctx);

/**
 * @brief Destroy auth middleware state.
 */
void io_auth_destroy(void);

#endif /* IOHTTP_MIDDLEWARE_AUTH_H */

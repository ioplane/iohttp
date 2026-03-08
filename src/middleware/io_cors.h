/**
 * @file io_cors.h
 * @brief CORS middleware — preflight and simple request handling.
 */

#ifndef IOHTTP_MIDDLEWARE_CORS_H
#define IOHTTP_MIDDLEWARE_CORS_H

#include "router/io_route_group.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    const char **allowed_origins;
    uint32_t origin_count;
    const char **allowed_methods;
    uint32_t method_count;
    const char **allowed_headers;
    uint32_t header_count;
    bool allow_credentials;
    uint32_t max_age_seconds;
} io_cors_config_t;

/**
 * @brief Initialize CORS config with sensible defaults.
 * @param cfg Config to initialize.
 */
void io_cors_config_init(io_cors_config_t *cfg);

/**
 * @brief Create CORS middleware from configuration.
 * @param cfg CORS config (copied internally).
 * @return Middleware function, or nullptr on error.
 */
[[nodiscard]] io_middleware_fn io_cors_create(const io_cors_config_t *cfg);

/**
 * @brief Destroy CORS middleware state.
 */
void io_cors_destroy(void);

#endif /* IOHTTP_MIDDLEWARE_CORS_H */

/**
 * @file io_security.h
 * @brief Security headers middleware.
 */

#ifndef IOHTTP_MIDDLEWARE_SECURITY_H
#define IOHTTP_MIDDLEWARE_SECURITY_H

#include "router/io_route_group.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    const char *csp;
    bool hsts;
    uint32_t hsts_max_age;
    const char *frame_options;
    const char *referrer_policy;
    bool nosniff;
} io_security_config_t;

/**
 * @brief Initialize security config with sensible defaults.
 * @param cfg Config to initialize.
 */
void io_security_config_init(io_security_config_t *cfg);

/**
 * @brief Create security headers middleware from configuration.
 * @param cfg Security config (copied internally).
 * @return Middleware function, or nullptr on error.
 */
[[nodiscard]] io_middleware_fn io_security_create(
    const io_security_config_t *cfg);

/**
 * @brief Destroy security middleware state.
 */
void io_security_destroy(void);

#endif /* IOHTTP_MIDDLEWARE_SECURITY_H */

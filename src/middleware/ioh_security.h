/**
 * @file ioh_security.h
 * @brief Security headers middleware.
 */

#ifndef IOHTTP_MIDDLEWARE_SECURITY_H
#define IOHTTP_MIDDLEWARE_SECURITY_H

#include "router/ioh_route_group.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    const char *csp;
    bool hsts;
    uint32_t hsts_max_age;
    const char *frame_options;
    const char *referrer_policy;
    bool nosniff;
} ioh_security_config_t;

/**
 * @brief Initialize security config with sensible defaults.
 * @param cfg Config to initialize.
 */
void ioh_security_config_init(ioh_security_config_t *cfg);

/**
 * @brief Create security headers middleware from configuration.
 * @param cfg Security config (copied internally).
 * @return Middleware function, or nullptr on error.
 */
[[nodiscard]] ioh_middleware_fn ioh_security_create(const ioh_security_config_t *cfg);

/**
 * @brief Destroy security middleware state.
 */
void ioh_security_destroy(void);

#endif /* IOHTTP_MIDDLEWARE_SECURITY_H */

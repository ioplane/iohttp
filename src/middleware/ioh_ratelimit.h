/**
 * @file ioh_ratelimit.h
 * @brief Token-bucket rate limiting middleware.
 */

#ifndef IOHTTP_MIDDLEWARE_RATELIMIT_H
#define IOHTTP_MIDDLEWARE_RATELIMIT_H

#include "router/ioh_route_group.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint32_t requests_per_second;
    uint32_t burst;
} ioh_ratelimit_config_t;

/**
 * @brief Initialize rate limit config with defaults (10 rps, burst 20).
 * @param cfg Config to initialize.
 */
void ioh_ratelimit_config_init(ioh_ratelimit_config_t *cfg);

/**
 * @brief Create rate limiting middleware from configuration.
 * @param cfg Rate limit config (copied internally).
 * @return Middleware function, or nullptr on error.
 */
[[nodiscard]] ioh_middleware_fn ioh_ratelimit_create(const ioh_ratelimit_config_t *cfg);

/**
 * @brief Destroy rate limiting middleware state.
 */
void ioh_ratelimit_destroy(void);

/**
 * @brief Reset all token buckets (for testing).
 */
void ioh_ratelimit_reset(void);

/**
 * @brief Manually check/consume a token for a given key (for testing).
 * @param key Client identifier string.
 * @return true if allowed, false if rate limited.
 */
[[nodiscard]] bool ioh_ratelimit_check(const char *key);

#endif /* IOHTTP_MIDDLEWARE_RATELIMIT_H */

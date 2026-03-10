/**
 * @file ioh_health.h
 * @brief Health check endpoint framework (/health, /ready, /live).
 */

#ifndef IOHTTP_CORE_HEALTH_H
#define IOHTTP_CORE_HEALTH_H

#include <stdbool.h>
#include <stdint.h>

typedef struct ioh_ctx ioh_ctx_t;
typedef struct ioh_router ioh_router_t;
typedef struct ioh_server ioh_server_t;

/**
 * Health check callback.
 * @param message  Output: human-readable status (points into static/literal storage).
 * @param user_data  Opaque user context.
 * @return 0 = healthy, negative errno = unhealthy.
 */
typedef int (*ioh_health_check_fn)(const char **message, void *user_data);

constexpr uint32_t IOH_HEALTH_MAX_CHECKS = 8;

typedef struct {
    const char *name;
    ioh_health_check_fn check;
    void *user_data;
} ioh_health_checker_t;

typedef struct {
    bool enabled;
    const char *health_path; /**< default "/health" */
    const char *ready_path;  /**< default "/ready" */
    const char *live_path;   /**< default "/live" */
    ioh_health_checker_t checkers[IOH_HEALTH_MAX_CHECKS];
    uint32_t checker_count;
} ioh_health_config_t;

/**
 * @brief Initialize health config with defaults.
 */
void ioh_health_config_init(ioh_health_config_t *cfg);

/**
 * @brief Add a custom deep-liveness checker.
 * @return 0 on success, -ENOSPC if max checkers reached.
 */
[[nodiscard]] int ioh_health_add_checker(ioh_health_config_t *cfg, const char *name,
                                        ioh_health_check_fn check, void *user_data);

/**
 * @brief Register health endpoints on a router.
 * @param r    Router to register routes on.
 * @param srv  Server pointer (used by /ready to check drain state).
 * @param cfg  Health configuration.
 * @return 0 on success, negative errno on error.
 */
[[nodiscard]] int ioh_health_register(ioh_router_t *r, ioh_server_t *srv,
                                     const ioh_health_config_t *cfg);

#endif /* IOHTTP_CORE_HEALTH_H */

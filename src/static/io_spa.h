/**
 * @file io_spa.h
 * @brief SPA fallback serving with hashed asset caching.
 */

#ifndef IOHTTP_STATIC_SPA_H
#define IOHTTP_STATIC_SPA_H

#include "static/io_static.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    const char *root_dir;      /* document root */
    const char *index_file;    /* default "index.html" */
    const char **api_prefixes; /* paths excluded from fallback, e.g. "/api/" */
    uint32_t api_prefix_count;
    uint32_t max_age_default;   /* for static assets, default 3600 */
    uint32_t max_age_immutable; /* for hashed assets, default 31536000 (1 year) */
} io_spa_config_t;

/**
 * @brief Initialize SPA config with defaults.
 * @param cfg Config to initialize.
 */
void io_spa_config_init(io_spa_config_t *cfg);

/**
 * @brief Serve SPA: try file first, then fallback to index.html.
 *        API prefixes return -ENOENT instead of fallback.
 * @param cfg   SPA serving config.
 * @param req   HTTP request (uses path for file lookup).
 * @param resp  HTTP response to populate.
 * @return 0 on success, -ENOENT if API prefix or missing file with extension, <0 on error.
 */
[[nodiscard]] int io_spa_serve(const io_spa_config_t *cfg, const io_request_t *req,
                               io_response_t *resp);

/**
 * @brief Check if a path looks like a hashed asset.
 *        Heuristic: filename contains a dot followed by 6+ hex chars followed by another dot.
 *        e.g., "app.abc123de.js" -> true, "style.css" -> false
 * @param path     File path.
 * @param path_len Path length.
 * @return true if path looks like a hashed asset.
 */
bool io_spa_is_hashed_asset(const char *path, size_t path_len);

#endif /* IOHTTP_STATIC_SPA_H */

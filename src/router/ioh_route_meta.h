/**
 * @file ioh_route_meta.h
 * @brief Route metadata types for introspection and documentation.
 *
 * Provides structured per-route metadata (summary, tags, params, deprecated)
 * independent of liboas. Stored via ioh_route_opts_t.meta pointer.
 */

#ifndef IOHTTP_ROUTER_ROUTE_META_H
#define IOHTTP_ROUTER_ROUTE_META_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Parameter location in HTTP request */
typedef enum : uint8_t {
    IOH_PARAM_PATH,   /* /users/:id */
    IOH_PARAM_QUERY,  /* ?page=1 */
    IOH_PARAM_HEADER, /* X-Request-Id */
    IOH_PARAM_COOKIE, /* session_id */
} ioh_param_in_t;

/* Per-parameter metadata (name, location, description) */
typedef struct {
    const char *name;        /* parameter name, e.g. "id" */
    const char *description; /* human-readable, e.g. "User UUID" */
    ioh_param_in_t in;        /* where in the request */
    bool required;           /* always true for path params */
} ioh_param_meta_t;

/* Per-route metadata for introspection and documentation */
typedef struct {
    const char *summary;           /* short description, e.g. "Get user by ID" */
    const char *description;       /* longer text (nullable) */
    const char *const *tags;       /* nullptr-terminated array, e.g. {"users", nullptr} */
    bool deprecated;               /* route is deprecated */
    const ioh_param_meta_t *params; /* array of param descriptors */
    uint32_t param_count;          /* number of params */
} ioh_route_meta_t;

/**
 * @brief Get string name for a parameter location.
 * @param in Parameter location enum value.
 * @return Static string: "path", "query", "header", "cookie", or "unknown".
 */
const char *ioh_param_in_name(ioh_param_in_t in);

#endif /* IOHTTP_ROUTER_ROUTE_META_H */

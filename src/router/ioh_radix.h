/**
 * @file ioh_radix.h
 * @brief Internal radix trie (compressed prefix tree) for URL routing.
 *
 * Not part of the public API. Used by ioh_router for pattern matching.
 */

#ifndef IOHTTP_ROUTER_RADIX_H
#define IOHTTP_ROUTER_RADIX_H

#include "http/ioh_request.h" /* for ioh_param_t, IOH_MAX_PATH_PARAMS */
#include "router/ioh_route_meta.h"

#include <stddef.h>
#include <stdint.h>

/* Node types with priority ordering: STATIC > PARAM > WILDCARD */
typedef enum : uint8_t {
    IOH_NODE_STATIC,   /* /users/list -- highest priority */
    IOH_NODE_PARAM,    /* /:id        -- medium priority  */
    IOH_NODE_WILDCARD, /* wildcard    -- lowest priority  */
} ioh_node_type_t;

typedef struct ioh_radix_node {
    char *prefix; /* compressed edge label */
    ioh_node_type_t type;
    char *param_name;                /* for PARAM/WILDCARD nodes */
    void *handler;                   /* opaque handler pointer */
    void *metadata;                  /* route options, oas_operation_t* */
    const ioh_route_meta_t *meta;     /* route metadata (separate from opts) */
    struct ioh_radix_node **children; /* sorted by: STATIC > PARAM > WILDCARD */
    uint32_t child_count;
    uint32_t child_capacity;
    uint32_t priority; /* sum of handlers in subtree */
} ioh_radix_node_t;

typedef struct {
    ioh_radix_node_t *root;
} ioh_radix_tree_t;

/* Match result */
typedef struct {
    void *handler;
    void *metadata;
    const ioh_route_meta_t *meta;
    ioh_param_t params[IOH_MAX_PATH_PARAMS];
    uint32_t param_count;
} ioh_radix_match_t;

/**
 * @brief Create a new radix trie.
 * @return New tree, or nullptr on allocation failure.
 */
[[nodiscard]] ioh_radix_tree_t *ioh_radix_create(void);

/**
 * @brief Destroy a radix trie and free all nodes.
 * @param tree Tree to destroy (nullptr safe).
 */
void ioh_radix_destroy(ioh_radix_tree_t *tree);

/**
 * @brief Insert a route pattern into the trie.
 *
 * Patterns use :name for parameters, *name for wildcards.
 * Wildcards must be the last segment.
 *
 * @param tree     Radix tree.
 * @param pattern  URL pattern (e.g. "/users/:id").
 * @param handler  Opaque handler pointer.
 * @param metadata Opaque metadata pointer.
 * @param meta     Route metadata pointer (may be nullptr).
 * @return 0 on success, -ENOMEM, -EINVAL for bad pattern, -EEXIST for conflict.
 */
[[nodiscard]] int ioh_radix_insert(ioh_radix_tree_t *tree, const char *pattern, void *handler,
                                  void *metadata, const ioh_route_meta_t *meta);

/**
 * @brief Look up a concrete path in the trie.
 *
 * @param tree     Radix tree.
 * @param path     Concrete URL path (e.g. "/users/42").
 * @param path_len Length of path.
 * @param match    Output match result with extracted parameters.
 * @return 0 if found (match populated), -ENOENT if no match.
 */
[[nodiscard]] int ioh_radix_lookup(const ioh_radix_tree_t *tree, const char *path, size_t path_len,
                                  ioh_radix_match_t *match);

#endif /* IOHTTP_ROUTER_RADIX_H */

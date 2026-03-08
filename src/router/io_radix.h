/**
 * @file io_radix.h
 * @brief Internal radix trie (compressed prefix tree) for URL routing.
 *
 * Not part of the public API. Used by io_router for pattern matching.
 */

#ifndef IOHTTP_ROUTER_RADIX_H
#define IOHTTP_ROUTER_RADIX_H

#include "http/io_request.h" /* for io_param_t, IO_MAX_PATH_PARAMS */
#include "router/io_route_meta.h"

#include <stddef.h>
#include <stdint.h>

/* Node types with priority ordering: STATIC > PARAM > WILDCARD */
typedef enum : uint8_t {
    IO_NODE_STATIC,   /* /users/list -- highest priority */
    IO_NODE_PARAM,    /* /:id        -- medium priority  */
    IO_NODE_WILDCARD, /* wildcard    -- lowest priority  */
} io_node_type_t;

typedef struct io_radix_node {
    char *prefix; /* compressed edge label */
    io_node_type_t type;
    char *param_name;                /* for PARAM/WILDCARD nodes */
    void *handler;                   /* opaque handler pointer */
    void *metadata;                  /* route options, oas_operation_t* */
    const io_route_meta_t *meta;     /* route metadata (separate from opts) */
    struct io_radix_node **children; /* sorted by: STATIC > PARAM > WILDCARD */
    uint32_t child_count;
    uint32_t child_capacity;
    uint32_t priority; /* sum of handlers in subtree */
} io_radix_node_t;

typedef struct {
    io_radix_node_t *root;
} io_radix_tree_t;

/* Match result */
typedef struct {
    void *handler;
    void *metadata;
    const io_route_meta_t *meta;
    io_param_t params[IO_MAX_PATH_PARAMS];
    uint32_t param_count;
} io_radix_match_t;

/**
 * @brief Create a new radix trie.
 * @return New tree, or nullptr on allocation failure.
 */
[[nodiscard]] io_radix_tree_t *io_radix_create(void);

/**
 * @brief Destroy a radix trie and free all nodes.
 * @param tree Tree to destroy (nullptr safe).
 */
void io_radix_destroy(io_radix_tree_t *tree);

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
[[nodiscard]] int io_radix_insert(io_radix_tree_t *tree, const char *pattern, void *handler,
                                  void *metadata, const io_route_meta_t *meta);

/**
 * @brief Look up a concrete path in the trie.
 *
 * @param tree     Radix tree.
 * @param path     Concrete URL path (e.g. "/users/42").
 * @param path_len Length of path.
 * @param match    Output match result with extracted parameters.
 * @return 0 if found (match populated), -ENOENT if no match.
 */
[[nodiscard]] int io_radix_lookup(const io_radix_tree_t *tree, const char *path, size_t path_len,
                                  io_radix_match_t *match);

#endif /* IOHTTP_ROUTER_RADIX_H */

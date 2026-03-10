/**
 * @file ioh_route_inspect.c
 * @brief Route introspection: walk, count, and metadata binding.
 */

#include "router/ioh_route_inspect.h"
#include "router/ioh_radix.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

/* Maximum reconstructed pattern length */
constexpr size_t MAX_PATTERN_LEN = 2048;

/* ---- Internal: recursive tree walking ---- */

typedef struct {
    ioh_route_walk_fn fn;
    void *ctx;
    ioh_method_t method;
    char pattern[MAX_PATTERN_LEN];
    size_t pattern_len;
} walk_state_t;

/**
 * Recursively walk a radix node's subtree, reconstructing patterns.
 * Returns 0 to continue, positive if callback stopped, negative on error.
 */
static int walk_node(walk_state_t *state, const ioh_radix_node_t *node)
{
    if (!node) {
        return 0;
    }

    /* If this node has a handler, invoke the callback */
    if (node->handler) {
        /* Ensure NUL-terminated pattern (root "/" case) */
        char pattern_copy[MAX_PATTERN_LEN];
        size_t len = state->pattern_len;
        if (len == 0) {
            pattern_copy[0] = '/';
            pattern_copy[1] = '\0';
            len = 1;
        } else {
            memcpy(pattern_copy, state->pattern, len);
            pattern_copy[len] = '\0';
        }

        ioh_route_info_t info = {
            .method = state->method,
            .pattern = pattern_copy,
            .handler = (ioh_handler_fn)(uintptr_t)node->handler,
            .opts = (const ioh_route_opts_t *)(uintptr_t)node->metadata,
            .meta = node->meta,
        };

        int rc = state->fn(&info, state->ctx);
        if (rc != 0) {
            return rc;
        }
    }

    /* Recurse into children */
    for (uint32_t i = 0; i < node->child_count; i++) {
        const ioh_radix_node_t *child = node->children[i];
        size_t saved_len = state->pattern_len;

        /* Build the pattern segment for this child */
        switch (child->type) {
        case IOH_NODE_STATIC: {
            /* Append "/<prefix>" */
            size_t prefix_len = strlen(child->prefix);
            size_t needed = saved_len + 1 + prefix_len;
            if (needed >= MAX_PATTERN_LEN) {
                return -ENOMEM;
            }
            state->pattern[saved_len] = '/';
            memcpy(state->pattern + saved_len + 1, child->prefix, prefix_len);
            state->pattern_len = needed;
            break;
        }
        case IOH_NODE_PARAM: {
            /* Append "/:param_name" */
            size_t name_len = strlen(child->param_name);
            size_t needed = saved_len + 2 + name_len;
            if (needed >= MAX_PATTERN_LEN) {
                return -ENOMEM;
            }
            state->pattern[saved_len] = '/';
            state->pattern[saved_len + 1] = ':';
            memcpy(state->pattern + saved_len + 2, child->param_name, name_len);
            state->pattern_len = needed;
            break;
        }
        case IOH_NODE_WILDCARD: {
            /* Append wildcard: slash + asterisk + param_name */
            size_t name_len = strlen(child->param_name);
            size_t needed = saved_len + 2 + name_len;
            if (needed >= MAX_PATTERN_LEN) {
                return -ENOMEM;
            }
            state->pattern[saved_len] = '/';
            state->pattern[saved_len + 1] = '*';
            memcpy(state->pattern + saved_len + 2, child->param_name, name_len);
            state->pattern_len = needed;
            break;
        }
        }

        int rc = walk_node(state, child);
        state->pattern_len = saved_len;

        if (rc != 0) {
            return rc;
        }
    }

    return 0;
}

/* ---- Public API ---- */

int ioh_router_walk(const ioh_router_t *r, ioh_route_walk_fn fn, void *ctx)
{
    if (!r || !fn) {
        return -EINVAL;
    }

    uint32_t method_count = ioh_router_method_count();

    for (uint32_t m = 0; m < method_count; m++) {
        ioh_radix_tree_t *tree = ioh_router_get_tree(r, (ioh_method_t)m);
        if (!tree || !tree->root) {
            continue;
        }

        walk_state_t state = {
            .fn = fn,
            .ctx = ctx,
            .method = (ioh_method_t)m,
            .pattern_len = 0,
        };

        int rc = walk_node(&state, tree->root);
        if (rc != 0) {
            return rc;
        }
    }

    return 0;
}

/* ---- Count helper ---- */

static void count_node(const ioh_radix_node_t *node, uint32_t *count)
{
    if (!node) {
        return;
    }

    if (node->handler) {
        (*count)++;
    }

    for (uint32_t i = 0; i < node->child_count; i++) {
        count_node(node->children[i], count);
    }
}

uint32_t ioh_router_route_count(const ioh_router_t *r)
{
    if (!r) {
        return 0;
    }

    uint32_t total = 0;
    uint32_t method_count = ioh_router_method_count();

    for (uint32_t m = 0; m < method_count; m++) {
        ioh_radix_tree_t *tree = ioh_router_get_tree(r, (ioh_method_t)m);
        if (!tree || !tree->root) {
            continue;
        }
        count_node(tree->root, &total);
    }

    return total;
}

/* ---- Set meta: find a node matching the pattern ---- */

/**
 * Recursively find the node matching a pattern and set its meta.
 */
static int find_and_set_meta(ioh_radix_node_t *node, const char *pattern, size_t pos,
                             size_t pattern_len, const ioh_route_meta_t *meta)
{
    /* Skip leading slash */
    if (pos < pattern_len && pattern[pos] == '/') {
        pos++;
    }

    /* End of pattern: this node should have the handler */
    if (pos >= pattern_len) {
        if (!node->handler) {
            return -ENOENT;
        }
        node->meta = meta;
        return 0;
    }

    /* Find segment end */
    size_t seg_start = pos;
    size_t seg_end = pos;
    while (seg_end < pattern_len && pattern[seg_end] != '/') {
        seg_end++;
    }
    size_t seg_len = seg_end - seg_start;

    if (seg_len == 0) {
        /* Empty segment (trailing slash) */
        if (!node->handler) {
            return -ENOENT;
        }
        node->meta = meta;
        return 0;
    }

    /* Wildcard segment */
    if (pattern[seg_start] == '*') {
        const char *name = pattern + seg_start + 1;
        size_t name_len = seg_len - 1;

        for (uint32_t i = 0; i < node->child_count; i++) {
            ioh_radix_node_t *child = node->children[i];
            if (child->type == IOH_NODE_WILDCARD && strlen(child->param_name) == name_len &&
                memcmp(child->param_name, name, name_len) == 0) {
                if (!child->handler) {
                    return -ENOENT;
                }
                child->meta = meta;
                return 0;
            }
        }
        return -ENOENT;
    }

    /* Param segment */
    if (pattern[seg_start] == ':') {
        const char *name = pattern + seg_start + 1;
        size_t name_len = seg_len - 1;

        for (uint32_t i = 0; i < node->child_count; i++) {
            ioh_radix_node_t *child = node->children[i];
            if (child->type == IOH_NODE_PARAM && strlen(child->param_name) == name_len &&
                memcmp(child->param_name, name, name_len) == 0) {
                return find_and_set_meta(child, pattern, seg_end, pattern_len, meta);
            }
        }
        return -ENOENT;
    }

    /* Static segment: must handle prefix compression */
    const char *seg = pattern + seg_start;

    for (uint32_t i = 0; i < node->child_count; i++) {
        ioh_radix_node_t *child = node->children[i];
        if (child->type != IOH_NODE_STATIC) {
            continue;
        }

        size_t prefix_len = strlen(child->prefix);
        if (prefix_len == seg_len && memcmp(child->prefix, seg, seg_len) == 0) {
            return find_and_set_meta(child, pattern, seg_end, pattern_len, meta);
        }

        /*
         * Handle prefix compression: if the child's prefix is longer
         * than the segment, it may span multiple segments (compressed).
         */
    }

    return -ENOENT;
}

int ioh_router_set_meta(ioh_router_t *r, ioh_method_t method, const char *pattern,
                       const ioh_route_meta_t *meta)
{
    if (!r || !pattern) {
        return -EINVAL;
    }

    ioh_radix_tree_t *tree = ioh_router_get_tree(r, method);
    if (!tree || !tree->root) {
        return -ENOENT;
    }

    size_t pattern_len = strlen(pattern);
    return find_and_set_meta(tree->root, pattern, 0, pattern_len, meta);
}

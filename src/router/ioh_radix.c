/**
 * @file ioh_radix.c
 * @brief Radix trie (compressed prefix tree) for URL pattern matching.
 */

#include "router/ioh_radix.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* Initial capacity for children array */
constexpr uint32_t RADIX_INITIAL_CAPACITY = 4;

/* ---- Internal: node lifecycle ---- */

static ioh_radix_node_t *ioh_radix_node_create(const char *prefix, size_t prefix_len,
                                             ioh_node_type_t type)
{
    ioh_radix_node_t *node = calloc(1, sizeof(*node));
    if (!node) {
        return nullptr;
    }

    if (prefix && prefix_len > 0) {
        node->prefix = strndup(prefix, prefix_len);
        if (!node->prefix) {
            free(node);
            return nullptr;
        }
    } else {
        node->prefix = strdup("");
        if (!node->prefix) {
            free(node);
            return nullptr;
        }
    }

    node->type = type;
    return node;
}

static void ioh_radix_node_destroy(ioh_radix_node_t *node)
{
    if (!node) {
        return;
    }

    for (uint32_t i = 0; i < node->child_count; i++) {
        ioh_radix_node_destroy(node->children[i]);
    }

    free(node->children);
    free(node->prefix);
    free(node->param_name);
    free(node);
}

/**
 * Add child in sorted order: STATIC < PARAM < WILDCARD by type enum value,
 * alphabetical by prefix within STATIC nodes.
 */
static int ioh_radix_node_add_child(ioh_radix_node_t *parent, ioh_radix_node_t *child)
{
    if (parent->child_count >= parent->child_capacity) {
        uint32_t new_cap = parent->child_capacity == 0 ? RADIX_INITIAL_CAPACITY
                                                       : parent->child_capacity * 2;
        ioh_radix_node_t **new_children =
            realloc(parent->children, (size_t)new_cap * sizeof(*new_children));
        if (!new_children) {
            return -ENOMEM;
        }
        parent->children = new_children;
        parent->child_capacity = new_cap;
    }

    /* Find insertion position: sorted by type, then alphabetical for STATIC */
    uint32_t pos = 0;
    while (pos < parent->child_count) {
        ioh_radix_node_t *existing = parent->children[pos];
        if (child->type < existing->type) {
            break;
        }
        if (child->type == existing->type && child->type == IOH_NODE_STATIC &&
            strcmp(child->prefix, existing->prefix) < 0) {
            break;
        }
        pos++;
    }

    /* Shift elements right */
    for (uint32_t i = parent->child_count; i > pos; i--) {
        parent->children[i] = parent->children[i - 1];
    }

    parent->children[pos] = child;
    parent->child_count++;
    return 0;
}

/* ---- Internal: prefix utilities ---- */

static size_t common_prefix_len(const char *a, const char *b)
{
    size_t i = 0;
    while (a[i] && b[i] && a[i] == b[i]) {
        i++;
    }
    return i;
}

/* ---- Internal: priority propagation ---- */

static void ioh_radix_increment_priority(ioh_radix_node_t *node)
{
    node->priority++;
}

/* ---- Internal: find or split static child ---- */

/**
 * Split a static child node at position `split_pos` in its prefix.
 * Returns the new intermediate node, or nullptr on allocation failure.
 */
static ioh_radix_node_t *split_static_node(ioh_radix_node_t *parent, uint32_t child_idx,
                                          size_t split_pos)
{
    ioh_radix_node_t *existing = parent->children[child_idx];
    size_t existing_len = strlen(existing->prefix);

    /* Create intermediate node with the common prefix */
    ioh_radix_node_t *intermediate =
        ioh_radix_node_create(existing->prefix, split_pos, IOH_NODE_STATIC);
    if (!intermediate) {
        return nullptr;
    }

    /* Shorten existing node prefix to the remainder */
    char *remainder = strdup(existing->prefix + split_pos);
    if (!remainder) {
        ioh_radix_node_destroy(intermediate);
        return nullptr;
    }
    free(existing->prefix);
    existing->prefix = remainder;

    /* Move existing under intermediate */
    int rc = ioh_radix_node_add_child(intermediate, existing);
    if (rc < 0) {
        /* Restore prefix (best effort) */
        (void)existing_len;
        ioh_radix_node_destroy(intermediate);
        return nullptr;
    }

    /* Inherit priority from existing subtree */
    intermediate->priority = existing->priority;

    /* Replace in parent */
    parent->children[child_idx] = intermediate;
    return intermediate;
}

/* ---- Internal: insert segments recursively ---- */

/**
 * Parse the next segment from pattern starting at `pos`.
 * A segment is everything between '/' delimiters.
 * Returns segment start (excluding '/'), sets *seg_len and *next_pos.
 */
static const char *next_segment(const char *pattern, size_t pos, size_t pattern_len,
                                size_t *seg_len, size_t *next_pos)
{
    /* Skip leading slash */
    if (pos < pattern_len && pattern[pos] == '/') {
        pos++;
    }
    if (pos >= pattern_len) {
        *seg_len = 0;
        *next_pos = pattern_len;
        return nullptr;
    }

    const char *start = pattern + pos;
    size_t end = pos;
    while (end < pattern_len && pattern[end] != '/') {
        end++;
    }

    *seg_len = end - pos;
    *next_pos = end;
    return start;
}

static int insert_recursive(ioh_radix_node_t *node, const char *pattern, size_t pos,
                            size_t pattern_len, void *handler, void *metadata,
                            const ioh_route_meta_t *meta);

/**
 * Insert a static segment into the trie at the given node.
 * Handles prefix compression and splitting.
 */
static int insert_static_segment(ioh_radix_node_t *node, const char *seg, size_t seg_len,
                                 const char *pattern, size_t next_pos, size_t pattern_len,
                                 void *handler, void *metadata, const ioh_route_meta_t *meta)
{
    /* Look for existing static child with a common prefix */
    for (uint32_t i = 0; i < node->child_count; i++) {
        ioh_radix_node_t *child = node->children[i];
        if (child->type != IOH_NODE_STATIC) {
            continue;
        }

        size_t cplen = common_prefix_len(seg, child->prefix);
        if (cplen == 0) {
            continue;
        }

        size_t child_prefix_len = strlen(child->prefix);

        if (cplen == child_prefix_len && cplen == seg_len) {
            /* Exact match: descend into this child */
            return insert_recursive(child, pattern, next_pos, pattern_len, handler, metadata, meta);
        }

        if (cplen < child_prefix_len) {
            /* Split existing node at cplen */
            ioh_radix_node_t *intermediate = split_static_node(node, i, cplen);
            if (!intermediate) {
                return -ENOMEM;
            }

            if (cplen == seg_len) {
                /* The new segment IS the common prefix */
                return insert_recursive(intermediate, pattern, next_pos, pattern_len, handler,
                                        metadata, meta);
            }

            /* Create new child for remaining segment */
            ioh_radix_node_t *new_child =
                ioh_radix_node_create(seg + cplen, seg_len - cplen, IOH_NODE_STATIC);
            if (!new_child) {
                return -ENOMEM;
            }

            int rc = ioh_radix_node_add_child(intermediate, new_child);
            if (rc < 0) {
                ioh_radix_node_destroy(new_child);
                return rc;
            }

            return insert_recursive(new_child, pattern, next_pos, pattern_len, handler, metadata,
                                    meta);
        }

        /* cplen == child_prefix_len but cplen < seg_len */
        /* Common prefix matches entire child prefix, descend with remaining */
        /* We need to continue matching the rest of seg against child's children */
        /* Construct a "virtual" remaining segment */
        const char *rest = seg + cplen;
        size_t rest_len = seg_len - cplen;
        return insert_static_segment(child, rest, rest_len, pattern, next_pos, pattern_len, handler,
                                     metadata, meta);
    }

    /* No matching static child found; create new one */
    ioh_radix_node_t *new_child = ioh_radix_node_create(seg, seg_len, IOH_NODE_STATIC);
    if (!new_child) {
        return -ENOMEM;
    }

    int rc = ioh_radix_node_add_child(node, new_child);
    if (rc < 0) {
        ioh_radix_node_destroy(new_child);
        return rc;
    }

    return insert_recursive(new_child, pattern, next_pos, pattern_len, handler, metadata, meta);
}

static int insert_recursive(ioh_radix_node_t *node, const char *pattern, size_t pos,
                            size_t pattern_len, void *handler, void *metadata,
                            const ioh_route_meta_t *meta)
{
    /* If we've consumed the entire pattern, set handler here */
    if (pos >= pattern_len) {
        if (node->handler) {
            return -EEXIST;
        }
        node->handler = handler;
        node->metadata = metadata;
        node->meta = meta;
        ioh_radix_increment_priority(node);
        return 0;
    }

    size_t seg_len = 0;
    size_t next_pos = 0;
    const char *seg = next_segment(pattern, pos, pattern_len, &seg_len, &next_pos);

    if (!seg || seg_len == 0) {
        /* Trailing slash or empty segment */
        if (node->handler) {
            return -EEXIST;
        }
        node->handler = handler;
        node->metadata = metadata;
        node->meta = meta;
        ioh_radix_increment_priority(node);
        return 0;
    }

    if (seg[0] == '*') {
        /* Wildcard segment: must be the last segment */
        if (next_pos < pattern_len) {
            return -EINVAL; /* wildcard not at end */
        }

        const char *param_name = seg + 1;
        size_t param_name_len = seg_len - 1;
        if (param_name_len == 0) {
            return -EINVAL; /* empty wildcard name */
        }

        /* Check for conflicts: existing wildcard with different name */
        for (uint32_t i = 0; i < node->child_count; i++) {
            if (node->children[i]->type == IOH_NODE_WILDCARD) {
                if (strncmp(node->children[i]->param_name, param_name, param_name_len) != 0 ||
                    node->children[i]->param_name[param_name_len] != '\0') {
                    return -EEXIST;
                }
                /* Same wildcard name, set handler */
                if (node->children[i]->handler) {
                    return -EEXIST;
                }
                node->children[i]->handler = handler;
                node->children[i]->metadata = metadata;
                node->children[i]->meta = meta;
                ioh_radix_increment_priority(node->children[i]);
                ioh_radix_increment_priority(node);
                return 0;
            }
        }

        ioh_radix_node_t *wc = ioh_radix_node_create("", 0, IOH_NODE_WILDCARD);
        if (!wc) {
            return -ENOMEM;
        }
        wc->param_name = strndup(param_name, param_name_len);
        if (!wc->param_name) {
            ioh_radix_node_destroy(wc);
            return -ENOMEM;
        }
        wc->handler = handler;
        wc->metadata = metadata;
        wc->meta = meta;
        wc->priority = 1;

        int rc = ioh_radix_node_add_child(node, wc);
        if (rc < 0) {
            ioh_radix_node_destroy(wc);
            return rc;
        }
        ioh_radix_increment_priority(node);
        return 0;
    }

    if (seg[0] == ':') {
        /* Parameter segment */
        const char *param_name = seg + 1;
        size_t param_name_len = seg_len - 1;
        if (param_name_len == 0) {
            return -EINVAL; /* empty param name */
        }

        /* Check for conflicts: existing param with different name */
        for (uint32_t i = 0; i < node->child_count; i++) {
            if (node->children[i]->type == IOH_NODE_PARAM) {
                if (strncmp(node->children[i]->param_name, param_name, param_name_len) != 0 ||
                    node->children[i]->param_name[param_name_len] != '\0') {
                    return -EEXIST;
                }
                /* Same param name, descend */
                ioh_radix_increment_priority(node);
                return insert_recursive(node->children[i], pattern, next_pos, pattern_len, handler,
                                        metadata, meta);
            }
        }

        ioh_radix_node_t *param = ioh_radix_node_create("", 0, IOH_NODE_PARAM);
        if (!param) {
            return -ENOMEM;
        }
        param->param_name = strndup(param_name, param_name_len);
        if (!param->param_name) {
            ioh_radix_node_destroy(param);
            return -ENOMEM;
        }

        int rc = ioh_radix_node_add_child(node, param);
        if (rc < 0) {
            ioh_radix_node_destroy(param);
            return rc;
        }

        ioh_radix_increment_priority(node);
        return insert_recursive(param, pattern, next_pos, pattern_len, handler, metadata, meta);
    }

    /* Static segment */
    ioh_radix_increment_priority(node);
    return insert_static_segment(node, seg, seg_len, pattern, next_pos, pattern_len, handler,
                                 metadata, meta);
}

/* ---- Public API ---- */

ioh_radix_tree_t *ioh_radix_create(void)
{
    ioh_radix_tree_t *tree = calloc(1, sizeof(*tree));
    if (!tree) {
        return nullptr;
    }

    tree->root = ioh_radix_node_create("", 0, IOH_NODE_STATIC);
    if (!tree->root) {
        free(tree);
        return nullptr;
    }

    return tree;
}

void ioh_radix_destroy(ioh_radix_tree_t *tree)
{
    if (!tree) {
        return;
    }
    ioh_radix_node_destroy(tree->root);
    free(tree);
}

int ioh_radix_insert(ioh_radix_tree_t *tree, const char *pattern, void *handler, void *metadata,
                    const ioh_route_meta_t *meta)
{
    if (!tree || !pattern || !handler) {
        return -EINVAL;
    }

    size_t pattern_len = strlen(pattern);
    if (pattern_len == 0 || pattern[0] != '/') {
        return -EINVAL;
    }

    return insert_recursive(tree->root, pattern, 0, pattern_len, handler, metadata, meta);
}

/* ---- Internal: lookup ---- */

static int lookup_recursive(const ioh_radix_node_t *node, const char *path, size_t pos,
                            size_t path_len, ioh_radix_match_t *match);

/**
 * Try to match against static children, handling prefix compression.
 */
static int lookup_static(const ioh_radix_node_t *node, const char *seg, size_t seg_len,
                         const char *path, size_t next_pos, size_t path_len,
                         ioh_radix_match_t *match)
{
    for (uint32_t i = 0; i < node->child_count; i++) {
        const ioh_radix_node_t *child = node->children[i];
        if (child->type != IOH_NODE_STATIC) {
            continue;
        }

        size_t prefix_len = strlen(child->prefix);
        if (prefix_len > seg_len) {
            continue;
        }

        if (memcmp(seg, child->prefix, prefix_len) != 0) {
            continue;
        }

        if (prefix_len == seg_len) {
            /* Full segment match */
            int rc = lookup_recursive(child, path, next_pos, path_len, match);
            if (rc == 0) {
                return 0;
            }
        } else {
            /* Partial match: remaining segment continues in child */
            const char *rest = seg + prefix_len;
            size_t rest_len = seg_len - prefix_len;
            int rc = lookup_static(child, rest, rest_len, path, next_pos, path_len, match);
            if (rc == 0) {
                return 0;
            }
        }
    }
    return -ENOENT;
}

static int lookup_recursive(const ioh_radix_node_t *node, const char *path, size_t pos,
                            size_t path_len, ioh_radix_match_t *match)
{
    /* Consume leading slash */
    if (pos < path_len && path[pos] == '/') {
        pos++;
    }

    /* If we've consumed the entire path, check for handler */
    if (pos >= path_len) {
        if (node->handler) {
            match->handler = node->handler;
            match->metadata = node->metadata;
            match->meta = node->meta;
            return 0;
        }
        return -ENOENT;
    }

    /* Find current segment end */
    size_t seg_start = pos;
    size_t seg_end = pos;
    while (seg_end < path_len && path[seg_end] != '/') {
        seg_end++;
    }
    size_t seg_len = seg_end - seg_start;

    /* 1. Try STATIC children first (highest priority) */
    int rc = lookup_static(node, path + seg_start, seg_len, path, seg_end, path_len, match);
    if (rc == 0) {
        return 0;
    }

    /* 2. Try PARAM children */
    for (uint32_t i = 0; i < node->child_count; i++) {
        const ioh_radix_node_t *child = node->children[i];
        if (child->type != IOH_NODE_PARAM) {
            continue;
        }

        /* Save param */
        if (match->param_count >= IOH_MAX_PATH_PARAMS) {
            return -ENOENT;
        }
        uint32_t saved_count = match->param_count;
        ioh_param_t *p = &match->params[match->param_count++];
        p->name = child->param_name;
        p->name_len = strlen(child->param_name);
        p->value = path + seg_start;
        p->value_len = seg_len;

        rc = lookup_recursive(child, path, seg_end, path_len, match);
        if (rc == 0) {
            return 0;
        }

        /* Backtrack */
        match->param_count = saved_count;
    }

    /* 3. Try WILDCARD children (lowest priority) */
    for (uint32_t i = 0; i < node->child_count; i++) {
        const ioh_radix_node_t *child = node->children[i];
        if (child->type != IOH_NODE_WILDCARD) {
            continue;
        }

        if (!child->handler) {
            continue;
        }

        /* Wildcard captures everything remaining from seg_start */
        if (match->param_count >= IOH_MAX_PATH_PARAMS) {
            return -ENOENT;
        }
        ioh_param_t *p = &match->params[match->param_count++];
        p->name = child->param_name;
        p->name_len = strlen(child->param_name);
        p->value = path + seg_start;
        p->value_len = path_len - seg_start;

        match->handler = child->handler;
        match->metadata = child->metadata;
        match->meta = child->meta;
        return 0;
    }

    return -ENOENT;
}

int ioh_radix_lookup(const ioh_radix_tree_t *tree, const char *path, size_t path_len,
                    ioh_radix_match_t *match)
{
    if (!tree || !path || !match) {
        return -EINVAL;
    }

    memset(match, 0, sizeof(*match));

    if (path_len == 0) {
        return -ENOENT;
    }

    return lookup_recursive(tree->root, path, 0, path_len, match);
}

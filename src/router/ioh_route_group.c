/**
 * @file ioh_route_group.c
 * @brief Route groups with prefix composition and per-group middleware slots.
 */

#include "router/ioh_route_group.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

constexpr uint32_t GROUP_INITIAL_SUBGROUP_CAP = 4;

struct ioh_group {
    ioh_router_t *router;
    ioh_group_t *parent;
    char prefix[IOH_MAX_GROUP_PREFIX];
    ioh_middleware_fn middleware[IOH_MAX_GROUP_MIDDLEWARE];
    uint32_t middleware_count;
    ioh_group_t **subgroups;
    uint32_t subgroup_count;
    uint32_t subgroup_capacity;
};

/* ---- Internal helpers ---- */

/**
 * Build a full path by concatenating the group prefix and the pattern.
 * Returns 0 on success, -EINVAL if the result would overflow.
 */
static int build_full_path(const ioh_group_t *g, const char *pattern, char *out, size_t out_size)
{
    int written = snprintf(out, out_size, "%s%s", g->prefix, pattern);
    if (written < 0 || (size_t)written >= out_size) {
        return -EINVAL;
    }
    return 0;
}

static int group_add_subgroup(ioh_group_t *parent, ioh_group_t *child)
{
    if (parent->subgroup_count >= parent->subgroup_capacity) {
        uint32_t new_cap = parent->subgroup_capacity == 0 ? GROUP_INITIAL_SUBGROUP_CAP
                                                          : parent->subgroup_capacity * 2;
        ioh_group_t **new_arr = realloc(parent->subgroups, (size_t)new_cap * sizeof(*new_arr));
        if (!new_arr) {
            return -ENOMEM;
        }
        parent->subgroups = new_arr;
        parent->subgroup_capacity = new_cap;
    }

    parent->subgroups[parent->subgroup_count++] = child;
    return 0;
}

/* Destructor wrapper for router ownership (void* interface) */
static void group_destroy_wrapper(void *ptr)
{
    ioh_group_destroy((ioh_group_t *)ptr);
}

/* ---- Public API ---- */

ioh_group_t *ioh_router_group(ioh_router_t *r, const char *prefix)
{
    if (!r || !prefix) {
        return nullptr;
    }

    size_t prefix_len = strlen(prefix);
    if (prefix_len == 0 || prefix_len >= IOH_MAX_GROUP_PREFIX) {
        return nullptr;
    }

    ioh_group_t *g = calloc(1, sizeof(*g));
    if (!g) {
        return nullptr;
    }

    g->router = r;
    g->parent = nullptr;
    memcpy(g->prefix, prefix, prefix_len + 1);

    /* Register ownership with the router */
    int rc = ioh_router_own_group(r, g, group_destroy_wrapper);
    if (rc < 0) {
        free(g);
        return nullptr;
    }

    return g;
}

ioh_group_t *ioh_group_subgroup(ioh_group_t *g, const char *prefix)
{
    if (!g || !prefix) {
        return nullptr;
    }

    size_t parent_len = strlen(g->prefix);
    size_t prefix_len = strlen(prefix);

    if (prefix_len == 0 || parent_len + prefix_len >= IOH_MAX_GROUP_PREFIX) {
        return nullptr;
    }

    ioh_group_t *sub = calloc(1, sizeof(*sub));
    if (!sub) {
        return nullptr;
    }

    sub->router = g->router;
    sub->parent = g;

    /* Compose full prefix: parent prefix + this prefix */
    memcpy(sub->prefix, g->prefix, parent_len);
    memcpy(sub->prefix + parent_len, prefix, prefix_len + 1);

    int rc = group_add_subgroup(g, sub);
    if (rc < 0) {
        free(sub);
        return nullptr;
    }

    return sub;
}

int ioh_group_get(ioh_group_t *g, const char *pattern, ioh_handler_fn h)
{
    if (!g || !pattern || !h) {
        return -EINVAL;
    }

    char full[IOH_MAX_GROUP_PREFIX + IOH_MAX_URI_SIZE];
    int rc = build_full_path(g, pattern, full, sizeof(full));
    if (rc < 0) {
        return rc;
    }

    return ioh_router_handle(g->router, IOH_METHOD_GET, full, h);
}

int ioh_group_post(ioh_group_t *g, const char *pattern, ioh_handler_fn h)
{
    if (!g || !pattern || !h) {
        return -EINVAL;
    }

    char full[IOH_MAX_GROUP_PREFIX + IOH_MAX_URI_SIZE];
    int rc = build_full_path(g, pattern, full, sizeof(full));
    if (rc < 0) {
        return rc;
    }

    return ioh_router_handle(g->router, IOH_METHOD_POST, full, h);
}

int ioh_group_put(ioh_group_t *g, const char *pattern, ioh_handler_fn h)
{
    if (!g || !pattern || !h) {
        return -EINVAL;
    }

    char full[IOH_MAX_GROUP_PREFIX + IOH_MAX_URI_SIZE];
    int rc = build_full_path(g, pattern, full, sizeof(full));
    if (rc < 0) {
        return rc;
    }

    return ioh_router_handle(g->router, IOH_METHOD_PUT, full, h);
}

int ioh_group_delete(ioh_group_t *g, const char *pattern, ioh_handler_fn h)
{
    if (!g || !pattern || !h) {
        return -EINVAL;
    }

    char full[IOH_MAX_GROUP_PREFIX + IOH_MAX_URI_SIZE];
    int rc = build_full_path(g, pattern, full, sizeof(full));
    if (rc < 0) {
        return rc;
    }

    return ioh_router_handle(g->router, IOH_METHOD_DELETE, full, h);
}

int ioh_group_patch(ioh_group_t *g, const char *pattern, ioh_handler_fn h)
{
    if (!g || !pattern || !h) {
        return -EINVAL;
    }

    char full[IOH_MAX_GROUP_PREFIX + IOH_MAX_URI_SIZE];
    int rc = build_full_path(g, pattern, full, sizeof(full));
    if (rc < 0) {
        return rc;
    }

    return ioh_router_handle(g->router, IOH_METHOD_PATCH, full, h);
}

int ioh_group_use(ioh_group_t *g, ioh_middleware_fn mw)
{
    if (!g || !mw) {
        return -EINVAL;
    }

    if (g->middleware_count >= IOH_MAX_GROUP_MIDDLEWARE) {
        return -ENOSPC;
    }

    g->middleware[g->middleware_count++] = mw;
    return 0;
}

const char *ioh_group_prefix(const ioh_group_t *g)
{
    if (!g) {
        return nullptr;
    }
    return g->prefix;
}

const ioh_group_t *ioh_group_parent(const ioh_group_t *g)
{
    if (!g) {
        return nullptr;
    }
    return g->parent;
}

uint32_t ioh_group_middleware_count(const ioh_group_t *g)
{
    if (!g) {
        return 0;
    }
    return g->middleware_count;
}

ioh_middleware_fn ioh_group_middleware_at(const ioh_group_t *g, uint32_t idx)
{
    if (!g || idx >= g->middleware_count) {
        return nullptr;
    }
    return g->middleware[idx];
}

int ioh_group_get_with(ioh_group_t *g, const char *pattern, ioh_handler_fn h,
                      const ioh_route_opts_t *opts)
{
    if (!g || !pattern || !h) {
        return -EINVAL;
    }

    char full[IOH_MAX_GROUP_PREFIX + IOH_MAX_URI_SIZE];
    int rc = build_full_path(g, pattern, full, sizeof(full));
    if (rc < 0) {
        return rc;
    }

    return ioh_router_handle_with(g->router, IOH_METHOD_GET, full, h, opts);
}

void ioh_group_destroy(ioh_group_t *g)
{
    if (!g) {
        return;
    }

    /* Recursively destroy subgroups */
    for (uint32_t i = 0; i < g->subgroup_count; i++) {
        ioh_group_destroy(g->subgroups[i]);
    }
    free(g->subgroups);
    free(g);
}

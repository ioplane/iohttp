/**
 * @file io_router.c
 * @brief Public router API with per-method radix trees.
 */

#include "router/io_router.h"
#include "middleware/io_middleware.h"
#include "router/io_radix.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Number of method slots (IO_METHOD_GET through IO_METHOD_CONNECT) */
constexpr uint32_t ROUTER_METHOD_COUNT = 9;

constexpr uint32_t ROUTER_INITIAL_GROUP_CAP = 8;

/* Group entry with destructor to avoid circular dependency */
typedef struct {
    void *group;
    void (*destroy)(void *);
} io_owned_group_t;

struct io_router {
    io_radix_tree_t *trees[9]; /* one per io_method_t value */
    io_owned_group_t *groups;  /* owned route groups */
    uint32_t group_count;
    uint32_t group_capacity;
    io_middleware_fn global_mw[IO_MAX_GLOBAL_MIDDLEWARE];
    uint32_t global_mw_count;
    io_error_handler_fn error_handler;
    io_handler_fn not_found_handler;
    io_handler_fn method_not_allowed_handler;
};

/* ---- Path normalization ---- */

int io_path_normalize(const char *path, size_t path_len, char *out, size_t out_size,
                      size_t *out_len)
{
    if (!path || !out || !out_len || out_size == 0) {
        return -EINVAL;
    }

    if (path_len == 0 || path[0] != '/') {
        return -EINVAL;
    }

    /* Check for NUL bytes embedded in path */
    for (size_t i = 0; i < path_len; i++) {
        if (path[i] == '\0') {
            return -EINVAL;
        }
    }

    /*
     * Segment stack: store start offsets and lengths of each segment
     * after processing. Max depth is bounded by path_len.
     */
    size_t seg_starts[256];
    size_t seg_lens[256];
    uint32_t depth = 0;

    size_t pos = 0;
    while (pos < path_len) {
        /* Skip slashes */
        while (pos < path_len && path[pos] == '/') {
            pos++;
        }
        if (pos >= path_len) {
            break;
        }

        /* Find segment end */
        size_t seg_start = pos;
        while (pos < path_len && path[pos] != '/') {
            pos++;
        }
        size_t seg_len = pos - seg_start;

        /* Handle "." -- skip */
        if (seg_len == 1 && path[seg_start] == '.') {
            continue;
        }

        /* Handle ".." -- go up */
        if (seg_len == 2 && path[seg_start] == '.' && path[seg_start + 1] == '.') {
            if (depth == 0) {
                return -EINVAL; /* traversal above root */
            }
            depth--;
            continue;
        }

        if (depth >= 256) {
            return -EINVAL; /* path too deep */
        }
        seg_starts[depth] = seg_start;
        seg_lens[depth] = seg_len;
        depth++;
    }

    /* Build output (always without trailing slash, except root) */
    size_t written = 0;

    if (depth == 0) {
        /* Root path */
        if (out_size < 2) {
            return -EINVAL;
        }
        out[0] = '/';
        written = 1;
    } else {
        for (uint32_t i = 0; i < depth; i++) {
            /* Need: '/' + segment */
            if (written + 1 + seg_lens[i] >= out_size) {
                return -EINVAL; /* buffer too small */
            }
            out[written++] = '/';
            memcpy(out + written, path + seg_starts[i], seg_lens[i]);
            written += seg_lens[i];
        }
    }

    out[written] = '\0';
    *out_len = written;
    return 0;
}

/* ---- Internal helpers ---- */

static bool path_has_null_byte(const char *path, size_t path_len)
{
    return memchr(path, '\0', path_len) != nullptr;
}

/**
 * Check for %00 (URL-encoded NUL) in path.
 */
static bool path_has_encoded_null(const char *path, size_t path_len)
{
    for (size_t i = 0; i + 2 < path_len; i++) {
        if (path[i] == '%' && path[i + 1] == '0' && path[i + 2] == '0') {
            return true;
        }
    }
    return false;
}

static int router_add_route(io_router_t *r, io_method_t method, const char *pattern,
                            io_handler_fn h, const io_route_opts_t *opts)
{
    if (!r || !pattern || !h) {
        return -EINVAL;
    }

    if (method >= ROUTER_METHOD_COUNT) {
        return -EINVAL;
    }

    /* Lazily create tree for this method */
    if (!r->trees[method]) {
        r->trees[method] = io_radix_create();
        if (!r->trees[method]) {
            return -ENOMEM;
        }
    }

    /* Cast handler to void* for radix trie storage */
    const io_route_meta_t *meta = (opts != nullptr) ? opts->meta : nullptr;
    return io_radix_insert(r->trees[method], pattern, (void *)(uintptr_t)h, (void *)(uintptr_t)opts,
                           meta);
}

static const char *method_name_for_index(uint32_t idx)
{
    switch (idx) {
    case IO_METHOD_GET:
        return "GET";
    case IO_METHOD_POST:
        return "POST";
    case IO_METHOD_PUT:
        return "PUT";
    case IO_METHOD_DELETE:
        return "DELETE";
    case IO_METHOD_PATCH:
        return "PATCH";
    case IO_METHOD_HEAD:
        return "HEAD";
    case IO_METHOD_OPTIONS:
        return "OPTIONS";
    case IO_METHOD_TRACE:
        return "TRACE";
    case IO_METHOD_CONNECT:
        return "CONNECT";
    default:
        return nullptr;
    }
}

/* ---- Public API: lifecycle ---- */

io_router_t *io_router_create(void)
{
    io_router_t *r = calloc(1, sizeof(*r));
    return r;
}

void io_router_destroy(io_router_t *router)
{
    if (!router) {
        return;
    }

    /* Destroy owned groups (top-level only; they destroy their subgroups) */
    for (uint32_t i = 0; i < router->group_count; i++) {
        router->groups[i].destroy(router->groups[i].group);
    }
    free(router->groups);

    for (uint32_t i = 0; i < ROUTER_METHOD_COUNT; i++) {
        io_radix_destroy(router->trees[i]);
    }
    free(router);
}

/* ---- Public API: method-specific registration ---- */

int io_router_get(io_router_t *r, const char *pattern, io_handler_fn h)
{
    return router_add_route(r, IO_METHOD_GET, pattern, h, nullptr);
}

int io_router_post(io_router_t *r, const char *pattern, io_handler_fn h)
{
    return router_add_route(r, IO_METHOD_POST, pattern, h, nullptr);
}

int io_router_put(io_router_t *r, const char *pattern, io_handler_fn h)
{
    return router_add_route(r, IO_METHOD_PUT, pattern, h, nullptr);
}

int io_router_delete(io_router_t *r, const char *pattern, io_handler_fn h)
{
    return router_add_route(r, IO_METHOD_DELETE, pattern, h, nullptr);
}

int io_router_patch(io_router_t *r, const char *pattern, io_handler_fn h)
{
    return router_add_route(r, IO_METHOD_PATCH, pattern, h, nullptr);
}

int io_router_head(io_router_t *r, const char *pattern, io_handler_fn h)
{
    return router_add_route(r, IO_METHOD_HEAD, pattern, h, nullptr);
}

int io_router_options(io_router_t *r, const char *pattern, io_handler_fn h)
{
    return router_add_route(r, IO_METHOD_OPTIONS, pattern, h, nullptr);
}

int io_router_handle(io_router_t *r, io_method_t method, const char *pattern, io_handler_fn h)
{
    return router_add_route(r, method, pattern, h, nullptr);
}

int io_router_get_with(io_router_t *r, const char *pattern, io_handler_fn h,
                       const io_route_opts_t *opts)
{
    return router_add_route(r, IO_METHOD_GET, pattern, h, opts);
}

int io_router_handle_with(io_router_t *r, io_method_t method, const char *pattern, io_handler_fn h,
                          const io_route_opts_t *opts)
{
    return router_add_route(r, method, pattern, h, opts);
}

int io_router_post_with(io_router_t *r, const char *pattern, io_handler_fn h,
                        const io_route_opts_t *opts)
{
    return router_add_route(r, IO_METHOD_POST, pattern, h, opts);
}

int io_router_put_with(io_router_t *r, const char *pattern, io_handler_fn h,
                       const io_route_opts_t *opts)
{
    return router_add_route(r, IO_METHOD_PUT, pattern, h, opts);
}

int io_router_delete_with(io_router_t *r, const char *pattern, io_handler_fn h,
                          const io_route_opts_t *opts)
{
    return router_add_route(r, IO_METHOD_DELETE, pattern, h, opts);
}

int io_router_patch_with(io_router_t *r, const char *pattern, io_handler_fn h,
                         const io_route_opts_t *opts)
{
    return router_add_route(r, IO_METHOD_PATCH, pattern, h, opts);
}

int io_router_own_group(io_router_t *r, void *group, void (*destroy)(void *))
{
    if (!r || !group || !destroy) {
        return -EINVAL;
    }

    if (r->group_count >= r->group_capacity) {
        uint32_t new_cap = r->group_capacity == 0 ? ROUTER_INITIAL_GROUP_CAP
                                                  : r->group_capacity * 2;
        io_owned_group_t *new_arr = realloc(r->groups, (size_t)new_cap * sizeof(*new_arr));
        if (!new_arr) {
            return -ENOMEM;
        }
        r->groups = new_arr;
        r->group_capacity = new_cap;
    }

    r->groups[r->group_count++] = (io_owned_group_t){
        .group = group,
        .destroy = destroy,
    };
    return 0;
}

/* ---- Internal accessors (used by io_route_inspect.c) ---- */

io_radix_tree_t *io_router_get_tree(const io_router_t *r, io_method_t method)
{
    if (!r || method >= ROUTER_METHOD_COUNT) {
        return nullptr;
    }
    return r->trees[method];
}

uint32_t io_router_method_count(void)
{
    return ROUTER_METHOD_COUNT;
}

/* ---- Middleware & error handler registration ---- */

int io_router_use(io_router_t *r, io_middleware_fn mw)
{
    if (!r || !mw) {
        return -EINVAL;
    }
    if (r->global_mw_count >= IO_MAX_GLOBAL_MIDDLEWARE) {
        return -ENOSPC;
    }
    r->global_mw[r->global_mw_count++] = mw;
    return 0;
}

void io_router_set_error_handler(io_router_t *r, io_error_handler_fn h)
{
    if (r) {
        r->error_handler = h;
    }
}

void io_router_set_not_found(io_router_t *r, io_handler_fn h)
{
    if (r) {
        r->not_found_handler = h;
    }
}

void io_router_set_method_not_allowed(io_router_t *r, io_handler_fn h)
{
    if (r) {
        r->method_not_allowed_handler = h;
    }
}

io_middleware_fn *io_router_global_middleware(const io_router_t *r, uint32_t *count)
{
    if (!r || !count) {
        return nullptr;
    }
    *count = r->global_mw_count;
    /* Cast away const — caller must not modify through this pointer */
    return (io_middleware_fn *)r->global_mw;
}

io_error_handler_fn io_router_error_handler(const io_router_t *r)
{
    if (!r) {
        return nullptr;
    }
    return r->error_handler;
}

io_handler_fn io_router_not_found_handler(const io_router_t *r)
{
    if (!r) {
        return nullptr;
    }
    return r->not_found_handler;
}

io_handler_fn io_router_method_not_allowed_handler(const io_router_t *r)
{
    if (!r) {
        return nullptr;
    }
    return r->method_not_allowed_handler;
}

/**
 * Copy param values from transient buffer into match-owned param_storage.
 * Prevents dangling pointers when the source buffer (e.g. normalized[]) goes
 * out of scope. Returns 0 on success, -ENOMEM if storage is exhausted.
 */
static int stabilize_param_values(io_route_match_t *m)
{
    size_t offset = 0;
    for (uint32_t i = 0; i < m->param_count; i++) {
        size_t vlen = m->params[i].value_len;
        if (offset + vlen >= IO_MAX_PARAM_STORAGE) {
            return -ENOMEM;
        }
        memcpy(m->param_storage + offset, m->params[i].value, vlen);
        m->param_storage[offset + vlen] = '\0';
        m->params[i].value = m->param_storage + offset;
        offset += vlen + 1; /* +1 for NUL terminator */
    }
    m->param_storage_used = offset;
    return 0;
}

/* ---- Public API: dispatch ---- */

io_route_match_t io_router_dispatch(const io_router_t *r, io_method_t method, const char *path,
                                    size_t path_len)
{
    io_route_match_t result;
    memset(&result, 0, sizeof(result));
    result.status = IO_MATCH_NOT_FOUND;

    if (!r || !path || path_len == 0) {
        return result;
    }

    /* Security: reject paths with NUL bytes */
    if (path_has_null_byte(path, path_len)) {
        return result;
    }

    /* Security: reject paths with URL-encoded NUL (%00) */
    if (path_has_encoded_null(path, path_len)) {
        return result;
    }

    /* Normalize path (strips trailing slash except root) */
    char normalized[IO_MAX_URI_SIZE];
    size_t norm_len = 0;
    int rc = io_path_normalize(path, path_len, normalized, sizeof(normalized), &norm_len);
    if (rc < 0) {
        return result;
    }

    /*
     * Trailing-slash redirect detection: if the original path had a
     * trailing slash but normalization removed it, the client should
     * be redirected to the canonical (no-trailing-slash) form.
     */
    bool had_trailing_slash = (path_len > 1 && path[path_len - 1] == '/');
    bool norm_has_trailing = (norm_len > 1 && normalized[norm_len - 1] == '/');
    bool needs_redirect = had_trailing_slash && !norm_has_trailing;

    if (needs_redirect) {
        /* Check if the normalized path matches in any tree */
        for (uint32_t i = 0; i < ROUTER_METHOD_COUNT; i++) {
            if (!r->trees[i]) {
                continue;
            }
            io_radix_match_t rmatch;
            rc = io_radix_lookup(r->trees[i], normalized, norm_len, &rmatch);
            if (rc == 0) {
                result.status = IO_MATCH_REDIRECT;
                memcpy(result.redirect_path, normalized, norm_len + 1);
                return result;
            }
        }
        /* No match even without trailing slash; fall through to 405/404 */
    }

    /* Try the requested method's tree */
    if (method < ROUTER_METHOD_COUNT && r->trees[method]) {
        io_radix_match_t rmatch;
        rc = io_radix_lookup(r->trees[method], normalized, norm_len, &rmatch);
        if (rc == 0) {
            result.status = IO_MATCH_FOUND;
            result.handler = (io_handler_fn)(uintptr_t)rmatch.handler;
            result.opts = (const io_route_opts_t *)(uintptr_t)rmatch.metadata;
            result.meta = rmatch.meta;
            result.param_count = rmatch.param_count;
            memcpy(result.params, rmatch.params, rmatch.param_count * sizeof(io_param_t));
            (void)stabilize_param_values(&result);
            return result;
        }
    }

    /* Auto-HEAD: if HEAD request, try GET tree */
    if (method == IO_METHOD_HEAD && r->trees[IO_METHOD_GET]) {
        io_radix_match_t rmatch;
        rc = io_radix_lookup(r->trees[IO_METHOD_GET], normalized, norm_len, &rmatch);
        if (rc == 0) {
            result.status = IO_MATCH_FOUND;
            result.handler = (io_handler_fn)(uintptr_t)rmatch.handler;
            result.opts = (const io_route_opts_t *)(uintptr_t)rmatch.metadata;
            result.meta = rmatch.meta;
            result.param_count = rmatch.param_count;
            memcpy(result.params, rmatch.params, rmatch.param_count * sizeof(io_param_t));
            (void)stabilize_param_values(&result);
            return result;
        }
    }

    /* Auto-405: check all other method trees for this path */
    size_t methods_offset = 0;
    bool any_method_matched = false;

    for (uint32_t i = 0; i < ROUTER_METHOD_COUNT; i++) {
        if (i == (uint32_t)method || !r->trees[i]) {
            continue;
        }

        io_radix_match_t rmatch;
        rc = io_radix_lookup(r->trees[i], normalized, norm_len, &rmatch);
        if (rc == 0) {
            const char *mname = method_name_for_index(i);
            if (mname) {
                if (any_method_matched) {
                    int written = snprintf(result.allowed_methods + methods_offset,
                                           sizeof(result.allowed_methods) - methods_offset, ", %s",
                                           mname);
                    if (written > 0) {
                        methods_offset += (size_t)written;
                    }
                } else {
                    int written = snprintf(result.allowed_methods + methods_offset,
                                           sizeof(result.allowed_methods) - methods_offset, "%s",
                                           mname);
                    if (written > 0) {
                        methods_offset += (size_t)written;
                    }
                }
                any_method_matched = true;
            }
        }
    }

    if (any_method_matched) {
        result.status = IO_MATCH_METHOD_NOT_ALLOWED;
    }

    return result;
}

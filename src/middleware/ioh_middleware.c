/**
 * @file ioh_middleware.c
 * @brief Middleware chain execution: global -> group -> handler.
 */

#include "middleware/ioh_middleware.h"

#include <errno.h>
#include <string.h>

/* Thread-local chain context for the "next" function */
static _Thread_local ioh_chain_t *tl_chain;
static _Thread_local ioh_ctx_t *tl_ctx;

static int chain_next(ioh_ctx_t *c)
{
    ioh_chain_t *chain = tl_chain;

    if (chain->current >= chain->count) {
        /* All middleware done — call final handler */
        return chain->handler(c);
    }

    ioh_middleware_fn mw = chain->middleware[chain->current++];
    return mw(c, chain_next);
}

int ioh_chain_execute(ioh_ctx_t *c, ioh_middleware_fn *global_mw, uint32_t global_count,
                     ioh_middleware_fn *group_mw, uint32_t group_count, ioh_handler_fn handler)
{
    if (!c || !handler) {
        return -EINVAL;
    }

    if (global_count > IOH_MAX_GLOBAL_MIDDLEWARE) {
        return -ENOSPC;
    }

    if (group_count > IOH_MAX_GROUP_MIDDLEWARE) {
        return -ENOSPC;
    }

    /* Build flat middleware array: global + group */
    ioh_middleware_fn all[IOH_MAX_GLOBAL_MIDDLEWARE + IOH_MAX_GROUP_MIDDLEWARE];
    uint32_t total = 0;

    for (uint32_t i = 0; i < global_count; i++) {
        all[total++] = global_mw[i];
    }
    for (uint32_t i = 0; i < group_count; i++) {
        all[total++] = group_mw[i];
    }

    ioh_chain_t chain = {
        .middleware = all,
        .count = total,
        .current = 0,
        .handler = handler,
    };

    /* Save previous chain context (supports nested execution) */
    ioh_chain_t *prev = tl_chain;
    ioh_ctx_t *prev_ctx = tl_ctx;
    tl_chain = &chain;
    tl_ctx = c;

    int rc = chain_next(c);

    tl_chain = prev;
    tl_ctx = prev_ctx;
    return rc;
}

/**
 * @file io_middleware.c
 * @brief Middleware chain execution: global -> group -> handler.
 */

#include "middleware/io_middleware.h"

#include <errno.h>
#include <string.h>

/* Thread-local chain context for the "next" function */
static _Thread_local io_chain_t *tl_chain;
static _Thread_local io_ctx_t *tl_ctx;

static int chain_next(io_ctx_t *c)
{
    io_chain_t *chain = tl_chain;

    if (chain->current >= chain->count) {
        /* All middleware done — call final handler */
        return chain->handler(c);
    }

    io_middleware_fn mw = chain->middleware[chain->current++];
    return mw(c, chain_next);
}

int io_chain_execute(io_ctx_t *c, io_middleware_fn *global_mw,
                     uint32_t global_count, io_middleware_fn *group_mw, uint32_t group_count,
                     io_handler_fn handler)
{
    if (!c || !handler) {
        return -EINVAL;
    }

    if (global_count > IO_MAX_GLOBAL_MIDDLEWARE) {
        return -ENOSPC;
    }

    if (group_count > IO_MAX_GROUP_MIDDLEWARE) {
        return -ENOSPC;
    }

    /* Build flat middleware array: global + group */
    io_middleware_fn all[IO_MAX_GLOBAL_MIDDLEWARE + IO_MAX_GROUP_MIDDLEWARE];
    uint32_t total = 0;

    for (uint32_t i = 0; i < global_count; i++) {
        all[total++] = global_mw[i];
    }
    for (uint32_t i = 0; i < group_count; i++) {
        all[total++] = group_mw[i];
    }

    io_chain_t chain = {
        .middleware = all,
        .count = total,
        .current = 0,
        .handler = handler,
    };

    /* Save previous chain context (supports nested execution) */
    io_chain_t *prev = tl_chain;
    io_ctx_t *prev_ctx = tl_ctx;
    tl_chain = &chain;
    tl_ctx = c;

    int rc = chain_next(c);

    tl_chain = prev;
    tl_ctx = prev_ctx;
    return rc;
}

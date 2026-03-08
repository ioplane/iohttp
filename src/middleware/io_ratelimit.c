/**
 * @file io_ratelimit.c
 * @brief Token-bucket rate limiting middleware implementation.
 */

#include "middleware/io_ratelimit.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

constexpr uint32_t IO_RATELIMIT_BUCKETS = 1024;

typedef struct {
    char key[128];
    double tokens;
    struct timespec last_refill;
    bool in_use;
} io_bucket_t;

typedef struct {
    io_ratelimit_config_t cfg;
    io_bucket_t buckets[1024];
} io_ratelimit_state_t;

static _Thread_local io_ratelimit_state_t *tl_rl_state;

static uint32_t hash_key(const char *key)
{
    uint32_t h = 5381;
    for (const char *p = key; *p != '\0'; p++) {
        h = ((h << 5) + h) + (uint32_t)(unsigned char)*p;
    }
    return h % IO_RATELIMIT_BUCKETS;
}

static io_bucket_t *find_bucket(io_ratelimit_state_t *st, const char *key)
{
    uint32_t idx = hash_key(key);

    /* linear probing */
    for (uint32_t i = 0; i < IO_RATELIMIT_BUCKETS; i++) {
        uint32_t slot = (idx + i) % IO_RATELIMIT_BUCKETS;
        io_bucket_t *b = &st->buckets[slot];

        if (!b->in_use) {
            /* new bucket */
            strncpy(b->key, key, sizeof(b->key) - 1);
            b->key[sizeof(b->key) - 1] = '\0';
            b->tokens = (double)st->cfg.burst;
            clock_gettime(CLOCK_MONOTONIC, &b->last_refill);
            b->in_use = true;
            return b;
        }
        if (strncmp(b->key, key, sizeof(b->key)) == 0) {
            return b;
        }
    }
    return nullptr; /* table full */
}

static void refill_bucket(io_bucket_t *b, const io_ratelimit_config_t *cfg)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);

    double elapsed = (double)(now.tv_sec - b->last_refill.tv_sec) +
                     (double)(now.tv_nsec - b->last_refill.tv_nsec) / 1e9;

    if (elapsed > 0.0) {
        b->tokens += elapsed * (double)cfg->requests_per_second;
        if (b->tokens > (double)cfg->burst) {
            b->tokens = (double)cfg->burst;
        }
        b->last_refill = now;
    }
}

bool io_ratelimit_check(const char *key)
{
    if (tl_rl_state == nullptr || key == nullptr) {
        return false;
    }

    io_bucket_t *b = find_bucket(tl_rl_state, key);
    if (b == nullptr) {
        return false;
    }

    refill_bucket(b, &tl_rl_state->cfg);

    if (b->tokens >= 1.0) {
        b->tokens -= 1.0;
        return true;
    }
    return false;
}

static int ratelimit_middleware(io_ctx_t *c, io_handler_fn next)
{
    /* use Host header as key, fallback to "default" */
    const char *key = io_request_header(c->req, "Host");
    if (key == nullptr) {
        key = "default";
    }

    if (!io_ratelimit_check(key)) {
        c->resp->status = 429;
        (void)io_response_set_header(c->resp, "Retry-After", "1");
        return 0;
    }

    return next(c);
}

void io_ratelimit_config_init(io_ratelimit_config_t *cfg)
{
    if (cfg == nullptr) {
        return;
    }
    cfg->requests_per_second = 10;
    cfg->burst = 20;
}

io_middleware_fn io_ratelimit_create(const io_ratelimit_config_t *cfg)
{
    if (cfg == nullptr) {
        return nullptr;
    }

    io_ratelimit_state_t *st = calloc(1, sizeof(*st));
    if (st == nullptr) {
        return nullptr;
    }
    st->cfg = *cfg;
    tl_rl_state = st;
    return ratelimit_middleware;
}

void io_ratelimit_destroy(void)
{
    free(tl_rl_state);
    tl_rl_state = nullptr;
}

void io_ratelimit_reset(void)
{
    if (tl_rl_state == nullptr) {
        return;
    }
    memset(tl_rl_state->buckets, 0, sizeof(tl_rl_state->buckets));
}

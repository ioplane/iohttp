/**
 * @file io_security.c
 * @brief Security headers middleware implementation.
 */

#include "middleware/io_security.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char *csp;
    bool hsts;
    uint32_t hsts_max_age;
    char *frame_options;
    char *referrer_policy;
    bool nosniff;
} io_security_state_t;

static _Thread_local io_security_state_t *tl_sec_state;

static int security_middleware(io_request_t *req, io_response_t *resp,
                               int (*next)(io_request_t *, io_response_t *))
{
    int rc = next(req, resp);
    if (rc != 0) {
        return rc;
    }

    io_security_state_t *st = tl_sec_state;

    if (st->csp != nullptr) {
        rc = io_response_set_header(resp, "Content-Security-Policy", st->csp);
        if (rc != 0) {
            return rc;
        }
    }

    if (st->hsts) {
        char buf[64];
        snprintf(buf, sizeof(buf), "max-age=%u; includeSubDomains",
                 st->hsts_max_age);
        rc = io_response_set_header(resp, "Strict-Transport-Security", buf);
        if (rc != 0) {
            return rc;
        }
    }

    if (st->frame_options != nullptr) {
        rc = io_response_set_header(resp, "X-Frame-Options",
                                    st->frame_options);
        if (rc != 0) {
            return rc;
        }
    }

    if (st->referrer_policy != nullptr) {
        rc = io_response_set_header(resp, "Referrer-Policy",
                                    st->referrer_policy);
        if (rc != 0) {
            return rc;
        }
    }

    if (st->nosniff) {
        rc = io_response_set_header(resp, "X-Content-Type-Options",
                                    "nosniff");
        if (rc != 0) {
            return rc;
        }
    }

    return 0;
}

void io_security_config_init(io_security_config_t *cfg)
{
    if (cfg == nullptr) {
        return;
    }
    memset(cfg, 0, sizeof(*cfg));
    cfg->hsts = true;
    cfg->hsts_max_age = 31536000;
    cfg->frame_options = "DENY";
    cfg->referrer_policy = "strict-origin-when-cross-origin";
    cfg->nosniff = true;
}

io_middleware_fn io_security_create(const io_security_config_t *cfg)
{
    if (cfg == nullptr) {
        return nullptr;
    }

    io_security_state_t *st = calloc(1, sizeof(*st));
    if (st == nullptr) {
        return nullptr;
    }

    if (cfg->csp != nullptr) {
        st->csp = strdup(cfg->csp);
    }
    st->hsts = cfg->hsts;
    st->hsts_max_age = cfg->hsts_max_age;
    if (cfg->frame_options != nullptr) {
        st->frame_options = strdup(cfg->frame_options);
    }
    if (cfg->referrer_policy != nullptr) {
        st->referrer_policy = strdup(cfg->referrer_policy);
    }
    st->nosniff = cfg->nosniff;

    tl_sec_state = st;
    return security_middleware;
}

void io_security_destroy(void)
{
    if (tl_sec_state == nullptr) {
        return;
    }
    free(tl_sec_state->csp);
    free(tl_sec_state->frame_options);
    free(tl_sec_state->referrer_policy);
    free(tl_sec_state);
    tl_sec_state = nullptr;
}

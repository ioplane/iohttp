/**
 * @file ioh_cors.c
 * @brief CORS middleware implementation.
 */

#include "middleware/ioh_cors.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static _Thread_local ioh_cors_config_t *tl_cors_cfg;

static const char *DEFAULT_METHODS = "GET, POST, PUT, DELETE, PATCH";
static const char *DEFAULT_HEADERS = "Content-Type, Authorization";

static bool origin_allowed(const ioh_cors_config_t *cfg, const char *origin)
{
    if (cfg->origin_count == 0 || cfg->allowed_origins == nullptr) {
        return true; /* wildcard */
    }
    for (uint32_t i = 0; i < cfg->origin_count; i++) {
        if (strcmp(cfg->allowed_origins[i], origin) == 0) {
            return true;
        }
    }
    return false;
}

static int set_origin_header(ioh_response_t *resp, const ioh_cors_config_t *cfg, const char *origin)
{
    if (cfg->allow_credentials && origin != nullptr) {
        /* credentials mode: echo back the matched origin, never "*" */
        return ioh_response_set_header(resp, "Access-Control-Allow-Origin", origin);
    }
    if (cfg->origin_count == 0 || cfg->allowed_origins == nullptr) {
        return ioh_response_set_header(resp, "Access-Control-Allow-Origin", "*");
    }
    /* specific origin list: echo back the matched one */
    if (origin != nullptr) {
        return ioh_response_set_header(resp, "Access-Control-Allow-Origin", origin);
    }
    return 0;
}

static int cors_middleware(ioh_ctx_t *c, ioh_handler_fn next)
{
    ioh_cors_config_t *cfg = tl_cors_cfg;
    const char *origin = ioh_request_header(c->req, "Origin");

    /* no Origin header — not a CORS request, pass through */
    if (origin == nullptr) {
        return next(c);
    }

    if (!origin_allowed(cfg, origin)) {
        /* origin not allowed — don't set CORS headers */
        if (c->req->method == IOH_METHOD_OPTIONS) {
            c->resp->status = 204;
            return 0;
        }
        return next(c);
    }

    if (c->req->method == IOH_METHOD_OPTIONS) {
        /* preflight request */
        c->resp->status = 204;

        int rc = set_origin_header(c->resp, cfg, origin);
        if (rc != 0) {
            return rc;
        }

        /* methods */
        if (cfg->method_count > 0 && cfg->allowed_methods != nullptr) {
            char buf[512] = {0};
            size_t off = 0;
            for (uint32_t i = 0; i < cfg->method_count; i++) {
                if (i > 0) {
                    off += (size_t)snprintf(buf + off, sizeof(buf) - off, ", ");
                }
                off +=
                    (size_t)snprintf(buf + off, sizeof(buf) - off, "%s", cfg->allowed_methods[i]);
            }
            rc = ioh_response_set_header(c->resp, "Access-Control-Allow-Methods", buf);
        } else {
            rc = ioh_response_set_header(c->resp, "Access-Control-Allow-Methods", DEFAULT_METHODS);
        }
        if (rc != 0) {
            return rc;
        }

        /* headers */
        if (cfg->header_count > 0 && cfg->allowed_headers != nullptr) {
            char buf[512] = {0};
            size_t off = 0;
            for (uint32_t i = 0; i < cfg->header_count; i++) {
                if (i > 0) {
                    off += (size_t)snprintf(buf + off, sizeof(buf) - off, ", ");
                }
                off +=
                    (size_t)snprintf(buf + off, sizeof(buf) - off, "%s", cfg->allowed_headers[i]);
            }
            rc = ioh_response_set_header(c->resp, "Access-Control-Allow-Headers", buf);
        } else {
            rc = ioh_response_set_header(c->resp, "Access-Control-Allow-Headers", DEFAULT_HEADERS);
        }
        if (rc != 0) {
            return rc;
        }

        /* max-age */
        char age_buf[32];
        snprintf(age_buf, sizeof(age_buf), "%u", cfg->max_age_seconds);
        rc = ioh_response_set_header(c->resp, "Access-Control-Max-Age", age_buf);
        if (rc != 0) {
            return rc;
        }

        if (cfg->allow_credentials) {
            rc = ioh_response_set_header(c->resp, "Access-Control-Allow-Credentials", "true");
            if (rc != 0) {
                return rc;
            }
        }

        return 0;
    }

    /* simple/actual request */
    int rc = next(c);
    if (rc != 0) {
        return rc;
    }

    return set_origin_header(c->resp, cfg, origin);
}

void ioh_cors_config_init(ioh_cors_config_t *cfg)
{
    if (cfg == nullptr) {
        return;
    }
    memset(cfg, 0, sizeof(*cfg));
    cfg->max_age_seconds = 86400;
}

ioh_middleware_fn ioh_cors_create(const ioh_cors_config_t *cfg)
{
    if (cfg == nullptr) {
        return nullptr;
    }

    ioh_cors_config_t *copy = malloc(sizeof(*copy));
    if (copy == nullptr) {
        return nullptr;
    }
    *copy = *cfg;
    tl_cors_cfg = copy;
    return cors_middleware;
}

void ioh_cors_destroy(void)
{
    free(tl_cors_cfg);
    tl_cors_cfg = nullptr;
}

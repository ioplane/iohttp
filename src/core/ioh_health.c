/**
 * @file ioh_health.c
 * @brief Health check endpoint framework implementation.
 */

#include "core/ioh_health.h"

#include "core/ioh_ctx.h"
#include "core/ioh_server.h"
#include "router/ioh_router.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#ifdef IOHTTP_HAVE_YYJSON
#    include <yyjson.h>
#endif

/* ---- JSON escape helper (for non-yyjson fallback) ---- */

#ifndef IOHTTP_HAVE_YYJSON

/**
 * Escape a string for JSON output. Handles " and \ characters.
 * Writes at most dst_size-1 bytes and always null-terminates.
 * Returns the number of characters that would have been written
 * (excluding NUL), or -1 on nullptr input.
 */
static int json_escape(char *dst, size_t dst_size, const char *src)
{
    if (dst == nullptr || src == nullptr || dst_size == 0) {
        return -1;
    }

    size_t written = 0;
    for (const char *p = src; *p != '\0'; p++) {
        if (*p == '"' || *p == '\\') {
            if (written + 2 >= dst_size) {
                dst[written] = '\0';
                return -1;
            }
            dst[written++] = '\\';
            dst[written++] = *p;
        } else if ((unsigned char)*p < 0x20) {
            /* Control characters: emit \uXXXX */
            if (written + 6 >= dst_size) {
                dst[written] = '\0';
                return -1;
            }
            int n = snprintf(dst + written, dst_size - written, "\\u%04x",
                             (unsigned int)(unsigned char)*p);
            if (n < 0) {
                dst[written] = '\0';
                return -1;
            }
            written += (size_t)n;
        } else {
            if (written + 1 >= dst_size) {
                dst[written] = '\0';
                return -1;
            }
            dst[written++] = *p;
        }
    }
    dst[written] = '\0';
    return (int)written;
}

/** Cached result from a single health checker invocation. */
typedef struct {
    int rc;
    const char *msg;
} health_check_result_t;

#endif /* !IOHTTP_HAVE_YYJSON */

/* ---- File-static state ---- */

static const ioh_health_config_t *s_health_cfg;
static ioh_server_t *s_health_srv;

/* ---- Config ---- */

void ioh_health_config_init(ioh_health_config_t *cfg)
{
    if (cfg == nullptr) {
        return;
    }

    memset(cfg, 0, sizeof(*cfg));
    cfg->enabled = true;
    cfg->health_path = "/health";
    cfg->ready_path = "/ready";
    cfg->live_path = "/live";
    cfg->checker_count = 0;
}

int ioh_health_add_checker(ioh_health_config_t *cfg, const char *name, ioh_health_check_fn check,
                          void *user_data)
{
    if (cfg == nullptr || name == nullptr || check == nullptr) {
        return -EINVAL;
    }
    if (cfg->checker_count >= IOH_HEALTH_MAX_CHECKS) {
        return -ENOSPC;
    }

    cfg->checkers[cfg->checker_count].name = name;
    cfg->checkers[cfg->checker_count].check = check;
    cfg->checkers[cfg->checker_count].user_data = user_data;
    cfg->checker_count++;

    return 0;
}

/* ---- Handlers ---- */

static int health_handler(ioh_ctx_t *c)
{
    return ioh_ctx_json(c, 200, "{\"status\":\"ok\"}");
}

static int ready_handler(ioh_ctx_t *c)
{
    if (ioh_server_is_draining(s_health_srv)) {
        return ioh_ctx_json(c, 503, "{\"status\":\"unavailable\"}");
    }
    return ioh_ctx_json(c, 200, "{\"status\":\"ready\"}");
}

static int live_handler(ioh_ctx_t *c)
{
    if (s_health_cfg == nullptr || s_health_cfg->checker_count == 0) {
        return ioh_ctx_json(c, 200, "{\"status\":\"ok\"}");
    }

    bool all_healthy = true;

#ifdef IOHTTP_HAVE_YYJSON
    yyjson_mut_doc *doc = yyjson_mut_doc_new(nullptr);
    if (doc == nullptr) {
        return ioh_ctx_json(c, 500, "{\"status\":\"error\",\"message\":\"OOM\"}");
    }

    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);

    yyjson_mut_val *checks_obj = yyjson_mut_obj(doc);

    for (uint32_t i = 0; i < s_health_cfg->checker_count; i++) {
        const ioh_health_checker_t *chk = &s_health_cfg->checkers[i];
        const char *msg = nullptr;
        int rc = chk->check(&msg, chk->user_data);

        yyjson_mut_val *entry = yyjson_mut_obj(doc);
        if (rc == 0) {
            yyjson_mut_obj_add_str(doc, entry, "status", "ok");
        } else {
            yyjson_mut_obj_add_str(doc, entry, "status", "fail");
            all_healthy = false;
        }
        if (msg != nullptr) {
            yyjson_mut_obj_add_str(doc, entry, "message", msg);
        }
        yyjson_mut_obj_add_val(doc, checks_obj, chk->name, entry);
    }

    yyjson_mut_obj_add_str(doc, root, "status", all_healthy ? "ok" : "degraded");
    yyjson_mut_obj_add_val(doc, root, "checks", checks_obj);

    const char *json = yyjson_mut_write(doc, 0, nullptr);
    int ret;
    if (json != nullptr) {
        ret = ioh_ctx_json(c, all_healthy ? 200 : 503, json);
        free((void *)json);
    } else {
        ret = ioh_ctx_json(c, 500, "{\"status\":\"error\"}");
    }

    yyjson_mut_doc_free(doc);
    return ret;

#else
    /* Simple string concat fallback without yyjson.
     * Run all checkers once and cache results to avoid double invocation. */
    health_check_result_t results[IOH_HEALTH_MAX_CHECKS];

    for (uint32_t i = 0; i < s_health_cfg->checker_count; i++) {
        const ioh_health_checker_t *chk = &s_health_cfg->checkers[i];
        results[i].msg = nullptr;
        results[i].rc = chk->check(&results[i].msg, chk->user_data);
        if (results[i].rc != 0) {
            all_healthy = false;
        }
    }

    char buf[1024];
    int off = snprintf(buf, sizeof(buf), "{\"status\":\"%s\",\"checks\":{",
                       all_healthy ? "ok" : "degraded");

    char escaped[256];

    for (uint32_t i = 0; i < s_health_cfg->checker_count; i++) {
        const ioh_health_checker_t *chk = &s_health_cfg->checkers[i];

        if (i > 0) {
            off += snprintf(buf + off, sizeof(buf) - (size_t)off, ",");
        }

        json_escape(escaped, sizeof(escaped), chk->name);
        off += snprintf(buf + off, sizeof(buf) - (size_t)off, "\"%s\":{\"status\":\"%s\"", escaped,
                        results[i].rc == 0 ? "ok" : "fail");
        if (results[i].msg != nullptr) {
            json_escape(escaped, sizeof(escaped), results[i].msg);
            off += snprintf(buf + off, sizeof(buf) - (size_t)off, ",\"message\":\"%s\"", escaped);
        }
        off += snprintf(buf + off, sizeof(buf) - (size_t)off, "}");
    }

    off += snprintf(buf + off, sizeof(buf) - (size_t)off, "}}");

    if (off < 0 || (size_t)off >= sizeof(buf)) {
        return ioh_ctx_json(c, 500, "{\"status\":\"error\",\"message\":\"response too large\"}");
    }

    return ioh_ctx_json(c, all_healthy ? 200 : 503, buf);
#endif
}

/* ---- Registration ---- */

int ioh_health_register(ioh_router_t *r, ioh_server_t *srv, const ioh_health_config_t *cfg)
{
    if (r == nullptr || cfg == nullptr) {
        return -EINVAL;
    }

    if (!cfg->enabled) {
        return 0;
    }

    s_health_cfg = cfg;
    s_health_srv = srv;

    int rc = ioh_router_get(r, cfg->health_path, health_handler);
    if (rc < 0) {
        return rc;
    }

    rc = ioh_router_get(r, cfg->ready_path, ready_handler);
    if (rc < 0) {
        return rc;
    }

    rc = ioh_router_get(r, cfg->live_path, live_handler);
    if (rc < 0) {
        return rc;
    }

    return 0;
}

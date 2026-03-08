/**
 * @file io_spa.c
 * @brief SPA fallback serving implementation.
 */

#include "static/io_spa.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

/* ---- Config ---- */

void io_spa_config_init(io_spa_config_t *cfg)
{
    if (cfg == nullptr) {
        return;
    }
    cfg->root_dir = nullptr;
    cfg->index_file = "index.html";
    cfg->api_prefixes = nullptr;
    cfg->api_prefix_count = 0;
    cfg->max_age_default = 3600;
    cfg->max_age_immutable = 31536000;
}

/* ---- Helpers ---- */

/**
 * Check if a character is a hex digit.
 */
static bool is_hex(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

/**
 * Check if request path starts with any API prefix.
 */
static bool is_api_path(const io_spa_config_t *cfg, const char *path, size_t path_len)
{
    for (uint32_t i = 0; i < cfg->api_prefix_count; i++) {
        size_t prefix_len = strnlen(cfg->api_prefixes[i], IO_MAX_URI_SIZE);
        if (path_len >= prefix_len && strncmp(path, cfg->api_prefixes[i], prefix_len) == 0) {
            return true;
        }
    }
    return false;
}

/**
 * Check if a path has a file extension (dot in the filename portion).
 */
static bool has_extension(const char *path, size_t path_len)
{
    /* find last slash to isolate filename */
    const char *filename = path;
    for (size_t i = path_len; i > 0; i--) {
        if (path[i - 1] == '/') {
            filename = &path[i];
            break;
        }
    }

    size_t fname_len = path_len - (size_t)(filename - path);

    /* look for a dot in filename */
    for (size_t i = 0; i < fname_len; i++) {
        if (filename[i] == '.') {
            return true;
        }
    }
    return false;
}

/* ---- Hashed asset detection ---- */

bool io_spa_is_hashed_asset(const char *path, size_t path_len)
{
    if (path == nullptr || path_len == 0) {
        return false;
    }

    /* find the filename portion (after last '/') */
    const char *filename = path;
    for (size_t i = path_len; i > 0; i--) {
        if (path[i - 1] == '/') {
            filename = &path[i];
            break;
        }
    }

    size_t fname_len = path_len - (size_t)(filename - path);

    /*
     * Look for pattern: name.HASH.ext
     * Find the last dot (extension), then check if there's a preceding segment
     * of 6+ hex chars between two dots.
     */

    /* find last dot (final extension) */
    const char *last_dot = nullptr;
    for (size_t i = fname_len; i > 0; i--) {
        if (filename[i - 1] == '.') {
            last_dot = &filename[i - 1];
            break;
        }
    }

    if (last_dot == nullptr || last_dot == filename) {
        return false;
    }

    /* find second-to-last dot */
    const char *prev_dot = nullptr;
    for (const char *p = last_dot - 1; p >= filename; p--) {
        if (*p == '.') {
            prev_dot = p;
            break;
        }
    }

    if (prev_dot == nullptr) {
        return false;
    }

    /* check that chars between prev_dot+1 and last_dot are all hex, count >= 6 */
    size_t hash_len = (size_t)(last_dot - prev_dot - 1);
    if (hash_len < 6) {
        return false;
    }

    for (size_t i = 0; i < hash_len; i++) {
        if (!is_hex(prev_dot[1 + i])) {
            return false;
        }
    }

    return true;
}

/* ---- Main serve ---- */

int io_spa_serve(const io_spa_config_t *cfg, const io_request_t *req, io_response_t *resp)
{
    if (cfg == nullptr || req == nullptr || resp == nullptr) {
        return -EINVAL;
    }
    if (cfg->root_dir == nullptr) {
        return -EINVAL;
    }
    if (req->path == nullptr || req->path_len == 0) {
        return -EINVAL;
    }

    /* 1. check API prefix exclusion */
    if (is_api_path(cfg, req->path, req->path_len)) {
        return -ENOENT;
    }

    /* 2. build full path and check if file exists */
    char full_path[PATH_MAX];
    int n = snprintf(full_path, sizeof(full_path), "%s%.*s", cfg->root_dir, (int)req->path_len,
                     req->path);
    if (n < 0 || (size_t)n >= sizeof(full_path)) {
        return -ENAMETOOLONG;
    }

    struct stat st;
    bool file_exists = (stat(full_path, &st) == 0 && S_ISREG(st.st_mode));

    if (file_exists) {
        /* 3. file exists — serve via io_static_serve */
        io_static_config_t static_cfg;
        io_static_config_init(&static_cfg);
        static_cfg.root_dir = cfg->root_dir;
        static_cfg.max_age_default = cfg->max_age_default;

        int rc = io_static_serve(&static_cfg, req, resp);
        if (rc != 0) {
            return rc;
        }

        /* override Cache-Control for hashed assets */
        if (io_spa_is_hashed_asset(req->path, req->path_len)) {
            char cc_buf[80];
            snprintf(cc_buf, sizeof(cc_buf), "public, max-age=%u, immutable",
                     cfg->max_age_immutable);
            (void)io_response_set_header(resp, "Cache-Control", cc_buf);
        }

        return 0;
    }

    /* 4. file doesn't exist and path has extension — missing real file */
    if (has_extension(req->path, req->path_len)) {
        return -ENOENT;
    }

    /* 5. file doesn't exist and no extension — SPA fallback to index.html */
    const char *index = cfg->index_file != nullptr ? cfg->index_file : "index.html";

    /* build a synthetic request for the index file */
    char index_path[PATH_MAX];
    n = snprintf(index_path, sizeof(index_path), "/%s", index);
    if (n < 0 || (size_t)n >= sizeof(index_path)) {
        return -ENAMETOOLONG;
    }

    io_request_t index_req;
    io_request_init(&index_req);
    index_req.method = req->method;
    index_req.path = index_path;
    index_req.path_len = strnlen(index_path, sizeof(index_path));

    io_static_config_t static_cfg;
    io_static_config_init(&static_cfg);
    static_cfg.root_dir = cfg->root_dir;

    int rc = io_static_serve(&static_cfg, &index_req, resp);
    if (rc != 0) {
        return rc;
    }

    /* set no-cache for SPA fallback pages */
    (void)io_response_set_header(resp, "Cache-Control", "no-cache");

    return 0;
}

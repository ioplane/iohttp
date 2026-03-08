/**
 * @file io_static.c
 * @brief Static file serving implementation.
 */

#include "static/io_static.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

/* ---- MIME type table ---- */

typedef struct {
    const char *ext;
    size_t ext_len;
    const char *mime;
} io_mime_entry_t;

static const io_mime_entry_t mime_table[] = {
    {".html",  5, "text/html"},
    {".htm",   4, "text/html"},
    {".css",   4, "text/css"},
    {".js",    3, "text/javascript"},
    {".json",  5, "application/json"},
    {".xml",   4, "application/xml"},
    {".png",   4, "image/png"},
    {".jpg",   4, "image/jpeg"},
    {".jpeg",  5, "image/jpeg"},
    {".gif",   4, "image/gif"},
    {".svg",   4, "image/svg+xml"},
    {".ico",   4, "image/x-icon"},
    {".webp",  5, "image/webp"},
    {".avif",  5, "image/avif"},
    {".woff",  5, "font/woff"},
    {".woff2", 6, "font/woff2"},
    {".ttf",   4, "font/ttf"},
    {".otf",   4, "font/otf"},
    {".pdf",   4, "application/pdf"},
    {".zip",   4, "application/zip"},
    {".gz",    3, "application/gzip"},
    {".br",    3, "application/x-brotli"},
    {".wasm",  5, "application/wasm"},
    {".txt",   4, "text/plain"},
    {".csv",   4, "text/csv"},
    {".mp4",   4, "video/mp4"},
    {".webm",  5, "video/webm"},
    {".mp3",   4, "audio/mpeg"},
    {".ogg",   4, "audio/ogg"},
    {".map",   4, "application/json"},
};

constexpr size_t MIME_TABLE_SIZE = sizeof(mime_table) / sizeof(mime_table[0]);

/* ---- Config ---- */

void io_static_config_init(io_static_config_t *cfg)
{
    if (cfg == nullptr) {
        return;
    }
    cfg->root_dir = nullptr;
    cfg->directory_listing = false;
    cfg->etag = true;
    cfg->max_age_default = 3600;
}

/* ---- MIME lookup ---- */

const char *io_mime_type(const char *path, size_t len)
{
    if (path == nullptr || len == 0) {
        return "application/octet-stream";
    }

    /* find last dot */
    const char *dot = nullptr;
    for (size_t i = len; i > 0; i--) {
        if (path[i - 1] == '.') {
            dot = &path[i - 1];
            break;
        }
        if (path[i - 1] == '/') {
            break;
        }
    }

    if (dot == nullptr) {
        return "application/octet-stream";
    }

    size_t ext_len = len - (size_t)(dot - path);

    for (size_t i = 0; i < MIME_TABLE_SIZE; i++) {
        if (ext_len == mime_table[i].ext_len &&
            strncasecmp(dot, mime_table[i].ext, ext_len) == 0) {
            return mime_table[i].mime;
        }
    }

    return "application/octet-stream";
}

/* ---- ETag ---- */

int io_etag_generate(uint64_t mtime, uint64_t size, char *buf, size_t buf_size)
{
    if (buf == nullptr || buf_size < 32) {
        return -ENOSPC;
    }

    int n = snprintf(buf, buf_size, "\"%lx-%lx\"",
                     (unsigned long)mtime, (unsigned long)size);
    if (n < 0 || (size_t)n >= buf_size) {
        return -ENOSPC;
    }
    return n;
}

/* ---- Path security ---- */

/**
 * Check for null bytes and ".." traversal in a path.
 * @return true if path is safe, false otherwise.
 */
static bool path_is_safe(const char *path, size_t len)
{
    /* reject embedded null bytes */
    if (memchr(path, '\0', len) != nullptr && strnlen(path, len) < len) {
        return false;
    }

    /* reject ".." components */
    for (size_t i = 0; i + 1 < len; i++) {
        if (path[i] == '.' && path[i + 1] == '.') {
            /* check that it's a path component boundary */
            bool at_start = (i == 0 || path[i - 1] == '/');
            bool at_end = (i + 2 >= len || path[i + 2] == '/');
            if (at_start && at_end) {
                return false;
            }
        }
    }

    return true;
}

/**
 * Parse Range header: "bytes=START-END".
 * Only single ranges are supported.
 * @return 0 on success, -EINVAL on parse error.
 */
static int parse_range(const char *range_hdr, uint64_t file_size,
                       uint64_t *out_start, uint64_t *out_end)
{
    if (range_hdr == nullptr) {
        return -EINVAL;
    }

    /* must start with "bytes=" */
    constexpr size_t PREFIX_LEN = 6;
    if (strncmp(range_hdr, "bytes=", PREFIX_LEN) != 0) {
        return -EINVAL;
    }

    const char *p = range_hdr + PREFIX_LEN;
    char *endptr = nullptr;

    /* suffix range: bytes=-N */
    if (*p == '-') {
        unsigned long suffix = strtoul(p + 1, &endptr, 10);
        if (endptr == p + 1 || suffix == 0 || suffix > file_size) {
            return -EINVAL;
        }
        *out_start = file_size - suffix;
        *out_end = file_size - 1;
        return 0;
    }

    unsigned long start = strtoul(p, &endptr, 10);
    if (endptr == p || *endptr != '-') {
        return -EINVAL;
    }

    p = endptr + 1;
    if (*p == '\0' || *p == ',' || *p == ' ') {
        /* open-ended: bytes=N- */
        *out_start = start;
        *out_end = file_size - 1;
    } else {
        unsigned long end = strtoul(p, &endptr, 10);
        if (endptr == p) {
            return -EINVAL;
        }
        *out_start = start;
        *out_end = end;
    }

    if (*out_start > *out_end || *out_end >= file_size) {
        return -EINVAL;
    }

    return 0;
}

/* ---- Main serve ---- */

int io_static_serve(const io_static_config_t *cfg,
                    const io_request_t *req,
                    io_response_t *resp)
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

    /* path security check */
    if (!path_is_safe(req->path, req->path_len)) {
        return -EACCES;
    }

    /* build full path: root_dir + request_path */
    char full_path[PATH_MAX];
    int n = snprintf(full_path, sizeof(full_path), "%s%.*s",
                     cfg->root_dir, (int)req->path_len, req->path);
    if (n < 0 || (size_t)n >= sizeof(full_path)) {
        return -ENAMETOOLONG;
    }

    /* resolve to real path and verify it's under root */
    char resolved[PATH_MAX];
    if (realpath(full_path, resolved) == nullptr) {
        return -ENOENT;
    }

    char resolved_root[PATH_MAX];
    if (realpath(cfg->root_dir, resolved_root) == nullptr) {
        return -ENOENT;
    }

    size_t root_len = strnlen(resolved_root, PATH_MAX);
    if (strncmp(resolved, resolved_root, root_len) != 0) {
        return -EACCES;
    }
    /* ensure it's a real subdirectory, not just a prefix match */
    if (resolved[root_len] != '/' && resolved[root_len] != '\0') {
        return -EACCES;
    }

    /* stat the file */
    struct stat st;
    if (stat(resolved, &st) != 0) {
        return -ENOENT;
    }
    if (S_ISDIR(st.st_mode)) {
        return -ENOENT;
    }

    uint64_t file_size = (uint64_t)st.st_size;
    uint64_t mtime = (uint64_t)st.st_mtime;

    /* generate ETag */
    char etag_buf[64];
    int etag_len = 0;
    if (cfg->etag) {
        etag_len = io_etag_generate(mtime, file_size, etag_buf, sizeof(etag_buf));
        if (etag_len < 0) {
            etag_len = 0;
        }
    }

    /* check If-None-Match */
    if (cfg->etag && etag_len > 0) {
        const char *inm = io_request_header(req, "If-None-Match");
        if (inm != nullptr) {
            size_t inm_len = strnlen(inm, IO_MAX_HEADER_SIZE);
            if (inm_len == (size_t)etag_len &&
                memcmp(inm, etag_buf, (size_t)etag_len) == 0) {
                resp->status = 304;
                (void)io_response_set_header(resp, "ETag", etag_buf);
                return 0;
            }
        }
    }

    /* check If-Modified-Since */
    const char *ims = io_request_header(req, "If-Modified-Since");
    if (ims != nullptr) {
        struct tm tm_parsed;
        memset(&tm_parsed, 0, sizeof(tm_parsed));
        char *result = strptime(ims, "%a, %d %b %Y %H:%M:%S GMT", &tm_parsed);
        if (result != nullptr) {
            time_t ims_time = timegm(&tm_parsed);
            if (ims_time >= 0 && (uint64_t)ims_time >= mtime) {
                resp->status = 304;
                if (cfg->etag && etag_len > 0) {
                    (void)io_response_set_header(resp, "ETag", etag_buf);
                }
                return 0;
            }
        }
    }

    /* check for range request */
    const char *range_hdr = io_request_header(req, "Range");
    uint64_t range_start = 0;
    uint64_t range_end = file_size > 0 ? file_size - 1 : 0;
    bool is_range = false;

    if (range_hdr != nullptr && file_size > 0) {
        if (parse_range(range_hdr, file_size, &range_start, &range_end) == 0) {
            is_range = true;
        }
    }

    /* read file content */
    FILE *fp = fopen(resolved, "rb");
    if (fp == nullptr) {
        return -ENOENT;
    }

    uint64_t read_len;
    if (is_range) {
        read_len = range_end - range_start + 1;
        if (fseek(fp, (long)range_start, SEEK_SET) != 0) {
            fclose(fp);
            return -EIO;
        }
    } else {
        read_len = file_size;
    }

    uint8_t *buf = malloc((size_t)read_len);
    if (buf == nullptr) {
        fclose(fp);
        return -ENOMEM;
    }

    size_t bytes_read = fread(buf, 1, (size_t)read_len, fp);
    fclose(fp);

    if (bytes_read != (size_t)read_len) {
        free(buf);
        return -EIO;
    }

    /* set response */
    const char *mime = io_mime_type(req->path, req->path_len);

    if (is_range) {
        resp->status = 206;

        char content_range[128];
        snprintf(content_range, sizeof(content_range),
                 "bytes %lu-%lu/%lu",
                 (unsigned long)range_start,
                 (unsigned long)range_end,
                 (unsigned long)file_size);
        (void)io_response_set_header(resp, "Content-Range", content_range);
    } else {
        resp->status = 200;
    }

    (void)io_response_set_header(resp, "Content-Type", mime);

    char len_str[32];
    snprintf(len_str, sizeof(len_str), "%lu", (unsigned long)bytes_read);
    (void)io_response_set_header(resp, "Content-Length", len_str);

    (void)io_response_set_header(resp, "Accept-Ranges", "bytes");

    if (cfg->etag && etag_len > 0) {
        (void)io_response_set_header(resp, "ETag", etag_buf);
    }

    /* Last-Modified */
    struct tm tm_mtime;
    char lm_buf[64];
    if (gmtime_r((time_t *)&st.st_mtime, &tm_mtime) != nullptr) {
        if (strftime(lm_buf, sizeof(lm_buf),
                     "%a, %d %b %Y %H:%M:%S GMT", &tm_mtime) > 0) {
            (void)io_response_set_header(resp, "Last-Modified", lm_buf);
        }
    }

    /* Cache-Control */
    char cc_buf[64];
    snprintf(cc_buf, sizeof(cc_buf), "public, max-age=%u", cfg->max_age_default);
    (void)io_response_set_header(resp, "Cache-Control", cc_buf);

    (void)io_response_set_body(resp, buf, bytes_read);
    free(buf);

    return 0;
}

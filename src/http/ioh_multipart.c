/**
 * @file ioh_multipart.c
 * @brief RFC 7578 multipart/form-data parser implementation.
 */

#include "http/ioh_multipart.h"

#include <errno.h>
#include <string.h>

/* ---- helpers ---- */

/**
 * Case-insensitive prefix match.
 */
static bool ci_prefix(const char *str, size_t str_len, const char *prefix, size_t prefix_len)
{
    if (str_len < prefix_len) {
        return false;
    }
    for (size_t i = 0; i < prefix_len; i++) {
        char a = str[i];
        char b = prefix[i];
        /* lowercase ASCII */
        if (a >= 'A' && a <= 'Z') {
            a = (char)(a + ('a' - 'A'));
        }
        if (b >= 'A' && b <= 'Z') {
            b = (char)(b + ('a' - 'A'));
        }
        if (a != b) {
            return false;
        }
    }
    return true;
}

/**
 * Find needle in haystack (binary-safe memmem).
 */
static const uint8_t *find_bytes(const uint8_t *haystack, size_t haystack_len,
                                 const uint8_t *needle, size_t needle_len)
{
    if (needle_len == 0) {
        return haystack;
    }
    if (needle_len > haystack_len) {
        return nullptr;
    }
    size_t limit = haystack_len - needle_len;
    for (size_t i = 0; i <= limit; i++) {
        if (memcmp(haystack + i, needle, needle_len) == 0) {
            return haystack + i;
        }
    }
    return nullptr;
}

/**
 * Extract a quoted or unquoted parameter value from a header.
 * Looks for key= in str, returns pointer to value and its length.
 */
static bool extract_param(const char *str, size_t str_len, const char *key, size_t key_len,
                          const char **value, size_t *value_len)
{
    const char *end = str + str_len;
    const char *p = str;

    while (p < end) {
        /* find key= */
        const char *found = nullptr;
        size_t remaining = (size_t)(end - p);
        for (size_t i = 0; i + key_len <= remaining; i++) {
            if (ci_prefix(p + i, remaining - i, key, key_len)) {
                found = p + i;
                break;
            }
        }
        if (!found) {
            return false;
        }

        const char *val_start = found + key_len;
        if (val_start >= end) {
            return false;
        }

        if (*val_start == '"') {
            /* quoted value */
            val_start++;
            const char *val_end = val_start;
            while (val_end < end && *val_end != '"') {
                val_end++;
            }
            *value = val_start;
            *value_len = (size_t)(val_end - val_start);
            return true;
        }

        /* unquoted value — ends at ; or whitespace or end */
        const char *val_end = val_start;
        while (val_end < end && *val_end != ';' && *val_end != ' ' && *val_end != '\t' &&
               *val_end != '\r' && *val_end != '\n') {
            val_end++;
        }
        *value = val_start;
        *value_len = (size_t)(val_end - val_start);
        return true;
    }
    return false;
}

/**
 * Parse headers of a single part (between boundary line and \r\n\r\n).
 */
static int parse_part_headers(const char *headers, size_t headers_len, ioh_multipart_part_t *part)
{
    const char *p = headers;
    const char *end = headers + headers_len;
    bool found_disposition = false;

    while (p < end) {
        /* find end of this header line */
        const char *line_end = p;
        while (line_end + 1 < end && !(line_end[0] == '\r' && line_end[1] == '\n')) {
            line_end++;
        }

        size_t line_len = (size_t)(line_end - p);

        /* Content-Disposition */
        const char *cd = "content-disposition:";
        size_t cd_len = 20;
        if (ci_prefix(p, line_len, cd, cd_len)) {
            found_disposition = true;
            const char *val = p + cd_len;
            size_t val_len = line_len - cd_len;
            /* skip leading whitespace */
            while (val_len > 0 && (*val == ' ' || *val == '\t')) {
                val++;
                val_len--;
            }

            extract_param(val, val_len, "name=", 5, &part->name, &part->name_len);
            extract_param(val, val_len, "filename=", 9, &part->filename, &part->filename_len);
        }

        /* Content-Type */
        const char *ct = "content-type:";
        size_t ct_len = 13;
        if (ci_prefix(p, line_len, ct, ct_len)) {
            const char *val = p + ct_len;
            size_t val_len = line_len - ct_len;
            while (val_len > 0 && (*val == ' ' || *val == '\t')) {
                val++;
                val_len--;
            }
            /* trim trailing whitespace */
            while (val_len > 0 && (val[val_len - 1] == ' ' || val[val_len - 1] == '\t')) {
                val_len--;
            }
            part->content_type = val;
            part->content_type_len = val_len;
        }

        /* advance past \r\n */
        p = line_end;
        if (p + 1 < end && p[0] == '\r' && p[1] == '\n') {
            p += 2;
        } else {
            break;
        }
    }

    if (!found_disposition) {
        return -EINVAL;
    }
    return 0;
}

/* ---- public API ---- */

void ioh_multipart_config_init(ioh_multipart_config_t *cfg)
{
    if (!cfg) {
        return;
    }
    cfg->max_parts = IOH_MULTIPART_DEFAULT_MAX_PARTS;
    cfg->max_part_size = IOH_MULTIPART_DEFAULT_MAX_PART_SIZE;
    cfg->max_total_size = IOH_MULTIPART_DEFAULT_MAX_TOTAL_SIZE;
}

int ioh_multipart_boundary(const char *content_type, const char **boundary, size_t *boundary_len)
{
    if (!content_type || !boundary || !boundary_len) {
        return -EINVAL;
    }

    /* must start with multipart/form-data */
    const char *prefix = "multipart/form-data";
    size_t prefix_len = 19;
    if (!ci_prefix(content_type, strlen(content_type), prefix, prefix_len)) {
        return -EINVAL;
    }

    size_t ct_len = strlen(content_type);
    if (!extract_param(content_type, ct_len, "boundary=", 9, boundary, boundary_len)) {
        return -EINVAL;
    }

    if (*boundary_len == 0) {
        return -EINVAL;
    }

    return 0;
}

int ioh_multipart_parse(const uint8_t *body, size_t body_len, const char *boundary,
                       size_t boundary_len, const ioh_multipart_config_t *cfg,
                       ioh_multipart_part_t *parts, uint32_t *part_count)
{
    if (!body || !boundary || !cfg || !parts || !part_count) {
        return -EINVAL;
    }

    if (body_len > cfg->max_total_size) {
        return -E2BIG;
    }

    uint32_t max_parts = *part_count;
    *part_count = 0;

    if (body_len == 0) {
        return 0;
    }

    /* build delimiter: "--" + boundary */
    uint8_t delim[256 + 2];
    if (boundary_len > 256) {
        return -EINVAL;
    }
    delim[0] = '-';
    delim[1] = '-';
    memcpy(delim + 2, boundary, boundary_len);
    size_t delim_len = boundary_len + 2;

    /* find first delimiter */
    const uint8_t *pos = find_bytes(body, body_len, delim, delim_len);
    if (!pos) {
        return -EINVAL;
    }

    /* move past first delimiter + \r\n */
    pos += delim_len;
    size_t remain = body_len - (size_t)(pos - body);
    if (remain < 2) {
        return -EINVAL;
    }

    /* check for closing delimiter (empty body with just --boundary--) */
    if (pos[0] == '-' && pos[1] == '-') {
        return 0;
    }

    if (pos[0] != '\r' || pos[1] != '\n') {
        return -EINVAL;
    }
    pos += 2;
    remain = body_len - (size_t)(pos - body);

    size_t total_data = 0;

    while (remain > 0) {
        /* find the header/body separator: \r\n\r\n */
        const uint8_t *hdr_end = find_bytes(pos, remain, (const uint8_t *)"\r\n\r\n", 4);
        if (!hdr_end) {
            return -EINVAL;
        }

        size_t headers_len = (size_t)(hdr_end - pos);
        const uint8_t *data_start = hdr_end + 4;
        size_t data_offset = (size_t)(data_start - body);
        if (data_offset > body_len) {
            return -EINVAL;
        }
        size_t data_remain = body_len - data_offset;

        /* find next delimiter to determine data end */
        const uint8_t *next_delim = find_bytes(data_start, data_remain, delim, delim_len);
        if (!next_delim) {
            return -EINVAL;
        }

        /* data ends at \r\n before delimiter */
        size_t data_len = (size_t)(next_delim - data_start);
        if (data_len >= 2 && data_start[data_len - 2] == '\r' && data_start[data_len - 1] == '\n') {
            data_len -= 2; /* strip trailing \r\n before delimiter */
        }

        /* check part size limit */
        if (data_len > cfg->max_part_size) {
            return -E2BIG;
        }

        total_data += data_len;
        if (total_data > cfg->max_total_size) {
            return -E2BIG;
        }

        /* check part count */
        if (*part_count >= max_parts || *part_count >= cfg->max_parts) {
            return -E2BIG;
        }

        /* parse part */
        ioh_multipart_part_t *part = &parts[*part_count];
        memset(part, 0, sizeof(*part));
        part->data = data_start;
        part->data_len = data_len;

        int rc = parse_part_headers((const char *)pos, headers_len, part);
        if (rc != 0) {
            return rc;
        }

        (*part_count)++;

        /* advance past data + delimiter */
        pos = next_delim + delim_len;
        remain = body_len - (size_t)(pos - body);

        /* check for closing -- */
        if (remain >= 2 && pos[0] == '-' && pos[1] == '-') {
            break;
        }

        /* expect \r\n after delimiter */
        if (remain < 2 || pos[0] != '\r' || pos[1] != '\n') {
            return -EINVAL;
        }
        pos += 2;
        remain = body_len - (size_t)(pos - body);
    }

    return 0;
}

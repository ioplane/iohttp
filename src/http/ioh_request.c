/**
 * @file ioh_request.c
 * @brief Protocol-independent HTTP request implementation.
 */

#include "http/ioh_request.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

void ioh_request_init(ioh_request_t *req)
{
    if (req == nullptr) {
        return;
    }
    memset(req, 0, sizeof(*req));
    req->method = IOH_METHOD_GET;
}

void ioh_request_reset(ioh_request_t *req)
{
    ioh_request_init(req);
}

const char *ioh_request_header(const ioh_request_t *req, const char *name)
{
    if (req == nullptr || name == nullptr) {
        return nullptr;
    }

    size_t name_len = strnlen(name, IOH_MAX_HEADER_SIZE);

    for (uint32_t i = 0; i < req->header_count; i++) {
        if (req->headers[i].name_len != name_len) {
            continue;
        }
        if (strncasecmp(req->headers[i].name, name, name_len) == 0) {
            return req->headers[i].value;
        }
    }
    return nullptr;
}

ioh_method_t ioh_method_parse(const char *method, size_t len)
{
    if (method == nullptr || len == 0) {
        return IOH_METHOD_UNKNOWN;
    }

    /* Compare case-sensitively per RFC 9110 */
    switch (len) {
    case 3:
        if (memcmp(method, "GET", 3) == 0) {
            return IOH_METHOD_GET;
        }
        if (memcmp(method, "PUT", 3) == 0) {
            return IOH_METHOD_PUT;
        }
        break;
    case 4:
        if (memcmp(method, "POST", 4) == 0) {
            return IOH_METHOD_POST;
        }
        if (memcmp(method, "HEAD", 4) == 0) {
            return IOH_METHOD_HEAD;
        }
        break;
    case 5:
        if (memcmp(method, "PATCH", 5) == 0) {
            return IOH_METHOD_PATCH;
        }
        if (memcmp(method, "TRACE", 5) == 0) {
            return IOH_METHOD_TRACE;
        }
        break;
    case 6:
        if (memcmp(method, "DELETE", 6) == 0) {
            return IOH_METHOD_DELETE;
        }
        break;
    case 7:
        if (memcmp(method, "OPTIONS", 7) == 0) {
            return IOH_METHOD_OPTIONS;
        }
        if (memcmp(method, "CONNECT", 7) == 0) {
            return IOH_METHOD_CONNECT;
        }
        break;
    default:
        break;
    }
    return IOH_METHOD_UNKNOWN;
}

const char *ioh_method_name(ioh_method_t method)
{
    static const char *const names[] = {
        [IOH_METHOD_GET] = "GET",         [IOH_METHOD_POST] = "POST",
        [IOH_METHOD_PUT] = "PUT",         [IOH_METHOD_DELETE] = "DELETE",
        [IOH_METHOD_PATCH] = "PATCH",     [IOH_METHOD_HEAD] = "HEAD",
        [IOH_METHOD_OPTIONS] = "OPTIONS", [IOH_METHOD_TRACE] = "TRACE",
        [IOH_METHOD_CONNECT] = "CONNECT",
    };

    if (method >= IOH_METHOD_UNKNOWN) {
        return "UNKNOWN";
    }
    return names[method];
}

const char *ioh_request_param(const ioh_request_t *req, const char *name)
{
    if (req == nullptr || name == nullptr) {
        return nullptr;
    }

    size_t name_len = strnlen(name, IOH_MAX_URI_SIZE);

    for (uint32_t i = 0; i < req->param_count; i++) {
        if (req->params[i].name_len != name_len) {
            continue;
        }
        if (memcmp(req->params[i].name, name, name_len) == 0) {
            return req->params[i].value;
        }
    }
    return nullptr;
}

int ioh_request_param_i64(const ioh_request_t *req, const char *name, int64_t *out)
{
    if (req == nullptr || name == nullptr || out == nullptr) {
        return -EINVAL;
    }

    size_t name_len = strnlen(name, IOH_MAX_URI_SIZE);
    const ioh_param_t *param = nullptr;

    for (uint32_t i = 0; i < req->param_count; i++) {
        if (req->params[i].name_len == name_len &&
            memcmp(req->params[i].name, name, name_len) == 0) {
            param = &req->params[i];
            break;
        }
    }

    if (param == nullptr || param->value == nullptr || param->value_len == 0) {
        return -EINVAL;
    }

    char *endptr = nullptr;
    errno = 0;
    long long val = strtoll(param->value, &endptr, 10);

    if (errno == ERANGE) {
        return -ERANGE;
    }
    if (endptr != param->value + param->value_len) {
        return -EINVAL;
    }

    *out = (int64_t)val;
    return 0;
}

int ioh_request_param_u64(const ioh_request_t *req, const char *name, uint64_t *out)
{
    if (req == nullptr || name == nullptr || out == nullptr) {
        return -EINVAL;
    }

    size_t name_len = strnlen(name, IOH_MAX_URI_SIZE);
    const ioh_param_t *param = nullptr;

    for (uint32_t i = 0; i < req->param_count; i++) {
        if (req->params[i].name_len == name_len &&
            memcmp(req->params[i].name, name, name_len) == 0) {
            param = &req->params[i];
            break;
        }
    }

    if (param == nullptr || param->value == nullptr || param->value_len == 0) {
        return -EINVAL;
    }

    /* Reject negative values for unsigned extraction */
    for (size_t i = 0; i < param->value_len; i++) {
        if (param->value[i] == '-') {
            return -EINVAL;
        }
        if (param->value[i] != ' ') {
            break;
        }
    }

    char *endptr = nullptr;
    errno = 0;
    unsigned long long val = strtoull(param->value, &endptr, 10);

    if (errno == ERANGE) {
        return -ERANGE;
    }
    if (endptr != param->value + param->value_len) {
        return -EINVAL;
    }

    *out = (uint64_t)val;
    return 0;
}

int ioh_request_param_bool(const ioh_request_t *req, const char *name, bool *out)
{
    if (req == nullptr || name == nullptr || out == nullptr) {
        return -EINVAL;
    }

    size_t name_len = strnlen(name, IOH_MAX_URI_SIZE);
    const ioh_param_t *param = nullptr;

    for (uint32_t i = 0; i < req->param_count; i++) {
        if (req->params[i].name_len == name_len &&
            memcmp(req->params[i].name, name, name_len) == 0) {
            param = &req->params[i];
            break;
        }
    }

    if (param == nullptr || param->value == nullptr || param->value_len == 0) {
        return -EINVAL;
    }

    size_t len = param->value_len;
    const char *v = param->value;

    if ((len == 4 && strncasecmp(v, "true", 4) == 0) || (len == 1 && v[0] == '1') ||
        (len == 3 && strncasecmp(v, "yes", 3) == 0)) {
        *out = true;
        return 0;
    }

    if ((len == 5 && strncasecmp(v, "false", 5) == 0) || (len == 1 && v[0] == '0') ||
        (len == 2 && strncasecmp(v, "no", 2) == 0)) {
        *out = false;
        return 0;
    }

    return -EINVAL;
}

uint32_t ioh_request_param_count(const ioh_request_t *req)
{
    if (req == nullptr) {
        return 0;
    }
    return req->param_count;
}

const char *ioh_request_cookie(const ioh_request_t *req, const char *name)
{
    if (req == nullptr || name == nullptr) {
        return nullptr;
    }

    const char *cookie = ioh_request_header(req, "Cookie");
    if (cookie == nullptr) {
        return nullptr;
    }

    size_t name_len = strnlen(name, IOH_MAX_HEADER_SIZE);
    const char *p = cookie;

    while (*p != '\0') {
        /* skip leading whitespace */
        while (*p == ' ') {
            p++;
        }

        /* check if this segment matches "name=" */
        if (strncmp(p, name, name_len) == 0 && p[name_len] == '=') {
            return p + name_len + 1;
        }

        /* advance to next cookie (after ';') */
        const char *semi = strchr(p, ';');
        if (semi == nullptr) {
            break;
        }
        p = semi + 1;
    }
    return nullptr;
}

const char *ioh_request_query_param(const ioh_request_t *req, const char *name)
{
    if (req == nullptr || name == nullptr || req->query == nullptr) {
        return nullptr;
    }

    size_t name_len = strnlen(name, IOH_MAX_URI_SIZE);
    const char *p = req->query;
    const char *end = req->query + req->query_len;

    while (p < end) {
        /* check if key matches */
        if ((size_t)(end - p) > name_len && memcmp(p, name, name_len) == 0 && p[name_len] == '=') {
            return p + name_len + 1;
        }

        /* advance to next param (after '&') */
        const char *amp = memchr(p, '&', (size_t)(end - p));
        if (amp == nullptr) {
            break;
        }
        p = amp + 1;
    }
    return nullptr;
}

const char *ioh_request_accepts(const ioh_request_t *req, const char **types, uint32_t count)
{
    if (req == nullptr || types == nullptr || count == 0) {
        return nullptr;
    }

    const char *accept = ioh_request_header(req, "Accept");
    if (accept == nullptr) {
        return nullptr;
    }

    size_t accept_len = strnlen(accept, IOH_MAX_HEADER_SIZE);
    const char *p = accept;
    const char *end = accept + accept_len;

    while (p < end) {
        /* skip whitespace */
        while (p < end && *p == ' ') {
            p++;
        }

        /* find end of this media type (comma or end) */
        const char *comma = memchr(p, ',', (size_t)(end - p));
        const char *type_end = comma ? comma : end;

        /* strip quality parameter (;q=...) */
        const char *semi = memchr(p, ';', (size_t)(type_end - p));
        const char *mtype_end = semi ? semi : type_end;

        /* trim trailing whitespace */
        while (mtype_end > p && *(mtype_end - 1) == ' ') {
            mtype_end--;
        }

        size_t mtype_len = (size_t)(mtype_end - p);

        // check for full wildcard
        if (mtype_len == 3 && memcmp(p, "*/*", 3) == 0) {
            // wildcard matches first offered type
            return types[0];
        }

        // check for type wildcard (e.g. "text/*")
        if (mtype_len > 2 && p[mtype_len - 1] == '*' && p[mtype_len - 2] == '/') {
            size_t prefix_len = mtype_len - 1; /* "text/" */
            for (uint32_t i = 0; i < count; i++) {
                if (strncasecmp(types[i], p, prefix_len) == 0) {
                    return types[i];
                }
            }
        }

        /* exact match */
        for (uint32_t i = 0; i < count; i++) {
            size_t tlen = strnlen(types[i], IOH_MAX_HEADER_SIZE);
            if (tlen == mtype_len && strncasecmp(types[i], p, mtype_len) == 0) {
                return types[i];
            }
        }

        p = comma ? comma + 1 : end;
    }
    return nullptr;
}

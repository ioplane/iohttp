/**
 * @file io_vhost.c
 * @brief Host-based virtual routing dispatcher implementation.
 */

#include "router/io_vhost.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

typedef struct {
    char host[256];      /* host pattern (e.g., "*.example.com") */
    bool is_wildcard;    /* true if host starts with "*." */
    io_router_t *router; /* NOT owned */
} io_vhost_entry_t;

struct io_vhost {
    io_vhost_entry_t entries[IO_VHOST_MAX_HOSTS];
    uint32_t count;
    io_router_t *default_router; /* NOT owned */
};

io_vhost_t *io_vhost_create(void)
{
    io_vhost_t *v = calloc(1, sizeof(*v));
    return v;
}

void io_vhost_destroy(io_vhost_t *v)
{
    free(v);
}

int io_vhost_add(io_vhost_t *v, const char *host, io_router_t *router)
{
    if (v == nullptr || host == nullptr || router == nullptr) {
        return -EINVAL;
    }

    if (v->count >= IO_VHOST_MAX_HOSTS) {
        return -ENOSPC;
    }

    size_t len = strnlen(host, 256);
    if (len == 0 || len >= 256) {
        return -EINVAL;
    }

    io_vhost_entry_t *entry = &v->entries[v->count];
    memcpy(entry->host, host, len);
    entry->host[len] = '\0';
    entry->is_wildcard = (len >= 2 && host[0] == '*' && host[1] == '.');
    entry->router = router;
    v->count++;

    return 0;
}

void io_vhost_set_default(io_vhost_t *v, io_router_t *router)
{
    if (v != nullptr) {
        v->default_router = router;
    }
}

/**
 * Strip port from host string, handling IPv6 bracket notation.
 * Returns length of host portion (without port).
 */
static size_t strip_port(const char *host, size_t len)
{
    if (len == 0) {
        return 0;
    }

    /* IPv6 literal: [::1]:8080 — find closing bracket first */
    if (host[0] == '[') {
        const char *bracket = memchr(host, ']', len);
        if (bracket != nullptr) {
            /* Return up to and including the bracket */
            return (size_t)(bracket - host + 1);
        }
        /* Malformed IPv6, return as-is */
        return len;
    }

    /* Regular host: find last colon */
    for (size_t i = len; i > 0; i--) {
        if (host[i - 1] == ':') {
            return i - 1;
        }
    }

    return len;
}

/**
 * Check if a hostname matches a wildcard pattern "*.suffix".
 * "*.example.com" matches "foo.example.com" but NOT "example.com".
 */
static bool wildcard_match(const char *pattern, const char *host, size_t host_len)
{
    /* pattern starts with "*.", skip the "*" to get ".suffix" */
    const char *suffix = pattern + 1;
    size_t suffix_len = strlen(suffix);

    /* Host must be longer than suffix (needs at least one char before ".suffix") */
    if (host_len <= suffix_len) {
        return false;
    }

    /* Compare the suffix portion case-insensitively */
    return strncasecmp(host + host_len - suffix_len, suffix, suffix_len) == 0;
}

io_route_match_t io_vhost_dispatch(const io_vhost_t *v, io_method_t method, const char *host,
                                   const char *path, size_t path_len)
{
    io_route_match_t result;
    memset(&result, 0, sizeof(result));
    result.status = IO_MATCH_NOT_FOUND;

    if (v == nullptr) {
        return result;
    }

    io_router_t *matched_router = nullptr;

    if (host != nullptr) {
        size_t host_len = strnlen(host, 256);
        host_len = strip_port(host, host_len);

        /* Pass 1: exact match */
        for (uint32_t i = 0; i < v->count; i++) {
            if (v->entries[i].is_wildcard) {
                continue;
            }
            size_t entry_len = strlen(v->entries[i].host);
            if (entry_len == host_len && strncasecmp(v->entries[i].host, host, host_len) == 0) {
                matched_router = v->entries[i].router;
                break;
            }
        }

        /* Pass 2: wildcard match */
        if (matched_router == nullptr) {
            for (uint32_t i = 0; i < v->count; i++) {
                if (!v->entries[i].is_wildcard) {
                    continue;
                }
                if (wildcard_match(v->entries[i].host, host, host_len)) {
                    matched_router = v->entries[i].router;
                    break;
                }
            }
        }
    }

    /* Fall back to default router */
    if (matched_router == nullptr) {
        matched_router = v->default_router;
    }

    if (matched_router != nullptr) {
        result = io_router_dispatch(matched_router, method, path, path_len);
    }

    return result;
}

uint32_t io_vhost_count(const io_vhost_t *v)
{
    if (v == nullptr) {
        return 0;
    }
    return v->count;
}

/**
 * @file io_cookie.c
 * @brief Set-Cookie response builder implementation (RFC 6265bis).
 */

#include "http/io_cookie.h"

#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

constexpr size_t IO_COOKIE_NAME_MAX = 256;

int io_cookie_validate_name(const char *name)
{
    if (name == nullptr) {
        return -EINVAL;
    }

    size_t len = strnlen(name, IO_COOKIE_NAME_MAX + 1);
    if (len == 0 || len > IO_COOKIE_NAME_MAX) {
        return -EINVAL;
    }

    for (size_t i = 0; i < len; i++) {
        unsigned char ch = (unsigned char)name[i];

        /* reject CTL chars (0x00-0x1F, 0x7F) */
        if (ch <= 0x1F || ch == 0x7F) {
            return -EINVAL;
        }

        /* reject cookie-name separators: = ; SP HT */
        if (ch == '=' || ch == ';' || ch == ' ' || ch == '\t') {
            return -EINVAL;
        }
    }

    return 0;
}

int io_cookie_serialize(const io_cookie_t *cookie, char *buf, size_t buf_size)
{
    if (cookie == nullptr || buf == nullptr || buf_size == 0) {
        return -EINVAL;
    }

    int rc = io_cookie_validate_name(cookie->name);
    if (rc != 0) {
        return rc;
    }

    if (cookie->value == nullptr) {
        return -EINVAL;
    }

    /* SameSite=None forces Secure */
    bool secure = cookie->secure;
    if (cookie->same_site == IO_SAME_SITE_NONE) {
        secure = true;
    }

    size_t pos = 0;

    /* name=value */
    int n = snprintf(buf + pos, buf_size - pos, "%s=%s", cookie->name, cookie->value);
    if (n < 0) {
        return -EINVAL;
    }
    pos += (size_t)n;
    if (pos >= buf_size) {
        return -ENOSPC;
    }

    /* Domain */
    if (cookie->domain != nullptr) {
        n = snprintf(buf + pos, buf_size - pos, "; Domain=%s", cookie->domain);
        if (n < 0) {
            return -EINVAL;
        }
        pos += (size_t)n;
        if (pos >= buf_size) {
            return -ENOSPC;
        }
    }

    /* Path */
    if (cookie->path != nullptr) {
        n = snprintf(buf + pos, buf_size - pos, "; Path=%s", cookie->path);
        if (n < 0) {
            return -EINVAL;
        }
        pos += (size_t)n;
        if (pos >= buf_size) {
            return -ENOSPC;
        }
    }

    /* Max-Age: -1 = omit (session), 0 = delete, >0 = include */
    if (cookie->max_age >= 0) {
        n = snprintf(buf + pos, buf_size - pos, "; Max-Age=%" PRId64, cookie->max_age);
        if (n < 0) {
            return -EINVAL;
        }
        pos += (size_t)n;
        if (pos >= buf_size) {
            return -ENOSPC;
        }
    }

    /* SameSite */
    switch (cookie->same_site) {
    case IO_SAME_SITE_LAX:
        n = snprintf(buf + pos, buf_size - pos, "; SameSite=Lax");
        break;
    case IO_SAME_SITE_STRICT:
        n = snprintf(buf + pos, buf_size - pos, "; SameSite=Strict");
        break;
    case IO_SAME_SITE_NONE:
        n = snprintf(buf + pos, buf_size - pos, "; SameSite=None");
        break;
    case IO_SAME_SITE_DEFAULT:
        n = 0;
        break;
    }
    if (n < 0) {
        return -EINVAL;
    }
    pos += (size_t)n;
    if (pos >= buf_size) {
        return -ENOSPC;
    }

    /* Secure */
    if (secure) {
        n = snprintf(buf + pos, buf_size - pos, "; Secure");
        if (n < 0) {
            return -EINVAL;
        }
        pos += (size_t)n;
        if (pos >= buf_size) {
            return -ENOSPC;
        }
    }

    /* HttpOnly */
    if (cookie->http_only) {
        n = snprintf(buf + pos, buf_size - pos, "; HttpOnly");
        if (n < 0) {
            return -EINVAL;
        }
        pos += (size_t)n;
        if (pos >= buf_size) {
            return -ENOSPC;
        }
    }

    return (int)pos;
}

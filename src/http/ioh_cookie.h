/**
 * @file ioh_cookie.h
 * @brief Set-Cookie response builder (RFC 6265bis).
 */

#ifndef IOHTTP_HTTP_COOKIE_H
#define IOHTTP_HTTP_COOKIE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ---- SameSite attribute ---- */

typedef enum : uint8_t {
    IOH_SAME_SITE_DEFAULT, /* browser default -- omit attribute */
    IOH_SAME_SITE_LAX,
    IOH_SAME_SITE_STRICT,
    IOH_SAME_SITE_NONE, /* requires Secure */
} ioh_same_site_t;

/* ---- Cookie descriptor ---- */

typedef struct {
    const char *name;
    const char *value;
    const char *domain; /* nullable */
    const char *path;   /* nullable */
    int64_t max_age;    /* seconds, -1 = session (omit Max-Age), 0 = delete */
    ioh_same_site_t same_site;
    bool secure;
    bool http_only;
} ioh_cookie_t;

/* ---- API ---- */

/**
 * @brief Serialize a cookie into a Set-Cookie header value.
 * @param cookie Cookie descriptor (must not be nullptr).
 * @param buf    Output buffer.
 * @param buf_size Size of output buffer.
 * @return Bytes written (excluding NUL) on success, -EINVAL on bad input,
 *         -ENOSPC if buffer is too small.
 */
[[nodiscard]] int ioh_cookie_serialize(const ioh_cookie_t *cookie, char *buf, size_t buf_size);

/**
 * @brief Validate a cookie name per RFC 6265bis.
 * @param name Cookie name string.
 * @return 0 on valid, -EINVAL on invalid (nullptr, empty, CTL chars, separators).
 */
[[nodiscard]] int ioh_cookie_validate_name(const char *name);

#endif /* IOHTTP_HTTP_COOKIE_H */

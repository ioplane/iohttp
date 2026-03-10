/**
 * @file ioh_alt_svc.c
 * @brief Alt-Svc header generation for HTTP/3 discovery (RFC 7838).
 */

#include "http/ioh_alt_svc.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

int ioh_alt_svc_format(char *buf, size_t buf_len, uint16_t port, uint32_t max_age)
{
    if (buf == nullptr || buf_len == 0) {
        return -EINVAL;
    }

    int n;
    if (max_age > 0) {
        n = snprintf(buf, buf_len, "h3=\":%u\"; ma=%u", (unsigned)port, (unsigned)max_age);
    } else {
        n = snprintf(buf, buf_len, "h3=\":%u\"", (unsigned)port);
    }

    if (n < 0) {
        return -EIO;
    }
    if ((size_t)n >= buf_len) {
        return -ENOSPC;
    }
    return n;
}

int ioh_alt_svc_add_header(ioh_response_t *resp, uint16_t port, uint32_t max_age)
{
    if (resp == nullptr) {
        return -EINVAL;
    }

    char buf[64];
    int n = ioh_alt_svc_format(buf, sizeof(buf), port, max_age);
    if (n < 0) {
        return n;
    }

    return ioh_response_set_header(resp, "alt-svc", buf);
}

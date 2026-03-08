/**
 * @file io_proxy_proto.c
 * @brief PROXY protocol v1 (text) and v2 (binary) decoder implementation.
 */

#include "http/io_proxy_proto.h"

#include <errno.h>
#include <string.h>

#include <arpa/inet.h>

/* ---- Internal helpers ---- */

/**
 * Find \r\n within the first max_len bytes.
 * Returns pointer to \r, or nullptr if not found.
 */
static const uint8_t *find_crlf(const uint8_t *buf, size_t len, size_t max_len)
{
    size_t limit = len < max_len ? len : max_len;

    for (size_t i = 0; i + 1 < limit; i++) {
        if (buf[i] == '\r' && buf[i + 1] == '\n') {
            return &buf[i];
        }
    }
    return nullptr;
}

/**
 * Decode PROXY protocol v1 (text format).
 * Format: "PROXY TCP4|TCP6|UNKNOWN <src> <dst> <sport> <dport>\r\n"
 */
static int decode_v1(const uint8_t *buf, size_t len, io_proxy_result_t *result)
{
    /* Need at least "PROXY " prefix */
    if (len < 6) {
        return -EAGAIN;
    }

    /* Find CRLF terminator */
    const uint8_t *crlf = find_crlf(buf, len, IO_PROXY_V1_MAX_LEN);

    if (crlf == nullptr) {
        /* If we have IO_PROXY_V1_MAX_LEN bytes and still no CRLF, malformed */
        if (len >= IO_PROXY_V1_MAX_LEN) {
            return -EINVAL;
        }
        return -EAGAIN;
    }

    size_t line_len = (size_t)(crlf - buf);
    size_t total_consumed = line_len + 2; /* include \r\n */

    /* Copy line into null-terminated buffer for parsing */
    char line[IO_PROXY_V1_MAX_LEN + 1];

    memcpy(line, buf, line_len);
    line[line_len] = '\0';

    /* Skip past "PROXY " */
    const char *pos = line + 6;

    memset(result, 0, sizeof(*result));
    result->version = 1;

    /* Check protocol family */
    if (strncmp(pos, "UNKNOWN", 7) == 0) {
        result->is_local = true;
        return (int)total_consumed;
    }

    bool is_tcp4 = (strncmp(pos, "TCP4 ", 5) == 0);
    bool is_tcp6 = (strncmp(pos, "TCP6 ", 5) == 0);

    if (!is_tcp4 && !is_tcp6) {
        return -EINVAL;
    }

    pos += 5; /* skip "TCP4 " or "TCP6 " */

    /* Parse: src_ip dst_ip src_port dst_port */
    char src_ip[INET6_ADDRSTRLEN];
    char dst_ip[INET6_ADDRSTRLEN];
    unsigned int src_port = 0;
    unsigned int dst_port = 0;

    /* Tokenize manually: src_ip */
    const char *token_end = strchr(pos, ' ');

    if (token_end == nullptr || (size_t)(token_end - pos) >= sizeof(src_ip)) {
        return -EINVAL;
    }
    memcpy(src_ip, pos, (size_t)(token_end - pos));
    src_ip[token_end - pos] = '\0';
    pos = token_end + 1;

    /* dst_ip */
    token_end = strchr(pos, ' ');
    if (token_end == nullptr || (size_t)(token_end - pos) >= sizeof(dst_ip)) {
        return -EINVAL;
    }
    memcpy(dst_ip, pos, (size_t)(token_end - pos));
    dst_ip[token_end - pos] = '\0';
    pos = token_end + 1;

    /* src_port */
    token_end = strchr(pos, ' ');
    if (token_end == nullptr) {
        return -EINVAL;
    }

    for (const char *p = pos; p < token_end; p++) {
        if (*p < '0' || *p > '9') {
            return -EINVAL;
        }
        src_port = src_port * 10 + (unsigned int)(*p - '0');
    }
    pos = token_end + 1;

    /* dst_port — rest of line */
    for (const char *p = pos; *p != '\0'; p++) {
        if (*p < '0' || *p > '9') {
            return -EINVAL;
        }
        dst_port = dst_port * 10 + (unsigned int)(*p - '0');
    }

    /* Validate port range */
    if (src_port > 65535 || dst_port > 65535) {
        return -EINVAL;
    }

    if (is_tcp4) {
        result->family = AF_INET;

        struct sockaddr_in *src = (struct sockaddr_in *)&result->src_addr;
        struct sockaddr_in *dst = (struct sockaddr_in *)&result->dst_addr;

        src->sin_family = AF_INET;
        src->sin_port = htons((uint16_t)src_port);
        if (inet_pton(AF_INET, src_ip, &src->sin_addr) != 1) {
            return -EINVAL;
        }

        dst->sin_family = AF_INET;
        dst->sin_port = htons((uint16_t)dst_port);
        if (inet_pton(AF_INET, dst_ip, &dst->sin_addr) != 1) {
            return -EINVAL;
        }
    } else {
        result->family = AF_INET6;

        struct sockaddr_in6 *src = (struct sockaddr_in6 *)&result->src_addr;
        struct sockaddr_in6 *dst = (struct sockaddr_in6 *)&result->dst_addr;

        src->sin6_family = AF_INET6;
        src->sin6_port = htons((uint16_t)src_port);
        if (inet_pton(AF_INET6, src_ip, &src->sin6_addr) != 1) {
            return -EINVAL;
        }

        dst->sin6_family = AF_INET6;
        dst->sin6_port = htons((uint16_t)dst_port);
        if (inet_pton(AF_INET6, dst_ip, &dst->sin6_addr) != 1) {
            return -EINVAL;
        }
    }

    return (int)total_consumed;
}

/**
 * Decode PROXY protocol v2 (binary format).
 */
static int decode_v2(const uint8_t *buf, size_t len, io_proxy_result_t *result)
{
    if (len < IO_PROXY_V2_MIN_LEN) {
        return -EAGAIN;
    }

    /* Verify signature */
    if (memcmp(buf, IO_PROXY_V2_SIG, 12) != 0) {
        return -EINVAL;
    }

    uint8_t ver_cmd = buf[12];
    uint8_t fam_proto = buf[13];

    /* Version must be 0x2 (high nibble) */
    if ((ver_cmd >> 4) != 0x2) {
        return -EINVAL;
    }

    uint8_t command = ver_cmd & 0x0F;

    /* Command: 0=LOCAL, 1=PROXY */
    if (command > 1) {
        return -EINVAL;
    }

    /* Address length (big-endian uint16_t) */
    uint16_t addr_len = (uint16_t)((uint16_t)buf[14] << 8 | (uint16_t)buf[15]);
    size_t total = IO_PROXY_V2_MIN_LEN + addr_len;

    if (len < total) {
        return -EAGAIN;
    }

    memset(result, 0, sizeof(*result));
    result->version = 2;

    if (command == 0) {
        result->is_local = true;
        return (int)total;
    }

    /* PROXY command — decode addresses based on family */
    uint8_t family = (fam_proto >> 4) & 0x0F;
    const uint8_t *addrs = buf + IO_PROXY_V2_MIN_LEN;

    if (family == 0x1) {
        /* AF_INET: 4+4+2+2 = 12 bytes */
        if (addr_len < 12) {
            return -EINVAL;
        }

        result->family = AF_INET;

        struct sockaddr_in *src = (struct sockaddr_in *)&result->src_addr;
        struct sockaddr_in *dst = (struct sockaddr_in *)&result->dst_addr;

        src->sin_family = AF_INET;
        memcpy(&src->sin_addr, addrs, 4);
        memcpy(&dst->sin_addr, addrs + 4, 4);

        uint16_t sport;
        uint16_t dport;

        memcpy(&sport, addrs + 8, 2);
        memcpy(&dport, addrs + 10, 2);
        src->sin_port = sport; /* already network byte order */
        dst->sin_port = dport;

        dst->sin_family = AF_INET;
    } else if (family == 0x2) {
        /* AF_INET6: 16+16+2+2 = 36 bytes */
        if (addr_len < 36) {
            return -EINVAL;
        }

        result->family = AF_INET6;

        struct sockaddr_in6 *src = (struct sockaddr_in6 *)&result->src_addr;
        struct sockaddr_in6 *dst = (struct sockaddr_in6 *)&result->dst_addr;

        src->sin6_family = AF_INET6;
        memcpy(&src->sin6_addr, addrs, 16);
        memcpy(&dst->sin6_addr, addrs + 16, 16);

        uint16_t sport;
        uint16_t dport;

        memcpy(&sport, addrs + 32, 2);
        memcpy(&dport, addrs + 34, 2);
        src->sin6_port = sport;
        dst->sin6_port = dport;

        dst->sin6_family = AF_INET6;
    } else {
        /* UNSPEC or AF_UNIX — treat as local */
        result->is_local = true;
    }

    return (int)total;
}

/* ---- Public API ---- */

int io_proxy_decode(const uint8_t *buf, size_t len, io_proxy_result_t *result)
{
    if (buf == nullptr || result == nullptr) {
        return -EINVAL;
    }

    if (len == 0) {
        return -EAGAIN;
    }

    /* Try v2 first (binary signature check) */
    if (len >= 12 && memcmp(buf, IO_PROXY_V2_SIG, 12) == 0) {
        return decode_v2(buf, len, result);
    }

    /* Try v1 (text: starts with "PROXY ") */
    if (len >= 6 && memcmp(buf, "PROXY ", 6) == 0) {
        return decode_v1(buf, len, result);
    }

    /* Not enough data to determine version */
    if (len < 6) {
        return -EAGAIN;
    }

    return -ENOSPC;
}

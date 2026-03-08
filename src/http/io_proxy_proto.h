/**
 * @file io_proxy_proto.h
 * @brief PROXY protocol v1 (text) and v2 (binary) decoder.
 *
 * Extracts real client addresses from PROXY protocol headers
 * injected by load balancers (HAProxy, AWS NLB, etc.).
 */

#ifndef IOHTTP_HTTP_PROXY_PROTO_H
#define IOHTTP_HTTP_PROXY_PROTO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <netinet/in.h>

/* PROXY protocol v2 signature (12 bytes) */
constexpr uint8_t IO_PROXY_V2_SIG[12] = {0x0D, 0x0A, 0x0D, 0x0A, 0x00, 0x0D,
                                         0x0A, 0x51, 0x55, 0x49, 0x54, 0x0A};

constexpr size_t IO_PROXY_V1_MAX_LEN = 108; /* "PROXY" + max text line */
constexpr size_t IO_PROXY_V2_MIN_LEN = 16;  /* 12 sig + 4 header */

typedef struct {
    uint8_t version; /* 1 or 2 */
    bool is_local;   /* LOCAL command (health check, no addrs) */
    uint8_t family;  /* AF_INET or AF_INET6 */
    struct sockaddr_storage src_addr;
    struct sockaddr_storage dst_addr;
} io_proxy_result_t;

/**
 * Decode PROXY protocol header from buffer.
 *
 * @param buf    Input buffer.
 * @param len    Buffer length.
 * @param result Output result with decoded addresses.
 * @return >0 bytes consumed, -EAGAIN incomplete, -EINVAL malformed, -ENOSPC unknown.
 */
[[nodiscard]] int io_proxy_decode(const uint8_t *buf, size_t len, io_proxy_result_t *result);

#endif /* IOHTTP_HTTP_PROXY_PROTO_H */

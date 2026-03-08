/**
 * @file test_proxy_proto.c
 * @brief Unit tests for PROXY protocol v1/v2 decoder.
 */

#include "http/io_proxy_proto.h"

#include <errno.h>
#include <string.h>

#include <arpa/inet.h>
#include <unity.h>

void setUp(void)
{
}
void tearDown(void)
{
}

/* ---- PPv1 tests ---- */

void test_proxy_v1_tcp4(void)
{
    const char *hdr = "PROXY TCP4 192.168.1.1 10.0.0.1 56324 443\r\n";
    io_proxy_result_t res;

    int rc = io_proxy_decode((const uint8_t *)hdr, strlen(hdr), &res);
    TEST_ASSERT_GREATER_THAN(0, rc);
    TEST_ASSERT_EQUAL_UINT8(1, res.version);
    TEST_ASSERT_FALSE(res.is_local);
    TEST_ASSERT_EQUAL_UINT8(AF_INET, res.family);

    struct sockaddr_in *src = (struct sockaddr_in *)&res.src_addr;
    struct sockaddr_in *dst = (struct sockaddr_in *)&res.dst_addr;

    char src_ip[INET_ADDRSTRLEN];
    char dst_ip[INET_ADDRSTRLEN];

    inet_ntop(AF_INET, &src->sin_addr, src_ip, sizeof(src_ip));
    inet_ntop(AF_INET, &dst->sin_addr, dst_ip, sizeof(dst_ip));

    TEST_ASSERT_EQUAL_STRING("192.168.1.1", src_ip);
    TEST_ASSERT_EQUAL_STRING("10.0.0.1", dst_ip);
    TEST_ASSERT_EQUAL_UINT16(56324, ntohs(src->sin_port));
    TEST_ASSERT_EQUAL_UINT16(443, ntohs(dst->sin_port));
    TEST_ASSERT_EQUAL_size_t(strlen(hdr), (size_t)rc);
}

void test_proxy_v1_tcp6(void)
{
    const char *hdr = "PROXY TCP6 ::1 ::1 56324 443\r\n";
    io_proxy_result_t res;

    int rc = io_proxy_decode((const uint8_t *)hdr, strlen(hdr), &res);
    TEST_ASSERT_GREATER_THAN(0, rc);
    TEST_ASSERT_EQUAL_UINT8(1, res.version);
    TEST_ASSERT_FALSE(res.is_local);
    TEST_ASSERT_EQUAL_UINT8(AF_INET6, res.family);

    struct sockaddr_in6 *src = (struct sockaddr_in6 *)&res.src_addr;

    TEST_ASSERT_EQUAL_UINT16(56324, ntohs(src->sin6_port));
    TEST_ASSERT_EQUAL_size_t(strlen(hdr), (size_t)rc);
}

void test_proxy_v1_unknown(void)
{
    const char *hdr = "PROXY UNKNOWN\r\n";
    io_proxy_result_t res;

    int rc = io_proxy_decode((const uint8_t *)hdr, strlen(hdr), &res);
    TEST_ASSERT_GREATER_THAN(0, rc);
    TEST_ASSERT_EQUAL_UINT8(1, res.version);
    TEST_ASSERT_TRUE(res.is_local);
    TEST_ASSERT_EQUAL_size_t(strlen(hdr), (size_t)rc);
}

void test_proxy_v1_incomplete(void)
{
    const char *hdr = "PROXY TCP4 192.168.1.1";
    io_proxy_result_t res;

    int rc = io_proxy_decode((const uint8_t *)hdr, strlen(hdr), &res);
    TEST_ASSERT_EQUAL_INT(-EAGAIN, rc);
}

void test_proxy_v1_malformed(void)
{
    const char *hdr = "PROXY GARBAGE 1 2 3 4\r\n";
    io_proxy_result_t res;

    int rc = io_proxy_decode((const uint8_t *)hdr, strlen(hdr), &res);
    TEST_ASSERT_EQUAL_INT(-EINVAL, rc);
}

/* ---- PPv2 tests ---- */

void test_proxy_v2_tcp4(void)
{
    uint8_t pp2[] = {
        /* Signature (12 bytes) */
        0x0D,
        0x0A,
        0x0D,
        0x0A,
        0x00,
        0x0D,
        0x0A,
        0x51,
        0x55,
        0x49,
        0x54,
        0x0A,
        /* Ver+Cmd: version 2, command PROXY (0x21) */
        0x21,
        /* Family+Proto: AF_INET + STREAM (0x11) */
        0x11,
        /* Address length: 12 bytes (big-endian) */
        0x00,
        0x0C,
        /* Source IP: 192.168.1.1 */
        0xC0,
        0xA8,
        0x01,
        0x01,
        /* Dest IP: 10.0.0.1 */
        0x0A,
        0x00,
        0x00,
        0x01,
        /* Source port: 56324 (big-endian) */
        0xDC,
        0x04,
        /* Dest port: 443 (big-endian) */
        0x01,
        0xBB,
    };

    io_proxy_result_t res;
    int rc = io_proxy_decode(pp2, sizeof(pp2), &res);

    TEST_ASSERT_EQUAL_INT((int)sizeof(pp2), rc);
    TEST_ASSERT_EQUAL_UINT8(2, res.version);
    TEST_ASSERT_FALSE(res.is_local);
    TEST_ASSERT_EQUAL_UINT8(AF_INET, res.family);

    struct sockaddr_in *src = (struct sockaddr_in *)&res.src_addr;
    struct sockaddr_in *dst = (struct sockaddr_in *)&res.dst_addr;

    char src_ip[INET_ADDRSTRLEN];
    char dst_ip[INET_ADDRSTRLEN];

    inet_ntop(AF_INET, &src->sin_addr, src_ip, sizeof(src_ip));
    inet_ntop(AF_INET, &dst->sin_addr, dst_ip, sizeof(dst_ip));

    TEST_ASSERT_EQUAL_STRING("192.168.1.1", src_ip);
    TEST_ASSERT_EQUAL_STRING("10.0.0.1", dst_ip);
    TEST_ASSERT_EQUAL_UINT16(56324, ntohs(src->sin_port));
    TEST_ASSERT_EQUAL_UINT16(443, ntohs(dst->sin_port));
}

void test_proxy_v2_tcp6(void)
{
    uint8_t pp2[] = {
        /* Signature */
        0x0D,
        0x0A,
        0x0D,
        0x0A,
        0x00,
        0x0D,
        0x0A,
        0x51,
        0x55,
        0x49,
        0x54,
        0x0A,
        /* Ver+Cmd: version 2, PROXY */
        0x21,
        /* Family+Proto: AF_INET6 + STREAM (0x21) */
        0x21,
        /* Address length: 36 bytes */
        0x00,
        0x24,
        /* Source IPv6: ::1 */
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x01,
        /* Dest IPv6: ::1 */
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x01,
        /* Source port: 56324 */
        0xDC,
        0x04,
        /* Dest port: 443 */
        0x01,
        0xBB,
    };

    io_proxy_result_t res;
    int rc = io_proxy_decode(pp2, sizeof(pp2), &res);

    TEST_ASSERT_EQUAL_INT((int)sizeof(pp2), rc);
    TEST_ASSERT_EQUAL_UINT8(2, res.version);
    TEST_ASSERT_FALSE(res.is_local);
    TEST_ASSERT_EQUAL_UINT8(AF_INET6, res.family);

    struct sockaddr_in6 *src = (struct sockaddr_in6 *)&res.src_addr;

    TEST_ASSERT_EQUAL_UINT16(56324, ntohs(src->sin6_port));
}

void test_proxy_v2_local(void)
{
    uint8_t pp2[] = {
        /* Signature */
        0x0D,
        0x0A,
        0x0D,
        0x0A,
        0x00,
        0x0D,
        0x0A,
        0x51,
        0x55,
        0x49,
        0x54,
        0x0A,
        /* Ver+Cmd: version 2, LOCAL (0x20) */
        0x20,
        /* Family+Proto: UNSPEC (0x00) */
        0x00,
        /* Address length: 0 */
        0x00,
        0x00,
    };

    io_proxy_result_t res;
    int rc = io_proxy_decode(pp2, sizeof(pp2), &res);

    TEST_ASSERT_EQUAL_INT((int)sizeof(pp2), rc);
    TEST_ASSERT_EQUAL_UINT8(2, res.version);
    TEST_ASSERT_TRUE(res.is_local);
}

void test_proxy_v2_with_tlv(void)
{
    uint8_t pp2[] = {
        /* Signature */
        0x0D,
        0x0A,
        0x0D,
        0x0A,
        0x00,
        0x0D,
        0x0A,
        0x51,
        0x55,
        0x49,
        0x54,
        0x0A,
        /* Ver+Cmd: version 2, PROXY */
        0x21,
        /* Family+Proto: AF_INET + STREAM */
        0x11,
        /* Address length: 12 + 7 TLV = 19 bytes */
        0x00,
        0x13,
        /* Source IP: 192.168.1.1 */
        0xC0,
        0xA8,
        0x01,
        0x01,
        /* Dest IP: 10.0.0.1 */
        0x0A,
        0x00,
        0x00,
        0x01,
        /* Source port: 56324 */
        0xDC,
        0x04,
        /* Dest port: 443 */
        0x01,
        0xBB,
        /* TLV: type=0x04, length=4, value="test" (arbitrary extension) */
        0x04,
        0x00,
        0x04,
        0x74,
        0x65,
        0x73,
        0x74,
    };

    io_proxy_result_t res;
    int rc = io_proxy_decode(pp2, sizeof(pp2), &res);

    /* Should consume all bytes including TLV */
    TEST_ASSERT_EQUAL_INT((int)sizeof(pp2), rc);
    TEST_ASSERT_EQUAL_UINT8(2, res.version);
    TEST_ASSERT_FALSE(res.is_local);
    TEST_ASSERT_EQUAL_UINT8(AF_INET, res.family);

    struct sockaddr_in *src = (struct sockaddr_in *)&res.src_addr;

    TEST_ASSERT_EQUAL_UINT16(56324, ntohs(src->sin_port));
}

void test_proxy_v2_invalid_signature(void)
{
    uint8_t pp2[] = {
        /* Bad signature (first byte wrong) */
        0xFF, 0x0A, 0x0D, 0x0A, 0x00, 0x0D, 0x0A, 0x51, 0x55, 0x49, 0x54, 0x0A, 0x21, 0x11,
        0x00, 0x0C, 0xC0, 0xA8, 0x01, 0x01, 0x0A, 0x00, 0x00, 0x01, 0xDC, 0x04, 0x01, 0xBB,
    };

    io_proxy_result_t res;
    int rc = io_proxy_decode(pp2, sizeof(pp2), &res);

    /* Not a v2 header, not "PROXY " prefix either */
    TEST_ASSERT_EQUAL_INT(-ENOSPC, rc);
}

/* ---- Test runner ---- */

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_proxy_v1_tcp4);
    RUN_TEST(test_proxy_v1_tcp6);
    RUN_TEST(test_proxy_v1_unknown);
    RUN_TEST(test_proxy_v1_incomplete);
    RUN_TEST(test_proxy_v1_malformed);
    RUN_TEST(test_proxy_v2_tcp4);
    RUN_TEST(test_proxy_v2_tcp6);
    RUN_TEST(test_proxy_v2_local);
    RUN_TEST(test_proxy_v2_with_tlv);
    RUN_TEST(test_proxy_v2_invalid_signature);

    return UNITY_END();
}

/**
 * @file io_websocket.c
 * @brief WebSocket protocol (RFC 6455) implementation.
 */

#include "ws/io_websocket.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <wolfssl/options.h>
#include <wolfssl/wolfcrypt/coding.h>
#include <wolfssl/wolfcrypt/sha.h>

/* RFC 6455 §4.2.2: magic GUID */
static const char WS_GUID[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

/* ---- Upgrade handshake ---- */

int io_ws_compute_accept(const char *client_key, char *accept_out)
{
    if (client_key == nullptr || accept_out == nullptr) {
        return -EINVAL;
    }

    /* Concatenate key + GUID */
    size_t key_len = strlen(client_key);
    size_t guid_len = strlen(WS_GUID);
    size_t concat_len = key_len + guid_len;

    /* Stack buffer for concatenation (key is 24 chars + GUID is 36 chars = 60) */
    char concat[64];
    if (concat_len >= sizeof(concat)) {
        return -EINVAL;
    }

    memcpy(concat, client_key, key_len);
    memcpy(concat + key_len, WS_GUID, guid_len);

    /* SHA-1 hash */
    wc_Sha sha;
    int ret = wc_InitSha(&sha);
    if (ret != 0) {
        return -EINVAL;
    }

    ret = wc_ShaUpdate(&sha, (const unsigned char *)concat, (unsigned int)concat_len);
    if (ret != 0) {
        wc_ShaFree(&sha);
        return -EINVAL;
    }

    unsigned char hash[WC_SHA_DIGEST_SIZE];
    ret = wc_ShaFinal(&sha, hash);
    wc_ShaFree(&sha);
    if (ret != 0) {
        return -EINVAL;
    }

    /* Base64 encode */
    unsigned int out_len = IO_WS_ACCEPT_KEY_LEN + 1;
    ret = Base64_Encode_NoNl(hash, WC_SHA_DIGEST_SIZE, (unsigned char *)accept_out, &out_len);
    if (ret != 0) {
        return -EINVAL;
    }

    accept_out[out_len] = '\0';
    return 0;
}

int io_ws_validate_upgrade(const char *method, const char *upgrade_hdr, const char *conn_hdr,
                           const char *ws_key, const char *ws_version)
{
    if (method == nullptr || upgrade_hdr == nullptr || conn_hdr == nullptr || ws_key == nullptr ||
        ws_version == nullptr) {
        return -EINVAL;
    }

    /* Must be GET */
    if (strcasecmp(method, "GET") != 0) {
        return -EINVAL;
    }

    /* Upgrade: websocket (case-insensitive) */
    if (strcasecmp(upgrade_hdr, "websocket") != 0) {
        return -EINVAL;
    }

    /* Connection must contain "Upgrade" (case-insensitive) */
    if (strcasestr(conn_hdr, "Upgrade") == nullptr) {
        return -EINVAL;
    }

    /* Sec-WebSocket-Key must be non-empty */
    if (strlen(ws_key) == 0) {
        return -EINVAL;
    }

    /* Sec-WebSocket-Version must be "13" */
    if (strcmp(ws_version, "13") != 0) {
        return -EINVAL;
    }

    return 0;
}

/* ---- Frame encoding ---- */

int io_ws_frame_encode(uint8_t *buf, size_t buf_size, io_ws_opcode_t opcode, bool fin,
                       const uint8_t *payload, size_t len)
{
    if (buf == nullptr) {
        return -EINVAL;
    }

    /* Calculate header size */
    size_t header_size = 2;
    if (len > 65535) {
        header_size += 8;
    } else if (len > 125) {
        header_size += 2;
    }

    size_t total = header_size + len;
    if (total > buf_size) {
        return -ENOSPC;
    }

    /* Byte 0: FIN + opcode */
    buf[0] = (uint8_t)((fin ? 0x80U : 0x00U) | ((uint8_t)opcode & 0x0FU));

    /* Byte 1: payload length (no mask bit for server frames) */
    if (len <= 125) {
        buf[1] = (uint8_t)len;
    } else if (len <= 65535) {
        buf[1] = 126;
        buf[2] = (uint8_t)(len >> 8);
        buf[3] = (uint8_t)(len & 0xFF);
    } else {
        buf[1] = 127;
        uint64_t n = (uint64_t)len;
        buf[2] = (uint8_t)(n >> 56);
        buf[3] = (uint8_t)(n >> 48);
        buf[4] = (uint8_t)(n >> 40);
        buf[5] = (uint8_t)(n >> 32);
        buf[6] = (uint8_t)(n >> 24);
        buf[7] = (uint8_t)(n >> 16);
        buf[8] = (uint8_t)(n >> 8);
        buf[9] = (uint8_t)(n & 0xFF);
    }

    /* Payload */
    if (len > 0 && payload != nullptr) {
        memcpy(buf + header_size, payload, len);
    }

    return (int)total;
}

/* ---- Frame decoding ---- */

int io_ws_frame_decode(const uint8_t *buf, size_t buf_len, io_ws_frame_t *frame)
{
    if (buf == nullptr || frame == nullptr) {
        return -EINVAL;
    }

    if (buf_len < 2) {
        return -EAGAIN;
    }

    /* Byte 0: FIN, RSV, opcode */
    frame->fin = (buf[0] & 0x80U) != 0;
    uint8_t rsv = (buf[0] >> 4) & 0x07U;
    if (rsv != 0) {
        return -EINVAL; /* RSV bits must be 0 without extensions */
    }
    frame->opcode = (io_ws_opcode_t)(buf[0] & 0x0FU);

    /* Byte 1: mask flag, initial length */
    frame->masked = (buf[1] & 0x80U) != 0;
    uint8_t len7 = buf[1] & 0x7FU;

    size_t offset = 2;
    uint64_t payload_len;

    if (len7 <= 125) {
        payload_len = len7;
    } else if (len7 == 126) {
        if (buf_len < 4) {
            return -EAGAIN;
        }
        payload_len = ((uint64_t)buf[2] << 8) | (uint64_t)buf[3];
        offset = 4;
    } else { /* 127 */
        if (buf_len < 10) {
            return -EAGAIN;
        }
        payload_len = ((uint64_t)buf[2] << 56) | ((uint64_t)buf[3] << 48) |
                      ((uint64_t)buf[4] << 40) | ((uint64_t)buf[5] << 32) |
                      ((uint64_t)buf[6] << 24) | ((uint64_t)buf[7] << 16) |
                      ((uint64_t)buf[8] << 8) | (uint64_t)buf[9];
        offset = 10;
    }

    /* Control frames: payload must be <= 125 bytes */
    if ((uint8_t)frame->opcode >= 0x8 && payload_len > IO_WS_MAX_CONTROL_PAYLOAD) {
        return -EINVAL;
    }

    /* Read mask key if present */
    if (frame->masked) {
        if (buf_len < offset + 4) {
            return -EAGAIN;
        }
        memcpy(frame->mask_key, buf + offset, 4);
        offset += 4;
    } else {
        memset(frame->mask_key, 0, 4);
    }

    /* Check we have complete payload */
    if (buf_len < offset + payload_len) {
        return -EAGAIN;
    }

    /* Guard against int overflow on return value */
    size_t total = offset + payload_len;
    if (total > (size_t)INT32_MAX) {
        return -E2BIG;
    }

    frame->payload_len = payload_len;
    frame->payload = (payload_len > 0) ? (buf + offset) : nullptr;

    return (int)total;
}

/* ---- Masking ---- */

void io_ws_apply_mask(uint8_t *data, size_t len, const uint8_t mask_key[4])
{
    if (data == nullptr || mask_key == nullptr) {
        return;
    }

    for (size_t i = 0; i < len; i++) {
        data[i] ^= mask_key[i % 4];
    }
}

/* ---- Connection context ---- */

int io_ws_conn_init(io_ws_conn_t *ws, size_t max_message_size)
{
    if (ws == nullptr) {
        return -EINVAL;
    }

    memset(ws, 0, sizeof(*ws));
    ws->max_message_size = (max_message_size > 0) ? max_message_size : IO_WS_DEFAULT_MAX_MSG;
    return 0;
}

void io_ws_conn_reset(io_ws_conn_t *ws)
{
    if (ws == nullptr) {
        return;
    }

    free(ws->frag_buf);
    ws->frag_buf = nullptr;
    ws->frag_len = 0;
    ws->frag_capacity = 0;
    ws->in_fragment = false;
}

void io_ws_conn_destroy(io_ws_conn_t *ws)
{
    if (ws == nullptr) {
        return;
    }

    io_ws_conn_reset(ws);
}

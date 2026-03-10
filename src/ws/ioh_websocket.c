/**
 * @file ioh_websocket.c
 * @brief WebSocket (RFC 6455) — wslay-backed implementation.
 *
 * Handshake: iohttp (wolfSSL SHA-1 + base64).
 * Framing, close, ping/pong, fragmentation: wslay.
 */

#include "ws/ioh_websocket.h"

#include <errno.h>
#include <string.h>
#include <strings.h>

#include <wolfssl/options.h>
#include <wolfssl/wolfcrypt/coding.h>
#include <wolfssl/wolfcrypt/sha.h>

/* RFC 6455 §4.2.2: magic GUID */
static const char WS_GUID[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

/* ---- Upgrade handshake (iohttp's own) ---- */

int ioh_ws_compute_accept(const char *client_key, char *accept_out)
{
    if (client_key == nullptr || accept_out == nullptr) {
        return -EINVAL;
    }

    size_t key_len = strlen(client_key);
    size_t guid_len = strlen(WS_GUID);
    size_t concat_len = key_len + guid_len;

    /* Stack buffer: key (24) + GUID (36) = 60 max */
    char concat[64];
    if (concat_len >= sizeof(concat)) {
        return -EINVAL;
    }

    memcpy(concat, client_key, key_len);
    memcpy(concat + key_len, WS_GUID, guid_len);

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

    unsigned int out_len = IOH_WS_ACCEPT_KEY_LEN + 1;
    ret = Base64_Encode_NoNl(hash, WC_SHA_DIGEST_SIZE, (unsigned char *)accept_out, &out_len);
    if (ret != 0) {
        return -EINVAL;
    }

    accept_out[out_len] = '\0';
    return 0;
}

int ioh_ws_validate_upgrade(const char *method, const char *upgrade_hdr, const char *conn_hdr,
                           const char *ws_key, const char *ws_version)
{
    if (method == nullptr || upgrade_hdr == nullptr || conn_hdr == nullptr || ws_key == nullptr ||
        ws_version == nullptr) {
        return -EINVAL;
    }

    if (strcasecmp(method, "GET") != 0) {
        return -EINVAL;
    }
    if (strcasecmp(upgrade_hdr, "websocket") != 0) {
        return -EINVAL;
    }
    if (strcasestr(conn_hdr, "Upgrade") == nullptr) {
        return -EINVAL;
    }
    if (strlen(ws_key) == 0) {
        return -EINVAL;
    }
    if (strcmp(ws_version, "13") != 0) {
        return -EINVAL;
    }

    return 0;
}

/* ---- wslay callbacks ---- */

static ssize_t wslay_recv_cb(wslay_event_context_ptr ctx, uint8_t *buf, size_t len, int flags,
                             void *user_data)
{
    (void)ctx;
    (void)flags;
    ioh_ws_ctx_t *ws = user_data;

    if (ws->recv == nullptr) {
        wslay_event_set_error(ctx, WSLAY_ERR_CALLBACK_FAILURE);
        return -1;
    }

    bool wouldblock = false;
    ssize_t n = ws->recv(buf, len, &wouldblock, ws->user_data);
    if (n < 0) {
        if (wouldblock) {
            wslay_event_set_error(ctx, WSLAY_ERR_WOULDBLOCK);
        } else {
            wslay_event_set_error(ctx, WSLAY_ERR_CALLBACK_FAILURE);
        }
        return -1;
    }

    return n;
}

static ssize_t wslay_send_cb(wslay_event_context_ptr ctx, const uint8_t *data, size_t len,
                             int flags, void *user_data)
{
    (void)ctx;
    (void)flags;
    ioh_ws_ctx_t *ws = user_data;

    if (ws->send == nullptr) {
        wslay_event_set_error(ctx, WSLAY_ERR_CALLBACK_FAILURE);
        return -1;
    }

    bool wouldblock = false;
    ssize_t n = ws->send(data, len, &wouldblock, ws->user_data);
    if (n < 0) {
        if (wouldblock) {
            wslay_event_set_error(ctx, WSLAY_ERR_WOULDBLOCK);
        } else {
            wslay_event_set_error(ctx, WSLAY_ERR_CALLBACK_FAILURE);
        }
        return -1;
    }

    return n;
}

static void wslay_on_msg_cb(wslay_event_context_ptr ctx,
                            const struct wslay_event_on_msg_recv_arg *arg, void *user_data)
{
    (void)ctx;
    ioh_ws_ctx_t *ws = user_data;

    if (arg->opcode == WSLAY_CONNECTION_CLOSE) {
        if (ws->on_close != nullptr) {
            ws->on_close(arg->status_code, ws->user_data);
        }
        return;
    }

    if (ws->on_msg != nullptr) {
        ws->on_msg(arg->opcode, arg->msg, arg->msg_length, ws->user_data);
    }
}

/* ---- Context lifecycle ---- */

int ioh_ws_ctx_init(ioh_ws_ctx_t *ws)
{
    if (ws == nullptr) {
        return -EINVAL;
    }
    if (ws->recv == nullptr || ws->send == nullptr) {
        return -EINVAL;
    }

    struct wslay_event_callbacks cbs = {
        .recv_callback = wslay_recv_cb,
        .send_callback = wslay_send_cb,
        .genmask_callback = nullptr, /* server does not mask */
        .on_frame_recv_start_callback = nullptr,
        .on_frame_recv_chunk_callback = nullptr,
        .on_frame_recv_end_callback = nullptr,
        .on_msg_recv_callback = wslay_on_msg_cb,
    };

    int ret = wslay_event_context_server_init(&ws->wslay_ctx, &cbs, ws);
    if (ret != 0) {
        return -ENOMEM;
    }

    uint64_t max_len = ws->max_recv_msg_len > 0 ? ws->max_recv_msg_len : IOH_WS_DEFAULT_MAX_MSG;
    wslay_event_config_set_max_recv_msg_length(ws->wslay_ctx, max_len);

    return 0;
}

void ioh_ws_ctx_destroy(ioh_ws_ctx_t *ws)
{
    if (ws == nullptr) {
        return;
    }
    if (ws->wslay_ctx != nullptr) {
        wslay_event_context_free(ws->wslay_ctx);
        ws->wslay_ctx = nullptr;
    }
}

/* ---- Event processing ---- */

int ioh_ws_recv(ioh_ws_ctx_t *ws)
{
    if (ws == nullptr || ws->wslay_ctx == nullptr) {
        return -EINVAL;
    }

    int ret = wslay_event_recv(ws->wslay_ctx);
    if (ret != 0) {
        return -EIO;
    }
    return 0;
}

int ioh_ws_send(ioh_ws_ctx_t *ws)
{
    if (ws == nullptr || ws->wslay_ctx == nullptr) {
        return -EINVAL;
    }

    int ret = wslay_event_send(ws->wslay_ctx);
    if (ret != 0) {
        return -EIO;
    }
    return 0;
}

/* ---- Message queueing ---- */

static int ws_queue_msg(ioh_ws_ctx_t *ws, uint8_t opcode, const uint8_t *data, size_t len)
{
    if (ws == nullptr || ws->wslay_ctx == nullptr) {
        return -EINVAL;
    }

    struct wslay_event_msg msg = {
        .opcode = opcode,
        .msg = data,
        .msg_length = len,
    };

    int ret = wslay_event_queue_msg(ws->wslay_ctx, &msg);
    if (ret != 0) {
        if (ret == WSLAY_ERR_NOMEM) {
            return -ENOMEM;
        }
        return -EINVAL;
    }
    return 0;
}

int ioh_ws_queue_text(ioh_ws_ctx_t *ws, const char *msg, size_t len)
{
    return ws_queue_msg(ws, WSLAY_TEXT_FRAME, (const uint8_t *)msg, len);
}

int ioh_ws_queue_binary(ioh_ws_ctx_t *ws, const uint8_t *data, size_t len)
{
    return ws_queue_msg(ws, WSLAY_BINARY_FRAME, data, len);
}

int ioh_ws_queue_close(ioh_ws_ctx_t *ws, uint16_t code, const char *reason, size_t reason_len)
{
    if (ws == nullptr || ws->wslay_ctx == nullptr) {
        return -EINVAL;
    }

    int ret = wslay_event_queue_close(ws->wslay_ctx, code, (const uint8_t *)reason, reason_len);
    if (ret != 0) {
        return -EINVAL;
    }
    return 0;
}

int ioh_ws_queue_ping(ioh_ws_ctx_t *ws)
{
    return ws_queue_msg(ws, WSLAY_PING, nullptr, 0);
}

/* ---- State queries ---- */

bool ioh_ws_want_read(const ioh_ws_ctx_t *ws)
{
    if (ws == nullptr || ws->wslay_ctx == nullptr) {
        return false;
    }
    return wslay_event_want_read(ws->wslay_ctx) != 0;
}

bool ioh_ws_want_write(const ioh_ws_ctx_t *ws)
{
    if (ws == nullptr || ws->wslay_ctx == nullptr) {
        return false;
    }
    return wslay_event_want_write(ws->wslay_ctx) != 0;
}

bool ioh_ws_close_received(const ioh_ws_ctx_t *ws)
{
    if (ws == nullptr || ws->wslay_ctx == nullptr) {
        return false;
    }
    return wslay_event_get_close_received(ws->wslay_ctx) != 0;
}

bool ioh_ws_close_sent(const ioh_ws_ctx_t *ws)
{
    if (ws == nullptr || ws->wslay_ctx == nullptr) {
        return false;
    }
    return wslay_event_get_close_sent(ws->wslay_ctx) != 0;
}

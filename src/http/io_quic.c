/**
 * @file io_quic.c
 * @brief QUIC transport — ngtcp2 + ngtcp2_crypto_wolfssl implementation.
 *
 * Buffer-based I/O: no sockets. Application feeds UDP datagrams
 * via io_quic_on_recv(), retrieves output via io_quic_flush().
 */

#include "http/io_quic.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <wolfssl/options.h>
#include <wolfssl/ssl.h>
#include <ngtcp2/ngtcp2.h>
#include <ngtcp2/ngtcp2_crypto.h>
#include <ngtcp2/ngtcp2_crypto_wolfssl.h>

/* ---- Constants ---- */

constexpr uint32_t QUIC_DEFAULT_MAX_STREAMS_BIDI = 100;
constexpr uint32_t QUIC_DEFAULT_MAX_STREAM_DATA = 256 * 1024;
constexpr uint32_t QUIC_DEFAULT_MAX_DATA = 1024 * 1024;
constexpr uint32_t QUIC_DEFAULT_IDLE_TIMEOUT_MS = 30000;
constexpr uint32_t QUIC_DEFAULT_MAX_UDP_PAYLOAD = 1200;
constexpr size_t QUIC_OUTPUT_BUF_SIZE = 65536;
constexpr size_t QUIC_MAX_PKTLEN = 1452;

/* ---- Internal connection state ---- */

struct io_quic_conn {
    ngtcp2_conn *ngtcp2_conn;
    WOLFSSL_CTX *ssl_ctx;
    WOLFSSL *ssl;
    io_quic_config_t config;
    io_quic_callbacks_t callbacks;
    void *user_data;

    /* Crypto conn ref for wolfSSL <-> ngtcp2 bridge */
    ngtcp2_crypto_conn_ref conn_ref;

    /* Path storage for connection */
    ngtcp2_path_storage ps;

    /* Output buffer for flush() */
    uint8_t *out_buf;
    size_t out_len;
    size_t out_cap;

    /* Connection state */
    bool handshake_done;
    bool closed;
};

/* ---- Helpers ---- */

static ngtcp2_tstamp quic_timestamp(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (ngtcp2_tstamp)ts.tv_sec * NGTCP2_SECONDS +
           (ngtcp2_tstamp)ts.tv_nsec * NGTCP2_NANOSECONDS;
}

static socklen_t sockaddr_len(const struct sockaddr *addr)
{
    if (addr->sa_family == AF_INET6) {
        return sizeof(struct sockaddr_in6);
    }
    return sizeof(struct sockaddr_in);
}

/* ---- ngtcp2 callbacks ---- */

static int recv_stream_data_cb(ngtcp2_conn *conn, uint32_t flags, int64_t stream_id,
                               uint64_t offset, const uint8_t *data, size_t datalen,
                               void *user_data, void *stream_user_data)
{
    (void)conn;
    (void)offset;
    (void)stream_user_data;

    io_quic_conn_t *qconn = user_data;
    if (qconn->callbacks.on_stream_data == nullptr) {
        return 0;
    }

    bool fin = (flags & NGTCP2_STREAM_DATA_FLAG_FIN) != 0;
    int rc =
        qconn->callbacks.on_stream_data(qconn, stream_id, data, datalen, fin, qconn->user_data);
    return (rc == 0) ? 0 : NGTCP2_ERR_CALLBACK_FAILURE;
}

static int stream_open_cb(ngtcp2_conn *conn, int64_t stream_id, void *user_data)
{
    (void)conn;

    io_quic_conn_t *qconn = user_data;
    if (qconn->callbacks.on_stream_open == nullptr) {
        return 0;
    }

    int rc = qconn->callbacks.on_stream_open(qconn, stream_id, qconn->user_data);
    return (rc == 0) ? 0 : NGTCP2_ERR_CALLBACK_FAILURE;
}

static int handshake_completed_cb(ngtcp2_conn *conn, void *user_data)
{
    (void)conn;

    io_quic_conn_t *qconn = user_data;
    qconn->handshake_done = true;

    if (qconn->callbacks.on_handshake_done != nullptr) {
        qconn->callbacks.on_handshake_done(qconn, qconn->user_data);
    }

    return 0;
}

static void rand_cb(uint8_t *dest, size_t destlen, const ngtcp2_rand_ctx *rand_ctx)
{
    (void)rand_ctx;
    /* wolfSSL provides a good CSPRNG via wc_RNG */
    wolfSSL_RAND_bytes(dest, (int)destlen);
}

static int get_new_connection_id_cb(ngtcp2_conn *conn, ngtcp2_cid *cid, uint8_t *token,
                                    size_t cidlen, void *user_data)
{
    (void)conn;
    (void)user_data;

    /* Generate random CID */
    wolfSSL_RAND_bytes(cid->data, (int)cidlen);
    cid->datalen = cidlen;

    /* Generate stateless reset token */
    wolfSSL_RAND_bytes(token, NGTCP2_STATELESS_RESET_TOKENLEN);

    return 0;
}

/* Bridge callback: wolfSSL -> ngtcp2 conn lookup */
static ngtcp2_conn *get_conn_cb(ngtcp2_crypto_conn_ref *conn_ref)
{
    io_quic_conn_t *qconn = conn_ref->user_data;
    return qconn->ngtcp2_conn;
}

/* ---- wolfSSL QUIC setup ---- */

static int setup_wolfssl(io_quic_conn_t *qconn, const io_quic_config_t *cfg)
{
    qconn->ssl_ctx = wolfSSL_CTX_new(wolfTLSv1_3_server_method());
    if (qconn->ssl_ctx == nullptr) {
        return -EIO;
    }

    if (ngtcp2_crypto_wolfssl_configure_server_context(qconn->ssl_ctx) != 0) {
        wolfSSL_CTX_free(qconn->ssl_ctx);
        qconn->ssl_ctx = nullptr;
        return -EIO;
    }

    if (wolfSSL_CTX_use_certificate_file(qconn->ssl_ctx, cfg->cert_file, WOLFSSL_FILETYPE_PEM) !=
        WOLFSSL_SUCCESS) {
        wolfSSL_CTX_free(qconn->ssl_ctx);
        qconn->ssl_ctx = nullptr;
        return -EIO;
    }

    if (wolfSSL_CTX_use_PrivateKey_file(qconn->ssl_ctx, cfg->key_file, WOLFSSL_FILETYPE_PEM) !=
        WOLFSSL_SUCCESS) {
        wolfSSL_CTX_free(qconn->ssl_ctx);
        qconn->ssl_ctx = nullptr;
        return -EIO;
    }

    /* Set ALPN for HTTP/3 */
    static const unsigned char alpn[] = "\x02h3";
    if (wolfSSL_CTX_set_alpn_protos(qconn->ssl_ctx, alpn, sizeof(alpn) - 1) != 0) {
        wolfSSL_CTX_free(qconn->ssl_ctx);
        qconn->ssl_ctx = nullptr;
        return -EIO;
    }

    qconn->ssl = wolfSSL_new(qconn->ssl_ctx);
    if (qconn->ssl == nullptr) {
        wolfSSL_CTX_free(qconn->ssl_ctx);
        qconn->ssl_ctx = nullptr;
        return -EIO;
    }

    /* Set the conn_ref so ngtcp2_crypto can find our ngtcp2_conn */
    wolfSSL_set_app_data(qconn->ssl, &qconn->conn_ref);

    return 0;
}

/* ---- Public API ---- */

void io_quic_config_init(io_quic_config_t *cfg)
{
    if (cfg == nullptr) {
        return;
    }
    memset(cfg, 0, sizeof(*cfg));
    cfg->max_streams_bidi = QUIC_DEFAULT_MAX_STREAMS_BIDI;
    cfg->max_stream_data_bidi = QUIC_DEFAULT_MAX_STREAM_DATA;
    cfg->max_data = QUIC_DEFAULT_MAX_DATA;
    cfg->idle_timeout_ms = QUIC_DEFAULT_IDLE_TIMEOUT_MS;
    cfg->max_udp_payload = QUIC_DEFAULT_MAX_UDP_PAYLOAD;
    cfg->cert_file = nullptr;
    cfg->key_file = nullptr;
}

int io_quic_config_validate(const io_quic_config_t *cfg)
{
    if (cfg == nullptr) {
        return -EINVAL;
    }
    /* QUIC requires TLS — cert and key are mandatory */
    if (cfg->cert_file == nullptr || cfg->key_file == nullptr) {
        return -EINVAL;
    }
    if (cfg->max_streams_bidi == 0) {
        return -EINVAL;
    }
    if (cfg->idle_timeout_ms == 0) {
        return -EINVAL;
    }
    return 0;
}

io_quic_conn_t *io_quic_conn_create(const io_quic_config_t *cfg, const io_quic_callbacks_t *cbs,
                                    const uint8_t *dcid, size_t dcid_len, const uint8_t *scid,
                                    size_t scid_len, const struct sockaddr *local,
                                    const struct sockaddr *remote, void *user_data)
{
    if (cfg == nullptr || cbs == nullptr || dcid == nullptr || scid == nullptr ||
        local == nullptr || remote == nullptr) {
        return nullptr;
    }

    if (io_quic_config_validate(cfg) != 0) {
        return nullptr;
    }

    io_quic_conn_t *qconn = calloc(1, sizeof(*qconn));
    if (qconn == nullptr) {
        return nullptr; //-V773
    }

    qconn->config = *cfg; //-V522
    qconn->callbacks = *cbs;
    qconn->user_data = user_data;

    /* Setup conn_ref bridge */
    qconn->conn_ref.get_conn = get_conn_cb;
    qconn->conn_ref.user_data = qconn;

    /* Allocate output buffer */
    qconn->out_buf = malloc(QUIC_OUTPUT_BUF_SIZE);
    if (qconn->out_buf == nullptr) {
        free(qconn);
        return nullptr;
    }
    qconn->out_cap = QUIC_OUTPUT_BUF_SIZE;

    /* Setup wolfSSL */
    if (setup_wolfssl(qconn, cfg) != 0) {
        free(qconn->out_buf);
        free(qconn);
        return nullptr;
    }

    /* Initialize path */
    ngtcp2_path_storage_init(&qconn->ps, (const ngtcp2_sockaddr *)local,
                             (ngtcp2_socklen)sockaddr_len(local), (const ngtcp2_sockaddr *)remote,
                             (ngtcp2_socklen)sockaddr_len(remote), nullptr);

    /* Setup ngtcp2 callbacks */
    ngtcp2_callbacks callbacks = {
        .recv_client_initial = ngtcp2_crypto_recv_client_initial_cb,
        .recv_crypto_data = ngtcp2_crypto_recv_crypto_data_cb,
        .handshake_completed = handshake_completed_cb,
        .encrypt = ngtcp2_crypto_encrypt_cb,
        .decrypt = ngtcp2_crypto_decrypt_cb,
        .hp_mask = ngtcp2_crypto_hp_mask_cb,
        .recv_stream_data = recv_stream_data_cb,
        .stream_open = stream_open_cb,
        .rand = rand_cb,
        .get_new_connection_id = get_new_connection_id_cb,
        .update_key = ngtcp2_crypto_update_key_cb,
        .delete_crypto_aead_ctx = ngtcp2_crypto_delete_crypto_aead_ctx_cb,
        .delete_crypto_cipher_ctx = ngtcp2_crypto_delete_crypto_cipher_ctx_cb,
        .get_path_challenge_data = ngtcp2_crypto_get_path_challenge_data_cb,
    };

    /* Transport parameters */
    ngtcp2_transport_params params;
    ngtcp2_transport_params_default(&params);
    params.initial_max_streams_bidi = cfg->max_streams_bidi;
    params.initial_max_stream_data_bidi_local = cfg->max_stream_data_bidi;
    params.initial_max_stream_data_bidi_remote = cfg->max_stream_data_bidi;
    params.initial_max_data = cfg->max_data;
    params.max_idle_timeout = (ngtcp2_duration)cfg->idle_timeout_ms * NGTCP2_MILLISECONDS;
    params.max_udp_payload_size = cfg->max_udp_payload;

    /* Server must echo the client's DCID as original_dcid */
    ngtcp2_cid_init(&params.original_dcid, dcid, dcid_len);
    params.original_dcid_present = 1;

    /* Settings */
    ngtcp2_settings settings;
    ngtcp2_settings_default(&settings);
    settings.initial_ts = quic_timestamp();
    settings.max_tx_udp_payload_size = QUIC_MAX_PKTLEN;

    /* Connection IDs */
    ngtcp2_cid nd_cid;
    ngtcp2_cid ns_cid;
    ngtcp2_cid_init(&nd_cid, dcid, dcid_len);
    ngtcp2_cid_init(&ns_cid, scid, scid_len);

    /* Create server connection */
    int rv = ngtcp2_conn_server_new(&qconn->ngtcp2_conn, &nd_cid, &ns_cid, &qconn->ps.path,
                                    NGTCP2_PROTO_VER_V1, &callbacks, &settings, &params, nullptr,
                                    qconn);
    if (rv != 0) {
        wolfSSL_free(qconn->ssl);
        wolfSSL_CTX_free(qconn->ssl_ctx);
        free(qconn->out_buf);
        free(qconn);
        return nullptr;
    }

    /* Bind TLS handle to ngtcp2 connection */
    ngtcp2_conn_set_tls_native_handle(qconn->ngtcp2_conn, qconn->ssl);

    return qconn;
}

void io_quic_conn_destroy(io_quic_conn_t *conn)
{
    if (conn == nullptr) {
        return;
    }
    if (conn->ngtcp2_conn != nullptr) {
        ngtcp2_conn_del(conn->ngtcp2_conn);
    }
    if (conn->ssl != nullptr) {
        wolfSSL_free(conn->ssl);
    }
    if (conn->ssl_ctx != nullptr) {
        wolfSSL_CTX_free(conn->ssl_ctx);
    }
    free(conn->out_buf);
    free(conn);
}

int io_quic_on_recv(io_quic_conn_t *conn, const uint8_t *data, size_t len,
                    const struct sockaddr *remote)
{
    if (conn == nullptr || data == nullptr || remote == nullptr) {
        return -EINVAL;
    }
    if (conn->closed) {
        return -ECONNRESET;
    }

    ngtcp2_path path = conn->ps.path;
    path.remote.addr = (ngtcp2_sockaddr *)remote;
    path.remote.addrlen = (ngtcp2_socklen)sockaddr_len(remote);

    ngtcp2_pkt_info pi = {0};
    ngtcp2_tstamp ts = quic_timestamp();

    int rv = ngtcp2_conn_read_pkt(conn->ngtcp2_conn, &path, &pi, data, len, ts);
    if (rv != 0) {
        if (rv == NGTCP2_ERR_DRAINING) {
            conn->closed = true;
            return -ECONNRESET;
        }
        return -EIO;
    }

    return 0;
}

int io_quic_flush(io_quic_conn_t *conn, const uint8_t **out_data, size_t *out_len)
{
    if (conn == nullptr || out_data == nullptr || out_len == nullptr) {
        return -EINVAL;
    }

    /* Reset output buffer */
    conn->out_len = 0;

    ngtcp2_tstamp ts = quic_timestamp();
    ngtcp2_path_storage ps;
    ngtcp2_path_storage_zero(&ps);
    ngtcp2_pkt_info pi;

    for (;;) {
        /* Ensure we have room for a packet */
        if (conn->out_len + QUIC_MAX_PKTLEN > conn->out_cap) {
            size_t new_cap = conn->out_cap * 2;
            uint8_t *new_buf = realloc(conn->out_buf, new_cap);
            if (new_buf == nullptr) {
                return -ENOMEM; //-V773
            }
            conn->out_buf = new_buf;
            conn->out_cap = new_cap;
        }

        ngtcp2_ssize n = ngtcp2_conn_writev_stream(conn->ngtcp2_conn, &ps.path, &pi,
                                                   conn->out_buf + conn->out_len, QUIC_MAX_PKTLEN,
                                                   nullptr, NGTCP2_WRITE_STREAM_FLAG_NONE, -1,
                                                   nullptr, 0, ts);

        if (n < 0) {
            if (n == NGTCP2_ERR_WRITE_MORE) {
                continue;
            }
            /* Fatal error or nothing to write */
            break;
        }
        if (n == 0) {
            break;
        }

        conn->out_len += (size_t)n;
    }

    *out_data = conn->out_buf;
    *out_len = conn->out_len;
    return 0;
}

int io_quic_on_timeout(io_quic_conn_t *conn)
{
    if (conn == nullptr) {
        return -EINVAL;
    }
    if (conn->closed) {
        return -ECONNRESET;
    }

    ngtcp2_tstamp ts = quic_timestamp();
    int rv = ngtcp2_conn_handle_expiry(conn->ngtcp2_conn, ts);
    if (rv != 0) {
        if (rv == NGTCP2_ERR_IDLE_CLOSE || rv == NGTCP2_ERR_HANDSHAKE_TIMEOUT) {
            conn->closed = true;
            return -ETIMEDOUT;
        }
        return -EIO;
    }

    return 0;
}

ssize_t io_quic_write_stream(io_quic_conn_t *conn, int64_t stream_id, const uint8_t *data,
                             size_t len, bool fin)
{
    if (conn == nullptr || (data == nullptr && len > 0)) {
        return -EINVAL;
    }
    if (conn->closed) {
        return -ECONNRESET;
    }

    /* Ensure we have room for a packet */
    if (conn->out_len + QUIC_MAX_PKTLEN > conn->out_cap) {
        size_t new_cap = conn->out_cap * 2;
        uint8_t *new_buf = realloc(conn->out_buf, new_cap);
        if (new_buf == nullptr) {
            return -ENOMEM; //-V773
        }
        conn->out_buf = new_buf;
        conn->out_cap = new_cap;
    }

    ngtcp2_tstamp ts = quic_timestamp();
    ngtcp2_path_storage ps;
    ngtcp2_path_storage_zero(&ps);
    ngtcp2_pkt_info pi;

    ngtcp2_vec datav = {.base = (uint8_t *)data, .len = len};
    ngtcp2_ssize ndatalen = 0;

    uint32_t flags = NGTCP2_WRITE_STREAM_FLAG_NONE;
    if (fin) {
        flags |= NGTCP2_WRITE_STREAM_FLAG_FIN;
    }

    ngtcp2_ssize n = ngtcp2_conn_writev_stream(conn->ngtcp2_conn, &ps.path, &pi,
                                               conn->out_buf + conn->out_len, QUIC_MAX_PKTLEN,
                                               &ndatalen, flags, stream_id, &datav, 1, ts);

    if (n < 0) {
        return -EIO;
    }
    if (n > 0) {
        conn->out_len += (size_t)n;
    }

    return (ndatalen >= 0) ? ndatalen : -EIO;
}

uint64_t io_quic_get_timeout(const io_quic_conn_t *conn)
{
    if (conn == nullptr || conn->ngtcp2_conn == nullptr) {
        return UINT64_MAX;
    }

    ngtcp2_tstamp expiry = ngtcp2_conn_get_expiry(conn->ngtcp2_conn);
    if (expiry == UINT64_MAX) {
        return UINT64_MAX;
    }

    ngtcp2_tstamp now = quic_timestamp();
    if (expiry <= now) {
        return 0;
    }

    /* Convert nanoseconds delta to milliseconds */
    return (expiry - now) / NGTCP2_MILLISECONDS;
}

bool io_quic_is_handshake_done(const io_quic_conn_t *conn)
{
    if (conn == nullptr || conn->ngtcp2_conn == nullptr) {
        return false;
    }
    return ngtcp2_conn_get_handshake_completed(conn->ngtcp2_conn) != 0;
}

bool io_quic_is_closed(const io_quic_conn_t *conn)
{
    if (conn == nullptr) {
        return true;
    }
    if (conn->closed) {
        return true;
    }
    if (conn->ngtcp2_conn == nullptr) {
        return true;
    }
    return ngtcp2_conn_in_closing_period(conn->ngtcp2_conn) != 0 ||
           ngtcp2_conn_in_draining_period(conn->ngtcp2_conn) != 0;
}

bool io_quic_want_write(const io_quic_conn_t *conn)
{
    if (conn == nullptr || conn->ngtcp2_conn == nullptr || conn->closed) {
        return false;
    }
    /* There is data to write if expiry is not infinite or output is pending */
    return conn->out_len > 0 || ngtcp2_conn_get_expiry(conn->ngtcp2_conn) != UINT64_MAX;
}

int io_quic_open_uni_stream(io_quic_conn_t *conn, int64_t *stream_id)
{
    if (conn == nullptr || stream_id == nullptr) {
        return -EINVAL;
    }
    if (conn->closed || conn->ngtcp2_conn == nullptr) {
        return -ECONNRESET;
    }

    int rv = ngtcp2_conn_open_uni_stream(conn->ngtcp2_conn, stream_id, nullptr);
    if (rv != 0) {
        return -EIO;
    }

    return 0;
}

int io_quic_close(io_quic_conn_t *conn, uint64_t error_code)
{
    if (conn == nullptr) {
        return -EINVAL;
    }
    if (conn->closed) {
        return 0;
    }

    ngtcp2_ccerr ccerr;
    ngtcp2_ccerr_default(&ccerr);
    if (error_code != 0) {
        ngtcp2_ccerr_set_application_error(&ccerr, error_code, nullptr, 0);
    }

    /* Ensure room in output buffer */
    if (conn->out_len + QUIC_MAX_PKTLEN > conn->out_cap) {
        size_t new_cap = conn->out_cap * 2;
        uint8_t *new_buf = realloc(conn->out_buf, new_cap);
        if (new_buf == nullptr) {
            return -ENOMEM; //-V773
        }
        conn->out_buf = new_buf;
        conn->out_cap = new_cap;
    }

    ngtcp2_tstamp ts = quic_timestamp();
    ngtcp2_path_storage ps;
    ngtcp2_path_storage_zero(&ps);
    ngtcp2_pkt_info pi;

    ngtcp2_ssize n = ngtcp2_conn_write_connection_close(conn->ngtcp2_conn, &ps.path, &pi,
                                                        conn->out_buf + conn->out_len,
                                                        QUIC_MAX_PKTLEN, &ccerr, ts);

    if (n > 0) {
        conn->out_len += (size_t)n;
    }

    conn->closed = true;
    return 0;
}

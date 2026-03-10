/**
 * @file ioh_conn.h
 * @brief Connection state machine and connection pool.
 */

#ifndef IOHTTP_CORE_CONN_H
#define IOHTTP_CORE_CONN_H

#include <linux/time_types.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h>

/* ---- Connection states ---- */

typedef enum : uint8_t {
    IOH_CONN_FREE = 0,
    IOH_CONN_ACCEPTING,
    IOH_CONN_PROXY_HEADER,
    IOH_CONN_TLS_HANDSHAKE,
    IOH_CONN_HTTP_ACTIVE,
    IOH_CONN_WEBSOCKET,
    IOH_CONN_SSE,
    IOH_CONN_QUIC, /* QUIC/HTTP/3 active */
    IOH_CONN_DRAINING,
    IOH_CONN_CLOSING,
} ioh_conn_state_t;

/* ---- Timeout phases for linked recv timeouts ---- */

typedef enum : uint8_t {
    IOH_TIMEOUT_NONE = 0,
    IOH_TIMEOUT_HEADER,
    IOH_TIMEOUT_BODY,
    IOH_TIMEOUT_KEEPALIVE,
} ioh_timeout_phase_t;

/* ---- Connection ---- */

typedef struct {
    int fd;
    ioh_conn_state_t state;
    uint32_t id; /**< unique within pool */
    struct sockaddr_storage peer_addr;
    struct sockaddr_storage proxy_addr; /**< from PROXY protocol */
    bool proxy_used;
    uint64_t created_at_ms;
    uint64_t last_activity_ms;
    void *protocol_ctx; /**< HTTP/1.1, HTTP/2, or HTTP/3 state */
    void *tls_ctx;      /**< WOLFSSL * */

    /* ---- Recv buffer ---- */
    uint8_t *recv_buf;    /**< receive buffer (heap-allocated) */
    size_t recv_buf_size; /**< total buffer capacity */
    size_t recv_len;      /**< bytes currently in buffer */

    /* ---- Send state ---- */
    uint8_t *send_buf;                   /**< pending send data */
    size_t send_len;                     /**< bytes remaining to send */
    size_t send_offset;                  /**< bytes already sent */
    bool send_active;                    /**< true if IOH_OP_SEND is in-flight */
    bool keep_alive;                     /**< HTTP/1.1 keep-alive (re-arm recv after send) */
    bool tls_done;                       /**< TLS handshake completed */
    ioh_timeout_phase_t timeout_phase;    /**< current recv timeout phase */
    struct __kernel_timespec timeout_ts; /**< linked timeout spec (must outlive SQE) */

    /* Per-request timeout overrides (0 = use server default) */
    uint32_t route_body_timeout_ms;
    uint32_t route_keepalive_timeout_ms;
} ioh_conn_t;

/* ---- Opaque pool type ---- */

typedef struct ioh_conn_pool ioh_conn_pool_t;

/* ---- Pool lifecycle ---- */

/**
 * @brief Create a connection pool.
 * @param max_conns Maximum number of simultaneous connections.
 * @return Non-null on success; nullptr on failure.
 */
[[nodiscard]] ioh_conn_pool_t *ioh_conn_pool_create(uint32_t max_conns);

/**
 * @brief Destroy a connection pool and release all resources.
 * @param pool Pool to destroy (nullptr is safe).
 */
void ioh_conn_pool_destroy(ioh_conn_pool_t *pool);

/* ---- Connection management ---- */

/**
 * @brief Allocate a connection slot from the pool.
 * @return Connection in ACCEPTING state, or nullptr if the pool is full.
 */
[[nodiscard]] ioh_conn_t *ioh_conn_alloc(ioh_conn_pool_t *pool);

/**
 * @brief Free a connection slot back to the pool.
 * @param pool  Pool that owns the connection.
 * @param conn  Connection to release.
 */
void ioh_conn_free(ioh_conn_pool_t *pool, ioh_conn_t *conn);

/**
 * @brief Find a connection by file descriptor (linear scan).
 * @return Matching connection or nullptr if not found.
 */
ioh_conn_t *ioh_conn_find(ioh_conn_pool_t *pool, int fd);

/**
 * @brief Get a connection by pool index (O(1) lookup).
 * @param pool  Connection pool.
 * @param index Zero-based index into the pool array.
 * @return Connection pointer, or nullptr if index is out of range.
 */
ioh_conn_t *ioh_conn_pool_get(ioh_conn_pool_t *pool, uint32_t index);

/* ---- State machine ---- */

/**
 * @brief Transition a connection to a new state.
 * @return 0 on success, -EINVAL on invalid transition.
 */
[[nodiscard]] int ioh_conn_transition(ioh_conn_t *conn, ioh_conn_state_t new_state);

/**
 * @brief Return the human-readable name of a connection state.
 */
const char *ioh_conn_state_name(ioh_conn_state_t state);

/* ---- Pool stats ---- */

/**
 * @brief Return the number of active (non-FREE) connections.
 */
uint32_t ioh_conn_pool_active(const ioh_conn_pool_t *pool);

/**
 * @brief Return the maximum pool capacity.
 */
uint32_t ioh_conn_pool_capacity(const ioh_conn_pool_t *pool);

#endif /* IOHTTP_CORE_CONN_H */

/**
 * @file io_server.h
 * @brief Server lifecycle management — config, create, listen, accept, shutdown.
 */

#ifndef IOHTTP_CORE_SERVER_H
#define IOHTTP_CORE_SERVER_H

#include <stdbool.h>
#include <stdint.h>
#include "core/io_conn.h"
#include "core/io_loop.h"

/* ---- Server configuration ---- */

typedef struct {
    const char *listen_addr;       /**< bind address (default "0.0.0.0") */
    uint16_t listen_port;          /**< MUST be > 0 */
    uint32_t max_connections;      /**< default 1024 */
    uint32_t queue_depth;          /**< io_uring queue depth, default 256 */
    uint32_t keepalive_timeout_ms; /**< default 65000 */
    uint32_t header_timeout_ms;    /**< default 30000 */
    uint32_t body_timeout_ms;      /**< default 60000 */
    uint32_t max_header_size;      /**< default 8192 */
    uint32_t max_body_size;        /**< default 1048576 (1 MiB) */
    bool proxy_protocol;           /**< expect PROXY header, default false */
} io_server_config_t;

/* ---- Shutdown mode ---- */

typedef enum : uint8_t {
    IO_SHUTDOWN_IMMEDIATE, /**< close all connections now */
    IO_SHUTDOWN_DRAIN,     /**< stop accepting, drain active, then stop */
} io_shutdown_mode_t;

/* ---- Opaque server type ---- */

typedef struct io_server io_server_t;

/* ---- Lifecycle ---- */

/**
 * @brief Initialize server configuration with sensible defaults.
 * @param cfg Configuration to initialize.
 */
void io_server_config_init(io_server_config_t *cfg);

/**
 * @brief Validate a server configuration.
 * @return 0 on success, negative errno on error.
 */
[[nodiscard]] int io_server_config_validate(const io_server_config_t *cfg);

/**
 * @brief Create a new server with the given configuration.
 * @return Non-null on success; nullptr on failure.
 */
[[nodiscard]] io_server_t *io_server_create(const io_server_config_t *cfg);

/**
 * @brief Destroy a server and release all resources.
 * @param srv Server to destroy (nullptr is safe).
 */
void io_server_destroy(io_server_t *srv);

/* ---- Access ---- */

/**
 * @brief Return a pointer to the server's event loop.
 */
io_loop_t *io_server_loop(io_server_t *srv);

/**
 * @brief Return a pointer to the server's connection pool.
 */
io_conn_pool_t *io_server_pool(io_server_t *srv);

/**
 * @brief Return the listen socket file descriptor.
 * @return fd >= 0 if listening, -1 otherwise.
 */
int io_server_listen_fd(const io_server_t *srv);

/* ---- Run ---- */

/**
 * @brief Bind + listen + setup multishot accept.
 * @return listen fd (>= 0) on success, negative errno on error.
 */
[[nodiscard]] int io_server_listen(io_server_t *srv);

/**
 * @brief Run one iteration of the event loop, processing accept CQEs.
 * @return CQEs processed (>= 0) or negative errno on error.
 */
[[nodiscard]] int io_server_run_once(io_server_t *srv, uint32_t timeout_ms);

/**
 * @brief Signal the server to stop accepting and running.
 */
void io_server_stop(io_server_t *srv);

/**
 * @brief Shutdown: IMMEDIATE closes all, DRAIN stops accepting + waits.
 * @return 0 on success, negative errno on error.
 */
[[nodiscard]] int io_server_shutdown(io_server_t *srv, io_shutdown_mode_t mode);

/* ---- Forward declarations ---- */

typedef struct io_router io_router_t;
typedef struct io_tls_ctx io_tls_ctx_t;
typedef struct io_ctx io_ctx_t;

/* ---- Request callback (used when no router is set) ---- */

typedef int (*io_server_on_request_fn)(io_ctx_t *c, void *user_data);

/* ---- Configuration extensions ---- */

/**
 * @brief Attach a router for request dispatch.
 * @param srv  Server instance.
 * @param router  Router (NOT owned by server, caller manages lifetime).
 * @return 0 on success, -EINVAL if srv is nullptr.
 */
[[nodiscard]] int io_server_set_router(io_server_t *srv, io_router_t *router);

/**
 * @brief Set a fallback request callback (used when no router is set).
 * @param srv  Server instance.
 * @param fn   Callback function (may be nullptr to clear).
 * @param user_data  Opaque pointer passed to callback.
 * @return 0 on success, -EINVAL if srv is nullptr.
 */
[[nodiscard]] int io_server_set_on_request(io_server_t *srv, io_server_on_request_fn fn,
                                           void *user_data);

/**
 * @brief Attach a TLS context for encrypted connections.
 * @param srv  Server instance.
 * @param tls_ctx  TLS context (NOT owned by server, caller manages lifetime).
 * @return 0 on success, -EINVAL if srv is nullptr.
 */
[[nodiscard]] int io_server_set_tls(io_server_t *srv, io_tls_ctx_t *tls_ctx);

#endif /* IOHTTP_CORE_SERVER_H */

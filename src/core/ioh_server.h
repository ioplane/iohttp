/**
 * @file ioh_server.h
 * @brief Server lifecycle management — config, create, listen, accept, shutdown.
 */

#ifndef IOHTTP_CORE_SERVER_H
#define IOHTTP_CORE_SERVER_H

#include <stdbool.h>
#include <stdint.h>
#include "core/ioh_conn.h"
#include "core/ioh_loop.h"

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
} ioh_server_config_t;

/* ---- Shutdown mode ---- */

typedef enum : uint8_t {
    IOH_SHUTDOWN_IMMEDIATE, /**< close all connections now */
    IOH_SHUTDOWN_DRAIN,     /**< stop accepting, drain active, then stop */
} ioh_shutdown_mode_t;

/* ---- Opaque server type ---- */

typedef struct ioh_server ioh_server_t;

/* ---- Lifecycle ---- */

/**
 * @brief Initialize server configuration with sensible defaults.
 * @param cfg Configuration to initialize.
 */
void ioh_server_config_init(ioh_server_config_t *cfg);

/**
 * @brief Validate a server configuration.
 * @return 0 on success, negative errno on error.
 */
[[nodiscard]] int ioh_server_config_validate(const ioh_server_config_t *cfg);

/**
 * @brief Create a new server with the given configuration.
 * @return Non-null on success; nullptr on failure.
 */
[[nodiscard]] ioh_server_t *ioh_server_create(const ioh_server_config_t *cfg);

/**
 * @brief Destroy a server and release all resources.
 * @param srv Server to destroy (nullptr is safe).
 */
void ioh_server_destroy(ioh_server_t *srv);

/* ---- Access ---- */

/**
 * @brief Return a pointer to the server's event loop.
 */
ioh_loop_t *ioh_server_loop(ioh_server_t *srv);

/**
 * @brief Return a pointer to the server's connection pool.
 */
ioh_conn_pool_t *ioh_server_pool(ioh_server_t *srv);

/**
 * @brief Return the listen socket file descriptor.
 * @return fd >= 0 if listening, -1 otherwise.
 */
int ioh_server_listen_fd(const ioh_server_t *srv);

/* ---- Run ---- */

/**
 * @brief Bind + listen + setup multishot accept.
 * @return listen fd (>= 0) on success, negative errno on error.
 */
[[nodiscard]] int ioh_server_listen(ioh_server_t *srv);

/**
 * @brief Run one iteration of the event loop, processing accept CQEs.
 * @return CQEs processed (>= 0) or negative errno on error.
 */
[[nodiscard]] int ioh_server_run_once(ioh_server_t *srv, uint32_t timeout_ms);

/**
 * @brief Run the event loop until ioh_server_stop() is called.
 * @param srv Server instance (must have listen_port configured).
 * @return 0 on normal stop, negative errno on error.
 */
[[nodiscard]] int ioh_server_run(ioh_server_t *srv);

/**
 * @brief Signal the server to stop accepting and running.
 */
void ioh_server_stop(ioh_server_t *srv);

/**
 * @brief Shutdown: IMMEDIATE closes all, DRAIN stops accepting + waits.
 * @return 0 on success, negative errno on error.
 */
[[nodiscard]] int ioh_server_shutdown(ioh_server_t *srv, ioh_shutdown_mode_t mode);

/**
 * @brief Check if server is in draining/shutdown state.
 */
bool ioh_server_is_draining(const ioh_server_t *srv);

/* ---- Forward declarations ---- */

typedef struct ioh_router ioh_router_t;
typedef struct ioh_tls_ctx ioh_tls_ctx_t;
typedef struct ioh_ctx ioh_ctx_t;

/* ---- Request callback (used when no router is set) ---- */

typedef int (*ioh_server_on_request_fn)(ioh_ctx_t *c, void *user_data);

/* ---- Configuration extensions ---- */

/**
 * @brief Attach a router for request dispatch.
 * @param srv  Server instance.
 * @param router  Router (NOT owned by server, caller manages lifetime).
 * @return 0 on success, -EINVAL if srv is nullptr.
 */
[[nodiscard]] int ioh_server_set_router(ioh_server_t *srv, ioh_router_t *router);

/**
 * @brief Set a fallback request callback (used when no router is set).
 * @param srv  Server instance.
 * @param fn   Callback function (may be nullptr to clear).
 * @param user_data  Opaque pointer passed to callback.
 * @return 0 on success, -EINVAL if srv is nullptr.
 */
[[nodiscard]] int ioh_server_set_on_request(ioh_server_t *srv, ioh_server_on_request_fn fn,
                                           void *user_data);

/**
 * @brief Attach a TLS context for encrypted connections.
 * @param srv  Server instance.
 * @param tls_ctx  TLS context (NOT owned by server, caller manages lifetime).
 * @return 0 on success, -EINVAL if srv is nullptr.
 */
[[nodiscard]] int ioh_server_set_tls(ioh_server_t *srv, ioh_tls_ctx_t *tls_ctx);

#endif /* IOHTTP_CORE_SERVER_H */

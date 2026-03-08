/**
 * @file io_server.c
 * @brief Server lifecycle management implementation.
 */

#include "core/io_server.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <liburing.h>

/* ---- Internal struct ---- */

struct io_server {
    io_server_config_t config;
    io_loop_t *loop;
    io_conn_pool_t *pool;
    io_router_t *router;               /* NOT owned */
    io_tls_ctx_t *tls_ctx;             /* NOT owned */
    io_server_on_request_fn on_request;
    void *on_request_data;
    int listen_fd;
    bool listening;
    bool accepting; /* multishot accept armed */
    bool stopped;
};

/* ---- Lifecycle ---- */

void io_server_config_init(io_server_config_t *cfg)
{
    if (cfg == nullptr) {
        return;
    }

    memset(cfg, 0, sizeof(*cfg));
    cfg->listen_addr = "0.0.0.0";
    cfg->listen_port = 0; /* must be set by caller */
    cfg->max_connections = 1024;
    cfg->queue_depth = 256;
    cfg->keepalive_timeout_ms = 65000;
    cfg->header_timeout_ms = 30000;
    cfg->body_timeout_ms = 60000;
    cfg->max_header_size = 8192;
    cfg->max_body_size = 1048576;
    cfg->proxy_protocol = false;
}

int io_server_config_validate(const io_server_config_t *cfg)
{
    if (cfg == nullptr) {
        return -EINVAL;
    }
    if (cfg->listen_port == 0) {
        return -EINVAL;
    }
    if (cfg->max_connections == 0) {
        return -EINVAL;
    }
    return 0;
}

io_server_t *io_server_create(const io_server_config_t *cfg)
{
    if (cfg == nullptr) {
        return nullptr;
    }
    if (io_server_config_validate(cfg) != 0) {
        return nullptr;
    }

    /* Ignore SIGPIPE — mandatory for wolfSSL and network I/O */
    signal(SIGPIPE, SIG_IGN);

    io_server_t *srv = calloc(1, sizeof(*srv));
    if (srv == nullptr) {
        return nullptr;
    }

    srv->config = *cfg;
    srv->listen_fd = -1;
    srv->listening = false;
    srv->accepting = false;
    srv->stopped = false;

    /* Create the io_uring event loop */
    io_loop_config_t loop_cfg;
    io_loop_config_init(&loop_cfg);
    loop_cfg.queue_depth = cfg->queue_depth;

    srv->loop = io_loop_create(&loop_cfg);
    if (srv->loop == nullptr) {
        free(srv);
        return nullptr;
    }

    /* Create the connection pool */
    srv->pool = io_conn_pool_create(cfg->max_connections);
    if (srv->pool == nullptr) {
        io_loop_destroy(srv->loop);
        free(srv);
        return nullptr;
    }

    return srv;
}

void io_server_destroy(io_server_t *srv)
{
    if (srv == nullptr) {
        return;
    }

    if (srv->listen_fd >= 0) {
        close(srv->listen_fd);
        srv->listen_fd = -1;
    }

    io_conn_pool_destroy(srv->pool);
    io_loop_destroy(srv->loop);
    free(srv);
}

/* ---- Access ---- */

io_loop_t *io_server_loop(io_server_t *srv)
{
    if (srv == nullptr) {
        return nullptr;
    }
    return srv->loop;
}

io_conn_pool_t *io_server_pool(io_server_t *srv)
{
    if (srv == nullptr) {
        return nullptr;
    }
    return srv->pool;
}

int io_server_listen_fd(const io_server_t *srv)
{
    if (srv == nullptr) {
        return -1;
    }
    return srv->listen_fd;
}

/* ---- Helpers ---- */

static int arm_multishot_accept(io_server_t *srv)
{
    struct io_uring *ring = io_loop_ring(srv->loop);
    struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
    if (sqe == nullptr) {
        return -ENOSPC;
    }

    io_uring_prep_multishot_accept(sqe, srv->listen_fd, nullptr, nullptr, 0);
    io_uring_sqe_set_data64(sqe, IO_ENCODE_USERDATA(0, IO_OP_ACCEPT));

    srv->accepting = true;
    return 0;
}

static int arm_recv(io_server_t *srv, io_conn_t *conn)
{
    struct io_uring *ring = io_loop_ring(srv->loop);
    struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
    if (sqe == nullptr) {
        return -ENOSPC;
    }

    io_uring_prep_recv(sqe, conn->fd,
                       conn->recv_buf + conn->recv_len,
                       conn->recv_buf_size - conn->recv_len, 0);
    io_uring_sqe_set_data64(sqe, IO_ENCODE_USERDATA(conn->id, IO_OP_RECV));

    return 0;
}

/* ---- Run ---- */

int io_server_listen(io_server_t *srv)
{
    if (srv == nullptr) {
        return -EINVAL;
    }
    if (srv->listening) {
        return -EALREADY;
    }

    /* Create TCP socket */
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return -errno;
    }

    /* SO_REUSEADDR */
    int optval = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval)) < 0) {
        int err = errno;
        close(fd);
        return -err;
    }

    /* SO_REUSEPORT */
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &optval, sizeof(optval)) < 0) {
        int err = errno;
        close(fd);
        return -err;
    }

    /* Bind */
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(srv->config.listen_port);

    if (srv->config.listen_addr != nullptr) {
        if (inet_pton(AF_INET, srv->config.listen_addr, &addr.sin_addr) != 1) {
            close(fd);
            return -EINVAL;
        }
    } else {
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
    }

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        int err = errno;
        close(fd);
        return -err;
    }

    /* Listen */
    if (listen(fd, 128) < 0) {
        int err = errno;
        close(fd);
        return -err;
    }

    srv->listen_fd = fd;
    srv->listening = true;

    /* Arm multishot accept */
    int ret = arm_multishot_accept(srv);
    if (ret < 0) {
        close(fd);
        srv->listen_fd = -1;
        srv->listening = false;
        return ret;
    }

    return fd;
}

int io_server_run_once(io_server_t *srv, uint32_t timeout_ms)
{
    if (srv == nullptr) {
        return -EINVAL;
    }
    if (srv->stopped) {
        return 0;
    }

    struct io_uring *ring = io_loop_ring(srv->loop);

    /* Submit any pending SQEs */
    int ret = io_uring_submit(ring);
    if (ret < 0) {
        return ret;
    }

    /* Wait for at least one completion */
    struct io_uring_cqe *cqe;

    if (timeout_ms > 0) {
        struct __kernel_timespec ts = {
            .tv_sec = (long long)(timeout_ms / 1000),
            .tv_nsec = (long long)(timeout_ms % 1000) * 1000000LL,
        };
        ret = io_uring_wait_cqe_timeout(ring, &cqe, &ts);
    } else {
        ret = io_uring_wait_cqe(ring, &cqe);
    }

    if (ret < 0) {
        return ret;
    }

    /* Process all available CQEs */
    int processed = 0;
    unsigned head;

    io_uring_for_each_cqe(ring, head, cqe)
    {
        uint64_t ud = io_uring_cqe_get_data64(cqe);
        uint8_t op = IO_DECODE_OP(ud);

        if (op == IO_OP_ACCEPT) {
            if (cqe->res >= 0) {
                int client_fd = cqe->res;

                /* Try to allocate a connection from the pool */
                io_conn_t *conn = io_conn_alloc(srv->pool);
                if (conn != nullptr) {
                    conn->fd = client_fd;
                    (void)io_conn_transition(conn, IO_CONN_HTTP_ACTIVE);
                    (void)arm_recv(srv, conn);
                } else {
                    /* Backpressure: pool full, close immediately */
                    close(client_fd);
                }
            }

            /* Check if multishot accept is still armed */
            if (!(cqe->flags & IORING_CQE_F_MORE)) {
                srv->accepting = false;
                /* Re-arm if still listening and not stopped */
                if (srv->listening && !srv->stopped) {
                    (void)arm_multishot_accept(srv);
                }
            }
        }

        processed++;
    }

    io_uring_cq_advance(ring, (unsigned)processed);

    return processed;
}

void io_server_stop(io_server_t *srv)
{
    if (srv == nullptr) {
        return;
    }
    srv->stopped = true;
    io_loop_stop(srv->loop);
}

int io_server_shutdown(io_server_t *srv, io_shutdown_mode_t mode)
{
    if (srv == nullptr) {
        return -EINVAL;
    }

    srv->stopped = true;

    if (mode == IO_SHUTDOWN_IMMEDIATE || mode == IO_SHUTDOWN_DRAIN) {
        /* Cancel multishot accept if armed */
        if (srv->accepting) {
            struct io_uring *ring = io_loop_ring(srv->loop);
            struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
            if (sqe != nullptr) {
                io_uring_prep_cancel64(sqe, IO_ENCODE_USERDATA(0, IO_OP_ACCEPT), 0);
                io_uring_sqe_set_data64(sqe, IO_ENCODE_USERDATA(0, IO_OP_CANCEL));
            }
            srv->accepting = false;
        }

        /* Close listen socket */
        if (srv->listen_fd >= 0) {
            close(srv->listen_fd);
            srv->listen_fd = -1;
            srv->listening = false;
        }
    }

    io_loop_stop(srv->loop);
    return 0;
}

/* ---- Configuration extensions ---- */

int io_server_set_router(io_server_t *srv, io_router_t *router)
{
    if (srv == nullptr) {
        return -EINVAL;
    }
    srv->router = router;
    return 0;
}

int io_server_set_on_request(io_server_t *srv, io_server_on_request_fn fn, void *user_data)
{
    if (srv == nullptr) {
        return -EINVAL;
    }
    srv->on_request = fn;
    srv->on_request_data = user_data;
    return 0;
}

int io_server_set_tls(io_server_t *srv, io_tls_ctx_t *tls_ctx)
{
    if (srv == nullptr) {
        return -EINVAL;
    }
    srv->tls_ctx = tls_ctx;
    return 0;
}

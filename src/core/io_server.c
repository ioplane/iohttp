/**
 * @file io_server.c
 * @brief Server lifecycle management implementation.
 */

#include "core/io_server.h"

#include "core/io_ctx.h"
#include "http/io_http1.h"
#include "http/io_request.h"
#include "http/io_response.h"
#include "middleware/io_middleware.h"
#include "router/io_router.h"

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

/* ---- Pipeline helpers ---- */

static io_conn_t *find_conn_by_id(io_server_t *srv, uint32_t conn_id)
{
    io_conn_pool_t *pool = srv->pool;
    uint32_t cap = io_conn_pool_capacity(pool);
    for (uint32_t i = 0; i < cap; i++) {
        io_conn_t *c = io_conn_pool_get(pool, i);
        if (c != nullptr && c->state != IO_CONN_FREE && c->id == conn_id) {
            return c;
        }
    }
    return nullptr;
}

static int arm_send(io_server_t *srv, io_conn_t *conn,
                    const uint8_t *data, size_t len)
{
    if (conn->send_active) {
        return -EBUSY;
    }

    free(conn->send_buf);
    conn->send_buf = malloc(len);
    if (conn->send_buf == nullptr) {
        return -ENOMEM;
    }
    memcpy(conn->send_buf, data, len);
    conn->send_len = len;
    conn->send_offset = 0;
    conn->send_active = true;

    struct io_uring *ring = io_loop_ring(srv->loop);
    struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
    if (sqe == nullptr) {
        conn->send_active = false;
        return -ENOSPC;
    }

    io_uring_prep_send(sqe, conn->fd, conn->send_buf, len, MSG_NOSIGNAL);
    io_uring_sqe_set_data64(sqe, IO_ENCODE_USERDATA(conn->id, IO_OP_SEND));

    return 0;
}

static int arm_close(io_server_t *srv, io_conn_t *conn)
{
    struct io_uring *ring = io_loop_ring(srv->loop);
    struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
    if (sqe == nullptr) {
        close(conn->fd);
        conn->fd = -1;
        io_conn_free(srv->pool, conn);
        return 0;
    }

    io_uring_prep_close(sqe, conn->fd);
    io_uring_sqe_set_data64(sqe, IO_ENCODE_USERDATA(conn->id, IO_OP_CLOSE));
    (void)io_conn_transition(conn, IO_CONN_CLOSING);

    return 0;
}

static int dispatch_request(io_server_t *srv, io_conn_t *conn, io_request_t *req)
{
    io_response_t resp;
    (void)io_response_init(&resp);

    io_ctx_t ctx;
    int rc = io_ctx_init(&ctx, req, &resp, srv);
    if (rc < 0) {
        io_response_destroy(&resp);
        return rc;
    }

    if (srv->router != nullptr) {
        io_route_match_t m = io_router_dispatch(srv->router, req->method,
                                                req->path, req->path_len);
        if (m.status == IO_MATCH_FOUND && m.handler != nullptr) {
            req->param_count = m.param_count;
            for (uint32_t i = 0; i < m.param_count && i < IO_MAX_PATH_PARAMS;
                 i++) {
                req->params[i] = m.params[i];
            }

            uint32_t global_count = 0;
            io_middleware_fn *global_mw =
                io_router_global_middleware(srv->router, &global_count);
            rc = io_chain_execute(&ctx, global_mw, global_count,
                                  nullptr, 0, m.handler);
        } else if (m.status == IO_MATCH_METHOD_NOT_ALLOWED) {
            io_handler_fn h405 =
                io_router_method_not_allowed_handler(srv->router);
            if (h405 != nullptr) {
                rc = h405(&ctx);
            } else {
                rc = io_ctx_error(&ctx, 405, "Method Not Allowed");
            }
        } else {
            io_handler_fn h404 = io_router_not_found_handler(srv->router);
            if (h404 != nullptr) {
                rc = h404(&ctx);
            } else {
                rc = io_ctx_error(&ctx, 404, "Not Found");
            }
        }
    } else if (srv->on_request != nullptr) {
        rc = srv->on_request(&ctx, srv->on_request_data);
    } else {
        rc = io_ctx_error(&ctx, 503, "No handler configured");
    }

    /* Serialize HTTP/1.1 response and arm send */
    uint8_t resp_buf[8192];
    int resp_len = io_http1_serialize_response(&resp, resp_buf,
                                               sizeof(resp_buf));
    if (resp_len > 0) {
        if (arm_send(srv, conn, resp_buf, (size_t)resp_len) < 0) {
            io_ctx_destroy(&ctx);
            io_response_destroy(&resp);
            (void)arm_close(srv, conn);
            return -EIO;
        }
    }

    conn->keep_alive = req->keep_alive && (resp.status < 400);

    io_ctx_destroy(&ctx);
    io_response_destroy(&resp);

    return rc;
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
                    int recv_err = arm_recv(srv, conn);
                    if (recv_err < 0) {
                        close(conn->fd);
                        conn->fd = -1;
                        io_conn_free(srv->pool, conn);
                    }
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
        } else if (op == IO_OP_RECV) {
            uint32_t conn_id = (uint32_t)IO_DECODE_ID(ud);
            io_conn_t *conn = find_conn_by_id(srv, conn_id);

            if (conn == nullptr) {
                processed++;
                continue;
            }

            if (cqe->res <= 0) {
                (void)arm_close(srv, conn);
                processed++;
                continue;
            }

            conn->recv_len += (size_t)cqe->res;

            io_request_t req;
            int consumed = io_http1_parse_request(conn->recv_buf,
                                                  conn->recv_len, &req);

            if (consumed > 0) {
                size_t hdr_len = (size_t)consumed;
                size_t body_avail = conn->recv_len - hdr_len;

                /* Wait for full body if Content-Length specified */
                if (req.content_length > 0 &&
                    body_avail < req.content_length) {
                    (void)arm_recv(srv, conn);
                    processed++;
                    continue;
                }

                /* Set body pointer into recv buffer */
                if (req.content_length > 0) {
                    req.body = conn->recv_buf + hdr_len;
                    req.body_len = req.content_length;
                }

                size_t total_consumed = hdr_len + req.content_length;
                (void)dispatch_request(srv, conn, &req);

                size_t remaining = conn->recv_len - total_consumed;
                if (remaining > 0) {
                    memmove(conn->recv_buf,
                            conn->recv_buf + total_consumed, remaining);
                }
                conn->recv_len = remaining;
            } else if (consumed == -EAGAIN) {
                (void)arm_recv(srv, conn);
            } else {
                io_response_t bad_resp;
                (void)io_response_init(&bad_resp);
                bad_resp.status = 400;
                (void)io_response_set_body(&bad_resp,
                                           (const uint8_t *)"Bad Request",
                                           11);
                uint8_t resp_buf[512];
                int resp_len = io_http1_serialize_response(
                    &bad_resp, resp_buf, sizeof(resp_buf));
                if (resp_len > 0) {
                    (void)arm_send(srv, conn, resp_buf, (size_t)resp_len);
                }
                io_response_destroy(&bad_resp);
                conn->keep_alive = false;
            }
        } else if (op == IO_OP_SEND) {
            uint32_t conn_id = (uint32_t)IO_DECODE_ID(ud);
            io_conn_t *conn = find_conn_by_id(srv, conn_id);

            if (conn == nullptr) {
                processed++;
                continue;
            }

            conn->send_active = false;

            if (cqe->res < 0) {
                (void)arm_close(srv, conn);
            } else {
                conn->send_offset += (size_t)cqe->res;
                if (conn->send_offset < conn->send_len) {
                    size_t remaining = conn->send_len - conn->send_offset;
                    struct io_uring_sqe *send_sqe = io_uring_get_sqe(ring);
                    if (send_sqe != nullptr) {
                        conn->send_active = true;
                        io_uring_prep_send(
                            send_sqe, conn->fd,
                            conn->send_buf + conn->send_offset,
                            remaining, MSG_NOSIGNAL);
                        io_uring_sqe_set_data64(
                            send_sqe,
                            IO_ENCODE_USERDATA(conn->id, IO_OP_SEND));
                    } else {
                        /* SQE exhaustion — close connection */
                        (void)arm_close(srv, conn);
                    }
                } else {
                    free(conn->send_buf);
                    conn->send_buf = nullptr;
                    conn->send_len = 0;
                    conn->send_offset = 0;

                    if (conn->keep_alive &&
                        conn->state == IO_CONN_HTTP_ACTIVE) {
                        (void)arm_recv(srv, conn);
                    } else {
                        (void)arm_close(srv, conn);
                    }
                }
            }
        } else if (op == IO_OP_CLOSE) {
            uint32_t conn_id = (uint32_t)IO_DECODE_ID(ud);
            io_conn_t *conn = find_conn_by_id(srv, conn_id);
            if (conn != nullptr) {
                conn->fd = -1;
                io_conn_free(srv->pool, conn);
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

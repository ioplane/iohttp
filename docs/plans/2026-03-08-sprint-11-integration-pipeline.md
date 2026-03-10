# Sprint 11: Integration Pipeline — TCP End-to-End

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development to implement this plan task-by-task.

**Goal:** Wire all existing modules into a working TCP server pipeline: accept → recv → TLS → parse → route → handler → send → close. Transform iohttp from isolated modules into a functioning HTTP server.

**Architecture:** Extend `ioh_server_t` with router + TLS context + `on_request` callback. Add CQE handlers for IOH_OP_RECV, IOH_OP_SEND, IOH_OP_CLOSE in `ioh_server_run_once()`. Each accepted connection arms recv; recv completion feeds data to TLS (if configured) then HTTP/1.1 parser; parsed request dispatches through router + middleware chain; response serializes and arms send. Connection state machine drives transitions. HTTP/1.1 pipeline first; HTTP/2 ALPN upgrade in a later task.

**Tech Stack:** C23, liburing, wolfSSL, picohttpparser, io_uring (IORING_OP_RECV, IORING_OP_SEND), Unity tests.

**Existing modules used (NOT modified except where noted):**
- `ioh_loop.h/c` — ring management, provided buffers
- `ioh_conn.h/c` — connection pool, state machine
- `ioh_tls.h/c` — buffer-based TLS (feed_input/get_output/handshake/read/write)
- `ioh_http1.h/c` — parse_request, serialize_response
- `ioh_router.h/c` — dispatch (method + path → handler + params)
- `ioh_middleware.h/c` — chain_execute (global + group MW → handler)
- `ioh_ctx.h/c` — unified request context with arena
- `ioh_request.h/c` — request struct
- `ioh_response.h/c` — response builder

**Build/test:**
```bash
cmake --preset clang-debug
cmake --build --preset clang-debug
ctest --preset clang-debug
```

---

## Task 1: Extend ioh_server_t with Router and Request Callback

**Goal:** Add router, TLS context, and `on_request` callback to the server so it knows how to dispatch incoming requests.

**Files:**
- Modify: `src/core/ioh_server.h`
- Modify: `src/core/ioh_server.c`
- Modify: `tests/unit/test_ioh_server.c`

**Step 1: Add new fields to ioh_server.h**

Add forward declarations and new API functions after the existing `ioh_server_shutdown` declaration:

```c
/* Forward declarations */
typedef struct ioh_router ioh_router_t;
typedef struct ioh_tls_ctx ioh_tls_ctx_t;

/* ---- Request callback (used when no router is set) ---- */

typedef int (*ioh_server_on_request_fn)(ioh_ctx_t *c, void *user_data);

/* ---- Configuration extensions ---- */

/**
 * @brief Attach a router to the server for request dispatch.
 * @param srv Server instance.
 * @param router Router (ownership NOT transferred — caller must keep alive).
 * @return 0 on success, negative errno on error.
 */
[[nodiscard]] int ioh_server_set_router(ioh_server_t *srv, ioh_router_t *router);

/**
 * @brief Set a simple request callback (alternative to router).
 * @param srv Server instance.
 * @param fn Callback function.
 * @param user_data Opaque data passed to callback.
 * @return 0 on success, negative errno on error.
 */
[[nodiscard]] int ioh_server_set_on_request(ioh_server_t *srv, ioh_server_on_request_fn fn,
                                            void *user_data);

/**
 * @brief Attach a TLS context for encrypted connections.
 * @param srv Server instance.
 * @param tls_ctx TLS context (ownership NOT transferred).
 * @return 0 on success, negative errno on error.
 */
[[nodiscard]] int ioh_server_set_tls(ioh_server_t *srv, ioh_tls_ctx_t *tls_ctx);
```

**Step 2: Add fields to internal struct in ioh_server.c**

```c
struct ioh_server {
    ioh_server_config_t config;
    ioh_loop_t *loop;
    ioh_conn_pool_t *pool;
    ioh_router_t *router;            /* NOT owned */
    ioh_tls_ctx_t *tls_ctx;          /* NOT owned */
    ioh_server_on_request_fn on_request;
    void *on_request_data;
    int listen_fd;
    bool listening;
    bool accepting;
    bool stopped;
};
```

**Step 3: Implement setter functions in ioh_server.c**

```c
int ioh_server_set_router(ioh_server_t *srv, ioh_router_t *router)
{
    if (srv == nullptr) {
        return -EINVAL;
    }
    srv->router = router;
    return 0;
}

int ioh_server_set_on_request(ioh_server_t *srv, ioh_server_on_request_fn fn, void *user_data)
{
    if (srv == nullptr) {
        return -EINVAL;
    }
    srv->on_request = fn;
    srv->on_request_data = user_data;
    return 0;
}

int ioh_server_set_tls(ioh_server_t *srv, ioh_tls_ctx_t *tls_ctx)
{
    if (srv == nullptr) {
        return -EINVAL;
    }
    srv->tls_ctx = tls_ctx;
    return 0;
}
```

**Step 4: Add tests in test_ioh_server.c**

```c
void test_server_set_router(void)
{
    ioh_server_config_t cfg;
    ioh_server_config_init(&cfg);
    cfg.listen_port = 19001;
    ioh_server_t *srv = ioh_server_create(&cfg);
    TEST_ASSERT_NOT_NULL(srv);

    /* null server */
    TEST_ASSERT_EQUAL_INT(-EINVAL, ioh_server_set_router(nullptr, nullptr));

    /* null router is valid (unset) */
    TEST_ASSERT_EQUAL_INT(0, ioh_server_set_router(srv, nullptr));

    ioh_server_destroy(srv);
}

void test_server_set_on_request(void)
{
    ioh_server_config_t cfg;
    ioh_server_config_init(&cfg);
    cfg.listen_port = 19002;
    ioh_server_t *srv = ioh_server_create(&cfg);
    TEST_ASSERT_NOT_NULL(srv);

    TEST_ASSERT_EQUAL_INT(-EINVAL, ioh_server_set_on_request(nullptr, nullptr, nullptr));
    TEST_ASSERT_EQUAL_INT(0, ioh_server_set_on_request(srv, nullptr, nullptr));

    ioh_server_destroy(srv);
}

void test_server_set_tls(void)
{
    ioh_server_config_t cfg;
    ioh_server_config_init(&cfg);
    cfg.listen_port = 19003;
    ioh_server_t *srv = ioh_server_create(&cfg);
    TEST_ASSERT_NOT_NULL(srv);

    TEST_ASSERT_EQUAL_INT(-EINVAL, ioh_server_set_tls(nullptr, nullptr));
    TEST_ASSERT_EQUAL_INT(0, ioh_server_set_tls(srv, nullptr));

    ioh_server_destroy(srv);
}
```

Register in `main()`:
```c
RUN_TEST(test_server_set_router);
RUN_TEST(test_server_set_on_request);
RUN_TEST(test_server_set_tls);
```

**CMake:** No changes needed — test_ioh_server already links ioh_server.

**Tests:** 3 new (null safety for each setter)

---

## Task 2: Per-Connection Recv Buffer and Arm Recv After Accept

**Goal:** After accepting a connection, arm an `IORING_OP_RECV` to start reading data. Add a per-connection receive buffer to `ioh_conn_t`.

**Files:**
- Modify: `src/core/ioh_conn.h` — add recv buffer fields
- Modify: `src/core/ioh_conn.c` — alloc/free recv buffer
- Modify: `src/core/ioh_server.c` — arm recv after accept
- Modify: `tests/unit/test_ioh_conn.c`
- Modify: `tests/unit/test_ioh_server.c`

**Step 1: Add recv buffer to ioh_conn_t in ioh_conn.h**

```c
typedef struct {
    int fd;
    ioh_conn_state_t state;
    uint32_t id;
    struct sockaddr_storage peer_addr;
    struct sockaddr_storage proxy_addr;
    bool proxy_used;
    uint64_t created_at_ms;
    uint64_t last_activity_ms;
    void *protocol_ctx;
    void *tls_ctx;
    /* ---- Recv buffer ---- */
    uint8_t *recv_buf;       /**< receive buffer (heap-allocated) */
    size_t recv_buf_size;    /**< total buffer capacity */
    size_t recv_len;         /**< bytes currently in buffer */
    /* ---- Send state ---- */
    uint8_t *send_buf;       /**< pending send data */
    size_t send_len;         /**< bytes remaining to send */
    size_t send_offset;      /**< bytes already sent */
    bool send_active;        /**< true if IOH_OP_SEND is in-flight */
} ioh_conn_t;
```

**Step 2: Alloc/free in ioh_conn.c**

In `ioh_conn_alloc()`, after setting state to ACCEPTING:
```c
constexpr size_t IOH_CONN_RECV_BUF_SIZE = 8192;

/* Inside ioh_conn_alloc, after conn->state = IOH_CONN_ACCEPTING */
conn->recv_buf = malloc(IOH_CONN_RECV_BUF_SIZE);
if (conn->recv_buf == nullptr) {
    conn->state = IOH_CONN_FREE;
    pool->active_count--;
    return nullptr;
}
conn->recv_buf_size = IOH_CONN_RECV_BUF_SIZE;
conn->recv_len = 0;
conn->send_buf = nullptr;
conn->send_len = 0;
conn->send_offset = 0;
conn->send_active = false;
```

In `ioh_conn_free()`, before resetting:
```c
free(conn->recv_buf);
conn->recv_buf = nullptr;
conn->recv_buf_size = 0;
conn->recv_len = 0;
free(conn->send_buf);
conn->send_buf = nullptr;
conn->send_len = 0;
conn->send_offset = 0;
conn->send_active = false;
```

**Step 3: Arm recv after accept in ioh_server.c**

Add helper function:
```c
static int arm_recv(ioh_server_t *srv, ioh_conn_t *conn)
{
    struct io_uring *ring = ioh_loop_ring(srv->loop);
    struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
    if (sqe == nullptr) {
        return -ENOSPC;
    }

    io_uring_prep_recv(sqe, conn->fd,
                       conn->recv_buf + conn->recv_len,
                       conn->recv_buf_size - conn->recv_len, 0);
    io_uring_sqe_set_data64(sqe, IOH_ENCODE_USERDATA(conn->id, IOH_OP_RECV));

    return 0;
}
```

In `ioh_server_run_once()`, after `conn->fd = client_fd;`:
```c
conn->fd = client_fd;
(void)ioh_conn_transition(conn, IOH_CONN_HTTP_ACTIVE);
(void)arm_recv(srv, conn);
```

Note: For now, skip TLS and go straight to HTTP_ACTIVE. TLS handshake integration is Task 5.

**Step 4: Tests**

In `test_ioh_conn.c`:
```c
void test_conn_alloc_has_recv_buffer(void)
{
    ioh_conn_pool_t *pool = ioh_conn_pool_create(4);
    TEST_ASSERT_NOT_NULL(pool);

    ioh_conn_t *conn = ioh_conn_alloc(pool);
    TEST_ASSERT_NOT_NULL(conn);
    TEST_ASSERT_NOT_NULL(conn->recv_buf);
    TEST_ASSERT_GREATER_THAN(0, (int)conn->recv_buf_size);
    TEST_ASSERT_EQUAL(0, conn->recv_len);
    TEST_ASSERT_FALSE(conn->send_active);

    ioh_conn_free(pool, conn);
    ioh_conn_pool_destroy(pool);
}
```

In `test_ioh_server.c`:
```c
void test_server_accept_arms_recv(void)
{
    ioh_server_config_t cfg = make_config(19010, 16);
    ioh_server_t *srv = ioh_server_create(&cfg);
    TEST_ASSERT_NOT_NULL(srv);

    int fd = ioh_server_listen(srv);
    TEST_ASSERT_GREATER_THAN(0, fd);
    uint16_t port = get_bound_port(fd);

    int client_fd = connect_client(port);
    TEST_ASSERT_TRUE(client_fd >= 0);

    /* Run once to process accept — should also arm recv */
    int ret = ioh_server_run_once(srv, 1000);
    TEST_ASSERT_GREATER_THAN(0, ret);
    TEST_ASSERT_EQUAL_UINT32(1, ioh_conn_pool_active(ioh_server_pool(srv)));

    close(client_fd);
    ioh_server_destroy(srv);
}
```

**Tests:** 2 new

---

## Task 3: CQE Handler for IOH_OP_RECV — HTTP/1.1 Parse and Dispatch

**Goal:** Handle recv completions: parse HTTP/1.1 request, dispatch through router (or `on_request` callback), serialize response, arm send. This is the core pipeline task.

**Files:**
- Modify: `src/core/ioh_server.c` — add IOH_OP_RECV handler, dispatch logic, response send
- Create: `tests/integration/test_pipeline.c` — real TCP end-to-end tests
- Modify: `CMakeLists.txt` — add test_pipeline target

**Step 1: Add connection lookup helper in ioh_server.c**

```c
static ioh_conn_t *find_conn_by_id(ioh_server_t *srv, uint32_t conn_id)
{
    /* ioh_conn_pool stores conns in array; id is assigned at alloc.
     * For now, linear scan. Optimize later if needed. */
    ioh_conn_pool_t *pool = srv->pool;
    uint32_t cap = ioh_conn_pool_capacity(pool);
    for (uint32_t i = 0; i < cap; i++) {
        ioh_conn_t *c = ioh_conn_pool_get(pool, i);
        if (c != nullptr && c->state != IOH_CONN_FREE && c->id == conn_id) {
            return c;
        }
    }
    return nullptr;
}
```

Note: Requires adding `ioh_conn_pool_get(pool, index)` to `ioh_conn.h/c`:

```c
/* ioh_conn.h */
/**
 * @brief Get connection by pool index (for iteration).
 * @return Connection pointer or nullptr if index out of range.
 */
ioh_conn_t *ioh_conn_pool_get(ioh_conn_pool_t *pool, uint32_t index);

/* ioh_conn.c */
ioh_conn_t *ioh_conn_pool_get(ioh_conn_pool_t *pool, uint32_t index)
{
    if (pool == nullptr || index >= pool->max_conns) {
        return nullptr;
    }
    return &pool->conns[index];
}
```

**Step 2: Add arm_send helper in ioh_server.c**

```c
static int arm_send(ioh_server_t *srv, ioh_conn_t *conn,
                    const uint8_t *data, size_t len)
{
    if (conn->send_active) {
        return -EBUSY; /* send serialization: one at a time */
    }

    /* Copy response data — conn owns send buffer */
    free(conn->send_buf);
    conn->send_buf = malloc(len);
    if (conn->send_buf == nullptr) {
        return -ENOMEM;
    }
    memcpy(conn->send_buf, data, len);
    conn->send_len = len;
    conn->send_offset = 0;
    conn->send_active = true;

    struct io_uring *ring = ioh_loop_ring(srv->loop);
    struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
    if (sqe == nullptr) {
        conn->send_active = false;
        return -ENOSPC;
    }

    io_uring_prep_send(sqe, conn->fd, conn->send_buf, len, MSG_NOSIGNAL);
    io_uring_sqe_set_data64(sqe, IOH_ENCODE_USERDATA(conn->id, IOH_OP_SEND));

    return 0;
}
```

**Step 3: Add arm_close helper in ioh_server.c**

```c
static int arm_close(ioh_server_t *srv, ioh_conn_t *conn)
{
    struct io_uring *ring = ioh_loop_ring(srv->loop);
    struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
    if (sqe == nullptr) {
        /* Fallback: synchronous close */
        close(conn->fd);
        conn->fd = -1;
        ioh_conn_free(srv->pool, conn);
        return 0;
    }

    io_uring_prep_close(sqe, conn->fd);
    io_uring_sqe_set_data64(sqe, IOH_ENCODE_USERDATA(conn->id, IOH_OP_CLOSE));
    conn->fd = -1; /* fd ownership transferred to kernel */
    (void)ioh_conn_transition(conn, IOH_CONN_CLOSING);

    return 0;
}
```

**Step 4: Add dispatch_request helper in ioh_server.c**

```c
static int dispatch_request(ioh_server_t *srv, ioh_conn_t *conn, ioh_request_t *req)
{
    ioh_response_t resp;
    ioh_response_init(&resp);

    ioh_ctx_t ctx;
    int rc = ioh_ctx_init(&ctx, req, &resp, srv);
    if (rc < 0) {
        ioh_response_destroy(&resp);
        return rc;
    }

    /* Dispatch: router takes priority over on_request callback */
    if (srv->router != nullptr) {
        ioh_route_match_t m = ioh_router_dispatch(srv->router, req->method,
                                                 req->path, req->path_len);
        if (m.status == IOH_MATCH_OK && m.handler != nullptr) {
            /* Copy path params into request */
            req->param_count = m.param_count;
            for (uint32_t i = 0; i < m.param_count && i < IOH_MAX_PARAMS; i++) {
                req->params[i] = m.params[i];
            }

            /* Execute middleware chain + handler */
            uint32_t global_count = 0;
            ioh_middleware_fn *global_mw = ioh_router_global_middleware(srv->router,
                                                                      &global_count);
            rc = ioh_chain_execute(&ctx, global_mw, global_count,
                                  m.group_middleware, m.group_middleware_count,
                                  m.handler);
        } else if (m.status == IOH_MATCH_METHOD_NOT_ALLOWED) {
            ioh_handler_fn h405 = ioh_router_method_not_allowed_handler(srv->router);
            if (h405 != nullptr) {
                rc = h405(&ctx);
            } else {
                rc = ioh_ctx_error(&ctx, 405, "Method Not Allowed");
            }
        } else {
            ioh_handler_fn h404 = ioh_router_not_found_handler(srv->router);
            if (h404 != nullptr) {
                rc = h404(&ctx);
            } else {
                rc = ioh_ctx_error(&ctx, 404, "Not Found");
            }
        }
    } else if (srv->on_request != nullptr) {
        rc = srv->on_request(&ctx, srv->on_request_data);
    } else {
        rc = ioh_ctx_error(&ctx, 503, "No handler configured");
    }

    /* Serialize HTTP/1.1 response and arm send */
    uint8_t resp_buf[65536];
    int resp_len = ioh_http1_serialize_response(&resp, resp_buf, sizeof(resp_buf));
    if (resp_len > 0) {
        (void)arm_send(srv, conn, resp_buf, (size_t)resp_len);
    }

    /* Determine if connection should stay alive */
    bool keep_alive = req->keep_alive && (resp.status < 400);

    ioh_ctx_destroy(&ctx);
    ioh_response_destroy(&resp);

    if (!keep_alive) {
        /* Will close after send completes */
        conn->recv_len = 0; /* signal: don't re-arm recv after send */
    }

    return rc;
}
```

**Step 5: Add IOH_OP_RECV handler in ioh_server_run_once()**

In the CQE processing loop, add after the IOH_OP_ACCEPT block:

```c
} else if (op == IOH_OP_RECV) {
    uint32_t conn_id = (uint32_t)IOH_DECODE_ID(ud);
    ioh_conn_t *conn = find_conn_by_id(srv, conn_id);

    if (conn == nullptr) {
        processed++;
        continue;
    }

    if (cqe->res <= 0) {
        /* EOF or error — close connection */
        (void)arm_close(srv, conn);
        processed++;
        continue;
    }

    /* Data received */
    conn->recv_len += (size_t)cqe->res;

    /* Try to parse HTTP/1.1 request */
    ioh_request_t req;
    int consumed = ioh_http1_parse_request(conn->recv_buf, conn->recv_len, &req);

    if (consumed > 0) {
        /* Complete request — dispatch */
        (void)dispatch_request(srv, conn, &req);

        /* Shift any remaining bytes (pipelining) */
        size_t remaining = conn->recv_len - (size_t)consumed;
        if (remaining > 0) {
            memmove(conn->recv_buf, conn->recv_buf + consumed, remaining);
        }
        conn->recv_len = remaining;
    } else if (consumed == -EAGAIN) {
        /* Incomplete request — re-arm recv for more data */
        (void)arm_recv(srv, conn);
    } else {
        /* Parse error — send 400 and close */
        ioh_response_t resp;
        ioh_response_init(&resp);
        (void)ioh_response_set_status(&resp, 400);
        (void)ioh_response_set_body(&resp, (const uint8_t *)"Bad Request", 11);
        uint8_t resp_buf[512];
        int resp_len = ioh_http1_serialize_response(&resp, resp_buf, sizeof(resp_buf));
        if (resp_len > 0) {
            (void)arm_send(srv, conn, resp_buf, (size_t)resp_len);
        }
        ioh_response_destroy(&resp);
        conn->recv_len = 0; /* signal: close after send */
    }
```

**Step 6: Add IOH_OP_SEND handler**

```c
} else if (op == IOH_OP_SEND) {
    uint32_t conn_id = (uint32_t)IOH_DECODE_ID(ud);
    ioh_conn_t *conn = find_conn_by_id(srv, conn_id);

    if (conn == nullptr) {
        processed++;
        continue;
    }

    conn->send_active = false;

    if (cqe->res < 0) {
        /* Send error — close */
        (void)arm_close(srv, conn);
    } else {
        conn->send_offset += (size_t)cqe->res;
        if (conn->send_offset < conn->send_len) {
            /* Partial send — send remaining */
            size_t remaining = conn->send_len - conn->send_offset;
            conn->send_active = true;
            struct io_uring *ring = ioh_loop_ring(srv->loop);
            struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
            if (sqe != nullptr) {
                io_uring_prep_send(sqe, conn->fd,
                                   conn->send_buf + conn->send_offset,
                                   remaining, MSG_NOSIGNAL);
                io_uring_sqe_set_data64(sqe, IOH_ENCODE_USERDATA(conn->id, IOH_OP_SEND));
            }
        } else {
            /* Send complete */
            free(conn->send_buf);
            conn->send_buf = nullptr;
            conn->send_len = 0;
            conn->send_offset = 0;

            if (conn->recv_len == 0 && conn->state == IOH_CONN_HTTP_ACTIVE) {
                /* Check if we should keep alive */
                (void)arm_recv(srv, conn);
            } else if (conn->state == IOH_CONN_CLOSING ||
                       conn->state == IOH_CONN_DRAINING) {
                (void)arm_close(srv, conn);
            } else {
                /* Re-arm recv for next request (keep-alive) */
                (void)arm_recv(srv, conn);
            }
        }
    }
```

**Step 7: Add IOH_OP_CLOSE handler**

```c
} else if (op == IOH_OP_CLOSE) {
    uint32_t conn_id = (uint32_t)IOH_DECODE_ID(ud);
    ioh_conn_t *conn = find_conn_by_id(srv, conn_id);
    if (conn != nullptr) {
        ioh_conn_free(srv->pool, conn);
    }
}
```

**Step 8: Add includes to ioh_server.c**

Add at the top:
```c
#include "core/ioh_ctx.h"
#include "http/ioh_http1.h"
#include "http/ioh_request.h"
#include "http/ioh_response.h"
#include "middleware/ioh_middleware.h"
#include "router/ioh_router.h"
```

**Step 9: Create test_pipeline.c**

```c
/**
 * @file test_pipeline.c
 * @brief End-to-end integration tests: real TCP connections through server pipeline.
 *
 * Tests the full flow: TCP connect → send HTTP request → server recv →
 * parse → route → handler → serialize → send response → client recv.
 */

#include "core/ioh_server.h"
#include "http/ioh_request.h"
#include "router/ioh_router.h"

#include <errno.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <unity.h>

void setUp(void) {}
void tearDown(void) {}

/* ---- Helpers ---- */

static uint16_t get_bound_port(int fd)
{
    struct sockaddr_in addr;
    socklen_t len = sizeof(addr);
    getsockname(fd, (struct sockaddr *)&addr, &len);
    return ntohs(addr.sin_port);
}

static int connect_to(uint16_t port)
{
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return -errno;
    }
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(port),
        .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
    };
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -errno;
    }
    return fd;
}

static int send_all(int fd, const char *data, size_t len)
{
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(fd, data + sent, len - sent, MSG_NOSIGNAL);
        if (n <= 0) {
            return -1;
        }
        sent += (size_t)n;
    }
    return 0;
}

static ssize_t recv_response(int fd, char *buf, size_t cap)
{
    size_t total = 0;
    /* Read until we find \r\n\r\n (end of headers) + body */
    while (total < cap) {
        ssize_t n = recv(fd, buf + total, cap - total, 0);
        if (n <= 0) {
            break;
        }
        total += (size_t)n;
        /* Simple: look for complete response (headers + body) */
        if (total > 4 && memmem(buf, total, "\r\n\r\n", 4) != nullptr) {
            break;
        }
    }
    return (ssize_t)total;
}

/* ---- Handler ---- */

static int hello_handler(ioh_ctx_t *c)
{
    return ioh_ctx_text(c, 200, "Hello, World!");
}

static int echo_handler(ioh_ctx_t *c)
{
    size_t body_len = 0;
    const uint8_t *body = ioh_ctx_body(c, &body_len);
    return ioh_ctx_blob(c, 200, "application/octet-stream", body, body_len);
}

/* ---- Test 1: Simple GET request/response ---- */

void test_pipeline_simple_get(void)
{
    /* Setup server with router */
    ioh_server_config_t cfg;
    ioh_server_config_init(&cfg);
    cfg.listen_port = 0; /* kernel-assigned */
    cfg.max_connections = 16;
    cfg.queue_depth = 32;

    ioh_server_t *srv = ioh_server_create(&cfg);
    TEST_ASSERT_NOT_NULL(srv);

    ioh_router_t *router = ioh_router_create();
    TEST_ASSERT_NOT_NULL(router);
    TEST_ASSERT_EQUAL_INT(0, ioh_router_get(router, "/hello", hello_handler));
    TEST_ASSERT_EQUAL_INT(0, ioh_server_set_router(srv, router));

    int listen_fd = ioh_server_listen(srv);
    TEST_ASSERT_GREATER_THAN(0, listen_fd);
    uint16_t port = get_bound_port(listen_fd);

    /* Client: connect and send GET /hello */
    int client = connect_to(port);
    TEST_ASSERT_TRUE(client >= 0);

    const char *req = "GET /hello HTTP/1.1\r\n"
                      "Host: localhost\r\n"
                      "Connection: close\r\n"
                      "\r\n";
    TEST_ASSERT_EQUAL_INT(0, send_all(client, req, strlen(req)));

    /* Server: process accept + recv + parse + dispatch + send */
    for (int i = 0; i < 5; i++) {
        ioh_server_run_once(srv, 100);
    }

    /* Client: receive response */
    char resp[4096];
    ssize_t resp_len = recv_response(client, resp, sizeof(resp));
    TEST_ASSERT_GREATER_THAN(0, resp_len);

    /* Verify response */
    TEST_ASSERT_NOT_NULL(memmem(resp, (size_t)resp_len, "200", 3));
    TEST_ASSERT_NOT_NULL(memmem(resp, (size_t)resp_len, "Hello, World!", 13));

    close(client);
    ioh_server_destroy(srv);
    ioh_router_destroy(router);
}

/* ---- Test 2: 404 Not Found ---- */

void test_pipeline_not_found(void)
{
    ioh_server_config_t cfg;
    ioh_server_config_init(&cfg);
    cfg.listen_port = 0;
    cfg.max_connections = 16;
    cfg.queue_depth = 32;

    ioh_server_t *srv = ioh_server_create(&cfg);
    TEST_ASSERT_NOT_NULL(srv);

    ioh_router_t *router = ioh_router_create();
    TEST_ASSERT_NOT_NULL(router);
    TEST_ASSERT_EQUAL_INT(0, ioh_router_get(router, "/hello", hello_handler));
    TEST_ASSERT_EQUAL_INT(0, ioh_server_set_router(srv, router));

    int listen_fd = ioh_server_listen(srv);
    uint16_t port = get_bound_port(listen_fd);

    int client = connect_to(port);
    TEST_ASSERT_TRUE(client >= 0);

    const char *req = "GET /nonexistent HTTP/1.1\r\n"
                      "Host: localhost\r\n"
                      "Connection: close\r\n"
                      "\r\n";
    send_all(client, req, strlen(req));

    for (int i = 0; i < 5; i++) {
        ioh_server_run_once(srv, 100);
    }

    char resp[4096];
    ssize_t resp_len = recv_response(client, resp, sizeof(resp));
    TEST_ASSERT_GREATER_THAN(0, resp_len);
    TEST_ASSERT_NOT_NULL(memmem(resp, (size_t)resp_len, "404", 3));

    close(client);
    ioh_server_destroy(srv);
    ioh_router_destroy(router);
}

/* ---- Test 3: POST with body ---- */

void test_pipeline_post_with_body(void)
{
    ioh_server_config_t cfg;
    ioh_server_config_init(&cfg);
    cfg.listen_port = 0;
    cfg.max_connections = 16;
    cfg.queue_depth = 32;

    ioh_server_t *srv = ioh_server_create(&cfg);
    TEST_ASSERT_NOT_NULL(srv);

    ioh_router_t *router = ioh_router_create();
    TEST_ASSERT_NOT_NULL(router);
    TEST_ASSERT_EQUAL_INT(0, ioh_router_post(router, "/echo", echo_handler));
    TEST_ASSERT_EQUAL_INT(0, ioh_server_set_router(srv, router));

    int listen_fd = ioh_server_listen(srv);
    uint16_t port = get_bound_port(listen_fd);

    int client = connect_to(port);
    TEST_ASSERT_TRUE(client >= 0);

    const char *req = "POST /echo HTTP/1.1\r\n"
                      "Host: localhost\r\n"
                      "Content-Length: 5\r\n"
                      "Connection: close\r\n"
                      "\r\n"
                      "hello";
    send_all(client, req, strlen(req));

    for (int i = 0; i < 5; i++) {
        ioh_server_run_once(srv, 100);
    }

    char resp[4096];
    ssize_t resp_len = recv_response(client, resp, sizeof(resp));
    TEST_ASSERT_GREATER_THAN(0, resp_len);
    TEST_ASSERT_NOT_NULL(memmem(resp, (size_t)resp_len, "200", 3));
    TEST_ASSERT_NOT_NULL(memmem(resp, (size_t)resp_len, "hello", 5));

    close(client);
    ioh_server_destroy(srv);
    ioh_router_destroy(router);
}

/* ---- Test 4: on_request callback (no router) ---- */

static int callback_handler(ioh_ctx_t *c, void *user_data)
{
    (void)user_data;
    return ioh_ctx_text(c, 200, "callback");
}

void test_pipeline_on_request_callback(void)
{
    ioh_server_config_t cfg;
    ioh_server_config_init(&cfg);
    cfg.listen_port = 0;
    cfg.max_connections = 16;
    cfg.queue_depth = 32;

    ioh_server_t *srv = ioh_server_create(&cfg);
    TEST_ASSERT_NOT_NULL(srv);

    TEST_ASSERT_EQUAL_INT(0, ioh_server_set_on_request(srv, callback_handler, nullptr));

    int listen_fd = ioh_server_listen(srv);
    uint16_t port = get_bound_port(listen_fd);

    int client = connect_to(port);
    TEST_ASSERT_TRUE(client >= 0);

    const char *req = "GET /anything HTTP/1.1\r\n"
                      "Host: localhost\r\n"
                      "Connection: close\r\n"
                      "\r\n";
    send_all(client, req, strlen(req));

    for (int i = 0; i < 5; i++) {
        ioh_server_run_once(srv, 100);
    }

    char resp[4096];
    ssize_t resp_len = recv_response(client, resp, sizeof(resp));
    TEST_ASSERT_GREATER_THAN(0, resp_len);
    TEST_ASSERT_NOT_NULL(memmem(resp, (size_t)resp_len, "200", 3));
    TEST_ASSERT_NOT_NULL(memmem(resp, (size_t)resp_len, "callback", 8));

    close(client);
    ioh_server_destroy(srv);
}

/* ---- Test 5: Malformed request returns 400 ---- */

void test_pipeline_bad_request(void)
{
    ioh_server_config_t cfg;
    ioh_server_config_init(&cfg);
    cfg.listen_port = 0;
    cfg.max_connections = 16;
    cfg.queue_depth = 32;

    ioh_server_t *srv = ioh_server_create(&cfg);
    TEST_ASSERT_NOT_NULL(srv);
    TEST_ASSERT_EQUAL_INT(0, ioh_server_set_on_request(srv, callback_handler, nullptr));

    int listen_fd = ioh_server_listen(srv);
    uint16_t port = get_bound_port(listen_fd);

    int client = connect_to(port);
    TEST_ASSERT_TRUE(client >= 0);

    const char *req = "INVALID GARBAGE\r\n\r\n";
    send_all(client, req, strlen(req));

    for (int i = 0; i < 5; i++) {
        ioh_server_run_once(srv, 100);
    }

    char resp[4096];
    ssize_t resp_len = recv_response(client, resp, sizeof(resp));
    TEST_ASSERT_GREATER_THAN(0, resp_len);
    TEST_ASSERT_NOT_NULL(memmem(resp, (size_t)resp_len, "400", 3));

    close(client);
    ioh_server_destroy(srv);
}

/* ---- Test 6: Client disconnect (EOF) ---- */

void test_pipeline_client_disconnect(void)
{
    ioh_server_config_t cfg;
    ioh_server_config_init(&cfg);
    cfg.listen_port = 0;
    cfg.max_connections = 16;
    cfg.queue_depth = 32;

    ioh_server_t *srv = ioh_server_create(&cfg);
    TEST_ASSERT_NOT_NULL(srv);
    TEST_ASSERT_EQUAL_INT(0, ioh_server_set_on_request(srv, callback_handler, nullptr));

    int listen_fd = ioh_server_listen(srv);
    uint16_t port = get_bound_port(listen_fd);

    int client = connect_to(port);
    TEST_ASSERT_TRUE(client >= 0);

    /* Accept the connection */
    ioh_server_run_once(srv, 100);
    TEST_ASSERT_EQUAL_UINT32(1, ioh_conn_pool_active(ioh_server_pool(srv)));

    /* Client disconnects without sending data */
    close(client);

    /* Server processes EOF → cleans up connection */
    for (int i = 0; i < 3; i++) {
        ioh_server_run_once(srv, 100);
    }

    TEST_ASSERT_EQUAL_UINT32(0, ioh_conn_pool_active(ioh_server_pool(srv)));

    ioh_server_destroy(srv);
}

/* ---- main ---- */

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_pipeline_simple_get);
    RUN_TEST(test_pipeline_not_found);
    RUN_TEST(test_pipeline_post_with_body);
    RUN_TEST(test_pipeline_on_request_callback);
    RUN_TEST(test_pipeline_bad_request);
    RUN_TEST(test_pipeline_client_disconnect);
    return UNITY_END();
}
```

**Step 10: CMakeLists.txt addition**

```cmake
# Integration: TCP pipeline end-to-end
add_executable(test_pipeline tests/integration/test_pipeline.c)
target_include_directories(test_pipeline PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_pipeline PRIVATE
    unity ioh_server ioh_loop ioh_conn ioh_ctx
    ioh_http1 ioh_request ioh_response
    ioh_router ioh_radix ioh_middleware
    ioh_route_group ioh_route_inspect ioh_route_meta)
add_test(NAME test_pipeline COMMAND test_pipeline)
```

**Tests:** 6 new integration tests

---

## Task 4: Keep-Alive and Connection Reuse

**Goal:** After send completes, re-arm recv if the connection uses HTTP/1.1 keep-alive. Track keep-alive state per connection.

**Files:**
- Modify: `src/core/ioh_conn.h` — add `keep_alive` flag
- Modify: `src/core/ioh_server.c` — use keep_alive to decide close vs re-arm
- Create: `tests/integration/test_keepalive.c`
- Modify: `CMakeLists.txt`

**Step 1: Add keep_alive to ioh_conn_t**

In `ioh_conn.h`, inside the struct:
```c
    bool keep_alive;         /**< HTTP/1.1 keep-alive (re-arm recv after send) */
```

**Step 2: Set keep_alive in dispatch_request**

In `ioh_server.c dispatch_request()`, replace the keep_alive logic:
```c
    /* Determine if connection should stay alive */
    conn->keep_alive = req->keep_alive && (resp.status < 400);
```

**Step 3: Use keep_alive in IOH_OP_SEND handler**

In the "Send complete" block:
```c
            /* Send complete */
            free(conn->send_buf);
            conn->send_buf = nullptr;
            conn->send_len = 0;
            conn->send_offset = 0;

            if (conn->keep_alive && conn->state == IOH_CONN_HTTP_ACTIVE) {
                /* Re-arm recv for next request */
                (void)arm_recv(srv, conn);
            } else {
                /* Close connection */
                (void)arm_close(srv, conn);
            }
```

**Step 4: test_keepalive.c**

```c
/**
 * @file test_keepalive.c
 * @brief Integration tests for HTTP/1.1 keep-alive connection reuse.
 */

#include "core/ioh_server.h"
#include "router/ioh_router.h"

#include <errno.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <unity.h>

void setUp(void) {}
void tearDown(void) {}

static uint16_t get_bound_port(int fd)
{
    struct sockaddr_in addr;
    socklen_t len = sizeof(addr);
    getsockname(fd, (struct sockaddr *)&addr, &len);
    return ntohs(addr.sin_port);
}

static int connect_to(uint16_t port)
{
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(port),
        .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
    };
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static int hello_handler(ioh_ctx_t *c)
{
    return ioh_ctx_text(c, 200, "OK");
}

/* ---- Test 1: Two requests on same connection ---- */

void test_keepalive_two_requests(void)
{
    ioh_server_config_t cfg;
    ioh_server_config_init(&cfg);
    cfg.listen_port = 0;
    cfg.max_connections = 16;
    cfg.queue_depth = 32;

    ioh_server_t *srv = ioh_server_create(&cfg);
    ioh_router_t *router = ioh_router_create();
    ioh_router_get(router, "/hello", hello_handler);
    ioh_server_set_router(srv, router);

    int listen_fd = ioh_server_listen(srv);
    uint16_t port = get_bound_port(listen_fd);

    int client = connect_to(port);
    TEST_ASSERT_TRUE(client >= 0);

    /* First request (keep-alive is default for HTTP/1.1) */
    const char *req1 = "GET /hello HTTP/1.1\r\n"
                       "Host: localhost\r\n"
                       "\r\n";
    send(client, req1, strlen(req1), MSG_NOSIGNAL);

    for (int i = 0; i < 5; i++) {
        ioh_server_run_once(srv, 100);
    }

    char resp[4096];
    ssize_t n = recv(client, resp, sizeof(resp), 0);
    TEST_ASSERT_GREATER_THAN(0, n);
    TEST_ASSERT_NOT_NULL(memmem(resp, (size_t)n, "200", 3));

    /* Second request on same connection */
    const char *req2 = "GET /hello HTTP/1.1\r\n"
                       "Host: localhost\r\n"
                       "Connection: close\r\n"
                       "\r\n";
    send(client, req2, strlen(req2), MSG_NOSIGNAL);

    for (int i = 0; i < 5; i++) {
        ioh_server_run_once(srv, 100);
    }

    n = recv(client, resp, sizeof(resp), 0);
    TEST_ASSERT_GREATER_THAN(0, n);
    TEST_ASSERT_NOT_NULL(memmem(resp, (size_t)n, "200", 3));

    close(client);
    ioh_server_destroy(srv);
    ioh_router_destroy(router);
}

/* ---- Test 2: Connection: close closes after first request ---- */

void test_keepalive_connection_close(void)
{
    ioh_server_config_t cfg;
    ioh_server_config_init(&cfg);
    cfg.listen_port = 0;
    cfg.max_connections = 16;
    cfg.queue_depth = 32;

    ioh_server_t *srv = ioh_server_create(&cfg);
    ioh_router_t *router = ioh_router_create();
    ioh_router_get(router, "/hello", hello_handler);
    ioh_server_set_router(srv, router);

    int listen_fd = ioh_server_listen(srv);
    uint16_t port = get_bound_port(listen_fd);

    int client = connect_to(port);
    TEST_ASSERT_TRUE(client >= 0);

    const char *req = "GET /hello HTTP/1.1\r\n"
                      "Host: localhost\r\n"
                      "Connection: close\r\n"
                      "\r\n";
    send(client, req, strlen(req), MSG_NOSIGNAL);

    for (int i = 0; i < 5; i++) {
        ioh_server_run_once(srv, 100);
    }

    char resp[4096];
    recv(client, resp, sizeof(resp), 0);

    /* After close+cleanup, pool should be empty */
    for (int i = 0; i < 3; i++) {
        ioh_server_run_once(srv, 100);
    }
    TEST_ASSERT_EQUAL_UINT32(0, ioh_conn_pool_active(ioh_server_pool(srv)));

    close(client);
    ioh_server_destroy(srv);
    ioh_router_destroy(router);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_keepalive_two_requests);
    RUN_TEST(test_keepalive_connection_close);
    return UNITY_END();
}
```

**CMake:**
```cmake
add_executable(test_keepalive tests/integration/test_keepalive.c)
target_include_directories(test_keepalive PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_keepalive PRIVATE
    unity ioh_server ioh_loop ioh_conn ioh_ctx
    ioh_http1 ioh_request ioh_response
    ioh_router ioh_radix ioh_middleware
    ioh_route_group ioh_route_inspect ioh_route_meta)
add_test(NAME test_keepalive COMMAND test_keepalive)
```

**Tests:** 2 new integration tests

---

## Task 5: TLS Handshake Integration

**Goal:** When `ioh_server_set_tls()` is configured, perform TLS handshake after accept before HTTP parsing. Use existing buffer-based TLS API (ioh_tls_feed_input / ioh_tls_get_output).

**Files:**
- Modify: `src/core/ioh_server.c` — TLS handshake in recv handler
- Modify: `src/core/ioh_conn.h` — add `tls_done` flag
- Create: `tests/integration/test_tls_pipeline.c`
- Modify: `CMakeLists.txt`

**Step 1: Add TLS fields to ioh_conn_t**

```c
    bool tls_done;           /**< TLS handshake completed */
```

**Step 2: Modify accept handler — create TLS connection if TLS configured**

In `ioh_server_run_once()`, in the IOH_OP_ACCEPT handler, after `conn->fd = client_fd`:
```c
conn->fd = client_fd;

if (srv->tls_ctx != nullptr) {
    /* TLS configured — start handshake */
    conn->tls_ctx = ioh_tls_conn_create(srv->tls_ctx, client_fd);
    conn->tls_done = false;
    (void)ioh_conn_transition(conn, IOH_CONN_TLS_HANDSHAKE);
} else {
    (void)ioh_conn_transition(conn, IOH_CONN_HTTP_ACTIVE);
}
(void)arm_recv(srv, conn);
```

**Step 3: Modify IOH_OP_RECV — TLS handshake / decrypt path**

In the IOH_OP_RECV handler, add TLS processing before HTTP parsing:

```c
if (cqe->res <= 0) {
    (void)arm_close(srv, conn);
    processed++;
    continue;
}

conn->recv_len += (size_t)cqe->res;

/* ---- TLS path ---- */
if (conn->tls_ctx != nullptr) {
    ioh_tls_conn_t *tls = (ioh_tls_conn_t *)conn->tls_ctx;

    /* Feed received ciphertext to TLS */
    (void)ioh_tls_feed_input(tls, conn->recv_buf, conn->recv_len);
    conn->recv_len = 0;

    if (!conn->tls_done) {
        /* Continue handshake */
        int hs = ioh_tls_handshake(tls);
        /* Flush any TLS output (handshake messages) */
        const uint8_t *out_data = nullptr;
        size_t out_len = 0;
        if (ioh_tls_get_output(tls, &out_data, &out_len) == 0 && out_len > 0) {
            (void)arm_send(srv, conn, out_data, out_len);
            ioh_tls_consume_output(tls, out_len);
        }

        if (hs == 0) {
            /* Handshake complete */
            conn->tls_done = true;
            (void)ioh_conn_transition(conn, IOH_CONN_HTTP_ACTIVE);
            /* Re-arm recv for application data */
            if (!conn->send_active) {
                (void)arm_recv(srv, conn);
            }
        } else if (hs == -EAGAIN) {
            /* Need more data — re-arm recv */
            if (!conn->send_active) {
                (void)arm_recv(srv, conn);
            }
        } else {
            /* Handshake failed — close */
            (void)arm_close(srv, conn);
        }
        processed++;
        continue;
    }

    /* Handshake done — decrypt application data */
    uint8_t plain[8192];
    int rret = ioh_tls_read(tls, plain, sizeof(plain));
    if (rret > 0) {
        /* Copy plaintext into recv_buf for HTTP parsing */
        if ((size_t)rret <= conn->recv_buf_size) {
            memcpy(conn->recv_buf, plain, (size_t)rret);
            conn->recv_len = (size_t)rret;
        }
    } else if (rret == -EAGAIN) {
        (void)arm_recv(srv, conn);
        processed++;
        continue;
    } else {
        (void)arm_close(srv, conn);
        processed++;
        continue;
    }
}

/* ---- HTTP parsing (plaintext in recv_buf) ---- */
ioh_request_t req;
int consumed = ioh_http1_parse_request(conn->recv_buf, conn->recv_len, &req);
/* ... rest of existing parsing logic ... */
```

**Step 4: Modify arm_send for TLS connections**

When TLS is active, encrypt before sending:
```c
static int arm_send(ioh_server_t *srv, ioh_conn_t *conn,
                    const uint8_t *data, size_t len)
{
    if (conn->send_active) {
        return -EBUSY;
    }

    const uint8_t *send_data = data;
    size_t send_len = len;
    uint8_t *encrypted = nullptr;

    /* Encrypt if TLS active and handshake done */
    if (conn->tls_ctx != nullptr && conn->tls_done) {
        ioh_tls_conn_t *tls = (ioh_tls_conn_t *)conn->tls_ctx;
        int wret = ioh_tls_write(tls, data, len);
        if (wret < 0) {
            return wret;
        }

        const uint8_t *out_data = nullptr;
        size_t out_len = 0;
        if (ioh_tls_get_output(tls, &out_data, &out_len) == 0 && out_len > 0) {
            encrypted = malloc(out_len);
            if (encrypted == nullptr) {
                return -ENOMEM;
            }
            memcpy(encrypted, out_data, out_len);
            ioh_tls_consume_output(tls, out_len);
            send_data = encrypted;
            send_len = out_len;
        }
    }

    free(conn->send_buf);
    if (encrypted != nullptr) {
        conn->send_buf = encrypted;
    } else {
        conn->send_buf = malloc(send_len);
        if (conn->send_buf == nullptr) {
            return -ENOMEM;
        }
        memcpy(conn->send_buf, send_data, send_len);
    }
    conn->send_len = send_len;
    conn->send_offset = 0;
    conn->send_active = true;

    struct io_uring *ring = ioh_loop_ring(srv->loop);
    struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
    if (sqe == nullptr) {
        conn->send_active = false;
        return -ENOSPC;
    }

    io_uring_prep_send(sqe, conn->fd, conn->send_buf, send_len, MSG_NOSIGNAL);
    io_uring_sqe_set_data64(sqe, IOH_ENCODE_USERDATA(conn->id, IOH_OP_SEND));

    return 0;
}
```

**Step 5: Clean up TLS in arm_close / conn_free**

In `arm_close()`, add before close:
```c
if (conn->tls_ctx != nullptr) {
    ioh_tls_conn_destroy((ioh_tls_conn_t *)conn->tls_ctx);
    conn->tls_ctx = nullptr;
}
```

**Step 6: test_tls_pipeline.c**

```c
/**
 * @file test_tls_pipeline.c
 * @brief End-to-end TLS pipeline tests using wolfSSL client + server.
 */

#include "core/ioh_server.h"
#include "router/ioh_router.h"
#include "tls/ioh_tls.h"

#include <errno.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <wolfssl/options.h>
#include <wolfssl/ssl.h>

#include <unity.h>

#ifndef TEST_CERTS_DIR
#define TEST_CERTS_DIR "/opt/projects/repositories/iohttp/tests/certs"
#endif

void setUp(void) {}
void tearDown(void) {}

static uint16_t get_bound_port(int fd)
{
    struct sockaddr_in addr;
    socklen_t len = sizeof(addr);
    getsockname(fd, (struct sockaddr *)&addr, &len);
    return ntohs(addr.sin_port);
}

static int hello_handler(ioh_ctx_t *c)
{
    return ioh_ctx_text(c, 200, "TLS Hello");
}

void test_tls_pipeline_get(void)
{
    /* Check certs exist */
    char cert_path[256];
    char key_path[256];
    snprintf(cert_path, sizeof(cert_path), "%s/server-cert.pem", TEST_CERTS_DIR);
    snprintf(key_path, sizeof(key_path), "%s/server-key.pem", TEST_CERTS_DIR);

    if (access(cert_path, R_OK) != 0 || access(key_path, R_OK) != 0) {
        TEST_IGNORE_MESSAGE("Test certs not available — skipping TLS pipeline test");
    }

    /* Server TLS context */
    ioh_tls_config_t tls_cfg;
    ioh_tls_config_init(&tls_cfg);
    tls_cfg.cert_file = cert_path;
    tls_cfg.key_file = key_path;

    ioh_tls_ctx_t *tls_ctx = ioh_tls_ctx_create(&tls_cfg);
    TEST_ASSERT_NOT_NULL(tls_ctx);

    /* Server */
    ioh_server_config_t cfg;
    ioh_server_config_init(&cfg);
    cfg.listen_port = 0;
    cfg.max_connections = 16;
    cfg.queue_depth = 32;

    ioh_server_t *srv = ioh_server_create(&cfg);
    TEST_ASSERT_NOT_NULL(srv);

    ioh_router_t *router = ioh_router_create();
    ioh_router_get(router, "/hello", hello_handler);
    ioh_server_set_router(srv, router);
    ioh_server_set_tls(srv, tls_ctx);

    int listen_fd = ioh_server_listen(srv);
    uint16_t port = get_bound_port(listen_fd);

    /* wolfSSL client */
    WOLFSSL_CTX *client_ctx = wolfSSL_CTX_new(wolfTLSv1_3_client_method());
    TEST_ASSERT_NOT_NULL(client_ctx);
    wolfSSL_CTX_set_verify(client_ctx, WOLFSSL_VERIFY_NONE, nullptr);

    int client_fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(port),
        .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
    };
    TEST_ASSERT_EQUAL_INT(0, connect(client_fd, (struct sockaddr *)&addr, sizeof(addr)));

    WOLFSSL *ssl = wolfSSL_new(client_ctx);
    wolfSSL_set_fd(ssl, client_fd);

    /* Process TLS handshake: client connect + server event loop iterations */
    int connected = 0;
    for (int i = 0; i < 20 && !connected; i++) {
        int ret = wolfSSL_connect(ssl);
        if (ret == WOLFSSL_SUCCESS) {
            connected = 1;
        }
        ioh_server_run_once(srv, 50);
    }

    if (connected) {
        /* Send HTTP request over TLS */
        const char *req = "GET /hello HTTP/1.1\r\n"
                          "Host: localhost\r\n"
                          "Connection: close\r\n"
                          "\r\n";
        wolfSSL_write(ssl, req, (int)strlen(req));

        for (int i = 0; i < 10; i++) {
            ioh_server_run_once(srv, 50);
        }

        /* Read response */
        char resp[4096];
        int n = wolfSSL_read(ssl, resp, sizeof(resp) - 1);
        if (n > 0) {
            resp[n] = '\0';
            TEST_ASSERT_NOT_NULL(strstr(resp, "200"));
            TEST_ASSERT_NOT_NULL(strstr(resp, "TLS Hello"));
        }
    } else {
        TEST_IGNORE_MESSAGE("TLS handshake did not complete — skipping");
    }

    wolfSSL_free(ssl);
    wolfSSL_CTX_free(client_ctx);
    close(client_fd);
    ioh_server_destroy(srv);
    ioh_router_destroy(router);
    ioh_tls_ctx_destroy(tls_ctx);
}

int main(void)
{
    wolfSSL_Init();
    UNITY_BEGIN();
    RUN_TEST(test_tls_pipeline_get);
    int result = UNITY_END();
    wolfSSL_Cleanup();
    return result;
}
```

**CMake:**
```cmake
if(WOLFSSL_FOUND)
    add_executable(test_tls_pipeline tests/integration/test_tls_pipeline.c)
    target_include_directories(test_tls_pipeline PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(test_tls_pipeline PRIVATE
        unity ioh_server ioh_loop ioh_conn ioh_ctx ioh_tls
        ioh_http1 ioh_request ioh_response
        ioh_router ioh_radix ioh_middleware
        ioh_route_group ioh_route_inspect ioh_route_meta
        ${WOLFSSL_LIBRARIES})
    target_include_directories(test_tls_pipeline PRIVATE ${WOLFSSL_INCLUDE_DIRS})
    target_compile_definitions(test_tls_pipeline PRIVATE
        TEST_CERTS_DIR="${CMAKE_SOURCE_DIR}/tests/certs")
    add_test(NAME test_tls_pipeline COMMAND test_tls_pipeline)
endif()
```

**Tests:** 1 TLS pipeline test (gracefully skips if certs missing)

---

## Task 6: Graceful Shutdown with Connection Drain

**Goal:** Implement `IOH_SHUTDOWN_DRAIN` properly: stop accepting, wait for active connections to finish, close all, then return.

**Files:**
- Modify: `src/core/ioh_server.c` — rewrite `ioh_server_shutdown()`
- Modify: `tests/unit/test_ioh_server.c`
- Create: `tests/integration/test_shutdown.c`
- Modify: `CMakeLists.txt`

**Step 1: Rewrite ioh_server_shutdown**

```c
int ioh_server_shutdown(ioh_server_t *srv, ioh_shutdown_mode_t mode)
{
    if (srv == nullptr) {
        return -EINVAL;
    }

    srv->stopped = true;

    /* Cancel multishot accept */
    if (srv->accepting) {
        struct io_uring *ring = ioh_loop_ring(srv->loop);
        struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
        if (sqe != nullptr) {
            io_uring_prep_cancel64(sqe, IOH_ENCODE_USERDATA(0, IOH_OP_ACCEPT), 0);
            io_uring_sqe_set_data64(sqe, IOH_ENCODE_USERDATA(0, IOH_OP_CANCEL));
        }
        srv->accepting = false;
    }

    /* Close listen socket */
    if (srv->listen_fd >= 0) {
        close(srv->listen_fd);
        srv->listen_fd = -1;
        srv->listening = false;
    }

    if (mode == IOH_SHUTDOWN_IMMEDIATE) {
        /* Close all active connections immediately */
        uint32_t cap = ioh_conn_pool_capacity(srv->pool);
        for (uint32_t i = 0; i < cap; i++) {
            ioh_conn_t *conn = ioh_conn_pool_get(srv->pool, i);
            if (conn != nullptr && conn->state != IOH_CONN_FREE) {
                if (conn->tls_ctx != nullptr) {
                    ioh_tls_conn_destroy((ioh_tls_conn_t *)conn->tls_ctx);
                    conn->tls_ctx = nullptr;
                }
                if (conn->fd >= 0) {
                    close(conn->fd);
                    conn->fd = -1;
                }
                ioh_conn_free(srv->pool, conn);
            }
        }
    } else if (mode == IOH_SHUTDOWN_DRAIN) {
        /* Transition all active connections to DRAINING */
        uint32_t cap = ioh_conn_pool_capacity(srv->pool);
        for (uint32_t i = 0; i < cap; i++) {
            ioh_conn_t *conn = ioh_conn_pool_get(srv->pool, i);
            if (conn != nullptr && conn->state == IOH_CONN_HTTP_ACTIVE) {
                (void)ioh_conn_transition(conn, IOH_CONN_DRAINING);
                conn->keep_alive = false; /* force close after current response */
            }
        }

        /* Run event loop until all connections close or timeout */
        uint32_t drain_timeout_ms = srv->config.keepalive_timeout_ms;
        uint32_t elapsed = 0;
        constexpr uint32_t DRAIN_POLL_MS = 50;

        while (ioh_conn_pool_active(srv->pool) > 0 && elapsed < drain_timeout_ms) {
            ioh_server_run_once(srv, DRAIN_POLL_MS);
            elapsed += DRAIN_POLL_MS;
        }

        /* Force-close remaining */
        for (uint32_t i = 0; i < cap; i++) {
            ioh_conn_t *conn = ioh_conn_pool_get(srv->pool, i);
            if (conn != nullptr && conn->state != IOH_CONN_FREE) {
                if (conn->tls_ctx != nullptr) {
                    ioh_tls_conn_destroy((ioh_tls_conn_t *)conn->tls_ctx);
                    conn->tls_ctx = nullptr;
                }
                if (conn->fd >= 0) {
                    close(conn->fd);
                    conn->fd = -1;
                }
                ioh_conn_free(srv->pool, conn);
            }
        }
    }

    ioh_loop_stop(srv->loop);
    return 0;
}
```

**Step 2: test_shutdown.c**

```c
/**
 * @file test_shutdown.c
 * @brief Integration tests for graceful shutdown.
 */

#include "core/ioh_server.h"
#include "router/ioh_router.h"

#include <netinet/in.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <unity.h>

void setUp(void) {}
void tearDown(void) {}

static uint16_t get_bound_port(int fd)
{
    struct sockaddr_in addr;
    socklen_t len = sizeof(addr);
    getsockname(fd, (struct sockaddr *)&addr, &len);
    return ntohs(addr.sin_port);
}

static int handler(ioh_ctx_t *c)
{
    return ioh_ctx_text(c, 200, "OK");
}

void test_shutdown_immediate_closes_all(void)
{
    ioh_server_config_t cfg;
    ioh_server_config_init(&cfg);
    cfg.listen_port = 0;
    cfg.max_connections = 16;
    cfg.queue_depth = 32;

    ioh_server_t *srv = ioh_server_create(&cfg);
    ioh_server_set_on_request(srv, (ioh_server_on_request_fn)(void *)handler, nullptr);

    int listen_fd = ioh_server_listen(srv);
    uint16_t port = get_bound_port(listen_fd);

    /* Open connections */
    int c1 = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons(port),
                               .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    connect(c1, (struct sockaddr *)&addr, sizeof(addr));

    ioh_server_run_once(srv, 100);
    TEST_ASSERT_EQUAL_UINT32(1, ioh_conn_pool_active(ioh_server_pool(srv)));

    /* Immediate shutdown */
    TEST_ASSERT_EQUAL_INT(0, ioh_server_shutdown(srv, IOH_SHUTDOWN_IMMEDIATE));
    TEST_ASSERT_EQUAL_UINT32(0, ioh_conn_pool_active(ioh_server_pool(srv)));

    close(c1);
    ioh_server_destroy(srv);
}

void test_shutdown_drain(void)
{
    ioh_server_config_t cfg;
    ioh_server_config_init(&cfg);
    cfg.listen_port = 0;
    cfg.max_connections = 16;
    cfg.queue_depth = 32;
    cfg.keepalive_timeout_ms = 500; /* short drain timeout */

    ioh_server_t *srv = ioh_server_create(&cfg);
    ioh_server_set_on_request(srv, (ioh_server_on_request_fn)(void *)handler, nullptr);

    int listen_fd = ioh_server_listen(srv);
    uint16_t port = get_bound_port(listen_fd);

    int c1 = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons(port),
                               .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    connect(c1, (struct sockaddr *)&addr, sizeof(addr));

    ioh_server_run_once(srv, 100);

    /* Drain shutdown — should close idle connections */
    TEST_ASSERT_EQUAL_INT(0, ioh_server_shutdown(srv, IOH_SHUTDOWN_DRAIN));
    TEST_ASSERT_EQUAL_UINT32(0, ioh_conn_pool_active(ioh_server_pool(srv)));

    close(c1);
    ioh_server_destroy(srv);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_shutdown_immediate_closes_all);
    RUN_TEST(test_shutdown_drain);
    return UNITY_END();
}
```

**CMake:**
```cmake
add_executable(test_shutdown tests/integration/test_shutdown.c)
target_include_directories(test_shutdown PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_shutdown PRIVATE
    unity ioh_server ioh_loop ioh_conn ioh_ctx
    ioh_http1 ioh_request ioh_response
    ioh_router ioh_radix ioh_middleware
    ioh_route_group ioh_route_inspect ioh_route_meta)
add_test(NAME test_shutdown COMMAND test_shutdown)
```

**Tests:** 2 new

---

## Task 7: ioh_server_run — Blocking Event Loop

**Goal:** Implement `ioh_server_run()` that blocks until `ioh_server_stop()` is called. This is the public API users will call.

**Files:**
- Modify: `src/core/ioh_server.c`
- Modify: `tests/unit/test_ioh_server.c`

**Step 1: Implement ioh_server_run**

```c
int ioh_server_run(ioh_server_t *srv)
{
    if (srv == nullptr) {
        return -EINVAL;
    }

    if (!srv->listening) {
        int ret = ioh_server_listen(srv);
        if (ret < 0) {
            return ret;
        }
    }

    while (!srv->stopped) {
        int ret = ioh_server_run_once(srv, 1000);
        if (ret < 0 && ret != -ETIME && ret != -EINTR) {
            return ret;
        }
    }

    return 0;
}
```

**Step 2: Add test**

```c
void test_server_run_stop(void)
{
    ioh_server_config_t cfg;
    ioh_server_config_init(&cfg);
    cfg.listen_port = 0;
    cfg.max_connections = 16;
    cfg.queue_depth = 32;

    ioh_server_t *srv = ioh_server_create(&cfg);
    TEST_ASSERT_NOT_NULL(srv);

    /* Stop immediately so ioh_server_run returns */
    ioh_server_stop(srv);
    int ret = ioh_server_run(srv);
    TEST_ASSERT_EQUAL_INT(0, ret);

    ioh_server_destroy(srv);
}
```

**Tests:** 1 new

---

## Task 8: Middleware Integration Test

**Goal:** Verify that middleware chain (CORS, auth, rate limiting) works end-to-end through the TCP pipeline.

**Files:**
- Create: `tests/integration/test_middleware_pipeline.c`
- Modify: `CMakeLists.txt`

**Step 1: test_middleware_pipeline.c**

```c
/**
 * @file test_middleware_pipeline.c
 * @brief End-to-end tests verifying middleware executes in the TCP pipeline.
 */

#include "core/ioh_server.h"
#include "middleware/ioh_cors.h"
#include "middleware/ioh_middleware.h"
#include "router/ioh_router.h"

#include <netinet/in.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <unity.h>

void setUp(void) {}
void tearDown(void) {}

static uint16_t get_bound_port(int fd)
{
    struct sockaddr_in addr;
    socklen_t len = sizeof(addr);
    getsockname(fd, (struct sockaddr *)&addr, &len);
    return ntohs(addr.sin_port);
}

static int connect_to(uint16_t port)
{
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons(port),
                               .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static int api_handler(ioh_ctx_t *c) { return ioh_ctx_json(c, 200, "{\"ok\":true}"); }

/* Custom middleware that adds X-Custom header */
static int custom_mw(ioh_ctx_t *c, ioh_handler_fn next)
{
    ioh_ctx_set_header(c, "X-Custom", "applied");
    return next(c);
}

void test_middleware_pipeline_cors(void)
{
    ioh_server_config_t cfg;
    ioh_server_config_init(&cfg);
    cfg.listen_port = 0;
    cfg.max_connections = 16;
    cfg.queue_depth = 32;

    ioh_server_t *srv = ioh_server_create(&cfg);
    ioh_router_t *router = ioh_router_create();

    /* Add CORS middleware */
    ioh_cors_config_t cors_cfg;
    ioh_cors_config_init(&cors_cfg);
    cors_cfg.allow_origins = (const char *[]){"*"};
    cors_cfg.allow_origins_count = 1;
    ioh_middleware_fn cors_mw = ioh_cors_create(&cors_cfg);
    ioh_router_use(router, cors_mw);

    ioh_router_get(router, "/api", api_handler);
    ioh_server_set_router(srv, router);

    int listen_fd = ioh_server_listen(srv);
    uint16_t port = get_bound_port(listen_fd);

    int client = connect_to(port);
    TEST_ASSERT_TRUE(client >= 0);

    const char *req = "GET /api HTTP/1.1\r\n"
                      "Host: localhost\r\n"
                      "Origin: http://example.com\r\n"
                      "Connection: close\r\n"
                      "\r\n";
    send(client, req, strlen(req), MSG_NOSIGNAL);

    for (int i = 0; i < 5; i++) {
        ioh_server_run_once(srv, 100);
    }

    char resp[4096];
    ssize_t n = recv(client, resp, sizeof(resp) - 1, 0);
    TEST_ASSERT_GREATER_THAN(0, n);
    resp[n] = '\0';

    TEST_ASSERT_NOT_NULL(strstr(resp, "200"));
    TEST_ASSERT_NOT_NULL(strstr(resp, "{\"ok\":true}"));
    /* CORS header should be present */
    TEST_ASSERT_NOT_NULL(strstr(resp, "Access-Control-Allow-Origin"));

    close(client);
    ioh_server_destroy(srv);
    ioh_router_destroy(router);
}

void test_middleware_pipeline_custom(void)
{
    ioh_server_config_t cfg;
    ioh_server_config_init(&cfg);
    cfg.listen_port = 0;
    cfg.max_connections = 16;
    cfg.queue_depth = 32;

    ioh_server_t *srv = ioh_server_create(&cfg);
    ioh_router_t *router = ioh_router_create();

    ioh_router_use(router, custom_mw);
    ioh_router_get(router, "/test", api_handler);
    ioh_server_set_router(srv, router);

    int listen_fd = ioh_server_listen(srv);
    uint16_t port = get_bound_port(listen_fd);

    int client = connect_to(port);
    TEST_ASSERT_TRUE(client >= 0);

    const char *req = "GET /test HTTP/1.1\r\n"
                      "Host: localhost\r\n"
                      "Connection: close\r\n"
                      "\r\n";
    send(client, req, strlen(req), MSG_NOSIGNAL);

    for (int i = 0; i < 5; i++) {
        ioh_server_run_once(srv, 100);
    }

    char resp[4096];
    ssize_t n = recv(client, resp, sizeof(resp) - 1, 0);
    TEST_ASSERT_GREATER_THAN(0, n);
    resp[n] = '\0';

    TEST_ASSERT_NOT_NULL(strstr(resp, "200"));
    TEST_ASSERT_NOT_NULL(strstr(resp, "X-Custom: applied"));

    close(client);
    ioh_server_destroy(srv);
    ioh_router_destroy(router);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_middleware_pipeline_cors);
    RUN_TEST(test_middleware_pipeline_custom);
    return UNITY_END();
}
```

**CMake:**
```cmake
add_executable(test_middleware_pipeline tests/integration/test_middleware_pipeline.c)
target_include_directories(test_middleware_pipeline PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_middleware_pipeline PRIVATE
    unity ioh_server ioh_loop ioh_conn ioh_ctx
    ioh_http1 ioh_request ioh_response
    ioh_router ioh_radix ioh_middleware ioh_cors
    ioh_route_group ioh_route_inspect ioh_route_meta)
add_test(NAME test_middleware_pipeline COMMAND test_middleware_pipeline)
```

**Tests:** 2 new

---

## Task 9: Quality Pipeline and Cleanup

**Goal:** Run full quality pipeline, fix any issues, commit.

**Files:**
- Possibly modify: any file with format/lint/PVS issues

**Step 1: Run clang-format**
```bash
cmake --build --preset clang-debug --target format
```

**Step 2: Run full quality pipeline**
```bash
podman run --rm --security-opt seccomp=unconfined \
  -v /opt/projects/repositories/iohttp:/workspace:Z \
  localhost/iohttp-dev:latest bash -c "cd /workspace && ./scripts/quality.sh"
```

**Step 3: Fix any findings**

Apply suppressions for PVS-Studio false positives using `//-VXXX` inline comments. Fix any real issues.

**Step 4: Final commit**

---

## Summary

| Task | Description | New Tests |
|------|-------------|-----------|
| 1 | Extend ioh_server with router/TLS/callback | 3 |
| 2 | Per-conn recv buffer, arm recv after accept | 2 |
| 3 | CQE handlers: RECV→parse→dispatch→SEND→CLOSE | 6 |
| 4 | Keep-alive connection reuse | 2 |
| 5 | TLS handshake integration | 1 |
| 6 | Graceful shutdown with drain | 2 |
| 7 | ioh_server_run blocking loop | 1 |
| 8 | Middleware pipeline integration test | 2 |
| 9 | Quality pipeline cleanup | 0 |
| **Total** | | **19** |

**Key constraints:**
- HTTP/1.1 only in this sprint (HTTP/2 ALPN dispatch is a future sprint)
- Send serialization: one IOH_OP_SEND per connection at a time
- fd close ordering: close only after all CQEs processed
- No blocking in CQE handlers
- Tests use kernel-assigned ports (port 0) to avoid conflicts

# Sprint 12: Production Hardening

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development to implement this plan task-by-task.

**Goal:** Make the HTTP/1.1 TCP pipeline production-viable by adding linked timeouts, request limit enforcement, signal handling, structured logging, request ID middleware, and PROXY protocol integration.

**Architecture:** Extend `ioh_server.c` pipeline with `IORING_OP_LINK_TIMEOUT` linked to every recv operation, timeout phase tracking in `ioh_conn_t`, Content-Length/header-size validation in the parse path, signalfd-based SIGTERM/SIGQUIT handling through the io_uring ring, a minimal structured logging module (`ioh_log`), request ID generation in `dispatch_request()`, and PROXY protocol v1/v2 decoding wired into the accept→recv flow.

**Tech Stack:** C23, liburing (linked timeouts, signalfd), picohttpparser, wolfSSL, Unity tests, Linux kernel 6.7+.

**Skills (MANDATORY):**
- `iohttp-architecture` — naming, directory layout, state machine
- `io-uring-patterns` — linked timeouts, signalfd, CQE error handling
- `rfc-reference` — HTTP status codes (413, 431, 408)

**References:**
- PROXY Protocol spec: https://www.haproxy.org/download/3.4/doc/proxy-protocol.txt
- RFC 9110 §15.5.8 (408 Request Timeout), §15.5.14 (413 Content Too Large), §15.5.32 (431 Request Header Fields Too Large)
- RFC 9112 §6 (Message Body), §2.3 (Request Line limits)

**Build/test:**
```bash
cmake --preset clang-debug
cmake --build --preset clang-debug
ctest --preset clang-debug
```

**Existing files used (read before implementing):**
- `src/core/ioh_server.c` — main pipeline (modify heavily)
- `src/core/ioh_server.h` — server config and API (modify)
- `src/core/ioh_conn.h` — connection struct (modify)
- `src/core/ioh_conn.c` — connection state machine (modify)
- `src/core/ioh_loop.h` — op types, user_data encoding (read-only)
- `src/core/ioh_ctx.h` — per-request context (modify for request ID)
- `src/http/ioh_http1.h` — parse limits (read-only)
- `src/http/ioh_proxy_proto.h` — decoder API (read-only)
- `src/http/ioh_request.h` — request struct (read-only)
- `src/http/ioh_response.h` — response builder (read-only)
- `CMakeLists.txt` — test registration (modify)

---

## Task 1: Linked Timeouts for Recv Operations

**Goal:** Link `IORING_OP_LINK_TIMEOUT` to every `arm_recv()` call. Three phases: HEADER (waiting for first complete request), BODY (waiting for Content-Length body), KEEPALIVE (idle between requests). On timeout → 408 Request Timeout or close.

**Files:**
- Modify: `src/core/ioh_conn.h` — add `timeout_phase` field
- Modify: `src/core/ioh_server.c` — modify `arm_recv()`, add `IOH_OP_TIMEOUT` CQE handler
- Modify: `tests/unit/test_ioh_server.c` — timeout unit tests
- Create: `tests/integration/test_timeout.c` — timeout integration tests
- Modify: `CMakeLists.txt` — register `test_timeout`

**Step 1: Add timeout phase to ioh_conn_t**

In `src/core/ioh_conn.h`, add a timeout phase enum and field to `ioh_conn_t`:

```c
/* After ioh_conn_state_t enum, before ioh_conn_t struct */
typedef enum : uint8_t {
    IOH_TIMEOUT_NONE = 0,
    IOH_TIMEOUT_HEADER,    /**< waiting for complete request headers */
    IOH_TIMEOUT_BODY,      /**< waiting for Content-Length body */
    IOH_TIMEOUT_KEEPALIVE, /**< idle between pipelined requests */
} ioh_timeout_phase_t;
```

Add to `ioh_conn_t` struct (after `tls_done` field):

```c
    ioh_timeout_phase_t timeout_phase; /**< current recv timeout phase */
```

**Step 2: Modify arm_recv() to link a timeout SQE**

In `src/core/ioh_server.c`, modify `arm_recv()` to accept a timeout_ms parameter and link a timeout:

```c
static int arm_recv(ioh_server_t *srv, ioh_conn_t *conn)
{
    struct io_uring *ring = ioh_loop_ring(srv->loop);
    struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
    if (sqe == nullptr) {
        return -ENOSPC;
    }

    io_uring_prep_recv(sqe, conn->fd, conn->recv_buf + conn->recv_len,
                       conn->recv_buf_size - conn->recv_len, 0);
    io_uring_sqe_set_data64(sqe, IOH_ENCODE_USERDATA(conn->id, IOH_OP_RECV));

    /* Determine timeout based on phase */
    uint32_t timeout_ms = 0;
    switch (conn->timeout_phase) {
    case IOH_TIMEOUT_HEADER:
        timeout_ms = srv->config.header_timeout_ms;
        break;
    case IOH_TIMEOUT_BODY:
        timeout_ms = srv->config.body_timeout_ms;
        break;
    case IOH_TIMEOUT_KEEPALIVE:
        timeout_ms = srv->config.keepalive_timeout_ms;
        break;
    case IOH_TIMEOUT_NONE:
        break;
    }

    if (timeout_ms > 0) {
        sqe->flags |= IOSQE_IO_LINK;

        struct io_uring_sqe *tsqe = io_uring_get_sqe(ring);
        if (tsqe == nullptr) {
            /* Undo the link flag — proceed without timeout */
            sqe->flags &= ~((uint8_t)IOSQE_IO_LINK);
            return 0;
        }

        struct __kernel_timespec ts = {
            .tv_sec = (long long)(timeout_ms / 1000),
            .tv_nsec = (long long)(timeout_ms % 1000) * 1000000LL,
        };
        io_uring_prep_link_timeout(tsqe, &ts, 0);
        io_uring_sqe_set_data64(tsqe, IOH_ENCODE_USERDATA(conn->id, IOH_OP_TIMEOUT));
    }

    return 0;
}
```

**Step 3: Set timeout phase on connection accept**

In `ioh_server_run_once()` accept handler (after `ioh_conn_transition`), set initial phase:

```c
conn->timeout_phase = IOH_TIMEOUT_HEADER;
```

**Step 4: Set timeout phase transitions in recv handler**

After successful header parse when waiting for body (line ~598):
```c
conn->timeout_phase = IOH_TIMEOUT_BODY;
```

After successful dispatch + send completion with keep_alive (send CQE handler, line ~669):
```c
conn->timeout_phase = IOH_TIMEOUT_KEEPALIVE;
conn->recv_len = 0;  /* reset recv buffer for next request */
```

After TLS handshake complete:
```c
conn->timeout_phase = IOH_TIMEOUT_HEADER;
```

**Step 5: Add IOH_OP_TIMEOUT CQE handler**

In `ioh_server_run_once()`, add handler after the `IOH_OP_CLOSE` block:

```c
} else if (op == IOH_OP_TIMEOUT) {
    uint32_t conn_id = (uint32_t)IOH_DECODE_ID(ud);
    ioh_conn_t *conn = find_conn_by_id(srv, conn_id);

    if (conn == nullptr) {
        processed++;
        continue;
    }

    if (cqe->res == -ECANCELED) {
        /* Timeout was cancelled because recv completed first — normal */
        processed++;
        continue;
    }

    /* Timeout fired (-ETIME) — close the connection */
    (void)arm_close(srv, conn);
}
```

**Step 6: Handle -ECANCELED on recv from linked timeout**

In the `IOH_OP_RECV` handler, the existing `cqe->res <= 0` check already handles this:
- `cqe->res == -ECANCELED` means the linked timeout expired and cancelled recv → `arm_close` is correct
- `cqe->res == -ETIME` should not appear on recv (appears on timeout SQE)

No change needed — the existing `arm_close` on `cqe->res <= 0` already covers this.

**Step 7: Write unit tests**

Add to `tests/unit/test_ioh_server.c`:

```c
void test_server_conn_timeout_phase_default(void)
{
    ioh_server_config_t cfg = make_config(19030, 16);
    ioh_server_t *srv = ioh_server_create(&cfg);
    TEST_ASSERT_NOT_NULL(srv);

    int fd = ioh_server_listen(srv);
    TEST_ASSERT_GREATER_THAN(0, fd);
    uint16_t port = get_bound_port(fd);

    int client_fd = connect_client(port);
    TEST_ASSERT_TRUE(client_fd >= 0);

    int ret = ioh_server_run_once(srv, 1000);
    TEST_ASSERT_GREATER_THAN(0, ret);

    /* Verify connection was allocated with HEADER timeout phase */
    ioh_conn_t *conn = ioh_conn_pool_get(ioh_server_pool(srv), 0);
    TEST_ASSERT_NOT_NULL(conn);
    TEST_ASSERT_EQUAL_UINT8(IOH_TIMEOUT_HEADER, conn->timeout_phase);

    close(client_fd);
    ioh_server_destroy(srv);
}
```

Register in `main()`:
```c
RUN_TEST(test_server_conn_timeout_phase_default);
```

**Step 8: Write integration test for header timeout**

Create `tests/integration/test_timeout.c`:

```c
/**
 * @file test_timeout.c
 * @brief Integration tests for recv linked timeouts.
 */

#include "core/ioh_server.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <unity.h>

void setUp(void) {}
void tearDown(void) {}

static ioh_server_config_t make_config(uint16_t port)
{
    ioh_server_config_t cfg;
    ioh_server_config_init(&cfg);
    cfg.listen_addr = "127.0.0.1";
    cfg.listen_port = port;
    cfg.max_connections = 16;
    cfg.header_timeout_ms = 500;     /* 500ms for fast tests */
    cfg.keepalive_timeout_ms = 500;
    return cfg;
}

static uint16_t get_bound_port(int listen_fd)
{
    struct sockaddr_in addr;
    socklen_t len = sizeof(addr);
    getsockname(listen_fd, (struct sockaddr *)&addr, &len);
    return ntohs(addr.sin_port);
}

static int connect_client(uint16_t port)
{
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(port),
    };
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    connect(fd, (struct sockaddr *)&addr, sizeof(addr));
    return fd;
}

void test_header_timeout_closes_idle_connection(void)
{
    ioh_server_config_t cfg = make_config(19100);
    ioh_server_t *srv = ioh_server_create(&cfg);
    TEST_ASSERT_NOT_NULL(srv);

    int fd = ioh_server_listen(srv);
    TEST_ASSERT_GREATER_THAN(0, fd);
    uint16_t port = get_bound_port(fd);

    /* Connect but send nothing — should timeout after 500ms */
    int client_fd = connect_client(port);
    TEST_ASSERT_TRUE(client_fd >= 0);

    /* Process accept */
    int ret = ioh_server_run_once(srv, 1000);
    TEST_ASSERT_GREATER_THAN(0, ret);
    TEST_ASSERT_EQUAL_UINT32(1, ioh_conn_pool_active(ioh_server_pool(srv)));

    /* Run loop until timeout fires (~500ms + some margin) */
    for (int i = 0; i < 20; i++) {
        (void)ioh_server_run_once(srv, 100);
        if (ioh_conn_pool_active(ioh_server_pool(srv)) == 0) {
            break;
        }
    }

    /* Connection should have been closed by timeout */
    TEST_ASSERT_EQUAL_UINT32(0, ioh_conn_pool_active(ioh_server_pool(srv)));

    close(client_fd);
    ioh_server_destroy(srv);
}

void test_keepalive_timeout_closes_after_response(void)
{
    ioh_server_config_t cfg = make_config(19101);
    ioh_server_t *srv = ioh_server_create(&cfg);
    TEST_ASSERT_NOT_NULL(srv);

    /* Set up a simple on_request handler */
    (void)ioh_server_set_on_request(srv, nullptr, nullptr);

    int fd = ioh_server_listen(srv);
    TEST_ASSERT_GREATER_THAN(0, fd);
    uint16_t port = get_bound_port(fd);

    int client_fd = connect_client(port);
    TEST_ASSERT_TRUE(client_fd >= 0);

    /* Send a complete GET request with keep-alive */
    const char *req = "GET / HTTP/1.1\r\nHost: localhost\r\nConnection: keep-alive\r\n\r\n";
    send(client_fd, req, strlen(req), 0);

    /* Process accept + recv + response */
    for (int i = 0; i < 5; i++) {
        (void)ioh_server_run_once(srv, 200);
    }

    /* Read server response */
    char buf[4096];
    recv(client_fd, buf, sizeof(buf), 0);

    /* Now idle — keepalive timeout should fire after 500ms */
    for (int i = 0; i < 20; i++) {
        (void)ioh_server_run_once(srv, 100);
        if (ioh_conn_pool_active(ioh_server_pool(srv)) == 0) {
            break;
        }
    }

    TEST_ASSERT_EQUAL_UINT32(0, ioh_conn_pool_active(ioh_server_pool(srv)));

    close(client_fd);
    ioh_server_destroy(srv);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_header_timeout_closes_idle_connection);
    RUN_TEST(test_keepalive_timeout_closes_after_response);
    return UNITY_END();
}
```

**Step 9: Register test in CMakeLists.txt**

Add after the `test_shutdown` block:

```cmake
    add_executable(test_timeout tests/integration/test_timeout.c)
    target_include_directories(test_timeout PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(test_timeout PRIVATE
        unity ioh_server ioh_loop ioh_conn ioh_ctx
        ioh_http1 ioh_request ioh_response
        ioh_router ioh_middleware ioh_radix
        ioh_route_group ioh_route_inspect ioh_route_meta
    )
    target_compile_options(test_timeout PRIVATE
        -Wno-missing-prototypes -Wno-missing-declarations
    )
    add_test(NAME test_timeout COMMAND test_timeout)
```

**Step 10: Build, test, commit**

```bash
cmake --preset clang-debug && cmake --build --preset clang-debug && ctest --preset clang-debug --output-on-failure
```

```bash
git add src/core/ioh_conn.h src/core/ioh_server.c tests/unit/test_ioh_server.c tests/integration/test_timeout.c CMakeLists.txt
git commit -m "feat(core): add linked timeouts for recv operations (header/body/keepalive)"
```

---

## Task 2: Request Limit Enforcement

**Goal:** Validate Content-Length against `max_body_size` and accumulated header bytes against `max_header_size` in the recv→parse path. Return 413 (Content Too Large) or 431 (Request Header Fields Too Large) and close.

**Files:**
- Modify: `src/core/ioh_server.c` — add limit checks in recv handler
- Create: `tests/integration/test_limits.c` — limit enforcement tests
- Modify: `CMakeLists.txt` — register `test_limits`

**Step 1: Add header size check before parse**

In `ioh_server_run_once()` recv handler, before `ioh_http1_parse_request()` (line ~591), add:

```c
/* Reject oversized headers (before parse — recv_len is header accumulation) */
if (conn->recv_len > srv->config.max_header_size) {
    ioh_response_t err_resp;
    (void)ioh_response_init(&err_resp);
    err_resp.status = 431;
    (void)ioh_response_set_body(&err_resp, (const uint8_t *)"Request Header Fields Too Large", 31);
    uint8_t resp_buf[512];
    int resp_len = ioh_http1_serialize_response(&err_resp, resp_buf, sizeof(resp_buf));
    if (resp_len > 0) {
        (void)arm_send(srv, conn, resp_buf, (size_t)resp_len);
    }
    ioh_response_destroy(&err_resp);
    conn->keep_alive = false;
    processed++;
    continue;
}
```

**Step 2: Add Content-Length validation after parse**

After `ioh_http1_parse_request()` returns `consumed > 0`, before body wait (line ~598), add:

```c
/* Reject oversized body */
if (req.content_length > srv->config.max_body_size) {
    ioh_response_t err_resp;
    (void)ioh_response_init(&err_resp);
    err_resp.status = 413;
    (void)ioh_response_set_body(&err_resp, (const uint8_t *)"Content Too Large", 17);
    uint8_t resp_buf[512];
    int resp_len = ioh_http1_serialize_response(&err_resp, resp_buf, sizeof(resp_buf));
    if (resp_len > 0) {
        (void)arm_send(srv, conn, resp_buf, (size_t)resp_len);
    }
    ioh_response_destroy(&err_resp);
    conn->keep_alive = false;
    processed++;
    continue;
}
```

**Step 3: Extract error response helper**

The pattern above repeats — extract a `send_error_response()` helper:

```c
static void send_error_response(ioh_server_t *srv, ioh_conn_t *conn, uint16_t status,
                                const char *msg)
{
    ioh_response_t err_resp;
    (void)ioh_response_init(&err_resp);
    err_resp.status = status;
    if (msg != nullptr) {
        (void)ioh_response_set_body(&err_resp, (const uint8_t *)msg, strlen(msg));
    }
    uint8_t resp_buf[512];
    int resp_len = ioh_http1_serialize_response(&err_resp, resp_buf, sizeof(resp_buf));
    if (resp_len > 0) {
        (void)arm_send(srv, conn, resp_buf, (size_t)resp_len);
    }
    ioh_response_destroy(&err_resp);
    conn->keep_alive = false;
}
```

Then refactor the 400 Bad Request response (line ~621-631) and the new 431/413 responses to use this helper.

**Step 4: Write integration tests**

Create `tests/integration/test_limits.c`:

```c
/**
 * @file test_limits.c
 * @brief Integration tests for request limit enforcement.
 */

#include "core/ioh_server.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <unity.h>

void setUp(void) {}
void tearDown(void) {}

static ioh_server_config_t make_config(uint16_t port)
{
    ioh_server_config_t cfg;
    ioh_server_config_init(&cfg);
    cfg.listen_addr = "127.0.0.1";
    cfg.listen_port = port;
    cfg.max_connections = 16;
    cfg.max_header_size = 256;   /* tiny for testing */
    cfg.max_body_size = 64;      /* tiny for testing */
    cfg.header_timeout_ms = 5000;
    return cfg;
}

static uint16_t get_bound_port(int listen_fd)
{
    struct sockaddr_in addr;
    socklen_t len = sizeof(addr);
    getsockname(listen_fd, (struct sockaddr *)&addr, &len);
    return ntohs(addr.sin_port);
}

static int connect_client(uint16_t port)
{
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(port),
    };
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    connect(fd, (struct sockaddr *)&addr, sizeof(addr));
    return fd;
}

void test_oversized_header_returns_431(void)
{
    ioh_server_config_t cfg = make_config(19200);
    ioh_server_t *srv = ioh_server_create(&cfg);
    TEST_ASSERT_NOT_NULL(srv);

    int fd = ioh_server_listen(srv);
    TEST_ASSERT_GREATER_THAN(0, fd);
    uint16_t port = get_bound_port(fd);

    int client_fd = connect_client(port);
    TEST_ASSERT_TRUE(client_fd >= 0);

    /* Send oversized headers (>256 bytes) */
    char big_req[512];
    memset(big_req, 0, sizeof(big_req));
    int len = snprintf(big_req, sizeof(big_req),
                       "GET / HTTP/1.1\r\n"
                       "Host: localhost\r\n"
                       "X-Huge: %0200d\r\n\r\n", 0);

    send(client_fd, big_req, (size_t)len, 0);

    for (int i = 0; i < 10; i++) {
        (void)ioh_server_run_once(srv, 200);
    }

    char resp[4096] = {0};
    recv(client_fd, resp, sizeof(resp) - 1, 0);
    TEST_ASSERT_NOT_NULL(strstr(resp, "431"));

    close(client_fd);
    ioh_server_destroy(srv);
}

void test_oversized_body_returns_413(void)
{
    ioh_server_config_t cfg = make_config(19201);
    ioh_server_t *srv = ioh_server_create(&cfg);
    TEST_ASSERT_NOT_NULL(srv);

    int fd = ioh_server_listen(srv);
    TEST_ASSERT_GREATER_THAN(0, fd);
    uint16_t port = get_bound_port(fd);

    int client_fd = connect_client(port);
    TEST_ASSERT_TRUE(client_fd >= 0);

    /* Send request with Content-Length > max_body_size (64) */
    const char *req = "POST / HTTP/1.1\r\nHost: localhost\r\nContent-Length: 128\r\n\r\n";
    send(client_fd, req, strlen(req), 0);

    for (int i = 0; i < 10; i++) {
        (void)ioh_server_run_once(srv, 200);
    }

    char resp[4096] = {0};
    recv(client_fd, resp, sizeof(resp) - 1, 0);
    TEST_ASSERT_NOT_NULL(strstr(resp, "413"));

    close(client_fd);
    ioh_server_destroy(srv);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_oversized_header_returns_431);
    RUN_TEST(test_oversized_body_returns_413);
    return UNITY_END();
}
```

**Step 5: Register test in CMakeLists.txt**

Same pattern as `test_timeout` above, add `test_limits` integration test block.

**Step 6: Build, test, commit**

```bash
cmake --build --preset clang-debug && ctest --preset clang-debug --output-on-failure
```

```bash
git add src/core/ioh_server.c tests/integration/test_limits.c CMakeLists.txt
git commit -m "feat(core): enforce request header size and body size limits (431/413)"
```

---

## Task 3: Signal Handling via signalfd

**Goal:** Create a signalfd for SIGTERM + SIGQUIT in `ioh_server_run()`, read it via io_uring multishot recv. SIGTERM → graceful drain. SIGQUIT → immediate shutdown.

**Files:**
- Modify: `src/core/ioh_server.c` — add signalfd setup in `ioh_server_run()`, `IOH_OP_SIGNAL` handler
- Modify: `src/core/ioh_server.h` — (no public API change)
- Modify: internal struct — add `signal_fd` field
- Modify: `tests/unit/test_ioh_server.c` — signal handling test
- Create: `tests/integration/test_signal.c` — signal tests
- Modify: `CMakeLists.txt` — register `test_signal`

**Step 1: Add signal_fd to internal server struct**

In `src/core/ioh_server.c`, add to `struct ioh_server`:

```c
    int signal_fd; /**< signalfd for SIGTERM/SIGQUIT, -1 if not set up */
```

Initialize to `-1` in `ioh_server_create()`. Close in `ioh_server_destroy()`.

**Step 2: Add signalfd setup in ioh_server_run()**

Add `#include <sys/signalfd.h>` to ioh_server.c. Before the `while (!srv->stopped)` loop in `ioh_server_run()`:

```c
/* Block SIGTERM + SIGQUIT, redirect to signalfd */
sigset_t mask;
sigemptyset(&mask);
sigaddset(&mask, SIGTERM);
sigaddset(&mask, SIGQUIT);
sigprocmask(SIG_BLOCK, &mask, nullptr);

srv->signal_fd = signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
if (srv->signal_fd >= 0) {
    /* Arm recv on signalfd — we use a special conn_id of 0 (same as accept) */
    struct io_uring *ring = ioh_loop_ring(srv->loop);
    struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
    if (sqe != nullptr) {
        io_uring_prep_read(sqe, srv->signal_fd, &srv->siginfo_buf,
                           sizeof(srv->siginfo_buf), 0);
        io_uring_sqe_set_data64(sqe, IOH_ENCODE_USERDATA(0, IOH_OP_SIGNAL));
    }
}
```

Add `siginfo_buf` to struct:
```c
    struct signalfd_siginfo siginfo_buf; /**< signal read buffer */
```

**Step 3: Add IOH_OP_SIGNAL CQE handler**

In `ioh_server_run_once()`, add handler:

```c
} else if (op == IOH_OP_SIGNAL) {
    if (cqe->res > 0) {
        uint32_t signo = srv->siginfo_buf.ssi_signo;
        if (signo == SIGTERM) {
            (void)ioh_server_shutdown(srv, IOH_SHUTDOWN_DRAIN);
        } else if (signo == SIGQUIT) {
            (void)ioh_server_shutdown(srv, IOH_SHUTDOWN_IMMEDIATE);
        }
    }
    /* Re-arm signal read if not stopped */
    if (!srv->stopped && srv->signal_fd >= 0) {
        struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
        if (sqe != nullptr) {
            io_uring_prep_read(sqe, srv->signal_fd, &srv->siginfo_buf,
                               sizeof(srv->siginfo_buf), 0);
            io_uring_sqe_set_data64(sqe, IOH_ENCODE_USERDATA(0, IOH_OP_SIGNAL));
        }
    }
}
```

**Step 4: Close signalfd on destroy**

In `ioh_server_destroy()`:
```c
if (srv->signal_fd >= 0) {
    close(srv->signal_fd);
    srv->signal_fd = -1;
}
```

**Step 5: Restore signal mask on shutdown**

After the `while` loop in `ioh_server_run()`:
```c
/* Restore signal handling */
if (srv->signal_fd >= 0) {
    close(srv->signal_fd);
    srv->signal_fd = -1;
    sigprocmask(SIG_UNBLOCK, &mask, nullptr);
}
```

**Step 6: Write integration test**

Create `tests/integration/test_signal.c`:

```c
/**
 * @file test_signal.c
 * @brief Integration tests for signal-based shutdown.
 */

#include "core/ioh_server.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <unity.h>

void setUp(void) {}
void tearDown(void) {}

static ioh_server_config_t make_config(uint16_t port)
{
    ioh_server_config_t cfg;
    ioh_server_config_init(&cfg);
    cfg.listen_addr = "127.0.0.1";
    cfg.listen_port = port;
    cfg.max_connections = 16;
    cfg.header_timeout_ms = 5000;
    return cfg;
}

static void *send_signal_thread(void *arg)
{
    uint32_t delay_ms = *(uint32_t *)arg;
    usleep(delay_ms * 1000);
    kill(getpid(), SIGTERM);
    return nullptr;
}

void test_sigterm_triggers_graceful_shutdown(void)
{
    ioh_server_config_t cfg = make_config(19300);
    ioh_server_t *srv = ioh_server_create(&cfg);
    TEST_ASSERT_NOT_NULL(srv);

    /* Schedule SIGTERM after 200ms */
    uint32_t delay = 200;
    pthread_t thr;
    pthread_create(&thr, nullptr, send_signal_thread, &delay);

    /* ioh_server_run() should return after signal */
    int ret = ioh_server_run(srv);
    TEST_ASSERT_EQUAL_INT(0, ret);

    pthread_join(thr, nullptr);
    ioh_server_destroy(srv);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_sigterm_triggers_graceful_shutdown);
    return UNITY_END();
}
```

**Step 7: Register test in CMakeLists.txt**

Add `test_signal` block with `-lpthread` link:

```cmake
    add_executable(test_signal tests/integration/test_signal.c)
    target_include_directories(test_signal PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(test_signal PRIVATE
        unity ioh_server ioh_loop ioh_conn ioh_ctx
        ioh_http1 ioh_request ioh_response
        ioh_router ioh_middleware ioh_radix
        ioh_route_group ioh_route_inspect ioh_route_meta
        pthread
    )
    target_compile_options(test_signal PRIVATE
        -Wno-missing-prototypes -Wno-missing-declarations
    )
    add_test(NAME test_signal COMMAND test_signal)
```

**Step 8: Build, test, commit**

```bash
cmake --build --preset clang-debug && ctest --preset clang-debug --output-on-failure
```

```bash
git add src/core/ioh_server.c tests/integration/test_signal.c CMakeLists.txt
git commit -m "feat(core): add signalfd-based SIGTERM/SIGQUIT shutdown handling"
```

---

## Task 4: Structured Logging Module

**Goal:** Create `ioh_log.h` + `ioh_log.c` with level-filtered logging, custom sink callback, and default stderr output. Integrate into the server pipeline (accept, error, timeout, shutdown).

**Files:**
- Create: `src/core/ioh_log.h` — logging API
- Create: `src/core/ioh_log.c` — logging implementation
- Create: `tests/unit/test_ioh_log.c` — logging unit tests
- Modify: `src/core/ioh_server.c` — add log calls at key pipeline points
- Modify: `CMakeLists.txt` — add `ioh_log` library + test

**Step 1: Create ioh_log.h**

Create `src/core/ioh_log.h`:

```c
/**
 * @file ioh_log.h
 * @brief Structured logging with level filtering and custom sinks.
 */

#ifndef IOHTTP_CORE_LOG_H
#define IOHTTP_CORE_LOG_H

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>

/* ---- Log levels ---- */

typedef enum : uint8_t {
    IOH_LOG_ERROR = 0,
    IOH_LOG_WARN = 1,
    IOH_LOG_INFO = 2,
    IOH_LOG_DEBUG = 3,
} ioh_log_level_t;

/* ---- Log sink callback ---- */

/**
 * @brief Custom log sink function type.
 * @param level    Log level.
 * @param module   Module name (e.g. "server", "tls").
 * @param message  Formatted message.
 * @param user_data Opaque data from ioh_log_set_sink().
 */
typedef void (*ioh_log_sink_fn)(ioh_log_level_t level, const char *module, const char *message,
                               void *user_data);

/* ---- Configuration ---- */

/**
 * @brief Set the minimum log level (messages below are suppressed).
 * @param level Minimum level to emit (default IOH_LOG_INFO).
 */
void ioh_log_set_level(ioh_log_level_t level);

/**
 * @brief Get the current minimum log level.
 */
ioh_log_level_t ioh_log_get_level(void);

/**
 * @brief Set a custom log sink (replaces default stderr sink).
 * @param sink     Sink function (nullptr restores default).
 * @param user_data Opaque data passed to sink.
 */
void ioh_log_set_sink(ioh_log_sink_fn sink, void *user_data);

/* ---- Logging functions ---- */

/**
 * @brief Log a message at the given level.
 * @param level  Log level.
 * @param module Module name.
 * @param fmt    Printf format string.
 */
void ioh_log(ioh_log_level_t level, const char *module, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

/**
 * @brief Return the string name of a log level.
 * @param level Log level.
 * @return Static string ("ERROR", "WARN", "INFO", "DEBUG").
 */
const char *ioh_log_level_name(ioh_log_level_t level);

/* ---- Convenience macros ---- */

#define IOH_LOG_ERROR(mod, ...) ioh_log(IOH_LOG_ERROR, (mod), __VA_ARGS__)
#define IOH_LOG_WARN(mod, ...) ioh_log(IOH_LOG_WARN, (mod), __VA_ARGS__)
#define IOH_LOG_INFO(mod, ...) ioh_log(IOH_LOG_INFO, (mod), __VA_ARGS__)
#define IOH_LOG_DEBUG(mod, ...) ioh_log(IOH_LOG_DEBUG, (mod), __VA_ARGS__)

#endif /* IOHTTP_CORE_LOG_H */
```

**Step 2: Create ioh_log.c**

Create `src/core/ioh_log.c`:

```c
/**
 * @file ioh_log.c
 * @brief Structured logging implementation.
 */

#define _GNU_SOURCE

#include "core/ioh_log.h"

#include <stdarg.h>
#include <stdio.h>
#include <time.h>

/* ---- Global state ---- */

static ioh_log_level_t g_min_level = IOH_LOG_INFO;
static ioh_log_sink_fn g_sink = nullptr;
static void *g_sink_data = nullptr;

/* ---- Level names ---- */

static const char *const level_names[] = {
    [IOH_LOG_ERROR] = "ERROR",
    [IOH_LOG_WARN] = "WARN",
    [IOH_LOG_INFO] = "INFO",
    [IOH_LOG_DEBUG] = "DEBUG",
};

/* ---- Configuration ---- */

void ioh_log_set_level(ioh_log_level_t level)
{
    g_min_level = level;
}

ioh_log_level_t ioh_log_get_level(void)
{
    return g_min_level;
}

void ioh_log_set_sink(ioh_log_sink_fn sink, void *user_data)
{
    g_sink = sink;
    g_sink_data = user_data;
}

/* ---- Default stderr sink ---- */

static void default_sink(ioh_log_level_t level, const char *module, const char *message,
                          void *user_data)
{
    (void)user_data;
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm;
    gmtime_r(&ts.tv_sec, &tm);

    fprintf(stderr, "[%s] %04d-%02d-%02dT%02d:%02d:%02d.%03ldZ %s: %s\n",
            level_names[level],
            tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
            tm.tm_hour, tm.tm_min, tm.tm_sec,
            ts.tv_nsec / 1000000L,
            module, message);
}

/* ---- Core logging ---- */

const char *ioh_log_level_name(ioh_log_level_t level)
{
    if (level > IOH_LOG_DEBUG) {
        return "UNKNOWN";
    }
    return level_names[level];
}

void ioh_log(ioh_log_level_t level, const char *module, const char *fmt, ...)
{
    if (level > g_min_level) {
        return;
    }
    if (module == nullptr || fmt == nullptr) {
        return;
    }

    char buf[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    ioh_log_sink_fn sink = (g_sink != nullptr) ? g_sink : default_sink;
    sink(level, module, buf, g_sink_data);
}
```

**Step 3: Write unit tests**

Create `tests/unit/test_ioh_log.c`:

```c
/**
 * @file test_ioh_log.c
 * @brief Unit tests for structured logging module.
 */

#include "core/ioh_log.h"

#include <string.h>
#include <unity.h>

/* ---- Test sink that captures messages ---- */

static char last_module[64];
static char last_message[1024];
static ioh_log_level_t last_level;
static int sink_call_count;

static void test_sink(ioh_log_level_t level, const char *module, const char *message,
                      void *user_data)
{
    (void)user_data;
    last_level = level;
    snprintf(last_module, sizeof(last_module), "%s", module);
    snprintf(last_message, sizeof(last_message), "%s", message);
    sink_call_count++;
}

void setUp(void)
{
    memset(last_module, 0, sizeof(last_module));
    memset(last_message, 0, sizeof(last_message));
    last_level = IOH_LOG_DEBUG;
    sink_call_count = 0;
    ioh_log_set_level(IOH_LOG_DEBUG);
    ioh_log_set_sink(test_sink, nullptr);
}

void tearDown(void)
{
    ioh_log_set_sink(nullptr, nullptr);
    ioh_log_set_level(IOH_LOG_INFO);
}

void test_log_basic_message(void)
{
    ioh_log(IOH_LOG_INFO, "server", "listening on port %d", 8080);
    TEST_ASSERT_EQUAL_UINT8(IOH_LOG_INFO, last_level);
    TEST_ASSERT_EQUAL_STRING("server", last_module);
    TEST_ASSERT_EQUAL_STRING("listening on port 8080", last_message);
}

void test_log_level_filtering(void)
{
    ioh_log_set_level(IOH_LOG_WARN);
    ioh_log(IOH_LOG_DEBUG, "server", "should be filtered");
    TEST_ASSERT_EQUAL_INT(0, sink_call_count);

    ioh_log(IOH_LOG_INFO, "server", "also filtered");
    TEST_ASSERT_EQUAL_INT(0, sink_call_count);

    ioh_log(IOH_LOG_WARN, "server", "passes filter");
    TEST_ASSERT_EQUAL_INT(1, sink_call_count);

    ioh_log(IOH_LOG_ERROR, "server", "also passes");
    TEST_ASSERT_EQUAL_INT(2, sink_call_count);
}

void test_log_level_names(void)
{
    TEST_ASSERT_EQUAL_STRING("ERROR", ioh_log_level_name(IOH_LOG_ERROR));
    TEST_ASSERT_EQUAL_STRING("WARN", ioh_log_level_name(IOH_LOG_WARN));
    TEST_ASSERT_EQUAL_STRING("INFO", ioh_log_level_name(IOH_LOG_INFO));
    TEST_ASSERT_EQUAL_STRING("DEBUG", ioh_log_level_name(IOH_LOG_DEBUG));
}

void test_log_convenience_macros(void)
{
    IOH_LOG_ERROR("tls", "handshake failed: %s", "timeout");
    TEST_ASSERT_EQUAL_UINT8(IOH_LOG_ERROR, last_level);
    TEST_ASSERT_EQUAL_STRING("tls", last_module);
    TEST_ASSERT_EQUAL_STRING("handshake failed: timeout", last_message);
}

void test_log_get_set_level(void)
{
    ioh_log_set_level(IOH_LOG_ERROR);
    TEST_ASSERT_EQUAL_UINT8(IOH_LOG_ERROR, ioh_log_get_level());
}

void test_log_null_inputs(void)
{
    ioh_log(IOH_LOG_INFO, nullptr, "test");
    TEST_ASSERT_EQUAL_INT(0, sink_call_count);

    ioh_log(IOH_LOG_INFO, "mod", nullptr);
    TEST_ASSERT_EQUAL_INT(0, sink_call_count);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_log_basic_message);
    RUN_TEST(test_log_level_filtering);
    RUN_TEST(test_log_level_names);
    RUN_TEST(test_log_convenience_macros);
    RUN_TEST(test_log_get_set_level);
    RUN_TEST(test_log_null_inputs);
    return UNITY_END();
}
```

**Step 4: Register ioh_log library and test in CMakeLists.txt**

Add after the `ioh_buffer` library block:

```cmake
# ============================================================================
# Structured logging
# ============================================================================

add_library(ioh_log STATIC src/core/ioh_log.c)
target_include_directories(ioh_log PUBLIC ${CMAKE_SOURCE_DIR}/src)

ioh_add_test(test_ioh_log tests/unit/test_ioh_log.c ioh_log)
```

Add `ioh_log` to `ioh_server` link:
```cmake
target_link_libraries(ioh_server PUBLIC ioh_loop ioh_conn ioh_ctx ioh_http1 ioh_request ioh_response ioh_router ioh_middleware ioh_tls ioh_log)
```

**Step 5: Integrate logging into ioh_server.c**

Add `#include "core/ioh_log.h"` to includes. Add log calls at key points:

```c
/* In ioh_server_listen() on success: */
IOH_LOG_INFO("server", "listening on %s:%u (fd=%d)", srv->config.listen_addr,
            srv->config.listen_port, fd);

/* In accept handler on pool full: */
IOH_LOG_WARN("server", "connection pool full, rejecting fd=%d", client_fd);

/* In recv handler on timeout close: */
IOH_LOG_DEBUG("server", "conn %u: timeout (%s), closing",
             conn->id, conn->timeout_phase == IOH_TIMEOUT_HEADER ? "header" :
             conn->timeout_phase == IOH_TIMEOUT_KEEPALIVE ? "keepalive" : "body");

/* In recv handler on 431/413: */
IOH_LOG_WARN("server", "conn %u: header too large (%zu > %u), returning 431",
            conn->id, conn->recv_len, srv->config.max_header_size);

/* In signal handler: */
IOH_LOG_INFO("server", "received signal %u, initiating %s shutdown",
            signo, signo == SIGTERM ? "drain" : "immediate");

/* In ioh_server_shutdown(): */
IOH_LOG_INFO("server", "shutdown mode=%s, active=%u connections",
            mode == IOH_SHUTDOWN_DRAIN ? "drain" : "immediate",
            ioh_conn_pool_active(srv->pool));
```

**Step 6: Build, test, commit**

```bash
cmake --build --preset clang-debug && ctest --preset clang-debug --output-on-failure
```

```bash
git add src/core/ioh_log.h src/core/ioh_log.c tests/unit/test_ioh_log.c src/core/ioh_server.c CMakeLists.txt
git commit -m "feat(core): add structured logging module with level filtering and custom sinks"
```

---

## Task 5: Request ID Generation

**Goal:** Generate a 128-bit hex request ID for every dispatched request. Store in `ioh_ctx_t`, add `X-Request-Id` response header. Propagate incoming `X-Request-Id` if present.

**Files:**
- Modify: `src/core/ioh_server.c` — generate request ID in `dispatch_request()`
- Create: `tests/integration/test_request_id.c` — request ID tests
- Modify: `CMakeLists.txt` — register `test_request_id`

**Step 1: Generate request ID in dispatch_request()**

In `src/core/ioh_server.c`, add `dispatch_request()` request ID generation after `ioh_ctx_init()`:

```c
/* Generate or propagate request ID */
const char *incoming_id = ioh_request_header(req, "X-Request-Id");
if (incoming_id != nullptr) {
    (void)ioh_ctx_set(&ctx, "request_id", (void *)incoming_id);
    (void)ioh_response_set_header(&resp, "X-Request-Id", incoming_id);
} else {
    /* Generate 128-bit hex ID using arc4random */
    uint32_t r1 = arc4random();
    uint32_t r2 = arc4random();
    uint32_t r3 = arc4random();
    uint32_t r4 = arc4random();
    char *rid = ioh_ctx_sprintf(&ctx, "%08x%08x%08x%08x", r1, r2, r3, r4);
    if (rid != nullptr) {
        (void)ioh_ctx_set(&ctx, "request_id", rid);
        (void)ioh_response_set_header(&resp, "X-Request-Id", rid);
    }
}
```

Add `#include <stdlib.h>` if not already present (for `arc4random`).

**Step 2: Write integration test**

Create `tests/integration/test_request_id.c`:

```c
/**
 * @file test_request_id.c
 * @brief Integration tests for X-Request-Id generation and propagation.
 */

#include "core/ioh_server.h"
#include "router/ioh_router.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <unity.h>

void setUp(void) {}
void tearDown(void) {}

static ioh_server_config_t make_config(uint16_t port)
{
    ioh_server_config_t cfg;
    ioh_server_config_init(&cfg);
    cfg.listen_addr = "127.0.0.1";
    cfg.listen_port = port;
    cfg.max_connections = 16;
    cfg.header_timeout_ms = 5000;
    return cfg;
}

static uint16_t get_bound_port(int listen_fd)
{
    struct sockaddr_in addr;
    socklen_t len = sizeof(addr);
    getsockname(listen_fd, (struct sockaddr *)&addr, &len);
    return ntohs(addr.sin_port);
}

static int connect_and_send(uint16_t port, const char *req)
{
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(port),
    };
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    connect(fd, (struct sockaddr *)&addr, sizeof(addr));
    send(fd, req, strlen(req), 0);
    return fd;
}

static int dummy_handler(ioh_ctx_t *c)
{
    return ioh_ctx_text(c, 200, "OK");
}

void test_response_contains_generated_request_id(void)
{
    ioh_server_config_t cfg = make_config(19400);
    ioh_server_t *srv = ioh_server_create(&cfg);
    TEST_ASSERT_NOT_NULL(srv);

    ioh_router_t *r = ioh_router_create();
    (void)ioh_router_add(r, IOH_METHOD_GET, "/", dummy_handler);
    (void)ioh_server_set_router(srv, r);

    int fd = ioh_server_listen(srv);
    uint16_t port = get_bound_port(fd);

    int client_fd = connect_and_send(port, "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n");

    for (int i = 0; i < 10; i++) {
        (void)ioh_server_run_once(srv, 200);
    }

    char resp[4096] = {0};
    recv(client_fd, resp, sizeof(resp) - 1, 0);

    /* Response should contain X-Request-Id header with 32 hex chars */
    TEST_ASSERT_NOT_NULL(strstr(resp, "X-Request-Id: "));
    char *id_start = strstr(resp, "X-Request-Id: ") + 14;
    /* Verify it's 32 hex characters */
    int hex_count = 0;
    for (int i = 0; i < 32 && id_start[i] != '\r'; i++) {
        if ((id_start[i] >= '0' && id_start[i] <= '9') ||
            (id_start[i] >= 'a' && id_start[i] <= 'f')) {
            hex_count++;
        }
    }
    TEST_ASSERT_EQUAL_INT(32, hex_count);

    close(client_fd);
    ioh_router_destroy(r);
    ioh_server_destroy(srv);
}

void test_propagates_incoming_request_id(void)
{
    ioh_server_config_t cfg = make_config(19401);
    ioh_server_t *srv = ioh_server_create(&cfg);
    TEST_ASSERT_NOT_NULL(srv);

    ioh_router_t *r = ioh_router_create();
    (void)ioh_router_add(r, IOH_METHOD_GET, "/", dummy_handler);
    (void)ioh_server_set_router(srv, r);

    int fd = ioh_server_listen(srv);
    uint16_t port = get_bound_port(fd);

    int client_fd = connect_and_send(port,
        "GET / HTTP/1.1\r\nHost: localhost\r\nX-Request-Id: abc123\r\n\r\n");

    for (int i = 0; i < 10; i++) {
        (void)ioh_server_run_once(srv, 200);
    }

    char resp[4096] = {0};
    recv(client_fd, resp, sizeof(resp) - 1, 0);

    /* Should echo back the incoming request ID */
    TEST_ASSERT_NOT_NULL(strstr(resp, "X-Request-Id: abc123"));

    close(client_fd);
    ioh_router_destroy(r);
    ioh_server_destroy(srv);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_response_contains_generated_request_id);
    RUN_TEST(test_propagates_incoming_request_id);
    return UNITY_END();
}
```

**Step 3: Register test in CMakeLists.txt**

Add `test_request_id` integration test block (same pattern as other integration tests).

**Step 4: Build, test, commit**

```bash
cmake --build --preset clang-debug && ctest --preset clang-debug --output-on-failure
```

```bash
git add src/core/ioh_server.c tests/integration/test_request_id.c CMakeLists.txt
git commit -m "feat(core): generate X-Request-Id header with 128-bit hex ID"
```

---

## Task 6: PROXY Protocol Pipeline Integration

**Goal:** Wire `ioh_proxy_decode()` into the accept→recv flow. When `config.proxy_protocol == true`, transition to `IOH_CONN_PROXY_HEADER` after accept, parse the PROXY header on first recv, extract addresses, then proceed to TLS/HTTP.

**Reference:** https://www.haproxy.org/download/3.4/doc/proxy-protocol.txt

**Files:**
- Modify: `src/core/ioh_server.c` — add PROXY protocol state handling in accept + recv
- Modify: `src/core/ioh_conn.h` — add `proxy_bytes_received` counter
- Create: `tests/integration/test_proxy_pipeline.c` — PROXY protocol integration tests
- Modify: `CMakeLists.txt` — register `test_proxy_pipeline`, link `ioh_proxy_proto`

**Step 1: Add proxy tracking field to ioh_conn_t**

In `src/core/ioh_conn.h`, add to `ioh_conn_t`:

```c
    size_t proxy_bytes_received; /**< bytes received during PROXY_HEADER phase */
```

**Step 2: Modify accept handler for PROXY protocol**

In `ioh_server_run_once()` accept handler, change the state transition:

```c
if (srv->config.proxy_protocol) {
    (void)ioh_conn_transition(conn, IOH_CONN_PROXY_HEADER);
    conn->timeout_phase = IOH_TIMEOUT_HEADER;
    conn->proxy_bytes_received = 0;
} else if (srv->tls_ctx != nullptr) {
    conn->tls_ctx = ioh_tls_conn_create(srv->tls_ctx, client_fd);
    conn->tls_done = false;
    (void)ioh_conn_transition(conn, IOH_CONN_TLS_HANDSHAKE);
    conn->timeout_phase = IOH_TIMEOUT_HEADER;
} else {
    (void)ioh_conn_transition(conn, IOH_CONN_HTTP_ACTIVE);
    conn->timeout_phase = IOH_TIMEOUT_HEADER;
}
```

**Step 3: Add PROXY header parsing in recv handler**

In the `IOH_OP_RECV` handler, after `conn->recv_len += (size_t)cqe->res;`, add a PROXY_HEADER state check before the TLS path:

```c
/* ---- PROXY protocol path ---- */
if (conn->state == IOH_CONN_PROXY_HEADER) {
    ioh_proxy_result_t proxy_result;
    int proxy_ret = ioh_proxy_decode(conn->recv_buf, conn->recv_len, &proxy_result);

    if (proxy_ret > 0) {
        /* PROXY header decoded — store addresses */
        conn->proxy_addr = proxy_result.src_addr;
        conn->proxy_used = true;

        /* Consume PROXY header bytes from buffer */
        size_t consumed = (size_t)proxy_ret;
        size_t remaining = conn->recv_len - consumed;
        if (remaining > 0) {
            memmove(conn->recv_buf, conn->recv_buf + consumed, remaining);
        }
        conn->recv_len = remaining;

        IOH_LOG_DEBUG("server", "conn %u: PROXY v%u from %s",
                     conn->id, proxy_result.version,
                     proxy_result.is_local ? "LOCAL" : "remote");

        /* Transition to next state */
        if (srv->tls_ctx != nullptr) {
            conn->tls_ctx = ioh_tls_conn_create(srv->tls_ctx, conn->fd);
            conn->tls_done = false;
            (void)ioh_conn_transition(conn, IOH_CONN_TLS_HANDSHAKE);
        } else {
            (void)ioh_conn_transition(conn, IOH_CONN_HTTP_ACTIVE);
        }

        /* If there's remaining data after PROXY header, re-process */
        if (conn->recv_len > 0) {
            /* Fall through to TLS/HTTP parsing below */
        } else {
            (void)arm_recv(srv, conn);
            processed++;
            continue;
        }
    } else if (proxy_ret == -EAGAIN) {
        /* Need more data */
        (void)arm_recv(srv, conn);
        processed++;
        continue;
    } else {
        /* Malformed PROXY header — close */
        IOH_LOG_WARN("server", "conn %u: malformed PROXY header, closing", conn->id);
        (void)arm_close(srv, conn);
        processed++;
        continue;
    }
}
```

**Step 4: Add ioh_proxy_proto.h include**

In `ioh_server.c`, add:
```c
#include "http/ioh_proxy_proto.h"
```

**Step 5: Write integration tests**

Create `tests/integration/test_proxy_pipeline.c`:

```c
/**
 * @file test_proxy_pipeline.c
 * @brief Integration tests for PROXY protocol in the server pipeline.
 */

#include "core/ioh_server.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <unity.h>

void setUp(void) {}
void tearDown(void) {}

static ioh_server_config_t make_config(uint16_t port)
{
    ioh_server_config_t cfg;
    ioh_server_config_init(&cfg);
    cfg.listen_addr = "127.0.0.1";
    cfg.listen_port = port;
    cfg.max_connections = 16;
    cfg.header_timeout_ms = 5000;
    cfg.proxy_protocol = true;
    return cfg;
}

static uint16_t get_bound_port(int listen_fd)
{
    struct sockaddr_in addr;
    socklen_t len = sizeof(addr);
    getsockname(listen_fd, (struct sockaddr *)&addr, &len);
    return ntohs(addr.sin_port);
}

static int connect_client(uint16_t port)
{
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(port),
    };
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    connect(fd, (struct sockaddr *)&addr, sizeof(addr));
    return fd;
}

static int on_request_echo(ioh_ctx_t *c, void *user_data)
{
    (void)user_data;
    return ioh_ctx_text(c, 200, "OK");
}

void test_proxy_v1_tcp4_pipeline(void)
{
    ioh_server_config_t cfg = make_config(19500);
    ioh_server_t *srv = ioh_server_create(&cfg);
    TEST_ASSERT_NOT_NULL(srv);
    (void)ioh_server_set_on_request(srv, on_request_echo, nullptr);

    int fd = ioh_server_listen(srv);
    TEST_ASSERT_GREATER_THAN(0, fd);
    uint16_t port = get_bound_port(fd);

    int client_fd = connect_client(port);
    TEST_ASSERT_TRUE(client_fd >= 0);

    /* Send PROXY v1 header followed by HTTP request */
    const char *proxy_header = "PROXY TCP4 192.168.1.1 192.168.1.2 12345 80\r\n";
    const char *http_req = "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n";

    send(client_fd, proxy_header, strlen(proxy_header), 0);
    send(client_fd, http_req, strlen(http_req), 0);

    for (int i = 0; i < 15; i++) {
        (void)ioh_server_run_once(srv, 200);
    }

    char resp[4096] = {0};
    recv(client_fd, resp, sizeof(resp) - 1, 0);

    /* Should get a 200 response — PROXY header was parsed + HTTP processed */
    TEST_ASSERT_NOT_NULL(strstr(resp, "200"));

    close(client_fd);
    ioh_server_destroy(srv);
}

void test_proxy_invalid_header_closes_connection(void)
{
    ioh_server_config_t cfg = make_config(19501);
    ioh_server_t *srv = ioh_server_create(&cfg);
    TEST_ASSERT_NOT_NULL(srv);
    (void)ioh_server_set_on_request(srv, on_request_echo, nullptr);

    int fd = ioh_server_listen(srv);
    TEST_ASSERT_GREATER_THAN(0, fd);
    uint16_t port = get_bound_port(fd);

    int client_fd = connect_client(port);
    TEST_ASSERT_TRUE(client_fd >= 0);

    /* Send garbage instead of PROXY header */
    const char *garbage = "NOT_A_PROXY_HEADER\r\n";
    send(client_fd, garbage, strlen(garbage), 0);

    for (int i = 0; i < 10; i++) {
        (void)ioh_server_run_once(srv, 200);
    }

    /* Connection should be closed */
    TEST_ASSERT_EQUAL_UINT32(0, ioh_conn_pool_active(ioh_server_pool(srv)));

    close(client_fd);
    ioh_server_destroy(srv);
}

void test_non_proxy_listener_ignores_proxy_headers(void)
{
    ioh_server_config_t cfg = make_config(19502);
    cfg.proxy_protocol = false;  /* NOT a PROXY listener */
    ioh_server_t *srv = ioh_server_create(&cfg);
    TEST_ASSERT_NOT_NULL(srv);
    (void)ioh_server_set_on_request(srv, on_request_echo, nullptr);

    int fd = ioh_server_listen(srv);
    TEST_ASSERT_GREATER_THAN(0, fd);
    uint16_t port = get_bound_port(fd);

    int client_fd = connect_client(port);
    TEST_ASSERT_TRUE(client_fd >= 0);

    /* Send normal HTTP request (no PROXY) */
    const char *http_req = "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n";
    send(client_fd, http_req, strlen(http_req), 0);

    for (int i = 0; i < 10; i++) {
        (void)ioh_server_run_once(srv, 200);
    }

    char resp[4096] = {0};
    recv(client_fd, resp, sizeof(resp) - 1, 0);
    TEST_ASSERT_NOT_NULL(strstr(resp, "200"));

    close(client_fd);
    ioh_server_destroy(srv);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_proxy_v1_tcp4_pipeline);
    RUN_TEST(test_proxy_invalid_header_closes_connection);
    RUN_TEST(test_non_proxy_listener_ignores_proxy_headers);
    return UNITY_END();
}
```

**Step 6: Register test in CMakeLists.txt**

Add `test_proxy_pipeline` block, including `ioh_proxy_proto` in link libraries:

```cmake
    add_executable(test_proxy_pipeline tests/integration/test_proxy_pipeline.c)
    target_include_directories(test_proxy_pipeline PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(test_proxy_pipeline PRIVATE
        unity ioh_server ioh_loop ioh_conn ioh_ctx
        ioh_http1 ioh_request ioh_response
        ioh_router ioh_middleware ioh_radix
        ioh_route_group ioh_route_inspect ioh_route_meta
        ioh_proxy_proto ioh_log
    )
    target_compile_options(test_proxy_pipeline PRIVATE
        -Wno-missing-prototypes -Wno-missing-declarations
    )
    add_test(NAME test_proxy_pipeline COMMAND test_proxy_pipeline)
```

Also add `ioh_proxy_proto` and `ioh_log` to `ioh_server` link libraries in CMakeLists.txt:

```cmake
target_link_libraries(ioh_server PUBLIC ioh_loop ioh_conn ioh_ctx ioh_http1 ioh_request ioh_response ioh_router ioh_middleware ioh_tls ioh_log ioh_proxy_proto)
```

**Step 7: Build, test, commit**

```bash
cmake --build --preset clang-debug && ctest --preset clang-debug --output-on-failure
```

```bash
git add src/core/ioh_server.c src/core/ioh_conn.h tests/integration/test_proxy_pipeline.c CMakeLists.txt
git commit -m "feat(core): wire PROXY protocol v1/v2 into accept→recv pipeline"
```

---

## Task 7: Quality Pipeline + clang-format

**Goal:** Run the full quality pipeline. Fix any clang-format, cppcheck, PVS-Studio, or CodeChecker findings.

**Files:**
- Modify: any files flagged by quality tools

**Step 1: Run clang-format**

```bash
cmake --build --preset clang-debug --target format
```

If any files changed, commit:
```bash
git add -u && git commit -m "style: apply clang-format to Sprint 12 source files"
```

**Step 2: Run full quality pipeline**

```bash
podman run --rm --security-opt seccomp=unconfined \
  -v /opt/projects/repositories/iohttp:/workspace:Z \
  localhost/iohttp-dev:latest bash -c "cd /workspace && ./scripts/quality.sh"
```

Expected: **PASS: 6 / FAIL: 0**

**Step 3: Fix any findings**

For PVS-Studio false positives, add inline `//-VXXX` suppressions on the **exact reported line**.

For CodeChecker findings, fix the underlying issue (not suppress).

**Step 4: Commit fixes if needed**

```bash
git add -u && git commit -m "fix(quality): resolve Sprint 12 static analysis findings"
```

---

## Summary

| Task | Deliverable | Tests |
|------|-------------|-------|
| 1 | Linked timeouts (header/body/keepalive) | 1 unit + 2 integration |
| 2 | Request limits (431/413) | 2 integration |
| 3 | signalfd (SIGTERM/SIGQUIT) | 1 integration |
| 4 | Structured logging (`ioh_log`) | 6 unit |
| 5 | Request ID (`X-Request-Id`) | 2 integration |
| 6 | PROXY protocol pipeline | 3 integration |
| 7 | Quality pipeline pass | — |

**Total new tests: ~17**
**Total tests after sprint: ~57**

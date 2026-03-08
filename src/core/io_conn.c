/**
 * @file io_conn.c
 * @brief Connection state machine and connection pool implementation.
 */

#include "core/io_conn.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* ---- Internal pool structure ---- */

struct io_conn_pool {
    io_conn_t *conns; /* calloc(max_conns) array */
    uint32_t max_conns;
    uint32_t active_count;
    uint32_t next_id; /* monotonically increasing connection ID */
};

/* ---- State transition table ---- */

/*
 * Valid transitions (bit mask per source state).
 * Bit position corresponds to the destination io_conn_state_t value.
 */
static const uint16_t valid_transitions[] = {
    /* FREE        → ACCEPTING */
    [IO_CONN_FREE] = 1u << IO_CONN_ACCEPTING,
    /* ACCEPTING   → PROXY_HEADER, TLS_HANDSHAKE, HTTP_ACTIVE, CLOSING */
    [IO_CONN_ACCEPTING] = (1u << IO_CONN_PROXY_HEADER) | (1u << IO_CONN_TLS_HANDSHAKE) |
                          (1u << IO_CONN_HTTP_ACTIVE) | (1u << IO_CONN_CLOSING),
    /* PROXY_HEADER → TLS_HANDSHAKE, HTTP_ACTIVE, CLOSING */
    [IO_CONN_PROXY_HEADER] = (1u << IO_CONN_TLS_HANDSHAKE) | (1u << IO_CONN_HTTP_ACTIVE) |
                             (1u << IO_CONN_CLOSING),
    /* TLS_HANDSHAKE → HTTP_ACTIVE, CLOSING */
    [IO_CONN_TLS_HANDSHAKE] = (1u << IO_CONN_HTTP_ACTIVE) | (1u << IO_CONN_CLOSING),
    /* HTTP_ACTIVE → WEBSOCKET, SSE, DRAINING, CLOSING */
    [IO_CONN_HTTP_ACTIVE] = (1u << IO_CONN_WEBSOCKET) | (1u << IO_CONN_SSE) |
                            (1u << IO_CONN_DRAINING) | (1u << IO_CONN_CLOSING),
    /* WEBSOCKET → DRAINING, CLOSING */
    [IO_CONN_WEBSOCKET] = (1u << IO_CONN_DRAINING) | (1u << IO_CONN_CLOSING),
    /* SSE → DRAINING, CLOSING */
    [IO_CONN_SSE] = (1u << IO_CONN_DRAINING) | (1u << IO_CONN_CLOSING),
    /* DRAINING → CLOSING */
    [IO_CONN_DRAINING] = (1u << IO_CONN_CLOSING),
    /* CLOSING → FREE (only via io_conn_free, not transition) */
    [IO_CONN_CLOSING] = 0,
};

/* ---- State name strings ---- */

static const char *const state_names[] = {
    [IO_CONN_FREE] = "FREE",
    [IO_CONN_ACCEPTING] = "ACCEPTING",
    [IO_CONN_PROXY_HEADER] = "PROXY_HEADER",
    [IO_CONN_TLS_HANDSHAKE] = "TLS_HANDSHAKE",
    [IO_CONN_HTTP_ACTIVE] = "HTTP_ACTIVE",
    [IO_CONN_WEBSOCKET] = "WEBSOCKET",
    [IO_CONN_SSE] = "SSE",
    [IO_CONN_DRAINING] = "DRAINING",
    [IO_CONN_CLOSING] = "CLOSING",
};

/* ---- Pool lifecycle ---- */

io_conn_pool_t *io_conn_pool_create(uint32_t max_conns)
{
    if (max_conns == 0) {
        return nullptr;
    }

    io_conn_pool_t *pool = calloc(1, sizeof(*pool));
    if (!pool) {
        return nullptr;
    }

    pool->conns = calloc(max_conns, sizeof(*pool->conns));
    if (!pool->conns) {
        free(pool);
        return nullptr;
    }

    pool->max_conns = max_conns;
    pool->active_count = 0;
    pool->next_id = 1;

    /* Initialize all slots as FREE with fd = -1 */
    for (uint32_t i = 0; i < max_conns; i++) {
        pool->conns[i].fd = -1;
        pool->conns[i].state = IO_CONN_FREE;
    }

    return pool;
}

void io_conn_pool_destroy(io_conn_pool_t *pool)
{
    if (!pool) {
        return;
    }

    /* Free per-connection buffers before releasing the pool */
    for (uint32_t i = 0; i < pool->max_conns; i++) {
        free(pool->conns[i].recv_buf);
        free(pool->conns[i].send_buf);
    }

    free(pool->conns);
    free(pool);
}

/* ---- Connection management ---- */

constexpr size_t IO_CONN_RECV_BUF_SIZE = 8192;

io_conn_t *io_conn_alloc(io_conn_pool_t *pool)
{
    if (!pool) {
        return nullptr;
    }

    for (uint32_t i = 0; i < pool->max_conns; i++) {
        if (pool->conns[i].state == IO_CONN_FREE) {
            io_conn_t *conn = &pool->conns[i];
            memset(conn, 0, sizeof(*conn));
            conn->fd = -1;
            conn->state = IO_CONN_ACCEPTING;
            conn->id = pool->next_id++;
            pool->active_count++;

            conn->recv_buf = malloc(IO_CONN_RECV_BUF_SIZE);
            if (conn->recv_buf == nullptr) {
                conn->state = IO_CONN_FREE;
                pool->active_count--;
                return nullptr;
            }
            conn->recv_buf_size = IO_CONN_RECV_BUF_SIZE;
            conn->recv_len = 0;
            conn->send_buf = nullptr;
            conn->send_len = 0;
            conn->send_offset = 0;
            conn->send_active = false;
            conn->keep_alive = false;

            return conn;
        }
    }

    return nullptr;
}

void io_conn_free(io_conn_pool_t *pool, io_conn_t *conn)
{
    if (!pool || !conn) {
        return;
    }

    free(conn->recv_buf);
    free(conn->send_buf);

    memset(conn, 0, sizeof(*conn));
    conn->fd = -1;
    conn->state = IO_CONN_FREE;
    pool->active_count--;
}

io_conn_t *io_conn_find(io_conn_pool_t *pool, int fd)
{
    if (!pool || fd < 0) {
        return nullptr;
    }

    for (uint32_t i = 0; i < pool->max_conns; i++) {
        if (pool->conns[i].state != IO_CONN_FREE && pool->conns[i].fd == fd) {
            return &pool->conns[i];
        }
    }

    return nullptr;
}

io_conn_t *io_conn_pool_get(io_conn_pool_t *pool, uint32_t index)
{
    if (pool == nullptr || index >= pool->max_conns) {
        return nullptr;
    }
    return &pool->conns[index];
}

/* ---- State machine ---- */

int io_conn_transition(io_conn_t *conn, io_conn_state_t new_state)
{
    if (!conn) {
        return -EINVAL;
    }

    if (conn->state >= sizeof(valid_transitions) / sizeof(valid_transitions[0])) {
        return -EINVAL;
    }

    if (new_state >= sizeof(valid_transitions) / sizeof(valid_transitions[0])) {
        return -EINVAL;
    }

    if (!(valid_transitions[conn->state] & (1u << new_state))) {
        return -EINVAL;
    }

    conn->state = new_state;
    return 0;
}

const char *io_conn_state_name(io_conn_state_t state)
{
    if (state >= sizeof(state_names) / sizeof(state_names[0])) {
        return "UNKNOWN";
    }

    return state_names[state];
}

/* ---- Pool stats ---- */

uint32_t io_conn_pool_active(const io_conn_pool_t *pool)
{
    if (!pool) {
        return 0;
    }

    return pool->active_count;
}

uint32_t io_conn_pool_capacity(const io_conn_pool_t *pool)
{
    if (!pool) {
        return 0;
    }

    return pool->max_conns;
}

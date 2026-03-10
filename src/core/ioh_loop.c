/**
 * @file ioh_loop.c
 * @brief io_uring ring management wrapper implementation.
 */

#include "core/ioh_loop.h"

#include <errno.h>
#include <stdlib.h>

/* ---- Internal constants ---- */

#define IOH_MAX_TIMERS 64

/* ---- Internal struct ---- */

struct ioh_loop {
    struct io_uring ring;
    ioh_loop_config_t config;
    bool running;
    bool stopped;

    /* Provided buffer ring */
    struct io_uring_buf_ring *buf_ring;
    uint8_t *buf_base;
    uint32_t buf_count;
    uint32_t buf_size;
    uint16_t buf_bgid;

    /* Timer tracking */
    struct {
        struct __kernel_timespec ts;
        void (*callback)(void *);
        void *data;
        bool active;
    } timers[IOH_MAX_TIMERS];
    uint32_t timer_count;
};

/* ---- Helpers ---- */

static bool is_power_of_2(uint32_t v)
{
    return v != 0 && (v & (v - 1)) == 0;
}

/* ---- Lifecycle ---- */

void ioh_loop_config_init(ioh_loop_config_t *cfg)
{
    if (cfg == nullptr) {
        return;
    }
    cfg->queue_depth = 256;
    cfg->buf_ring_size = 128;
    cfg->buf_size = 4096;
    cfg->defer_taskrun = true;
}

int ioh_loop_config_validate(const ioh_loop_config_t *cfg)
{
    if (cfg == nullptr) {
        return -EINVAL;
    }
    if (cfg->queue_depth == 0) {
        return -EINVAL;
    }
    if (!is_power_of_2(cfg->queue_depth)) {
        return -EINVAL;
    }
    if (cfg->buf_ring_size != 0 && !is_power_of_2(cfg->buf_ring_size)) {
        return -EINVAL;
    }
    if (cfg->buf_size == 0) {
        return -EINVAL;
    }
    return 0;
}

ioh_loop_t *ioh_loop_create(const ioh_loop_config_t *cfg)
{
    if (cfg == nullptr) {
        return nullptr;
    }
    if (ioh_loop_config_validate(cfg) != 0) {
        return nullptr;
    }

    ioh_loop_t *loop = calloc(1, sizeof(*loop));
    if (loop == nullptr) {
        return nullptr;
    }
    loop->config = *cfg;

    struct io_uring_params params = {0};

    if (cfg->defer_taskrun) {
        params.flags = IORING_SETUP_DEFER_TASKRUN | IORING_SETUP_SINGLE_ISSUER;
    }

    int ret = io_uring_queue_init_params(cfg->queue_depth, &loop->ring, &params);
    if (ret < 0) {
        free(loop);
        return nullptr;
    }

    loop->running = false;
    loop->stopped = false;

    return loop;
}

void ioh_loop_destroy(ioh_loop_t *loop)
{
    if (loop == nullptr) {
        return;
    }

    /* Free provided buffer ring if set up */
    if (loop->buf_ring != nullptr) {
        io_uring_free_buf_ring(&loop->ring, loop->buf_ring, loop->buf_count, loop->buf_bgid);
        free(loop->buf_base);
        loop->buf_ring = nullptr;
        loop->buf_base = nullptr;
    }

    io_uring_queue_exit(&loop->ring);
    free(loop);
}

/* ---- Ring access ---- */

struct io_uring *ioh_loop_ring(ioh_loop_t *loop)
{
    if (loop == nullptr) {
        return nullptr;
    }
    return &loop->ring;
}

/* ---- NOP ---- */

int ioh_loop_submit_nop(ioh_loop_t *loop)
{
    if (loop == nullptr) {
        return -EINVAL;
    }

    struct io_uring_sqe *sqe = io_uring_get_sqe(&loop->ring);
    if (sqe == nullptr) {
        return -ENOSPC;
    }

    io_uring_prep_nop(sqe);
    io_uring_sqe_set_data64(sqe, IOH_ENCODE_USERDATA(0, IOH_OP_NOP));

    int ret = io_uring_submit(&loop->ring);
    if (ret < 0) {
        return ret;
    }

    return 0;
}

/* ---- Event loop ---- */

int ioh_loop_run_once(ioh_loop_t *loop, uint32_t timeout_ms)
{
    if (loop == nullptr) {
        return -EINVAL;
    }

    if (loop->stopped) {
        return 0;
    }

    loop->running = true;

    /* Submit any pending SQEs */
    int ret = io_uring_submit(&loop->ring);
    if (ret < 0) {
        loop->running = false;
        return ret;
    }

    /* Wait for at least one completion */
    struct io_uring_cqe *cqe;

    if (timeout_ms > 0) {
        struct __kernel_timespec ts = {
            .tv_sec = (long long)(timeout_ms / 1000),
            .tv_nsec = (long long)(timeout_ms % 1000) * 1000000LL,
        };
        ret = io_uring_wait_cqe_timeout(&loop->ring, &cqe, &ts);
    } else {
        ret = io_uring_wait_cqe(&loop->ring, &cqe);
    }

    if (ret < 0) {
        loop->running = false;
        /* -ETIME is expected for timeout expiration, not a fatal error */
        return ret;
    }

    /* Process all available CQEs */
    int processed = 0;
    unsigned head;
    io_uring_for_each_cqe(&loop->ring, head, cqe)
    {
        uint64_t ud = io_uring_cqe_get_data64(cqe);
        uint8_t op = IOH_DECODE_OP(ud);

        if (op == IOH_OP_TIMEOUT) {
            uint64_t timer_id = IOH_DECODE_ID(ud);
            if (timer_id < IOH_MAX_TIMERS && loop->timers[timer_id].active) {
                /* Timer fired (res == -ETIME) or was cancelled (res == -ECANCELED) */
                if (cqe->res == -ETIME || cqe->res == 0) {
                    if (loop->timers[timer_id].callback != nullptr) {
                        loop->timers[timer_id].callback(loop->timers[timer_id].data);
                    }
                }
                loop->timers[timer_id].active = false;
            }
        }
        processed++;
    }
    io_uring_cq_advance(&loop->ring, (unsigned)processed);

    loop->running = false;
    return processed;
}

void ioh_loop_stop(ioh_loop_t *loop)
{
    if (loop == nullptr) {
        return;
    }
    loop->stopped = true;
}

/* ---- Timer management ---- */

int ioh_loop_add_timer(ioh_loop_t *loop, uint32_t ms, void (*cb)(void *), void *data)
{
    if (loop == nullptr || cb == nullptr) {
        return -EINVAL;
    }

    /* Find a free timer slot */
    int slot = -1;
    for (uint32_t i = 0; i < IOH_MAX_TIMERS; i++) {
        if (!loop->timers[i].active) {
            slot = (int)i;
            break;
        }
    }
    if (slot < 0) {
        return -ENOMEM;
    }

    loop->timers[slot].ts.tv_sec = (long long)(ms / 1000);
    loop->timers[slot].ts.tv_nsec = (long long)(ms % 1000) * 1000000LL;
    loop->timers[slot].callback = cb;
    loop->timers[slot].data = data;
    loop->timers[slot].active = true;
    loop->timer_count++;

    struct io_uring_sqe *sqe = io_uring_get_sqe(&loop->ring);
    if (sqe == nullptr) {
        loop->timers[slot].active = false;
        loop->timer_count--;
        return -ENOSPC;
    }

    io_uring_prep_timeout(sqe, &loop->timers[slot].ts, 0, 0);
    io_uring_sqe_set_data64(sqe, IOH_ENCODE_USERDATA((uint64_t)slot, IOH_OP_TIMEOUT));

    return slot;
}

int ioh_loop_cancel_timer(ioh_loop_t *loop, int timer_id)
{
    if (loop == nullptr) {
        return -EINVAL;
    }
    if (timer_id < 0 || (uint32_t)timer_id >= IOH_MAX_TIMERS) {
        return -EINVAL;
    }
    if (!loop->timers[timer_id].active) {
        return -ENOENT;
    }

    struct io_uring_sqe *sqe = io_uring_get_sqe(&loop->ring);
    if (sqe == nullptr) {
        return -ENOSPC;
    }

    /* Cancel by user_data match */
    io_uring_prep_cancel64(sqe, IOH_ENCODE_USERDATA((uint64_t)timer_id, IOH_OP_TIMEOUT), 0);
    io_uring_sqe_set_data64(sqe, IOH_ENCODE_USERDATA((uint64_t)timer_id, IOH_OP_CANCEL));

    loop->timers[timer_id].active = false;
    loop->timer_count--;

    return 0;
}

/* ---- Provided buffer ring ---- */

int ioh_loop_setup_buf_ring(ioh_loop_t *loop, uint16_t bgid, uint32_t buf_count, uint32_t buf_size)
{
    if (loop == nullptr) {
        return -EINVAL;
    }
    if (!is_power_of_2(buf_count)) {
        return -EINVAL;
    }
    if (buf_size == 0) {
        return -EINVAL;
    }

    /* Allocate the buffer storage */
    uint8_t *base = calloc(buf_count, buf_size);
    if (base == nullptr) {
        return -ENOMEM;
    }

    int err = 0;
    struct io_uring_buf_ring *br = io_uring_setup_buf_ring(&loop->ring, buf_count, bgid, 0, &err);
    if (br == nullptr) {
        free(base);
        return err;
    }

    /* Register each buffer in the ring */
    for (uint32_t i = 0; i < buf_count; i++) {
        io_uring_buf_ring_add(br, base + ((size_t)i * buf_size), buf_size, (unsigned short)i,
                              io_uring_buf_ring_mask(buf_count), (int)i);
    }
    io_uring_buf_ring_advance(br, (int)buf_count);

    loop->buf_ring = br;
    loop->buf_base = base;
    loop->buf_count = buf_count;
    loop->buf_size = buf_size;
    loop->buf_bgid = bgid;

    return 0;
}

void ioh_loop_return_buf(ioh_loop_t *loop, uint16_t bgid, uint32_t buf_id, uint8_t *buf,
                        uint32_t buf_size)
{
    if (loop == nullptr || loop->buf_ring == nullptr) {
        return;
    }
    if (bgid != loop->buf_bgid) {
        return;
    }

    io_uring_buf_ring_add(loop->buf_ring, buf, buf_size, (unsigned short)buf_id,
                          io_uring_buf_ring_mask(loop->buf_count), 0);
    io_uring_buf_ring_advance(loop->buf_ring, 1);
}

uint8_t *ioh_loop_get_buf(ioh_loop_t *loop, uint16_t bgid, uint32_t buf_id)
{
    if (loop == nullptr || loop->buf_base == nullptr) {
        return nullptr;
    }
    if (bgid != loop->buf_bgid) {
        return nullptr;
    }
    if (buf_id >= loop->buf_count) {
        return nullptr;
    }
    return loop->buf_base + ((size_t)buf_id * loop->buf_size);
}

/* ---- Registered buffers ---- */

int ioh_loop_register_buffers(ioh_loop_t *loop, struct iovec *iovs, unsigned nr)
{
    if (loop == nullptr || iovs == nullptr || nr == 0) {
        return -EINVAL;
    }
    int ret = io_uring_register_buffers(&loop->ring, iovs, nr);
    if (ret < 0) {
        return ret;
    }
    return 0;
}

int ioh_loop_unregister_buffers(ioh_loop_t *loop)
{
    if (loop == nullptr) {
        return -EINVAL;
    }
    int ret = io_uring_unregister_buffers(&loop->ring);
    if (ret < 0) {
        return ret;
    }
    return 0;
}

/* ---- Registered files ---- */

int ioh_loop_register_files(ioh_loop_t *loop, int *fds, unsigned nr)
{
    if (loop == nullptr || fds == nullptr || nr == 0) {
        return -EINVAL;
    }
    int ret = io_uring_register_files(&loop->ring, fds, nr);
    if (ret < 0) {
        return ret;
    }
    return 0;
}

int ioh_loop_unregister_files(ioh_loop_t *loop)
{
    if (loop == nullptr) {
        return -EINVAL;
    }
    int ret = io_uring_unregister_files(&loop->ring);
    if (ret < 0) {
        return ret;
    }
    return 0;
}

/**
 * @file ioh_buffer.c
 * @brief Higher-level buffer pool implementation wrapping io_uring primitives.
 */

#include "core/ioh_buffer.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* ---- Internal struct ---- */

struct ioh_bufpool {
    ioh_bufpool_config_t config;

    /* Provided buffer ring */
    struct io_uring_buf_ring *buf_ring;
    uint8_t *buf_base;
    bool ring_registered;

    /* Registered buffers (pinned DMA) */
    uint8_t *reg_buf_base;
    struct iovec *reg_iovs;
    bool bufs_registered;

    /* Registered files (fd slots) */
    int *file_slots;
    bool files_registered;
};

/* ---- Helpers ---- */

static bool is_power_of_2(uint32_t v)
{
    return v != 0 && (v & (v - 1)) == 0;
}

/* ---- Lifecycle ---- */

void ioh_bufpool_config_init(ioh_bufpool_config_t *cfg)
{
    if (cfg == nullptr) {
        return;
    }
    cfg->ring_size = 128;
    cfg->buf_size = 4096;
    cfg->bgid = 0;
    cfg->reg_buf_count = 0;
    cfg->reg_buf_size = 16384;
    cfg->reg_file_count = 0;
}

int ioh_bufpool_config_validate(const ioh_bufpool_config_t *cfg)
{
    if (cfg == nullptr) {
        return -EINVAL;
    }
    if (cfg->ring_size == 0) {
        return -EINVAL;
    }
    if (!is_power_of_2(cfg->ring_size)) {
        return -EINVAL;
    }
    if (cfg->buf_size == 0) {
        return -EINVAL;
    }
    if (cfg->reg_buf_count > 0 && cfg->reg_buf_size == 0) {
        return -EINVAL;
    }
    return 0;
}

ioh_bufpool_t *ioh_bufpool_create(const ioh_bufpool_config_t *cfg)
{
    if (cfg == nullptr) {
        return nullptr;
    }
    if (ioh_bufpool_config_validate(cfg) != 0) {
        return nullptr;
    }

    ioh_bufpool_t *pool = calloc(1, sizeof(*pool));
    if (pool == nullptr) {
        return nullptr;
    }
    pool->config = *cfg;

    /* Allocate provided buffer storage */
    pool->buf_base = calloc(cfg->ring_size, cfg->buf_size);
    if (pool->buf_base == nullptr) {
        free(pool);
        return nullptr;
    }

    /* Allocate registered buffer storage if requested */
    if (cfg->reg_buf_count > 0) {
        pool->reg_buf_base = calloc(cfg->reg_buf_count, cfg->reg_buf_size);
        if (pool->reg_buf_base == nullptr) {
            free(pool->buf_base);
            free(pool);
            return nullptr;
        }

        pool->reg_iovs = calloc(cfg->reg_buf_count, sizeof(*pool->reg_iovs));
        if (pool->reg_iovs == nullptr) {
            free(pool->reg_buf_base);
            free(pool->buf_base);
            free(pool);
            return nullptr;
        }

        for (uint32_t i = 0; i < cfg->reg_buf_count; i++) {
            pool->reg_iovs[i].iov_base = pool->reg_buf_base + ((size_t)i * cfg->reg_buf_size);
            pool->reg_iovs[i].iov_len = cfg->reg_buf_size;
        }
    }

    return pool;
}

void ioh_bufpool_destroy(ioh_bufpool_t *pool, struct io_uring *ring)
{
    if (pool == nullptr) {
        return;
    }

    /* Free provided buffer ring */
    if (pool->ring_registered && pool->buf_ring != nullptr && ring != nullptr) {
        io_uring_free_buf_ring(ring, pool->buf_ring, pool->config.ring_size, pool->config.bgid);
    }
    free(pool->buf_base);

    /* Unregister and free registered buffers */
    if (pool->bufs_registered && ring != nullptr) {
        io_uring_unregister_buffers(ring);
    }
    free(pool->reg_iovs);
    free(pool->reg_buf_base);

    /* Unregister and free file slots */
    if (pool->files_registered && ring != nullptr) {
        io_uring_unregister_files(ring);
    }
    free(pool->file_slots);

    free(pool);
}

/* ---- Registration ---- */

int ioh_bufpool_register_ring(ioh_bufpool_t *pool, struct io_uring *ring)
{
    if (pool == nullptr || ring == nullptr) {
        return -EINVAL;
    }
    if (pool->ring_registered) {
        return -EALREADY;
    }

    int err = 0;
    struct io_uring_buf_ring *br =
        io_uring_setup_buf_ring(ring, pool->config.ring_size, pool->config.bgid, 0, &err);
    if (br == nullptr) {
        return err;
    }

    /* Add all buffers to the ring */
    for (uint32_t i = 0; i < pool->config.ring_size; i++) {
        io_uring_buf_ring_add(br, pool->buf_base + ((size_t)i * pool->config.buf_size),
                              pool->config.buf_size, (unsigned short)i,
                              io_uring_buf_ring_mask(pool->config.ring_size), (int)i);
    }
    io_uring_buf_ring_advance(br, (int)pool->config.ring_size);

    pool->buf_ring = br;
    pool->ring_registered = true;

    return 0;
}

int ioh_bufpool_register_bufs(ioh_bufpool_t *pool, struct io_uring *ring)
{
    if (pool == nullptr || ring == nullptr) {
        return -EINVAL;
    }
    if (pool->config.reg_buf_count == 0) {
        return -EINVAL;
    }
    if (pool->bufs_registered) {
        return -EALREADY;
    }

    int ret = io_uring_register_buffers(ring, pool->reg_iovs, pool->config.reg_buf_count);
    if (ret < 0) {
        return ret;
    }

    pool->bufs_registered = true;
    return 0;
}

int ioh_bufpool_register_files(ioh_bufpool_t *pool, struct io_uring *ring)
{
    if (pool == nullptr || ring == nullptr) {
        return -EINVAL;
    }
    if (pool->config.reg_file_count == 0) {
        return -EINVAL;
    }
    if (pool->files_registered) {
        return -EALREADY;
    }

    pool->file_slots = malloc(pool->config.reg_file_count * sizeof(*pool->file_slots));
    if (pool->file_slots == nullptr) {
        return -ENOMEM;
    }

    /* Fill with -1 (empty slots) */
    for (uint32_t i = 0; i < pool->config.reg_file_count; i++) {
        pool->file_slots[i] = -1;
    }

    int ret = io_uring_register_files(ring, pool->file_slots, pool->config.reg_file_count);
    if (ret < 0) {
        free(pool->file_slots);
        pool->file_slots = nullptr;
        return ret;
    }

    pool->files_registered = true;
    return 0;
}

/* ---- Provided buffer access ---- */

uint8_t *ioh_bufpool_get_buf(ioh_bufpool_t *pool, uint32_t buf_id)
{
    if (pool == nullptr || pool->buf_base == nullptr) {
        return nullptr;
    }
    if (buf_id >= pool->config.ring_size) {
        return nullptr;
    }
    return pool->buf_base + ((size_t)buf_id * pool->config.buf_size);
}

void ioh_bufpool_return_buf(ioh_bufpool_t *pool, uint32_t buf_id)
{
    if (pool == nullptr || pool->buf_ring == nullptr) {
        return;
    }
    if (buf_id >= pool->config.ring_size) {
        return;
    }

    io_uring_buf_ring_add(pool->buf_ring, pool->buf_base + ((size_t)buf_id * pool->config.buf_size),
                          pool->config.buf_size, (unsigned short)buf_id,
                          io_uring_buf_ring_mask(pool->config.ring_size), 0);
    io_uring_buf_ring_advance(pool->buf_ring, 1);
}

/* ---- Registered buffer access ---- */

uint8_t *ioh_bufpool_get_reg_buf(ioh_bufpool_t *pool, uint32_t idx)
{
    if (pool == nullptr || pool->reg_buf_base == nullptr) {
        return nullptr;
    }
    if (idx >= pool->config.reg_buf_count) {
        return nullptr;
    }
    return pool->reg_buf_base + ((size_t)idx * pool->config.reg_buf_size);
}

/* ---- Registered file slot management ---- */

int ioh_bufpool_register_fd(ioh_bufpool_t *pool, struct io_uring *ring, int fd)
{
    if (pool == nullptr || ring == nullptr) {
        return -EINVAL;
    }
    if (!pool->files_registered || pool->file_slots == nullptr) {
        return -EINVAL;
    }
    if (fd < 0) {
        return -EBADF;
    }

    /* Find first free slot */
    int slot = -1;
    for (uint32_t i = 0; i < pool->config.reg_file_count; i++) {
        if (pool->file_slots[i] == -1) {
            slot = (int)i;
            break;
        }
    }
    if (slot < 0) {
        return -ENOSPC;
    }

    int ret = io_uring_register_files_update(ring, (unsigned)slot, &fd, 1);
    if (ret < 0) {
        return ret;
    }

    pool->file_slots[slot] = fd;
    return slot;
}

void ioh_bufpool_unregister_fd(ioh_bufpool_t *pool, struct io_uring *ring, int slot)
{
    if (pool == nullptr || ring == nullptr) {
        return;
    }
    if (!pool->files_registered || pool->file_slots == nullptr) {
        return;
    }
    if (slot < 0 || (uint32_t)slot >= pool->config.reg_file_count) {
        return;
    }

    int empty = -1;
    io_uring_register_files_update(ring, (unsigned)slot, &empty, 1);
    pool->file_slots[slot] = -1;
}

/* ---- Stats ---- */

uint32_t ioh_bufpool_ring_size(const ioh_bufpool_t *pool)
{
    if (pool == nullptr) {
        return 0;
    }
    return pool->config.ring_size;
}

uint32_t ioh_bufpool_buf_size(const ioh_bufpool_t *pool)
{
    if (pool == nullptr) {
        return 0;
    }
    return pool->config.buf_size;
}

uint32_t ioh_bufpool_reg_buf_count(const ioh_bufpool_t *pool)
{
    if (pool == nullptr) {
        return 0;
    }
    return pool->config.reg_buf_count;
}

uint32_t ioh_bufpool_reg_file_count(const ioh_bufpool_t *pool)
{
    if (pool == nullptr) {
        return 0;
    }
    return pool->config.reg_file_count;
}

/**
 * @file io_buffer.h
 * @brief Higher-level buffer pool managing provided buffer rings, registered
 *        buffers, and registered file descriptor slots for io_uring.
 */

#ifndef IOHTTP_CORE_BUFFER_H
#define IOHTTP_CORE_BUFFER_H

#include <liburing.h>
#include <stdbool.h>
#include <stdint.h>

/* ---- Configuration ---- */

typedef struct {
    uint32_t ring_size;      /**< Provided buffer ring entries (default 128, power of 2) */
    uint32_t buf_size;       /**< Size of each provided buffer (default 4096) */
    uint16_t bgid;           /**< Buffer group ID (default 0) */
    uint32_t reg_buf_count;  /**< Registered buffers (0 = disabled) */
    uint32_t reg_buf_size;   /**< Each registered buffer size (default 16384, TLS record) */
    uint32_t reg_file_count; /**< Registered file slots (0 = disabled) */
} io_bufpool_config_t;

/* ---- Opaque type ---- */

typedef struct io_bufpool io_bufpool_t;

/* ---- Lifecycle ---- */

/**
 * @brief Initialize configuration with sensible defaults.
 * @param cfg Configuration to initialize.
 */
void io_bufpool_config_init(io_bufpool_config_t *cfg);

/**
 * @brief Validate a buffer pool configuration.
 * @return 0 on success, negative errno on error.
 */
[[nodiscard]] int io_bufpool_config_validate(const io_bufpool_config_t *cfg);

/**
 * @brief Create a new buffer pool with the given configuration.
 *
 * Allocates memory for provided buffers and registered buffers based on
 * config. Does NOT register with io_uring yet.
 *
 * @return Non-null on success; nullptr on failure.
 */
[[nodiscard]] io_bufpool_t *io_bufpool_create(const io_bufpool_config_t *cfg);

/**
 * @brief Destroy a buffer pool and release all resources.
 * @param pool Pool to destroy (nullptr-safe).
 * @param ring io_uring instance to unregister from (may be nullptr if not registered).
 */
void io_bufpool_destroy(io_bufpool_t *pool, struct io_uring *ring);

/* ---- Registration (call after create, before accepting connections) ---- */

/**
 * @brief Set up the provided buffer ring with the io_uring instance.
 * @return 0 on success, negative errno on error.
 */
[[nodiscard]] int io_bufpool_register_ring(io_bufpool_t *pool, struct io_uring *ring);

/**
 * @brief Register pinned buffers for DMA acceleration.
 * @return 0 on success, negative errno on error.
 */
[[nodiscard]] int io_bufpool_register_bufs(io_bufpool_t *pool, struct io_uring *ring);

/**
 * @brief Register file descriptor slots with the kernel.
 * @return 0 on success, negative errno on error.
 */
[[nodiscard]] int io_bufpool_register_files(io_bufpool_t *pool, struct io_uring *ring);

/* ---- Provided buffer access ---- */

/**
 * @brief Get a pointer to buffer by ID (from CQE flags >> IORING_CQE_BUFFER_SHIFT).
 * @return Buffer pointer, or nullptr if buf_id is out of range.
 */
uint8_t *io_bufpool_get_buf(io_bufpool_t *pool, uint32_t buf_id);

/**
 * @brief Return a buffer to the ring after processing.
 */
void io_bufpool_return_buf(io_bufpool_t *pool, uint32_t buf_id);

/* ---- Registered buffer access ---- */

/**
 * @brief Get pointer to a registered buffer by index.
 * @return Buffer pointer, or nullptr if idx is out of range.
 */
uint8_t *io_bufpool_get_reg_buf(io_bufpool_t *pool, uint32_t idx);

/* ---- Registered file slot management ---- */

/**
 * @brief Allocate a slot and register fd.
 * @return Slot index (>= 0) on success, negative errno on error.
 */
[[nodiscard]] int io_bufpool_register_fd(io_bufpool_t *pool,
                                          struct io_uring *ring, int fd);

/**
 * @brief Unregister fd from its slot.
 */
void io_bufpool_unregister_fd(io_bufpool_t *pool, struct io_uring *ring,
                               int slot);

/* ---- Stats ---- */

uint32_t io_bufpool_ring_size(const io_bufpool_t *pool);
uint32_t io_bufpool_buf_size(const io_bufpool_t *pool);
uint32_t io_bufpool_reg_buf_count(const io_bufpool_t *pool);
uint32_t io_bufpool_reg_file_count(const io_bufpool_t *pool);

#endif /* IOHTTP_CORE_BUFFER_H */

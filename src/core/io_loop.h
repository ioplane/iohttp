/**
 * @file io_loop.h
 * @brief io_uring ring management wrapper for the iohttp event loop.
 */

#ifndef IOHTTP_CORE_LOOP_H
#define IOHTTP_CORE_LOOP_H

#include <liburing.h>
#include <stdbool.h>
#include <stdint.h>

/* ---- CQE user_data encoding ---- */

#define IO_OP_BITS                 8
#define IO_ENCODE_USERDATA(id, op) (((uint64_t)(id) << IO_OP_BITS) | (uint8_t)(op))
#define IO_DECODE_OP(ud)           ((uint8_t)((ud) & 0xFF))
#define IO_DECODE_ID(ud)           ((ud) >> IO_OP_BITS)

typedef enum : uint8_t {
    IO_OP_NOP = 0x00,
    IO_OP_ACCEPT = 0x01,
    IO_OP_RECV = 0x02,
    IO_OP_SEND = 0x03,
    IO_OP_TIMEOUT = 0x04,
    IO_OP_FILE = 0x05,
    IO_OP_TLS = 0x06,
    IO_OP_SIGNAL = 0x07,
    IO_OP_CLOSE = 0x08,
    IO_OP_SEND_ZC = 0x09,
    IO_OP_CANCEL = 0x0A,
    IO_OP_RECVMSG = 0x0B, /* UDP recvmsg for QUIC datagrams */
    IO_OP_SENDMSG = 0x0C, /* UDP sendmsg for QUIC datagrams */
} io_op_type_t;

/* ---- Ring configuration ---- */

typedef struct {
    uint32_t queue_depth;   /**< SQ/CQ ring depth (default 256) */
    uint32_t buf_ring_size; /**< Provided buffer ring entries (default 128, power of 2) */
    uint32_t buf_size;      /**< Each provided buffer size in bytes (default 4096) */
    bool defer_taskrun;     /**< IORING_SETUP_DEFER_TASKRUN + SINGLE_ISSUER (default true) */
} io_loop_config_t;

/* ---- Opaque loop handle ---- */

typedef struct io_loop io_loop_t;

/* ---- Lifecycle ---- */

/**
 * @brief Initialize configuration with sensible defaults.
 * @param cfg Configuration to initialize.
 */
void io_loop_config_init(io_loop_config_t *cfg);

/**
 * @brief Validate a loop configuration.
 * @return 0 on success, negative errno on error.
 */
[[nodiscard]] int io_loop_config_validate(const io_loop_config_t *cfg);

/**
 * @brief Create a new io_loop with the given configuration.
 * @return Non-null on success; nullptr on failure.
 */
[[nodiscard]] io_loop_t *io_loop_create(const io_loop_config_t *cfg);

/**
 * @brief Destroy a loop and release all resources.
 */
void io_loop_destroy(io_loop_t *loop);

/* ---- Ring access ---- */

/**
 * @brief Return a pointer to the underlying io_uring ring.
 */
struct io_uring *io_loop_ring(io_loop_t *loop);

/* ---- Event loop ---- */

/**
 * @brief Submit a NOP to verify the ring is operational.
 * @return 0 on success, negative errno on error.
 */
[[nodiscard]] int io_loop_submit_nop(io_loop_t *loop);

/**
 * @brief Submit pending SQEs, wait for at least 1 CQE, process completions.
 * @param loop  Event loop handle.
 * @param timeout_ms  Maximum wait time in milliseconds (0 = no timeout).
 * @return Number of CQEs processed, or negative errno on error.
 */
[[nodiscard]] int io_loop_run_once(io_loop_t *loop, uint32_t timeout_ms);

/**
 * @brief Signal the event loop to stop.
 */
void io_loop_stop(io_loop_t *loop);

/* ---- Timer management ---- */

/**
 * @brief Add a one-shot timer.
 * @return Timer ID (>= 0) on success, negative errno on error.
 */
[[nodiscard]] int io_loop_add_timer(io_loop_t *loop, uint32_t ms, void (*cb)(void *), void *data);

/**
 * @brief Cancel a pending timer.
 * @return 0 on success, negative errno on error.
 */
[[nodiscard]] int io_loop_cancel_timer(io_loop_t *loop, int timer_id);

/* ---- Provided buffer ring ---- */

/**
 * @brief Set up a provided buffer ring.
 * @return 0 on success, negative errno on error.
 */
[[nodiscard]] int io_loop_setup_buf_ring(io_loop_t *loop, uint16_t bgid, uint32_t buf_count,
                                         uint32_t buf_size);

/**
 * @brief Return a buffer to the provided buffer ring.
 */
void io_loop_return_buf(io_loop_t *loop, uint16_t bgid, uint32_t buf_id, uint8_t *buf,
                        uint32_t buf_size);

/**
 * @brief Get a pointer to a specific buffer in the ring.
 */
uint8_t *io_loop_get_buf(io_loop_t *loop, uint16_t bgid, uint32_t buf_id);

/* ---- Registered buffers ---- */

/**
 * @brief Register fixed buffers with the kernel.
 * @return 0 on success, negative errno on error.
 */
[[nodiscard]] int io_loop_register_buffers(io_loop_t *loop, struct iovec *iovs, unsigned nr);

/**
 * @brief Unregister previously registered buffers.
 * @return 0 on success, negative errno on error.
 */
[[nodiscard]] int io_loop_unregister_buffers(io_loop_t *loop);

/* ---- Registered files ---- */

/**
 * @brief Register fixed file descriptors with the kernel.
 * @return 0 on success, negative errno on error.
 */
[[nodiscard]] int io_loop_register_files(io_loop_t *loop, int *fds, unsigned nr);

/**
 * @brief Unregister previously registered file descriptors.
 * @return 0 on success, negative errno on error.
 */
[[nodiscard]] int io_loop_unregister_files(io_loop_t *loop);

#endif /* IOHTTP_CORE_LOOP_H */

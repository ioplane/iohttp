---
name: io-uring-patterns
description: Use when writing io_uring code in iohttp — SQE/CQE patterns, provided buffers, registered resources, multishot operations, linked timeouts, zero-copy send, REGISTER_RESTRICTIONS, DEFER_TASKRUN. MANDATORY for all src/core/ and src/net/ code.
---

# io_uring Development Patterns for iohttp

## Core Rule

io_uring is NOT a transport backend — it IS the core runtime. All I/O, timers, and signals go through the ring. No epoll fallback. Minimum kernel: 6.7+.

## Ring Setup

```c
// Minimum kernel: 6.7+ (CVE-2024-0582 avoidance, IOU_PBUF_RING_INC)
// DEFER_TASKRUN + SINGLE_ISSUER: preferred for HTTP (bursty traffic)
// SQPOLL: optional, NOT default (burns CPU core, mutually exclusive with DEFER_TASKRUN)
struct io_uring_params params = {
    .flags = IORING_SETUP_COOP_TASKRUN
           | IORING_SETUP_SINGLE_ISSUER
           | IORING_SETUP_DEFER_TASKRUN,
};

// SIGPIPE: MUST ignore at startup — wolfSSL can trigger via internal writes
signal(SIGPIPE, SIG_IGN);
```

## Ring Hardening (IORING_REGISTER_RESTRICTIONS)

**Why:** io_uring operations bypass seccomp BPF filters — they use shared memory ring, not
direct syscalls. Without restrictions, a compromised parser can issue arbitrary kernel
operations (OPENAT, CONNECT) through the existing ring fd.

```c
// After ring creation + buffer registration, BEFORE accepting connections:
struct io_uring_restriction restrictions[] = {
    { .opcode = IORING_RESTRICTION_SQE_OP, .sqe_op = IORING_OP_RECV },
    { .opcode = IORING_RESTRICTION_SQE_OP, .sqe_op = IORING_OP_SEND },
    { .opcode = IORING_RESTRICTION_SQE_OP, .sqe_op = IORING_OP_SEND_ZC },
    { .opcode = IORING_RESTRICTION_SQE_OP, .sqe_op = IORING_OP_ACCEPT },
    { .opcode = IORING_RESTRICTION_SQE_OP, .sqe_op = IORING_OP_TIMEOUT },
    { .opcode = IORING_RESTRICTION_SQE_OP, .sqe_op = IORING_OP_TIMEOUT_REMOVE },
    { .opcode = IORING_RESTRICTION_SQE_OP, .sqe_op = IORING_OP_LINK_TIMEOUT },
    { .opcode = IORING_RESTRICTION_SQE_OP, .sqe_op = IORING_OP_CANCEL },
    { .opcode = IORING_RESTRICTION_SQE_OP, .sqe_op = IORING_OP_SPLICE },
    { .opcode = IORING_RESTRICTION_SQE_OP, .sqe_op = IORING_OP_MSG_RING },
    { .opcode = IORING_RESTRICTION_SQE_OP, .sqe_op = IORING_OP_NOP },
    { .opcode = IORING_RESTRICTION_SQE_OP, .sqe_op = IORING_OP_CLOSE },
    { .opcode = IORING_RESTRICTION_REGISTER_OP, .register_op = IORING_REGISTER_RESTRICTIONS },
};
io_uring_register_restrictions(&ring, restrictions, ARRAY_SIZE(restrictions));
io_uring_enable_rings(&ring);
```

## SQE Submission Patterns

### Multishot Accept (one SQE accepts all connections)
```c
struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
io_uring_prep_multishot_accept(sqe, listen_fd, nullptr, nullptr, 0);
io_uring_sqe_set_data64(sqe, IO_ENCODE_USERDATA(0, IO_OP_ACCEPT));
// Handle IORING_CQE_F_MORE cessation → re-arm immediately
```

### Provided Buffer Rings (kernel picks buffer)
```c
// Setup: register buffer ring with fixed-size buffers
struct io_uring_buf_ring *br = io_uring_setup_buf_ring(ring, nentries, bgid, 0, &ret);
// Each buffer: add to ring
io_uring_buf_ring_add(br, buf, buf_size, bid, mask, idx);
io_uring_buf_ring_advance(br, count);

// Recv with provided buffer:
io_uring_prep_recv(sqe, fd, nullptr, 0, 0);  // nullptr buf — kernel picks
sqe->flags |= IOSQE_BUFFER_SELECT;
sqe->buf_group = bgid;
// CQE: buffer id in cqe->flags >> IORING_CQE_BUFFER_SHIFT

// ENOBUFS handling: when all buffers consumed, recv returns -ENOBUFS.
// Strategy: double-buffering — maintain two buffer groups, swap when one exhausts.
// Do NOT drop connections on -ENOBUFS; apply backpressure and retry.
```

### Linked Timeouts
```c
// SQE 1: the operation
struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
io_uring_prep_recv(sqe, fd, buf, len, 0);
sqe->flags |= IOSQE_IO_LINK;
io_uring_sqe_set_data64(sqe, IO_ENCODE_USERDATA(conn_id, IO_OP_RECV));

// SQE 2: linked timeout (auto-cancels recv if expired)
struct io_uring_sqe *tsqe = io_uring_get_sqe(ring);
io_uring_prep_link_timeout(tsqe, &ts, 0);
io_uring_sqe_set_data64(tsqe, IO_ENCODE_USERDATA(conn_id, IO_OP_TIMEOUT));
```

### Registered Buffers (DMA acceleration)
```c
// Register at startup — pins memory, avoids page table walks
struct iovec iovs[N];
io_uring_register_buffers(ring, iovs, N);
// Use: io_uring_prep_read_fixed / io_uring_prep_write_fixed
io_uring_prep_write_fixed(sqe, fd, buf, len, 0, buf_index);
```

### Registered Files (skip fd table lookup)
```c
int fds[N];
io_uring_register_files(ring, fds, N);
// Use with IOSQE_FIXED_FILE flag
sqe->flags |= IOSQE_FIXED_FILE;
sqe->fd = registered_index;  // NOT the actual fd
```

### Zero-Copy Send (SEND_ZC)
```c
// Threshold heuristic:
// - Regular SEND: payloads < 2 KiB (headers, small JSON)
// - SEND_ZC: payloads > 2 KiB (large responses, streaming)
// - splice: static files

io_uring_prep_send_zc(sqe, fd, buf, len, 0, 0);
// MUST handle notification CQE (IORING_CQE_F_NOTIF)
// Don't free buffer until notification received
// SEND_ZC generates 2 CQEs: completion + buffer notification

// Use IORING_SEND_ZC_REPORT_USAGE to detect kernel fallback to copy on loopback
sqe->ioprio |= IORING_SEND_ZC_REPORT_USAGE;
```

### Static File Serving (splice / sendfile path)
```c
// splice: pipe → socket, zero-copy
io_uring_prep_splice(sqe, file_fd, offset, pipe_wr, -1, len, SPLICE_F_MOVE);
// Then splice pipe_rd → socket_fd
```

## CQE Dispatch

### user_data Encoding
```c
// Pack operation type into user_data for fast CQE discrimination
#define IO_OP_BITS    8
#define IO_CONN_BITS  56

#define IO_ENCODE_USERDATA(conn_id, op)  (((uint64_t)(conn_id) << IO_OP_BITS) | (uint8_t)(op))
#define IO_DECODE_OP(ud)                 ((uint8_t)((ud) & 0xFF))
#define IO_DECODE_CONN(ud)               ((ud) >> IO_OP_BITS)

typedef enum : uint8_t {
    IO_OP_ACCEPT   = 0x01,
    IO_OP_RECV     = 0x02,
    IO_OP_SEND     = 0x03,
    IO_OP_TIMEOUT  = 0x04,
    IO_OP_FILE     = 0x05,
    IO_OP_TLS      = 0x06,
    IO_OP_SIGNAL   = 0x07,
    IO_OP_CLOSE    = 0x08,
    IO_OP_SEND_ZC  = 0x09,
} io_op_type_t;
```

### CQE Processing Loop
```c
unsigned head;
struct io_uring_cqe *cqe;
io_uring_for_each_cqe(ring, head, cqe) {
    uint8_t op = IO_DECODE_OP(cqe->user_data);
    uint64_t conn_id = IO_DECODE_CONN(cqe->user_data);

    switch (op) {
    case IO_OP_ACCEPT:
        // Handle new connection
        // Check IORING_CQE_F_MORE for multishot continuation
        if (!(cqe->flags & IORING_CQE_F_MORE)) {
            // Re-arm multishot accept
        }
        break;
    case IO_OP_RECV:
        // Check provided buffer id: cqe->flags >> IORING_CQE_BUFFER_SHIFT
        // Handle -ENOBUFS: apply backpressure, do NOT drop connection
        break;
    case IO_OP_SEND_ZC:
        // Handle both CQEs: completion and IORING_CQE_F_NOTIF (buffer release)
        if (cqe->flags & IORING_CQE_F_NOTIF) {
            // Buffer notification — safe to free/reuse send buffer now
            break;
        }
        // Regular completion — send succeeded but buffer still pinned
        break;
    // ...
    }
}
io_uring_cq_advance(ring, count);
```

## Multishot Accept Backpressure

When connection pool is exhausted:
1. Stop re-arming multishot accept
2. Log warning
3. When connections free up, re-arm
4. Never silently drop — always explicit backpressure

## Multishot Recv

For long-lived connections (WebSocket, SSE):
```c
io_uring_prep_recv_multishot(sqe, fd, nullptr, 0, 0);
sqe->flags |= IOSQE_BUFFER_SELECT;
sqe->buf_group = bgid;
// Multiple CQEs per SQE — handle IORING_CQE_F_MORE
```

## Timeouts as Ring Operations

ALL timeouts via io_uring — never sleep/timer_create in hot path:
- Accept backlog timeout
- PROXY header read timeout
- TLS handshake timeout
- Request header timeout
- Request body timeout
- Keep-alive idle timeout
- WebSocket ping/pong liveness
- Graceful drain/shutdown timeout

## Buffer Ownership Rules

1. **Provided buffers**: kernel owns until CQE delivered, then application owns until returned to ring
2. **Registered buffers**: application owns, pinned for DMA — don't free while registered
3. **TLS buffers**: separate ciphertext (io_uring recv target) and plaintext (wolfSSL output) ownership
4. **Zero-copy send**: don't free until IORING_CQE_F_NOTIF received
5. **ENOBUFS**: when provided buffer ring is exhausted, recv returns -ENOBUFS; use double-buffering (two buffer groups, swap on exhaust)

## Debug/Trace Counters

Track per-worker:
- Ring operations submitted/completed
- CQE errors by type
- Buffer ring stats (allocated/returned/exhausted)
- Zero-copy send usage (vs regular send, vs kernel fallback)
- Timeout fires
- Multishot re-arms
- ENOBUFS events

## CRITICAL: Error Handling

`cqe->res < 0` is `-errno`. io_uring does NOT use global `errno`.

| Error | Meaning | Action |
|-------|---------|--------|
| `-ECANCELED` | Operation cancelled (timeout or explicit) | Cleanup, don't retry |
| `-ETIME` | Linked timeout expired | Close idle connection |
| `-EPIPE` | Peer reset / write to closed socket | Close connection |
| `-ENOBUFS` | Provided buffer pool exhausted | Backpressure, retry later |
| `-EAGAIN` | Transient | Retry with POLL_FIRST |
| `res == 0` on recv | Peer closed (EOF) | Graceful close |
| `0 < res < requested` | Short read/write | Handle partial I/O |

**Every CQE must be handled.** Ignoring errors = silent data loss or leaks.

## CRITICAL: fd Close Ordering

```
1. Cancel all pending ops on fd  (IORING_OP_ASYNC_CANCEL)
2. Wait for ALL CQE (including -ECANCELED)
3. Close fd  (IORING_OP_CLOSE or close())
4. Free connection structures
```

**WRONG:** close fd while ops are in-flight = kernel use-after-free.
Track in-flight op count per connection. Only close when count reaches 0.

## CRITICAL: Send Serialization

**ONE active send per TCP connection.** Kernel may reorder concurrent sends.

Per-connection send queue with bounded depth. Activate next send only after CQE.
For ordered multi-buffer sends: `IOSQE_IO_LINK` chains.

## Memory Domains

| Domain | Lifetime | Examples |
|--------|----------|----------|
| Control plane | Connection lifetime | Connection state, parser state, router match |
| Data plane RX | CQE → parse → return | Provided buffer rings |
| Data plane TX | Enqueue → CQE/NOTIF → return | Response buffers |
| TLS cipher | Connection lifetime | cipher_in/cipher_out (io_uring targets) |
| TLS plain | Single operation | wolfSSL_read output, wolfSSL_write input |
| HTTP/2 frames | Session lifetime | nghttp2 output buffer |

- **RLIMIT_MEMLOCK**: registered buffers count against memlock limit
- **send_zc**: buffer pinned until `IORING_CQE_F_NOTIF`
- **Provided buffers**: kernel owns until CQE, then app owns until returned to ring

## Performance

- **SQE batching**: NEVER submit per-SQE. Batch 16-64 minimum.
- **CQE batching**: `io_uring_for_each_cqe` + `io_uring_cq_advance`, not single wait.
- **DEFER_TASKRUN**: completions only on `io_uring_enter(GETEVENTS)`.
- **Ring-per-worker + CPU pinning**: cache locality, no contention.
- **NUMA**: allocate ring memory on local node.

## Anti-patterns (NEVER DO)

| Anti-pattern | Fix |
|--------------|-----|
| Blocking syscall in CQE handler | Offload to thread pool or io_uring op |
| Mixed sync/async on same fd | All ops through ring |
| Multiple outstanding sends on TCP | Per-conn send queue |
| Unbounded queues | Fixed-size with backpressure |
| Ignoring CQE_F_MORE | Always check, re-arm multishot |
| WANT_READ/WANT_WRITE as fatal | Normal — arm recv/send, resume |
| Close fd before all CQE | Cancel → wait → close |
| Submit per-SQE | Accumulate, batch submit |

## Library Integration

- **picohttpparser**: streaming parse from provided buffers, zero-copy header pointers
- **nghttp2**: buffer-based API (`nghttp2_session_mem_recv2`/`mem_send2`), no socket callbacks — app feeds raw bytes, gets output buffer
- **ngtcp2 + nghttp3**: QUIC transport + HTTP/3, buffer-based same pattern as nghttp2
- **wslay**: WebSocket frame codec, I/O agnostic — app manages send/recv
- **yyjson**: in-place parse from recv buffers, zero-copy where possible

## Testing Patterns

- **Mock CQE delivery**: inject CQE sequences to test state machines deterministically
- **Negative injection**: `-ECANCELED`, `-EPIPE`, `-ENOBUFS`, short reads
- **socketpair**: integration tests without real network
- **In-flight tracking**: assert op count == 0 after cleanup (detect leaks)

## context7 Documentation

Fetch up-to-date liburing API docs:
- context7 ID: `/axboe/liburing`

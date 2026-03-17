---
name: modern-c23
description: Use when writing modern C23 code for iohttp, especially io_uring handlers, TLS callbacks, HTTP parsers, and safety-critical paths. Covers required language features, checked arithmetic, and patterns to avoid.
---

# Modern C23

## Required Features

- Prefer `nullptr` over `NULL`.
- Use `[[nodiscard]]` on public error-returning functions.
- Use `_Static_assert` for ABI, enum, and limit invariants.
- Use `constexpr` for compile-time constants where it improves clarity.
- Use `<stdckdint.h>` when input-derived arithmetic can overflow.
- Use `bool` keyword directly (not `_Bool` or `<stdbool.h>`).

## iohttp-Specific Guidance

- Keep io_uring CQE handlers compact and non-blocking.
- Keep TLS callback buffers explicitly sized and documented.
- Keep connection state machine transitions explicit with enum values.
- Optimize only after scalar correctness and tests are stable.
- Use `memset_explicit()` for secret zeroing (C23), `explicit_bzero()` as fallback.

## Avoid

- `_BitInt` — no use case in iohttp.
- `auto` type inference — hurts readability in network code.
- `#embed` — except for static file serving.
- Locale-sensitive helpers for protocol parsing.
- Allocation-heavy convenience wrappers in hot paths.
- Non-portable SIMD assumptions without scalar fallback.

## Workflow

When writing new code:
1. Choose the narrowest data type that still fits protocol limits.
2. Guard size arithmetic with checked operations.
3. Keep public API contracts explicit and `[[nodiscard]]`.
4. Add `_Static_assert` for assumptions that must never silently drift.
5. Use `typeof` for type-safe macros when appropriate.

## References

- `references/c23-checklist.md`

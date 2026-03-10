# iohttp Backlog

## Container Updates

### BL-1: Update library versions in Containerfile

**Priority:** High
**Status:** Done (Containerfile updated, image rebuild pending)

Containerfile ARG versions updated in commit `9be21ee`:

| Library | Old | New | Status |
|---------|-----|-----|--------|
| wolfSSL | 5.8.2 | **5.8.4** | Updated |
| nghttp2 | 1.65.0 | **1.68.0** | Updated |
| ngtcp2 | 1.12.0 | **1.21.0** | Updated |
| nghttp3 | 1.9.0 | **1.15.0** | Updated |
| cppcheck | 2.18.3 | **2.20.0** | Updated |

**Remaining:**
1. Rebuild: `podman build -t localhost/iohttp-dev:latest -f deploy/podman/Containerfile .`
2. Recreate container: `podman rm iohttp-dev && podman run -d --name iohttp-dev ...`

---

### BL-2: Review glibc release notes

**Priority:** Medium
**Status:** Done (reviewed 2026-03-08)

Container runs glibc 2.39 (OL10.1). Reviewed 2.40–2.43 release notes.

#### glibc 2.40 (2024-07)

- `_ISOC23_SOURCE` macro added (we use `_GNU_SOURCE` which covers it)
- `<stdbit.h>` type-generic macros improved for GCC 14.1+
- **FORTIFY_SOURCE enhanced for Clang** — relevant for our Clang 22 builds
- 5 CVEs fixed (iconv, nscd)
- No io_uring changes, no `<stdckdint.h>` changes

#### glibc 2.41 (2025-01)

- C23 trig functions (acospi, sinpi, etc.) — not needed for iohttp
- `_ISOC2Y_SOURCE` macro for draft C2Y
- **`sched_setattr`/`sched_getattr`** — useful for worker thread SCHED_DEADLINE (future)
- **`dlopen` no longer makes stack executable** — security improvement
- **`abort()` now async-signal-safe** — good for signal handlers
- DNS stub resolver `strict-error` option
- rseq ABI extension (kernel 6.3+)
- 1 CVE fixed (assert buffer overflow)

#### glibc 2.42 (2025-07)

- **`pthread_gettid_np`** — cleaner than `syscall(SYS_gettid)`, consider adopting
- **Arbitrary baud rate** via redefined `speed_t` — not relevant
- **malloc tcache caches large blocks** — perf improvement for free (we use mimalloc though)
- **Stack guard pages** via `MADV_GUARD_INSTALL` in `pthread_create` — free security
- **`termio.h` removed** — verify we don't use it (we don't)
- TX lock elision deprecated — we don't use it
- 4 CVEs fixed

#### glibc 2.43 (2026-01)

- **C23 const-preserving macros** — `strchr()` et al. propagate const; may break code assuming mutable returns. **Test with our codebase before upgrading.**
- NPTL trylock optimization for contended mutexes
- No io_uring changes

#### Upgrade decision

**No upgrade needed now.** Current glibc 2.39 is sufficient for iohttp.

Useful features if we upgrade later:
- `pthread_gettid_np` (2.42) — cleaner tid retrieval
- `sched_setattr` (2.41) — SCHED_DEADLINE for workers
- Stack guard pages (2.42) — free security
- FORTIFY_SOURCE Clang improvements (2.40)

**Risk:** C23 const-preserving macros in 2.43 may require code changes. Test before upgrading.

**Path:** OL10 dnf updates may ship newer glibc; check `dnf info glibc` periodically.

---

## Future Improvements

### BL-3: Pin mold version in Containerfile

**Status:** Done (pinned to v2.40.4 in commit `9be21ee`)

### BL-4: Add sfparse version tracking

sfparse (RFC 9651 structured fields) has no GitHub releases, only tags. Current: v0.3.0. Monitor for updates.

---

## Sprint Plans

### BL-5: Sprint 13 — Critical Fixes & Sanitizer Green (iohttp)

**Priority:** P0 — CRITICAL memory safety bugs
**Status:** DONE (merged to main)
**Plan:** `docs/plans/2026-03-10-sprint-13-critical-fixes.md`

4 tasks — all completed: HTTP/2 UAF fix, router stack-use-after-return fix, PVS warnings, ASan green (46/46).

---

### BL-7: Sprint 14 — Project Hardening

**Priority:** P1 — production-grade open-source standards
**Status:** DONE (merged to main)
**Plan:** `docs/plans/2026-03-10-sprint-14-project-hardening.md`

7 tasks — all completed: .clangd, .editorconfig, SECURITY.md, GitHub infra, coverage preset, SPDX headers, quality pipeline.

---

### BL-8: Sprints 15–19 — Feature Roadmap & liboas Integration

**Priority:** P0/P1/P2 — remaining features for 0.1.0 release
**Status:** Sprint 15 DONE, Sprints 16-19 planned
**Plan:** `docs/plans/2026-03-10-sprints-15-19-roadmap.md`

**Sprint 15 (P0):** DONE — Health check endpoints, per-route timeout configuration
**Sprint 16 (P1):** Planned — Set-Cookie builder (RFC 6265bis), Vary header, host-based virtual routing
**Sprint 17 (P1):** Planned — Streaming request body (chunked input), circuit breaker middleware
**Sprint 18 (P1):** Planned — liboas adapter — OpenAPI spec generation, Scalar UI, request validation
**Sprint 19 (P2):** Planned — W3C trace context propagation hooks, SIGHUP config hot reload

**Dependency chain:** ~~13 → 15~~ (done) → 16 → 17 → 18 → 19. Sprint 14 is done (independent).

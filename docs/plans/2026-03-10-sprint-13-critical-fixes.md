# Sprint 13: Critical Fixes & Sanitizer Green

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Fix the two memory-safety bugs found by ASan (heap-use-after-free in HTTP/2, stack-use-after-return in router), normalize PVS warnings in tests, and achieve 46/46 ASan-green test suite.

**Architecture:** Ownership transfer for HTTP/2 response body data; arena/request-scoped copy for normalized path params in router; PVS inline suppression or code fix in test files.

**Tech Stack:** C23, nghttp2, Unity tests, ASan+UBSan, PVS-Studio.

**Skills (MANDATORY):**
- `iohttp-architecture` — naming, connection lifecycle, ownership model
- `io-uring-patterns` — CQE handling, send serialization

**Source:** Analysis report `docs/tmp/repository-analysis-2026-03-09.md`

**Build/test:**
```bash
cmake --preset clang-debug && cmake --build --preset clang-debug && ctest --preset clang-debug
cmake --preset clang-asan && cmake --build --preset clang-asan && ctest --preset clang-asan
```

**Existing files (read before implementing):**
- `src/http/ioh_http2.c` — HTTP/2 session, `submit_response()`, `resp_data_read_cb` (Task 1)
- `src/http/ioh_response.h` / `ioh_response.c` — response body ownership (Task 1)
- `src/router/ioh_router.c` — `ioh_router_dispatch()`, `normalized[]` stack buffer (Task 2)
- `src/router/ioh_radix.c` — `ioh_radix_lookup()`, param value pointers (Task 2)
- `src/core/ioh_server.c` — request param copy from match (Task 2)
- `tests/integration/test_limits.c:95` — PVS V590 (Task 3)
- `tests/unit/test_ioh_log.c:153` — PVS V618 (Task 3)

---

## Task 1: Fix heap-use-after-free in HTTP/2 response body

**Severity:** CRITICAL — UB in runtime, potential crash/corruption under load.

**Root cause:**
1. `submit_response()` saves pointer to `resp->body` in `h2_stream_data.body` (`ioh_http2.c:253`)
2. `ioh_response_destroy(&resp)` frees `resp->body` (`ioh_http2.c:475`, `ioh_response.c:72`)
3. nghttp2 later calls `resp_data_read_cb` which reads freed `rd->body + rd->offset` (`ioh_http2.c:171`)

**Fix strategy:** Transfer body ownership to `h2_stream_data` — either move the pointer (set `resp->body = NULL` before destroy) or copy the body into stream-owned memory.

**Files:**
- Modify: `src/http/ioh_http2.c` — `submit_response()` transfers body ownership to `rd`
- Modify: `src/http/ioh_response.c` — ensure `destroy` handles `body == NULL` gracefully
- Modify: `tests/unit/test_ioh_http2.c` — add regression test for body lifetime

**Step 1: Read the affected files**

Read `src/http/ioh_http2.c` (focus on `submit_response`, `resp_data_read_cb`, `h2_stream_data` struct), `src/http/ioh_response.c` (focus on `ioh_response_destroy`), and `src/http/ioh_response.h`.

**Step 2: Write the failing test**

In `tests/unit/test_ioh_http2.c`, add a test that creates a response with a heap-allocated body, submits it to the HTTP/2 data provider, destroys the response, then verifies the data callback can still read the body. Under ASan this should crash before the fix and pass after.

```c
void test_http2_body_lifetime_after_response_destroy(void)
{
    /* Setup: create response with body, submit to h2 stream, destroy response */
    /* Verify: data read callback still returns correct body data */
    /* This test catches the UAF — under ASan it should crash without the fix */
}
```

**Step 3: Run test to verify it fails under ASan**

```bash
cmake --preset clang-asan && cmake --build --preset clang-asan
ctest --preset clang-asan -R test_ioh_http2 --output-on-failure
```
Expected: FAIL (heap-use-after-free)

**Step 4: Fix — transfer body ownership in submit_response()**

In `src/http/ioh_http2.c`, modify `submit_response()`:

```c
/* Instead of: rd->body = resp->body; */
/* Transfer ownership: */
rd->body = resp->body;
rd->body_len = resp->body_len;
resp->body = nullptr;      /* prevent double-free in ioh_response_destroy */
resp->body_len = 0;
```

In `src/http/ioh_response.c`, ensure `ioh_response_destroy()` handles `body == NULL`:

```c
/* Already safe if we check: */
if (resp->body != nullptr) {
    free(resp->body);
    resp->body = nullptr;
}
```

Add cleanup in the HTTP/2 stream close path to free `rd->body` when the stream ends.

**Step 5: Run ASan tests to verify fix**

```bash
ctest --preset clang-asan -R "test_ioh_http2|test_http2_server" --output-on-failure
```
Expected: PASS (both tests)

**Step 6: Commit**

```bash
git add src/http/ioh_http2.c src/http/ioh_response.c tests/unit/test_ioh_http2.c
git commit -m "fix(http2): transfer response body ownership to stream data provider

Fixes heap-use-after-free: nghttp2 data callback was reading freed
response body. Now submit_response() transfers body pointer ownership
to h2_stream_data, preventing double-free and use-after-free."
```

---

## Task 2: Fix stack-use-after-return in router path params

**Severity:** HIGH — incorrect param values, UB in middleware/handlers.

**Root cause:**
1. `ioh_router_dispatch()` normalizes path into `char normalized[IOH_MAX_URI_SIZE]` on stack (`ioh_router.c:439`)
2. `ioh_radix_lookup()` stores `param.value` as pointer into the passed path buffer (`ioh_radix.c:552,:582`)
3. These pointers are copied into `ioh_route_match_t` and returned
4. After `ioh_router_dispatch()` returns, `normalized` is invalid but pointers remain

**Fix strategy:** Copy param values into a stable buffer owned by the request/match. Either:
- (A) Use request-owned arena/buffer for param strings, or
- (B) Copy param values into `ioh_route_match_t` fixed-size buffers before returning

**Files:**
- Modify: `src/router/ioh_router.h` — add param value storage to `ioh_route_match_t`
- Modify: `src/router/ioh_router.c` — copy param values before returning from dispatch
- Modify: `tests/unit/test_ioh_router.c` — add regression test

**Step 1: Read affected files**

Read `src/router/ioh_router.c` (focus on `ioh_router_dispatch`, the `normalized` buffer, and how match params are populated), `src/router/ioh_radix.c` (how `param.value` is set), `src/router/ioh_router.h` (`ioh_route_match_t`).

**Step 2: Write the failing test**

In `tests/unit/test_ioh_router.c`, add a test that dispatches a route with params, then verifies param values after the dispatch function returns. Under ASan with `detect_stack_use_after_return=1`, this should fail before the fix.

```c
void test_router_param_lifetime_after_dispatch(void)
{
    /* Setup: router with "/users/:id" route */
    /* Dispatch: "/users/42" */
    /* Verify: match.params[0].value == "42" AFTER dispatch returns */
    /* ASan should catch stack-use-after-return without the fix */
}
```

**Step 3: Run test to verify it fails under ASan**

```bash
ctest --preset clang-asan -R test_ioh_router --output-on-failure
```
Expected: FAIL (stack-use-after-return)

**Step 4: Fix — copy param values into match-owned storage**

Add a fixed buffer to `ioh_route_match_t` for param value storage:

```c
/* In ioh_router.h, add to ioh_route_match_t: */
#define IOH_MAX_PARAM_STORAGE 512  /* total storage for all param values */

typedef struct {
    /* ... existing fields ... */
    char   param_storage[IOH_MAX_PARAM_STORAGE]; /**< stable storage for param values */
    size_t param_storage_used;
} ioh_route_match_t;
```

In `ioh_router_dispatch()`, after `ioh_radix_lookup()`, copy each param value into `param_storage`:

```c
/* After radix lookup, before returning match: */
size_t offset = 0;
for (size_t i = 0; i < match->num_params; i++) {
    size_t vlen = match->params[i].value_len;
    if (offset + vlen > IOH_MAX_PARAM_STORAGE) {
        return -ENOMEM;
    }
    memcpy(match->param_storage + offset, match->params[i].value, vlen);
    match->params[i].value = match->param_storage + offset;
    offset += vlen;
}
match->param_storage_used = offset;
```

**Step 5: Run ASan tests**

```bash
ctest --preset clang-asan -R test_ioh_router --output-on-failure
```
Expected: PASS

**Step 6: Commit**

```bash
git add src/router/ioh_router.h src/router/ioh_router.c tests/unit/test_ioh_router.c
git commit -m "fix(router): copy param values to match-owned storage

Fixes stack-use-after-return: ioh_router_dispatch() stored param value
pointers into a stack-local normalized[] buffer. After return, the
pointers were dangling. Now copies param values into ioh_route_match_t
param_storage buffer before returning."
```

---

## Task 3: Normalize PVS-Studio warnings in tests

**Severity:** MEDIUM — CI pipeline false-red, masks real regressions.

**Root cause:** Two PVS findings in test files:
- `tests/integration/test_limits.c:95` — V590 (suspicious expression)
- `tests/unit/test_ioh_log.c:153` — V618 (dangerous function call)

**Fix strategy:** Examine each finding. If the test code is genuinely wrong, fix it. If it's a false positive or intentional test pattern, add inline `// -V590` / `// -V618` suppressions.

**Files:**
- Modify: `tests/integration/test_limits.c` — fix or suppress V590
- Modify: `tests/unit/test_ioh_log.c` — fix or suppress V618

**Step 1: Read the flagged test files**

Read both files, focusing on the flagged lines. Determine if the code is actually buggy or if it's a valid test pattern.

**Step 2: Fix or suppress**

For each finding, either:
- Fix the code if it's genuinely wrong
- Add `// -V590` or `// -V618` at the exact line (PVS inline suppression)

**Step 3: Run quality pipeline**

```bash
./scripts/quality.sh
```
Expected: `PASS: 6, FAIL: 0`

**Step 4: Commit**

```bash
git add tests/integration/test_limits.c tests/unit/test_ioh_log.c
git commit -m "fix(tests): resolve PVS-Studio V590/V618 warnings in test files

Normalize quality pipeline to green. V590 in test_limits.c was
[description]. V618 in test_ioh_log.c was [description]."
```

---

## Task 4: Verify full ASan suite green (46/46)

**Goal:** Confirm all 46 tests pass under ASan after Tasks 1-3.

**Step 1: Full ASan build + test**

```bash
cmake --preset clang-asan
cmake --build --preset clang-asan
ctest --preset clang-asan --output-on-failure
```
Expected: `46/46 tests passed`

**Step 2: Full quality pipeline**

```bash
./scripts/quality.sh
```
Expected: `PASS: 6, FAIL: 0, SKIP: 0`

**Step 3: Commit (if any additional fixes were needed)**

Only if additional changes were required to get to green.

---

## Acceptance Criteria

- [ ] `ctest --preset clang-asan` — 46/46 PASS (zero ASan/UBSan errors)
- [ ] `ctest --preset clang-debug` — 46/46 PASS
- [ ] `./scripts/quality.sh` — PASS: 6, FAIL: 0
- [ ] HTTP/2 body data provider reads valid memory after response destroy
- [ ] Router path params are valid after `ioh_router_dispatch()` returns
- [ ] No regression in existing tests

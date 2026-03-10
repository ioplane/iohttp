# S9 Design: Route Metadata + Introspection API

## Problem

Route metadata is unstructured (`void *metadata` in radix nodes, `void *oas_operation` in opts).
Introspection (`ioh_router_walk`) returns raw `void *` — consumers must know the type.
No way to attach summary, tags, params description, deprecated flag to routes.
liboas integration requires structured metadata, but iohttp should not depend on liboas.

## Decisions

1. **Hybrid metadata (C)** — `ioh_route_meta_t` with basic fields (summary, tags, deprecated, params) + `void *oas_operation` for liboas
2. **Params in meta** — `ioh_param_meta_t` array (name, in, required, description); full schema/style/explode stays in liboas
3. **No response metadata** — response schemas are liboas territory
4. **Via opts.meta pointer** — `const ioh_route_meta_t *meta` field in `ioh_route_opts_t`; aspect-pointer pattern for future extensibility

## New Types

### `src/router/ioh_route_meta.h`

```c
typedef enum : uint8_t {
    IOH_PARAM_PATH,
    IOH_PARAM_QUERY,
    IOH_PARAM_HEADER,
    IOH_PARAM_COOKIE,
} ioh_param_in_t;

typedef struct {
    const char *name;           // "id"
    const char *description;    // "User identifier"
    ioh_param_in_t in;           // IOH_PARAM_PATH
    bool required;              // true (always true for path params)
} ioh_param_meta_t;

typedef struct {
    const char *summary;            // "Get user by ID"
    const char *description;        // longer text, nullable
    const char *const *tags;        // {"users", "admin", nullptr} — nullptr-terminated
    bool deprecated;                // false
    const ioh_param_meta_t *params;  // array of param descriptors
    uint32_t param_count;           // number of params
} ioh_route_meta_t;

const char *ioh_param_in_name(ioh_param_in_t in);
```

## Changes to Existing Types

### `ioh_route_opts_t` (ioh_router.h)

```c
typedef struct {
    const ioh_route_meta_t *meta;  // NEW — nullable, for introspection
    void *oas_operation;          // for liboas compiled operation binding
    uint32_t permissions;         // bitmask for auth
    bool auth_required;
} ioh_route_opts_t;
```

### `ioh_radix_node_t` (ioh_radix.h) — internal

Add separate `meta` pointer so `set_meta()` can update it without replacing opts:

```c
typedef struct ioh_radix_node_t {
    // ... existing fields ...
    void *handler;
    void *metadata;                 // opts pointer (unchanged)
    const ioh_route_meta_t *meta;    // NEW — extracted from opts or set via set_meta
    // ...
} ioh_radix_node_t;
```

### `ioh_route_info_t` (ioh_route_inspect.h)

Replace `void *metadata` with typed fields:

```c
typedef struct {
    ioh_method_t method;
    const char *pattern;
    ioh_handler_fn handler;
    const ioh_route_opts_t *opts;    // full opts (may be nullptr)
    const ioh_route_meta_t *meta;    // convenience: opts->meta or set_meta value
} ioh_route_info_t;
```

### `ioh_router_set_metadata()` -> `ioh_router_set_meta()`

Rename and retype:

```c
[[nodiscard]] int ioh_router_set_meta(ioh_router_t *r, ioh_method_t method,
                                     const char *pattern,
                                     const ioh_route_meta_t *meta);
```

Sets `node->meta` directly. Does NOT replace opts.

## Storage Model

- Opts pointer stored as `void *metadata` in radix node (unchanged, no copy)
- Meta pointer stored separately in `node->meta`
- On insert: if `opts && opts->meta`, set `node->meta = opts->meta`
- On `set_meta()`: set `node->meta` directly
- On walk: `info.opts = node->metadata`, `info.meta = node->meta`
- On dispatch: `match.opts = node->metadata` (unchanged)
- **Lifetime requirement**: meta/opts pointers must outlive the router (string literals, static storage, or heap-allocated)

## Registration API (no new functions)

```c
// Via existing _with() — compound literal
ioh_router_get_with(router, "/users/:id", get_user, &(ioh_route_opts_t){
    .meta = &(ioh_route_meta_t){
        .summary = "Get user by ID",
        .tags = (const char *[]){"users", nullptr},
        .params = (ioh_param_meta_t[]){{
            .name = "id", .in = IOH_PARAM_PATH,
            .required = true, .description = "User UUID",
        }},
        .param_count = 1,
    },
    .auth_required = true,
});

// Via set_meta() after registration
static const ioh_route_meta_t list_meta = { .summary = "List users", .tags = (const char *[]){"users", nullptr} };
ioh_router_set_meta(router, IOH_METHOD_GET, "/users", &list_meta);

// Group routes
ioh_group_get_with(api, "/users/:id", get_user, &(ioh_route_opts_t){
    .meta = &(ioh_route_meta_t){ .summary = "Get user" },
});
```

## Introspection

```c
int print_route(const ioh_route_info_t *info, void *ctx) {
    if (info->meta) {
        printf("%-6s %-30s %s%s\n",
               method_str(info->method), info->pattern,
               info->meta->summary ? info->meta->summary : "(no summary)",
               info->meta->deprecated ? " [DEPRECATED]" : "");
    }
    return 0;
}
ioh_router_walk(router, print_route, nullptr);
```

## Files Changed

| File | Action | Description |
|------|--------|-------------|
| `src/router/ioh_route_meta.h` | CREATE | `ioh_param_in_t`, `ioh_param_meta_t`, `ioh_route_meta_t`, `ioh_param_in_name()` |
| `src/router/ioh_route_meta.c` | CREATE | `ioh_param_in_name()` implementation |
| `src/router/ioh_router.h` | MODIFY | Add `const ioh_route_meta_t *meta` to `ioh_route_opts_t`, add include |
| `src/router/ioh_radix.h` | MODIFY | Add `const ioh_route_meta_t *meta` to `ioh_radix_node_t` |
| `src/router/ioh_radix.c` | MODIFY | Propagate meta on insert |
| `src/router/ioh_route_inspect.h` | MODIFY | `ioh_route_info_t`: replace `void *metadata` with `opts` + `meta` |
| `src/router/ioh_route_inspect.c` | MODIFY | Walk fills `info.opts`/`info.meta` from node; rename `set_metadata` -> `set_meta` |
| `src/router/ioh_router.c` | MODIFY | `router_add_route` extracts meta from opts and passes to radix |
| `tests/unit/test_ioh_route_inspect.c` | MODIFY | Update for new `ioh_route_info_t` fields, add meta tests |
| `tests/unit/test_ioh_route_meta.c` | CREATE | New test file for meta types and param_in_name |
| `CMakeLists.txt` | MODIFY | Add `ioh_route_meta.c` to library, add test target |

## Test Plan (~12 tests)

### New: `test_ioh_route_meta.c`
1. `test_param_in_name_path` — `ioh_param_in_name(IOH_PARAM_PATH)` returns "path"
2. `test_param_in_name_query` — returns "query"
3. `test_param_in_name_header` — returns "header"
4. `test_param_in_name_cookie` — returns "cookie"
5. `test_meta_summary_via_opts` — register with meta, walk reads summary
6. `test_meta_tags_nullptr_terminated` — tags array, iterate to nullptr
7. `test_meta_deprecated_flag` — deprecated=true visible in walk
8. `test_meta_params_path` — path param with name/in/required/description
9. `test_meta_params_multiple` — multiple params, verify count and contents
10. `test_meta_null_when_no_opts` — route without opts, meta is nullptr
11. `test_meta_set_meta_after_registration` — set_meta() updates meta without replacing opts
12. `test_meta_opts_and_meta_combined` — auth_required + meta both accessible

### Modified: `test_ioh_route_inspect.c`
- Update `collect_routes` for new `ioh_route_info_t` fields (opts/meta instead of metadata)
- Update `test_route_set_metadata` -> `test_route_set_meta` (new API name)
- Update `test_route_metadata_in_match` for new field names

## What Is NOT In Scope

- Response metadata (liboas territory)
- JSON Schema / validation (liboas territory)
- `oas_operation` type definition (liboas territory)
- `/debug/routes` endpoint (future sprint)
- Auto-extraction of params from pattern (YAGNI)
- Deep copy of meta/opts (caller ensures lifetime)
- Group-level meta inheritance (future, if needed)

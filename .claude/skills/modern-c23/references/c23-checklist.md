# C23 Checklist for iohttp

## Mandatory (use everywhere)

| Feature | Example |
|---------|---------|
| `nullptr` | `if (ptr == nullptr)` |
| `[[nodiscard]]` | `[[nodiscard]] int ioh_server_start(...)` |
| `constexpr` | `constexpr size_t MAX_HEADERS = 100;` |
| `bool` keyword | `bool is_connected = true;` |
| `_Static_assert` | `_Static_assert(sizeof(ioh_conn_t) <= 4096, "conn too large");` |

## Use when appropriate

| Feature | When |
|---------|------|
| `typeof` | Type-safe container macros |
| `[[maybe_unused]]` | Callback parameters, conditional compilation |
| `_Atomic` | Shared counters, flags between threads |
| `<stdckdint.h>` | Size/length arithmetic from untrusted input |
| Digit separators | Large constants: `1'000'000` |
| `unreachable()` | After exhaustive switch/enum handling |

## Avoid

| Feature | Reason |
|---------|--------|
| `auto` inference | Hurts readability in C |
| `_BitInt` | No use case in HTTP server |
| `#embed` | Only for static file module |

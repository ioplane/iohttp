# iohttp - Project Instructions for Codex

## Start Here

Read these in order before making non-trivial changes:

- `AGENTS.md`
- `CLAUDE.md`
- `docs/plans/2026-03-10-sprints-15-19-roadmap.md`
- `.claude/skills/ROADMAP.md`

## Core Working Rules

- Preserve the current public API shape in `include/iohttp/` unless the task explicitly requires an API change.
- Keep io_uring as the core runtime, not a backend. No epoll fallback.
- Keep wolfSSL Native API (not OpenSSL compat layer).
- Keep protocol layers separate: HTTP/1.1 (picohttpparser), HTTP/2 (nghttp2), HTTP/3 (ngtcp2+nghttp3).
- Default to strict behavior. Any lenient behavior must be explicit, documented, and covered by tests.
- Keep zero-copy outputs and avoid hidden allocations in hot paths.

## Code and Review Focus

- Public names: `ioh_*`
- Type suffix: `_t`
- Macro/enum prefix: `IOH_`
- Connection state machine: ACCEPTING → PROXY_HEADER → TLS → PROTOCOL_NEGOTIATION → HTTP_ACTIVE → WS/SSE/DRAINING → CLOSING
- Security focus: send serialization, fd close ordering, CQE error handling, constant-time crypto, secret zeroing

## Required Follow-Through

- Update unit tests when behavior changes.
- Update docs when layer boundaries, defaults, or public API semantics change.
- Keep `CLAUDE.md`, `CODEX.md`, `AGENTS.md`, and `.claude/skills/` aligned when repository rules change.

## Compiler Policy

- Keep both Clang and GCC lanes healthy for C23 work.
- Measured and published lanes must use explicit `-std=c23`.
- Do not mix GCC and Clang sanitizer objects, LTO objects, or FMV resolvers in one binary.
- Keep `-ffast-math` disabled.

## Local Skills

Repository-local skills live under `.claude/skills/`. They are the closest thing to project memory for repeated architecture and standards decisions, even when the active agent is not Claude.

## Required Utilities

For effective Codex work on this repository, keep these available on the host:

- `git` for branches, worktrees, history, and patch-oriented review
- `gh` with working auth, especially `gh api graphql`, for repository and PR automation
- `rg` (`ripgrep`) for fast code and path discovery
- `jq` for processing JSON output from GitHub APIs, tool output, and generated reports
- `python3` for small project-local automation and validation scripts
- `podman` for the required build/test/quality execution environment
- `uv` / `uvx` for optional MCP and helper tooling such as Serena
- `clangd` for local semantic C/C++ navigation when Serena or other LSP-based tooling is used

Useful but optional:

- `fd` for fast filename discovery
- `yq` for YAML inspection
- `hyperfine` for repeatable benchmark comparisons

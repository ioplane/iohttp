# Repository Guidelines

## Project Structure & Module Organization

`src/` contains implementation split by domain: `core` (io_uring event loop, workers, buffers), `net` (listener, accept, sockets, PROXY protocol), `http` (HTTP/1.1 picohttpparser, HTTP/2 nghttp2, HTTP/3 ngtcp2+nghttp3), `tls` (wolfSSL TLS 1.3, QUIC crypto, mTLS), `router` (radix-trie, route groups), `ws` (WebSocket, SSE), `static` (static files, #embed, SPA fallback), `middleware` (rate limiting, CORS, JWT, security headers). Public headers in `include/iohttp/`; internal declarations in `src/`. Unit tests in `tests/unit/`, integration tests in `tests/integration/`, fuzz targets in `tests/fuzz/`, benchmarks in `tests/bench/`. Development tooling in `scripts/` and `deploy/podman/`. Plans in `docs/plans/`; `docs/tmp/` is scratch space.

## Build, Test, and Development Commands

Develop inside the container:

```bash
podman build -t iohttp-dev:latest -f deploy/podman/Containerfile .
podman run --rm -it --security-opt seccomp=unconfined -v $(pwd):/workspace:Z localhost/iohttp-dev:latest
```

Primary workflow:

```bash
cmake --preset clang-debug
cmake --build --preset clang-debug
ctest --preset clang-debug
```

Use `cmake --build --preset clang-debug --target format` for formatting and `--target format-check` to verify. Run `./scripts/quality.sh` before submitting changes.

## Profiling Toolchain

The development image carries profiling/debug tools:

- `gdb`
- `valgrind`
- `uftrace`
- `ftracer` helpers (`frun`, `fresolve`, `/usr/local/lib/ftracer/ftracer.o`)
- `hyperfine`

## Required Host Utilities

Keep these available on the host:

- `git`
- `gh` with working `gh api graphql` authentication
- `rg` (`ripgrep`)
- `jq`
- `python3`
- `podman`

Strongly recommended:

- `uv` / `uvx`
- `clangd`
- `fd`
- `yq`
- `hyperfine`

## Documentation Style

Write English documentation first. Treat `docs/en/*` as authoritative and `docs/ru/*` as the translation/adaptation layer.

Documentation must use strict technical language:

- write facts, contracts, limits, ownership rules, and examples
- do not write narrative introductions, philosophy sections, or marketing text
- keep comparisons measurable and testable
- keep non-goals explicit and tied to API or layer boundaries
- use Mermaid for diagrams
- use extended GitHub Markdown only where it improves technical readability
- use badges in every stable document
- Russian documentation must use Russian prose; keep English only for API identifiers, macro names, external project names, protocol names, and RFC identifiers

Mermaid diagram type must match the content:

- architecture: `architecture-beta` or `flowchart`
- interaction or handoff: `sequenceDiagram`
- state transitions: `stateDiagram-v2`
- release gates: `requirementDiagram`
- classification: `mindmap`, `block`, or `flowchart`
- comparison: `quadrantChart` only when axes are explicit and measurable

Keep the stable structure aligned with other `io*` projects:

- `docs/README.md`
- `docs/en/README.md`, `docs/ru/README.md`
- numbered documents in `docs/en/` and `docs/ru/`
- `docs/plans/README.md`, `docs/plans/ROADMAP.md`, `docs/plans/BACKLOG.md`

`docs/tmp/` is non-authoritative scratch space.

## Changelog And Versioning

Maintain CHANGELOG.md for every merge that changes behavior, public API, verification evidence, release automation, or published documentation.

Rules:

- Follow Keep a Changelog 1.1.0
- Keep `Unreleased` at the top
- Standard sections: `Added`, `Changed`, `Deprecated`, `Removed`, `Fixed`, `Security`
- Write short factual bullets
- Record externally visible changes only
- Update in the same branch as the code change

Versioning:

- Semantic Versioning with leading `v`
- `v0.y.z` for pre-1.0 releases
- `v0.y.z-rc.N` for release candidates
- First release: `v0.1.0`

## Coding Style & Naming Conventions

Target C23 only; C extensions disabled. Follow `.clang-format`: 4-space indentation, Linux brace style, 100-column limit, right-aligned pointer stars (`int *ptr`). Public symbols: `ioh_` prefix, `IOH_` for macros/enums, `_t` for typedefs. Prefer small, single-purpose translation units.

## Testing Guidelines

Unit tests use Unity, enabled through `IOHTTP_BUILD_TESTS`. Name tests `tests/unit/test_<module>.c`. Sanitizer presets: `clang-asan`, `clang-msan`, `clang-tsan`. Fuzz targets in `tests/fuzz/` (Clang only).

## Commit & Pull Request Guidelines

Follow convention: `feat:`, `fix:`, `refactor:`, `test:`, `docs:` with short imperative subjects. PRs should describe behavior change, commands run, analyzer results.

# Contributing to iohttp

## Development Environment

All development and compilation MUST happen inside the Podman container:

```bash
podman build -t iohttp-dev:latest -f deploy/podman/Containerfile .
podman run --rm -it --security-opt seccomp=unconfined \
  -v $(pwd):/workspace:Z localhost/iohttp-dev:latest
```

## Build and Test

```bash
cmake --preset clang-debug
cmake --build --preset clang-debug
ctest --preset clang-debug
```

## Quality Pipeline

Run before every PR:

```bash
./scripts/quality.sh
```

## Code Conventions

- C23 (`-std=c23`, `CMAKE_C_EXTENSIONS OFF`)
- Public API prefix: `ioh_`
- Pointer style: `int *ptr` (right-aligned)
- Column limit: 100
- Braces: Linux kernel style
- Tests: Unity framework

## Commit Messages

Use conventional commits:

- `feat:` — new feature
- `fix:` — bug fix
- `refactor:` — code restructuring
- `test:` — test additions/changes
- `docs:` — documentation
- `chore:` — maintenance

## Pull Requests

- Describe behavior change
- List commands run and results
- Note any analyzer findings or skipped checks
- All tests must pass
- clang-format must be clean

---
name: iohttp-release-management
description: Use when preparing a release candidate, final release, tag, release notes, or changelog update for iohttp.
---

# iohttp Release Management

## Required Standards

- Follow [Keep a Changelog 1.1.0](https://keepachangelog.com/en/1.1.0/).
- Follow [Semantic Versioning 2.0.0](https://semver.org/spec/v2.0.0.html).
- Use Git tags with a leading `v`.

## Versioning Policy

- `v0.y.z` is a normal pre-1.0 release.
- `v0.y.z-rc.N` is a release candidate.
- `v1.0.0` is reserved for the first release with a stable public contract, stable release gate, and published verification evidence.
- The current release line starts at `v0.1.0`.

## Changelog Rules

- Update `CHANGELOG.md` in the same branch as the change.
- Keep `## [Unreleased]` at the top.
- Move completed work into a dated version section when preparing a tag.
- Use only standard Keep a Changelog sections: `Added`, `Changed`, `Deprecated`, `Removed`, `Fixed`, `Security`.
- Write one factual bullet per externally visible change.

## Release Workflow

1. Verify `main` is clean and synchronized.
2. Update `CHANGELOG.md`.
3. Run the release gate: `./scripts/run-release-gate.sh`.
4. Create an annotated tag: `git tag -a v0.1.0 -m "Release v0.1.0"`.
5. Push the tag: `git push origin v0.1.0`.
6. Verify the GitHub release workflow and published release assets.

## Quality Gate

Before any release, the full quality pipeline must pass:

```bash
podman run --rm --security-opt seccomp=unconfined \
  -v $(pwd):/workspace:Z \
  localhost/iohttp-dev:latest bash -c "cd /workspace && ./scripts/quality.sh"
```

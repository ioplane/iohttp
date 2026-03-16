#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DIST_DIR="${ROOT_DIR}/dist"
TAG_NAME="${1:-${GITHUB_REF_NAME:-dev}}"

mkdir -p "${DIST_DIR}"

cat > "${DIST_DIR}/RELEASE_NOTES.md" <<EOF
# iohttp ${TAG_NAME}

## Scope

- Embedded HTTP server library for C23 with io_uring and wolfSSL
- HTTP/1.1 + HTTP/2 + HTTP/3 support
- TLS 1.3 via wolfSSL, QUIC via ngtcp2

## Verification

- Release gate: \`scripts/run-release-gate.sh\`
- Quality pipeline: \`scripts/quality.sh\`

## Published Assets

- Source tarball
- Source zip archive
- Generated API reference archive (if available)
- SHA256 checksums

## References

- Architecture: \`docs/en/01-architecture.md\`
- Comparison: \`docs/en/04-framework-comparison.md\`
EOF

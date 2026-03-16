#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

./scripts/quality.sh
cmake --build --preset clang-debug --target docs
python3 scripts/lint-docs.py

#!/usr/bin/env bash
set -euo pipefail

cmake --preset gcc-debug --fresh
cmake --build --preset gcc-debug 2>&1

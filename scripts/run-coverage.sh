#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build/clang-coverage"
REPORT_DIR="${BUILD_DIR}/coverage"
PROFILE_DIR="${BUILD_DIR}/profiles"

cmake --preset clang-coverage
cmake --build "${BUILD_DIR}" -j"$(nproc)"

rm -rf "${PROFILE_DIR}" "${REPORT_DIR}"
mkdir -p "${PROFILE_DIR}" "${REPORT_DIR}"

LLVM_PROFILE_FILE="${PROFILE_DIR}/%p.profraw" \
  ctest --test-dir "${BUILD_DIR}" --output-on-failure

llvm-profdata merge \
  -sparse \
  "${PROFILE_DIR}"/*.profraw \
  -o "${REPORT_DIR}/coverage.profdata"

mapfile -t COVERAGE_OBJECTS < <(find "${BUILD_DIR}" -maxdepth 1 -type f -perm -111 -name 'test_*' | sort)

if [[ ${#COVERAGE_OBJECTS[@]} -eq 0 ]]; then
  echo "no coverage objects found in ${BUILD_DIR}" >&2
  exit 1
fi

LLVM_COV_OBJECT_ARGS=()
for obj in "${COVERAGE_OBJECTS[@]:1}"; do
  LLVM_COV_OBJECT_ARGS+=("-object=${obj}")
done

IGNORE_REGEX='(.*/tests/.*|.*/usr/local/src/unity/.*)'

llvm-cov export \
  -format=lcov \
  "${COVERAGE_OBJECTS[0]}" \
  "${LLVM_COV_OBJECT_ARGS[@]}" \
  -instr-profile="${REPORT_DIR}/coverage.profdata" \
  -ignore-filename-regex="${IGNORE_REGEX}" \
  > "${REPORT_DIR}/coverage.lcov"

llvm-cov report \
  "${COVERAGE_OBJECTS[0]}" \
  "${LLVM_COV_OBJECT_ARGS[@]}" \
  -instr-profile="${REPORT_DIR}/coverage.profdata" \
  -ignore-filename-regex="${IGNORE_REGEX}" \
  > "${REPORT_DIR}/coverage.txt"

cat "${REPORT_DIR}/coverage.txt"

llvm-cov show \
  "${COVERAGE_OBJECTS[0]}" \
  "${LLVM_COV_OBJECT_ARGS[@]}" \
  -instr-profile="${REPORT_DIR}/coverage.profdata" \
  -ignore-filename-regex="${IGNORE_REGEX}" \
  --format=html \
  --output-dir="${REPORT_DIR}/html" \
  > /dev/null

chmod 0644 "${REPORT_DIR}/coverage.lcov" "${REPORT_DIR}/coverage.txt"

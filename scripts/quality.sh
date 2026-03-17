#!/usr/bin/env bash
# shellcheck shell=bash
# Repository quality pipeline for iohttp.
# Run inside the dev container: cd /workspace && ./scripts/quality.sh
# Or from host: podman run --rm --security-opt seccomp=unconfined \
#   -v /opt/projects/repositories/iohttp:/workspace:Z \
#   localhost/iohttp-dev:latest bash -c "cd /workspace && ./scripts/quality.sh"
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
readonly SCRIPT_DIR

# shellcheck disable=SC1091
if [[ -f /usr/local/lib/ioplane/common.sh ]]; then
    source /usr/local/lib/ioplane/common.sh
else
    source "${SCRIPT_DIR}/lib/common.sh"
fi

ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd -P)"
readonly ROOT_DIR
readonly BUILD_DIR="${BUILD_DIR:-build/clang-debug}"
readonly PRESET="${PRESET:-clang-debug}"
readonly TOTAL_STEPS=11

cd "${ROOT_DIR}"

# ═══════════════════════════════════════════════════════
# Step 1: Repository baseline
# ═══════════════════════════════════════════════════════

ioj_step 1 "${TOTAL_STEPS}" "Repository baseline"
ioj_check_repo_baseline

# ═══════════════════════════════════════════════════════
# Step 2: Stale identifier scan
# ═══════════════════════════════════════════════════════

ioj_step 2 "${TOTAL_STEPS}" "Stale identifier scan"
STALE_HITS=$(grep -rn '\bio_[a-z]' docs/ scripts/ \
    --include='*.md' --include='*.sh' --include='*.py' \
    --exclude='quality.sh' \
    2>/dev/null || true)
if [[ -z "${STALE_HITS}" ]]; then
    ioj_record_pass "No stale io_ prefix remnants in docs/scripts"
else
    printf '%s\n' "${STALE_HITS}"
    ioj_record_fail "Stale io_ prefix found in docs/scripts"
fi

# ═══════════════════════════════════════════════════════
# Step 3: Documentation lint
# ═══════════════════════════════════════════════════════

ioj_step 3 "${TOTAL_STEPS}" "Documentation lint"
ioj_check_docs_lint

# ═══════════════════════════════════════════════════════
# Step 4: Project bootstrap scripts check
# ═══════════════════════════════════════════════════════

ioj_step 4 "${TOTAL_STEPS}" "Project bootstrap scripts check"
SCRIPTS_OK=true
for s in scripts/quality.sh scripts/run-coverage.sh scripts/run-release-gate.sh \
         scripts/run-gcc-analyzer.sh scripts/build-release-assets.sh \
         scripts/render-release-notes.sh scripts/lint-docs.py; do
    if [[ ! -f "${s}" ]]; then
        printf '  missing: %s\n' "${s}"
        SCRIPTS_OK=false
    fi
done
if ${SCRIPTS_OK}; then
    ioj_record_pass "All required scripts present"
else
    ioj_record_fail "Some required scripts missing"
fi

# ═══════════════════════════════════════════════════════
# Step 5: GCC analyzer
# ═══════════════════════════════════════════════════════

ioj_step 5 "${TOTAL_STEPS}" "GCC analyzer"
ioj_check_gcc_analyzer

# ═══════════════════════════════════════════════════════
# Step 6: Configure and build
# ═══════════════════════════════════════════════════════

ioj_step 6 "${TOTAL_STEPS}" "Configure and build"
ioj_check_build_and_test "${PRESET}" "${BUILD_DIR}"

# ═══════════════════════════════════════════════════════
# Step 7: Format check
# ═══════════════════════════════════════════════════════

ioj_step 7 "${TOTAL_STEPS}" "Format check"
ioj_check_format "${PRESET}"

# ═══════════════════════════════════════════════════════
# Step 8: cppcheck
# ═══════════════════════════════════════════════════════

ioj_step 8 "${TOTAL_STEPS}" "cppcheck"
ioj_check_cppcheck "${BUILD_DIR}"

# ═══════════════════════════════════════════════════════
# Step 9: PVS-Studio
# ═══════════════════════════════════════════════════════

ioj_step 9 "${TOTAL_STEPS}" "PVS-Studio"
ioj_check_pvs_studio "${BUILD_DIR}"

# ═══════════════════════════════════════════════════════
# Step 10: CodeChecker (with picohttpparser exclusion)
# ═══════════════════════════════════════════════════════

ioj_step 10 "${TOTAL_STEPS}" "CodeChecker"
if ioj_has_cmake_surface; then
    if [[ ! -f "${BUILD_DIR}/compile_commands.json" ]]; then
        ioj_record_fail "CodeChecker: compile database missing"
    elif ! command -v CodeChecker >/dev/null 2>&1; then
        ioj_record_skip "CodeChecker not installed"
    else
        nproc_val="$(ioj_nproc)"
        cc_dir="$(ioj_mktemp -d)"
        cc_skip="$(ioj_mktemp)"

        cat > "${cc_skip}" <<'SKIP'
-/usr/local/src/unity/*
-*/picohttpparser.c
SKIP

        if CodeChecker analyze "${BUILD_DIR}/compile_commands.json" \
            -o "${cc_dir}" \
            --analyzers clangsa clang-tidy \
            --skip "${cc_skip}" \
            -j"${nproc_val}"; then

            cc_parse=$(CodeChecker parse "${cc_dir}" \
                --trim-path-prefix "$(pwd)/" 2>&1 || true)
            cc_parse=$(printf '%s\n' "${cc_parse}" \
                | grep -v '^\[INFO\]' \
                | grep -v '^$' \
                | grep -v '/usr/local/src/unity/' || true)

            cc_high=$(printf '%s\n' "${cc_parse}" | grep -c '\[HIGH\]' || true)
            cc_med=$(printf '%s\n' "${cc_parse}" | grep -c '\[MEDIUM\]' || true)

            if [[ "${cc_high}" -gt 0 || "${cc_med}" -gt 0 ]]; then
                printf '%s\n' "${cc_parse}" | grep -E '\[(HIGH|MEDIUM)\]' || true
                ioj_record_fail "CodeChecker: ${cc_high} HIGH, ${cc_med} MEDIUM"
            else
                ioj_record_pass "CodeChecker clean (no HIGH/MEDIUM)"
            fi
        else
            ioj_record_fail "CodeChecker analysis failed"
        fi
    fi
else
    ioj_record_skip "Compile database unavailable before CMake bootstrap"
fi

# ═══════════════════════════════════════════════════════
# Step 11: shellcheck
# ═══════════════════════════════════════════════════════

ioj_step 11 "${TOTAL_STEPS}" "Shellcheck"
ioj_check_shellcheck

# ═══════════════════════════════════════════════════════
# Summary
# ═══════════════════════════════════════════════════════

ioj_print_summary

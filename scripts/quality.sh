#!/usr/bin/env bash
# Full quality pipeline for iohttp
# Run inside the dev container: cd /workspace && ./scripts/quality.sh
# Or from host: podman run --rm --security-opt seccomp=unconfined \
#   -v /opt/projects/repositories/iohttp:/workspace:Z \
#   localhost/iohttp-dev:latest bash -c "cd /workspace && ./scripts/quality.sh"
set -euo pipefail

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[0;33m'
CYAN='\033[0;36m'
NC='\033[0m'

PASS=0
FAIL=0
SKIP=0

step() { printf "\n${CYAN}=== [%d/11] %s ===${NC}\n" "$1" "$2"; }
ok()   { printf "${GREEN}PASS${NC}: %s\n" "$1"; PASS=$((PASS + 1)); }
fail() { printf "${RED}FAIL${NC}: %s\n" "$1"; FAIL=$((FAIL + 1)); }
skip() { printf "${YELLOW}SKIP${NC}: %s\n" "$1"; SKIP=$((SKIP + 1)); }

has_cmake_surface() {
    [[ -f CMakeLists.txt && -f CMakePresets.json ]]
}

BUILD_DIR="${BUILD_DIR:-build/clang-debug}"
PRESET="${PRESET:-clang-debug}"
NPROC="$(nproc)"

# ── Step 1: Repository baseline ─────────────────────────────────────────
step 1 "Repository baseline"
BASELINE_OK=true
for f in AGENTS.md CLAUDE.md CODEX.md .github; do
    if [[ ! -e "$f" ]]; then
        printf "  missing: %s\n" "$f"
        BASELINE_OK=false
    fi
done
if $BASELINE_OK; then
    ok "Repository baseline files present"
else
    fail "Repository baseline files missing"
fi

# ── Step 2: Stale identifier scan ───────────────────────────────────────
step 2 "Stale identifier scan"
STALE_HITS=$(grep -rn '\bio_[a-z]' docs/ scripts/ \
    --include='*.md' --include='*.sh' --include='*.py' \
    --exclude='quality.sh' \
    2>/dev/null || true)
if [[ -z "$STALE_HITS" ]]; then
    ok "No stale io_ prefix remnants in docs/scripts"
else
    echo "$STALE_HITS"
    fail "Stale io_ prefix found in docs/scripts"
fi

# ── Step 3: Documentation lint ──────────────────────────────────────────
step 3 "Documentation lint"
if [[ -f scripts/lint-docs.py ]]; then
    if python3 scripts/lint-docs.py 2>&1; then
        ok "Documentation lint passed"
    else
        fail "Documentation lint failed"
    fi
else
    skip "scripts/lint-docs.py not present"
fi

# ── Step 4: Project bootstrap scripts check ─────────────────────────────
step 4 "Project bootstrap scripts check"
SCRIPTS_OK=true
for s in scripts/quality.sh scripts/run-coverage.sh scripts/run-release-gate.sh \
         scripts/run-gcc-analyzer.sh scripts/build-release-assets.sh \
         scripts/render-release-notes.sh scripts/lint-docs.py; do
    if [[ ! -f "$s" ]]; then
        printf "  missing: %s\n" "$s"
        SCRIPTS_OK=false
    fi
done
if $SCRIPTS_OK; then
    ok "All required scripts present"
else
    fail "Some required scripts missing"
fi

# ── Step 5: GCC analyzer ────────────────────────────────────────────────
step 5 "GCC analyzer"
if [[ -f scripts/run-gcc-analyzer.sh ]]; then
    if bash scripts/run-gcc-analyzer.sh 2>&1 | tail -10; then
        ok "GCC analyzer passed"
    else
        fail "GCC analyzer failed"
    fi
else
    skip "scripts/run-gcc-analyzer.sh not present"
fi

# ── Step 6: Configure + Build ───────────────────────────────────────────
step 6 "Configure and Build"
if has_cmake_surface; then
    cmake --preset "${PRESET}" 2>&1 | tail -3
    if cmake --build --preset "${PRESET}" 2>&1 | tail -5; then
        ok "Build succeeded"
    else
        fail "Build failed"
        exit 1
    fi
else
    skip "No CMakeLists.txt / CMakePresets.json found"
fi

# ── Step 7: Unit Tests ──────────────────────────────────────────────────
step 7 "Unit Tests"
if has_cmake_surface; then
    if ctest --preset "${PRESET}" --output-on-failure 2>&1; then
        ok "All tests passed"
    else
        fail "Some tests failed"
    fi
else
    skip "No CMakeLists.txt / CMakePresets.json found"
fi

# ── Step 8: clang-format ────────────────────────────────────────────────
step 8 "clang-format"
if has_cmake_surface; then
    if cmake --build --preset "${PRESET}" --target format-check 2>&1; then
        ok "Formatting clean"
    else
        fail "Formatting issues found"
    fi
else
    skip "No CMakeLists.txt / CMakePresets.json found"
fi

# ── Step 9: cppcheck ───────────────────────────────────────────────────
step 9 "cppcheck"
if has_cmake_surface; then
    if command -v cppcheck >/dev/null 2>&1; then
        if cppcheck --enable=warning,performance,portability \
            --error-exitcode=1 --inline-suppr \
            --project="${BUILD_DIR}/compile_commands.json" \
            --suppress='*:/usr/local/src/unity/*' \
            -q 2>&1; then
            ok "cppcheck clean"
        else
            fail "cppcheck found issues"
        fi
    else
        skip "cppcheck not installed"
    fi
else
    skip "No CMakeLists.txt / CMakePresets.json found"
fi

# ── Step 10: PVS-Studio ─────────────────────────────────────────────────
step 10 "PVS-Studio"
if has_cmake_surface; then
    if command -v pvs-studio-analyzer >/dev/null 2>&1; then
        # Load license from .env
        if [[ -f .env ]]; then
            # shellcheck disable=SC1091
            source .env
        fi
        if [[ -z "${PVS_NAME:-}" || -z "${PVS_KEY:-}" ]]; then
            skip "PVS-Studio: no license in .env (PVS_NAME/PVS_KEY)"
        else
            pvs-studio-analyzer credentials "${PVS_NAME}" "${PVS_KEY}" >/dev/null 2>&1
            PVS_LOG="${BUILD_DIR}/pvs-studio.log"
            PVS_SUPPRESS=".pvs-suppress.json"
            PVS_SUPPRESS_ARG=""
            if [[ -f "${PVS_SUPPRESS}" ]]; then
                PVS_SUPPRESS_ARG="-s ${PVS_SUPPRESS}"
            fi
            # shellcheck disable=SC2086
            pvs-studio-analyzer analyze \
                -f "${BUILD_DIR}/compile_commands.json" \
                -o "${PVS_LOG}" \
                -e /usr/local/src/unity/ \
                ${PVS_SUPPRESS_ARG} \
                -j"${NPROC}" 2>&1 | grep -v '^\[' || true

            # GA:1,2 = errors + warnings (skip notes/low)
            PVS_OUT=$(plog-converter -t errorfile -a 'GA:1,2' "${PVS_LOG}" 2>/dev/null \
                | grep -v '^pvs-studio.com' | grep -v '^Analyzer log' \
                | grep -v '^PVS-Studio is' | grep -v '^$' \
                | grep -v 'Total messages' | grep -v 'Filtered messages' \
                | grep -v '^Copyright')
            PVS_COUNT=$(echo "${PVS_OUT}" | grep -cE '(error|warning):' || true)
            if [[ "${PVS_COUNT}" -eq 0 ]]; then
                ok "PVS-Studio clean (GA:1,2)"
            else
                echo "${PVS_OUT}"
                fail "PVS-Studio: ${PVS_COUNT} errors/warnings"
            fi

            # Hint: to create/update baselines from current findings:
            # pvs-studio-analyzer suppress -o .pvs-suppress.json build/clang-debug/pvs-studio.log
            # CodeChecker parse <results-dir> -e baseline -o .codechecker.baseline
        fi
    else
        skip "PVS-Studio not installed"
    fi
else
    skip "No CMakeLists.txt / CMakePresets.json found"
fi

# ── Step 11: CodeChecker ────────────────────────────────────────────────
step 11 "CodeChecker (Clang SA + clang-tidy)"
if has_cmake_surface; then
    if command -v CodeChecker >/dev/null 2>&1; then
        CC_DIR=$(mktemp -d)

        # Create skip file to exclude vendored/third-party code
        CC_SKIP=$(mktemp)
        cat > "${CC_SKIP}" <<'SKIP'
-/usr/local/src/unity/*
-*/picohttpparser.c
SKIP

        CC_BASELINE=".codechecker.baseline"
        CC_BASELINE_ARG=""
        if [[ -f "${CC_BASELINE}" ]]; then
            CC_BASELINE_ARG="--baseline ${CC_BASELINE}"
        fi

        CodeChecker analyze "${BUILD_DIR}/compile_commands.json" \
            -o "${CC_DIR}" \
            --analyzers clangsa clang-tidy \
            --skip "${CC_SKIP}" \
            -j"${NPROC}" 2>&1 | grep -E '(Summary|Successfully|Failed|error:)' || true
        # shellcheck disable=SC2086
        CC_OUT=$(CodeChecker parse "${CC_DIR}" \
            --trim-path-prefix "$(pwd)/" \
            ${CC_BASELINE_ARG} 2>&1 || true)
        CC_OUT=$(echo "${CC_OUT}" \
            | grep -v '^\[INFO\]' | grep -v '^$' \
            | grep -v '/usr/local/src/unity/' || true)
        CC_HIGH=$(echo "${CC_OUT}" | grep -c '\[HIGH\]' || true)
        CC_MED=$(echo "${CC_OUT}" | grep -c '\[MEDIUM\]' || true)
        if [[ "${CC_HIGH}" -gt 0 || "${CC_MED}" -gt 0 ]]; then
            echo "${CC_OUT}" | grep -E '\[(HIGH|MEDIUM)\]' || true
            fail "CodeChecker: ${CC_HIGH} HIGH, ${CC_MED} MEDIUM"
        else
            ok "CodeChecker clean (no HIGH/MEDIUM)"
        fi
        rm -rf "${CC_DIR}" "${CC_SKIP}"
    else
        skip "CodeChecker not installed"
    fi
else
    skip "No CMakeLists.txt / CMakePresets.json found"
fi

# ── Summary ───────────────────────────────────────────────────────────
printf "\n${CYAN}=== Summary ===${NC}\n"
printf "${GREEN}PASS: %d${NC}  ${RED}FAIL: %d${NC}  ${YELLOW}SKIP: %d${NC}\n" \
    "${PASS}" "${FAIL}" "${SKIP}"

if [[ "${FAIL}" -gt 0 ]]; then
    printf "${RED}Quality pipeline FAILED${NC}\n"
    exit 1
else
    printf "${GREEN}Quality pipeline PASSED${NC}\n"
fi

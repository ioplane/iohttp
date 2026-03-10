#!/usr/bin/env bash
# rename_prefix.sh — Rename io_ prefix to ioh_ across the iohttp project
# Safety: operates on git-tracked files only, creates no unrecoverable state
set -euo pipefail

REPO_ROOT="$(git -C "$(dirname "$0")/.." rev-parse --show-toplevel)"
cd "$REPO_ROOT"

# Verify clean working tree
if ! git diff --quiet || ! git diff --cached --quiet; then
    echo "ERROR: Working tree is not clean. Commit or stash changes first."
    exit 1
fi

echo "=== Phase 1: Rename source files (git mv) ==="

# Rename io_*.h and io_*.c in src/ (but NOT picohttpparser)
find src -type f \( -name 'io_*.h' -o -name 'io_*.c' \) | sort | while read -r f; do
    dir=$(dirname "$f")
    base=$(basename "$f")
    newbase="${base/io_/ioh_}"
    echo "  git mv $f $dir/$newbase"
    git mv "$f" "$dir/$newbase"
done

# Rename test_io_*.c in tests/unit/
find tests/unit -type f -name 'test_io_*.c' | sort | while read -r f; do
    dir=$(dirname "$f")
    base=$(basename "$f")
    newbase="${base/test_io_/test_ioh_}"
    echo "  git mv $f $dir/$newbase"
    git mv "$f" "$dir/$newbase"
done

echo ""
echo "=== Phase 2: Replace content in source files ==="

# Collect all files to process (source, headers, tests, CMake, docs, skills, examples)
mapfile -t FILES < <(git ls-files -- \
    '*.c' '*.h' 'CMakeLists.txt' 'cmake/*.cmake' \
    '*.md' '.claude/skills/*' \
    'examples/*' 'scripts/*' \
    '.clang-format' '.clang-tidy' \
    '*.json' '*.yml' '*.yaml' \
    'Containerfile' 'deploy/*' \
    '.gitignore' '.env.example' \
    2>/dev/null | sort -u)

# Also include CMakePresets.json if it exists
if [ -f "CMakePresets.json" ]; then
    FILES+=("CMakePresets.json")
fi

echo "  Processing ${#FILES[@]} files..."

for f in "${FILES[@]}"; do
    [ -f "$f" ] || continue

    # Skip vendored picohttpparser
    case "$f" in
        src/http/picohttpparser.*) continue ;;
    esac

    # Phase 2a: Protect io_uring references (replace with placeholder)
    # Protect: io_uring (struct, functions, type references)
    sed -i \
        -e 's/io_uring/__IORING_PH__/g' \
        "$f"

    # Phase 2b: Replace io_ -> ioh_ (lowercase, our project prefix)
    sed -i \
        -e 's/\bio_/ioh_/g' \
        "$f"

    # Phase 2c: Replace IO_ -> IOH_ (uppercase macros/enums)
    # But NOT IOHTTP_ (include guards) and NOT IORING_ (io_uring constants)
    # IO_ at word boundary, but not followed by RING or HTTP
    sed -i \
        -e 's/\bIO_\([A-Z]\)/IOH_\1/g' \
        "$f"

    # Phase 2d: Restore io_uring placeholders
    sed -i \
        -e 's/__IORING_PH__/io_uring/g' \
        "$f"
done

echo ""
echo "=== Phase 3: Fix CMake macro names ==="

# Rename io_add_test -> ioh_add_test, io_add_fuzz -> ioh_add_fuzz
if [ -f CMakeLists.txt ]; then
    sed -i \
        -e 's/\bio_add_test\b/ioh_add_test/g' \
        -e 's/\bio_add_fuzz\b/ioh_add_fuzz/g' \
        CMakeLists.txt
fi

echo ""
echo "=== Phase 4: Verification ==="

# Check for any remaining io_ that should have been renamed (excluding io_uring)
echo "  Checking for missed io_ references in source..."
MISSED_LOWER=$(grep -rn '\bio_[a-z]' src/ tests/ CMakeLists.txt \
    --include='*.c' --include='*.h' --include='CMakeLists.txt' \
    | grep -v 'io_uring' \
    | grep -v 'picohttpparser' \
    | grep -v '__IORING_PH__' \
    || true)

if [ -n "$MISSED_LOWER" ]; then
    echo "  WARNING: Possible missed io_ references:"
    echo "$MISSED_LOWER" | head -30
else
    echo "  OK: No missed io_ references in source"
fi

echo ""
echo "  Checking for missed IO_ references in source..."
MISSED_UPPER=$(grep -rn '\bIO_[A-Z]' src/ tests/ CMakeLists.txt \
    --include='*.c' --include='*.h' --include='CMakeLists.txt' \
    | grep -v 'IOHTTP_' \
    | grep -v 'IORING_' \
    | grep -v 'IO_uring' \
    || true)

if [ -n "$MISSED_UPPER" ]; then
    echo "  WARNING: Possible missed IO_ references:"
    echo "$MISSED_UPPER" | head -30
else
    echo "  OK: No missed IO_ references in source"
fi

echo ""
echo "  Checking io_uring references preserved..."
URING_COUNT=$(grep -rc 'io_uring' src/ --include='*.c' --include='*.h' | awk -F: '{s+=$2}END{print s}')
echo "  io_uring references in src/: $URING_COUNT (should be > 0)"

echo ""
echo "=== Phase 5: Summary ==="
echo "  Files renamed: $(git diff --name-only --diff-filter=R | wc -l)"
echo "  Files modified: $(git diff --name-only --diff-filter=M | wc -l)"
echo ""
echo "Review changes with: git diff --stat"
echo "Build test with: podman exec -w /workspace iohttp-dev bash -c 'cmake --preset clang-debug && cmake --build --preset clang-debug'"
echo ""
echo "Done. Changes are unstaged — review before committing."

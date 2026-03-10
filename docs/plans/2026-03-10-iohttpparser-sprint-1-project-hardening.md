# iohttpparser Sprint 1: Project Hardening

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Bring the iohttpparser repository to production-grade C23 open-source project standards per `docs/tmp/draft/git-project-recommendations-for-c23.md`.

**Architecture:** Fill documentation and tooling gaps identified in the gap analysis: missing config files (.clangd, Doxyfile), missing documentation (SUPPORT.md, README.ru.md), YAML issue templates, coverage preset, fuzz infrastructure, .gitignore completeness.

**Tech Stack:** C23, CMake 3.25+, Clang 22, GCC 15, Unity, LibFuzzer, Doxygen, PVS-Studio, CodeChecker.

**Source:** Gap analysis of iohttpparser repo vs `docs/tmp/draft/git-project-recommendations-for-c23.md` recommendations.

**Build/test:**
```bash
cmake --preset clang-debug && cmake --build --preset clang-debug && ctest --preset clang-debug
```

---

## Task 1: Add .clangd and fix .gitignore

**Priority:** P0

**Files:**
- Create: `.clangd`
- Modify: `.gitignore` — add `CMakeUserPresets.json`

**Step 1: Create .clangd**

```yaml
CompileFlags:
  CompilationDatabase: build/clang-debug/
  Add: [-std=c23, -Wall, -Wextra]
Diagnostics:
  ClangTidy:
    Add: [bugprone-*, cert-*, clang-analyzer-*]
  UnusedIncludes: Strict
InlayHints:
  Enabled: true
  ParameterNames: true
```

**Step 2: Add CMakeUserPresets.json to .gitignore**

Append to `.gitignore`:
```
# Developer-local preset overrides
CMakeUserPresets.json
```

**Step 3: Commit**

```bash
git add .clangd .gitignore
git commit -m "chore: add .clangd config and fix .gitignore completeness"
```

---

## Task 2: Create Doxyfile for API documentation

**Priority:** P0 — CMakeLists.txt already references it.

**Files:**
- Create: `Doxyfile`

**Step 1: Create Doxyfile**

Generate with `doxygen -g Doxyfile` inside the container, then customize:

Key settings:
```
PROJECT_NAME           = iohttpparser
PROJECT_NUMBER         = 0.1.0
PROJECT_BRIEF          = "High-performance HTTP/1.1 parser for C23"
OUTPUT_DIRECTORY       = docs/api
OPTIMIZE_OUTPUT_FOR_C  = YES
INPUT                  = include/ src/
FILE_PATTERNS          = *.h *.c
RECURSIVE              = YES
EXTRACT_ALL            = YES
GENERATE_HTML          = YES
GENERATE_MAN           = YES
MAN_EXTENSION          = .3
GENERATE_XML           = NO
EXCLUDE_PATTERNS       = */tests/* */bench/* */examples/*
SOURCE_BROWSER         = YES
STRIP_FROM_PATH        = include/
```

**Step 2: Verify it works**

```bash
cmake --build --preset clang-debug --target docs
```

**Step 3: Commit**

```bash
git add Doxyfile
git commit -m "docs: add Doxyfile for API documentation generation"
```

---

## Task 3: Create SUPPORT.md and README.ru.md

**Priority:** P1

**Files:**
- Create: `SUPPORT.md`
- Create: `README.ru.md`

**Step 1: Create SUPPORT.md**

```markdown
# Support

## Getting Help

- **Bugs**: [GitHub Issues](https://github.com/pnx/iohttpparser/issues)
- **Security**: See [SECURITY.md](SECURITY.md)
- **Questions**: GitHub Discussions

## Vulnerability Classes We Track

- Buffer overflow (heap/stack)
- Use-after-free
- Integer overflow in size/length arithmetic
- Uninitialized memory reads
- Chunked encoding smuggling
- Request-line / header injection
```

**Step 2: Create README.ru.md**

Russian translation of README.md with language switcher at top:

```markdown
> 🇬🇧 [English](README.md) | 🇷🇺 **Русский**
```

Add the same switcher to README.md:

```markdown
> 🇬🇧 **English** | 🇷🇺 [Русский](README.ru.md)
```

**Step 3: Add Requirements table and CMake options table to both READMEs**

Requirements:
| Requirement | Minimum |
|-------------|---------|
| C compiler | Clang 22+ or GCC 15+ |
| CMake | 3.25+ |
| Linux kernel | Any (parser is portable) |
| Unity | 2.6.1 (tests only) |

CMake options:
| Option | Default | Description |
|--------|---------|-------------|
| `IOHTTPPARSER_BUILD_TESTS` | ON | Build unit tests |
| `IOHTTPPARSER_BUILD_BENCH` | OFF | Build benchmarks |
| `IOHTTPPARSER_BUILD_FUZZ` | OFF | Build fuzz targets (Clang only) |
| `IOHTTPPARSER_BUILD_EXAMPLES` | OFF | Build examples |
| `IOHTTPPARSER_SIMD_SSE42` | ON | Enable SSE4.2 scanner |
| `IOHTTPPARSER_SIMD_AVX2` | ON | Enable AVX2 scanner |

**Step 4: Commit**

```bash
git add SUPPORT.md README.ru.md README.md
git commit -m "docs: add SUPPORT.md, README.ru.md, requirements and options tables"
```

---

## Task 4: Convert issue templates to YAML format

**Priority:** P2

**Files:**
- Delete: `.github/ISSUE_TEMPLATE/bug_report.md`
- Delete: `.github/ISSUE_TEMPLATE/feature_request.md`
- Create: `.github/ISSUE_TEMPLATE/bug_report.yml`
- Create: `.github/ISSUE_TEMPLATE/feature_request.yml`
- Create: `.github/ISSUE_TEMPLATE/config.yml`

**Step 1: Create YAML bug report template**

```yaml
name: "Bug Report"
description: "Report a bug in iohttpparser"
labels: ["bug", "triage"]
body:
  - type: dropdown
    id: compiler
    attributes:
      label: Compiler
      options: [GCC 15, GCC 14, Clang 22, Clang 19, Other]
    validations: { required: true }
  - type: textarea
    id: description
    attributes:
      label: Description
      description: Clear description of the bug
    validations: { required: true }
  - type: textarea
    id: reproduce
    attributes:
      label: Steps to Reproduce
  - type: textarea
    id: sanitizer-output
    attributes:
      label: "Sanitizer Output (ASan/UBSan/TSan)"
      render: shell
```

**Step 2: Create YAML feature request template**

```yaml
name: "Feature Request"
description: "Suggest a feature for iohttpparser"
labels: ["enhancement"]
body:
  - type: textarea
    id: description
    attributes:
      label: Description
    validations: { required: true }
  - type: textarea
    id: api
    attributes:
      label: Proposed API
      render: c
```

**Step 3: Create config.yml**

```yaml
blank_issues_enabled: true
```

**Step 4: Commit**

```bash
git rm .github/ISSUE_TEMPLATE/bug_report.md .github/ISSUE_TEMPLATE/feature_request.md
git add .github/ISSUE_TEMPLATE/
git commit -m "chore: convert issue templates from markdown to YAML format"
```

---

## Task 5: Add coverage preset and fuzz infrastructure

**Priority:** P2

**Files:**
- Modify: `CMakePresets.json` — add `clang-coverage` preset
- Create: `tests/fuzz/corpus/parser/` — seed inputs for parser fuzzer
- Create: `tests/fuzz/corpus/chunked/` — seed inputs for chunked fuzzer
- Create: `tests/fuzz/dictionaries/http.dict` — HTTP token dictionary

**Step 1: Add coverage preset to CMakePresets.json**

Add to `configurePresets` array:

```json
{
  "name": "clang-coverage",
  "displayName": "Clang Coverage (llvm-cov)",
  "description": "Code coverage via llvm profiling",
  "inherits": "clang-base",
  "cacheVariables": {
    "CMAKE_BUILD_TYPE": "Debug",
    "CMAKE_C_FLAGS": "-fprofile-instr-generate -fcoverage-mapping -g",
    "CMAKE_EXE_LINKER_FLAGS": "-fprofile-instr-generate -fuse-ld=mold"
  }
}
```

Add to `buildPresets`:

```json
{ "name": "clang-coverage", "configurePreset": "clang-coverage", "jobs": 0 }
```

Add to `testPresets`:

```json
{
  "name": "clang-coverage",
  "configurePreset": "clang-coverage",
  "output": { "outputOnFailure": true }
}
```

**Step 2: Create fuzz corpus seed files**

`tests/fuzz/corpus/parser/simple_get.txt`:
```
GET / HTTP/1.1\r\nHost: example.com\r\n\r\n
```

`tests/fuzz/corpus/parser/post_with_body.txt`:
```
POST /data HTTP/1.1\r\nHost: example.com\r\nContent-Length: 5\r\n\r\nhello
```

`tests/fuzz/corpus/chunked/simple.txt`:
```
5\r\nhello\r\n0\r\n\r\n
```

**Step 3: Create HTTP dictionary for LibFuzzer**

`tests/fuzz/dictionaries/http.dict`:
```
# HTTP methods
"GET"
"POST"
"PUT"
"DELETE"
"HEAD"
"OPTIONS"
"PATCH"

# HTTP versions
"HTTP/1.0"
"HTTP/1.1"

# Common headers
"Host:"
"Content-Length:"
"Transfer-Encoding:"
"Connection:"
"Content-Type:"

# Delimiters
"\r\n"
"\r\n\r\n"
": "

# Chunked encoding
"chunked"
"0\r\n\r\n"
```

**Step 4: Commit**

```bash
git add CMakePresets.json tests/fuzz/corpus/ tests/fuzz/dictionaries/
git commit -m "test: add coverage preset, fuzz corpus seeds, and HTTP dictionary"
```

---

## Task 6: Run and baseline quality tools

**Priority:** P2

**Files:**
- Create: `.pvs-suppress.json` (generated)
- Create: `.codechecker.baseline` (generated)

**Step 1: Run full quality pipeline inside container**

```bash
podman run --rm -v $(pwd):/workspace:Z localhost/iohttpparser-dev:latest \
  bash -c "cd /workspace && ./scripts/quality.sh"
```

**Step 2: If PVS findings exist, generate suppress file**

```bash
pvs-studio-analyzer suppress -o .pvs-suppress.json build/clang-debug/pvs-studio.log
```

**Step 3: If CodeChecker findings exist, generate baseline**

```bash
CodeChecker parse build/clang-debug/cc-results -e baseline -o .codechecker.baseline
```

**Step 4: Commit baselines**

```bash
git add .pvs-suppress.json .codechecker.baseline
git commit -m "chore: generate PVS-Studio and CodeChecker analysis baselines"
```

---

## Acceptance Criteria

- [ ] `.clangd` exists and provides LSP configuration
- [ ] `CMakeUserPresets.json` is in `.gitignore`
- [ ] `Doxyfile` exists and `cmake --build --preset clang-debug --target docs` works
- [ ] `SUPPORT.md` exists with vulnerability class list
- [ ] `README.ru.md` exists with Russian translation
- [ ] Both READMEs have requirements table, CMake options table, language switcher
- [ ] Issue templates are YAML format (.yml) with compiler dropdown
- [ ] `clang-coverage` preset exists in `CMakePresets.json`
- [ ] `tests/fuzz/corpus/` and `tests/fuzz/dictionaries/` populated
- [ ] Quality pipeline green: `./scripts/quality.sh` → PASS: 6, FAIL: 0
- [ ] PVS/CodeChecker baselines generated

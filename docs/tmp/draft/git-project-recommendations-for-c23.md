# Документация и структура open-source C23-проекта на GitHub

**C23 (ISO/IEC 9899:2024) стал новым стандартом для системного программирования — GCC 15 уже использует `-std=gnu23` по умолчанию.** Правильно оформленный open-source C-проект на GitHub в 2025–2026 году требует не только хорошего кода, но и продуманной инфраструктуры: конфигурации сборки, CI/CD с санитайзерами, фаззинг-тестов, подписи релизов и структурированной документации. Этот документ — полное руководство по созданию и оформлению проектов уровня liburing, curl или systemd для проектов ringwall, iohttp и liboas, работающих поверх io_uring на современном Linux.

---

## 1. Структура каталогов: библиотека vs приложение

Выбор структуры каталогов — фундаментальное решение. Проекты **liboas** (библиотека) и **ringwall/iohttp** (приложения) требуют разных подходов, хотя и строятся на общем каркасе.

### Эталонная структура для C-библиотеки (liboas)

```
liboas/
├── .github/                     # GitHub Actions, шаблоны issues/PR
│   ├── ISSUE_TEMPLATE/
│   │   ├── bug_report.yml
│   │   ├── feature_request.yml
│   │   └── config.yml
│   ├── PULL_REQUEST_TEMPLATE.md
│   ├── CODEOWNERS
│   ├── dependabot.yml
│   └── workflows/
│       ├── ci.yml
│       ├── sanitizers.yml
│       ├── static-analysis.yml
│       ├── coverage.yml
│       └── release.yml
├── cmake/                       # CMake модули
│   └── liboasConfig.cmake.in
├── docs/                        # Документация (Doxygen, Sphinx или MkDocs)
├── examples/                    # Примеры использования API
├── fuzz/                        # Фаззинг-харнессы
│   ├── CMakeLists.txt
│   ├── fuzz_parser.c
│   └── corpus/
├── include/                     # Публичные заголовки (устанавливаются)
│   └── liboas/
│       ├── oas.h
│       └── oas_types.h
├── man/                         # Man-страницы (секция 3)
├── src/                         # Исходники + приватные заголовки
│   ├── internal.h               # НЕ устанавливается
│   ├── oas.c
│   └── parser.c
├── tests/                       # Юнит- и интеграционные тесты
│   ├── CMakeLists.txt
│   └── test_parser.c
├── bench/                       # Бенчмарки
├── scripts/                     # Вспомогательные скрипты
├── .clang-format
├── .clang-tidy
├── .clangd
├── .editorconfig
├── .gitignore
├── CMakeLists.txt
├── CMakePresets.json
├── Containerfile                # Podman (не Dockerfile!)
├── liboas.pc.in                 # pkg-config шаблон
├── LICENSE                      # MIT
├── COPYING                      # LGPL-2.1 (при двойном лицензировании)
├── README.md
├── README.ru.md
├── CONTRIBUTING.md
├── CODE_OF_CONDUCT.md
├── SECURITY.md
├── CHANGELOG.md
└── SUPPORT.md
```

### Структура для приложения (ringwall, iohttp)

```
ringwall/
├── .github/                     # (аналогично)
├── cmake/
├── docs/
├── man/                         # Man-страницы (секция 1 — команды)
│   └── ringwall.1.scd           # Исходник для scdoc
├── src/
│   ├── main.c                   # Точка входа
│   ├── config.c
│   ├── config.h                 # Все заголовки в src/ — они приватные
│   ├── firewall.c
│   └── firewall.h
├── etc/
│   └── ringwall.conf.example    # Пример конфигурации
├── systemd/
│   └── ringwall.service         # Unit-файл systemd
├── tests/
├── fuzz/
├── .clang-format
├── .clang-tidy
├── CMakeLists.txt
├── CMakePresets.json
├── Containerfile
├── LICENSE
├── README.md
└── ...
```

**Ключевое различие**: у библиотеки публичные заголовки живут в `include/{project}/` и устанавливаются в систему, а у приложения все заголовки находятся в `src/` и являются приватными. Библиотека обязана предоставлять `pkg-config` `.pc`-файл и CMake config-файлы для потребителей через `find_package()`. Приложение вместо этого предоставляет man-страницы, systemd unit-файлы и примеры конфигурации.

### Публичные vs приватные заголовки в CMake

```cmake
target_include_directories(liboas
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)
```

Директория `PUBLIC` распространяется на потребителей библиотеки. Директория `PRIVATE` доступна только при сборке самой библиотеки. Generator expressions `$<BUILD_INTERFACE:...>` и `$<INSTALL_INTERFACE:...>` разделяют пути для дерева сборки и установленной библиотеки. Этот паттерн используют **mimalloc**, **curl**, **HAProxy** — практически все зрелые C-библиотеки.

---

## 2. Стандартные файлы документации

### README.md — витрина проекта

README для C-проекта с io_uring должен содержать секции, которые сразу отвечают на критические вопросы пользователя: какое ядро нужно, какой компилятор, как собрать.

**Обязательные секции:**
- Название + краткое описание + бейджи (CI, coverage, лицензия, OpenSSF Scorecard)
- **Требования**: минимальная версия ядра Linux (например, ≥ 6.1 для io_uring с buffer rings), компилятор C23 (GCC ≥ 15 или Clang ≥ 19), CMake ≥ 4.0, liburing ≥ 2.7
- **Быстрый старт**: `cmake --preset release && cmake --build --preset release`
- **Примеры использования** API или CLI
- **Таблица CMake-опций** (`-DBUILD_TESTING=ON`, `-DENABLE_SANITIZERS=ON`)
- Ссылки на CONTRIBUTING.md, SECURITY.md, документацию
- Переключатель языка: `🇬🇧 [English](README.md) | 🇷🇺 [Русский](README.ru.md)`

### Выбор лицензии по модели liburing

Анализ лицензий успешных C-проектов показывает чёткую закономерность:

| Проект | Лицензия | Тип |
|--------|----------|-----|
| **liburing** | LGPL-2.1 OR MIT | Двойная |
| **curl** | curl (MIT-подобная) | Пермиссивная |
| **mimalloc** | MIT | Пермиссивная |
| **jemalloc** | BSD-2-Clause | Пермиссивная |
| **nginx** | BSD-2-Clause | Пермиссивная |
| **Valkey** | BSD-3-Clause | Пермиссивная |
| **systemd** | LGPL-2.1+ | Слабый copyleft |
| **WireGuard** | GPL-2.0 | Copyleft |
| **HAProxy** | GPL-2.0+ | Copyleft |

**Рекомендация для liboas** (библиотека): **двойная MIT / LGPL-2.1-or-later** по модели liburing. MIT обеспечивает максимальное усвоение; LGPL гарантирует возврат изменений в саму библиотеку, но позволяет проприетарное линкование.

**Рекомендация для ringwall/iohttp** (приложения): **GPL-2.0-or-later** (если цель — сохранить код открытым) или **MIT** (для максимального распространения).

**SPDX-заголовки** в каждом файле — стандарт, заданный ядром Linux:

```c
// SPDX-License-Identifier: LGPL-2.1-or-later OR MIT
// SPDX-FileCopyrightText: 2025 Your Name <email@example.com>
```

Заголовок ставится **первой строкой** файла (для `.c` — первая строка, для `.h` — перед include guard). Полный текст лицензии хранится в файлах `LICENSE` (MIT) и `COPYING` (LGPL).

### SECURITY.md для системных C-проектов

В отличие от проектов на memory-safe языках, C-проект должен **явно перечислить классы уязвимостей**, которые проект отслеживает. curl ведёт таксономию «C-ошибок»: OVERFLOW, OVERREAD, DOUBLE_FREE, USE_AFTER_FREE, NULL_MISTAKE, UNINIT, BAD_FREE.

```markdown
# Security Policy

## Reporting a Vulnerability

**НЕ создавайте публичный issue для уязвимостей.**

Используйте [GitHub Private Vulnerability Reporting](ссылка) или пишите
на security@project.org (PGP ключ: [ссылка]).

- **Подтверждение**: в течение 48 часов
- **Оценка**: в течение 1 недели
- **Исправление**: координированный выпуск

## Интересующие классы уязвимостей

- Buffer overflow (heap/stack)
- Use-after-free
- Double-free
- Integer overflow/underflow → некорректные аллокации
- Неинициализированная память
- Data races в асинхронных путях io_uring
- Некорректная валидация ввода в SQ/CQ io_uring

## Рекомендации пользователям

- Компилируйте с `-D_FORTIFY_SOURCE=3` и stack protectors
- Запускайте с AddressSanitizer при тестировании
- Проверяйте минимальную версию ядра для используемых io_uring операций
```

systemd направляет отчёты на `systemd-security@redhat.com`; curl использует HackerOne и является CNA (может сам присваивать CVE). Для небольшого проекта оптимально включить **GitHub Private Vulnerability Reporting** (кнопка «Report a vulnerability» в Security tab) и создавать **GitHub Security Advisories** для координированного раскрытия.

### CONTRIBUTING.md, CODE_OF_CONDUCT.md, CHANGELOG.md

**CONTRIBUTING.md** для C-проекта обязательно включает: ссылку на `.clang-format`, формат коммитов (Conventional Commits: `feat:`, `fix:`, `perf:`), инструкции по запуску санитайзеров (`cmake --preset asan && cmake --build --preset asan && ctest --preset asan`), политику squash-merge.

**CODE_OF_CONDUCT.md** — стандартный **Contributor Covenant v2.1**, заполнив контактный метод для enforcement.

**CHANGELOG.md** — формат **Keep a Changelog** с авто-генерацией через `git-cliff` из Conventional Commits. Категории: Added, Changed, Deprecated, Removed, Fixed, Security.

---

## 3. C23-специфичные конфигурации проекта

### CMakeLists.txt — modern CMake для библиотеки

```cmake
cmake_minimum_required(VERSION 3.20...4.2)
project(liboas
    VERSION 1.0.0
    LANGUAGES C
    DESCRIPTION "OpenAPI Schema library for C23"
    HOMEPAGE_URL "https://github.com/user/liboas"
)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

# Основной target
add_library(oas
    src/oas.c
    src/parser.c
)
add_library(oas::oas ALIAS oas)

set_target_properties(oas PROPERTIES
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
    C_STANDARD 23
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF
)

target_include_directories(oas
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# Зависимость от liburing через pkg-config
find_package(PkgConfig REQUIRED)
pkg_check_modules(URING REQUIRED IMPORTED_TARGET liburing)
target_link_libraries(oas PRIVATE PkgConfig::URING)

# Установка
install(TARGETS oas EXPORT oasTargets
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

# CMake config для find_package(oas)
install(EXPORT oasTargets
    FILE oasTargets.cmake
    NAMESPACE oas::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/oas
)
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/oasConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)
configure_package_config_file(
    cmake/oasConfig.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/oasConfig.cmake"
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/oas
)
install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/oasConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/oasConfigVersion.cmake"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/oas
)

# pkg-config
configure_file(liboas.pc.in ${CMAKE_CURRENT_BINARY_DIR}/liboas.pc @ONLY)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/liboas.pc
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)

# Тесты
option(BUILD_TESTING "Build tests" ON)
if(BUILD_TESTING)
    enable_testing()
    add_subdirectory(tests)
endif()
```

Потребитель после `sudo make install` использует: `find_package(oas 1.0 REQUIRED)` и `target_link_libraries(app PRIVATE oas::oas)`.

### CMakePresets.json — пресеты для разработки и CI

Полный набор пресетов покрывает все сценарии: разработка, CI, релиз, каждый санитайзер, покрытие.

```json
{
  "version": 6,
  "cmakeMinimumRequired": { "major": 3, "minor": 25, "patch": 0 },
  "configurePresets": [
    {
      "name": "base",
      "hidden": true,
      "generator": "Ninja",
      "binaryDir": "${sourceDir}/build/${presetName}",
      "cacheVariables": {
        "CMAKE_EXPORT_COMPILE_COMMANDS": "ON",
        "CMAKE_C_STANDARD": "23",
        "CMAKE_C_STANDARD_REQUIRED": "ON",
        "CMAKE_C_EXTENSIONS": "OFF"
      }
    },
    {
      "name": "dev-gcc",
      "displayName": "Dev GCC 15",
      "inherits": "base",
      "cacheVariables": {
        "CMAKE_C_COMPILER": "gcc-15",
        "CMAKE_BUILD_TYPE": "Debug",
        "CMAKE_C_FLAGS": "-Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wformat=2"
      }
    },
    {
      "name": "dev-clang",
      "displayName": "Dev Clang",
      "inherits": "base",
      "cacheVariables": {
        "CMAKE_C_COMPILER": "clang",
        "CMAKE_BUILD_TYPE": "Debug",
        "CMAKE_C_FLAGS": "-Wall -Wextra -Wpedantic -Wconversion -Wshadow"
      }
    },
    {
      "name": "release",
      "displayName": "Release (LTO)",
      "inherits": "base",
      "cacheVariables": {
        "CMAKE_BUILD_TYPE": "Release",
        "CMAKE_INTERPROCEDURAL_OPTIMIZATION": "ON",
        "CMAKE_C_FLAGS": "-Wall -Wextra -Wpedantic"
      }
    },
    {
      "name": "asan",
      "displayName": "ASan + UBSan",
      "inherits": "base",
      "cacheVariables": {
        "CMAKE_C_COMPILER": "clang",
        "CMAKE_BUILD_TYPE": "Debug",
        "CMAKE_C_FLAGS_DEBUG": "-O1 -g3 -fno-omit-frame-pointer -fsanitize=address,undefined -fno-sanitize-recover=all"
      },
      "environment": {
        "ASAN_OPTIONS": "detect_leaks=1:detect_stack_use_after_return=1",
        "UBSAN_OPTIONS": "print_stacktrace=1:halt_on_error=1"
      }
    },
    {
      "name": "tsan",
      "displayName": "ThreadSanitizer",
      "inherits": "base",
      "cacheVariables": {
        "CMAKE_C_COMPILER": "clang",
        "CMAKE_BUILD_TYPE": "Debug",
        "CMAKE_C_FLAGS_DEBUG": "-O1 -g3 -fno-omit-frame-pointer -fsanitize=thread"
      },
      "environment": { "TSAN_OPTIONS": "halt_on_error=1:second_deadlock_stack=1" }
    },
    {
      "name": "msan",
      "displayName": "MSan (Clang only)",
      "inherits": "base",
      "cacheVariables": {
        "CMAKE_C_COMPILER": "clang",
        "CMAKE_BUILD_TYPE": "Debug",
        "CMAKE_C_FLAGS_DEBUG": "-O1 -g3 -fno-omit-frame-pointer -fsanitize=memory -fsanitize-memory-track-origins=2"
      }
    },
    {
      "name": "coverage",
      "displayName": "Coverage (gcov)",
      "inherits": "base",
      "cacheVariables": {
        "CMAKE_C_COMPILER": "gcc-15",
        "CMAKE_BUILD_TYPE": "Debug",
        "CMAKE_C_FLAGS_DEBUG": "-O0 -g --coverage -fprofile-arcs -ftest-coverage",
        "CMAKE_EXE_LINKER_FLAGS": "--coverage"
      }
    }
  ],
  "buildPresets": [
    { "name": "dev-gcc",    "configurePreset": "dev-gcc" },
    { "name": "dev-clang",  "configurePreset": "dev-clang" },
    { "name": "release",    "configurePreset": "release" },
    { "name": "asan",       "configurePreset": "asan" },
    { "name": "tsan",       "configurePreset": "tsan" },
    { "name": "msan",       "configurePreset": "msan" },
    { "name": "coverage",   "configurePreset": "coverage" }
  ],
  "testPresets": [
    {
      "name": "dev-gcc",
      "configurePreset": "dev-gcc",
      "output": { "outputOnFailure": true }
    },
    {
      "name": "asan",
      "configurePreset": "asan",
      "output": { "outputOnFailure": true }
    }
  ]
}
```

**Важные правила**: ASan и TSan **несовместимы** в одной сборке — всегда отдельные пресеты. MSan — **только Clang**, требует инструментированного libc. `CMakeUserPresets.json` добавляется в `.gitignore` — это файл для локальных переопределений разработчика.

### .clang-format для C23

```yaml
---
BasedOnStyle: LLVM
Language: Cpp           # clang-format использует Cpp для C и C++
Standard: Latest        # Критично для C23: [[nodiscard]], typeof, auto
IndentWidth: 4
TabWidth: 4
UseTab: Never
ColumnLimit: 100

BreakBeforeBraces: Custom
BraceWrapping:
  AfterFunction: true         # Linux-стиль: { на новой строке для функций
  AfterStruct: false
  AfterEnum: false
  AfterControlStatement: Never
  BeforeElse: false

PointerAlignment: Right       # int *ptr, а не int* ptr
DerivePointerAlignment: false
AlignConsecutiveMacros: AcrossEmptyLines

SortIncludes: CaseSensitive
IncludeBlocks: Regroup
IncludeCategories:
  - Regex: '^".*"'
    Priority: 1               # Локальные заголовки первыми
  - Regex: '^<sys/.*>'
    Priority: 2               # Системные
  - Regex: '^<.*>'
    Priority: 3               # Остальные

SpaceBeforeParens: ControlStatements
MaxEmptyLinesToKeep: 1
ReflowComments: true
```

### .clang-tidy для системного C-кода

```yaml
---
Checks: >
  -*,
  bugprone-*,
  cert-*,
  clang-analyzer-*,
  concurrency-*,
  misc-*,
  performance-*,
  portability-*,
  readability-*,
  -bugprone-easily-swappable-parameters,
  -readability-magic-numbers,
  -readability-identifier-length,
  -misc-no-recursion

HeaderFilterRegex: '(include|src)/.*\.(h)$'
WarningsAsErrors: >
  bugprone-null-dereference,
  clang-analyzer-core.*

FormatStyle: file
CheckOptions:
  readability-identifier-naming.FunctionCase: lower_case
  readability-identifier-naming.VariableCase: lower_case
  readability-identifier-naming.MacroDefinitionCase: UPPER_CASE
  readability-identifier-naming.StructCase: lower_case
  readability-identifier-naming.EnumConstantCase: UPPER_CASE
```

C++-специфичные категории (`modernize-*`, `cppcoreguidelines-*`, `abseil-*`) намеренно исключены — они не применимы к C-коду. Для io_uring-проектов особенно важны проверки `concurrency-*` и `bugprone-*`, выявляющие гонки данных и обращения к памяти.

### .clangd, .editorconfig, .gitignore

**.clangd** указывает на `compile_commands.json` и включает inline-проверки clang-tidy:

```yaml
CompileFlags:
  CompilationDatabase: build/
  Add: [-std=c2x, -Wall, -Wextra]
  Remove: [-fstack-clash-protection]  # GCC-specific
Diagnostics:
  ClangTidy:
    Add: [bugprone-*, cert-*, clang-analyzer-*]
  UnusedIncludes: Strict
InlayHints:
  Enabled: true
  ParameterNames: true
```

`compile_commands.json` генерируется через `CMAKE_EXPORT_COMPILE_COMMANDS=ON` (уже включено в базовом пресете) и симлинкуется в корень: `ln -sf build/dev-gcc/compile_commands.json .`

**.editorconfig** задаёт: `indent_size = 4` для `.c/.h`, `indent_size = 2` для CMake/YAML/JSON, `indent_style = tab` для Makefile, `trim_trailing_whitespace = false` для `.md`.

**.gitignore** исключает: `build/`, `cmake-build-*/`, `*.o`, `*.so`, `*.a`, `/compile_commands.json` (симлинк), `.cache/`, `.vscode/`, `.idea/`, `CMakeUserPresets.json`, `*.gcno`, `*.gcda`, `lcov.info`.

### Containerfile для Podman

```dockerfile
# Containerfile — мультиэтапная сборка для C23
# Сборка:  podman build -t ringwall:latest .
# Запуск:  podman run --rm --cap-add=NET_ADMIN ringwall:latest

# === Стадия 1: Сборка ===
FROM registry.fedoraproject.org/fedora:42 AS builder

RUN dnf install -y --setopt=install_weak_deps=False \
        gcc cmake ninja-build pkg-config liburing-devel \
    && dnf clean all

WORKDIR /src
COPY CMakeLists.txt CMakePresets.json ./
COPY cmake/ cmake/
COPY include/ include/
COPY src/ src/

RUN cmake --preset release && cmake --build --preset release

# === Стадия 2: Минимальный рантайм ===
FROM registry.fedoraproject.org/fedora-minimal:42

RUN microdnf install -y liburing && microdnf clean all
RUN useradd --system --no-create-home appuser

COPY --from=builder /src/build/release/bin/ringwall /usr/local/bin/
USER appuser
ENTRYPOINT ["/usr/local/bin/ringwall"]
```

**Containerfile vs Dockerfile**: синтаксис идентичен. Различия — в рантайме: Podman работает **без демона** (daemonless), **rootless по умолчанию**, выдаёт OCI-формат образов. Podman читает и `Containerfile`, и `Dockerfile`; файл игнорирования — `.containerignore` (но `.dockerignore` тоже поддерживается).

---

## 4. GitHub-специфичные файлы и CI/CD

### Шаблоны issues с YAML-формами

Современные YAML-формы (не markdown-шаблоны) дают структурированный ввод с dropdown, checkbox, textarea:

**`.github/ISSUE_TEMPLATE/bug_report.yml`** — ключевые поля для C/ioh_uring проекта:

```yaml
name: "🐛 Bug Report"
description: "Report a bug"
labels: ["bug", "triage"]
body:
  - type: dropdown
    id: compiler
    attributes:
      label: Compiler
      options: [GCC 15, GCC 14, Clang 20, Clang 19, Other]
    validations: { required: true }

  - type: input
    id: kernel-version
    attributes:
      label: "Kernel Version (uname -r)"
      placeholder: "6.12.0-1-generic"

  - type: textarea
    id: sanitizer-output
    attributes:
      label: "Sanitizer Output (ASan/UBSan/TSan)"
      render: shell

  - type: checkboxes
    id: terms
    attributes:
      label: Checklist
      options:
        - label: "I searched existing issues"
          required: true
```

Поле **версии ядра** критично для io_uring-проектов: разные операции появились в разных версиях ядра (multishot accept — 5.19+, zero-copy send — 6.0+, buffer rings — 6.1+).

### Pull Request Template

**`.github/PULL_REQUEST_TEMPLATE.md`** с чеклистом для C-кода:

```markdown
## Description
<!-- Опишите изменения. Closes #123 -->

## C Code Quality Checklist
- [ ] Компилируется без предупреждений с `-Wall -Wextra -Werror -pedantic -std=c23`
- [ ] `clang-format` применён (соответствует `.clang-format`)
- [ ] ASan + UBSan проходят без ошибок
- [ ] TSan проходит (если затронут многопоточный код)
- [ ] `clang-tidy` проверки пройдены
- [ ] Добавлены тесты для нового кода
- [ ] Документация обновлена (doc-comments в заголовках)
- [ ] Запись в CHANGELOG добавлена
```

### GitHub Actions: матричная сборка

**`.github/workflows/ci.yml`** — базовый CI:

```yaml
name: CI
on: [push, pull_request]
concurrency:
  group: ${{ github.workflow }}-${{ github.ref }}
  cancel-in-progress: true

jobs:
  build:
    name: "${{ matrix.compiler }} / ${{ matrix.build_type }}"
    runs-on: ubuntu-latest
    strategy:
      fail-fast: false
      matrix:
        compiler: [gcc-15, clang-19]
        build_type: [Debug, Release]
    steps:
      - uses: actions/checkout@v4
      - name: Install compiler
        run: |
          if [[ "${{ matrix.compiler }}" == "gcc-15" ]]; then
            sudo add-apt-repository -y ppa:ubuntu-toolchain-r/test
            sudo apt-get install -y gcc-15
            echo "CC=gcc-15" >> $GITHUB_ENV
          else
            wget https://apt.llvm.org/llvm.sh && chmod +x llvm.sh
            sudo ./llvm.sh 19
            echo "CC=clang-19" >> $GITHUB_ENV
          fi
      - name: Configure
        run: cmake -S . -B build -DCMAKE_BUILD_TYPE=${{ matrix.build_type }}
             -DCMAKE_C_STANDARD=23 -DBUILD_TESTING=ON
      - name: Build
        run: cmake --build build --parallel $(nproc)
      - name: Test
        run: ctest --test-dir build --output-on-failure --parallel $(nproc)
```

### GitHub Actions: санитайзеры

Выделяются в **отдельный workflow** `sanitizers.yml` с тремя независимыми job'ами:

- **asan-ubsan**: `-fsanitize=address,undefined -fno-omit-frame-pointer` + `ASAN_OPTIONS=detect_leaks=1:halt_on_error=1`
- **tsan**: `-fsanitize=thread` (критично для io_uring — асинхронная модель с completion callbacks) + `TSAN_OPTIONS=halt_on_error=1`
- **msan**: `-fsanitize=memory -fsanitize-memory-track-origins=2` (только Clang, требует инструментированного libc)

**Стратегия запуска**: ASan+UBSan на **каждый PR** (быстрая обратная связь, ~2x overhead). TSan — **ночные билды** (5-15x overhead). MSan — **еженедельно** (сложнейший в настройке).

### GitHub Actions: статический анализ

Три инструмента в одном workflow:

- **clang-tidy**: `clang-tidy -p build/ $(find src include -name '*.c' -o -name '*.h') --warnings-as-errors='*'`
- **cppcheck**: `cppcheck --project=build/compile_commands.json --enable=all --error-exitcode=1 --std=c23`
- **CodeQL**: `github/codeql-action/init@v3` с `languages: c-cpp` и `queries: security-and-quality`

### GitHub Actions: покрытие кода

Workflow `coverage.yml` собирает с `--coverage`, запускает тесты, собирает данные `lcov`, загружает в **Codecov**:

```yaml
- name: Capture coverage
  run: |
    lcov --capture --directory build -o coverage.info --rc lcov_branch_coverage=1
    lcov --remove coverage.info '/usr/*' '*/tests/*' -o coverage.info
- uses: codecov/codecov-action@v5
  with:
    files: coverage.info
```

### GitHub Actions: релиз

Workflow `release.yml` триггерится на push тега `v*`, собирает Release-билд, создаёт пакеты через **CPack** (`.tar.gz`, `.deb`), загружает артефакты и создаёт **GitHub Release** через `softprops/action-gh-release@v2` с автоматическими release notes.

### CODEOWNERS и dependabot.yml

**CODEOWNERS** назначает ревьюеров: `@core-team` для `src/` и `include/`, `@devops` для `.github/workflows/`, `@docs-team` для `*.md`.

**dependabot.yml** — только для `github-actions` ecosystem (dependabot **не поддерживает** CMake/Conan нативно). Для C-зависимостей используется **Renovate** как альтернатива с поддержкой кастомных regex-менеджеров.

---

## 5. Возможности C23 и их влияние на проект

GCC 15 сделал **`-std=gnu23` стандартом по умолчанию** — это переломный момент для экосистемы. Ключевые возможности C23, которые меняют подход к написанию и документированию кода:

**`nullptr` и `nullptr_t`** заменяют `NULL` / `(void*)0`. clang-tidy проверка `modernize-use-nullptr` автоматически находит замены. В документации API используйте `nullptr` вместо `NULL`.

**`[[nodiscard]]`** и **`[[deprecated]]`** — стандартные атрибуты для API. `[[nodiscard]]` на функциях, возвращающих error code (критично для io_uring операций, где игнорирование ошибки = баг):

```c
[[nodiscard]] int oas_parse(const char *input, struct oas_doc *doc);
[[deprecated("Use oas_parse_v2")]] int oas_parse_legacy(const char *input);
```

**`constexpr`** для объектов позволяет заменить `#define MAX_EVENTS 1024` на `constexpr int MAX_EVENTS = 1024;` — безопаснее, типизировано, видно в дебаггере.

**`auto`** (вывод типа для переменных) и **`typeof`** / **`typeof_unqual`** упрощают обобщённые макросы.

**`#embed`** — директива включения бинарных данных: `unsigned char icon[] = { #embed "icon.png" };`. GCC 15 реализовал с **значительными оптимизациями производительности**.

**`bool`, `true`, `false`** — теперь ключевые слова, `<stdbool.h>` больше не нужен.

**`static_assert` без сообщения**, **бинарные литералы** (`0b11001010`), **разделители цифр** (`1'000'000`), **`_BitInt(N)`** — все поддерживаются GCC 15 и Clang 19+.

**Влияние на конфигурацию**: в `.clang-format` установить `Standard: Latest`. В `CMakeLists.txt` — `CMAKE_C_STANDARD 23`. Doxygen корректно обрабатывает новый синтаксис при включённом `OPTIMIZE_OUTPUT_FOR_C = YES`.

---

## 6. Фаззинг, санитайзеры и статический анализ

### Организация фаззинга

Директория `fuzz/` с отдельными целями для каждого парсера или обработчика:

```
fuzz/
├── CMakeLists.txt
├── fuzz_parser.c          # libFuzzer харнесс
├── fuzz_protocol.c
├── corpus/                # Начальные входные данные
│   ├── parser/
│   └── protocol/
└── dictionaries/
    └── protocol.dict      # Токены для ускорения фаззинга
```

**Харнесс libFuzzer**:

```c
#include <stdint.h>
#include <stddef.h>
#include <liboas/oas.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    struct oas_doc doc = {0};
    oas_parse((const char *)data, size, &doc);
    oas_free(&doc);
    return 0;
}
```

Компиляция: `clang -fsanitize=fuzzer,address -g -O2 fuzz_parser.c -loas -o fuzz_parser`. Запуск: `./fuzz_parser corpus/parser/ -max_len=4096 -jobs=4`.

**AFL++** совместим с libFuzzer-харнессами и даёт **5-10x ускорение** через persistent mode (`__AFL_LOOP()`).

**OSS-Fuzz** — бесплатный непрерывный фаззинг для open-source проектов. Интеграция требует 3 файла: `project.yaml`, `Dockerfile`, `build.sh`. **CIFuzz** — GitHub Action для фаззинга на каждый PR, использует инфраструктуру OSS-Fuzz.

### Пайплайн статического анализа

Рекомендуемая комбинация: **clang-tidy** (основной, наибольшее покрытие) + **cppcheck** (дополнительный, нулевые false positives как цель) + **CodeQL** (нативный GitHub, семантические запросы). Для дополнительного покрытия: **PVS-Studio** (бесплатен для open-source), **Coverity Scan** (бесплатен, ~2 сборки/день), **CodeChecker** (мета-инструмент, объединяющий clang-tidy + Clang Static Analyzer + cppcheck + GCC Static Analyzer + Facebook Infer).

---

## 7. CMake 4.x и управление зависимостями

### Что изменилось в CMake 4.0–4.3

**CMake 4.0** (начало 2025) поднял минимальную версию политик до **3.5** — вызовы `cmake_minimum_required()` ниже 3.5 теперь дают ошибку. Рекомендуемый паттерн: `cmake_minimum_required(VERSION 3.20...4.2)` для широкой совместимости.

**CMake 4.2** (ноябрь 2025) добавил генератор FASTBuild, поддержку Emscripten, `cmake -E copy_if_newer`, `cmake_language(TRACE)`.

**CMake 4.3** (RC, март 2026) — наиболее значимый релиз: **Common Package Specification (CPS)**, `project(... SPDX_LICENSE ...)`, `export(SBOM ...)` для генерации SBOM прямо из CMake. `import std` — **только для C++**, C не затронут.

Для C23 в CMake нет специфичных изменений: `CMAKE_C_STANDARD 23` работает с CMake 3.21+. Новые возможности 4.x касаются в основном инфраструктуры пакетирования и диагностики.

### Управление зависимостями: четыре подхода

| Подход | Когда использовать | Пример для liburing |
|--------|-------------------|---------------------|
| **find_package()** | Зависимость в системе | `pkg_check_modules(URING REQUIRED liburing)` |
| **FetchContent** | Vendor'инг исходников | `FetchContent_Declare(liburing GIT_TAG liburing-2.7)` |
| **Conan 2.x** | Сложное дерево зависимостей | `conanfile.txt` + профили gcc-15/clang |
| **vcpkg** | Кроссплатформенные зависимости | `vcpkg.json` manifest mode |

**Рекомендация для io_uring Linux-проектов**: `find_package()` + `pkg_check_modules()` как основной метод (liburing — системная зависимость на Linux). `FetchContent` — как fallback, если пакет не найден. Conan/vcpkg — если проект имеет сложное дерево зависимостей.

### CPack для создания пакетов

```cmake
set(CPACK_GENERATOR "TGZ;DEB;RPM")
set(CPACK_DEBIAN_PACKAGE_DEPENDS "liburing2 (>= 2.5)")
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
set(CPACK_RPM_PACKAGE_LICENSE "MIT")
set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
set(CPACK_RPM_FILE_NAME RPM-DEFAULT)
include(CPack)
```

Генерация: `cd build && cpack -G DEB && cpack -G RPM && cpack -G TGZ`.

---

## 8. Инструменты документации

### Doxygen для C23 API

Ключевые настройки `Doxyfile`:

```
OPTIMIZE_OUTPUT_FOR_C  = YES     # Критично: без этого Doxygen трактует код как C++
GENERATE_HTML          = YES
GENERATE_MAN           = YES
GENERATE_XML           = YES     # Для Sphinx+Breathe
MAN_EXTENSION          = .3      # Секция 3 — библиотечные функции
INPUT                  = include/ src/
FILE_PATTERNS          = *.h *.c
EXTRACT_ALL            = YES
```

Стиль комментариев — **JavaDoc** (`/** */`), самый распространённый в C-проектах:

```c
/**
 * @brief Parse an OpenAPI schema document.
 *
 * @param[in]  input  Input string (null-terminated)
 * @param[out] doc    Parsed document structure
 * @return 0 on success, negative errno on failure
 *
 * @note Minimum kernel: Linux 5.10+ (for io_uring buffered reads)
 * @since 1.0.0
 */
[[nodiscard]] int oas_parse(const char *input, struct oas_doc *doc);
```

### Sphinx + Breathe для hosted-документации

Пайплайн: C-код → Doxygen (XML) → Breathe → Sphinx (HTML). В `docs/conf.py`:

```python
extensions = ['breathe']
breathe_projects = {"liboas": "../build/docs/xml"}
breathe_default_project = "liboas"
```

В RST: `.. doxygenfunction:: oas_parse` автоматически подтягивает документацию из заголовков.

### MkDocs Material как альтернатива

Проще в настройке, Markdown-нативный, красивая тема по умолчанию. Оптимален для пользовательских руководств, но **не имеет нативной интеграции с Doxygen** для API-документации. Решение: генерировать API-доку отдельно через Doxygen, ссылаться из MkDocs.

### Man-страницы через scdoc

**scdoc** — минимальный инструмент (< 1000 строк C99, нулевые зависимости), идеален для C-проектов:

```
ringwall(1)

# NAME

ringwall - io_uring-based network firewall

# SYNOPSIS

*ringwall* [*-c* _config_] [*-d*] [*-v*]

# OPTIONS

*-c* _config_
	Path to configuration file (default: /etc/ringwall.conf)

*-d*
	Run as daemon

# SEE ALSO

*iptables*(8), *nftables*(8)
```

Сборка: `scdoc < ringwall.1.scd > ringwall.1`. Установка через CMake в `${CMAKE_INSTALL_MANDIR}/man1`.

Для библиотечных функций (секция 3) лучше использовать **Doxygen** с `GENERATE_MAN = YES` — он автоматически генерирует man-страницу для каждой функции.

### Двуязычная документация (EN + RU)

Для малых/средних проектов: **`README.md` + `README.ru.md`** с переключателем в начале каждого файла. Для масштабирования: **MkDocs Material + mkdocs-static-i18n** (файлы `index.md` / `index.ru.md`) или **Sphinx + gettext i18n** (для полного перевода через .po-файлы).

---

## 9. Безопасность цепочки поставок

### SBOM (Software Bill of Materials)

C-проекты создают уникальную проблему: нет централизованного менеджера пакетов, статическое линкование скрывает границы компонентов. Инструменты:

- **cmake-sbom** (DEMCON): `sbom_generate()` + `sbom_add()` прямо в CMakeLists.txt
- **CMake 4.3**: нативный `export(SBOM ...)` — генерация SBOM из самого CMake
- **Syft** (Anchore): сканирует Conan lockfiles, CMake файлы, vcpkg манифесты
- **CycloneDX для Conan**: `cyclonedx-conan --output sbom.json`

Форматы: **SPDX** (Linux Foundation, сильна в лицензионном compliance) и **CycloneDX** (OWASP, фокус на уязвимостях). Рекомендуется генерировать оба формата и прикреплять к релизам.

### OpenSSF Scorecard и Best Practices Badge

**OpenSSF Scorecard** — 18 автоматических проверок: branch protection, code review, signed releases, SECURITY.md, фаззинг, SAST, CI-тесты. GitHub Action: `ossf/scorecard-action@v2`. Бейдж для README: `[![OpenSSF Scorecard](https://api.scorecard.dev/projects/github.com/{owner}/{repo}/badge)]`.

**OpenSSF Best Practices Badge** (уровень Passing): требует задокументированный процесс сборки, автоматические тесты, процесс реагирования на уязвимости, включённые предупреждения компилятора (`-Wall -Wextra`), использование статического анализа и динамической проверки (санитайзеры/фаззинг).

### Подпись и верификация

**GPG-подпись тегов**: `git tag -s v1.0.0 -m "Release v1.0.0"`.

**Sigstore/cosign** для keyless-подписи релизных бинарников через OIDC-токены GitHub Actions (не нужно управлять ключами).

**SLSA Level 3** через `slsa-framework/slsa-github-generator` или новый `actions/attest-build-provenance` — генерирует нефальсифицируемую provenance-запись, привязанную к конкретному workflow.

**Воспроизводимые сборки** замыкают цепочку доверия. Основные источники невоспроизводимости в C: `__DATE__`/`__TIME__` (решение: `-Werror=date-time`), пути в отладочной информации (решение: `-ffile-prefix-map=${CMAKE_SOURCE_DIR}=.`), `SOURCE_DATE_EPOCH` для timestamp'ов (GCC ≥ 7, Clang ≥ 16, CMake ≥ 3.8 — все уважают эту переменную). Верификация: `diffoscope build1/binary build2/binary`.

### Документирование требований io_uring к ядру

io_uring — быстро развивающийся API, где каждая версия ядра добавляет операции. Критически важно документировать:

| Возможность | Минимальное ядро |
|-------------|-----------------|
| Базовый io_uring | 5.1 |
| Практический сетевой I/O | 5.5 |
| `io_uring_probe` | 5.6 |
| Restrictions API (production-ready) | 5.10 LTS |
| Multishot accept | 5.19 |
| Zero-copy send, buffer rings | 6.0+ |
| Улучшенные buffer rings | 6.1+ |

**В коде**: `io_uring_probe` для runtime-определения доступности операций:

```c
struct io_uring_probe *probe = io_uring_get_probe();
if (!probe || !io_uring_opcode_supported(probe, IORING_OP_ACCEPT))
    return -ENOTSUP;
io_uring_free_probe(probe);
```

**В README**: таблица «Minimum Kernel Requirements» с указанием, какие функции проекта требуют какой версии ядра.

---

## 10. Уроки лучших C-проектов

Анализ структуры ведущих open-source C-проектов выявляет консенсусные паттерны:

**liburing** — каноничный пример для io_uring-проекта. Публичные заголовки в `src/include/liburing/`, приватные рядом с исходниками в `src/`. Symbol versioning через `.map`-файлы для ABI-стабильности. Огромный тестовый набор (один файл = один тест). Man-страницы в традиционном groff-формате. Двойная лицензия LGPL/MIT.

**curl** — золотой стандарт для комбинированного library+CLI проекта. `lib/` для libcurl, `src/` для CLI, `include/curl/` для публичных заголовков. Два build-системы (CMake + autotools). Образцовый SECURITY.md и процесс CVE (curl — CNA). Таксономия «C-ошибок» как модель для любого C-проекта.

**mimalloc** — эталон CMake-интеграции для C-библиотеки. Чистый `include/` с минимумом публичных заголовков. Предоставляет `mimalloc-config.cmake` для `find_package()`. Платформо-специфичный код изолирован в `src/prim/{unix,windows}/`.

**Valkey** (форк Redis, BSD-3-Clause) — плоская структура `src/` с исходниками и заголовками вместе. Vendor'ированные зависимости в `deps/`. Недавно добавил CMake помимо традиционного Makefile. Конфигурационный файл в корне проекта.

**Общий паттерн**: зрелые проекты **всегда** разделяют публичные и приватные заголовки, **всегда** имеют обширные тесты, **почти всегда** предоставляют и `pkg-config`, и CMake config-файлы для потребителей.

---

## Заключение

Современный C23-проект на GitHub — это не просто код, а **экосистема инструментов и практик**. Минимальный viable open-source проект в 2025–2026 требует: target-based CMake с пресетами (dev/release/asan/tsan), `.clang-format` + `.clang-tidy` для единообразия кода, GitHub Actions с матричной сборкой GCC/Clang и санитайзерами на каждый PR, структурированные issue-шаблоны с полями для версии ядра и компилятора, SECURITY.md с перечислением классов memory-safety уязвимостей, и SPDX-заголовки в каждом файле.

Три ключевых инсайта, выходящих за рамки «стандартного чеклиста»: **ThreadSanitizer критически важен для io_uring** — асинхронная модель с completion callbacks порождает тонкие data races, невидимые для ASan; **CMake 4.3 с нативным `export(SBOM ...)`** меняет подход к supply chain security — SBOM генерируется прямо из build-системы, а не отдельным инструментом; **`[[nodiscard]]` из C23 — не косметика, а средство безопасности** для io_uring-проектов, где каждая операция возвращает error code, игнорирование которого = потенциальная уязвимость.
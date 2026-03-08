# ============================================================================
# iohttp — Embedded HTTP server for C23
# io_uring · wolfSSL · HTTP/1.1·2·3
# ============================================================================

cmake_minimum_required(VERSION 3.25)

project(iohttp
    VERSION 0.1.0
    DESCRIPTION "Embedded HTTP server for C23 with io_uring and wolfSSL"
    LANGUAGES C
)

# ============================================================================
# C23 Standard (mandatory)
# ============================================================================

set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# ============================================================================
# Compile Options
# ============================================================================

add_compile_options(
    -Wall -Wextra -Wpedantic -Werror
    -Wformat=2 -Wformat-overflow=2 -Wformat-truncation=2
    -Wconversion -Wsign-conversion
    -Wdouble-promotion -Wfloat-equal
    -Wshadow -Wundef
    -Wunused -Wuninitialized
    -Wnull-dereference
    -Wstrict-prototypes -Wmissing-prototypes -Wmissing-declarations
    -Wredundant-decls
    -Wvla
    -fstack-protector-strong
    -D_FORTIFY_SOURCE=3
    -fPIE
)

add_link_options(
    -pie
    -Wl,-z,relro
    -Wl,-z,now
)

# ============================================================================
# _GNU_SOURCE (required for io_uring, signalfd, etc.)
# ============================================================================

add_compile_definitions(_GNU_SOURCE)

# ============================================================================
# Dependencies
# ============================================================================

# pkg-config
find_package(PkgConfig REQUIRED)

# liburing (mandatory)
pkg_check_modules(LIBURING REQUIRED liburing>=2.7)

# wolfSSL (mandatory)
pkg_check_modules(WOLFSSL REQUIRED wolfssl>=5.8)

# picohttpparser (bundled — header-only + one .c file)
# Will be added as source in src/http/

# nghttp2 (HTTP/2)
pkg_check_modules(NGHTTP2 nghttp2)

# ngtcp2 (QUIC transport)
pkg_check_modules(NGTCP2 libngtcp2)
pkg_check_modules(NGTCP2_WOLFSSL libngtcp2_crypto_wolfssl)

# nghttp3 (HTTP/3)
pkg_check_modules(NGHTTP3 libnghttp3)

# yyjson (JSON)
find_path(YYJSON_INCLUDE_DIR NAMES yyjson.h)
find_library(YYJSON_LIBRARY NAMES yyjson)

# ============================================================================
# Testing
# ============================================================================

if(BUILD_TESTING)
    enable_testing()

    # Unity test framework (bundled or system)
    find_path(UNITY_INCLUDE_DIR NAMES unity.h PATH_SUFFIXES unity)
    find_library(UNITY_LIBRARY NAMES unity)

    if(UNITY_INCLUDE_DIR AND UNITY_LIBRARY)
        message(STATUS "Found Unity: ${UNITY_LIBRARY}")
    else()
        message(WARNING "Unity test framework not found — tests disabled")
    endif()

    # Test helper macro
    macro(io_add_test TEST_NAME TEST_SRC)
        if(UNITY_INCLUDE_DIR AND UNITY_LIBRARY)
            add_executable(${TEST_NAME} ${TEST_SRC})
            target_include_directories(${TEST_NAME} PRIVATE
                ${CMAKE_SOURCE_DIR}/src
                ${UNITY_INCLUDE_DIR}
            )
            target_link_libraries(${TEST_NAME} PRIVATE
                ${UNITY_LIBRARY}
                ${ARGN}
            )
            add_test(NAME ${TEST_NAME} COMMAND ${TEST_NAME})
        endif()
    endmacro()
endif()

# ============================================================================
# Code Quality Targets
# ============================================================================

# clang-format
find_program(CLANG_FORMAT clang-format)
if(CLANG_FORMAT)
    file(GLOB_RECURSE ALL_C_SOURCES
        ${CMAKE_SOURCE_DIR}/src/*.c
        ${CMAKE_SOURCE_DIR}/src/*.h
        ${CMAKE_SOURCE_DIR}/tests/*.c
    )
    add_custom_target(format
        COMMAND ${CLANG_FORMAT} -i ${ALL_C_SOURCES}
        COMMENT "Running clang-format"
        VERBATIM
    )
    add_custom_target(format-check
        COMMAND ${CLANG_FORMAT} --dry-run --Werror ${ALL_C_SOURCES}
        COMMENT "Checking clang-format compliance"
        VERBATIM
    )
endif()

# clang-tidy
find_program(CLANG_TIDY clang-tidy)
if(CLANG_TIDY)
    set(CMAKE_C_CLANG_TIDY ${CLANG_TIDY})
    add_custom_target(lint
        COMMAND ${CLANG_TIDY} -p ${CMAKE_BINARY_DIR}
            ${CMAKE_SOURCE_DIR}/src/**/*.c
        COMMENT "Running clang-tidy"
        VERBATIM
    )
endif()

# cppcheck
find_program(CPPCHECK cppcheck)
if(CPPCHECK)
    add_custom_target(cppcheck
        COMMAND ${CPPCHECK}
            --enable=all --inconclusive --std=c23
            --suppress=missingIncludeSystem
            -I ${CMAKE_SOURCE_DIR}/src
            ${CMAKE_SOURCE_DIR}/src/
        COMMENT "Running cppcheck"
        VERBATIM
    )
endif()

# Doxygen
find_package(Doxygen)
if(DOXYGEN_FOUND)
    add_custom_target(docs
        COMMAND ${DOXYGEN_EXECUTABLE} ${CMAKE_SOURCE_DIR}/Doxyfile
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        COMMENT "Generating API documentation with Doxygen"
        VERBATIM
    )
endif()

# ============================================================================
# Fuzz Testing (Clang only)
# ============================================================================

macro(io_add_fuzz FUZZ_NAME FUZZ_SRC)
    if(CMAKE_C_COMPILER_ID STREQUAL "Clang")
        add_executable(${FUZZ_NAME} ${FUZZ_SRC})
        target_include_directories(${FUZZ_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/src)
        target_link_libraries(${FUZZ_NAME} PRIVATE -fsanitize=fuzzer ${ARGN})
        target_compile_options(${FUZZ_NAME} PRIVATE -fsanitize=fuzzer-no-link)
    endif()
endmacro()

# ============================================================================
# Libraries (to be populated per sprint)
# ============================================================================

# Sprint 1: Core io_uring event loop
# Sprint 2: wolfSSL TLS integration
# Sprint 3: HTTP/1.1 parser (picohttpparser)
# Sprint 4: Router and middleware
# Sprint 5: Static files and WebSocket
# Sprint 6: HTTP/2 (nghttp2)
# Sprint 7: HTTP/3 (ngtcp2 + nghttp3)
# Sprint 8: Polish, benchmarks, packaging

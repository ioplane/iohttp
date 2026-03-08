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
    -Wformat=2
    -Wconversion -Wsign-conversion
    -Wdouble-promotion -Wfloat-equal
    -Wshadow -Wundef
    -Wunused -Wuninitialized
    -Wnull-dereference
    -Wstrict-prototypes -Wmissing-prototypes -Wmissing-declarations
    -Wredundant-decls
    -Wvla
    -fstack-protector-strong
    -fPIE
)

# GCC-specific warning flags (not supported by Clang)
if(CMAKE_C_COMPILER_ID STREQUAL "GNU")
    add_compile_options(
        -Wformat-overflow=2 -Wformat-truncation=2
    )
endif()

# _FORTIFY_SOURCE requires optimization; only enable for Release/RelWithDebInfo
if(NOT CMAKE_BUILD_TYPE STREQUAL "Debug")
    add_compile_definitions(_FORTIFY_SOURCE=3)
endif()

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

    # Unity test framework (compile from source)
    find_path(UNITY_INCLUDE_DIR unity/unity.h
        PATHS /usr/local/include /usr/include
    )
    find_path(UNITY_SRC_DIR unity.c
        PATHS /usr/local/src/unity /usr/src/unity
    )

    if(UNITY_INCLUDE_DIR AND UNITY_SRC_DIR)
        message(STATUS "Found Unity: ${UNITY_SRC_DIR}/unity.c")
        add_library(unity STATIC ${UNITY_SRC_DIR}/unity.c)
        target_include_directories(unity PUBLIC ${UNITY_INCLUDE_DIR} ${UNITY_INCLUDE_DIR}/unity)
    else()
        message(WARNING "Unity test framework not found — tests disabled")
    endif()

    # Test helper macro
    macro(io_add_test TEST_NAME TEST_SRC)
        if(TARGET unity)
            add_executable(${TEST_NAME} ${TEST_SRC})
            target_include_directories(${TEST_NAME} PRIVATE
                ${CMAKE_SOURCE_DIR}/src
            )
            target_link_libraries(${TEST_NAME} PRIVATE
                unity
                ${ARGN}
            )
            # Unity test functions lack prototypes by design
            target_compile_options(${TEST_NAME} PRIVATE
                -Wno-missing-prototypes -Wno-missing-declarations
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

# clang-tidy (run via 'lint' target, not during normal builds)
find_program(CLANG_TIDY clang-tidy)
if(CLANG_TIDY)
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

# Sprint 2: wolfSSL TLS integration
# Sprint 3: HTTP/1.1 parser (picohttpparser)
# Sprint 4: Router and middleware
# Sprint 5: Static files and WebSocket
# Sprint 6: HTTP/2 (nghttp2)
# Sprint 7: HTTP/3 (ngtcp2 + nghttp3)
# Sprint 8: Polish, benchmarks, packaging

# ============================================================================
# Sprint 1: Core io_uring event loop
# ============================================================================

add_library(io_loop STATIC src/core/io_loop.c)
target_include_directories(io_loop PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_include_directories(io_loop PUBLIC ${LIBURING_INCLUDE_DIRS})
target_link_directories(io_loop PUBLIC ${LIBURING_LIBRARY_DIRS})
target_link_libraries(io_loop PUBLIC ${LIBURING_LIBRARIES})

io_add_test(test_io_loop tests/unit/test_io_loop.c io_loop)

# ============================================================================
# Sprint 1: Connection state machine and pool
# ============================================================================

add_library(io_conn STATIC src/core/io_conn.c)
target_include_directories(io_conn PUBLIC ${CMAKE_SOURCE_DIR}/src)

io_add_test(test_io_conn tests/unit/test_io_conn.c io_conn)

# ============================================================================
# Sprint 1: Server lifecycle management
# ============================================================================

add_library(io_server STATIC src/core/io_server.c)
target_include_directories(io_server PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(io_server PUBLIC io_loop io_conn)

io_add_test(test_io_server tests/unit/test_io_server.c io_server io_loop io_conn)

# ============================================================================
# Sprint 1: Buffer pool management
# ============================================================================

add_library(io_buffer STATIC src/core/io_buffer.c)
target_include_directories(io_buffer PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_include_directories(io_buffer PUBLIC ${LIBURING_INCLUDE_DIRS})
target_link_directories(io_buffer PUBLIC ${LIBURING_LIBRARY_DIRS})
target_link_libraries(io_buffer PUBLIC ${LIBURING_LIBRARIES})

io_add_test(test_io_buffer tests/unit/test_io_buffer.c io_buffer)

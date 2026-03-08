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
# Sprint 7: HTTP/2 (nghttp2)
# Sprint 8: HTTP/3 (ngtcp2 + nghttp3)
# Sprint 9: Polish, benchmarks, packaging

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

# ============================================================================
# Sprint 2: wolfSSL TLS integration
# ============================================================================

add_library(io_tls STATIC src/tls/io_tls.c)
target_include_directories(io_tls PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_include_directories(io_tls PUBLIC ${WOLFSSL_INCLUDE_DIRS})
target_link_directories(io_tls PUBLIC ${WOLFSSL_LIBRARY_DIRS})
target_link_libraries(io_tls PUBLIC ${WOLFSSL_LIBRARIES})

if(BUILD_TESTING AND TARGET unity)
    add_executable(test_io_tls tests/unit/test_io_tls.c)
    target_include_directories(test_io_tls PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_include_directories(test_io_tls PRIVATE ${WOLFSSL_INCLUDE_DIRS})
    target_link_directories(test_io_tls PRIVATE ${WOLFSSL_LIBRARY_DIRS})
    target_link_libraries(test_io_tls PRIVATE unity io_tls ${WOLFSSL_LIBRARIES})
    target_compile_options(test_io_tls PRIVATE
        -Wno-missing-prototypes -Wno-missing-declarations
    )
    target_compile_definitions(test_io_tls PRIVATE
        TEST_CERTS_DIR="${CMAKE_SOURCE_DIR}/tests/certs"
    )
    add_test(NAME test_io_tls COMMAND test_io_tls)
endif()

# ============================================================================
# Sprint 2.2: TLS buffer-based integration tests
# ============================================================================

if(BUILD_TESTING AND TARGET unity)
    add_executable(test_tls_uring tests/integration/test_tls_uring.c)
    target_include_directories(test_tls_uring PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_include_directories(test_tls_uring PRIVATE ${WOLFSSL_INCLUDE_DIRS})
    target_link_directories(test_tls_uring PRIVATE ${WOLFSSL_LIBRARY_DIRS})
    target_link_libraries(test_tls_uring PRIVATE unity io_tls ${WOLFSSL_LIBRARIES})
    target_compile_options(test_tls_uring PRIVATE
        -Wno-missing-prototypes -Wno-missing-declarations
    )
    target_compile_definitions(test_tls_uring PRIVATE
        TEST_CERTS_DIR="${CMAKE_SOURCE_DIR}/tests/certs"
    )
    add_test(NAME test_tls_uring COMMAND test_tls_uring)
endif()

# ============================================================================
# Sprint 3: HTTP/1.1 request/response abstractions
# ============================================================================

add_library(io_request STATIC src/http/io_request.c)
target_include_directories(io_request PUBLIC ${CMAKE_SOURCE_DIR}/src)

add_library(io_response STATIC src/http/io_response.c)
target_include_directories(io_response PUBLIC ${CMAKE_SOURCE_DIR}/src)

io_add_test(test_io_request tests/unit/test_io_request.c io_request)
io_add_test(test_io_response tests/unit/test_io_response.c io_response)

# ============================================================================
# Sprint 3: HTTP/1.1 parser (picohttpparser)
# ============================================================================

add_library(io_http1 STATIC src/http/io_http1.c src/http/picohttpparser.c)
target_include_directories(io_http1 PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(io_http1 PUBLIC io_request)
# picohttpparser uses pointer arithmetic that triggers sign-conversion warnings
target_compile_options(io_http1 PRIVATE -Wno-sign-conversion -Wno-conversion -Wno-undef)

io_add_test(test_io_http1 tests/unit/test_io_http1.c io_http1 io_request io_response)

# ============================================================================
# Sprint 3: PROXY protocol decoder
# ============================================================================

add_library(io_proxy_proto STATIC src/http/io_proxy_proto.c)
target_include_directories(io_proxy_proto PUBLIC ${CMAKE_SOURCE_DIR}/src)

io_add_test(test_proxy_proto tests/unit/test_proxy_proto.c io_proxy_proto)

# ============================================================================
# Sprint 3: HTTP/1.1 integration tests
# ============================================================================

if(BUILD_TESTING AND TARGET unity)
    add_executable(test_http1_server tests/integration/test_http1_server.c)
    target_include_directories(test_http1_server PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_include_directories(test_http1_server PRIVATE ${WOLFSSL_INCLUDE_DIRS})
    target_link_directories(test_http1_server PRIVATE ${WOLFSSL_LIBRARY_DIRS})
    target_link_libraries(test_http1_server PRIVATE
        unity io_http1 io_request io_response io_proxy_proto io_tls ${WOLFSSL_LIBRARIES}
    )
    target_compile_options(test_http1_server PRIVATE
        -Wno-missing-prototypes -Wno-missing-declarations
    )
    target_compile_definitions(test_http1_server PRIVATE
        TEST_CERTS_DIR="${CMAKE_SOURCE_DIR}/tests/certs"
    )
    add_test(NAME test_http1_server COMMAND test_http1_server)
endif()

# ============================================================================
# Sprint 4: Radix trie (internal)
# ============================================================================

add_library(io_radix STATIC src/router/io_radix.c)
target_include_directories(io_radix PUBLIC ${CMAKE_SOURCE_DIR}/src)

io_add_test(test_io_radix tests/unit/test_io_radix.c io_radix io_request)

# ============================================================================
# Sprint 4: Router public API
# ============================================================================

add_library(io_router STATIC src/router/io_router.c)
target_include_directories(io_router PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(io_router PUBLIC io_radix io_request)

io_add_test(test_io_router tests/unit/test_io_router.c io_router io_radix io_request io_response)

# ============================================================================
# Sprint 4: Route groups
# ============================================================================

add_library(io_route_group STATIC src/router/io_route_group.c)
target_include_directories(io_route_group PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(io_route_group PUBLIC io_router)

io_add_test(test_io_route_group tests/unit/test_io_route_group.c io_route_group io_router io_radix io_request io_response)

# ============================================================================
# Sprint 4: Typed parameter extraction
# ============================================================================

io_add_test(test_io_params tests/unit/test_io_params.c io_request)

# ============================================================================
# Sprint 4: Route introspection
# ============================================================================

add_library(io_route_inspect STATIC src/router/io_route_inspect.c)
target_include_directories(io_route_inspect PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(io_route_inspect PUBLIC io_router io_radix)

io_add_test(test_io_route_inspect tests/unit/test_io_route_inspect.c io_route_inspect io_router io_route_group io_radix io_request io_response)

# ============================================================================
# Sprint 4: Middleware chain
# ============================================================================

add_library(io_middleware STATIC src/middleware/io_middleware.c)
target_include_directories(io_middleware PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(io_middleware PUBLIC io_router io_route_group io_request io_response)

io_add_test(test_io_middleware tests/unit/test_io_middleware.c io_middleware io_router io_route_group io_radix io_request io_response)

# ============================================================================
# Sprint 4: Built-in middleware modules
# ============================================================================

add_library(io_cors STATIC src/middleware/io_cors.c)
target_include_directories(io_cors PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(io_cors PUBLIC io_request io_response)

add_library(io_ratelimit STATIC src/middleware/io_ratelimit.c)
target_include_directories(io_ratelimit PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(io_ratelimit PUBLIC io_request io_response)

add_library(io_security STATIC src/middleware/io_security.c)
target_include_directories(io_security PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(io_security PUBLIC io_request io_response)

add_library(io_auth STATIC src/middleware/io_auth.c)
target_include_directories(io_auth PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(io_auth PUBLIC io_request io_response)

io_add_test(test_io_cors tests/unit/test_io_cors.c io_cors io_request io_response)
io_add_test(test_io_ratelimit tests/unit/test_io_ratelimit.c io_ratelimit io_request io_response)
io_add_test(test_io_security tests/unit/test_io_security.c io_security io_request io_response)
io_add_test(test_io_auth tests/unit/test_io_auth.c io_auth io_request io_response)

# ============================================================================
# Sprint 5: Static file server
# ============================================================================

add_library(io_static STATIC src/static/io_static.c)
target_include_directories(io_static PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(io_static PUBLIC io_request io_response)

io_add_test(test_io_static tests/unit/test_io_static.c io_static io_request io_response)

# ============================================================================
# Sprint 5: Multipart parser
# ============================================================================

add_library(io_multipart STATIC src/http/io_multipart.c)
target_include_directories(io_multipart PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(io_multipart PUBLIC io_request)

io_add_test(test_io_multipart tests/unit/test_io_multipart.c io_multipart io_request)

# ============================================================================
# Sprint 5: SPA fallback
# ============================================================================

add_library(io_spa STATIC src/static/io_spa.c)
target_include_directories(io_spa PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(io_spa PUBLIC io_static io_request io_response)

io_add_test(test_io_spa tests/unit/test_io_spa.c io_spa io_static io_request io_response)

# ============================================================================
# Sprint 5: Compression
# ============================================================================

# Check for optional compression libraries
pkg_check_modules(ZLIB zlib)
pkg_check_modules(BROTLI_ENC libbrotlienc)

add_library(io_compress STATIC src/static/io_compress.c)
target_include_directories(io_compress PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(io_compress PUBLIC io_request io_response)

if(ZLIB_FOUND)
    target_compile_definitions(io_compress PRIVATE IOHTTP_HAVE_ZLIB)
    target_include_directories(io_compress PRIVATE ${ZLIB_INCLUDE_DIRS})
    target_link_libraries(io_compress PRIVATE ${ZLIB_LIBRARIES})
endif()

if(BROTLI_ENC_FOUND)
    target_compile_definitions(io_compress PRIVATE IOHTTP_HAVE_BROTLI)
    target_include_directories(io_compress PRIVATE ${BROTLI_ENC_INCLUDE_DIRS})
    target_link_libraries(io_compress PRIVATE ${BROTLI_ENC_LIBRARIES})
endif()

io_add_test(test_io_compress tests/unit/test_io_compress.c io_compress io_request io_response)
if(ZLIB_FOUND)
    target_compile_definitions(test_io_compress PRIVATE IOHTTP_HAVE_ZLIB)
endif()
if(BROTLI_ENC_FOUND)
    target_compile_definitions(test_io_compress PRIVATE IOHTTP_HAVE_BROTLI)
endif()

# ============================================================================
# Sprint 6: Server-Sent Events
# ============================================================================

add_library(io_sse STATIC src/ws/io_sse.c)
target_include_directories(io_sse PUBLIC ${CMAKE_SOURCE_DIR}/src)

io_add_test(test_io_sse tests/unit/test_io_sse.c io_sse)

# ============================================================================
# Sprint 6: WebSocket (RFC 6455, wslay-backed)
# ============================================================================

# wslay (WebSocket RFC 6455, I/O-agnostic)
find_path(WSLAY_INCLUDE_DIR NAMES wslay/wslay.h)
find_library(WSLAY_LIBRARY NAMES wslay)

add_library(io_websocket STATIC src/ws/io_websocket.c)
target_include_directories(io_websocket PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_include_directories(io_websocket PUBLIC ${WOLFSSL_INCLUDE_DIRS})
target_link_directories(io_websocket PUBLIC ${WOLFSSL_LIBRARY_DIRS})
target_link_libraries(io_websocket PUBLIC ${WOLFSSL_LIBRARIES})
if(WSLAY_INCLUDE_DIR AND WSLAY_LIBRARY)
    target_include_directories(io_websocket PUBLIC ${WSLAY_INCLUDE_DIR})
    target_link_libraries(io_websocket PUBLIC ${WSLAY_LIBRARY})
endif()

io_add_test(test_io_websocket tests/unit/test_io_websocket.c io_websocket)

# ============================================================================
# Sprint 7: HTTP/2 (nghttp2)
# ============================================================================

find_path(NGHTTP2_INCLUDE_DIR NAMES nghttp2/nghttp2.h)
find_library(NGHTTP2_LIBRARY NAMES nghttp2)

add_library(io_http2 STATIC src/http/io_http2.c)
target_include_directories(io_http2 PUBLIC ${CMAKE_SOURCE_DIR}/src)
if(NGHTTP2_INCLUDE_DIR AND NGHTTP2_LIBRARY)
    target_include_directories(io_http2 PUBLIC ${NGHTTP2_INCLUDE_DIR})
    target_link_libraries(io_http2 PUBLIC ${NGHTTP2_LIBRARY})
endif()
target_link_libraries(io_http2 PUBLIC io_request io_response)

if(BUILD_TESTING AND TARGET unity AND NGHTTP2_INCLUDE_DIR AND NGHTTP2_LIBRARY)
    add_executable(test_io_http2 tests/unit/test_io_http2.c)
    target_include_directories(test_io_http2 PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${NGHTTP2_INCLUDE_DIR}
    )
    target_link_libraries(test_io_http2 PRIVATE
        unity io_http2 io_request io_response ${NGHTTP2_LIBRARY}
    )
    target_compile_options(test_io_http2 PRIVATE
        -Wno-missing-prototypes -Wno-missing-declarations
    )
    add_test(NAME test_io_http2 COMMAND test_io_http2)
endif()

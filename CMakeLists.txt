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

# Ensure pkg-config finds libraries in /usr/local
set(ENV{PKG_CONFIG_PATH} "/usr/local/lib/pkgconfig:/usr/local/lib64/pkgconfig:$ENV{PKG_CONFIG_PATH}")

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

# PVS-Studio (compile_commands.json approach — simpler, no cmake module needed)
find_program(PVS_STUDIO pvs-studio-analyzer)
find_program(PLOG_CONVERTER plog-converter)
if(PVS_STUDIO AND PLOG_CONVERTER)
    add_custom_target(pvs-studio
        COMMAND ${PVS_STUDIO} analyze
            -f ${CMAKE_BINARY_DIR}/compile_commands.json
            -o ${CMAKE_BINARY_DIR}/pvs-studio.log
            -j 4
            -e ${CMAKE_SOURCE_DIR}/src/http/picohttpparser.c
        COMMAND ${PLOG_CONVERTER}
            -t errorfile
            -a "GA:1,2"
            ${CMAKE_BINARY_DIR}/pvs-studio.log
        COMMENT "Running PVS-Studio analysis (GA errors + warnings)"
        VERBATIM
    )
endif()

# CodeChecker (Clang SA + clang-tidy)
find_program(CODECHECKER CodeChecker)
if(CODECHECKER)
    add_custom_target(codechecker
        COMMAND ${CODECHECKER} analyze
            ${CMAKE_BINARY_DIR}/compile_commands.json
            -o ${CMAKE_BINARY_DIR}/cc-results
            --analyzers clangsa clang-tidy
            -j 4
        COMMAND ${CODECHECKER} parse
            ${CMAKE_BINARY_DIR}/cc-results
            --trim-path-prefix ${CMAKE_SOURCE_DIR}/
        COMMENT "Running CodeChecker analysis (Clang SA + clang-tidy)"
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
target_link_libraries(io_server PUBLIC io_loop io_conn io_ctx io_http1 io_request io_response io_router io_middleware io_tls)

io_add_test(test_io_server tests/unit/test_io_server.c io_server io_loop io_conn)

# ============================================================================
# Sprint 8: Unified request context
# ============================================================================

add_library(io_ctx STATIC src/core/io_ctx.c)
target_include_directories(io_ctx PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(io_ctx PUBLIC io_request io_response)

io_add_test(test_io_ctx tests/unit/test_io_ctx.c io_ctx io_request io_response)

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
target_link_libraries(io_router PUBLIC io_radix io_request io_ctx)

io_add_test(test_io_router tests/unit/test_io_router.c io_router io_radix io_request io_response io_ctx)

# ============================================================================
# Sprint 4: Route groups
# ============================================================================

add_library(io_route_group STATIC src/router/io_route_group.c)
target_include_directories(io_route_group PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(io_route_group PUBLIC io_router)

io_add_test(test_io_route_group tests/unit/test_io_route_group.c io_route_group io_router io_radix io_request io_response io_ctx)

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

io_add_test(test_io_route_inspect tests/unit/test_io_route_inspect.c io_route_inspect io_route_meta io_router io_route_group io_radix io_request io_response io_ctx)

# ============================================================================
# Sprint 9: Route metadata
# ============================================================================

add_library(io_route_meta STATIC src/router/io_route_meta.c)
target_include_directories(io_route_meta PUBLIC ${CMAKE_SOURCE_DIR}/src)

io_add_test(test_io_route_meta tests/unit/test_io_route_meta.c io_route_meta io_route_inspect io_router io_radix io_request io_response io_ctx)

# ============================================================================
# Sprint 4: Middleware chain
# ============================================================================

add_library(io_middleware STATIC src/middleware/io_middleware.c)
target_include_directories(io_middleware PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(io_middleware PUBLIC io_router io_route_group io_request io_response io_ctx)

io_add_test(test_io_middleware tests/unit/test_io_middleware.c io_middleware io_router io_route_group io_radix io_request io_response io_ctx)

# ============================================================================
# Sprint 4: Built-in middleware modules
# ============================================================================

add_library(io_cors STATIC src/middleware/io_cors.c)
target_include_directories(io_cors PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(io_cors PUBLIC io_request io_response io_ctx)

add_library(io_ratelimit STATIC src/middleware/io_ratelimit.c)
target_include_directories(io_ratelimit PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(io_ratelimit PUBLIC io_request io_response io_ctx)

add_library(io_security STATIC src/middleware/io_security.c)
target_include_directories(io_security PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(io_security PUBLIC io_request io_response io_ctx)

add_library(io_auth STATIC src/middleware/io_auth.c)
target_include_directories(io_auth PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(io_auth PUBLIC io_request io_response io_ctx)

io_add_test(test_io_cors tests/unit/test_io_cors.c io_cors io_request io_response io_ctx)
io_add_test(test_io_ratelimit tests/unit/test_io_ratelimit.c io_ratelimit io_request io_response io_ctx)
io_add_test(test_io_security tests/unit/test_io_security.c io_security io_request io_response io_ctx)
io_add_test(test_io_auth tests/unit/test_io_auth.c io_auth io_request io_response io_ctx)

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
target_link_libraries(io_http2 PUBLIC io_request io_response io_ctx)

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

# Sprint 7: HTTP/2 ALPN protocol selection integration tests
if(BUILD_TESTING AND TARGET unity AND NGHTTP2_INCLUDE_DIR AND NGHTTP2_LIBRARY)
    add_executable(test_http2_server tests/integration/test_http2_server.c)
    target_include_directories(test_http2_server PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_include_directories(test_http2_server PRIVATE ${WOLFSSL_INCLUDE_DIRS})
    target_include_directories(test_http2_server PRIVATE ${NGHTTP2_INCLUDE_DIR})
    target_link_directories(test_http2_server PRIVATE ${WOLFSSL_LIBRARY_DIRS})
    target_link_libraries(test_http2_server PRIVATE
        unity io_http2 io_http1 io_tls io_request io_response
        ${WOLFSSL_LIBRARIES} ${NGHTTP2_LIBRARY}
    )
    target_compile_options(test_http2_server PRIVATE
        -Wno-missing-prototypes -Wno-missing-declarations
    )
    target_compile_definitions(test_http2_server PRIVATE
        TEST_CERTS_DIR="${CMAKE_SOURCE_DIR}/tests/certs"
    )
    add_test(NAME test_http2_server COMMAND test_http2_server)
endif()

# ============================================================================
# Sprint 10: QUIC transport (ngtcp2 + ngtcp2_crypto_wolfssl)
# ============================================================================

if(NGTCP2_FOUND AND NGTCP2_WOLFSSL_FOUND)
    add_library(io_quic STATIC src/http/io_quic.c)
    target_include_directories(io_quic PUBLIC ${CMAKE_SOURCE_DIR}/src)
    target_include_directories(io_quic PUBLIC
        ${NGTCP2_INCLUDE_DIRS}
        ${NGTCP2_WOLFSSL_INCLUDE_DIRS}
        ${WOLFSSL_INCLUDE_DIRS}
    )
    target_link_directories(io_quic PUBLIC
        ${NGTCP2_LIBRARY_DIRS}
        ${NGTCP2_WOLFSSL_LIBRARY_DIRS}
        ${WOLFSSL_LIBRARY_DIRS}
    )
    target_link_libraries(io_quic PUBLIC
        ${NGTCP2_LIBRARIES}
        ${NGTCP2_WOLFSSL_LIBRARIES}
        ${WOLFSSL_LIBRARIES}
    )
    message(STATUS "QUIC transport enabled (ngtcp2 + wolfSSL)")

    if(BUILD_TESTING AND TARGET unity)
        add_executable(test_io_quic tests/unit/test_io_quic.c)
        target_include_directories(test_io_quic PRIVATE ${CMAKE_SOURCE_DIR}/src)
        target_include_directories(test_io_quic PRIVATE
            ${NGTCP2_INCLUDE_DIRS}
            ${NGTCP2_WOLFSSL_INCLUDE_DIRS}
            ${WOLFSSL_INCLUDE_DIRS}
        )
        target_link_directories(test_io_quic PRIVATE
            ${NGTCP2_LIBRARY_DIRS}
            ${NGTCP2_WOLFSSL_LIBRARY_DIRS}
            ${WOLFSSL_LIBRARY_DIRS}
        )
        target_link_libraries(test_io_quic PRIVATE
            unity io_quic
            ${NGTCP2_LIBRARIES}
            ${NGTCP2_WOLFSSL_LIBRARIES}
            ${WOLFSSL_LIBRARIES}
        )
        target_compile_options(test_io_quic PRIVATE
            -Wno-missing-prototypes -Wno-missing-declarations
        )
        target_compile_definitions(test_io_quic PRIVATE
            TEST_CERTS_DIR="${CMAKE_SOURCE_DIR}/tests/certs"
        )
        add_test(NAME test_io_quic COMMAND test_io_quic)
    endif()
else()
    message(STATUS "QUIC transport disabled (ngtcp2 or ngtcp2_crypto_wolfssl not found)")
endif()

# ============================================================================
# Sprint 10: HTTP/3 session (nghttp3)
# ============================================================================

if(NGHTTP3_FOUND AND TARGET io_quic)
    add_library(io_http3 STATIC src/http/io_http3.c)
    target_include_directories(io_http3 PUBLIC ${CMAKE_SOURCE_DIR}/src)
    target_include_directories(io_http3 PUBLIC ${NGHTTP3_INCLUDE_DIRS})
    target_link_directories(io_http3 PUBLIC ${NGHTTP3_LIBRARY_DIRS})
    target_link_libraries(io_http3 PUBLIC ${NGHTTP3_LIBRARIES} io_quic io_request io_response io_ctx)
    message(STATUS "HTTP/3 enabled (nghttp3)")

    if(BUILD_TESTING AND TARGET unity)
        add_executable(test_io_http3 tests/unit/test_io_http3.c)
        target_include_directories(test_io_http3 PRIVATE ${CMAKE_SOURCE_DIR}/src)
        target_include_directories(test_io_http3 PRIVATE ${NGHTTP3_INCLUDE_DIRS})
        target_link_directories(test_io_http3 PRIVATE ${NGHTTP3_LIBRARY_DIRS})
        target_link_libraries(test_io_http3 PRIVATE
            unity io_http3 io_quic io_request io_response io_ctx
            ${NGHTTP3_LIBRARIES}
        )
        target_compile_options(test_io_http3 PRIVATE
            -Wno-missing-prototypes -Wno-missing-declarations
        )
        add_test(NAME test_io_http3 COMMAND test_io_http3)
    endif()
else()
    message(STATUS "HTTP/3 disabled (nghttp3 or io_quic not available)")
endif()

# ============================================================================
# Sprint 10: Alt-Svc header (HTTP/3 discovery)
# ============================================================================

add_library(io_alt_svc STATIC src/http/io_alt_svc.c)
target_include_directories(io_alt_svc PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(io_alt_svc PUBLIC io_response)

io_add_test(test_io_alt_svc tests/unit/test_io_alt_svc.c io_alt_svc io_response)

# ============================================================================
# Sprint 10: HTTP/3 integration tests
# ============================================================================

if(BUILD_TESTING AND TARGET unity AND TARGET io_http3 AND TARGET io_quic)
    add_executable(test_http3_server tests/integration/test_http3_server.c)
    target_include_directories(test_http3_server PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_include_directories(test_http3_server PRIVATE
        ${WOLFSSL_INCLUDE_DIRS} ${NGTCP2_INCLUDE_DIRS}
        ${NGTCP2_WOLFSSL_INCLUDE_DIRS} ${NGHTTP3_INCLUDE_DIRS}
    )
    target_link_directories(test_http3_server PRIVATE
        ${WOLFSSL_LIBRARY_DIRS} ${NGTCP2_LIBRARY_DIRS}
        ${NGTCP2_WOLFSSL_LIBRARY_DIRS} ${NGHTTP3_LIBRARY_DIRS}
    )
    target_link_libraries(test_http3_server PRIVATE
        unity io_http3 io_quic io_alt_svc io_request io_response io_ctx
        ${WOLFSSL_LIBRARIES} ${NGTCP2_LIBRARIES}
        ${NGTCP2_WOLFSSL_LIBRARIES} ${NGHTTP3_LIBRARIES}
    )
    target_compile_options(test_http3_server PRIVATE
        -Wno-missing-prototypes -Wno-missing-declarations
    )
    target_compile_definitions(test_http3_server PRIVATE
        TEST_CERTS_DIR="${CMAKE_SOURCE_DIR}/tests/certs"
    )
    add_test(NAME test_http3_server COMMAND test_http3_server)
endif()

# ============================================================================
# Sprint 11: TCP pipeline end-to-end integration test
# ============================================================================

if(BUILD_TESTING AND TARGET unity)
    add_executable(test_pipeline tests/integration/test_pipeline.c)
    target_include_directories(test_pipeline PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(test_pipeline PRIVATE
        unity io_server io_loop io_conn io_ctx
        io_http1 io_request io_response
        io_router io_radix io_middleware
        io_route_group io_route_inspect io_route_meta
    )
    target_compile_options(test_pipeline PRIVATE
        -Wno-missing-prototypes -Wno-missing-declarations
    )
    add_test(NAME test_pipeline COMMAND test_pipeline)

    add_executable(test_middleware_pipeline tests/integration/test_middleware_pipeline.c)
    target_include_directories(test_middleware_pipeline PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(test_middleware_pipeline PRIVATE
        unity io_server io_loop io_conn io_ctx
        io_http1 io_request io_response
        io_router io_radix io_middleware
        io_route_group io_route_inspect io_route_meta
    )
    target_compile_options(test_middleware_pipeline PRIVATE
        -Wno-missing-prototypes -Wno-missing-declarations
    )
    add_test(NAME test_middleware_pipeline COMMAND test_middleware_pipeline)

    add_executable(test_keepalive tests/integration/test_keepalive.c)
    target_include_directories(test_keepalive PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(test_keepalive PRIVATE
        unity io_server io_loop io_conn io_ctx
        io_http1 io_request io_response
        io_router io_radix io_middleware
        io_route_group io_route_inspect io_route_meta
    )
    target_compile_options(test_keepalive PRIVATE
        -Wno-missing-prototypes -Wno-missing-declarations
    )
    add_test(NAME test_keepalive COMMAND test_keepalive)

    add_executable(test_timeout tests/integration/test_timeout.c)
    target_include_directories(test_timeout PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(test_timeout PRIVATE
        unity io_server io_loop io_conn io_ctx
        io_http1 io_request io_response
        io_router io_radix io_middleware
        io_route_group io_route_inspect io_route_meta
    )
    target_compile_options(test_timeout PRIVATE
        -Wno-missing-prototypes -Wno-missing-declarations
    )
    add_test(NAME test_timeout COMMAND test_timeout)

    add_executable(test_shutdown tests/integration/test_shutdown.c)
    target_include_directories(test_shutdown PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(test_shutdown PRIVATE
        unity io_server io_loop io_conn io_ctx
        io_http1 io_request io_response
        io_router io_radix io_middleware
        io_route_group io_route_inspect io_route_meta
    )
    target_compile_options(test_shutdown PRIVATE
        -Wno-missing-prototypes -Wno-missing-declarations
    )
    add_test(NAME test_shutdown COMMAND test_shutdown)

    add_executable(test_limits tests/integration/test_limits.c)
    target_include_directories(test_limits PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(test_limits PRIVATE
        unity io_server io_loop io_conn io_ctx
        io_http1 io_request io_response
        io_router io_radix io_middleware
        io_route_group io_route_inspect io_route_meta
    )
    target_compile_options(test_limits PRIVATE
        -Wno-missing-prototypes -Wno-missing-declarations
    )
    add_test(NAME test_limits COMMAND test_limits)

    add_executable(test_signal tests/integration/test_signal.c)
    target_include_directories(test_signal PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(test_signal PRIVATE
        unity io_server io_loop io_conn io_ctx
        io_http1 io_request io_response
        io_router io_radix io_middleware
        io_route_group io_route_inspect io_route_meta
        pthread
    )
    target_compile_options(test_signal PRIVATE
        -Wno-missing-prototypes -Wno-missing-declarations
    )
    add_test(NAME test_signal COMMAND test_signal)

    add_executable(test_tls_pipeline tests/integration/test_tls_pipeline.c)
    target_include_directories(test_tls_pipeline PRIVATE
        ${CMAKE_SOURCE_DIR}/src ${WOLFSSL_INCLUDE_DIRS})
    target_link_directories(test_tls_pipeline PRIVATE
        ${WOLFSSL_LIBRARY_DIRS})
    target_link_libraries(test_tls_pipeline PRIVATE
        unity io_server io_loop io_conn io_ctx io_tls
        io_http1 io_request io_response
        io_router io_radix io_middleware
        io_route_group io_route_inspect io_route_meta
        ${WOLFSSL_LIBRARIES}
    )
    target_compile_options(test_tls_pipeline PRIVATE
        -Wno-missing-prototypes -Wno-missing-declarations
    )
    target_compile_definitions(test_tls_pipeline PRIVATE
        TEST_CERTS_DIR="${CMAKE_SOURCE_DIR}/tests/certs"
    )
    add_test(NAME test_tls_pipeline COMMAND test_tls_pipeline)
endif()

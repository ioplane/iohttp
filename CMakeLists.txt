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
    macro(ioh_add_test TEST_NAME TEST_SRC)
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

macro(ioh_add_fuzz FUZZ_NAME FUZZ_SRC)
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

add_library(ioh_loop STATIC src/core/ioh_loop.c)
target_include_directories(ioh_loop PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_include_directories(ioh_loop PUBLIC ${LIBURING_INCLUDE_DIRS})
target_link_directories(ioh_loop PUBLIC ${LIBURING_LIBRARY_DIRS})
target_link_libraries(ioh_loop PUBLIC ${LIBURING_LIBRARIES})

ioh_add_test(test_ioh_loop tests/unit/test_ioh_loop.c ioh_loop)

# ============================================================================
# Sprint 1: Connection state machine and pool
# ============================================================================

add_library(ioh_conn STATIC src/core/ioh_conn.c)
target_include_directories(ioh_conn PUBLIC ${CMAKE_SOURCE_DIR}/src)

ioh_add_test(test_ioh_conn tests/unit/test_ioh_conn.c ioh_conn)

# ============================================================================
# Sprint 12: Structured logging
# ============================================================================

add_library(ioh_log STATIC src/core/ioh_log.c)
target_include_directories(ioh_log PUBLIC ${CMAKE_SOURCE_DIR}/src)

ioh_add_test(test_ioh_log tests/unit/test_ioh_log.c ioh_log)

# ============================================================================
# Sprint 15: Health check endpoint framework
# ============================================================================

add_library(ioh_health STATIC src/core/ioh_health.c)
target_include_directories(ioh_health PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(ioh_health PUBLIC ioh_router ioh_ctx ioh_request ioh_response)
if(YYJSON_INCLUDE_DIR AND YYJSON_LIBRARY)
    target_include_directories(ioh_health PRIVATE ${YYJSON_INCLUDE_DIR})
    target_link_libraries(ioh_health PRIVATE ${YYJSON_LIBRARY})
    target_compile_definitions(ioh_health PRIVATE IOHTTP_HAVE_YYJSON)
endif()

# Test uses a separate object without yyjson to avoid PIC issues with static yyjson
if(BUILD_TESTING AND TARGET unity)
    add_library(ioh_health_test STATIC src/core/ioh_health.c)
    target_include_directories(ioh_health_test PUBLIC ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(ioh_health_test PUBLIC ioh_router ioh_ctx ioh_request ioh_response)
    ioh_add_test(test_ioh_health tests/unit/test_ioh_health.c ioh_health_test ioh_server ioh_router ioh_radix ioh_ctx ioh_request ioh_response)
endif()

# Sprint 15: Per-route timeout tests
ioh_add_test(test_ioh_route_timeout tests/unit/test_ioh_route_timeout.c
    ioh_router ioh_radix ioh_ctx ioh_request ioh_response)

# ============================================================================
# Sprint 1: Server lifecycle management
# ============================================================================

add_library(ioh_server STATIC src/core/ioh_server.c)
target_include_directories(ioh_server PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(ioh_server PUBLIC ioh_loop ioh_conn ioh_ctx ioh_http1 ioh_request ioh_response ioh_router ioh_middleware ioh_tls ioh_log ioh_proxy_proto)

ioh_add_test(test_ioh_server tests/unit/test_ioh_server.c ioh_server ioh_loop ioh_conn)

# ============================================================================
# Sprint 16: Cookie handling (RFC 6265bis)
# ============================================================================

add_library(ioh_cookie STATIC src/http/ioh_cookie.c)
target_include_directories(ioh_cookie PUBLIC ${CMAKE_SOURCE_DIR}/src)

# ============================================================================
# Sprint 8: Unified request context
# ============================================================================

add_library(ioh_ctx STATIC src/core/ioh_ctx.c)
target_include_directories(ioh_ctx PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(ioh_ctx PUBLIC ioh_request ioh_response ioh_cookie)

ioh_add_test(test_ioh_ctx tests/unit/test_ioh_ctx.c ioh_ctx ioh_request ioh_response)

# Sprint 16: Cookie tests
ioh_add_test(test_ioh_cookie tests/unit/test_ioh_cookie.c
    ioh_cookie ioh_ctx ioh_request ioh_response)

# ============================================================================
# Sprint 1: Buffer pool management
# ============================================================================

add_library(ioh_buffer STATIC src/core/ioh_buffer.c)
target_include_directories(ioh_buffer PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_include_directories(ioh_buffer PUBLIC ${LIBURING_INCLUDE_DIRS})
target_link_directories(ioh_buffer PUBLIC ${LIBURING_LIBRARY_DIRS})
target_link_libraries(ioh_buffer PUBLIC ${LIBURING_LIBRARIES})

ioh_add_test(test_ioh_buffer tests/unit/test_ioh_buffer.c ioh_buffer)

# ============================================================================
# Sprint 2: wolfSSL TLS integration
# ============================================================================

add_library(ioh_tls STATIC src/tls/ioh_tls.c)
target_include_directories(ioh_tls PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_include_directories(ioh_tls PUBLIC ${WOLFSSL_INCLUDE_DIRS})
target_link_directories(ioh_tls PUBLIC ${WOLFSSL_LIBRARY_DIRS})
target_link_libraries(ioh_tls PUBLIC ${WOLFSSL_LIBRARIES})

if(BUILD_TESTING AND TARGET unity)
    add_executable(test_ioh_tls tests/unit/test_ioh_tls.c)
    target_include_directories(test_ioh_tls PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_include_directories(test_ioh_tls PRIVATE ${WOLFSSL_INCLUDE_DIRS})
    target_link_directories(test_ioh_tls PRIVATE ${WOLFSSL_LIBRARY_DIRS})
    target_link_libraries(test_ioh_tls PRIVATE unity ioh_tls ${WOLFSSL_LIBRARIES})
    target_compile_options(test_ioh_tls PRIVATE
        -Wno-missing-prototypes -Wno-missing-declarations
    )
    target_compile_definitions(test_ioh_tls PRIVATE
        TEST_CERTS_DIR="${CMAKE_SOURCE_DIR}/tests/certs"
    )
    add_test(NAME test_ioh_tls COMMAND test_ioh_tls)
endif()

# ============================================================================
# Sprint 2.2: TLS buffer-based integration tests
# ============================================================================

if(BUILD_TESTING AND TARGET unity)
    add_executable(test_tls_uring tests/integration/test_tls_uring.c)
    target_include_directories(test_tls_uring PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_include_directories(test_tls_uring PRIVATE ${WOLFSSL_INCLUDE_DIRS})
    target_link_directories(test_tls_uring PRIVATE ${WOLFSSL_LIBRARY_DIRS})
    target_link_libraries(test_tls_uring PRIVATE unity ioh_tls ${WOLFSSL_LIBRARIES})
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

add_library(ioh_request STATIC src/http/ioh_request.c)
target_include_directories(ioh_request PUBLIC ${CMAKE_SOURCE_DIR}/src)

add_library(ioh_response STATIC src/http/ioh_response.c)
target_include_directories(ioh_response PUBLIC ${CMAKE_SOURCE_DIR}/src)

ioh_add_test(test_ioh_request tests/unit/test_ioh_request.c ioh_request)
ioh_add_test(test_ioh_response tests/unit/test_ioh_response.c ioh_response)

# Sprint 16: Vary header tests
ioh_add_test(test_ioh_vary tests/unit/test_ioh_vary.c
    ioh_response ioh_request)

# ============================================================================
# Sprint 3: HTTP/1.1 parser (picohttpparser)
# ============================================================================

add_library(ioh_http1 STATIC src/http/ioh_http1.c src/http/picohttpparser.c)
target_include_directories(ioh_http1 PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(ioh_http1 PUBLIC ioh_request)
# picohttpparser uses pointer arithmetic that triggers sign-conversion warnings
target_compile_options(ioh_http1 PRIVATE -Wno-sign-conversion -Wno-conversion -Wno-undef)

ioh_add_test(test_ioh_http1 tests/unit/test_ioh_http1.c ioh_http1 ioh_request ioh_response)

# ============================================================================
# Sprint 3: PROXY protocol decoder
# ============================================================================

add_library(ioh_proxy_proto STATIC src/http/ioh_proxy_proto.c)
target_include_directories(ioh_proxy_proto PUBLIC ${CMAKE_SOURCE_DIR}/src)

ioh_add_test(test_proxy_proto tests/unit/test_proxy_proto.c ioh_proxy_proto)

# ============================================================================
# Sprint 3: HTTP/1.1 integration tests
# ============================================================================

if(BUILD_TESTING AND TARGET unity)
    add_executable(test_http1_server tests/integration/test_http1_server.c)
    target_include_directories(test_http1_server PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_include_directories(test_http1_server PRIVATE ${WOLFSSL_INCLUDE_DIRS})
    target_link_directories(test_http1_server PRIVATE ${WOLFSSL_LIBRARY_DIRS})
    target_link_libraries(test_http1_server PRIVATE
        unity ioh_http1 ioh_request ioh_response ioh_proxy_proto ioh_tls ${WOLFSSL_LIBRARIES}
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

add_library(ioh_radix STATIC src/router/ioh_radix.c)
target_include_directories(ioh_radix PUBLIC ${CMAKE_SOURCE_DIR}/src)

ioh_add_test(test_ioh_radix tests/unit/test_ioh_radix.c ioh_radix ioh_request)

# ============================================================================
# Sprint 4: Router public API
# ============================================================================

add_library(ioh_router STATIC src/router/ioh_router.c)
target_include_directories(ioh_router PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(ioh_router PUBLIC ioh_radix ioh_request ioh_ctx)

ioh_add_test(test_ioh_router tests/unit/test_ioh_router.c ioh_router ioh_radix ioh_request ioh_response ioh_ctx)

# ============================================================================
# Sprint 4: Route groups
# ============================================================================

add_library(ioh_route_group STATIC src/router/ioh_route_group.c)
target_include_directories(ioh_route_group PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(ioh_route_group PUBLIC ioh_router)

ioh_add_test(test_ioh_route_group tests/unit/test_ioh_route_group.c ioh_route_group ioh_router ioh_radix ioh_request ioh_response ioh_ctx)

# ============================================================================
# Sprint 4: Typed parameter extraction
# ============================================================================

ioh_add_test(test_ioh_params tests/unit/test_ioh_params.c ioh_request)

# ============================================================================
# Sprint 4: Route introspection
# ============================================================================

add_library(ioh_route_inspect STATIC src/router/ioh_route_inspect.c)
target_include_directories(ioh_route_inspect PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(ioh_route_inspect PUBLIC ioh_router ioh_radix)

ioh_add_test(test_ioh_route_inspect tests/unit/test_ioh_route_inspect.c ioh_route_inspect ioh_route_meta ioh_router ioh_route_group ioh_radix ioh_request ioh_response ioh_ctx)

# ============================================================================
# Sprint 9: Route metadata
# ============================================================================

add_library(ioh_route_meta STATIC src/router/ioh_route_meta.c)
target_include_directories(ioh_route_meta PUBLIC ${CMAKE_SOURCE_DIR}/src)

ioh_add_test(test_ioh_route_meta tests/unit/test_ioh_route_meta.c ioh_route_meta ioh_route_inspect ioh_router ioh_radix ioh_request ioh_response ioh_ctx)

# ============================================================================
# Sprint 16: Virtual host routing
# ============================================================================

add_library(ioh_vhost STATIC src/router/ioh_vhost.c)
target_include_directories(ioh_vhost PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(ioh_vhost PUBLIC ioh_router)

ioh_add_test(test_ioh_vhost tests/unit/test_ioh_vhost.c
    ioh_vhost ioh_router ioh_radix ioh_ctx ioh_request ioh_response)

# ============================================================================
# Sprint 4: Middleware chain
# ============================================================================

add_library(ioh_middleware STATIC src/middleware/ioh_middleware.c)
target_include_directories(ioh_middleware PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(ioh_middleware PUBLIC ioh_router ioh_route_group ioh_request ioh_response ioh_ctx)

ioh_add_test(test_ioh_middleware tests/unit/test_ioh_middleware.c ioh_middleware ioh_router ioh_route_group ioh_radix ioh_request ioh_response ioh_ctx)

# ============================================================================
# Sprint 4: Built-in middleware modules
# ============================================================================

add_library(ioh_cors STATIC src/middleware/ioh_cors.c)
target_include_directories(ioh_cors PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(ioh_cors PUBLIC ioh_request ioh_response ioh_ctx)

add_library(ioh_ratelimit STATIC src/middleware/ioh_ratelimit.c)
target_include_directories(ioh_ratelimit PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(ioh_ratelimit PUBLIC ioh_request ioh_response ioh_ctx)

add_library(ioh_security STATIC src/middleware/ioh_security.c)
target_include_directories(ioh_security PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(ioh_security PUBLIC ioh_request ioh_response ioh_ctx)

add_library(ioh_auth STATIC src/middleware/ioh_auth.c)
target_include_directories(ioh_auth PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(ioh_auth PUBLIC ioh_request ioh_response ioh_ctx)

ioh_add_test(test_ioh_cors tests/unit/test_ioh_cors.c ioh_cors ioh_request ioh_response ioh_ctx)
ioh_add_test(test_ioh_ratelimit tests/unit/test_ioh_ratelimit.c ioh_ratelimit ioh_request ioh_response ioh_ctx)
ioh_add_test(test_ioh_security tests/unit/test_ioh_security.c ioh_security ioh_request ioh_response ioh_ctx)
ioh_add_test(test_ioh_auth tests/unit/test_ioh_auth.c ioh_auth ioh_request ioh_response ioh_ctx)

# ============================================================================
# Sprint 5: Static file server
# ============================================================================

add_library(ioh_static STATIC src/static/ioh_static.c)
target_include_directories(ioh_static PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(ioh_static PUBLIC ioh_request ioh_response)

ioh_add_test(test_ioh_static tests/unit/test_ioh_static.c ioh_static ioh_request ioh_response)

# ============================================================================
# Sprint 5: Multipart parser
# ============================================================================

add_library(ioh_multipart STATIC src/http/ioh_multipart.c)
target_include_directories(ioh_multipart PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(ioh_multipart PUBLIC ioh_request)

ioh_add_test(test_ioh_multipart tests/unit/test_ioh_multipart.c ioh_multipart ioh_request)

# ============================================================================
# Sprint 5: SPA fallback
# ============================================================================

add_library(ioh_spa STATIC src/static/ioh_spa.c)
target_include_directories(ioh_spa PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(ioh_spa PUBLIC ioh_static ioh_request ioh_response)

ioh_add_test(test_ioh_spa tests/unit/test_ioh_spa.c ioh_spa ioh_static ioh_request ioh_response)

# ============================================================================
# Sprint 5: Compression
# ============================================================================

# Check for optional compression libraries
pkg_check_modules(ZLIB zlib)
pkg_check_modules(BROTLI_ENC libbrotlienc)

add_library(ioh_compress STATIC src/static/ioh_compress.c)
target_include_directories(ioh_compress PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(ioh_compress PUBLIC ioh_request ioh_response)

if(ZLIB_FOUND)
    target_compile_definitions(ioh_compress PRIVATE IOHTTP_HAVE_ZLIB)
    target_include_directories(ioh_compress PRIVATE ${ZLIB_INCLUDE_DIRS})
    target_link_libraries(ioh_compress PRIVATE ${ZLIB_LIBRARIES})
endif()

if(BROTLI_ENC_FOUND)
    target_compile_definitions(ioh_compress PRIVATE IOHTTP_HAVE_BROTLI)
    target_include_directories(ioh_compress PRIVATE ${BROTLI_ENC_INCLUDE_DIRS})
    target_link_libraries(ioh_compress PRIVATE ${BROTLI_ENC_LIBRARIES})
endif()

ioh_add_test(test_ioh_compress tests/unit/test_ioh_compress.c ioh_compress ioh_request ioh_response)
if(ZLIB_FOUND)
    target_compile_definitions(test_ioh_compress PRIVATE IOHTTP_HAVE_ZLIB)
endif()
if(BROTLI_ENC_FOUND)
    target_compile_definitions(test_ioh_compress PRIVATE IOHTTP_HAVE_BROTLI)
endif()

# ============================================================================
# Sprint 6: Server-Sent Events
# ============================================================================

add_library(ioh_sse STATIC src/ws/ioh_sse.c)
target_include_directories(ioh_sse PUBLIC ${CMAKE_SOURCE_DIR}/src)

ioh_add_test(test_ioh_sse tests/unit/test_ioh_sse.c ioh_sse)

# ============================================================================
# Sprint 6: WebSocket (RFC 6455, wslay-backed)
# ============================================================================

# wslay (WebSocket RFC 6455, I/O-agnostic)
find_path(WSLAY_INCLUDE_DIR NAMES wslay/wslay.h)
find_library(WSLAY_LIBRARY NAMES wslay)

add_library(ioh_websocket STATIC src/ws/ioh_websocket.c)
target_include_directories(ioh_websocket PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_include_directories(ioh_websocket PUBLIC ${WOLFSSL_INCLUDE_DIRS})
target_link_directories(ioh_websocket PUBLIC ${WOLFSSL_LIBRARY_DIRS})
target_link_libraries(ioh_websocket PUBLIC ${WOLFSSL_LIBRARIES})
if(WSLAY_INCLUDE_DIR AND WSLAY_LIBRARY)
    target_include_directories(ioh_websocket PUBLIC ${WSLAY_INCLUDE_DIR})
    target_link_libraries(ioh_websocket PUBLIC ${WSLAY_LIBRARY})
endif()

ioh_add_test(test_ioh_websocket tests/unit/test_ioh_websocket.c ioh_websocket)

# ============================================================================
# Sprint 7: HTTP/2 (nghttp2)
# ============================================================================

find_path(NGHTTP2_INCLUDE_DIR NAMES nghttp2/nghttp2.h)
find_library(NGHTTP2_LIBRARY NAMES nghttp2)

add_library(ioh_http2 STATIC src/http/ioh_http2.c)
target_include_directories(ioh_http2 PUBLIC ${CMAKE_SOURCE_DIR}/src)
if(NGHTTP2_INCLUDE_DIR AND NGHTTP2_LIBRARY)
    target_include_directories(ioh_http2 PUBLIC ${NGHTTP2_INCLUDE_DIR})
    target_link_libraries(ioh_http2 PUBLIC ${NGHTTP2_LIBRARY})
endif()
target_link_libraries(ioh_http2 PUBLIC ioh_request ioh_response ioh_ctx)

if(BUILD_TESTING AND TARGET unity AND NGHTTP2_INCLUDE_DIR AND NGHTTP2_LIBRARY)
    add_executable(test_ioh_http2 tests/unit/test_ioh_http2.c)
    target_include_directories(test_ioh_http2 PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${NGHTTP2_INCLUDE_DIR}
    )
    target_link_libraries(test_ioh_http2 PRIVATE
        unity ioh_http2 ioh_request ioh_response ${NGHTTP2_LIBRARY}
    )
    target_compile_options(test_ioh_http2 PRIVATE
        -Wno-missing-prototypes -Wno-missing-declarations
    )
    add_test(NAME test_ioh_http2 COMMAND test_ioh_http2)
endif()

# Sprint 7: HTTP/2 ALPN protocol selection integration tests
if(BUILD_TESTING AND TARGET unity AND NGHTTP2_INCLUDE_DIR AND NGHTTP2_LIBRARY)
    add_executable(test_http2_server tests/integration/test_http2_server.c)
    target_include_directories(test_http2_server PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_include_directories(test_http2_server PRIVATE ${WOLFSSL_INCLUDE_DIRS})
    target_include_directories(test_http2_server PRIVATE ${NGHTTP2_INCLUDE_DIR})
    target_link_directories(test_http2_server PRIVATE ${WOLFSSL_LIBRARY_DIRS})
    target_link_libraries(test_http2_server PRIVATE
        unity ioh_http2 ioh_http1 ioh_tls ioh_request ioh_response
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
    add_library(ioh_quic STATIC src/http/ioh_quic.c)
    target_include_directories(ioh_quic PUBLIC ${CMAKE_SOURCE_DIR}/src)
    target_include_directories(ioh_quic PUBLIC
        ${NGTCP2_INCLUDE_DIRS}
        ${NGTCP2_WOLFSSL_INCLUDE_DIRS}
        ${WOLFSSL_INCLUDE_DIRS}
    )
    target_link_directories(ioh_quic PUBLIC
        ${NGTCP2_LIBRARY_DIRS}
        ${NGTCP2_WOLFSSL_LIBRARY_DIRS}
        ${WOLFSSL_LIBRARY_DIRS}
    )
    target_link_libraries(ioh_quic PUBLIC
        ${NGTCP2_LIBRARIES}
        ${NGTCP2_WOLFSSL_LIBRARIES}
        ${WOLFSSL_LIBRARIES}
    )
    message(STATUS "QUIC transport enabled (ngtcp2 + wolfSSL)")

    if(BUILD_TESTING AND TARGET unity)
        add_executable(test_ioh_quic tests/unit/test_ioh_quic.c)
        target_include_directories(test_ioh_quic PRIVATE ${CMAKE_SOURCE_DIR}/src)
        target_include_directories(test_ioh_quic PRIVATE
            ${NGTCP2_INCLUDE_DIRS}
            ${NGTCP2_WOLFSSL_INCLUDE_DIRS}
            ${WOLFSSL_INCLUDE_DIRS}
        )
        target_link_directories(test_ioh_quic PRIVATE
            ${NGTCP2_LIBRARY_DIRS}
            ${NGTCP2_WOLFSSL_LIBRARY_DIRS}
            ${WOLFSSL_LIBRARY_DIRS}
        )
        target_link_libraries(test_ioh_quic PRIVATE
            unity ioh_quic
            ${NGTCP2_LIBRARIES}
            ${NGTCP2_WOLFSSL_LIBRARIES}
            ${WOLFSSL_LIBRARIES}
        )
        target_compile_options(test_ioh_quic PRIVATE
            -Wno-missing-prototypes -Wno-missing-declarations
        )
        target_compile_definitions(test_ioh_quic PRIVATE
            TEST_CERTS_DIR="${CMAKE_SOURCE_DIR}/tests/certs"
        )
        add_test(NAME test_ioh_quic COMMAND test_ioh_quic)
    endif()
else()
    message(STATUS "QUIC transport disabled (ngtcp2 or ngtcp2_crypto_wolfssl not found)")
endif()

# ============================================================================
# Sprint 10: HTTP/3 session (nghttp3)
# ============================================================================

if(NGHTTP3_FOUND AND TARGET ioh_quic)
    add_library(ioh_http3 STATIC src/http/ioh_http3.c)
    target_include_directories(ioh_http3 PUBLIC ${CMAKE_SOURCE_DIR}/src)
    target_include_directories(ioh_http3 PUBLIC ${NGHTTP3_INCLUDE_DIRS})
    target_link_directories(ioh_http3 PUBLIC ${NGHTTP3_LIBRARY_DIRS})
    target_link_libraries(ioh_http3 PUBLIC ${NGHTTP3_LIBRARIES} ioh_quic ioh_request ioh_response ioh_ctx)
    message(STATUS "HTTP/3 enabled (nghttp3)")

    if(BUILD_TESTING AND TARGET unity)
        add_executable(test_ioh_http3 tests/unit/test_ioh_http3.c)
        target_include_directories(test_ioh_http3 PRIVATE ${CMAKE_SOURCE_DIR}/src)
        target_include_directories(test_ioh_http3 PRIVATE ${NGHTTP3_INCLUDE_DIRS})
        target_link_directories(test_ioh_http3 PRIVATE ${NGHTTP3_LIBRARY_DIRS})
        target_link_libraries(test_ioh_http3 PRIVATE
            unity ioh_http3 ioh_quic ioh_request ioh_response ioh_ctx
            ${NGHTTP3_LIBRARIES}
        )
        target_compile_options(test_ioh_http3 PRIVATE
            -Wno-missing-prototypes -Wno-missing-declarations
        )
        add_test(NAME test_ioh_http3 COMMAND test_ioh_http3)
    endif()
else()
    message(STATUS "HTTP/3 disabled (nghttp3 or ioh_quic not available)")
endif()

# ============================================================================
# Sprint 10: Alt-Svc header (HTTP/3 discovery)
# ============================================================================

add_library(ioh_alt_svc STATIC src/http/ioh_alt_svc.c)
target_include_directories(ioh_alt_svc PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(ioh_alt_svc PUBLIC ioh_response)

ioh_add_test(test_ioh_alt_svc tests/unit/test_ioh_alt_svc.c ioh_alt_svc ioh_response)

# ============================================================================
# Sprint 10: HTTP/3 integration tests
# ============================================================================

if(BUILD_TESTING AND TARGET unity AND TARGET ioh_http3 AND TARGET ioh_quic)
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
        unity ioh_http3 ioh_quic ioh_alt_svc ioh_request ioh_response ioh_ctx
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
        unity ioh_server ioh_loop ioh_conn ioh_ctx
        ioh_http1 ioh_request ioh_response
        ioh_router ioh_radix ioh_middleware
        ioh_route_group ioh_route_inspect ioh_route_meta
    )
    target_compile_options(test_pipeline PRIVATE
        -Wno-missing-prototypes -Wno-missing-declarations
    )
    add_test(NAME test_pipeline COMMAND test_pipeline)

    add_executable(test_middleware_pipeline tests/integration/test_middleware_pipeline.c)
    target_include_directories(test_middleware_pipeline PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(test_middleware_pipeline PRIVATE
        unity ioh_server ioh_loop ioh_conn ioh_ctx
        ioh_http1 ioh_request ioh_response
        ioh_router ioh_radix ioh_middleware
        ioh_route_group ioh_route_inspect ioh_route_meta
    )
    target_compile_options(test_middleware_pipeline PRIVATE
        -Wno-missing-prototypes -Wno-missing-declarations
    )
    add_test(NAME test_middleware_pipeline COMMAND test_middleware_pipeline)

    add_executable(test_health_pipeline tests/integration/test_health_pipeline.c)
    target_include_directories(test_health_pipeline PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(test_health_pipeline PRIVATE
        unity ioh_health_test ioh_server ioh_loop ioh_conn ioh_ctx
        ioh_http1 ioh_request ioh_response
        ioh_router ioh_radix ioh_middleware
        ioh_route_group ioh_route_inspect ioh_route_meta
    )
    target_compile_options(test_health_pipeline PRIVATE
        -Wno-missing-prototypes -Wno-missing-declarations
    )
    add_test(NAME test_health_pipeline COMMAND test_health_pipeline)

    add_executable(test_keepalive tests/integration/test_keepalive.c)
    target_include_directories(test_keepalive PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(test_keepalive PRIVATE
        unity ioh_server ioh_loop ioh_conn ioh_ctx
        ioh_http1 ioh_request ioh_response
        ioh_router ioh_radix ioh_middleware
        ioh_route_group ioh_route_inspect ioh_route_meta
    )
    target_compile_options(test_keepalive PRIVATE
        -Wno-missing-prototypes -Wno-missing-declarations
    )
    add_test(NAME test_keepalive COMMAND test_keepalive)

    add_executable(test_timeout tests/integration/test_timeout.c)
    target_include_directories(test_timeout PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(test_timeout PRIVATE
        unity ioh_server ioh_loop ioh_conn ioh_ctx
        ioh_http1 ioh_request ioh_response
        ioh_router ioh_radix ioh_middleware
        ioh_route_group ioh_route_inspect ioh_route_meta
    )
    target_compile_options(test_timeout PRIVATE
        -Wno-missing-prototypes -Wno-missing-declarations
    )
    add_test(NAME test_timeout COMMAND test_timeout)

    add_executable(test_shutdown tests/integration/test_shutdown.c)
    target_include_directories(test_shutdown PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(test_shutdown PRIVATE
        unity ioh_server ioh_loop ioh_conn ioh_ctx
        ioh_http1 ioh_request ioh_response
        ioh_router ioh_radix ioh_middleware
        ioh_route_group ioh_route_inspect ioh_route_meta
    )
    target_compile_options(test_shutdown PRIVATE
        -Wno-missing-prototypes -Wno-missing-declarations
    )
    add_test(NAME test_shutdown COMMAND test_shutdown)

    add_executable(test_limits tests/integration/test_limits.c)
    target_include_directories(test_limits PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(test_limits PRIVATE
        unity ioh_server ioh_loop ioh_conn ioh_ctx
        ioh_http1 ioh_request ioh_response
        ioh_router ioh_radix ioh_middleware
        ioh_route_group ioh_route_inspect ioh_route_meta
    )
    target_compile_options(test_limits PRIVATE
        -Wno-missing-prototypes -Wno-missing-declarations
    )
    add_test(NAME test_limits COMMAND test_limits)

    add_executable(test_signal tests/integration/test_signal.c)
    target_include_directories(test_signal PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(test_signal PRIVATE
        unity ioh_server ioh_loop ioh_conn ioh_ctx
        ioh_http1 ioh_request ioh_response
        ioh_router ioh_radix ioh_middleware
        ioh_route_group ioh_route_inspect ioh_route_meta
        pthread
    )
    target_compile_options(test_signal PRIVATE
        -Wno-missing-prototypes -Wno-missing-declarations
    )
    add_test(NAME test_signal COMMAND test_signal)

    add_executable(test_request_id tests/integration/test_request_id.c)
    target_include_directories(test_request_id PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(test_request_id PRIVATE
        unity ioh_server ioh_loop ioh_conn ioh_ctx
        ioh_http1 ioh_request ioh_response
        ioh_router ioh_radix ioh_middleware
        ioh_route_group ioh_route_inspect ioh_route_meta
        ioh_log
    )
    target_compile_options(test_request_id PRIVATE
        -Wno-missing-prototypes -Wno-missing-declarations
    )
    add_test(NAME test_request_id COMMAND test_request_id)

    add_executable(test_proxy_pipeline tests/integration/test_proxy_pipeline.c)
    target_include_directories(test_proxy_pipeline PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(test_proxy_pipeline PRIVATE
        unity ioh_server ioh_loop ioh_conn ioh_ctx
        ioh_http1 ioh_request ioh_response
        ioh_router ioh_radix ioh_middleware
        ioh_route_group ioh_route_inspect ioh_route_meta
        ioh_proxy_proto ioh_log
    )
    target_compile_options(test_proxy_pipeline PRIVATE
        -Wno-missing-prototypes -Wno-missing-declarations
    )
    add_test(NAME test_proxy_pipeline COMMAND test_proxy_pipeline)

    add_executable(test_tls_pipeline tests/integration/test_tls_pipeline.c)
    target_include_directories(test_tls_pipeline PRIVATE
        ${CMAKE_SOURCE_DIR}/src ${WOLFSSL_INCLUDE_DIRS})
    target_link_directories(test_tls_pipeline PRIVATE
        ${WOLFSSL_LIBRARY_DIRS})
    target_link_libraries(test_tls_pipeline PRIVATE
        unity ioh_server ioh_loop ioh_conn ioh_ctx ioh_tls
        ioh_http1 ioh_request ioh_response
        ioh_router ioh_radix ioh_middleware
        ioh_route_group ioh_route_inspect ioh_route_meta
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

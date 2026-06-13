cmake_minimum_required(VERSION 3.20)
project(Now VERSION 1.0.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Default to shared unless explicitly set (must precede add_library)
if(NOT DEFINED BUILD_SHARED_LIBS)
    set(BUILD_SHARED_LIBS ON)
endif()

# ---- Pasta dependency (submodule) ----
set(PASTA_BUILD_TESTS OFF CACHE BOOL "" FORCE)
# Pasta replaced by Basta (strict superset) — compat shim at src/main/h/pasta.h
# add_subdirectory(lib/pasta)

# HTTPS backend: apennines native stack (TLS 1.3, HTTP/2, zero external deps).
# mbedTLS was removed in rc3 — apennines is the sole backend.

# ---- Now library ----
add_library(now
    src/main/c/now.c
    src/main/c/now_pom.c
    src/main/c/now_fs.c
    src/main/c/now_lang.c
    src/main/c/now_build.c
    src/main/c/now_manifest.c
    src/main/c/now_dirwalk.c
    src/main/c/now_version.c
    src/main/c/now_resolve.c
    lib/pico_http/src/main/c/pico_http.c
    lib/pico_http/src/main/c/pico_transport.c
    lib/pico_http/src/main/c/pico_http_apennines.c
    lib/pico_http/src/main/c/pico_h2.c
    lib/pico_http/src/main/c/pico_ws.c
    src/main/c/now_procure.c
    src/main/c/now_package.c
    src/main/c/now_workspace.c
    src/main/c/now_layer.c
    src/main/c/now_arch.c
    src/main/c/now_module.c
    src/main/c/now_cache.c
    # now-enterprise module
    components/enterprise/src/main/c/now_auth.c
    components/enterprise/src/main/c/now_audit.c
    components/enterprise/src/main/c/now_remote.c
    components/enterprise/src/main/c/now_graph.c
    components/enterprise/src/main/c/now_trust.c
    components/enterprise/src/main/c/now_repro.c
    components/enterprise/src/main/c/now_advisory.c
    components/enterprise/src/main/c/now_sbom.c
    # now-export module (nested: cmake + make + meson + bazel + maven)
    components/export/cmake/src/main/c/now_export_cmake.c
    components/export/cmake/src/main/c/now_import_cmake.c
    components/export/make/src/main/c/now_export_make.c
    components/export/meson/src/main/c/now_export_meson.c
    components/export/bazel/src/main/c/now_export_bazel.c
    components/export/maven/src/main/c/now_maven.c
    # now-plugins (nested: core + registry)
    components/plugins/core/src/main/c/now_plugin.c
    components/plugins/registry/src/main/c/now_plugin_registry.c
    # now-crypto
    components/crypto/src/main/c/now_ed25519.c
    # now-cli (nested: ci + tui + watch)
    components/cli/ci/src/main/c/now_ci.c
    components/cli/tui/src/main/c/now_tui.c
    components/cli/watch/src/main/c/now_watch.c
    # Alforno (vendored sources — avoids FetchContent conflict with pasta)
    lib/alforno/src/main/c/alforno.c
    lib/alforno/src/main/c/alforno_param.c
    lib/alforno/src/main/c/alforno_merge.c
    lib/alforno/src/main/c/alforno_link.c
    lib/alforno/src/main/c/alforno_include.c
    lib/alforno/src/main/c/alforno_validate.c
    # Apennines (vendored — tier paths match canonical apennines repo
    # one-to-one so refreshes are a literal cp -R)
    lib/apennines/src/main/c/t3/fs/fwatch.c
    lib/apennines/src/main/c/t1/buffer/buf.c
    lib/apennines/src/main/c/t2/compress/compress.c
    # Basta (vendored sources — single-file packages with embedded blobs)
    lib/basta/src/main/c/basta_lexer.c
    lib/basta/src/main/c/basta_parser.c
    lib/basta/src/main/c/basta_value.c
    lib/basta/src/main/c/basta_writer.c
)

# Apennines HTTPS stack — always compiled (the only HTTPS backend since rc3).
target_sources(now PRIVATE
    lib/apennines/src/main/c/t3/net/tcp.c
    lib/apennines/src/main/c/t3/net/tls.c
    lib/apennines/src/main/c/t3/net/dns.c
    lib/apennines/src/main/c/t3/net/http.c
    lib/apennines/src/main/c/t3/net/h2.c
    lib/apennines/src/main/c/t4/net/http_client.c
    lib/apennines/src/main/c/t4/net/https_client.c
    lib/apennines/src/main/c/t2/crypto/cipher.c
    lib/apennines/src/main/c/t2/crypto/ct.c
    lib/apennines/src/main/c/t2/crypto/ec.c
    lib/apennines/src/main/c/t2/crypto/ecdsa.c
    lib/apennines/src/main/c/t2/crypto/hash.c
    lib/apennines/src/main/c/t2/crypto/rsa.c
    lib/apennines/src/main/c/t2/crypto/secret.c
    lib/apennines/src/main/c/t2/crypto/x509.c
    lib/apennines/src/main/c/t2/encoding/asn1_der.c
    lib/apennines/src/main/c/t2/encoding/base.c
    lib/apennines/src/main/c/t2/encoding/pem.c
    lib/apennines/src/main/c/t1/random/entropy.c
    lib/apennines/src/main/c/t2/math/bigint/bigint.c
    lib/apennines/src/main/c/t3/crypto/pki.c
    lib/apennines/src/main/c/t3/async/threadpool.c
    lib/apennines/src/main/c/t2/net/addr.c
)

target_include_directories(now
    PUBLIC  $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/src/main/h>
            $<INSTALL_INTERFACE:include>
    PRIVATE ${CMAKE_SOURCE_DIR}/src/main/h/internal
            ${CMAKE_SOURCE_DIR}/lib/alforno/src/main/h
            ${CMAKE_SOURCE_DIR}/lib/alforno/src/main/c
            ${CMAKE_SOURCE_DIR}/lib/apennines/src/main/h
            ${CMAKE_SOURCE_DIR}/lib/pico_http/src/main/h
            ${CMAKE_SOURCE_DIR}/lib/pico_http/src/main/h/internal
            ${CMAKE_SOURCE_DIR}/components/enterprise/src/main/h/internal
            ${CMAKE_SOURCE_DIR}/components/export/src/main/h/internal
            ${CMAKE_SOURCE_DIR}/components/plugins/core/src/main/h/internal
            ${CMAKE_SOURCE_DIR}/components/plugins/registry/src/main/h/internal
            ${CMAKE_SOURCE_DIR}/components/cli/ci/src/main/h/internal
            ${CMAKE_SOURCE_DIR}/components/cli/tui/src/main/h/internal
            ${CMAKE_SOURCE_DIR}/components/cli/watch/src/main/h/internal
            ${CMAKE_SOURCE_DIR}/lib/basta/src/main/h
            ${CMAKE_SOURCE_DIR}/lib/basta/src/main/c
)

# Basta sources are vendored directly into libnow (no separate library)
if(WIN32)
    target_link_libraries(now PRIVATE ws2_32)
endif()

target_compile_definitions(now PRIVATE PICO_HTTP_APENNINES)

# POSIX feature-test macros for Linux/glibc. With CMAKE_C_EXTENSIONS OFF
# (i.e. -std=c11 not -std=gnu11), glibc hides clock_gettime, strdup,
# setenv, realpath, struct addrinfo, etc. behind these macros.
# _DEFAULT_SOURCE additionally re-enables BSD-isms removed from strict
# POSIX (usleep, useconds_t, etc.). Macros are harmless on macOS/BSD
# but unnecessary; gate on Linux to keep darwin headers permissive.
# Covers both now's own sources and vendored apennines/basta .c files
# compiled into the same target.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(NOW_LINUX_POSIX_DEFS _POSIX_C_SOURCE=200809L _DEFAULT_SOURCE)
    target_compile_definitions(now PRIVATE ${NOW_LINUX_POSIX_DEFS})
endif()

if(WIN32)
    target_link_libraries(now PRIVATE bcrypt)
endif()

# Threads. now_build / now_manifest call pthread_create directly. On
# modern glibc (2.34+) libpthread is folded into libc so it links with
# no flag, and Windows uses its own threading — but FreeBSD keeps
# pthreads in libthr and needs an explicit -pthread at link time.
# Link PUBLIC so the flag propagates to now_cli / now_test when now is
# built static (the failing case: libnow.a). Threads::Threads is a
# no-op on platforms that don't need it.
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)
target_link_libraries(now PUBLIC Threads::Threads)


if(BUILD_SHARED_LIBS)
    target_compile_definitions(now PRIVATE NOW_BUILDING PICO_HTTP_BUILDING ALF_BUILDING BASTA_BUILDING APENNINES_STATIC)
    set_target_properties(now PROPERTIES
        VERSION   ${PROJECT_VERSION}
        SOVERSION 0
        C_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN ON
    )
else()
    target_compile_definitions(now PUBLIC NOW_STATIC PICO_HTTP_STATIC ALF_STATIC BASTA_STATIC APENNINES_STATIC)
    set_target_properties(now PROPERTIES
        VERSION   ${PROJECT_VERSION}
    )
endif()

if(WIN32)
    set_target_properties(now PROPERTIES
        PREFIX "lib"
        IMPORT_PREFIX "lib"
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
        ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib
    )
endif()

# Place test exe next to the DLL so it finds it at runtime
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# ---- CLI executable ----
add_executable(now_cli components/cli/app/src/main/c/main.c)
set_target_properties(now_cli PROPERTIES OUTPUT_NAME now)
target_link_libraries(now_cli PRIVATE now)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_compile_definitions(now_cli PRIVATE ${NOW_LINUX_POSIX_DEFS})
endif()
target_include_directories(now_cli PRIVATE
    ${CMAKE_SOURCE_DIR}/src/main/h/internal
    ${CMAKE_SOURCE_DIR}/lib/alforno/src/main/h
    ${CMAKE_SOURCE_DIR}/lib/basta/src/main/h
    ${CMAKE_SOURCE_DIR}/lib/pico_http/src/main/h
    ${CMAKE_SOURCE_DIR}/components/enterprise/src/main/h/internal
    ${CMAKE_SOURCE_DIR}/components/export/src/main/h/internal
    ${CMAKE_SOURCE_DIR}/components/plugins/core/src/main/h/internal
            ${CMAKE_SOURCE_DIR}/components/plugins/registry/src/main/h/internal
    ${CMAKE_SOURCE_DIR}/components/cli/ci/src/main/h/internal
            ${CMAKE_SOURCE_DIR}/components/cli/tui/src/main/h/internal
            ${CMAKE_SOURCE_DIR}/components/cli/watch/src/main/h/internal
)

# ---- Test executable ----
option(NOW_BUILD_TESTS "Build test executable" ON)
if(NOW_BUILD_TESTS)
    add_executable(now_test src/test/c/now_test.c)
    target_link_libraries(now_test PRIVATE now)
    target_include_directories(now_test PRIVATE
        ${CMAKE_SOURCE_DIR}/src/main/h/internal
        ${CMAKE_SOURCE_DIR}/lib/alforno/src/main/h
        ${CMAKE_SOURCE_DIR}/lib/alforno/src/main/c
        ${CMAKE_SOURCE_DIR}/lib/basta/src/main/h
        ${CMAKE_SOURCE_DIR}/lib/basta/src/main/c
        ${CMAKE_SOURCE_DIR}/lib/pico_http/src/main/h
        ${CMAKE_SOURCE_DIR}/lib/pico_http/src/main/h/internal
        ${CMAKE_SOURCE_DIR}/components/enterprise/src/main/h/internal
        ${CMAKE_SOURCE_DIR}/components/export/src/main/h/internal
        ${CMAKE_SOURCE_DIR}/components/plugins/core/src/main/h/internal
            ${CMAKE_SOURCE_DIR}/components/plugins/registry/src/main/h/internal
        ${CMAKE_SOURCE_DIR}/components/cli/ci/src/main/h/internal
            ${CMAKE_SOURCE_DIR}/components/cli/tui/src/main/h/internal
            ${CMAKE_SOURCE_DIR}/components/cli/watch/src/main/h/internal
    )
    target_compile_definitions(now_test PRIVATE
        NOW_TEST_RESOURCES="${CMAKE_SOURCE_DIR}/src/test/resources"
    )
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        target_compile_definitions(now_test PRIVATE ${NOW_LINUX_POSIX_DEFS})
    endif()
endif()

# ---- Install rules ----
include(GNUInstallDirs)
install(TARGETS now now_cli
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)
install(FILES src/main/h/now.h
              src/main/h/pico_http.h
              src/main/h/pico_ws.h
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

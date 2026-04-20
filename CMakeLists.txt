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
    # Apennines (vendored — fwatch, buf, compress, optionally full HTTP stack)
    lib/apennines/src/main/c/fwatch.c
    lib/apennines/src/main/c/buf.c
    lib/apennines/src/main/c/compress.c
    # Basta (vendored sources — single-file packages with embedded blobs)
    lib/basta/src/main/c/basta_lexer.c
    lib/basta/src/main/c/basta_parser.c
    lib/basta/src/main/c/basta_value.c
    lib/basta/src/main/c/basta_writer.c
)

# Apennines HTTPS stack — always compiled (the only HTTPS backend since rc3).
target_sources(now PRIVATE
    lib/apennines/src/main/c/tcp.c
    lib/apennines/src/main/c/tls.c
    lib/apennines/src/main/c/dns.c
    lib/apennines/src/main/c/http.c
    lib/apennines/src/main/c/h2.c
    lib/apennines/src/main/c/http_client.c
    lib/apennines/src/main/c/https_client.c
    lib/apennines/src/main/c/cipher.c
    lib/apennines/src/main/c/ct.c
    lib/apennines/src/main/c/ec.c
    lib/apennines/src/main/c/ecdsa.c
    lib/apennines/src/main/c/hash.c
    lib/apennines/src/main/c/rsa.c
    lib/apennines/src/main/c/secret.c
    lib/apennines/src/main/c/x509.c
    lib/apennines/src/main/c/asn1_der.c
    lib/apennines/src/main/c/base.c
    lib/apennines/src/main/c/pem.c
    lib/apennines/src/main/c/entropy.c
    lib/apennines/src/main/c/bigint.c
    lib/apennines/src/main/c/pki.c
    lib/apennines/src/main/c/threadpool.c
    lib/apennines/src/main/c/addr.c
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
if(WIN32)
    target_link_libraries(now PRIVATE bcrypt)
endif()


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

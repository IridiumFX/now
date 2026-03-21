# now — v1.0 Release Candidate

**Version**: 1.0.0-rc1
**Date**: 2026-03-21
**Tests**: 313 passing
**Languages**: C, C++, C++20 modules, asm-gas, asm-nasm, Java, Rust, Go, Julia

---

## What is now?

`now` is a native-language build tool and package manager. Single binary, zero configuration for standard projects, full lifecycle from compile to publish.

```
now init          # scaffold a new project
now build         # procure → compile → link
now test          # build → compile tests → run
now publish       # build → package → publish to registry
now watch         # rebuild on file changes
```

---

## Architecture

### Project Descriptor

`now.pasta` — one file describes the entire project using the Pasta serialization format:

```
{
  group:    "dev.iridium",
  artifact: "mylib",
  version:  "1.0.0",
  lang:     "c",
  sources:  { dir: "src/main/c", headers: "src/main/h" },
  compile:  { warnings: ["Wall", "Wextra"], std: "c11" },
  link:     { output: "shared" },
  deps:     [{ id: "org.acme:core:^1.5.0" }]
}
```

### Source Modules (46 files)

| Module | Purpose |
|--------|---------|
| `now.c` | Public API: build, compile, test, version |
| `now_pom.c` | Project Object Model — parses `now.pasta` |
| `now_build.c` | Parallel compilation, toolchain detection, process pool |
| `now_manifest.c` | Incremental builds: SHA-256 + mtime, hash memoization |
| `now_cache.c` | Content-addressable build cache, header-aware two-level keys |
| `now_version.c` | SemVer 2.0 parsing, ranges, intersection |
| `now_resolve.c` | Dependency resolver, convergence policies, lock file |
| `now_procure.c` | Registry download, SHA-256 verify, local repo install |
| `now_package.c` | Tarball assembly, publish, yank |
| `now_auth.c` | Token/LDAP/OIDC auth, token caching, registry discovery |
| `now_remote.c` | Remote object cache with circuit breaker |
| `now_graph.c` | Build graph cache serialization (pasta format) |
| `now_audit.c` | Client-side audit trail (`~/.now/audit.pasta`) |
| `now_watch.c` | File watcher with fwatch backend |
| `now_tui.c` | TUI progress display (ANSI, `--tui` flag) |
| `now_lang.c` | Language registry: C, C++, asm, Java, Rust, Go, Julia |
| `now_fs.c` | Filesystem utilities, source discovery |
| `now_workspace.c` | Multi-module workspace, DAG topo sort, wave build |
| `now_layer.c` | Cascading config layers, alforno integration |
| `now_plugin.c` | Plugin hooks, built-in generators, external IPC |
| `now_plugin_registry.c` | Plugin marketplace: search, install, info |
| `now_ci.c` | CI integration: structured output, env detection |
| `now_export.c` | Export: CMake, Make, Meson, Bazel |
| `now_maven.c` | Maven import/export (pom.xml roundtrip) |
| `now_sbom.c` | SBOM: CycloneDX 1.5 JSON |
| `now_trust.c` | Signing, trust store, scope matching |
| `now_repro.c` | Reproducible builds |
| `now_advisory.c` | Security advisory guards |
| `now_module.c` | C++20 module pre-scan, topo sort |
| `now_arch.c` | Platform triples, host detection |
| `now_ed25519.c` | Native Ed25519 (SHA-512 + GF(2^255-19)) |
| `pico_http.c` | HTTP/1.1 client (standalone, MIT) |
| `pico_h2.c` | HTTP/2 client (HPACK, frame codec, ALPN) |
| `pico_transport.c` | Socket + TLS transport layer |
| `pico_http_apennines.c` | Apennines HTTPS backend adapter |
| `pico_ws.c` | WebSocket client (RFC 6455, permessage-deflate) |
| `main.c` | CLI entry point, 40+ commands |

### Dependencies

| Library | Source | Purpose |
|---------|--------|---------|
| Pasta | submodule | Serialization format |
| Basta | submodule | Pasta + binary blobs |
| Alforno | vendored | Pasta processor (templates, config merge) |
| Apennines fwatch | vendored | File system event watcher |
| Apennines compress | vendored | LZ4 + Deflate (permessage-deflate) |
| Apennines HTTPS | vendored, optional | TLS 1.3 + HTTP/2 (replaces mbedTLS) |
| mbedTLS | submodule, optional | TLS (default backend) |

---

## CLI Commands

### Build Lifecycle
```
now build              procure → compile → link
now compile            compile only
now link               link only
now test               build → test
now procure            dependency resolution
now package            build → assemble tarball
now install            package → install to local repo
now publish            package → upload to registry
now yank               remove published version
now clean              delete target/
```

### Development
```
now watch              watch sources, rebuild on changes
now watch --poll 200   custom poll interval (ms)
now init               scaffold new project
now fmt                format now.pasta
now compile-db         generate compile_commands.json
now ci                 CI mode (structured output, exit codes)
```

### Registry & Auth
```
now auth:login         authenticate (--method token|ldap|oidc)
now auth:status        show cached tokens
now auth:logout        clear cached token
now dep:updates        check for newer dependency versions
```

### Cache
```
now cache:clean        purge local build cache
now cache:stats        show cache object count + size
now cache:remote-stats show remote cache connectivity
```

### Export
```
now export:cmake       generate CMakeLists.txt
now export:make        generate Makefile
now export:meson       generate meson.build
now export:bazel       generate BUILD.bazel
now export:maven       generate pom.xml
now import:maven       convert pom.xml to now.pasta
```

### Trust & Security
```
now trust:list         show trust store
now trust:add          add signing key
now verify             verify artifact signatures
now sbom               generate CycloneDX 1.5 SBOM
now advisory:check     check deps against advisory DB
now advisory:update    update advisory database
now reproducible:check verify build reproducibility
```

### Plugins
```
now plugin:list        list installed plugins
now plugin:search      search plugin registry
now plugin:install     install a plugin
now plugin:info        show plugin details
```

### Config & Audit
```
now layers:show        show resolved config layers
now layers:audit       report policy violations
now audit:show         show audit log (--event, --last)
now version            print version
```

### Flags
```
-v                     verbose output
-j N                   parallel jobs (default: CPU count)
--tui                  live progress display
--output FMT           output format: text, json, pasta
--locked               fail if lock file inconsistent
--offline              no network access
--target TRIPLE        cross-compile target
--repo URL             registry URL override
--no-color             disable ANSI colors
```

---

## Language Support

| Language | Extensions | Tool | FFI Model |
|----------|-----------|------|-----------|
| C | `.c`, `.i` | `gcc`/`clang`/`cl.exe` | Native |
| C++ | `.cpp`, `.cc`, `.cxx`, `.cppm`, `.ixx` | `g++`/`clang++`/`cl.exe` | Native |
| C++20 Modules | `.cppm`, `.ixx`, `.ccm` | Pre-scanned, topo sorted | BMI paths |
| asm-gas | `.s`, `.S` | `as`/`gcc` | Object files |
| asm-nasm | `.asm` | `nasm` | Object files |
| Java | `.java` | `javac`/`jar` | Maven layout |
| Rust | `.rs` | `rustc --crate-type staticlib` | `extern "C"` FFI |
| Go | `.go` | `go build -buildmode=c-archive` | `//export` cgo |
| Julia | `.jl` | Embedded via `libjulia` | `jl_eval_string` |

---

## HTTP Client (pico_http v0.3.0)

Standalone MIT-licensed HTTP client, suitable for independent release:

- HTTP/1.1 with chunked transfer encoding, redirects, streaming
- HTTP/2 via ALPN (HPACK, frame codec, single-stream)
- TLS via mbedTLS (default) or apennines stack (optional, `-DPICO_HTTP_APENNINES=ON`)
- WebSocket client (RFC 6455) with permessage-deflate (RFC 7692)
- Platform: Windows (Winsock2), Linux, macOS, FreeBSD
- Connect timeout with IPv6 fallback and error detection

---

## Build Performance

| Scenario | Time |
|----------|------|
| Cold build (46 files, 4-way) | ~23s |
| No-op build | <1s |
| Incremental (1 file) | ~1-2s |

Optimizations: hash memoization, manifest mtime fast-path, link-skip, remote cache circuit breaker (latency-based), depfile-aware cache.

---

## Registry Integration (Cookbook)

Tested end-to-end with Cookbook registry server (619 tests):

- Token auth: `POST /auth/token` → JWT
- LDAP auth: `method: ldap` with graceful fallback
- Object cache: `PUT/GET /objects/{key}` — 42 objects pushed in one build
- Graph cache: `PUT/GET /graphs/{key}` — manifest serialization
- Audit: pasta-format logs on both client and server
- SBOM: CycloneDX 1.5 JSON with purl and dependency graph

---

## Test Suite

313 tests covering: POM parsing, language system, filesystem, SemVer, ranges, coordinates, manifest, resolver, lock file, HTTP client, TLS, WebSocket, procure, parallel build, toolchain, auth (token/LDAP/OIDC), publish, yank, cache, depfile parsing, dep-aware cache, manifest deps, remote cache, graph cache, enterprise auth, SBOM, audit logging, watch, Rust FFI, Go FFI, Julia, C++20 modules, multi-arch, exports (CMake/Make/Meson/Bazel/Maven), Ed25519, plugins, plugin registry, dep confusion, reproducible builds, trust, advisory, CI, layers, alforno integration, basta packages, build integration.

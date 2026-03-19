# now - Implementation Status

**Version**: 0.1.0 (development)
**Date**: 2026-03-07

`now` is a native-language build tool and package manager for C/C++.
This document describes what is implemented, what is planned, and the
architectural decisions made so far.

---

## Project Structure

Maven-style C project layout:

```
src/main/h/          Public headers (now.h)
src/main/c/          Library sources
src/test/c/          Test sources
src/test/resources/  Test data (pasta files, sample projects)
lib/pasta/           Pasta serialization library (git submodule)
lib/cookbook/         Registry server (git submodule)
specs/               Specification and implementation guide
docs/                This document
```

Build system: CMake 4.0+, C11, Ninja generator.
Shared library by default, static via `-DBUILD_SHARED_LIBS=OFF`.
CI: Linux, macOS (arm64), FreeBSD (vmactions), Windows (MSVC).

---

## Source Modules

| Module | Header | Description |
|--------|--------|-------------|
| `now.c` | `now.h` | Public API: version, project accessors, build/compile/test entry points |
| `now_pom.c` | `now_pom.h` | Project Object Model — parses `now.pasta` into `NowProject` struct |
| `now_fs.c` | `now_fs.h` | Filesystem utilities: path join, mkdir_p, source discovery, obj path derivation |
| `now_lang.c` | `now_lang.h` | Language type system: extension registry for C, C++, asm-gas, asm-nasm |
| `now_build.c` | `now_build.h` | Build, link, test phases: parallel compilation, toolchain resolution |
| `now_manifest.c` | `now_manifest.h` | Incremental build manifest: SHA-256 hashing, Pasta serialization |
| `now_version.c` | `now_version.h` | SemVer 2.0 parsing/comparison, version range parsing, coordinate parsing |
| `now_resolve.c` | `now_resolve.h` | Dependency resolver: constraint collection, range intersection, lock file |
| `now_procure.c` | `now_procure.h` | Procurement phase: registry query, download, SHA-256 verify, local repo install |
| `now_package.c` | `now_package.h` | Package and install phases: tarball assembly, local repo extraction |
| `pico_http.c` | `pico_http.h` | HTTP/1.1 client: GET/HEAD/POST/PUT/PATCH/DELETE, streaming GET, redirects, chunked TE, TLS |
| `pico_ws.c` | `pico_ws.h` | WebSocket client (RFC 6455): text/binary frames, ping/pong, TLS |
| `pico_internal.h` | — | Shared transport layer: PicoConn (raw TCP + optional mbedTLS) |
| `now_ci.c` | `now_ci.h` | CI integration: structured output, exit codes, env detection, `now ci` lifecycle |
| `now_layer.c` | `now_layer.h` | Cascading config layers: baseline, file/project layers, section merge, audit trail |
| `now_arch.c` | `now_arch.h` | Platform triples: parse, format, host detect, wildcard match, native detection |
| `now_export.c` | `now_export.h` | Build system export: CMakeLists.txt, Makefile, meson.build, BUILD.bazel generation from NowProject |
| `now_trust.c` | `now_trust.h` | Signing and trust: trust store, scope matching, policy, minisign verification |
| `now_repro.c` | `now_repro.h` | Reproducible builds: timebase, prefix maps, sorted inputs, date macro neutralization |
| `now_advisory.c` | `now_advisory.h` | Advisory guards: advisory DB loading, severity checking, override mechanism, dep checking |
| `now_plugin.c` | `now_plugin.h` | Plugin system: hook dispatch, built-in plugins, external process invocation with stdin/stdout Pasta IPC |
| `now_plugin_registry.c` | `now_plugin_registry.h` | Plugin registry: manifest parsing, search, install, list, info, binary discovery |
| `now_workspace.c` | `now_workspace.h` | Workspace/module system: DAG construction, Kahn's topo sort, wave build |
| `now_cache.c` | `now_cache.h` | Content-addressable build cache: SHA-256 key, two-level sharding, header-aware (depfile parsing, ccache-style two-level key) |
| `main.c` | — | CLI entry point: phase dispatch, option parsing |

---

## Implemented Capabilities

### 1. Project Descriptor (`now.pasta`) — Guide Step 2

Parses the full project descriptor including:

- **Identity**: group, artifact, version, name, description, url, license
- **Language**: `lang:` (scalar), `langs:` (array), `"mixed"` expands to `["c", "c++"]`
- **Standard**: `std:` field (e.g. `"c11"`, `"c++17"`)
- **Sources**: configurable source and header directories with defaults per language
- **Output**: type (executable, static, shared, header-only) and name
- **Compile settings**: warnings, defines, includes, flags, optimization level
- **Link settings**: flags, libraries, library directories, linker scripts
- **Dependencies**: id (group:artifact:version-range), scope, optional, override, exclude
- **Repositories**: url, id, release/snapshot flags, auth
- **Convergence**: lowest, highest, or exact policy
- **Private groups**: `private_groups:` array of group prefixes for dep confusion protection
- **Modules**: workspace module list

Default source directories are derived from the primary language (e.g. `src/main/c`
for C projects, `src/main/cpp` for C++).

### 2. Directory Layout — Guide Step 3

Standard Maven-style layout with `target/` as the build output root:

```
target/
  obj/main/         Compiled object files (mirroring source tree)
  obj/test/         Compiled test object files
  bin/              Final binaries (executables, libraries)
  pkg/              Package staging area and tarballs
  .now-manifest     Incremental build manifest
```

Object files preserve the source directory structure:
`src/main/c/net/parser.c` produces `target/obj/main/net/parser.c.o`.

### 3. Language Type System — Guide Step 4

Built-in registry with four language definitions:

| Language | Extensions (source) | Extensions (header) | Tool |
|----------|--------------------|--------------------|------|
| C | `.c`, `.i` | `.h` | `${cc}` |
| C++ | `.cpp`, `.cxx`, `.cc`, `.C`, `.ii` | `.hpp`, `.hxx`, `.hh`, `.H`, `.h` | `${cxx}` |
| asm-gas | `.s`, `.S` | | `${as}` |
| asm-nasm | `.asm`, `.nasm` | | `${nasm}` |

Classification respects language priority order per the spec. Files are
categorized by role (source, header) and produce type (object, intermediate, none).

### 4. Build Phase (Compile) — Guide Step 5

Direct compiler invocation (no Make/Ninja generation):

- **Toolchain resolution** from environment variables (`CC`, `CXX`, `AR`, `AS`)
  with sensible defaults per platform
- **GCC/Clang flag translation**: `-std=`, warnings with `-` prefix, `-D` defines,
  `-I` includes, optimization mapping (none/debug/size/speed/lto)
- **Source discovery**: recursive directory walk with extension filtering
- **Object directory creation**: automatic `target/obj/main/` tree creation
- **Subprocess execution**: `CreateProcess` on Windows, `fork/execvp` on POSIX
- **Dependency include/lib injection**: `-I`, `-L`, `-l` flags from resolved deps

### 5. Incremental Build Manifest — Guide Step 6

SHA-256 based rebuild detection stored at `target/.now-manifest` in Pasta format:

- **Per-source tracking**: source path, object path, source hash, flags hash, mtime
- **Fast path**: mtime comparison first; only hashes on mtime change
- **Flags change detection**: recompiles if any compiler flag changes
- **Object existence check**: rebuilds if the `.o` file is missing
- **Header dependency tracking**: compiler depfiles (`-MD -MF` for GCC/Clang, `/showIncludes` for MSVC) parsed after compilation; dep paths and hashes stored per manifest entry; `needs_rebuild` checks all header hashes
- **Embedded SHA-256**: public-domain implementation (Brad Conte), no external dependency
- **Deterministic output**: uses `PASTA_SORTED` for canonical key ordering

On subsequent builds, unchanged sources are skipped entirely.

### 5b. Content-Addressable Build Cache

Compiled objects are cached at `~/.now/cache/objects/` and survive `now clean`.
Uses a ccache-style two-level key for header awareness:

- **source_key**: `SHA-256(source_hash + flags_hash + compiler_path)` — identifies source+flags combo
- **result_key**: `SHA-256(source_key + sorted_dep_hashes)` — includes all header content
- **Storage**: two-level sharding `{root}/ab/cd/{key}{ext}` (65536 buckets)
- **`.deps` sidecar**: `{source_key}.deps` maps to dep list + hashes + result_key
- **Depfile parsing**: GCC/Clang `.d` format (Makefile-style, backslash continuations) and MSVC `/showIncludes` output
- **Lookup**: compute source_key → read .deps → verify dep hashes → restore result object
- **Store**: parse compiler depfile → hash deps → compute result_key → store object + .deps sidecar
- **Backward compatible**: old cache entries without `.deps` miss gracefully and recompile with deps
- **CLI**: `now cache:clean` (purge), `now cache:stats` (object count + total size)

Build summary reports: `compiled N, cached N, skipped N (up to date)`.

### 6. Link Phase — Guide Step 7

Three output types supported:

| Type | Tool | Output |
|------|------|--------|
| `executable` | compiler driver (cc/cxx) | `target/bin/{name}[.exe]` |
| `static` | `ar rcs` | `target/bin/lib{name}.a` (`.lib` on Windows) |
| `shared` | compiler driver with `-shared` | `target/bin/lib{name}.so` (`.dylib`/`.dll`) |

Handles library directories (`-L`), link libraries (`-l`), and link flags.
Uses C++ compiler driver when any C++ source is present.
Injects dependency library paths and names from resolved deps.

### 7. Semantic Versioning and Ranges — Guide Step 8

Full SemVer 2.0 implementation:

- **Parsing**: `MAJOR.MINOR.PATCH[-prerelease][+build]`
- **Comparison**: numeric precedence, pre-release ordering per spec section 11
  (`1.0.0-rc.1 < 1.0.0`), build metadata ignored
- **Formatting**: `now_semver_to_string` round-trips correctly

All range types from the spec:

| Syntax | Kind | Expanded |
|--------|------|----------|
| `1.2.3` | Exact | Only 1.2.3 |
| `^1.2.3` | Caret | `>=1.2.3 <2.0.0` |
| `^0.9.3` | Caret (pre-1.0) | `>=0.9.3 <0.10.0` |
| `~1.2.3` | Tilde | `>=1.2.3 <1.3.0` |
| `>=1.2.0` | Floor | Any version >= 1.2.0 |
| `>=1.2.0 <2.0.0` | Compound | Explicit intersection |
| `*` | Any | Any version |

Range satisfaction checking and range intersection are both implemented.
Coordinate parsing: `group:artifact:version-range` format.

### 8. Dependency Resolution — Guide Step 8

Constraint-based resolver with convergence policies:

- **Constraint collection**: parses `group:artifact:version-range` coordinates
- **Range intersection**: intersects all constraints on the same coordinate
- **Convergence policies**:
  - `lowest` — selects the floor of the intersected range (default)
  - `highest` — placeholder; needs registry query for available versions
  - `exact` — requires all constraints to agree on one version
- **Override support**: `override: true` forces a specific version
- **Conflict reporting**: identifies which constraints conflict and who declared them

### 9. Lock File (`now.lock.pasta`) — Guide Step 8

Serialized in Pasta format with per-entry fields:

- group, artifact, version (exact resolved)
- triple (platform or "noarch")
- url, sha256, descriptor_sha256
- scope, deps (direct dependency coordinates)
- overridden flag
- **Deterministic output**: uses `PASTA_SORTED` for canonical key ordering

Load and save operations preserve all fields through round-trips.

### 10. Procure Phase — Guide Steps 9–10

Full dependency procurement lifecycle:

- **Registry query**: `GET /resolve/{group}/{artifact}/{range}` via pico_http
- **Artifact download**: `GET /artifact/{path}` to retrieve `.tar.gz` archives
- **SHA-256 verification**: downloaded archives verified against registry-provided hash
- **Local repo install**: extracted to `~/.now/repo/{group-path}/{artifact}/{version}/`
- **Skip-if-present**: already-installed deps are not re-downloaded
- **Dependency path injection**: resolved dep paths feed `-I`, `-L`, `-l` into build
- **Auto-procure**: `now build`, `now compile`, `now test` all run procure automatically

### 11. Test Phase — Guide Step 11

Compiles and runs test sources:

- Discovers test sources from `src/test/c/` (or configured `tests.dir`)
- Compiles with production include paths (source + header directories)
- Links test objects together with production objects into `target/bin/{artifact}-test`
- Executes the test binary and checks exit code (0 = pass)
- Framework support: `none` (user-provided `main()`), others planned

### 12. Parallel Build — Guide Step 12

Process-pool based parallel compilation:

- **CPU detection**: `now_cpu_count()` via `GetSystemInfo` (Windows),
  `sysctlbyname` (macOS), `sysconf` (Linux/POSIX)
- **Process pool**: spawns up to N worker processes with captured stdout/stderr
- **Output buffering**: per-worker output collected via pipes, printed atomically
  on completion — no interleaving between workers
- **Wait mechanism**: `WaitForMultipleObjects` (Windows), `waitpid(-1)` (POSIX)
- **Incremental integration**: manifest check happens before dispatch; up-to-date
  files are skipped without spawning a process
- **CLI flag**: `now build -j N` (or `-jN`); default is CPU count
- **Single-job fast path**: when jobs=1, uses direct `now_exec` without pipes

### 13. Package Phase — Guide Step 15

Assembles distributable archive from build output:

- **Staging**: copies descriptor, headers, and built binaries to `target/pkg/{artifact}-{version}/`
- **Platform triple**: directory layout uses `bin/{triple}/` and `lib/{triple}/`
- **Triple detection**: `now_host_triple()` detects OS/arch/ABI (linux-x86_64-gnu, windows-x86_64-msvc, etc.)
- **Tarball creation**: `{artifact}-{version}-{triple}.tar.gz` in `target/pkg/`
- **SHA-256 sidecar**: `.sha256` file alongside the tarball
- **Windows compatibility**: uses `tar --force-local` to handle drive-letter paths

### 14. Install Phase — Guide Step 15

Copies packaged artifact into the local repo:

- **Target**: `~/.now/repo/{group-path}/{artifact}/{version}/`
- **Layout**: `now.pasta` descriptor, `h/` headers, `bin/{triple}/`, `lib/{triple}/`
- **Overwrites**: replaces existing installation for the same version

### 15. CLI Executable

Full command-line interface via `now` binary:

- **Phases**: `build`, `compile`, `link`, `test`, `procure`, `package`, `install`, `clean`, `version`
- **Options**: `-v` (verbose), `-j N` (parallel jobs), `-h` (help)
- **Lifecycle**: `package` and `install` automatically run `build` first
- **Error handling**: descriptive error messages for missing descriptor, unknown phase, etc.

### 16. HTTP Client (pico_http v0.3.0)

Standalone MIT-licensable HTTP/1.1 client:

- **Platform sockets**: POSIX `socket`/`connect`/`send`/`recv`, Windows `Winsock2`
- **DNS resolution**: `getaddrinfo` for IPv4/IPv6 dual-stack
- **Methods**: GET, HEAD, POST, PUT, PATCH, DELETE + URL-based `pico_http_request`
- **Streaming**: `pico_http_get_stream` — callback-based body delivery, no memory buffering
- **Response parsing**: status line, headers, `Content-Length` and chunked transfer encoding
- **Redirects**: 301/302/303/307/308 with cross-scheme (http↔https) tracking
- **TLS**: optional HTTPS via mbedTLS (`PICO_HTTP_TLS` compile flag)
  - CA verification: `MBEDTLS_SSL_VERIFY_REQUIRED` by default (was VERIFY_NONE)
  - System CA loading: `CertOpenSystemStore` on Windows, well-known PEM paths on POSIX
  - Options: `tls_noverify` (disable), `ca_file` (custom PEM), `ca_data` (in-memory PEM)
  - SNI hostname set for virtual hosting
- **URL parser**: `pico_http_parse_url_ex` with scheme/host/port/path/tls extraction
- **Connect timeout**: non-blocking connect with `select` (Windows) or `poll` (POSIX)

### 17. Publish Phase — Guide Step 26

Uploads packaged artifacts to a remote registry:

- **Lifecycle**: `now publish` runs build → package → publish
- **CLI**: `now publish --repo https://pkg.example.com/now`
- **Registry URL**: from `--repo` flag or first entry in project `repos:`
- **Files uploaded**: `now.pasta` descriptor, `.tar.gz` archive, `.sha256` sidecar
- **Artifact path**: `PUT /artifact/{group}/{artifact}/{version}/{filename}`
  (group dots become path separators: `org.acme` → `org/acme`)
- **Credentials**: loaded from `~/.now/credentials.pasta` (Pasta format)
  - Matches by registry URL prefix
  - Token sent as `Authorization: Bearer` header
- **Error handling**: validates identity (group/artifact/version), checks tarball exists

### 18. Plugins and Generate Phase — Guide Step 14

Plugin system with lifecycle hook dispatch and built-in code generators:

- **Plugin declaration**: `plugins:` array in `now.pasta` with `id`, `type`, `phase`, `config`, `timeout`
- **Built-in plugins** (no external process needed):
  - `now:version` — generates `target/generated/_now_version.c` with `NOW_GROUP`, `NOW_ARTIFACT`,
    `NOW_VERSION`, `NOW_VERSION_MAJOR/MINOR/PATCH`, `NOW_BUILD_TIME`
  - `now:embed` — embeds binary files as C arrays from configurable `src` directory with
    configurable `prefix`; generates `target/generated/_now_embed.c`
- **Lifecycle hook dispatch**: 13 hook points (pre/post for procure, compile, link, test, package, publish + generate)
  - Plugins register for hooks via `phase:` field
  - Multiple plugins per hook, executed in declaration order
  - Chain stops on first error (`status: "error"` or non-zero exit)
- **Generate phase**: runs after procure, before compile; collects `sources`, `includes`, `defines`
  from plugins and injects them into the build context
- **External plugin invocation**: full process spawning with stdin/stdout Pasta IPC
  - Windows: `CreateProcess()` with pipe handles, read/write Pasta payload
  - POSIX: `fork() + execl()` with `pipe()` for stdin/stdout
  - Input: `hook`, `project`, `basedir`, `target`, `config`
  - Output: `status` (ok/warn/error), `sources`, `includes`, `defines`, `messages`
  - Timeout support (default 30s, configurable via `timeout:` field)
  - Coordinate-based binary lookup: `~/.now/repo/{group}/{artifact}/{version}/bin/{artifact}`
- **Plugin registry** (`now_plugin_registry.c/h`):
  - `now plugin:list` — scan `~/.now/repo/` for installed plugins with `plugin.pasta` manifests
  - `now plugin:search <query>` — case-insensitive substring match on id/name/description
  - `now plugin:install <g:a:v>` — resolve from registry, download .basta, extract, validate manifest
  - `now plugin:info <g:a:v>` — show plugin manifest details (hooks, capabilities, network, binary path)
  - `plugin.pasta` manifest parsing: id, name, description, protocol, hooks, requires, optional, network, requires_now
- **CLI**: `now generate` runs generate phase standalone; `now build/compile/test` run it automatically

### 19. CI Integration — Guide Step 22

Full CI pipeline support with structured output and environment detection:

- **Structured exit codes**: 0 (success), 1 (build error), 2 (test failure), 3 (dep resolution),
  4 (config error), 5 (I/O error), 6 (auth failure). Mapped from `NowError` via `now_exit_code()`
- **Output formats**: `--output json` (JSON), `--output pasta` (Pasta), `--output text` (human-readable)
  - Auto-detection: JSON for CI environments, text for TTY, Pasta for piped output
  - Build results include: phase, status, duration_ms, steps (total/compiled/cached/failed)
  - Test results include: phase, status, duration_ms, tests (total/passed/failed/skipped)
- **CI environment detection**: `now_ci_detect()` reads env vars:
  - `CI`, `GITHUB_ACTIONS`, `GITLAB_CI` for platform detection
  - `NOW_LOCKED`, `NOW_OFFLINE`, `NOW_COLOR`, `NO_COLOR` for mode flags
- **CI annotations**: GitHub Actions `::error file=...::` and GitLab `section_start/end` markers
- **`now ci` command**: composite lifecycle (build → test) with:
  - Structured output to stdout
  - Report written to `target/now-ci-report.json`
  - Proper exit codes for CI interpretation
- **CLI flags**: `--locked`, `--offline`, `--no-color`, `--output FMT`

### 20. Dep Confusion Protection — Guide Step 24

Prevents dependency confusion attacks via private group enforcement:

- **`private_groups:`** array in `now.pasta` — lists group prefixes that must never
  resolve from public/default registries
- **Dotted prefix matching**: `org.acme` matches `org.acme` and `org.acme.internal`
  but NOT `org.acmecorp` (dot-boundary enforced)
- **Hard failure**: if a dep's group matches a private prefix and the project has
  no declared `repos:`, procure fails with a descriptive error
- **Registry ordering**: private group deps only resolve from project-declared repos;
  the default fallback registry is blocked
- **Multiple prefixes**: supports any number of private group prefixes
- **Config loading**: parsed from `now.pasta` alongside other project fields

### 21. Workspace and Modules — Guide Step 17

Multi-module workspace support with topological build ordering:

- **Workspace detection**: `now_is_workspace()` checks for `modules:` array in root descriptor
- **Module loading**: loads each module's `now.pasta` from its subdirectory
- **DAG construction**: builds dependency graph from sibling references in `deps[]` arrays
  - Parses `group:artifact:version` IDs, matches artifact name against workspace modules
  - Edge: if module B depends on module A, A must build before B
- **Kahn's algorithm**: topological sort producing parallel build waves
  - Wave = set of modules with zero unbuilt dependencies (in-degree 0)
  - After building a wave, decrement in-degree of dependents
  - Cycle detection: if no zero-in-degree nodes remain but sort is incomplete → cycle
- **Wave-based build**: iterates waves in order, builds each module via `now_build()`
  - TODO: parallelize modules within a wave
- **CLI integration**: `now build` auto-detects workspace and dispatches to wave builder

### 19. MSVC Support — Guide Step 23

Full MSVC toolchain support alongside GCC/Clang:

- **Toolchain detection**: `CC=cl.exe` (or any CC containing "cl") triggers MSVC mode
- **Tool resolution**: `cl.exe` (CC), `lib.exe` (AR), `ml64.exe` (AS), `link.exe` (linker)
- **Compile flag translation**:

| `now.pasta` | GCC/Clang | MSVC |
|-------------|-----------|------|
| `std: "c11"` | `-std=c11` | `/std:c11` |
| `Wall` | `-Wall` | `/W4` |
| `Wextra` | `-Wextra` | `/W4` |
| `Werror` | `-Werror` | `/WX` |
| `opt: "none"` | `-O0` | `/Od` |
| `opt: "debug"` | `-Og` | `/Od /Zi` |
| `opt: "size"` | `-Os` | `/O1` |
| `opt: "speed"` | `-O2` | `/O2` |
| `opt: "lto"` | `-O2 -flto` | `/O2 /GL` |
| defines | `-DFOO` | `/DFOO` |
| includes | `-Ipath` | `/Ipath` |
| output | `-o file` | `/Fofile` |

- **Link flag translation**:

| Operation | GCC/Clang | MSVC |
|-----------|-----------|------|
| Static lib | `ar rcs lib.a objs...` | `lib.exe /OUT:lib objs...` |
| Shared lib | `cc -shared -o lib.dll objs... -Ldir -llib` | `link.exe /DLL /OUT:lib.dll objs... /LIBPATH:dir lib.lib` |
| Executable | `cc -o app objs... -Ldir -llib` | `link.exe /OUT:app.exe objs... /LIBPATH:dir lib.lib` |

- **Object extension**: `.obj` (via `now_obj_path_ex`) instead of `.o`
- **Dependency injection**: `/I`, `/LIBPATH:`, `name.lib` instead of `-I`, `-L`, `-l`
- **Test phase**: MSVC-aware compile and link for test sources

### 22. Layer System — Guide Step 18

Cascading configuration layers with policy enforcement:

- **Layer stack**: baseline → filesystem-discovered → project, lowest to highest priority
- **Built-in baseline**: default compile (Wall/Wextra), repos (central), toolchain (gcc),
  advisory phase guards, private_groups, link
- **Section policies**: `open` (free override) and `locked` (overrides produce audit warnings)
- **Layer document format**: Pasta maps with `_policy`, `_description`, `_override_reason` metadata
- **Filesystem discovery**: walks from project dir upward, loading `.now-layer.pasta` files,
  stops at VCS root or home directory
- **Section merge**: bottom-up merge with type-aware handling:
  - Arrays: accumulate in locked mode (can't remove), `!exclude:` removes in open mode
  - Maps: recursive merge, overlay wins for scalars
  - Scalars: overlay wins (with audit violation if locked)
- **Project layer**: pushes compile, private_groups from NowProject as top layer
- **Deep clone**: `pasta_clone()` helper for safe value copying across merge steps
- **Workaround**: builds fresh maps on each merge step to avoid Pasta's `pasta_set()` append behavior

### 23. Multi-Architecture and Platform Triples — Guide Step 16

Platform triple system for multi-architecture builds:

- **Triple format**: `os:arch:variant` (e.g. `linux:amd64:gnu`, `windows:amd64:msvc`)
- **Components**:
  - OS: linux, macos, windows, freebsd, openbsd, freestanding
  - Arch: amd64, arm64, arm32, riscv64, riscv32, x86, wasm32
  - Variant: gnu, musl, msvc, mingw, none
- **Host detection**: compile-time detection of OS, architecture, and ABI variant
- **Triple parsing**: `now_triple_parse()` handles full and partial triples
- **Shorthand fill**: `:amd64:musl` fills OS from host via `now_triple_fill_from_host()`
- **Formatting**: `now_triple_format()` (colon-separated) and `now_triple_dir()` (dash-separated for paths)
- **Wildcard matching**: `now_triple_match()` with `*` in any component position
- **Native detection**: `now_triple_is_native()` compares target against host
- **CLI flag**: `--target os:arch:variant` for specifying build target

### 24. Layer Audit — Guide Step 25

CLI commands for layer inspection and audit:

- **`now layers:show`**: displays the full layer stack with source, policy, and description
  for each section. `--effective` flag shows merged configuration as Pasta.
- **`now layers:audit`**: merges all sections and reports advisory lock violations.
  Exit code 0 if clean, 1 if violations exist. Output includes section, locking layer,
  overriding layer, field, override reason, and violation code (NOW-W0401).

### 25. Export to CMake

Zero-lock-in escape hatch — generates a standalone CMakeLists.txt from `now.pasta`:

- **`now export:cmake`**: writes `CMakeLists.txt` in the project root
- **Target types**: executable (`add_executable`), static/shared (`add_library`), header-only (`INTERFACE`)
- **Compile settings**: warnings (with `-` prefix), defines, optimization, raw flags, include dirs
- **Link settings**: flags, libraries, library directories
- **Language support**: C and C++ (auto-detects from `langs:`, sets `CMAKE_C_STANDARD` / `CMAKE_CXX_STANDARD`)
- **Source discovery**: `file(GLOB_RECURSE ...)` matching now's recursive directory walk
- **Test target**: optional `{artifact}_test` executable from test source directory
- **Dependencies**: listed as comments with `FetchContent` hint (cannot auto-resolve across build systems)
- **Install rules**: `GNUInstallDirs`-based install targets for binaries and headers
- **Header comment**: includes regeneration command (`now export:cmake`)

### E2. Export to Makefile

Zero-lock-in escape hatch — generates a standalone GNU Makefile from `now.pasta`:

- **`now export:make`**: writes `Makefile` in the project root
- **Target types**: executable (direct link), static (`ar rcs`), shared (`-shared -fPIC`)
- **Compile settings**: `-std=`, warnings, defines (`-D`), optimization, include paths, raw flags
- **Link settings**: library directories (`-L`), libraries (`-l`), raw flags
- **Language support**: C (`CC ?= gcc`) and C++ (`CXX ?= g++`, auto-detects from `langs:`)
- **Source discovery**: `$(wildcard ...)` for `.c`, `.cpp`, `.cxx`, `.cc`
- **Test target**: `make test` compiles test sources, links, and runs the test binary
- **Dependencies**: listed as comments (cannot auto-resolve across build systems)
- **Install target**: `make install PREFIX=/usr/local` with proper permissions
- **Clean target**: `make clean` removes `target/` directory
- **Pattern rules**: separate `%.c.o`, `%.cpp.o` rules for proper multi-language support

### 25b. Meson Export (`now export:meson`)

Zero-lock-in escape hatch — generates a standalone `meson.build` from `now.pasta`:

- **`now export:meson`**: writes `meson.build` in the project root
- **project()**: language, version, license, default_options (std, optimization, b_lto)
- **Target types**: `executable()`, `static_library()`, `shared_library()`, `declare_dependency()` (header-only)
- **Compile settings**: `c_args`/`cpp_args` array with warnings, defines, raw flags
- **Link settings**: `link_args` array with flags, library dirs, libraries
- **Include directories**: `include_directories()` with public + private headers
- **Source discovery**: `run_command('find', ...)` for `.c`, `.cpp`, `.cxx`, `.cc`
- **Language support**: C and C++ (auto-detects from `langs:`, uses `cpp` language name)
- **Test target**: `test()` with `executable()` linked against the library
- **Header-only**: `declare_dependency(include_directories: inc)` — no test/install targets
- **Dependencies**: listed as comments (cannot auto-resolve across build systems)
- **Install**: `install_subdir()` for headers, `install : true` on targets
- **4 tests**: basic shared lib, executable, C++ project, header-only declare_dependency

### 25c. Bazel Export (`now export:bazel`)

Zero-lock-in escape hatch — generates a standalone `BUILD.bazel` from `now.pasta`:

- **`now export:bazel`**: writes `BUILD.bazel` in the project root
- **load()**: `@rules_cc//cc:defs.bzl` for `cc_binary`, `cc_library`, `cc_test`
- **package()**: `default_visibility = ["//visibility:public"]`
- **Target types**: `cc_binary` (executable), `cc_library` (static/shared/header-only)
- **COPTS**: `-std=`, optimization, warnings, defines, raw flags
- **LINKOPTS**: raw flags, library dirs (`-L`), libraries (`-l`)
- **Source patterns**: `glob()` for `.c`, `.cpp`, `.cc`, `.cxx`, `.h`, `.hpp`
- **includes + strip_include_prefix**: proper header path handling
- **Static libraries**: `linkstatic = True`
- **Test target**: `cc_test()` with `deps` linking to library target
- **Dependencies**: listed as comments (cannot auto-resolve across build systems)
- **5 tests**: basic cc_library, cc_binary, static linkstatic, C++ globs, deps comments

### 26. Reproducible Builds — Guide Step 20

Determinism measures for bit-identical builds across machines and time:

- **`reproducible: true`** — boolean shorthand enables all measures with defaults
- **`reproducible: { ... }`** — map form for selective control of individual measures
- **Timebase resolution**: controls timestamps embedded in build outputs
  - `"now"` — wall clock (default, non-reproducible)
  - `"git-commit"` — HEAD commit timestamp (reproducible across machines)
  - `"zero"` — Unix epoch (maximum reproducibility)
  - ISO 8601 literal — pinned timestamp (e.g. `"2026-03-05T00:00:00Z"`)
- **`path_prefix_map`**: strips absolute build paths from debug info and `__FILE__`
  - GCC/Clang: `-fdebug-prefix-map=<basedir>=.` + `-fmacro-prefix-map=<basedir>=.`
  - MSVC: `/pathmap:<basedir>=.`
- **`no_date_macros`**: neutralizes `__DATE__` and `__TIME__` with timebase-derived values
  - Injects `-D__DATE__="Mar  5 2026"` and `-D__TIME__="14:30:00"` (formatted from timebase)
- **`sort_inputs`**: sorts source file lists lexicographically before compilation and linking
  - Ensures deterministic ordering regardless of filesystem enumeration order
- **`strip_metadata`**: uses deterministic ELF build IDs
  - GCC/Clang: `-Wl,--build-id=sha1` (content-based, reproducible)
- **`verify`**: enables post-build verification (used by `now reproducible:check`)
- **`now reproducible:check`**: builds twice, compares output SHA-256 hashes
  - Deletes manifest between builds to force full rebuild
  - Reports PASS (match) or FAIL (differ)
- **Build integration**: repro compile flags injected into compile jobs, repro link flags into link phase
- **14 tests**: config parsing (bool/map/none), timebase resolution (zero/now/literal),
  compile flags (GCC/MSVC), link flags, sort, disabled no-op, null safety

### 26. Signing and Trust — Guide Step 19

Trust store management and signature verification for package integrity:

- **Trust store**: `~/.now/trust.pasta` — Pasta-format key database with scope-based matching
- **Scope rules**:
  - `*` — wildcard, matches any group/artifact
  - `org.acme` — group prefix with dot-boundary (`org.acme.sub` matches, `org.acmetools` does not)
  - `org.acme:core` — exact group:artifact match
- **Trust policy**: parsed from `trust:` section in `now.pasta`
  - `require_signatures: true` — reject unsigned packages (level: SIGNED)
  - `require_known_keys: true` — reject packages signed by unknown keys (level: TRUSTED)
- **Trust levels**: NONE (SHA-256 only), SIGNED (any valid signature), TRUSTED (known key required)
- **Signature verification**: native Ed25519 (RFC 8032) — no external dependencies
  - `now_ed25519_keypair()` — derive Ed25519 keypair from 32-byte seed
  - `now_ed25519_sign()` — sign a message with Ed25519
  - `now_ed25519_verify()` — verify an Ed25519 signature
  - `now_verify_file()` — verify archive against `.sig` file and base64 public key
  - Self-contained SHA-512, GF(2^255-19) field arithmetic (ref10-style 10-limb)
- **CLI commands**:
  - `now trust:list` — display all keys in the trust store
  - `now trust:add <scope> <key> [comment]` — add a key to the trust store
  - `now verify <archive> <sigfile>` — verify archive signature against trust store
- **Persistence**: load/save via Pasta format with `keys:` array of `{ scope, key, comment }` entries

### 21. Advisory Guards — Guide Step 21

Security advisory database integration for vulnerability checking during builds:

- **Advisory database**: `~/.now/advisories/now-advisory-db.pasta` — Pasta-format database
  - Entries: id, CVE list, severity, title, description, affects, fixed_in, blacklisted flag
  - Build-time vs runtime distinction for scope-aware checking
- **Severity levels**: info, low, medium, high, critical, blacklisted
  - Blacklisted: hard error, cannot be overridden
  - Critical/high: hard error, can be overridden with justification
  - Medium/low: warning, build continues
  - Info: recorded silently
- **Override mechanism**: `advisories: { allow: [...] }` in `now.pasta`
  - Mandatory `expires` date — overrides without expiry are rejected
  - Fields: advisory, dep, reason, expires, approved_by
  - Expired overrides fail the build with a reminder to re-evaluate
- **Dep checking**: each project dep checked against advisory DB
  - Version range matching against advisory `affects` entries
  - Scope-aware: runtime-only vuln in test dep does not block
  - Wildcard version ranges supported for blacklisted artifacts
- **CLI commands**:
  - `now advisory:check` — check project deps against advisory database
  - `now advisory:update` — placeholder (requires HTTP client for feed pull)
- **Report formatting**: severity, dep ID, advisory ID, override status, fix suggestions

### 18. WebSocket Client (pico_ws v0.1.0)

Native RFC 6455 WebSocket implementation sharing the `PicoConn` transport:

- **Handshake**: HTTP/1.1 Upgrade with `Sec-WebSocket-Key` (random base64)
- **Frames**: text and binary, FIN always set, client masking per RFC
- **Control frames**: close (reciprocal), ping (auto-pong), pong (skip)
- **Extended lengths**: 7-bit, 16-bit, and 64-bit payload lengths
- **TLS**: WSS via mbedTLS (same `PICO_HTTP_TLS` flag)
- **Platform RNG**: `SystemFunction036` (Windows), `/dev/urandom` (POSIX)

---

## Test Suite

257 tests across all modules:

| Category | Count | Description |
|----------|-------|-------------|
| Version | 1 | Library version string |
| POM (string) | 6 | Load from string: minimal, lang scalar, mixed, compile, deps, convergence |
| POM (file) | 2 | Load from .pasta files (minimal, rich) |
| POM (errors) | 3 | Syntax error, non-map root, missing file |
| Language | 7 | Find, classify (C, header, C++, unknown), source extensions |
| Filesystem | 4 | Path join, trailing separator, extension, obj path derivation |
| SemVer | 5 | Parse basic, prerelease, build; compare ordering; to_string roundtrip |
| Ranges | 8 | Exact, caret, caret pre-1.0, tilde, >=, compound, wildcard, intersection |
| Coordinates | 1 | Parse group:artifact:version |
| Manifest | 3 | Set/find, update, SHA-256 known-answer test (dep tracking tested separately) |
| Resolver | 5 | Single dep, convergence, conflict, multiple deps, override |
| Lock file | 1 | Save and reload round-trip |
| HTTP | 11 | Version, URL parsing (http/https/ftp reject), error strings, invalid args, DNS fail, connect fail, header lookup, response free, stream invalid args, stream connect fail, stream callback type |
| WebSocket | 6 | Version, error codes, invalid args, bad URL, connect failure, close NULL |
| Procure | 2 | Repo dep path construction, no-deps no-op |
| Parallel | 1 | CPU count detection |
| Toolchain | 3 | obj_path_ex .obj extension, GCC default, MSVC detect from CC=cl.exe |
| Publish | 2 | Missing identity rejected, missing package rejected |
| Workspace | 5 | Detect workspace root, single project not workspace, NULL safe, init modules/graph, topo sort ordering |
| Plugins | 6 | Built-in detection, POM loading, no-plugins no-op, result lifecycle, unknown builtin error, now:version generation |
| Plugin Registry | 10 | Manifest parse (full/minimal/missing-id/missing-file), info_free null safe, find_binary missing, list empty, search no-match, install bad registry, manifest roundtrip |
| CI | 6 | Exit code mapping, env detection, JSON/Pasta/text build format, JSON test format |
| Dep confusion | 7 | Exact match, dotted child, no false positive, multiple prefixes, NULL-safe, POM load, procure fail |
| Layers | 8 | Stack init, baseline sections, file load, open merge, locked audit, !exclude:, audit format, push project |
| Multi-arch | 9 | Triple parse, shorthand fill, format, dir name, compare, wildcard match exact, wildcard *, host detect, native detection |
| Export | 18 | CMake (4): shared, executable, deps, C++; Make (5): shared, executable, static, deps, C++; Meson (4): shared, executable, C++, header-only; Bazel (5): cc_library, cc_binary, static, C++, deps |
| Repro | 14 | Config init/bool/map/none, timebase zero/now/literal, compile flags GCC/MSVC, link flags/MSVC, sort, disabled, null |
| Trust | 12 | Init/free, add, scope wildcard/prefix/exact, find/no-match, policy none/signed/trusted, project parse, null safety |
| Advisory | 17 | DB init/free, severity parse/name/blocks, load string, blacklisted, override parse/no-expires/expiry, find override, check dep match/no-match/overridden, blacklisted-no-override, medium warning, report format, null safety |
| Package | 3 | Host triple detection, package phase, install phase |
| Build cache | 8 | Key determinism, key variation (source/flags/compiler), sharding, store/restore, miss, clean |
| Depfile parsing | 4 | Simple .d, multiline continuations, missing file, MSVC /showIncludes |
| Dep-aware cache | 3 | No .deps miss, store/restore with deps, dep deleted |
| Manifest deps | 3 | set_deps, save/load roundtrip, needs_rebuild on dep change |
| Build | 1 | Full compile+link of hello project (integration test) |
| CLI | 2 | Version command, help text |

All 257 tests pass (247 in CI — build integration test requires gcc in PATH at runtime).

---

## Dependencies

### Pasta (git submodule at `lib/pasta`)

MIT-licensed serialization format library. Used for:
- Parsing `now.pasta` project descriptors
- Reading/writing `.now-manifest` build manifests
- Reading/writing `now.lock.pasta` lock files
- `PASTA_SORTED` flag for deterministic canonical output

### mbedTLS (git submodule at `lib/mbedtls`)

Apache-2.0 licensed TLS library (v3.6.5). Used for:
- HTTPS support in pico_http (`PICO_HTTP_TLS` compile flag)
- WSS support in pico_ws
- BIO callbacks wrapping platform sockets (no `mbedtls_net` dependency)
- CA certificate verification: `MBEDTLS_SSL_VERIFY_REQUIRED` by default
- System CA loading: Windows `CertOpenSystemStore` + well-known POSIX paths
- `PicoHttpOptions.tls_noverify` / `ca_file` / `ca_data` for caller control

### Cookbook (git submodule at `lib/cookbook`)

MIT-licensed registry server. Phases A–E complete, 219 tests. Capabilities:

- **HTTP API**: resolve, download, publish, yank, auth, mirror manifest, metrics, health
- **Content negotiation**: `application/x-pasta` (default), `application/json`, `text/plain`; `?pretty` query param; `PASTA_COMPACT | PASTA_SORTED` canonical output
- **Data backends**: SQLite (default), PostgreSQL (optional via libpq)
- **Object stores**: Filesystem (default), S3-compatible (optional, raw sockets + AWS Sig V4)
- **Security**: JWT auth (EdDSA/Ed25519), publisher signatures, registry countersignatures, SHA-256 on ingest, ASCII enforcement, rate limiting, path traversal prevention
- **Tooling**: `cookbook-import` CLI for air-gapped environments, `cookbook_stress` concurrent stress test driver
- **Vendored**: civetweb (HTTP), libsodium (crypto), SQLite, Pasta

---

## Not Yet Implemented

Ordered by implementation priority (per the guide Part VIII).
Steps marked **[Post-v1]** are fully specified but not required for v1.

### v1 Core

| Step | Feature | Status | Notes |
|------|---------|--------|-------|
| 1 | Pasta parser | **DONE** | Submodule at lib/pasta |
| 2 | POM loader | **DONE** | Identity, lang, sources, compile, link, deps, repos, convergence |
| 3 | Directory layout | **DONE** | target/, obj naming, source discovery |
| 4 | Language type system | **DONE** | C, C++, asm-gas, asm-nasm |
| 5 | Build phase | **DONE** | GCC/Clang compilation with flag translation |
| 6 | Incremental manifest | **DONE** | SHA-256 + mtime, Pasta serialization, PASTA_SORTED |
| 7 | Link phase | **DONE** | Executable, static, shared |
| 8 | Dependency resolution | **DONE** | SemVer 2.0, ranges, convergence, lock file |
| 9 | Procure phase | **DONE** | HTTP download, SHA-256 verify, local repo install |
| 10 | Registry client | **DONE** | pico_http client, resolve + fetch endpoints |
| 11 | Test phase | **DONE** | Compile test sources, link with prod objects, execute |
| 12 | Parallel build | **DONE** | Process pool, -j flag, CPU auto-detect, output buffering |
| 13 | Module pre-scan | **Post-v1** | Moved to post-v1; coupled with C++20 modules and additional languages |
| 14 | Plugins and generate phase | **DONE** | Built-in plugins, hook dispatch, generate phase, IPC protocol |
| 15 | Packaging and install | **DONE** | Tarball assembly, SHA-256 sidecar, local repo extraction |
| 16 | Multi-architecture | **DONE** | Triple parsing, host detection, wildcard match, --target CLI |
| 17 | Workspace and modules | **DONE** | Root descriptor, Kahn's topo sort, wave-based build |
| 18 | Layer system | **DONE** | Baseline, file/project layers, section merge, policy enforcement |
| 19 | Signing and trust | **DONE** | Trust store, scope matching, policy, minisign verification, CLI |
| 20 | Reproducible builds | **DONE** | Timebase, prefix maps, sorted inputs, date macros, verify, CLI |
| 21 | Advisory guards | **DONE** | Advisory DB (Pasta format), severity blocking, override mechanism, dep checking, CLI |
| 22 | CI integration | **DONE** | Structured output (JSON/Pasta/text), exit codes, `now ci`, env detection |
| 23 | MSVC support | **DONE** | Flag translation, `cl.exe`/`link.exe`/`lib.exe`, `.obj`, dep injection |
| 24 | Dep confusion protection | **DONE** | Private group enforcement, prefix matching, registry ordering |
| 25 | Layer audit | **DONE** | `now layers:show`, `now layers:audit`, violation reporting |
| 26 | Publish | **DONE** | HTTP PUT to registry, credential loading, `--repo` CLI flag |

### v1 Additions

| Step | Feature | Status | Notes |
|------|---------|--------|-------|
| E1 | `now export:cmake` | **DONE** | Generate CMakeLists.txt from now.pasta — zero lock-in escape hatch |
| E2 | `now export:make` | **DONE** | Makefile generation: shared/static/executable, C/C++, deps as comments, test+install+clean |
| E4 | `now export:meson` | **DONE** | Meson build generation: project(), executable/static/shared/header-only, copts, linkopts, test target, install |
| E5 | `now export:bazel` | **DONE** | Bazel BUILD generation: cc_binary/cc_library/cc_test, COPTS/LINKOPTS, glob() patterns, linkstatic |
| E6 | Plugin registry | **DONE** | `now plugin:list/search/install/info`, manifest parsing, external process invocation, 10 tests |

### Post-v1

| Step | Feature | Notes |
|------|---------|-------|
| 13 | Module pre-scan | C++20 modules; coupled with additional languages |
| 27 | IDE integration | Compile database, `now stay` daemon, LSP bridge |
| 28 | Embedded platforms | Freestanding output, custom linker scripts, platform registry |
| 29 | Additional languages | Ada, Fortran, Modula-2, Pascal, and others |
| E3 | ~~Embedded Ed25519~~ | **DONE** | Native SHA-512 + Ed25519 keypair/sign/verify/file-verify (ref10-style field arithmetic, 7 tests) |

**Completed: 27 of 27 v1 steps** (steps 1–12, 14–21, 22–26, E1, E2, CLI, pico networking). All v1 steps done.

---

## Spec Design Notes (Not Yet Implemented)

The following features are fully specified in `specs/now-spec-v1.2.md` and documented
here for planning purposes. None of these are implemented yet.

### Lifecycle Hooks (§10.2)

Plugins register hooks that run at specific points in the build lifecycle.
Multiple plugins can register the same hook; they execute in declaration order.

| Hook | Execution Point | Example Use |
|------|-----------------|-------------|
| `pre-procure` | Before dependency resolution | Inject synthetic dependencies |
| `post-procure` | After dependencies installed | Validate dependency graph |
| `generate` | After procure, before compile | Code generation (protobuf, bindings) |
| `pre-compile` | After generate, before compile | Formatting, linting |
| `post-compile` | After compile, before link | Static analysis on compiled objects |
| `pre-link` | Before link | Modify object list |
| `post-link` | After link, before test | Binary patching, signing |
| `pre-test` | Before test execution | Set up fixtures, mocks |
| `post-test` | After test execution | Coverage reports |
| `pre-package` | Before packaging | Add extra files to archive |
| `post-package` | After packaging | Notarisation, signing |
| `pre-publish` | Before publish | Final validation |
| `post-publish` | After successful publish | Confirmation notifications, tag creation, changelog updates |

### Cache Structure (§13.3)

Three cache layers, all under `~/.now/`:

#### 1. Download Cache (`~/.now/cache/`)

Holds immutable downloaded archives. Never modified by build — only by `procure`
(populating) and `vacate --purge` (clearing).

```
~/.now/cache/
└── {group-path}/{artifact}/{version}/
    ├── {artifact}-{version}-{triple}.tar.gz      # Downloaded archive
    ├── {artifact}-{version}-{triple}.sha256      # Expected hash from registry
    └── {artifact}-{version}-noarch.tar.gz        # Header-only variant
```

- Incomplete downloads use `.tmp` path; moved to final path only after SHA-256 verification
- Cache validity: exists at expected path AND SHA-256 matches lock file record
- Commands: `now procure --warm` (pre-populate), `now cache:export/import` (transfer),
  `now cache:mirror` (mirror registry), `now cache:verify` (check integrity)

#### 2. Object Cache (`~/.now/object-cache/`) — §14.13

Content-addressed cache for compiled `.o` files, shared across all projects on the machine.
Keyed by node hash (computed from source content + compiler flags + include paths).

```
~/.now/object-cache/
└── {node-hash-prefix}/{node-hash}/
    ├── output.o         # The object file
    └── output.d         # The dependency file
```

Before compiling, `now` checks: if node hash matches a cached entry, fetch the object
instead of invoking the compiler. Result: clean checkouts of previously-built projects
rebuild in seconds — zero compilations, only the link step runs.

Also supports remote object cache:

```pasta
{
  object_cache: {
    url:   "https://build-cache.internal/objects",
    token: "${env.NOW_CACHE_TOKEN}",
    push:  true
  }
}
```

#### 3. Build Graph Cache (`target/.now-graph`) — §14.6

Compiled build graphs fingerprinted by `sha256(fingerprint map as canonical Pasta)`.
Supports remote graph cache protocol (`GET/PUT /graphs/{fingerprint-hash}`).

### CI Cache Integration (§19.4)

Cache keys use `now.lock.pasta` hash for dependency cache, and source hash for object cache:

**GitHub Actions:**

```yaml
# Dependency cache
- uses: actions/cache@v4
  with:
    path: ~/.now/cache
    key: now-${{ runner.os }}-${{ hashFiles('now.lock.pasta') }}
    restore-keys: now-${{ runner.os }}-

# Object cache
- uses: actions/cache@v4
  with:
    path: ~/.now/object-cache
    key: now-obj-${{ runner.os }}-${{ hashFiles('now.lock.pasta', 'src/**') }}
    restore-keys: |
      now-obj-${{ runner.os }}-${{ hashFiles('now.lock.pasta') }}-
      now-obj-${{ runner.os }}-
```

**GitLab CI:**

```yaml
cache:
  key:
    files: [now.lock.pasta]
  paths: [.now-cache/]
variables:
  NOW_CACHE_DIR: "$CI_PROJECT_DIR/.now-cache"
```

### Cache-Aware Build Modes

| Flag | Behavior |
|------|----------|
| `--locked` | Use lock file strictly; fail if `now.pasta` and lock are inconsistent (CI default) |
| `--offline` | Never touch network; fail if cache is incomplete |
| `--refresh` | Ignore lock; re-resolve and re-fetch everything |

---

## Platform Notes

- **Windows**: `strndup` is not available on either MSVC or MinGW; a compatibility
  shim is used (`#ifdef _WIN32`). Drive-letter paths require special handling in
  `now_mkdir_p` and tar invocation (`--force-local`). Process execution uses
  `CreateProcess` with `WaitForSingleObject` (single) or `WaitForMultipleObjects`
  (parallel pool). MinGW gcc requires its bin directory in PATH for `cc1.exe` to
  find `libssp-0.dll`.
- **POSIX**: Process execution uses `fork`/`execvp`/`waitpid`. Parallel pool uses
  `waitpid(-1)` to collect any finished child.
- **macOS**: CPU count via `sysctlbyname("hw.logicalcpu")`.
- **Build integration tests**: The build integration test requires `gcc` in the
  runtime PATH. It passes when MinGW bin is in PATH, fails otherwise.

---

## Proposals

- **`now execute`** — Graph-aware task runner with Pasta descriptors ("pastlets").
  See `docs/proposal-execute.md` for full design.

# now — User Guide

Complete reference for building C/C++ projects with `now`.

---

## Table of Contents

1. [Project Descriptor](#1-project-descriptor)
2. [Directory Layout](#2-directory-layout)
3. [Build Lifecycle](#3-build-lifecycle)
4. [Compile and Link Configuration](#4-compile-and-link-configuration)
5. [Dependencies](#5-dependencies)
6. [Version Ranges](#6-version-ranges)
7. [Testing](#7-testing)
8. [Parallel Builds](#8-parallel-builds)
9. [Packaging and Publishing](#9-packaging-and-publishing)
10. [Workspaces](#10-workspaces)
11. [Plugins](#11-plugins)
12. [Multi-Architecture Builds](#12-multi-architecture-builds)
13. [Reproducible Builds](#13-reproducible-builds)
14. [Security](#14-security)
15. [Configuration Layers](#15-configuration-layers)
16. [Exporting to Other Build Systems](#16-exporting-to-other-build-systems)
17. [CLI Reference](#17-cli-reference)

---

## 1. Project Descriptor

Every `now` project has a `now.pasta` file at its root. This is the single source of truth for your project's identity, dependencies, and build configuration.

### Identity

```pasta
{
  group:       "org.acme",
  artifact:    "rocketlib",
  version:     "3.0.0",
  name:        "Rocket Library",
  description: "High-performance rocket propulsion simulator",
  url:         "https://github.com/acme/rocketlib",
  license:     "Apache-2.0"
}
```

The **coordinate** `group:artifact:version` uniquely identifies your project in the registry. Use reverse-domain notation for `group` (e.g., `org.acme`, `io.github.user`).

### Language and Standard

```pasta
{
  langs: ["c"],       ; or ["c", "c++"] for mixed projects
  std:   "c11"        ; c89, c99, c11, c17, c23, c++11, c++14, c++17, c++20, c++23
}
```

Shorthand: `lang: "c"` is equivalent to `langs: ["c"]`. `lang: "mixed"` expands to `langs: ["c", "c++"]`.

### Output

```pasta
{
  output: {
    type: "executable",   ; executable | static | shared | header-only
    name: "myapp"
  }
}
```

Platform-specific filenames are derived automatically:

| Type | Linux | macOS | Windows |
|------|-------|-------|---------|
| `executable` | `myapp` | `myapp` | `myapp.exe` |
| `static` | `libmyapp.a` | `libmyapp.a` | `myapp.lib` |
| `shared` | `libmyapp.so` | `libmyapp.dylib` | `myapp.dll` |

---

## 2. Directory Layout

`now` uses a Maven-style directory layout:

```
project-root/
  now.pasta               Project descriptor
  src/
    main/
      c/                  C source files (*.c)
      h/                  Public headers (*.h)
    test/
      c/                  Test source files
  target/                 Build output (auto-created)
    obj/main/             Object files
    obj/test/             Test object files
    bin/                  Final binaries
    pkg/                  Package staging
    .now-manifest         Incremental build manifest
```

Default directories depend on the primary language:

| Language | Sources | Headers |
|----------|---------|---------|
| C | `src/main/c` | `src/main/h` |
| C++ | `src/main/cpp` | `src/main/hpp` |
| Mixed | `src/main` | `src/main/h` |

You can override any directory:

```pasta
{
  sources: {
    dir:     "lib",
    headers: "include",
    pattern: "**.c",
    exclude: ["vendor/**.c"]
  },
  tests: {
    dir: "test"
  }
}
```

---

## 3. Build Lifecycle

The lifecycle phases, in order:

```
procure → generate → compile → link → test → package → install → publish
```

Each command runs its phase plus all prerequisite phases:

| Command | Runs |
|---------|------|
| `now build` | procure → generate → compile → link |
| `now compile` | procure → generate → compile |
| `now test` | procure → generate → compile → link → test |
| `now package` | full build → package |
| `now install` | full build → package → install |
| `now publish` | full build → package → publish |

### Incremental Builds

`now` tracks every source file's SHA-256 hash, mtime, and the compiler flags used to compile it. On subsequent builds, only changed files are recompiled. The manifest is stored at `target/.now-manifest`.

### Cleaning

```sh
now clean          # deletes entire target/ directory
```

---

## 4. Compile and Link Configuration

### Compile

```pasta
{
  compile: {
    warnings: ["Wall", "Wextra", "Wpedantic"],  ; -W prepended automatically
    defines:  ["NDEBUG", "API_V=3"],             ; -D prepended automatically
    includes: ["vendor/include"],                 ; -I prepended automatically
    flags:    ["-fvisibility=hidden"],            ; raw flags passed through
    opt:      "speed"                             ; optimization level
  }
}
```

Optimization levels:

| `opt` | GCC/Clang | MSVC |
|-------|-----------|------|
| `none` | `-O0` | `/Od` |
| `debug` | `-Og` | `/Od` |
| `size` | `-Os` | `/O1` |
| `speed` | `-O2` | `/O2` |
| `lto` | `-O2 -flto` | `/O2 /GL` |

### Link

```pasta
{
  link: {
    flags:   ["-pthread"],                ; raw linker flags
    libs:    ["m", "dl", "pthread"],      ; -l prepended automatically
    libdirs: ["/opt/local/lib"],          ; -L prepended automatically
    script:  "link/memory.ld"             ; linker script path
  }
}
```

### MSVC Support

`now` auto-detects MSVC when `CC=cl.exe` or when running in a Visual Studio Developer Command Prompt. Flags are translated automatically:

| now concept | GCC/Clang | MSVC |
|-------------|-----------|------|
| Warning `Wall` | `-Wall` | `/W4` |
| Define `FOO` | `-DFOO` | `/DFOO` |
| Optimize speed | `-O2` | `/O2` |
| Shared lib | `-shared` | `/LD` |
| Object ext | `.o` | `.obj` |
| Static lib tool | `ar` | `lib.exe` |
| Link tool | `cc/c++` | `link.exe` |

---

## 5. Dependencies

### Declaring Dependencies

```pasta
{
  deps: [
    { id: "org.acme:core:^1.5.0",    scope: "compile" },
    { id: "zlib:zlib:^1.3",          scope: "compile" },
    { id: "unity:unity:2.5.2",       scope: "test" },
    { id: "org.acme:proto:^2.0",     scope: "provided" }
  ]
}
```

`depends:` is accepted as a Maven-friendly alias for `deps:`; when both are present, `deps:` wins. Use whichever your team finds clearer.

Entries accept either the compact `id:` shorthand or the long-form `{group, artifact, version}` shape — both produce the same coordinate:

```pasta
{
  depends: [
    { id: "org.acme:core:^1.5.0" },                          ; shorthand
    { group: "org.acme", artifact: "proto", version: "*" }   ; long form
  ]
}
```

### Scopes

| Scope | Compile | Link | Ship | Test |
|-------|---------|------|------|------|
| `compile` | yes | yes | yes | yes |
| `test` | no | no | no | yes |
| `provided` | yes | no | no | no |
| `runtime` | no | yes | yes | no |

### Repositories

```pasta
{
  repos: [
    "https://registry.now.build/central",
    { url: "https://pkg.acme.org/now", id: "acme", auth: "token" }
  ]
}
```

The local repo (`~/.now/repo/`) is always checked first.

### Convergence Policy

When multiple deps require different versions of the same artifact:

```pasta
{
  convergence: "lowest"    ; lowest (default) | highest | exact
}
```

### Overrides

Force a specific version regardless of other constraints:

```pasta
{
  deps: [
    { id: "zlib:zlib:1.3.1", override: true }
  ]
}
```

### Lock File

After resolution, `now` writes `now.lock.pasta` with exact resolved versions, SHA-256 hashes, and dependency graph. Commit this file for reproducible builds.

Use `--locked` to fail if the lock file is inconsistent:

```sh
now build --locked
```

---

## 6. Version Ranges

| Syntax | Meaning | Example |
|--------|---------|---------|
| `1.2.3` | Exact version | Only 1.2.3 |
| `^1.2.3` | Caret (compatible) | `>=1.2.3 <2.0.0` |
| `^0.9.3` | Caret (pre-1.0) | `>=0.9.3 <0.10.0` |
| `~1.2.3` | Tilde (patch-level) | `>=1.2.3 <1.3.0` |
| `>=1.2.0` | Floor | Any version >= 1.2.0 |
| `>=1.2.0 <2.0.0` | Compound | Explicit range |
| `*` | Any | Any version |

---

## 7. Testing

### Directory Structure

Place test sources in `src/test/c/` (or `src/test/cpp/` for C++). Test sources are compiled with the production include paths and linked with the production object files.

### Running Tests

```sh
now test            # build + run tests
now test -v         # verbose output
now test -j4        # parallel compilation
```

The test binary exit code determines pass/fail (0 = pass).

### Test Framework

`now` doesn't impose a test framework. Your test source provides its own `main()`. Popular choices:

- Roll your own (simple `assert` macros)
- [Unity](https://github.com/ThrowTheSwitch/Unity)
- [Check](https://libcheck.github.io/check/)
- [cmocka](https://cmocka.org/)

When the project is an `output.type: "executable"`, the production entry-point TU (`src/main/c/main.c`, or `.cpp/.cc/.cxx`) is automatically filtered out of the test link so the test source's `main()` doesn't collide with the project's. No configuration needed.

### Test Modes

```pasta
{
  tests: {
    dir:  "src/test/c",
    mode: "single"   ; "single" (default) | "each"
  }
}
```

- `mode: "single"` — every test source compiles and links into one binary at `target/bin/<artifact>-test.exe`. A single `main()` calls test cases.
- `mode: "each"` — each test source links into its own binary under `target/test/bin/<source-name>.exe`. Each file has its own `main()`; `now test` runs them all in turn. Mirrors CTest's per-test executable shape — useful when migrating projects that already have one-test-per-file conventions.

### Test Fixtures

When tests need to find data files at runtime, two mechanisms cover the common cases:

```pasta
{
  tests: {
    defines: [ "FIXTURES=\"src/test/resources\"" ],
    env:     [ "MYAPP_DATA_DIR=src/test/resources" ]
  }
}
```

- `tests.defines` — extra `-D` macros injected at test compile time. Use for paths baked in as C string literals.
- `tests.env` — `KEY=VAL` pairs set in the test binary's environment at launch.
- The test binary is run with `cwd` set to the module root (not `target/bin/`), so relative resource paths resolve against your source tree.

### Incremental Test Re-Runs

`now test` skips both compile and link when nothing has changed. Test compile uses an mtime check against each source's `.o`; test link compares mtime against every input object. No-op `now test` runs in milliseconds; the test binary is invoked only if its inputs (or its own outputs) are absent or stale.

---

## 8. Parallel Builds

```sh
now build           # auto-detect CPU count
now build -j8       # 8 parallel jobs
now build -j1       # sequential (useful for debugging)
```

Output from parallel compilations is buffered per-file — you'll never see interleaved compiler output.

---

## 9. Packaging and Publishing

### Package

```sh
now package
```

Creates `target/pkg/{artifact}-{version}-{triple}.tar.gz` with a `.sha256` sidecar.

### Install Locally

```sh
now install
```

Installs to `~/.now/repo/{group-path}/{artifact}/{version}/` so other local projects can depend on it.

### Publish to Registry

```sh
now publish --repo https://registry.now.build/central
```

Credentials are loaded from `~/.now/credentials.pasta`:

```pasta
{
  credentials: [
    { url: "https://registry.now.build", token: "your-token-here" }
  ]
}
```

---

## 10. Workspaces

A workspace is a multi-module project where sibling modules can depend on each other.

### Workspace Root

```pasta
{
  group:    "org.acme",
  version:  "3.0.0",
  modules:  ["core", "net", "tls", "cli"]
}
```

Each module directory contains its own `now.pasta`.

### Build Order

`now` builds a dependency graph from inter-module dependencies and uses Kahn's topological sort to determine build order. Modules at the same level in the graph are built in parallel waves.

```sh
now build           # builds all modules in dependency order
now build -v        # shows wave-by-wave progress
```

### Cycle Detection

Circular module dependencies are detected and reported before building begins.

### Sibling Auto-Injection

When a module's `depends:` (or `deps:`) entry resolves to a workspace sibling, `now` auto-injects everything the consumer needs to compile and link without re-declaring per-consumer:

| Auto-injected | From sibling |
|---|---|
| `compile.includes` | `<sibling>/src/main/h` |
| `link.libdirs` | `<sibling>/target/bin` |
| `link.libs` | sibling's `output.name` (or `artifact` if name unset) |
| `compile.defines` | `<UPPER>_STATIC` (only when sibling is `output.type: "static"`) |

A consumer two hops down the DAG also inherits its intermediate siblings' `link.libs` and `link.libdirs` transitively, so a host executable only needs to list its *direct* sibling deps:

```pasta
; hosts/cli/now.pasta — depends on parser (which depends on core)
{
  artifact: "cli",
  output:   { type: "executable", name: "mytool" },
  depends:  [{ id: "org.acme:parser:*" }]
  ; core's libdir + name flow in transitively via parser
}
```

Pushes are deduped, so listing the same dep twice (e.g. for clarity) is harmless. Workspace authors don't need to declare modules in topological order — inject runs N passes over N modules until convergence.

`output.type: "executable"` siblings are skipped (not linkable), and `output.type: "header-only"` siblings contribute only the include path.

### Shared-Library Runtime Co-Location (Windows)

When a workspace executable links against a sibling built with `output.type: "shared"` (or against a procured shared dep), `now` automatically copies the producer's `.dll` next to the consumer's `.exe` in `target/bin/`. This is Windows-only — POSIX bakes `-Wl,-rpath` at link time, so the binary finds its libraries without staging.

### OS-Conditional Sub-Blocks

`compile:` and `link:` accept nested OS-keyed sub-blocks. When the host triple matches the key, that block's arrays are appended to the parent. Non-matching keys are ignored.

```pasta
{
  link: {
    flags: ["-pthread"],
    windows: { libs: ["ws2_32", "bcrypt", "winmm"] },
    posix:   { libs: ["m"] },
    linux:   { libs: ["rt"] },
    macos:   { flags: ["-framework", "CoreFoundation"] }
  }
}
```

Recognised keys: `windows`, `linux`, `macos`, `freebsd`, `openbsd`, `netbsd` (specific OSes); `posix`, `unix` (group aliases — anything-not-windows). Use these for OS-specific link libraries, compiler flags, and defines without shell-detect ceremony in your build script.

---

## 11. Plugins

### Built-in Plugins

| Plugin | Phase | Description |
|--------|-------|-------------|
| `now:version` | generate | Generates `now_version_generated.h` with version macros |
| `now:embed` | generate | Embeds file contents as C byte arrays |

### Using Plugins

```pasta
{
  plugins: [
    { id: "now:version", hooks: ["generate"] }
  ]
}
```

### External Plugins

External plugins are executables that communicate via IPC:

```pasta
{
  plugins: [
    { id: "org.proto:protoc-now:3.21.0", hooks: ["generate"],
      run: "protoc-now --proto_path=src/proto" }
  ]
}
```

---

## 12. Multi-Architecture Builds

### Platform Triples

`now` uses `os:arch:variant` triples (e.g., `linux:x86_64:gnu`, `windows:x86_64:msvc`).

```sh
now build --target linux:arm64:gnu    # cross-compile
now build --target windows:x86_64    # variant auto-detected
```

### Host Detection

`now` auto-detects the host platform at runtime. Use `now build -v` to see the detected triple.

---

## 13. Reproducible Builds

Enable deterministic builds:

```pasta
{
  reproducible: true
}
```

This enables all reproducibility measures with sensible defaults:
- Timebase: `zero` (epoch timestamp for embedded dates)
- Path prefix maps: strip absolute paths from debug info
- Sorted inputs: deterministic file ordering
- Date macro neutralization: override `__DATE__` / `__TIME__`
- Strip metadata: deterministic build IDs

### Fine-Grained Control

```pasta
{
  reproducible: {
    timebase:         "git-commit",    ; now | git-commit | zero | ISO 8601 literal
    path_prefix_map:  true,
    sort_inputs:      true,
    no_date_macros:   true,
    strip_metadata:   true,
    verify:           true             ; post-build verification pass
  }
}
```

### Verification

```sh
now reproducible:check     # builds twice, compares output hashes
```

---

## 14. Security

### Signing and Trust

Configure signature verification for dependencies:

```pasta
{
  trust: {
    require_signatures: true,    ; reject unsigned packages
    require_known_keys: true     ; reject packages from unknown publishers
  }
}
```

Manage trusted keys:

```sh
now trust:list                       # list trusted keys
now trust:add "org.acme" "RWT..." "Acme Corp signing key"
now verify archive.tar.gz archive.tar.gz.sig
```

### Dependency Confusion Protection

Prevent private packages from being shadowed by public registry packages:

```pasta
{
  private_groups: ["com.mycompany", "com.mycompany.internal"]
}
```

### Advisory Guards

`now` checks dependencies against a security advisory database:

```sh
now advisory:check      # check deps against advisory DB
```

Advisory severity levels and default behavior:

| Severity | Default | Overridable? |
|----------|---------|-------------|
| `blacklisted` | Hard error | No |
| `critical` | Hard error | Yes, with justification |
| `high` | Hard error | Yes, with justification |
| `medium` | Warning | Yes, escalate to error |
| `low` | Warning | Yes, escalate to error |
| `info` | Silent | Yes, escalate to warning |

Override known advisories with justification and mandatory expiry:

```pasta
{
  advisories: {
    allow: [
      {
        advisory:    "NOW-SA-2026-0042",
        dep:         "zlib:zlib:1.3.0",
        reason:      "inflate() not called in our usage path",
        expires:     "2026-06-01",
        approved_by: "alice@acme.org"
      }
    ]
  }
}
```

---

## 15. Configuration Layers

`now` supports cascading configuration from multiple sources:

1. **Baseline** — built-in defaults
2. **Enterprise layer** — `~/.now/layers/*.pasta` (organization-wide policies)
3. **Project layer** — your `now.pasta`

Layers can lock sections to enforce policy:

```pasta
; ~/.now/layers/enterprise.pasta
{
  layer: "enterprise",
  sections: [
    { name: "compile", policy: "open" },
    { name: "toolchain", policy: "locked", description: "Standardized toolchain" }
  ]
}
```

Inspect the layer stack:

```sh
now layers:show            # display all layers and effective config
now layers:show --effective  # show merged configuration
now layers:audit           # report policy violations
```

---

## 16. Exporting to Other Build Systems

`now` provides zero-lock-in escape hatches:

```sh
now export:cmake      # generates CMakeLists.txt
now export:make       # generates Makefile
```

The generated files are standalone and fully functional. They include:
- Source discovery
- Compile and link settings
- Test targets
- Install rules
- Dependencies listed as comments (with hints for manual resolution)

---

## 17. CLI Reference

### Phases

| Command | Description |
|---------|-------------|
| `now build` | Procure deps, generate, compile, link |
| `now compile` | Procure deps, generate, compile only |
| `now generate` | Run generate-phase plugins only |
| `now link` | Link only (no compile) |
| `now test` | Full build + run tests |
| `now procure` | Resolve and download dependencies |
| `now package` | Create distributable archive |
| `now install` | Install to local repo |
| `now publish` | Upload to registry |

### Tools

| Command | Description |
|---------|-------------|
| `now export:cmake` | Generate CMakeLists.txt |
| `now export:make` | Generate Makefile |
| `now trust:list` | List trusted keys |
| `now trust:add <scope> <key> [comment]` | Add trusted key |
| `now verify <archive> <sig>` | Verify archive signature |
| `now advisory:check` | Check deps against advisory DB |
| `now reproducible:check` | Build twice and compare hashes |
| `now layers:show` | Show layer stack |
| `now layers:audit` | Report policy violations |
| `now ci` | CI mode (structured output) |
| `now clean` | Delete target/ |
| `now version` | Print version |

### Options

| Flag | Description |
|------|-------------|
| `-v`, `--verbose` | Verbose output |
| `-j N` | Parallel jobs (0 = auto) |
| `--repo URL` | Registry URL for publish |
| `--output FMT` | Output format: text, json, pasta |
| `--locked` | Fail if lock file inconsistent |
| `--offline` | No network access |
| `--target TRIPLE` | Target platform triple |
| `--no-color` | Disable ANSI colors |
| `-h`, `--help` | Show help |

# Building with `now` on GitHub Actions

Step-by-step guide for adding `now` to a GitHub Actions CI pipeline.

---

## TL;DR

If your repo has a `now.pasta` in the root and follows the convention layout (`src/main/c/`, `src/main/h/`), the workflow is three lines:

```yaml
- uses: actions/checkout@v4
  with: { submodules: recursive }
- uses: IridiumFX/now-action@v1
  with: { command: ci }
```

`now ci` runs build + test and reports results. That's the whole pipeline for a well-structured project.

---

## Language support

Be aware of what's actually battle-tested before picking a language:

| Language | Status | Validated on |
|----------|--------|--------------|
| **C (C11)** | Production | self-build (77 files), cookbook (62), apennines (286), gut (47) |
| **C++ (C++17/20)** | Works | Distinct Maven layout: `src/main/cpp/` + `src/main/hpp/`; C++20 modules scanner tested; smaller real-world footprint than C |
| **C + Rust (FFI)** | Works | rustc invoked as `--emit obj --crate-type staticlib`; verified end-to-end |
| **Go (cgo)** | Experimental | `go build -buildmode=c-archive` wired; untested against real Go projects |
| **Java** | Experimental | Dedicated `javac` + `java` path; test-project scaffold exists; not exercised against a real project |
| **asm-gas / asm-nasm** | Experimental | Registered, routes through C toolchain's `${as}`; untested on real asm-heavy projects |
| **Julia** | Not implemented | Registered in the language list but no compile path; **do not use in CI** yet |

**Rule of thumb**: if your project is C, treat `now` as production-ready. C++ follows the Maven convention with its own distinct dirs — `src/main/cpp/` for `.cpp` and `src/main/hpp/` for `.hpp` headers (C++ is not C; sharing `h/c` roots muddles that). Java → `src/main/java/`, Rust → `src/main/rust/`, Go → `src/main/go/`. For Rust FFI (C host + Rust `#[no_mangle]` helpers) drop `.rs` files alongside `.c` files in `src/main/c/`. Anything else — prototype locally before wiring up CI.

---

## Initial setup (one-time, already done)

The `IridiumFX/now-action@v1` action and `now` release binaries are already published. If you're bootstrapping a fresh `now` fork, the steps are:

1. Push `now-action` repo, tag `v1`.
2. Push `now` repo, tag `v1.0.0` — triggers `.github/workflows/release.yml` which uploads `now-{linux-x64, macos-arm64, windows-x64.exe, freebsd-x64}` assets.
3. Projects reference the action as `uses: IridiumFX/now-action@v1`; it downloads the matching binary on the runner.

Skip this section unless you're maintaining the action itself.

---

## Per-project workflow template

Starter CI file for a C/C++ project. Place at `.github/workflows/ci.yml`:

```yaml
name: CI
on:
  push:
    branches: [main]
  pull_request:
    branches: [main]

jobs:
  build:
    strategy:
      fail-fast: false
      matrix:
        os: [ubuntu-latest, macos-latest, windows-latest]
    runs-on: ${{ matrix.os }}

    steps:
      - uses: actions/checkout@v4
        with:
          submodules: recursive

      - name: Build and test
        uses: IridiumFX/now-action@v1
        with:
          command: ci

      - name: Upload binary
        if: always()
        uses: actions/upload-artifact@v4
        with:
          name: ${{ github.event.repository.name }}-${{ matrix.os }}
          path: target/bin/
```

Drop-in works for any project that compiles cleanly with `now build` locally.

---

## Writing your `now.pasta`

Three patterns cover ~95% of repos.

### Pattern 1 — Pure convention layout (simplest)

```
your-project/
  now.pasta
  src/main/c/          <- sources (.c, .cpp)
  src/main/h/          <- public headers
  src/main/h/internal/ <- internal headers
  src/test/c/          <- test sources
```

```pasta
{
  group:    "dev.example",
  artifact: "myproject",
  version:  "1.0.0",
  lang:     "c",
  compile:  { std: "c11", warnings: ["Wall", "Wextra"] },
  link:     { output: "executable", libs: ["m", "pthread"] }
}
```

No file list needed — `now` walks `src/main/c/` recursively.

### Pattern 2 — With a flat vendored dependency (cookbook/gut style)

When you vendor a library with non-Maven layout (e.g. `lib/apennines/c/*.c`):

```pasta
{
  group:    "dev.example",
  artifact: "myproject",
  version:  "1.0.0",
  lang:     "c",
  sources: {
    dir:     "src/main/c",
    headers: "src/main/h",
    include: [
      "lib/apennines/c/addr.c",
      "lib/apennines/c/buf.c",
      "lib/apennines/c/http_client.c"
      /* ... list each vendored .c file ... */
    ]
  },
  compile: {
    std: "c11",
    warnings: ["Wall", "Wextra"],
    includes: ["lib/apennines/h"]
  },
  link: { libs: ["ws2_32", "bcrypt"] }
}
```

`sources.include` accepts explicit file paths relative to the project root. Use this when the vendor tree is flat (no `src/main/c` inside).

### Pattern 3 — Sibling components (now's own layout)

When your own project has logical subsystems:

```
your-project/
  now.pasta
  src/main/c/             <- core
  components/
    enterprise/src/main/c/
    export/src/main/c/
    cli/src/main/c/
```

```pasta
{
  ...
  components: [
    "components/enterprise",
    "components/export",
    "components/cli"
  ]
}
```

Each component follows convention layout. No leaf `now.pasta` needed if the component is just sources.

### Starting from CMake

If you have a CMakeLists.txt, run:

```bash
now import:cmake CMakeLists.txt now.pasta
```

Read-only — generates a starter descriptor you then curate. See `docs/migration-guide.md` for what the importer does and doesn't handle.

---

## Real-world examples

### Cookbook (artifact registry, ~60 C files)

```yaml
jobs:
  build:
    strategy:
      matrix:
        os: [ubuntu-latest, macos-latest, windows-latest]
    runs-on: ${{ matrix.os }}
    steps:
      - uses: actions/checkout@v4
        with: { submodules: recursive }
      - uses: IridiumFX/now-action@v1
        with: { command: ci }
```

Cookbook's `now.pasta` uses `sources.include` to pull in vendored apennines (pattern 2). After its libsodium→apennines-crypto migration, no pre-built archives are needed. CI is three lines on three OSes.

### Apennines (Nova OS base runtime, 286 C files)

Zero external dependencies. Cleanest possible pasta (~37 lines, pattern 1). Works unmodified on Linux/macOS/Windows. Cold build ~18s on 4 cores — 6.6× faster than CMake+Ninja on the same tree.

### Gut (git reimplementation, 15 gut + 32 vendored apennines = 47 files)

Uses `sources.include` to pull 32 apennines files from a flat `lib/apennines/c/`. Windows link libs (`ws2_32`, `bcrypt`) declared explicitly. Cold build ~16s, within ninja's run-to-run noise band. Warm-ccache rebuilds come in under 1s.

All three projects ship with the same three-line workflow.

---

## Useful `now` commands in CI

| Command | What it does |
|---------|--------------|
| `now build` | compile + link |
| `now test` | build + run tests (`src/test/c/`) |
| `now ci` | build + test + structured exit codes (use this in workflows) |
| `now clean` | wipe `target/` (preserves `~/.now/dirwalk/` and `~/.now/cache/`) |
| `now cache:stats` | show local ccache hit rate / size |
| `now cache:remote-stats` | ping remote object cache (if configured) |
| `now sbom` | emit CycloneDX JSON SBOM for supply-chain tracking |
| `now import:cmake` | one-shot CMake→now.pasta converter |
| `now verify` | signature + trust policy check on pulled dependencies |

`now ci` is the right default for GitHub Actions — it emits JSON/Pasta/text formats (`--output <fmt>`) for downstream parsing.

### Test layouts: one binary vs. one-per-file

`now test` supports two link modes, set in `now.pasta`:

```pasta
tests: {
  dir:  "src/test/c",
  mode: "single"   /* default — all .c link into one binary with one main() */
}
```

```pasta
tests: {
  dir:  "src/test/c",
  mode: "each"     /* each .c is its own binary with its own main() */
}
```

**`single`** (the default) suits in-house test frameworks that aggregate cases under one driver — small to medium projects, `now`/cookbook/apennines/gut all use this.

**`each`** suits codebases migrated from CMake/CTest or any framework where each `test_*.c` is an independent executable with its own `main()`. Binaries land under `target/test/bin/<name>[.exe]`; `now test` runs them in sequence and reports `N passed, M failed`. Exit non-zero if any test fails.

---

## Performance expectations on GitHub runners

GitHub's default runners are 4-core. Measured on comparable hardware:

| Project | Files | Cold build | Warm ccache | No-op |
|---------|-------|-----------|-------------|-------|
| gut | 47 | ~16s | ~0.7s | ~0.1s |
| cookbook | 62 | ~30s | — | — |
| now (self) | 77 | ~24s | ~1.2s | ~0.1s |
| apennines | 286 | ~18s | — | — |

Two things make the steady-state fast:
- **ccache**: objects cached at `~/.now/cache/objects/` survive across jobs if you cache that dir (GitHub `actions/cache@v4` on `~/.now/cache`).
- **Dirwalk cache**: structure survives `now clean` (stored under `~/.now/dirwalk/`), so "clean rebuild" doesn't re-walk the tree.

Both are automatic — no configuration needed beyond caching `~/.now/` between runs.

### Caching `~/.now/` on GitHub Actions

```yaml
      - uses: actions/cache@v4
        with:
          path: ~/.now
          key: now-${{ matrix.os }}-${{ hashFiles('now.pasta') }}
          restore-keys: |
            now-${{ matrix.os }}-

      - uses: IridiumFX/now-action@v1
        with: { command: ci }
```

First run after a `now.pasta` change: cold. Every subsequent run: warm ccache → near-instant.

---

## Updating `now` or `now-action`

**New `now` release:**

```bash
cd now
git tag v1.0.1 && git push origin v1.0.1
```

The release workflow builds and uploads new binaries. Workflows using `uses: IridiumFX/now-action@v1` automatically pick up the latest release (action points at the `v1` tag, which tracks the latest `v1.x.y` release).

**Action logic change:**

```bash
cd now-action
# edit action.yml
git commit -am "Update action"
git tag -f v1 && git push origin v1 --force
```

All consumers get it on their next run.

---

## Troubleshooting

**"cannot scan source directory: src/main/X"** — your sources live elsewhere than the convention. Default directory depends on the primary language:

| Lang | `sources.dir` | `sources.headers` | `sources.private` | `tests.dir` |
|------|---------------|-------------------|-------------------|-------------|
| c    | `src/main/c`    | `src/main/h`   | `src/main/h/internal`   | `src/test/c`    |
| c++  | `src/main/cpp`  | `src/main/hpp` | `src/main/hpp/internal` | `src/test/cpp`  |
| java | `src/main/java` | (none)         | (none)                  | `src/test/java` |
| rust | `src/main/rust` | (none)         | (none)                  | `src/test/rust` |
| go   | `src/main/go`   | (none)         | (none)                  | `src/test/go`   |

Override any of these with `sources: { dir: "...", headers: "...", private: "..." }` or `tests: { dir: "..." }`.

**"javac not found (set JAVAC env var)"** — Java support needs `javac` in PATH. The `setup-java` GitHub action installs one. Treat Java support as experimental for now.

**Rust can't find `rustc`** — add `- uses: dtolnay/rust-toolchain@stable` before the `now-action` step. `rustc` must be in PATH at build time.

**Linker errors for platform-specific libs** — our autodetection doesn't cover everything. Declare them explicitly in `link.libs`. Common Windows additions: `ws2_32`, `bcrypt`, `advapi32`, `userenv`.

**ccache misses on CI but hits locally** — make sure `actions/cache@v4` restores `~/.now/` before the `now-action` step and saves it after. GitHub auto-saves only on a successful job.

---

## Summary

- One `now.pasta` at the repo root, one workflow file.
- `now ci` is the right default command.
- Cache `~/.now/` between runs for ~20× warm-build speedup.
- C is production. Rust (FFI) works. C++ works with a tweak. Go/Java/asm are experimental; Julia is not yet implemented.

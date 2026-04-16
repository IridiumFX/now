# Migrating to now

How to convert an existing C/C++ project from CMake/Make/Meson to `now`.

---

## Quick Start (under 5 minutes)

```bash
cd your-project
now init          # auto-detects languages, generates now.pasta
now build         # compile + link
```

If your project follows a standard layout (`src/`, `include/`, `lib/`), `now init` handles everything. If not, read on.

---

## The Convention

`now` expects the Maven-style layout:

```
your-project/
  now.pasta              ← project descriptor
  src/main/c/            ← sources (.c, .cpp, .rs, .go)
  src/main/h/            ← public headers
  src/main/h/internal/   ← private headers
  src/test/c/            ← test sources
```

**That's the only rule.** If your files are in these directories, `now` finds them automatically. No file listing needed.

---

## Step 1: Create now.pasta

Minimal descriptor:

```
{
  group:    "com.example",
  artifact: "myproject",
  version:  "1.0.0",
  lang:     "c",
  compile:  { warnings: ["Wall", "Wextra"] },
  link:     { libs: ["pthread", "m"] }
}
```

That's it for a simple project. `now` discovers sources in `src/main/c/`, headers in `src/main/h/`, and links with the listed libraries.

---

## Step 2: Organize Components

If your project has logical subsystems, organize them as components:

```
your-project/
  now.pasta
  src/main/c/              ← core
  components/
    networking/
      src/main/c/          ← networking code
      src/main/h/internal/ ← networking headers
    database/
      src/main/c/
      src/main/h/internal/
    ui/
      src/main/c/
```

In `now.pasta`:

```
components: [
  "components/networking",
  "components/database",
  "components/ui"
]
```

**No `now.pasta` needed in the components** — the directory structure is the module structure. Each component with a `src/main/c/` directory is automatically discovered as a leaf module. Sub-components (components within components) are auto-discovered too.

---

## Step 3: Handle Vendored Dependencies

External libraries go under `lib/`:

```
your-project/
  lib/
    sqlite/
      src/main/c/sqlite3.c
      src/main/h/sqlite3.h
    zlib/
      src/main/c/
      src/main/h/
```

In `now.pasta`:

```
vendored: [
  "lib/sqlite",
  "lib/zlib"
]
```

**Components vs vendored:**
- `components:` — your code, full control (`now fmt`, `now test` apply)
- `vendored:` — external code, read-only (discovered but not modified)

---

## Step 4: Pre-built Libraries

For complex vendored deps that can't be compiled from source (e.g., libsodium with platform-specific ASM):

```
link: {
  archives: ["lib/libsodium/libsodium.a"],
  libs: ["ws2_32", "bcrypt"]
}
```

---

## Step 5: Exclude Files

To skip specific files from compilation:

```
sources: {
  dir: "src/main/c",
  exclude: ["experimental.c", "deprecated_module.c"]
}
```

Matches by filename or relative path.

---

## Safety Rules

**Importers are read-only.** `now import:cmake` and `now import:maven` ONLY generate a `now.pasta` file. They never move, rename, or delete any existing files. Your project remains untouched until you decide to restructure.

### The safe migration workflow

```
1. git commit                    # clean working tree first
2. now import:cmake              # generates now.pasta (nothing else)
3. cat now.pasta                 # review what was generated
4. now build                     # try building — expect errors
5. edit now.pasta                # fix what the importer missed
6. now build                     # iterate until clean
7. git diff                      # only now.pasta was added
8. git commit -m "Add now.pasta" # commit when satisfied
```

**Do NOT reorganize your directory structure until the build works.** The importers generate a descriptor for your current layout. If you move files first, both CMake and `now` will break and you'll have nothing that compiles.

### Restructuring (optional, do it later)

Once `now build` works with your current layout:

```
1. git commit                    # clean state with working now build
2. mkdir -p src/main/c src/main/h
3. mv *.c src/main/c/           # move sources
4. mv *.h src/main/h/           # move headers
5. edit now.pasta                # remove explicit paths (convention takes over)
6. now build                     # verify
7. git commit                    # commit the restructure
```

Each step is independently revertable with `git checkout .`.

---

## Importer Limitations

### `now import:cmake`

**What it handles:**
- `project(NAME VERSION X.Y.Z LANGUAGES C CXX)` → artifact, version, lang
- `add_library(name STATIC/SHARED ...)` → output type
- `add_executable(name ...)` → output type
- `target_link_libraries(... lib1 lib2)` → link.libs
- `target_compile_definitions(... DEF1 DEF2)` → compile.defines
- `target_include_directories(... dir1 dir2)` → compile.includes
- `add_subdirectory(lib/foo)` → vendored

**What it CANNOT handle:**
- **CMake variables** (`${VAR}`) — skipped silently. If your defines, paths, or libs use variables, they'll be missing from the output.
- **Generator expressions** (`$<TARGET_FILE:...>`) — skipped entirely.
- **Conditional logic** (`if/else/endif`) — the importer sees ALL commands regardless of conditions. Platform-specific libs may all appear.
- **find_package** — no equivalent. You must manually add the library paths and names.
- **Multiple targets** — only extracts the first `add_library` or `add_executable`. Multi-target CMake projects need one `now.pasta` per target (use workspace or components).
- **Custom commands/targets** — `add_custom_command`, `add_custom_target` have no equivalent. These are typically code generators — use `now` plugins instead.
- **Toolchain files** — cross-compilation settings are not imported. Use `--target` flag or profiles.
- **FetchContent / ExternalProject** — not imported. Download deps manually and use `vendored:`.
- **Nested CMakeLists.txt** — only reads the root file. Subdirectory CMakeLists are not parsed.

**Always review the generated `now.pasta` before building.** The importer gives you a starting point, not a finished product.

### `now import:maven`

**What it handles:**
- `groupId`, `artifactId`, `version` → group, artifact, version
- `<dependencies>` with scope → deps array
- `<properties>` → variable substitution in coordinates
- `<build><plugins><plugin>` (maven-compiler-plugin source/target)

**What it CANNOT handle:**
- **Parent POM inheritance** — only reads the current pom.xml
- **Maven profiles** — conditional config not imported
- **Plugin execution** — only compiler plugin source/target extracted
- **Multi-module reactor** — reads single POM, not the parent/child tree
- **Repository declarations** — not imported, use `repos:` field manually

### General importer advice

1. **Start with a clean git tree** — you can always `git checkout .` to undo
2. **Build with the old system first** — confirm CMake/Maven works before importing
3. **Import generates, you curate** — the importer is 80% accurate, you fix the last 20%
4. **Keep the old build system** — don't delete CMakeLists.txt until `now build` is fully working
5. **Test incrementally** — get compilation working first, then linking, then tests
6. **Platform-specific libs** — the importer may include libs from all platforms; remove the ones that don't apply

---

## Migrating from CMake

### What maps where

| CMake | now.pasta |
|-------|-----------|
| `project(NAME VERSION X.Y.Z)` | `artifact: "name", version: "X.Y.Z"` |
| `add_library(... STATIC/SHARED)` | `link: { output: "static" }` or `"shared"` |
| `add_executable(...)` | `link: { output: "executable" }` |
| `target_sources(... file1.c file2.c)` | Not needed — convention discovers files |
| `target_include_directories(...)` | `compile: { includes: [...] }` or convention |
| `target_compile_definitions(...)` | `compile: { defines: [...] }` |
| `target_link_libraries(... lib1 lib2)` | `link: { libs: ["lib1", "lib2"] }` |
| `add_subdirectory(lib/foo)` | `vendored: ["lib/foo"]` |
| `find_package(...)` | `link: { libs: [...] }` (manual) |
| `file(GLOB_RECURSE ...)` | Not needed — convention |
| `set(CMAKE_C_STANDARD 11)` | `std: "c11"` |

### What you can delete (AFTER `now build` works)

**Do not delete anything until `now build` produces a working binary.**

Once verified:
- `CMakeLists.txt` (all of them)
- `cmake/` directory
- `.cmake` module files
- Build presets / toolchain files

### What stays

- Source files (move to convention layout if needed, or keep as-is)
- Test files (move to `src/test/c/` or keep in place with `tests.dir`)
- Vendored libraries (move to `lib/` or reference with `compile.includes`)

---

## Migrating from Make

Replace your `Makefile` with `now.pasta`. The key difference: Make requires you to list every source file and write compilation rules. `now` discovers files by convention.

A typical `Makefile` like:
```makefile
CC = gcc
CFLAGS = -Wall -Wextra -std=c11
SOURCES = src/main.c src/parser.c src/lexer.c
OBJECTS = $(SOURCES:.c=.o)
TARGET = myapp

$(TARGET): $(OBJECTS)
	$(CC) -o $@ $^ -lm -lpthread

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJECTS) $(TARGET)
```

Becomes:
```
{
  artifact: "myapp",
  lang: "c",
  std: "c11",
  compile: { warnings: ["Wall", "Wextra"] },
  link: { output: "executable", libs: ["m", "pthread"] }
}
```

---

## Multi-language Projects

`now` auto-detects languages. For mixed C/Rust:

```
{
  artifact: "myproject",
  langs: ["c", "rust"],
  std: "c11"
}
```

Place `.rs` files alongside `.c` files in `src/main/c/`. Rust files compile with `rustc --crate-type staticlib`, C files with `gcc`. Both link together with auto-injected runtime deps.

Supported: C, C++, C++20 modules, asm-gas, asm-nasm, Java, Rust, Go, Julia.

---

## Performance Expectations

| Project size | Cold build | No-op | Incremental |
|-------------|-----------|-------|-------------|
| Small (< 20 files) | < 5s | 0s | < 1s |
| Medium (20-100 files) | 5-15s | 0s | 1-2s |
| Large (100-300 files) | 15-30s | 0s | 1-2s |

Benchmarked projects:
- **now** (46 files, 20 components): 6s cold, 2.7x faster than CMake+Ninja
- **cookbook** (62 files, vendored deps): 30s cold, 3x faster than CMake+Ninja
- **apennines** (286 files): 18s cold, 6.6x faster than CMake+Ninja

---

## CI Integration

```yaml
# .github/workflows/build.yml
- uses: IridiumFX/now-action@v1
  with:
    command: ci
```

One line. Builds, tests, reports. Cross-platform (Linux, macOS, Windows, FreeBSD).

---

## Summary

| Before | After |
|--------|-------|
| CMakeLists.txt (100+ lines) | now.pasta (10-40 lines) |
| File lists | Convention |
| Build presets | Profiles |
| Toolchain files | Auto-detection |
| Package scripts | `now package` |
| CI complexity | `uses: IridiumFX/now-action@v1` |

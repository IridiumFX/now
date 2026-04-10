# Building with `now` on GitHub Actions

Step-by-step guide to replace CMake/Ninja CI with `now` for all IridiumFX projects.

---

## Part 1 — Set Up the now-action Repository

This is done once. It creates the GitHub Action that all projects use.

### 1.1 Create the repository

```bash
cd C:\Users\Iridium\Projects\now-action
git remote add origin https://github.com/IridiumFX/now-action.git
git push -u origin master
```

### 1.2 Tag the release

```bash
git tag v1
git push origin v1
```

The `v1` tag is what projects reference in their workflows (`uses: IridiumFX/now-action@v1`).

---

## Part 2 — Publish now Binaries

The action downloads a pre-built `now` binary. We need to create a GitHub release with binaries for each platform.

### 2.1 Push the now repository (if not already on GitHub)

```bash
cd C:\Users\Iridium\Projects\now
git remote add origin https://github.com/IridiumFX/now.git
git push -u origin main
```

### 2.2 Tag and push to trigger the release workflow

```bash
git tag v1.0.0
git push origin v1.0.0
```

This triggers `.github/workflows/release.yml` which:
- Builds static `now` binaries on Linux (gcc), macOS (clang), Windows (gcc), FreeBSD
- Runs the test suite on each platform
- Uploads `now-linux-x64`, `now-macos-arm64`, `now-windows-x64.exe`, `now-freebsd-x64` as release assets

### 2.3 Verify the release

Go to `https://github.com/IridiumFX/now/releases/tag/v1.0.0` and confirm all 4 binaries are listed.

---

## Part 3 — Enable CI on the now Repository

The now repo already has `.github/workflows/ci.yml`. Once pushed, every commit to `main` and every PR will:

1. Build with CMake on Linux, macOS, Windows, FreeBSD
2. Run the 313-test suite
3. Self-build (now builds itself from `now.pasta`)

No action needed — it activates automatically when pushed to GitHub.

---

## Part 4 — Instructions for the Cookbook Team

Send this to the cookbook team. They need a `now.pasta` in their repo root and a workflow file.

### 4.1 Add now.pasta to the cookbook repository

The cookbook team already has a working `now.pasta` from the build validation. Commit it to their repo root.

### 4.2 Add the CI workflow

Create `.github/workflows/ci.yml`:

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
      matrix:
        os: [ubuntu-latest, windows-latest]
    runs-on: ${{ matrix.os }}

    steps:
      - uses: actions/checkout@v4
        with:
          submodules: recursive

      - name: Build
        uses: IridiumFX/now-action@v1
        with:
          command: build
          verbose: true

      - name: Test
        uses: IridiumFX/now-action@v1
        with:
          command: test

      - name: Upload binary
        uses: actions/upload-artifact@v4
        with:
          name: cookbook-${{ matrix.os }}
          path: target/bin/
```

### 4.3 Pre-built libsodium

Cookbook uses `link.archives` for libsodium. The `.a` file needs to exist in the repo (or be built in a prior step). Options:

**Option A** — Commit the pre-built archive:
```
vendor/libsodium/libsodium.a    # Linux
vendor/libsodium/libsodium-win.a  # Windows
```

Then use a conditional in `now.pasta` or build separate per-platform archives.

**Option B** — Build libsodium in CI before `now build`:
```yaml
      - name: Build libsodium
        run: |
          cd vendor/libsodium
          ./configure --enable-static --disable-shared
          make -j$(nproc)

      - name: Build cookbook
        uses: IridiumFX/now-action@v1
        with:
          command: build
```

### 4.4 Cookbook-specific notes

- `sources.exclude` filters `cookbook_import.c` and `cookbook_db_postgres.c` — these are separate tools
- Windows link libs: `ws2_32`, `bcrypt`, `advapi32` — already in `now.pasta`
- macOS may need different system libs — test and adjust `link.libs` per platform if needed

---

## Part 5 — Instructions for the Apennines Team

Apennines is the simplest — zero external deps, pure C11.

### 5.1 Add now.pasta

They already have one (37 lines). Commit it to the repo root.

### 5.2 Add the CI workflow

Create `.github/workflows/ci.yml`:

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
      matrix:
        os: [ubuntu-latest, macos-latest, windows-latest]
    runs-on: ${{ matrix.os }}

    steps:
      - uses: actions/checkout@v4

      - name: Build & test
        uses: IridiumFX/now-action@v1
        with:
          command: ci
```

That's it. 286 files, 18 seconds, zero configuration beyond the `now.pasta` they already have.

### 5.3 Apennines-specific notes

- Recursive source discovery handles all t1/ through t7/ subdirectories automatically
- No submodules needed (zero deps)
- Windows libs (`ws2_32`, `bcrypt`, `winmm`) already in their `now.pasta`
- Static build variant: change `output.type` to `"static"` or use profiles

---

## Part 6 — Ongoing Maintenance

### Updating now

When a new `now` version is released:

```bash
cd C:\Users\Iridium\Projects\now
git tag v1.0.1
git push origin v1.0.1
```

The release workflow builds and uploads new binaries. Projects using `now-version: latest` (the default) pick up the new version automatically.

### Updating now-action

If the action logic changes:

```bash
cd C:\Users\Iridium\Projects\now-action
# make changes to action.yml
git add -A && git commit -m "Update action"
git tag -f v1  # force-update the v1 tag
git push origin v1 --force
```

All projects using `@v1` get the update on their next run.

### Adding a new project

For any C/C++/Rust/Go/Julia project:

1. Run `now init` in the project root (auto-detects languages)
2. Run `now build` locally to verify
3. Copy the workflow template from Part 5 into `.github/workflows/ci.yml`
4. Push

---

## Summary

| Step | Who | What | Time |
|------|-----|------|------|
| 1 | You | Push `now-action` repo, tag `v1` | 2 min |
| 2 | You | Push `now` repo, tag `v1.0.0` (triggers release build) | 5 min |
| 3 | Cookbook | Commit `now.pasta` + workflow, handle libsodium | 15 min |
| 4 | Apennines | Commit `now.pasta` + workflow | 5 min |
| 5 | Everyone | Verify green builds on GitHub | Watch |

**Result**: Three projects building on GitHub CI with `now`. No CMake, no Ninja, no Makefiles. One `now.pasta` per project, one `uses: IridiumFX/now-action@v1` per workflow.

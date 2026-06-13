# CI for now-based projects

now publishes two reusable GitHub Actions building blocks so any project
built with now gets CI without hand-rolling it. A downstream project's
buildability is tested on **its own** dashboard, not on now's — a
regression in cookbook turns cookbook red, not now.

## Quick start — reusable workflow

The least-effort option. Drop this in a downstream repo at
`.github/workflows/ci.yml`:

```yaml
name: CI
on: [push, pull_request]

jobs:
  build:
    uses: IridiumFX/now/.github/workflows/now-build.yml@main
```

That builds the project with `now build` across
ubuntu-latest / macos-latest / windows-latest, building now from
`main` on each runner.

### Options

| input               | default                                              | meaning                                                        |
|---------------------|------------------------------------------------------|----------------------------------------------------------------|
| `now-version`       | `source`                                             | A release tag (e.g. `v1.0.0-rc2`) downloads the prebuilt now; `source` builds from `now-ref`. |
| `now-ref`           | `main`                                               | now git ref to build from when `now-version: source`.          |
| `build-cmd`         | `now build`                                           | Command that builds the project.                               |
| `test-cmd`          | `''` (skipped)                                        | Optional test command, e.g. `now test`.                        |
| `runners`           | `'["ubuntu-latest", "macos-latest", "windows-latest"]'` | JSON array of runner labels.                                |
| `working-directory` | `.`                                                  | Where to run build/test.                                        |

Example pinning a released now and running tests on two OSes:

```yaml
jobs:
  build:
    uses: IridiumFX/now/.github/workflows/now-build.yml@main
    with:
      now-version: v1.0.0-rc2
      test-cmd: now test
      runners: '["ubuntu-latest", "windows-latest"]'
```

## Lower-level — composite action

If you need custom steps around the build, use the `setup-now` action
directly. It puts `now` on `PATH` and nothing else:

```yaml
jobs:
  build:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
        with:
          submodules: recursive
      - uses: IridiumFX/now/.github/actions/setup-now@main
        with:
          version: source     # or a release tag like v1.0.0-rc2
          ref: main           # used only when version: source
      - run: now build
      - run: now test
```

### Pinning

`@main` tracks the latest now CI building blocks. For reproducible CI,
pin to a tag or commit SHA instead:

```yaml
      - uses: IridiumFX/now/.github/actions/setup-now@v1.0.0-rc2
```

## Source build vs release download

- **`version: source`** (default) builds now from a git ref on each
  runner. Robust for tracking `main`; costs ~1–2 min of compile per job.
  On Windows it uses MinGW gcc (MSVC's preprocessor rejects some
  vendored apennines macros).
- **`version: <tag>`** downloads the prebuilt binary published by now's
  release workflow. Fast, no compiler needed. Asset names per OS:
  `now-linux-x64`, `now-macos-arm64`, `now-windows-x64.exe`.

## Private repositories

These building blocks run inside the downstream repo's own workflow, so
they use that repo's checkout credentials automatically — no deploy key
plumbing needed (unlike the old cross-repo cascade). A private project
just adds the same `ci.yml` as any public one.

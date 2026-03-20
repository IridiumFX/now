# now — Post-v1 Priority Map

Priorities organized by audience tier. Within each tier, items are ordered
by impact.

---

## Tier 0 — Hygiene

Things that should already be right. Fix before anything else.

| # | Item | Status |
|---|------|--------|
| 0.1 | Enforce h/c separation — move internal `.h` files out of `src/main/c/` | DONE |

---

## Tier 1 — Tinkerer Superchargers

Get people hooked. Empower early adopters, give them reasons to spread
the word.

| # | Item | Status | Notes |
|---|------|--------|-------|
| 1.1 | `compile_commands.json` | DONE | `now compile-db` generates arguments-form JSON |
| 1.2 | `now init` scaffolding | DONE | Alforno-templated scaffolding, C/C++ |
| 1.3 | `now fmt` (Pasta formatter) | DONE | `PASTA_SORTED` + `PASTA_PRETTY` |
| 1.4 | C++20 modules (pre-scan) | DONE | Module scanner, topo sort, GCC `-fmodules-ts`, MSVC `/interface`, `.cppm/.ixx/.ccm` |
| 1.5 | Java language support | DONE | Java lang registration, NowJava POM section, Maven layout defaults |
| 1.6 | Additional languages | TODO | Rust FFI, mixed C/C++ with Go |

---

## Pasta Ecosystem Integration

New libraries from the Pasta team unlock significant simplification and
new capabilities across `now`.

| # | Item | Library | Status | Impact |
|---|------|---------|--------|--------|
| P.1 | Update pasta submodule to v0.2 | pasta | DONE | `PASTA_LABEL`, `PASTA_SORTED`, section references |
| P.2 | Wire alforno into layer system | alforno | DONE | Vendored alforno sources, `alf_value_clone` + `alf_map_merge` in layer merge, 4 integration tests |
| P.3 | `now fmt` via pasta write | pasta | DONE | Parse → write with `PASTA_PRETTY \| PASTA_SORTED` — near one-liner |
| P.4 | Basta package format | basta | DONE | Single-file .basta packages with embedded blobs, replaces tar.gz + sidecar |
| P.5 | Alforno for `now init` templates | alforno | DONE | Parameterized project scaffolding via `@vars` + aggregate pipeline |

### Pasta ecosystem repos

- **Pasta** — serialization format, MIT. `PASTA_LABEL`, `PASTA_SORTED`, section refs. [github.com/IridiumFX/Pasta](https://github.com/IridiumFX/Pasta)
- **Basta** — Pasta + binary blobs (`BASTA_BLOB`: 0x00 sentinel + u64be length + raw bytes). [github.com/IridiumFX/basta](https://github.com/IridiumFX/basta)
- **Alforno** — Pasta processor. Three-pass pipeline (parameterize → merge → link). Aggregate and conflate operations. `@vars` substitution, section linking. [github.com/IridiumFX/alforno](https://github.com/IridiumFX/alforno)

---

## Tier 2 — Now Team Hardening

Prepare for growth. Solidify internals, reduce maintenance burden, be
ready for surging adoption.

| # | Item | Rationale |
|---|------|-----------|
| 2.1 | ~~Native Ed25519~~ | **DONE** — `now_ed25519.c`: SHA-512, GF(2^255-19), keypair/sign/verify/file-verify, 7 tests |
| 2.2 | ~~pico_http TLS hardening~~ | **DONE** — `VERIFY_REQUIRED` default, system CA loading (Windows+POSIX), `tls_noverify`/`ca_file`/`ca_data` options, pico_ws wired |
| 2.3 | ~~Build caching~~ | **DONE** — Content-addressable object cache at `~/.now/cache/objects/`, SHA-256 key, two-level sharding, integrated into build phase, `now cache:clean/stats` CLI. Header-aware: compiler depfiles (`-MD -MF` / `/showIncludes`), ccache-style two-level key (source_key → .deps sidecar → result_key), manifest tracks dep hashes |
| 2.4 | ~~`export:meson` / `export:bazel`~~ | **DONE** — `now export:meson` (meson.build) + `now export:bazel` (BUILD.bazel), 9 tests |
| 2.5 | ~~Plugin registry~~ | **DONE** — `now plugin:list/search/install/info`, manifest parsing, external plugin invocation via stdin/stdout Pasta IPC, 10 tests |

---

## Tier 3 — Enterprise

Pays the bills. Features that large organizations require before
adopting a build tool.

| # | Item | Rationale |
|---|------|-----------|
| 3.1 | Maven import/export | DONE — `now export:maven` + `now import:maven`, mini XML parser, roundtrip pom.xml ↔ now.pasta |
| 3.2 | Distributed/remote build | DONE (Part A) — Remote object cache: `GET/PUT /objects/{key}`, config in `~/.now/config.pasta`, integrated into build loop, `now cache:remote-stats` |
| 3.3 | SBOM generation | DONE — `now sbom` generates CycloneDX 1.5 JSON, lock file + declared deps, purl, SHA-256 hashes, dependency graph |
| 3.4 | LDAP/SSO auth for registries | DONE — Token/LDAP/OIDC auth methods, token caching with TTL, registry discovery, device code + client credentials flows, `auth:login/status/logout` CLI |
| 3.5 | Audit logging | DONE — Client-side audit trail at `~/.now/audit.pasta`, event types: build/publish/yank/procure/auth/verify/advisory, `now audit:show` CLI with filtering, config in `~/.now/config.pasta` audit section, matches cookbook server-side format |

---

## Tier 4 — Nice to Have

No urgency. Build when the moment is right or community asks.

| Item | Notes |
|------|-------|
| `export:bazel` | Promote to tier 2 if adoption demands it |
| WebSocket extensions (permessage-deflate) | pico_ws polish |
| HTTP/2 in pico_http | Performance gain but HTTP/1.1 works fine for registries |
| `now watch` (file watcher rebuild) | Cool DX but tinkerers already have `entr`/`watchexec` |
| GUI/TUI dashboard | Flashy but not essential |

---

## Self-hosting Milestone

`now` builds itself from `now.pasta` using a bootstrap binary.
27 source files, 4-way parallel, ~6s clean / ~700ms incremental.
Benchmark vs ninja+CMake: 2-5x faster across clean/incremental/no-op.

---

## Current Sprint

```
Tier 1 complete (1.1-1.5). Pasta ecosystem (P.1-P.3, P.5) complete.
Tier 2 complete (2.1-2.5). Tier 3 complete (3.1-3.5).
```

Self-hosting is done. Java + Maven interop bridges the enterprise world.
Enterprise auth (LDAP/SSO/OIDC) enables corporate registry integration.

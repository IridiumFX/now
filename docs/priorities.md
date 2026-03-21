# now — v1.0 Release Status

All tiers complete. Zero open backlog items.

---

## Tier 0 — Hygiene

| # | Item | Status |
|---|------|--------|
| 0.1 | Enforce h/c separation | DONE |

## Tier 1 — Tinkerer Superchargers

| # | Item | Status |
|---|------|--------|
| 1.1 | `compile_commands.json` | DONE — `now compile-db` |
| 1.2 | `now init` scaffolding | DONE — Alforno-templated |
| 1.3 | `now fmt` | DONE — `PASTA_SORTED` + `PASTA_PRETTY` |
| 1.4 | C++20 modules | DONE — pre-scan, topo sort, GCC/MSVC |
| 1.5 | Java support | DONE — javac/jar, Maven layout |
| 1.6 | Additional languages | DONE — Rust FFI, Go cgo, Julia embedding |

## Pasta Ecosystem

| # | Item | Status |
|---|------|--------|
| P.1 | Pasta v0.2 | DONE |
| P.2 | Alforno layer system | DONE |
| P.3 | `now fmt` via pasta write | DONE |
| P.4 | Basta packages | DONE |
| P.5 | Alforno templates | DONE |

## Tier 2 — Hardening

| # | Item | Status |
|---|------|--------|
| 2.1 | Native Ed25519 | DONE — SHA-512, GF(2^255-19), sign/verify |
| 2.2 | TLS hardening | DONE — VERIFY_REQUIRED, system CA, pico_ws |
| 2.3 | Build caching | DONE — SHA-256, two-level sharding, header-aware |
| 2.4 | Export: meson/bazel | DONE — 9 tests |
| 2.5 | Plugin registry | DONE — list/search/install/info |

## Tier 3 — Enterprise

| # | Item | Status |
|---|------|--------|
| 3.1 | Maven import/export | DONE — pom.xml roundtrip |
| 3.2 | Remote build cache | DONE — HTTP GET/PUT, circuit breaker, graph cache client |
| 3.3 | SBOM generation | DONE — CycloneDX 1.5 JSON |
| 3.4 | LDAP/SSO auth | DONE — token/LDAP/OIDC, device code flow |
| 3.5 | Audit logging | DONE — pasta format, matches cookbook server |

## Tier 4 — Polish

| Item | Status |
|------|--------|
| WebSocket permessage-deflate | DONE — RFC 7692 |
| HTTP/2 in pico_http | DONE — HPACK, frame codec, ALPN |
| `now watch` | DONE — fwatch backend, debounce |
| TUI dashboard | DONE — `--tui`, live progress bar |

---

## Languages Supported

C, C++ (including C++20 modules), asm-gas, asm-nasm, Java, Rust (FFI), Go (cgo), Julia (embedding).

## Self-hosting

`now` builds itself from `now.pasta`. 46 source files, 4-way parallel.
313 tests, all passing. Two HTTP backends: native (pico_http + mbedTLS) or apennines (TLS 1.3 + HTTP/2).

## Dependencies

- **Pasta** — serialization format (submodule)
- **Basta** — Pasta + binary blobs (submodule)
- **Alforno** — Pasta processor (vendored)
- **Apennines** — fwatch, compress, optional HTTPS stack (vendored)
- **Cookbook** — registry server (external, 619 tests)

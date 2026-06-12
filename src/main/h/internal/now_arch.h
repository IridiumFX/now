/*
 * now_arch.h — Multi-architecture platform triples (§11)
 *
 * Defines the os:arch:variant triple system, host detection,
 * target resolution, and toolchain mapping.
 */
#ifndef NOW_ARCH_H
#define NOW_ARCH_H

#include "now.h"
#include "now_pom.h"  /* NowStrArray, NowArchDict, NowProject */

/* Maximum length of a triple component */
#define NOW_TRIPLE_MAX 64

/* Platform triple: os:arch:variant */
typedef struct {
    char os[NOW_TRIPLE_MAX];       /* linux, macos, windows, freebsd, freestanding */
    char arch[NOW_TRIPLE_MAX];     /* amd64, arm64, arm32, riscv64, x86, wasm32 */
    char variant[NOW_TRIPLE_MAX];  /* gnu, musl, msvc, mingw, none */
} NowTriple;

/* Parse a triple string "os:arch:variant" into components.
 * Missing components are left empty. Returns 0 on success. */
NOW_API int now_triple_parse(NowTriple *t, const char *str);

/* Fill missing (empty) components of target from the host triple.
 * This implements the shorthand: ":amd64:musl" fills os from host. */
NOW_API void now_triple_fill_from_host(NowTriple *t);

/* Format a triple as "os:arch:variant". Returns static or malloc'd buffer.
 * buf must be at least NOW_TRIPLE_MAX*3+3 bytes. */
NOW_API void now_triple_format(const NowTriple *t, char *buf, size_t bufsize);

/* Format a triple as a directory name "os-arch-variant" (no colons). */
NOW_API void now_triple_dir(const NowTriple *t, char *buf, size_t bufsize);

/* Compare two triples. Returns 0 if equal. */
NOW_API int now_triple_cmp(const NowTriple *a, const NowTriple *b);

/* Check if a triple pattern matches a concrete triple.
 * Pattern may contain "*" in any component position.
 * Returns 1 if matches, 0 if not. */
NOW_API int now_triple_match(const NowTriple *pattern, const NowTriple *concrete);

/* Detect the host triple at runtime. Thread-safe, caches on first call. */
NOW_API const NowTriple *now_host_triple_parsed(void);

/* Returns 1 if target == host (native build), 0 if cross. */
NOW_API int now_triple_is_native(const NowTriple *target);

/* ---- Active tag set (path-based platform variants) ----
 *
 * The active tag set drives the now_fs source-discovery gate: a
 * directory whose name (resolved through arch.aliases) is in the
 * project's arch.tags compiles iff that tag is also in the active
 * set. Active set = triple os/arch/variant + user-supplied extras,
 * each canonicalized through the project's aliases.
 *
 * A NowTagSet is just a NowStrArray with deduplicating semantics on
 * add — small lookups, linear search is fine (n typically <8). */
typedef NowStrArray NowTagSet;

NOW_API void now_tagset_init(NowTagSet *s);
NOW_API void now_tagset_free(NowTagSet *s);
NOW_API int  now_tagset_has(const NowTagSet *s, const char *tag);
NOW_API int  now_tagset_add(NowTagSet *s, const char *tag);

/* Populate `out` with the active tags for this build.
 *   target    — target triple (os, arch, variant become tags)
 *   project   — used for arch.aliases canonicalization; may be NULL
 *               (then no alias resolution is applied)
 *   user_tags — extra tags from CLI (e.g. --platform-tag os4); may
 *               be NULL/0
 * Empty triple components are skipped. Duplicates are deduped. */
NOW_API int now_arch_active_tags(const NowTriple *target,
                                  const NowProject *project,
                                  const char *const *user_tags,
                                  size_t user_count,
                                  NowTagSet *out);

#endif /* NOW_ARCH_H */

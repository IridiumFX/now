/*
 * now_manifest.h — Incremental build manifest (§2.6)
 *
 * Tracks per-source-file inputs so unchanged files can be skipped.
 * Stored as target/.now-manifest (Pasta format).
 */
#ifndef NOW_MANIFEST_H
#define NOW_MANIFEST_H

#include <stddef.h>
#include "now.h"
#include "now_fs.h"  /* NowStatCache for needs_rebuild */

/* One entry per source file */
typedef struct {
    char  *source;        /* relative source path */
    char  *object;        /* output .o path */
    char  *source_hash;   /* hex SHA-256 of source file */
    char  *flags_hash;    /* hex SHA-256 of compiler flag string */
    long long mtime;      /* source file modification time (seconds since epoch) */
    char **deps;          /* header dependency paths (from compiler depfile) */
    char **dep_hashes;    /* SHA-256 of each dep */
    long long *dep_mtimes; /* mtime of each dep (fast-path skip) */
    size_t dep_count;
    size_t dep_mtime_count; /* number of valid entries in dep_mtimes */
} NowManifestEntry;

/* The full manifest */
typedef struct {
    NowManifestEntry *entries;
    size_t            count;
    size_t            capacity;
    char             *link_flags_hash;  /* hex SHA-256 of link flag string */
    /* O(1) source-path → entry index. Built lazily on first
     * now_manifest_find. Lifetime-tied to entries[] — when entries
     * resize, the index is dropped and rebuilt on the next lookup. */
    int              *index_buckets;    /* entry idx or -1; size = index_cap */
    size_t            index_cap;        /* power-of-2, 0 if unbuilt */
} NowManifest;

NOW_API void now_manifest_init(NowManifest *m);
NOW_API void now_manifest_free(NowManifest *m);

/* Load manifest from target/.now-manifest. Returns 0 on success.
 * Non-existent file is not an error — returns empty manifest. */
NOW_API int now_manifest_load(NowManifest *m, const char *path);

/* Save manifest to target/.now-manifest. Returns 0 on success. */
NOW_API int now_manifest_save(const NowManifest *m, const char *path);

/* Look up an entry by source path. Returns NULL if not found. */
NOW_API const NowManifestEntry *now_manifest_find(const NowManifest *m,
                                                    const char *source);

/* Add or update an entry. Returns 0 on success. */
NOW_API int now_manifest_set(NowManifest *m, const char *source,
                              const char *object, const char *source_hash,
                              const char *flags_hash, long long mtime);

/* Compute SHA-256 of a file. Returns malloc'd hex string, or NULL on error. */
NOW_API char *now_sha256_file(const char *path);

/* Compute SHA-256 of a string. Returns malloc'd hex string. */
NOW_API char *now_sha256_string(const char *data, size_t len);

/* Check if a source file needs recompilation against manifest entry.
 * Returns 1 if recompilation is needed, 0 if up-to-date.
 * `stat_cache` may be NULL — supplying one memoizes header-dep stat()
 * calls across sources in the same build and is the difference between
 * a fast and a slow no-op rebuild for large projects. */
NOW_API int now_manifest_needs_rebuild(const NowManifestEntry *entry,
                                        const char *basedir,
                                        const char *source,
                                        const char *flags_hash,
                                        NowStatCache *stat_cache);

/* Set header dependencies on an existing manifest entry.
 * The deps and hashes arrays are copied. Returns 0 on success. */
NOW_API int now_manifest_set_deps(NowManifest *m, const char *source,
                                   const char **deps, const char **dep_hashes,
                                   size_t dep_count);

/* ---- Per-build hash memoization cache ----
 * Avoids re-hashing the same file (e.g. system headers) across source files.
 * NOT thread-safe — use from the main build loop only. */

typedef struct NowHashMemoEntry {
    char *path;
    char *hash;
    long long mtime;  /* mtime when hash was computed */
    struct NowHashMemoEntry *next;
} NowHashMemoEntry;

typedef struct {
    NowHashMemoEntry **buckets;
    size_t bucket_count;
    size_t count;
} NowHashMemo;

NOW_API void now_hash_memo_init(NowHashMemo *memo, size_t bucket_count);
NOW_API void now_hash_memo_free(NowHashMemo *memo);

/* Hash a file with memoization. Returns malloc'd hex string (caller frees).
 * If the file was previously hashed and its mtime hasn't changed, returns
 * a copy of the cached hash without re-reading the file. */
NOW_API char *now_sha256_file_memo(const char *path, NowHashMemo *memo);

/* Global memo for use by subsystems (now_cache.c) during a build.
 * Set by now_build_compile, cleared after. NOT thread-safe. */
NOW_API extern NowHashMemo *now_hash_memo_global;

/* Prefill memo by hashing many files in parallel. After return, subsequent
 * now_sha256_file_memo() calls on these paths hit the cache instead of
 * re-reading. `paths` are absolute paths; `jobs` is the thread count
 * (0/negative → 4). Returns number of entries actually inserted. */
NOW_API size_t now_hash_memo_prefill(NowHashMemo *memo,
                                       const char *const *paths,
                                       size_t count, int jobs);

#endif /* NOW_MANIFEST_H */

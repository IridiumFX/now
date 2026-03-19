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

/* One entry per source file */
typedef struct {
    char  *source;        /* relative source path */
    char  *object;        /* output .o path */
    char  *source_hash;   /* hex SHA-256 of source file */
    char  *flags_hash;    /* hex SHA-256 of compiler flag string */
    long long mtime;      /* source file modification time (seconds since epoch) */
    char **deps;          /* header dependency paths (from compiler depfile) */
    char **dep_hashes;    /* SHA-256 of each dep */
    size_t dep_count;
} NowManifestEntry;

/* The full manifest */
typedef struct {
    NowManifestEntry *entries;
    size_t            count;
    size_t            capacity;
    char             *link_flags_hash;  /* hex SHA-256 of link flag string */
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
 * Returns 1 if recompilation is needed, 0 if up-to-date. */
NOW_API int now_manifest_needs_rebuild(const NowManifestEntry *entry,
                                        const char *basedir,
                                        const char *source,
                                        const char *flags_hash);

/* Set header dependencies on an existing manifest entry.
 * The deps and hashes arrays are copied. Returns 0 on success. */
NOW_API int now_manifest_set_deps(NowManifest *m, const char *source,
                                   const char **deps, const char **dep_hashes,
                                   size_t dep_count);

#endif /* NOW_MANIFEST_H */

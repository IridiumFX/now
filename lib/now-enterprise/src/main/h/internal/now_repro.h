/*
 * now_repro.h — Reproducible builds (§26)
 *
 * Determinism measures: timebase, path prefix maps,
 * sorted inputs, date macro neutralization, verify pass.
 */
#ifndef NOW_REPRO_H
#define NOW_REPRO_H

#include "now.h"

/* Reproducibility configuration (parsed from now.pasta reproducible:) */
typedef struct {
    int  enabled;           /* master switch (reproducible: true) */
    char *timebase;         /* "now", "git-commit", "zero", or ISO 8601 */
    int  path_prefix_map;   /* strip absolute build paths from debug info */
    int  sort_inputs;       /* deterministic file ordering */
    int  no_date_macros;    /* neutralize __DATE__ / __TIME__ */
    int  strip_metadata;    /* strip non-deterministic binary metadata */
    int  verify;            /* post-build verification pass */
} NowReproConfig;

/* Initialize with defaults (all off) */
NOW_API void now_repro_init(NowReproConfig *cfg);

/* Parse reproducible: field from project's Pasta root.
 * Handles both boolean shorthand and map form. */
NOW_API void now_repro_from_project(NowReproConfig *cfg, const void *project);

/* Free any allocated strings in the config */
NOW_API void now_repro_free(NowReproConfig *cfg);

/* Resolve the timebase to a Unix timestamp string (for --mtime, -D__DATE__).
 * basedir is the project root (for git-commit).
 * Returns a malloc'd string like "2026-03-05T00:00:00Z", or NULL on error. */
NOW_API char *now_repro_resolve_timebase(const NowReproConfig *cfg,
                                          const char *basedir,
                                          NowResult *result);

/* Collect extra compile flags needed for reproducibility.
 * Appends flags to the provided arrays. basedir is the project root.
 * timestamp is the resolved timebase (from now_repro_resolve_timebase).
 * is_msvc selects MSVC flag syntax.
 * Returns number of flags added, or -1 on error. */
NOW_API int now_repro_compile_flags(const NowReproConfig *cfg,
                                     const char *basedir,
                                     const char *timestamp,
                                     int is_msvc,
                                     char ***out_flags,
                                     size_t *out_count);

/* Free a flags array returned by now_repro_compile_flags */
NOW_API void now_repro_free_flags(char **flags, size_t count);

/* Sort a NowFileList lexicographically by path (for sort_inputs) */
NOW_API void now_repro_sort_filelist(void *filelist);

/* Run reproducible:check — build twice, compare output hashes.
 * Returns 0 if outputs match, 1 if they differ, -1 on error. */
NOW_API int now_repro_check(const void *project, const char *basedir,
                             int verbose, int jobs, NowResult *result);

/* Collect extra linker flags for reproducibility (e.g. --build-id=sha1).
 * Returns number of flags added. */
NOW_API int now_repro_link_flags(const NowReproConfig *cfg, int is_msvc,
                                  char ***out_flags, size_t *out_count);

#endif /* NOW_REPRO_H */

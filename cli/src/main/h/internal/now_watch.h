/*
 * now_watch.h — File watcher for incremental rebuilds
 *
 * Monitors source directories and triggers `now build` on changes.
 * Starts with stat-based polling; designed for fwatch swap-in later.
 */
#ifndef NOW_WATCH_H
#define NOW_WATCH_H

#include "now_pom.h"
#include "now.h"

/* Watch options */
typedef struct {
    int verbose;
    int jobs;
    int poll_ms;    /* polling interval in ms (default 500) */
} NowWatchOpts;

/* Initialize options with defaults */
NOW_API void now_watch_opts_init(NowWatchOpts *opts);

/* Run the watch loop (blocks until Ctrl+C or project file change).
 * Returns 0 on clean exit (Ctrl+C), NOW_ERR_SCHEMA if now.pasta changed
 * (caller should reload project and restart), or negative on error. */
NOW_API int now_watch(const NowProject *project, const char *basedir,
                      const NowWatchOpts *opts, NowResult *result);

/* ---- Snapshot API (internal, exported for testing) ---- */

typedef struct {
    char  *path;
    long long mtime;
} NowWatchEntry;

typedef struct {
    NowWatchEntry *entries;
    size_t count;
    size_t capacity;
    long long pasta_mtime;   /* mtime of now.pasta */
} NowWatchSnapshot;

/* Snapshot all watchable files (sources + headers) */
NOW_API int now_watch_snapshot(const NowProject *project,
                                const char *basedir,
                                NowWatchSnapshot *snap);

/* Compare snapshots. Returns:
 *   0 = no changes
 *   1 = source/header changed
 *   2 = now.pasta changed
 *   3 = both */
NOW_API int now_watch_diff(const NowWatchSnapshot *old,
                            const NowWatchSnapshot *cur);

NOW_API void now_watch_snapshot_free(NowWatchSnapshot *snap);

#endif /* NOW_WATCH_H */

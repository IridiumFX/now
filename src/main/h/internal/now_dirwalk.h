/*
 * now_dirwalk.h — Cached directory listings
 *
 * Persistent cache mapping absolute directory path → (mtime, entries).
 * When dir mtime matches the cached value, callers can skip readdir()
 * and use the cached file/subdir list. Dir mtime updates when files are
 * added/removed, but not on content edits — exactly the invalidation
 * signal we need for "is the list the same as last build?".
 */
#ifndef NOW_DIRWALK_H
#define NOW_DIRWALK_H

#include <stddef.h>
#include "now.h"

typedef struct {
    char      *dir_path;  /* absolute canonicalized path */
    long long  mtime;
    char     **entries;   /* non-hidden entries (files + dirs) */
    int       *is_dir;    /* parallel: 1 if directory */
    size_t     count;
} NowDirCacheEntry;

typedef struct {
    NowDirCacheEntry *entries;
    size_t            count;
    size_t            capacity;
} NowDirwalkCache;

NOW_API void now_dirwalk_init(NowDirwalkCache *cache);
NOW_API void now_dirwalk_free(NowDirwalkCache *cache);

/* Load from pasta file. Returns 0 if file missing (empty cache) or loaded OK. */
NOW_API int now_dirwalk_load(NowDirwalkCache *cache, const char *path);

/* Save to pasta file. Returns 0 on success. */
NOW_API int now_dirwalk_save(const NowDirwalkCache *cache, const char *path);

/* Look up dir. Returns entry if cached with matching mtime, else NULL. */
NOW_API const NowDirCacheEntry *now_dirwalk_get(const NowDirwalkCache *cache,
                                                  const char *dir_path,
                                                  long long cur_mtime);

/* Insert or update. Takes ownership of entries[] and is_dir[] (caller must
 * not free these arrays or the strings in entries[] afterwards). */
NOW_API void now_dirwalk_put(NowDirwalkCache *cache, const char *dir_path,
                              long long mtime, char **entries, int *is_dir,
                              size_t count);

/* Global cache: set during build phase, used by now_fs.c discover_recursive.
 * NULL means no cache active — full walk every time. */
NOW_API extern NowDirwalkCache *now_dirwalk_cache_global;

#endif /* NOW_DIRWALK_H */

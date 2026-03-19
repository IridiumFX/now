/*
 * now_remote.h — Remote object cache for distributed builds
 *
 * Enables shared compilation caches across CI machines and developer
 * workstations via HTTP GET/PUT to a remote object cache server.
 * Config loaded from ~/.now/config.pasta -> object_cache section.
 */
#ifndef NOW_REMOTE_H
#define NOW_REMOTE_H

#include "now.h"

/* Remote object cache configuration */
typedef struct {
    char *url;    /* Base URL, e.g. "http://cache.local:9090" */
    char *token;  /* Bearer token (may be NULL for unauthenticated) */
    int   push;   /* 1 = push objects after compile, 0 = read-only */
} NowRemoteCacheConfig;

/* Load remote cache config from ~/.now/config.pasta -> object_cache section.
 * Returns 0 if config found and populated, -1 if no config or error.
 * Caller must free via now_remote_config_free(). */
NOW_API int now_remote_config_load(NowRemoteCacheConfig *cfg);

/* Parse remote cache config from a Pasta string (for testing).
 * Returns 0 if object_cache section found with url, -1 otherwise. */
NOW_API int now_remote_config_parse(const char *pasta_str, size_t len,
                                     NowRemoteCacheConfig *cfg);

/* Free config fields. Safe to call on zeroed struct. */
NOW_API void now_remote_config_free(NowRemoteCacheConfig *cfg);

/* Try to restore an object from the remote cache.
 * GET {url}/objects/{cache_key}{obj_ext}
 * On 200, writes body to dst_path and returns 0.
 * On 404 or any error, returns -1 (silent fallback). */
NOW_API int now_remote_cache_restore(const NowRemoteCacheConfig *cfg,
                                      const char *cache_key,
                                      const char *dst_path,
                                      const char *obj_ext);

/* Push an object to the remote cache.
 * PUT {url}/objects/{cache_key}{obj_ext} with file body.
 * Returns 0 on 200/201, -1 on error. No-op if cfg->push is 0. */
NOW_API int now_remote_cache_store(const NowRemoteCacheConfig *cfg,
                                    const char *cache_key,
                                    const char *obj_path,
                                    const char *obj_ext);

/* Print remote cache info (connectivity check).
 * Returns 0 on success, -1 on error. */
NOW_API int now_remote_cache_print_stats(const NowRemoteCacheConfig *cfg,
                                          int verbose);

#endif /* NOW_REMOTE_H */

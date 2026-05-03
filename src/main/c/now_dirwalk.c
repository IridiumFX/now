/*
 * now_dirwalk.c — Cached directory listings (see now_dirwalk.h)
 */
#include "now_dirwalk.h"
#include "now_fs.h"
#include "now_manifest.h"
#include "pasta.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <limits.h>
#include <sys/stat.h>

/* On MinGW, PATH_MAX is 260 (MAX_PATH); on Linux/macOS it's 4096. We need at
 * least 4096 to satisfy glibc fortify's __realpath_chk static check. */
#if !defined(PATH_MAX) || PATH_MAX < 4096
  #undef PATH_MAX
  #define PATH_MAX 4096
#endif

NOW_API NowDirwalkCache *now_dirwalk_cache_global = NULL;

NOW_API void now_dirwalk_init(NowDirwalkCache *cache) {
    if (!cache) return;
    cache->entries  = NULL;
    cache->count    = 0;
    cache->capacity = 0;
}

NOW_API void now_dirwalk_free(NowDirwalkCache *cache) {
    if (!cache) return;
    for (size_t i = 0; i < cache->count; i++) {
        NowDirCacheEntry *e = &cache->entries[i];
        free(e->dir_path);
        for (size_t j = 0; j < e->count; j++) free(e->entries[j]);
        free(e->entries);
        free(e->is_dir);
    }
    free(cache->entries);
    cache->entries  = NULL;
    cache->count    = 0;
    cache->capacity = 0;
}

NOW_API const NowDirCacheEntry *now_dirwalk_get(const NowDirwalkCache *cache,
                                                  const char *dir_path,
                                                  long long cur_mtime) {
    if (!cache || !dir_path) return NULL;
    for (size_t i = 0; i < cache->count; i++) {
        if (strcmp(cache->entries[i].dir_path, dir_path) == 0) {
            if (cache->entries[i].mtime == cur_mtime)
                return &cache->entries[i];
            return NULL;  /* mtime mismatch */
        }
    }
    return NULL;
}

static NowDirCacheEntry *find_or_grow(NowDirwalkCache *cache, const char *dir_path) {
    for (size_t i = 0; i < cache->count; i++)
        if (strcmp(cache->entries[i].dir_path, dir_path) == 0)
            return &cache->entries[i];

    if (cache->count >= cache->capacity) {
        size_t newcap = cache->capacity ? cache->capacity * 2 : 16;
        NowDirCacheEntry *ne = (NowDirCacheEntry *)realloc(cache->entries,
                                  newcap * sizeof(NowDirCacheEntry));
        if (!ne) return NULL;
        cache->entries  = ne;
        cache->capacity = newcap;
    }
    NowDirCacheEntry *e = &cache->entries[cache->count++];
    memset(e, 0, sizeof(*e));
    e->dir_path = strdup(dir_path);
    return e;
}

NOW_API void now_dirwalk_put(NowDirwalkCache *cache, const char *dir_path,
                              long long mtime, char **entries, int *is_dir,
                              size_t count) {
    if (!cache || !dir_path) return;
    NowDirCacheEntry *e = find_or_grow(cache, dir_path);
    if (!e) return;

    /* Replace existing data */
    for (size_t j = 0; j < e->count; j++) free(e->entries[j]);
    free(e->entries);
    free(e->is_dir);

    e->mtime   = mtime;
    e->entries = entries;
    e->is_dir  = is_dir;
    e->count   = count;
}

/* ---- Pasta serialization ----
 * Format:
 * {
 *   "/abs/path/to/dir": {
 *     mtime: 1234567890,
 *     e: ["name1", "name2", ...],
 *     d: "01001..."    # parallel: '1' for dir, '0' for file
 *   },
 *   ...
 * }
 * Names-as-array avoids separator-byte hazards; flags as a packed
 * 0/1 string stays compact since we'd otherwise need an array of bools. */

static char *build_dir_flags(const int *is_dir, size_t count) {
    char *buf = (char *)malloc(count + 1);
    if (!buf) return NULL;
    for (size_t i = 0; i < count; i++) buf[i] = is_dir[i] ? '1' : '0';
    buf[count] = '\0';
    return buf;
}

NOW_API int now_dirwalk_save(const NowDirwalkCache *cache, const char *path) {
    if (!cache || !path) return -1;

    PastaValue *root = pasta_new_map();
    if (!root) return -1;

    for (size_t i = 0; i < cache->count; i++) {
        const NowDirCacheEntry *e = &cache->entries[i];
        PastaValue *entry = pasta_new_map();
        if (!entry) continue;

        /* Serialize mtime as a string — high-precision values (nanoseconds
         * or 100ns ticks) exceed double's 2^53 integer range. */
        char mbuf[32];
        snprintf(mbuf, sizeof(mbuf), "%lld", e->mtime);
        pasta_set(entry, "mtime", pasta_new_string(mbuf));

        PastaValue *arr = pasta_new_array();
        if (arr) {
            for (size_t k = 0; k < e->count; k++)
                pasta_push(arr, pasta_new_string(e->entries[k]));
            pasta_set(entry, "e", arr);
        }

        char *flags = build_dir_flags(e->is_dir, e->count);
        if (flags) {
            pasta_set(entry, "d", pasta_new_string(flags));
            free(flags);
        }

        pasta_set(root, e->dir_path, entry);
    }

    char *text = pasta_write(root, 0);
    pasta_free(root);
    if (!text) return -1;

    FILE *fp = fopen(path, "wb");
    if (!fp) { free(text); return -1; }
    size_t n = strlen(text);
    size_t written = fwrite(text, 1, n, fp);
    fclose(fp);
    free(text);
    return written == n ? 0 : -1;
}

NOW_API int now_dirwalk_load(NowDirwalkCache *cache, const char *path) {
    if (!cache || !path) return -1;
    now_dirwalk_init(cache);

    if (!now_path_exists(path)) return 0;  /* no file → empty cache OK */

    FILE *fp = fopen(path, "rb");
    if (!fp) return -1;

    fseek(fp, 0, SEEK_END);
    long len = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    char *buf = (char *)malloc((size_t)len + 1);
    if (!buf) { fclose(fp); return -1; }
    size_t nread = fread(buf, 1, (size_t)len, fp);
    buf[nread] = '\0';
    fclose(fp);

    PastaResult pr;
    PastaValue *root = pasta_parse(buf, nread, &pr);
    free(buf);
    if (!root || pr.code != PASTA_OK) { if (root) pasta_free(root); return -1; }
    if (pasta_type(root) != PASTA_MAP) { pasta_free(root); return -1; }

    size_t map_count = pasta_count(root);
    for (size_t i = 0; i < map_count; i++) {
        const char *dir_path = pasta_map_key(root, i);
        const PastaValue *entry = pasta_map_value(root, i);
        if (!dir_path || !entry || pasta_type(entry) != PASTA_MAP) continue;

        const PastaValue *mv = pasta_map_get(entry, "mtime");
        const PastaValue *ev = pasta_map_get(entry, "e");
        const PastaValue *dv = pasta_map_get(entry, "d");
        if (!mv || !ev || !dv) continue;
        if (pasta_type(ev) != PASTA_ARRAY)  continue;
        if (pasta_type(dv) != PASTA_STRING) continue;

        /* mtime may be either a string (high-precision, current format) or
         * a number (older low-precision caches — gets invalidated on first
         * walk anyway since units differ). */
        long long mtime;
        if (pasta_type(mv) == PASTA_STRING) {
            mtime = strtoll(pasta_get_string(mv), NULL, 10);
        } else if (pasta_type(mv) == PASTA_NUMBER) {
            mtime = (long long)pasta_get_number(mv);
        } else {
            continue;
        }
        size_t count = pasta_count(ev);
        const char *flags  = pasta_get_string(dv);

        char **entries = (char **)calloc(count, sizeof(char *));
        int *is_dir = (int *)calloc(count, sizeof(int));
        if (!entries || !is_dir) {
            free(entries); free(is_dir);
            continue;
        }
        size_t flen = strlen(flags);
        int ok = 1;
        for (size_t k = 0; k < count; k++) {
            const PastaValue *item = pasta_array_get(ev, k);
            if (!item || pasta_type(item) != PASTA_STRING) { ok = 0; break; }
            entries[k] = strdup(pasta_get_string(item));
            if (!entries[k]) { ok = 0; break; }
            is_dir[k] = (k < flen && flags[k] == '1') ? 1 : 0;
        }
        if (!ok) {
            for (size_t k = 0; k < count; k++) free(entries[k]);
            free(entries); free(is_dir);
            continue;
        }

        now_dirwalk_put(cache, dir_path, mtime, entries, is_dir, count);
    }

    pasta_free(root);
    return 0;
}

/* ---- Project-keyed cache path ---- */

NOW_API char *now_dirwalk_cache_path(const char *basedir) {
    if (!basedir) return NULL;

    /* Canonicalize basedir → absolute path. POSIX realpath() requires the
     * destination buffer to be at least PATH_MAX bytes; glibc's fortify
     * wrapper enforces that statically and aborts otherwise. */
    char canon[PATH_MAX];
#ifdef _WIN32
    if (!_fullpath(canon, basedir, sizeof(canon)))
        snprintf(canon, sizeof(canon), "%s", basedir);
#else
    if (!realpath(basedir, canon))
        snprintf(canon, sizeof(canon), "%s", basedir);
#endif

    /* SHA-256 prefix as project identity */
    char *full_hash = now_sha256_string(canon, strlen(canon));
    if (!full_hash) return NULL;
    char prefix[17];
    memcpy(prefix, full_hash, 16);
    prefix[16] = '\0';
    free(full_hash);

    /* ~/.now/dirwalk/ */
    const char *home = NULL;
#ifdef _WIN32
    home = getenv("USERPROFILE");
    if (!home) home = getenv("HOME");
#else
    home = getenv("HOME");
#endif
    if (!home) return NULL;

    char *dotnow  = now_path_join(home,   ".now");
    char *dwdir   = dotnow ? now_path_join(dotnow, "dirwalk") : NULL;
    free(dotnow);
    if (!dwdir) return NULL;
    now_mkdir_p(dwdir);

    char fname[32];
    snprintf(fname, sizeof(fname), "%s.pasta", prefix);
    char *out = now_path_join(dwdir, fname);
    free(dwdir);
    return out;
}

/*
 * now_cache.c — Content-addressable build cache
 *
 * Objects are stored at ~/.now/cache/objects/{ab}/{cd}/{hash}{ext}
 * where {ab} and {cd} are the first four hex chars of the cache key,
 * providing two-level sharding (65536 buckets).
 */
#include "now_cache.h"
#include "now_fs.h"
#include "now_manifest.h"  /* now_sha256_string */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>

#ifdef _WIN32
  #include <windows.h>
  #include <direct.h>
#else
  #include <dirent.h>
  #include <unistd.h>
  #include <pwd.h>
#endif

/* ---- Home directory resolution ---- */

NOW_API char *now_cache_root(void) {
    const char *home = NULL;
#ifdef _WIN32
    home = getenv("USERPROFILE");
    if (!home) home = getenv("HOME");
#else
    home = getenv("HOME");
    if (!home) {
        struct passwd *pw = getpwuid(getuid());
        if (pw) home = pw->pw_dir;
    }
#endif
    if (!home) return NULL;

    char *dot_now = now_path_join(home, ".now");
    if (!dot_now) return NULL;
    char *cache = now_path_join(dot_now, "cache");
    free(dot_now);
    if (!cache) return NULL;
    char *objects = now_path_join(cache, "objects");
    free(cache);
    return objects;
}

/* ---- Cache key computation ---- */

NOW_API char *now_cache_key(const char *source_hash,
                            const char *flags_hash,
                            const char *compiler_path) {
    if (!source_hash || !flags_hash || !compiler_path)
        return NULL;

    /* Concatenate: source_hash + "\n" + flags_hash + "\n" + compiler_path */
    size_t sh_len = strlen(source_hash);
    size_t fh_len = strlen(flags_hash);
    size_t cp_len = strlen(compiler_path);
    size_t total = sh_len + 1 + fh_len + 1 + cp_len;

    char *buf = (char *)malloc(total + 1);
    if (!buf) return NULL;

    memcpy(buf, source_hash, sh_len);
    buf[sh_len] = '\n';
    memcpy(buf + sh_len + 1, flags_hash, fh_len);
    buf[sh_len + 1 + fh_len] = '\n';
    memcpy(buf + sh_len + 1 + fh_len + 1, compiler_path, cp_len);
    buf[total] = '\0';

    char *key = now_sha256_string(buf, total);
    free(buf);
    return key;
}

/* ---- Sharded path computation ---- */

NOW_API char *now_cache_path(const char *cache_key, const char *obj_ext) {
    if (!cache_key || strlen(cache_key) < 4) return NULL;

    char *root = now_cache_root();
    if (!root) return NULL;

    /* Build: root/ab/cd/key.ext */
    char shard1[3] = { cache_key[0], cache_key[1], '\0' };
    char shard2[3] = { cache_key[2], cache_key[3], '\0' };

    char *d1 = now_path_join(root, shard1);
    free(root);
    if (!d1) return NULL;

    char *d2 = now_path_join(d1, shard2);
    free(d1);
    if (!d2) return NULL;

    /* filename: key + ext */
    const char *ext = obj_ext ? obj_ext : ".o";
    size_t klen = strlen(cache_key);
    size_t elen = strlen(ext);
    char *filename = (char *)malloc(klen + elen + 1);
    if (!filename) { free(d2); return NULL; }
    memcpy(filename, cache_key, klen);
    memcpy(filename + klen, ext, elen);
    filename[klen + elen] = '\0';

    char *result = now_path_join(d2, filename);
    free(d2);
    free(filename);
    return result;
}

/* ---- Ensure parent directory exists ---- */

static void ensure_parent_dir(const char *path) {
    char *parent = strdup(path);
    if (!parent) return;
    char *sep = strrchr(parent, '/');
    if (!sep) sep = strrchr(parent, '\\');
    if (sep) {
        *sep = '\0';
        now_mkdir_p(parent);
    }
    free(parent);
}

/* ---- Cache restore (lookup + copy) ---- */

NOW_API int now_cache_restore(const char *cache_key,
                              const char *dst_path,
                              const char *obj_ext) {
    if (!cache_key || !dst_path) return -1;

    char *cached = now_cache_path(cache_key, obj_ext);
    if (!cached) return -1;

    if (!now_path_exists(cached)) {
        free(cached);
        return -1;  /* miss */
    }

    /* Ensure destination directory exists */
    ensure_parent_dir(dst_path);

    int rc = now_file_copy(cached, dst_path);
    free(cached);
    return rc;
}

/* ---- Cache store (copy + atomic rename) ---- */

NOW_API int now_cache_store(const char *cache_key,
                            const char *obj_path,
                            const char *obj_ext) {
    if (!cache_key || !obj_path) return -1;

    char *cached = now_cache_path(cache_key, obj_ext);
    if (!cached) return -1;

    /* Already cached? Skip. */
    if (now_path_exists(cached)) {
        free(cached);
        return 0;
    }

    /* Ensure shard directory exists */
    ensure_parent_dir(cached);

    /* Write to temp file, then rename for atomicity */
    size_t clen = strlen(cached);
    char *tmp = (char *)malloc(clen + 5);
    if (!tmp) { free(cached); return -1; }
    memcpy(tmp, cached, clen);
    memcpy(tmp + clen, ".tmp", 5);

    int rc = now_file_copy(obj_path, tmp);
    if (rc == 0) {
#ifdef _WIN32
        /* Windows: remove target first (rename doesn't overwrite) */
        _unlink(cached);
#endif
        if (rename(tmp, cached) != 0) {
            /* Rename failed — clean up temp */
            remove(tmp);
            rc = -1;
        }
    } else {
        remove(tmp);
    }

    free(tmp);
    free(cached);
    return rc;
}

/* ---- Cache clean ---- */

NOW_API int now_cache_clean(void) {
    char *root = now_cache_root();
    if (!root) return -1;

    if (!now_path_exists(root)) {
        free(root);
        return 0;
    }

#ifdef _WIN32
    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "rmdir /s /q \"%s\"", root);
    int rc = system(cmd);
#else
    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", root);
    int rc = system(cmd);
#endif

    free(root);
    return rc == 0 ? 0 : -1;
}

/* ---- Cache statistics ---- */

#ifdef _WIN32
static void walk_cache_dir(const char *dir, int *count, long long *total_size) {
    char pattern[1024];
    snprintf(pattern, sizeof(pattern), "%s\\*", dir);

    WIN32_FIND_DATAA fdata;
    HANDLE h = FindFirstFileA(pattern, &fdata);
    if (h == INVALID_HANDLE_VALUE) return;

    do {
        if (fdata.cFileName[0] == '.') continue;
        char *child = now_path_join(dir, fdata.cFileName);
        if (!child) continue;

        if (fdata.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            walk_cache_dir(child, count, total_size);
        } else {
            (*count)++;
            *total_size += ((long long)fdata.nFileSizeHigh << 32) | fdata.nFileSizeLow;
        }
        free(child);
    } while (FindNextFileA(h, &fdata));

    FindClose(h);
}
#else
static void walk_cache_dir(const char *dir, int *count, long long *total_size) {
    DIR *d = opendir(dir);
    if (!d) return;

    struct dirent *entry;
    while ((entry = readdir(d)) != NULL) {
        if (entry->d_name[0] == '.') continue;
        char *child = now_path_join(dir, entry->d_name);
        if (!child) continue;

        struct stat st;
        if (stat(child, &st) == 0) {
            if (S_ISDIR(st.st_mode)) {
                walk_cache_dir(child, count, total_size);
            } else {
                (*count)++;
                *total_size += (long long)st.st_size;
            }
        }
        free(child);
    }
    closedir(d);
}
#endif

/* ---- Dependency list helpers ---- */

NOW_API void now_deplist_free(NowDepList *dl) {
    if (!dl) return;
    for (size_t i = 0; i < dl->count; i++)
        free(dl->paths[i]);
    free(dl->paths);
    dl->paths = NULL;
    dl->count = dl->capacity = 0;
}

static void deplist_push(NowDepList *dl, const char *path, size_t len) {
    if (dl->count >= dl->capacity) {
        size_t newcap = dl->capacity ? dl->capacity * 2 : 16;
        char **tmp = (char **)realloc(dl->paths, newcap * sizeof(char *));
        if (!tmp) return;
        dl->paths = tmp;
        dl->capacity = newcap;
    }
    char *s = (char *)malloc(len + 1);
    if (!s) return;
    memcpy(s, path, len);
    s[len] = '\0';
    dl->paths[dl->count++] = s;
}

/* Normalize path separators to '/' for comparison */
static void normalize_slashes(char *p) {
    for (; *p; p++)
        if (*p == '\\') *p = '/';
}

/* ---- GCC/Clang depfile parser ---- */

NOW_API int now_depfile_parse(const char *depfile_path,
                               const char *source_path,
                               NowDepList *out) {
    if (!depfile_path || !out) return -1;
    memset(out, 0, sizeof(*out));

    FILE *f = fopen(depfile_path, "rb");
    if (!f) return -1;

    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (fsize <= 0) { fclose(f); return 0; }

    char *buf = (char *)malloc((size_t)fsize + 1);
    if (!buf) { fclose(f); return -1; }
    size_t nread = fread(buf, 1, (size_t)fsize, f);
    fclose(f);
    buf[nread] = '\0';

    /* Collapse backslash-newline continuations */
    size_t w = 0;
    for (size_t r = 0; r < nread; r++) {
        if (buf[r] == '\\' && r + 1 < nread && buf[r + 1] == '\n') {
            buf[w++] = ' ';
            r++;  /* skip \n */
        } else if (buf[r] == '\\' && r + 2 < nread &&
                   buf[r + 1] == '\r' && buf[r + 2] == '\n') {
            buf[w++] = ' ';
            r += 2;  /* skip \r\n */
        } else {
            buf[w++] = buf[r];
        }
    }
    buf[w] = '\0';

    /* Skip past first ':' (target portion) */
    char *colon = strchr(buf, ':');
    char *pos = colon ? colon + 1 : buf;

    /* Normalize source_path for comparison */
    char *src_norm = NULL;
    if (source_path) {
        src_norm = strdup(source_path);
        if (src_norm) normalize_slashes(src_norm);
    }

    /* Tokenize on whitespace */
    while (*pos) {
        while (*pos == ' ' || *pos == '\t' || *pos == '\n' || *pos == '\r')
            pos++;
        if (!*pos) break;

        char *start = pos;
        while (*pos && *pos != ' ' && *pos != '\t' && *pos != '\n' && *pos != '\r')
            pos++;
        size_t tlen = (size_t)(pos - start);
        if (tlen == 0) continue;

        /* Normalize this token for comparison */
        char *token = (char *)malloc(tlen + 1);
        if (!token) continue;
        memcpy(token, start, tlen);
        token[tlen] = '\0';

        char *token_norm = strdup(token);
        if (token_norm) normalize_slashes(token_norm);

        /* Skip if it matches the source file */
        int skip = 0;
        if (src_norm && token_norm) {
            if (strcmp(token_norm, src_norm) == 0)
                skip = 1;
            /* Also check basename match */
            const char *tb = strrchr(token_norm, '/');
            const char *sb = strrchr(src_norm, '/');
            if (!skip && tb && sb && strcmp(tb, sb) == 0)
                skip = 1;
            if (!skip && !tb && sb && strcmp(token_norm, sb + 1) == 0)
                skip = 1;
            if (!skip && tb && !sb && strcmp(tb + 1, src_norm) == 0)
                skip = 1;
        }

        free(token_norm);

        if (!skip)
            deplist_push(out, token, tlen);

        free(token);
    }

    free(src_norm);
    free(buf);
    return 0;
}

/* ---- MSVC /showIncludes parser ---- */

NOW_API int now_depfile_parse_msvc(const char *output, size_t output_len,
                                    NowDepList *out) {
    if (!output || !out) return -1;
    memset(out, 0, sizeof(*out));

    static const char prefix[] = "Note: including file:";
    size_t prefix_len = sizeof(prefix) - 1;

    const char *pos = output;
    const char *end = output + output_len;

    while (pos < end) {
        /* Find end of line */
        const char *eol = pos;
        while (eol < end && *eol != '\n') eol++;

        size_t line_len = (size_t)(eol - pos);
        /* Strip trailing \r */
        if (line_len > 0 && pos[line_len - 1] == '\r')
            line_len--;

        /* Check for /showIncludes prefix */
        if (line_len > prefix_len &&
            memcmp(pos, prefix, prefix_len) == 0) {
            /* Extract path, skip leading whitespace after prefix */
            const char *pstart = pos + prefix_len;
            while (pstart < pos + line_len &&
                   (*pstart == ' ' || *pstart == '\t'))
                pstart++;
            size_t plen = (size_t)((pos + line_len) - pstart);
            if (plen > 0)
                deplist_push(out, pstart, plen);
        }

        pos = eol + 1;
    }
    return 0;
}

/* ---- Dep-aware cache: .deps sidecar I/O ---- */

/* Read a .deps sidecar file. Populates stored_result_key (caller frees),
 * dep_paths and dep_hashes arrays (parallel, caller frees each + arrays).
 * Returns 0 on success, -1 on error. */
static int read_deps_file(const char *deps_path,
                           char **stored_result_key,
                           char ***dep_paths, char ***dep_hashes,
                           size_t *dep_count) {
    FILE *f = fopen(deps_path, "r");
    if (!f) return -1;

    *stored_result_key = NULL;
    *dep_paths = NULL;
    *dep_hashes = NULL;
    *dep_count = 0;

    size_t cap = 0;
    char line[4096];

    while (fgets(line, sizeof(line), f)) {
        /* Strip trailing newline/CR */
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
            line[--len] = '\0';
        if (len == 0) continue;

        if (strncmp(line, "result:", 7) == 0) {
            free(*stored_result_key);
            *stored_result_key = strdup(line + 7);
            continue;
        }

        /* dep line: {path}\t{hash} */
        char *tab = strchr(line, '\t');
        if (!tab) continue;

        if (*dep_count >= cap) {
            size_t newcap = cap ? cap * 2 : 16;
            char **np = (char **)realloc(*dep_paths, newcap * sizeof(char *));
            char **nh = (char **)realloc(*dep_hashes, newcap * sizeof(char *));
            if (!np || !nh) { free(np); free(nh); break; }
            *dep_paths = np;
            *dep_hashes = nh;
            cap = newcap;
        }

        *tab = '\0';
        (*dep_paths)[*dep_count] = strdup(line);
        (*dep_hashes)[*dep_count] = strdup(tab + 1);
        (*dep_count)++;
    }

    fclose(f);
    return (*stored_result_key) ? 0 : -1;
}

static void free_deps_arrays(char **paths, char **hashes, size_t count) {
    for (size_t i = 0; i < count; i++) {
        free(paths[i]);
        free(hashes[i]);
    }
    free(paths);
    free(hashes);
}

/* Compute result_key from source_key + sorted dep hashes */
static char *compute_result_key(const char *source_key,
                                 const char **dep_hashes,
                                 size_t dep_count) {
    /* result_key = SHA-256(source_key + "\n" + hash1 + "\n" + hash2 + ...) */
    size_t sk_len = strlen(source_key);
    size_t total = sk_len;
    for (size_t i = 0; i < dep_count; i++)
        total += 1 + strlen(dep_hashes[i]);  /* "\n" + hash */

    char *buf = (char *)malloc(total + 1);
    if (!buf) return NULL;

    memcpy(buf, source_key, sk_len);
    size_t pos = sk_len;
    for (size_t i = 0; i < dep_count; i++) {
        buf[pos++] = '\n';
        size_t hlen = strlen(dep_hashes[i]);
        memcpy(buf + pos, dep_hashes[i], hlen);
        pos += hlen;
    }
    buf[pos] = '\0';

    char *key = now_sha256_string(buf, pos);
    free(buf);
    return key;
}

/* Sort helper for dep paths+hashes (parallel arrays, sort by path) */
static void sort_deps(char **paths, char **hashes, size_t count) {
    /* Simple insertion sort — dep lists are small (dozens, not thousands) */
    for (size_t i = 1; i < count; i++) {
        char *kp = paths[i];
        char *kh = hashes[i];
        size_t j = i;
        while (j > 0 && strcmp(paths[j - 1], kp) > 0) {
            paths[j] = paths[j - 1];
            hashes[j] = hashes[j - 1];
            j--;
        }
        paths[j] = kp;
        hashes[j] = kh;
    }
}

/* ---- Dep-aware cache restore ---- */

NOW_API int now_cache_restore_ex(const char *source_key,
                                  const char *dst_path,
                                  const char *obj_ext) {
    if (!source_key || !dst_path) return -1;

    char *deps_path = now_cache_path(source_key, ".deps");
    if (!deps_path) return -1;

    if (!now_path_exists(deps_path)) {
        free(deps_path);
        return -1;  /* no deps sidecar → miss */
    }

    char *stored_rkey = NULL;
    char **dep_paths = NULL;
    char **dep_hashes = NULL;
    size_t dep_count = 0;

    if (read_deps_file(deps_path, &stored_rkey, &dep_paths, &dep_hashes,
                       &dep_count) != 0) {
        free(deps_path);
        return -1;
    }
    free(deps_path);

    /* Verify each dep still matches and collect current hashes */
    char **cur_hashes = (char **)calloc(dep_count, sizeof(char *));
    if (!cur_hashes && dep_count > 0) {
        free(stored_rkey);
        free_deps_arrays(dep_paths, dep_hashes, dep_count);
        return -1;
    }

    int valid = 1;
    for (size_t i = 0; i < dep_count; i++) {
        cur_hashes[i] = now_sha256_file(dep_paths[i]);
        if (!cur_hashes[i] ||
            strcmp(cur_hashes[i], dep_hashes[i]) != 0) {
            valid = 0;
            break;
        }
    }

    if (!valid) {
        for (size_t i = 0; i < dep_count; i++) free(cur_hashes[i]);
        free(cur_hashes);
        free(stored_rkey);
        free_deps_arrays(dep_paths, dep_hashes, dep_count);
        return -1;
    }

    /* Recompute result_key from source_key + current dep hashes (sorted) */
    char *result_key = compute_result_key(source_key,
                                           (const char **)dep_hashes,
                                           dep_count);

    for (size_t i = 0; i < dep_count; i++) free(cur_hashes[i]);
    free(cur_hashes);
    free_deps_arrays(dep_paths, dep_hashes, dep_count);

    if (!result_key || !stored_rkey ||
        strcmp(result_key, stored_rkey) != 0) {
        free(result_key);
        free(stored_rkey);
        return -1;
    }

    free(stored_rkey);

    /* Result key matches — restore the object */
    int rc = now_cache_restore(result_key, dst_path, obj_ext);
    free(result_key);
    return rc;
}

/* ---- Dep-aware cache store ---- */

NOW_API int now_cache_store_ex(const char *source_key,
                                const char *obj_path,
                                const char *obj_ext,
                                const NowDepList *deps) {
    if (!source_key || !obj_path) return -1;

    /* No deps → fall back to simple store */
    if (!deps || deps->count == 0)
        return now_cache_store(source_key, obj_path, obj_ext);

    /* Hash each dep file and build parallel arrays */
    size_t n = deps->count;
    char **sorted_paths = (char **)malloc(n * sizeof(char *));
    char **sorted_hashes = (char **)malloc(n * sizeof(char *));
    if (!sorted_paths || !sorted_hashes) {
        free(sorted_paths);
        free(sorted_hashes);
        return -1;
    }

    for (size_t i = 0; i < n; i++) {
        sorted_paths[i] = strdup(deps->paths[i]);
        sorted_hashes[i] = now_sha256_file(deps->paths[i]);
        if (!sorted_paths[i] || !sorted_hashes[i]) {
            /* Cleanup on error — fall back to simple store */
            for (size_t j = 0; j <= i; j++) {
                free(sorted_paths[j]);
                free(sorted_hashes[j]);
            }
            free(sorted_paths);
            free(sorted_hashes);
            return now_cache_store(source_key, obj_path, obj_ext);
        }
    }

    /* Sort by path for determinism */
    sort_deps(sorted_paths, sorted_hashes, n);

    /* Compute result_key */
    char *result_key = compute_result_key(source_key,
                                           (const char **)sorted_hashes, n);
    if (!result_key) {
        free_deps_arrays(sorted_paths, sorted_hashes, n);
        return now_cache_store(source_key, obj_path, obj_ext);
    }

    /* Store the object under result_key */
    int rc = now_cache_store(result_key, obj_path, obj_ext);
    if (rc != 0) {
        free(result_key);
        free_deps_arrays(sorted_paths, sorted_hashes, n);
        return rc;
    }

    /* Write .deps sidecar */
    char *deps_cache_path = now_cache_path(source_key, ".deps");
    if (deps_cache_path) {
        ensure_parent_dir(deps_cache_path);

        /* Write to temp, then rename */
        size_t dlen = strlen(deps_cache_path);
        char *tmp_path = (char *)malloc(dlen + 5);
        if (tmp_path) {
            memcpy(tmp_path, deps_cache_path, dlen);
            memcpy(tmp_path + dlen, ".tmp", 5);

            FILE *f = fopen(tmp_path, "w");
            if (f) {
                fprintf(f, "result:%s\n", result_key);
                for (size_t i = 0; i < n; i++)
                    fprintf(f, "%s\t%s\n", sorted_paths[i], sorted_hashes[i]);
                fclose(f);

#ifdef _WIN32
                _unlink(deps_cache_path);
#endif
                if (rename(tmp_path, deps_cache_path) != 0)
                    remove(tmp_path);
            }
            free(tmp_path);
        }
        free(deps_cache_path);
    }

    free(result_key);
    free_deps_arrays(sorted_paths, sorted_hashes, n);
    return 0;
}

/* ---- Cache statistics ---- */

NOW_API int now_cache_print_stats(void) {
    char *root = now_cache_root();
    if (!root) {
        printf("cache: not available (no home directory)\n");
        return -1;
    }

    if (!now_path_exists(root)) {
        printf("cache: empty (0 objects, 0 B)\n");
        free(root);
        return 0;
    }

    int count = 0;
    long long total_size = 0;
    walk_cache_dir(root, &count, &total_size);
    free(root);

    if (total_size < 1024)
        printf("cache: %d objects, %lld B\n", count, total_size);
    else if (total_size < 1024 * 1024)
        printf("cache: %d objects, %.1f KB\n", count, total_size / 1024.0);
    else if (total_size < 1024LL * 1024 * 1024)
        printf("cache: %d objects, %.1f MB\n", count, total_size / (1024.0 * 1024));
    else
        printf("cache: %d objects, %.1f GB\n", count,
               total_size / (1024.0 * 1024 * 1024));

    return 0;
}

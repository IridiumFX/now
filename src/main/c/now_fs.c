/*
 * now_fs.c — Filesystem utilities
 */
#include "now_fs.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>

#ifdef _WIN32
  #include <windows.h>
  #include <direct.h>
  #define mkdir_one(p) _mkdir(p)
  #ifndef S_ISDIR
    #define S_ISDIR(m) (((m) & _S_IFMT) == _S_IFDIR)
  #endif
#else
  #include <dirent.h>
  #include <unistd.h>
  #define mkdir_one(p) mkdir(p, 0755)
#endif

/* ---- Path utilities ---- */

NOW_API char *now_path_join(const char *a, const char *b) {
    if (!a || !*a) return b ? strdup(b) : NULL;
    if (!b || !*b) return strdup(a);

    size_t la = strlen(a);
    size_t lb = strlen(b);

    /* Check if a already ends with separator */
    int has_sep = (a[la - 1] == '/' || a[la - 1] == '\\');

    size_t total = la + lb + (has_sep ? 1 : 2);
    char *out = malloc(total);
    if (!out) return NULL;

    if (has_sep)
        snprintf(out, total, "%s%s", a, b);
    else
        snprintf(out, total, "%s/%s", a, b);

    return out;
}

NOW_API const char *now_path_ext(const char *path) {
    if (!path) return "";
    const char *dot = NULL;
    for (const char *p = path; *p; p++) {
        if (*p == '.') dot = p;
        else if (*p == '/' || *p == '\\') dot = NULL;
    }
    return dot ? dot : "";
}

NOW_API const char *now_path_basename(const char *path) {
    if (!path) return "";
    const char *last = path;
    for (const char *p = path; *p; p++) {
        if (*p == '/' || *p == '\\') last = p + 1;
    }
    return last;
}

NOW_API int now_path_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

NOW_API int now_is_dir(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    return S_ISDIR(st.st_mode);
}

NOW_API int now_mkdir_p(const char *path) {
    if (!path || !*path) return -1;

    char *tmp = strdup(path);
    if (!tmp) return -1;

    /* Normalize separators to / */
    for (char *p = tmp; *p; p++) {
        if (*p == '\\') *p = '/';
    }

    /* Skip drive letter on Windows (e.g. "C:/") */
    char *start = tmp;
#ifdef _WIN32
    if (start[0] && start[1] == ':' && start[2] == '/')
        start += 3;
    else
        start += 1;
#else
    start += 1;
#endif

    /* Create each component */
    for (char *p = start; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (!now_path_exists(tmp))
                mkdir_one(tmp);  /* ignore errors on intermediate dirs */
            *p = '/';
        }
    }
    if (!now_path_exists(tmp)) {
        if (mkdir_one(tmp) != 0) { free(tmp); return -1; }
    }

    free(tmp);
    return 0;
}

/* ---- Object path derivation (§3.2) ---- */

NOW_API char *now_obj_path_ex(const char *basedir, const char *src_path,
                      const char *src_root, const char *target,
                      const char *obj_ext) {
    /*
     * src/main/c/net/parser.c with src_root="src/main/c"
     * → relative = "net/parser.c"
     * → target/obj/main/net/parser.c.o   (or .obj for MSVC)
     */
    const char *relative = src_path;

    /* Strip src_root prefix if present */
    if (src_root) {
        size_t root_len = strlen(src_root);
        if (strncmp(src_path, src_root, root_len) == 0) {
            relative = src_path + root_len;
            while (*relative == '/' || *relative == '\\') relative++;
        }
    }

    /* Build: basedir/target/obj/main/<relative><obj_ext> */
    char *target_dir = now_path_join(basedir, target);
    char *obj_dir = now_path_join(target_dir, "obj/main");
    free(target_dir);

    char *rel_copy = strdup(relative);
    if (!rel_copy) { free(obj_dir); return NULL; }

    char *obj_base = now_path_join(obj_dir, rel_copy);
    free(rel_copy);
    free(obj_dir);

    size_t base_len = strlen(obj_base);
    size_t ext_len = strlen(obj_ext);
    char *result = malloc(base_len + ext_len + 1);
    if (!result) { free(obj_base); return NULL; }
    memcpy(result, obj_base, base_len);
    memcpy(result + base_len, obj_ext, ext_len + 1);
    free(obj_base);

    return result;
}

NOW_API char *now_obj_path(const char *basedir, const char *src_path,
                   const char *src_root, const char *target) {
    return now_obj_path_ex(basedir, src_path, src_root, target, ".o");
}

/* ---- File list ---- */

NOW_API void now_filelist_init(NowFileList *fl) {
    fl->paths = NULL;
    fl->count = 0;
    fl->capacity = 0;
}

NOW_API int now_filelist_push(NowFileList *fl, const char *path) {
    if (fl->count >= fl->capacity) {
        size_t new_cap = fl->capacity ? fl->capacity * 2 : 16;
        char **tmp = realloc(fl->paths, new_cap * sizeof(char *));
        if (!tmp) return -1;
        fl->paths = tmp;
        fl->capacity = new_cap;
    }
    fl->paths[fl->count] = strdup(path);
    if (!fl->paths[fl->count]) return -1;
    fl->count++;
    return 0;
}

NOW_API void now_filelist_free(NowFileList *fl) {
    for (size_t i = 0; i < fl->count; i++)
        free(fl->paths[i]);
    free(fl->paths);
    now_filelist_init(fl);
}

/* ---- Source discovery ---- */

static int ext_matches(const char *path, const char **exts) {
    const char *ext = now_path_ext(path);
    for (const char **e = exts; *e; e++) {
        if (strcmp(ext, *e) == 0) return 1;
    }
    return 0;
}

#include "now_dirwalk.h"

/* Canonicalize a path for use as cache key. Falls back to the input on error. */
static void canonicalize_into(char *out, size_t outcap, const char *path) {
#ifdef _WIN32
    if (!_fullpath(out, path, outcap)) {
        strncpy(out, path, outcap - 1);
        out[outcap - 1] = '\0';
    }
#else
    if (!realpath(path, out)) {
        strncpy(out, path, outcap - 1);
        out[outcap - 1] = '\0';
    }
#endif
}

/* Walk a directory using the global dirwalk cache if available.
 * Writes matching source files to `out` (recursively through subdirs).
 * Updates the cache on miss/mtime mismatch. */
static int discover_recursive(const char *basedir, const char *rel_dir,
                               const char **exts, NowFileList *out) {
    char *abs_dir;
    if (rel_dir && *rel_dir)
        abs_dir = now_path_join(basedir, rel_dir);
    else
        abs_dir = strdup(basedir);
    if (!abs_dir) return 0;

    /* Get current mtime */
    struct stat st;
    long long cur_mtime = -1;
    if (stat(abs_dir, &st) == 0) cur_mtime = (long long)st.st_mtime;

    /* Try cache */
    char canon[1024];
    canonicalize_into(canon, sizeof(canon), abs_dir);

    const NowDirCacheEntry *cached =
        now_dirwalk_cache_global
            ? now_dirwalk_get(now_dirwalk_cache_global, canon, cur_mtime)
            : NULL;

    if (cached) {
        /* Replay from cache — no readdir */
        for (size_t i = 0; i < cached->count; i++) {
            const char *name = cached->entries[i];
            char *rel_path = (rel_dir && *rel_dir)
                ? now_path_join(rel_dir, name)
                : strdup(name);
            if (!rel_path) continue;
            if (cached->is_dir[i]) {
                discover_recursive(basedir, rel_path, exts, out);
            } else if (ext_matches(name, exts)) {
                now_filelist_push(out, rel_path);
            }
            free(rel_path);
        }
        free(abs_dir);
        return 0;
    }

    /* Miss — full readdir, collect entries for both traversal and cache */
    char **entries_list = NULL;
    int   *isdir_list   = NULL;
    size_t entries_count = 0, entries_cap = 0;

#ifdef _WIN32
    char *pattern = now_path_join(abs_dir, "*");
    WIN32_FIND_DATAA fd;
    HANDLE hFind = pattern ? FindFirstFileA(pattern, &fd) : INVALID_HANDLE_VALUE;
    free(pattern);
    if (hFind == INVALID_HANDLE_VALUE) { free(abs_dir); return 0; }
    do {
        if (fd.cFileName[0] == '.') continue;
        int is_dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? 1 : 0;
        if (entries_count >= entries_cap) {
            size_t newcap = entries_cap ? entries_cap * 2 : 16;
            char **ne = (char **)realloc(entries_list, newcap * sizeof(char *));
            int   *id = (int *)realloc(isdir_list,   newcap * sizeof(int));
            if (!ne || !id) { free(ne ? ne : entries_list); free(id ? id : isdir_list); FindClose(hFind); free(abs_dir); return 0; }
            entries_list = ne; isdir_list = id; entries_cap = newcap;
        }
        entries_list[entries_count] = strdup(fd.cFileName);
        isdir_list[entries_count]   = is_dir;
        entries_count++;
    } while (FindNextFileA(hFind, &fd));
    FindClose(hFind);
#else
    DIR *d = opendir(abs_dir);
    if (!d) { free(abs_dir); return 0; }
    struct dirent *entry;
    while ((entry = readdir(d)) != NULL) {
        if (entry->d_name[0] == '.') continue;
        char *full = now_path_join(abs_dir, entry->d_name);
        int is_dir = full ? now_is_dir(full) : 0;
        free(full);
        if (entries_count >= entries_cap) {
            size_t newcap = entries_cap ? entries_cap * 2 : 16;
            char **ne = (char **)realloc(entries_list, newcap * sizeof(char *));
            int   *id = (int *)realloc(isdir_list,   newcap * sizeof(int));
            if (!ne || !id) { free(ne ? ne : entries_list); free(id ? id : isdir_list); closedir(d); free(abs_dir); return 0; }
            entries_list = ne; isdir_list = id; entries_cap = newcap;
        }
        entries_list[entries_count] = strdup(entry->d_name);
        isdir_list[entries_count]   = is_dir;
        entries_count++;
    }
    closedir(d);
#endif
    free(abs_dir);

    /* Traverse using the collected list */
    for (size_t i = 0; i < entries_count; i++) {
        char *rel_path = (rel_dir && *rel_dir)
            ? now_path_join(rel_dir, entries_list[i])
            : strdup(entries_list[i]);
        if (!rel_path) continue;
        if (isdir_list[i]) {
            discover_recursive(basedir, rel_path, exts, out);
        } else if (ext_matches(entries_list[i], exts)) {
            now_filelist_push(out, rel_path);
        }
        free(rel_path);
    }

    /* Update cache — transfers ownership of entries_list[] and isdir_list[] */
    if (now_dirwalk_cache_global && cur_mtime >= 0) {
        now_dirwalk_put(now_dirwalk_cache_global, canon, cur_mtime,
                         entries_list, isdir_list, entries_count);
    } else {
        for (size_t i = 0; i < entries_count; i++) free(entries_list[i]);
        free(entries_list);
        free(isdir_list);
    }
    return 0;
}

NOW_API int now_discover_sources(const char *basedir, const char *dir,
                         const char **exts, NowFileList *out) {
    /* dir is relative to basedir */
    char *full = now_path_join(basedir, dir);
    if (!now_is_dir(full)) { free(full); return -1; }
    free(full);
    return discover_recursive(basedir, dir, exts, out);
}

NOW_API int now_file_copy(const char *src, const char *dst) {
    if (!src || !dst) return -1;
    FILE *in = fopen(src, "rb");
    if (!in) return -1;
    FILE *out = fopen(dst, "wb");
    if (!out) { fclose(in); return -1; }

    char buf[8192];
    size_t n;
    int err = 0;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) { err = -1; break; }
    }

    fclose(out);
    fclose(in);
    return err;
}

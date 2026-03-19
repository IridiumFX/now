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

#ifdef _WIN32

/* Windows: recursive directory walk using FindFirstFile/FindNextFile */
static int discover_recursive(const char *basedir, const char *rel_dir,
                               const char **exts, NowFileList *out) {
    char *search_dir;
    if (rel_dir && *rel_dir)
        search_dir = now_path_join(basedir, rel_dir);
    else
        search_dir = strdup(basedir);

    char *pattern = now_path_join(search_dir, "*");
    free(search_dir);

    WIN32_FIND_DATAA fd;
    HANDLE hFind = FindFirstFileA(pattern, &fd);
    free(pattern);

    if (hFind == INVALID_HANDLE_VALUE) return 0;  /* empty dir is OK */

    do {
        if (fd.cFileName[0] == '.') continue;

        char *rel_path;
        if (rel_dir && *rel_dir)
            rel_path = now_path_join(rel_dir, fd.cFileName);
        else
            rel_path = strdup(fd.cFileName);

        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            discover_recursive(basedir, rel_path, exts, out);
        } else if (ext_matches(fd.cFileName, exts)) {
            now_filelist_push(out, rel_path);
        }
        free(rel_path);
    } while (FindNextFileA(hFind, &fd));

    FindClose(hFind);
    return 0;
}

#else

/* POSIX: recursive directory walk using opendir/readdir */
static int discover_recursive(const char *basedir, const char *rel_dir,
                               const char **exts, NowFileList *out) {
    char *abs_dir;
    if (rel_dir && *rel_dir)
        abs_dir = now_path_join(basedir, rel_dir);
    else
        abs_dir = strdup(basedir);

    DIR *d = opendir(abs_dir);
    free(abs_dir);
    if (!d) return 0;

    struct dirent *entry;
    while ((entry = readdir(d)) != NULL) {
        if (entry->d_name[0] == '.') continue;

        char *rel_path;
        if (rel_dir && *rel_dir)
            rel_path = now_path_join(rel_dir, entry->d_name);
        else
            rel_path = strdup(entry->d_name);

        char *full = now_path_join(basedir, rel_path);
        if (now_is_dir(full)) {
            discover_recursive(basedir, rel_path, exts, out);
        } else if (ext_matches(entry->d_name, exts)) {
            now_filelist_push(out, rel_path);
        }
        free(full);
        free(rel_path);
    }
    closedir(d);
    return 0;
}

#endif

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

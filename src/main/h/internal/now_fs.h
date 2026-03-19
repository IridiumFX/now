/*
 * now_fs.h — Filesystem utilities
 *
 * Path manipulation, directory creation, source file discovery.
 */
#ifndef NOW_FS_H
#define NOW_FS_H

#include <stddef.h>
#include "now.h"  /* for NOW_API */

/* Path separator for the host platform */
#ifdef _WIN32
  #define NOW_SEP '\\'
  #define NOW_SEP_STR "\\"
#else
  #define NOW_SEP '/'
  #define NOW_SEP_STR "/"
#endif

NOW_API char       *now_path_join(const char *a, const char *b);
NOW_API const char *now_path_ext(const char *path);
NOW_API const char *now_path_basename(const char *path);
NOW_API int         now_mkdir_p(const char *path);
NOW_API int         now_path_exists(const char *path);
NOW_API int         now_is_dir(const char *path);

NOW_API char *now_obj_path(const char *basedir, const char *src_path,
                            const char *src_root, const char *target);
NOW_API char *now_obj_path_ex(const char *basedir, const char *src_path,
                              const char *src_root, const char *target,
                              const char *obj_ext);

/* Dynamic file list */
typedef struct {
    char  **paths;
    size_t  count;
    size_t  capacity;
} NowFileList;

NOW_API void now_filelist_init(NowFileList *fl);
NOW_API int  now_filelist_push(NowFileList *fl, const char *path);
NOW_API void now_filelist_free(NowFileList *fl);

NOW_API int now_discover_sources(const char *basedir, const char *dir,
                                  const char **exts, NowFileList *out);

/* Copy a file from src to dst. Returns 0 on success, -1 on error. */
NOW_API int now_file_copy(const char *src, const char *dst);

#endif /* NOW_FS_H */

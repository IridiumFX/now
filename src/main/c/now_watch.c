/*
 * now_watch.c — File watcher for incremental rebuilds
 *
 * Polls source directories for mtime changes and triggers rebuilds.
 * Debounces rapid edits (editors do save-rename-rename sequences).
 */
#include "now_watch.h"
#include "now_fs.h"
#include "now_lang.h"
#include "now_audit.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/stat.h>
#include <time.h>

#ifdef _WIN32
  #include <windows.h>
#else
  #include <unistd.h>
#endif

/* ---- Signal handling ---- */

static volatile sig_atomic_t g_watch_running = 1;

static void watch_signal_handler(int sig) {
    (void)sig;
    g_watch_running = 0;
}

#ifdef _WIN32
static BOOL WINAPI watch_ctrl_handler(DWORD type) {
    (void)type;
    g_watch_running = 0;
    return TRUE;
}
#endif

static void watch_sleep(int ms) {
#ifdef _WIN32
    Sleep((DWORD)ms);
#else
    usleep((useconds_t)ms * 1000);
#endif
}

/* ---- Options ---- */

NOW_API void now_watch_opts_init(NowWatchOpts *opts) {
    if (!opts) return;
    opts->verbose = 0;
    opts->jobs = 0;
    opts->poll_ms = 500;
}

/* ---- Snapshot ---- */

static void snap_push(NowWatchSnapshot *snap, const char *path, long long mtime) {
    if (snap->count >= snap->capacity) {
        size_t newcap = snap->capacity ? snap->capacity * 2 : 64;
        NowWatchEntry *ne = (NowWatchEntry *)realloc(snap->entries,
                                                       newcap * sizeof(NowWatchEntry));
        if (!ne) return;
        snap->entries = ne;
        snap->capacity = newcap;
    }
    snap->entries[snap->count].path = strdup(path);
    snap->entries[snap->count].mtime = mtime;
    snap->count++;
}

static int entry_cmp(const void *a, const void *b) {
    return strcmp(((const NowWatchEntry *)a)->path,
                  ((const NowWatchEntry *)b)->path);
}

/* Discover and stat all files with given extensions in a directory */
static void snap_dir(NowWatchSnapshot *snap, const char *basedir,
                      const char *dir, const char **exts) {
    if (!dir || !dir[0]) return;

    NowFileList fl;
    now_filelist_init(&fl);
    if (now_discover_sources(basedir, dir, exts, &fl) == 0) {
        for (size_t i = 0; i < fl.count; i++) {
            char *full = now_path_join(basedir, fl.paths[i]);
            if (full) {
                struct stat st;
                if (stat(full, &st) == 0)
                    snap_push(snap, fl.paths[i], (long long)st.st_mtime);
                free(full);
            }
        }
    }
    now_filelist_free(&fl);
}

NOW_API int now_watch_snapshot(const NowProject *project,
                                const char *basedir,
                                NowWatchSnapshot *snap) {
    if (!project || !basedir || !snap) return -1;
    memset(snap, 0, sizeof(*snap));

    /* Source + header extensions to watch */
    static const char *all_exts[] = {
        ".c", ".h", ".cpp", ".hpp", ".cc", ".cxx",
        ".cppm", ".ixx", ".ccm", ".java", ".rs",
        ".s", ".S", ".asm", NULL
    };

    /* Watch source dir */
    if (project->sources.dir)
        snap_dir(snap, basedir, project->sources.dir, all_exts);

    /* Watch public headers */
    if (project->sources.headers)
        snap_dir(snap, basedir, project->sources.headers, all_exts);

    /* Watch private headers */
    if (project->sources.private_headers)
        snap_dir(snap, basedir, project->sources.private_headers, all_exts);

    /* Sort for stable diffing */
    if (snap->count > 1)
        qsort(snap->entries, snap->count, sizeof(NowWatchEntry), entry_cmp);

    /* Stat now.pasta */
    {
        char *pasta_path = now_path_join(basedir, "now.pasta");
        if (pasta_path) {
            struct stat st;
            if (stat(pasta_path, &st) == 0)
                snap->pasta_mtime = (long long)st.st_mtime;
            free(pasta_path);
        }
    }

    return 0;
}

NOW_API int now_watch_diff(const NowWatchSnapshot *old,
                            const NowWatchSnapshot *cur) {
    if (!old || !cur) return 0;

    int flags = 0;

    /* Check now.pasta */
    if (cur->pasta_mtime != old->pasta_mtime)
        flags |= 2;

    /* Compare file lists */
    if (old->count != cur->count) {
        flags |= 1;
    } else {
        for (size_t i = 0; i < old->count; i++) {
            if (strcmp(old->entries[i].path, cur->entries[i].path) != 0 ||
                old->entries[i].mtime != cur->entries[i].mtime) {
                flags |= 1;
                break;
            }
        }
    }

    return flags;
}

NOW_API void now_watch_snapshot_free(NowWatchSnapshot *snap) {
    if (!snap) return;
    for (size_t i = 0; i < snap->count; i++)
        free(snap->entries[i].path);
    free(snap->entries);
    memset(snap, 0, sizeof(*snap));
}

/* ---- Watch loop ---- */

NOW_API int now_watch(const NowProject *project, const char *basedir,
                      const NowWatchOpts *opts, NowResult *result) {
    if (!project || !basedir || !opts || !result) return -1;

    int poll_ms = opts->poll_ms > 0 ? opts->poll_ms : 500;

    /* Install signal handlers */
    g_watch_running = 1;
    signal(SIGINT, watch_signal_handler);
    signal(SIGTERM, watch_signal_handler);
#ifdef _WIN32
    SetConsoleCtrlHandler(watch_ctrl_handler, TRUE);
#endif

    /* Initial build */
    fprintf(stderr, "[watch] initial build...\n");
    int rc = now_build(project, basedir, opts->verbose, opts->jobs, result);
    if (rc != 0)
        fprintf(stderr, "[watch] build failed: %s\n", result->message);
    else
        fprintf(stderr, "[watch] build ok, watching for changes...\n");

    /* Take initial snapshot */
    NowWatchSnapshot prev;
    now_watch_snapshot(project, basedir, &prev);

    int builds = 0;

    while (g_watch_running) {
        watch_sleep(poll_ms);
        if (!g_watch_running) break;

        NowWatchSnapshot cur;
        now_watch_snapshot(project, basedir, &cur);

        int diff = now_watch_diff(&prev, &cur);
        if (diff == 0) {
            now_watch_snapshot_free(&cur);
            continue;
        }

        /* Debounce: wait one more interval, check again */
        watch_sleep(poll_ms);
        if (!g_watch_running) { now_watch_snapshot_free(&cur); break; }

        NowWatchSnapshot debounced;
        now_watch_snapshot(project, basedir, &debounced);
        int diff2 = now_watch_diff(&cur, &debounced);
        now_watch_snapshot_free(&cur);

        /* Use the debounced snapshot as current */
        /* If still changing, the next poll iteration will catch it */

        if (diff & 2) {
            /* Project file changed — signal caller to reload */
            now_watch_snapshot_free(&debounced);
            now_watch_snapshot_free(&prev);
            result->code = NOW_ERR_SCHEMA;
            snprintf(result->message, sizeof(result->message),
                     "now.pasta changed — reload required");
            signal(SIGINT, SIG_DFL);
            signal(SIGTERM, SIG_DFL);
#ifdef _WIN32
            SetConsoleCtrlHandler(watch_ctrl_handler, FALSE);
#endif
            return NOW_ERR_SCHEMA;
        }

        /* Source changed — rebuild */
        fprintf(stderr, "[watch] change detected, rebuilding...\n");
        memset(result, 0, sizeof(*result));
        rc = now_build(project, basedir, opts->verbose, opts->jobs, result);
        builds++;

        if (rc != 0)
            fprintf(stderr, "[watch] build failed: %s\n", result->message);
        else
            fprintf(stderr, "[watch] build ok (%d rebuild%s)\n",
                    builds, builds == 1 ? "" : "s");

        now_audit_record(NOW_AUDIT_BUILD, "local",
                          project->group && project->artifact
                              ? project->artifact : basedir,
                          rc == 0 ? "ok" : "error",
                          rc != 0 ? result->message : "watch");

        now_watch_snapshot_free(&prev);
        prev = debounced;

        (void)diff2;
    }

    now_watch_snapshot_free(&prev);

    /* Restore signal handlers */
    signal(SIGINT, SIG_DFL);
    signal(SIGTERM, SIG_DFL);
#ifdef _WIN32
    SetConsoleCtrlHandler(watch_ctrl_handler, FALSE);
#endif

    fprintf(stderr, "[watch] stopped.\n");
    return 0;
}

/*
 * now_tui.h — Terminal UI for build progress
 *
 * ANSI escape-based live display showing compilation progress,
 * timing, errors, and cache stats. No external dependencies.
 * Works on Windows 10+ and POSIX terminals.
 */
#ifndef NOW_TUI_H
#define NOW_TUI_H

#include "now.h"
#include <stddef.h>

/* TUI state — tracks build progress for live display */
typedef struct {
    int    enabled;         /* 1 if TUI is active */
    int    total_files;     /* total files to compile */
    int    compiled;        /* files compiled so far */
    int    skipped;         /* files skipped (up to date) */
    int    cache_hits;      /* local cache hits */
    int    remote_hits;     /* remote cache hits */
    int    errors;          /* compilation errors */
    int    warnings;        /* compilation warnings */
    int    jobs;            /* parallel job count */
    double start_time;      /* build start time (seconds) */
    const char *project;    /* project name */
    const char *current;    /* currently compiling file (last started) */
    int    term_width;      /* terminal width */
    int    phase;           /* 0=compile, 1=link, 2=done */
} NowTui;

/* Initialize TUI. Enables ANSI on Windows, detects terminal width. */
NOW_API void now_tui_init(NowTui *tui, const char *project, int total, int jobs);

/* Update display with current progress. */
NOW_API void now_tui_update(NowTui *tui);

/* Report a file compilation start. */
NOW_API void now_tui_compile_start(NowTui *tui, const char *source);

/* Report a file compilation done (ok, cached, skipped, or error). */
NOW_API void now_tui_compile_done(NowTui *tui, const char *source, int ok);

/* Report a cache hit. */
NOW_API void now_tui_cache_hit(NowTui *tui, int remote);

/* Report a skip (up to date). */
NOW_API void now_tui_skip(NowTui *tui);

/* Report entering link phase. */
NOW_API void now_tui_link(NowTui *tui);

/* Report an error message (displayed in error log area). */
NOW_API void now_tui_error(NowTui *tui, const char *msg);

/* Finalize TUI — print summary, restore terminal. */
NOW_API void now_tui_finish(NowTui *tui, int success);

#endif /* NOW_TUI_H */

/*
 * now_tui.c — Terminal UI for build progress
 *
 * ANSI escape-based live display. Zero external dependencies.
 */
#include "now_tui.h"

NOW_API NowTui *now_tui_global = NULL;

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
  #include <windows.h>
#else
  #include <sys/ioctl.h>
  #include <unistd.h>
#endif

/* ---- ANSI escape codes ---- */
#define ESC         "\033["
#define RESET       ESC "0m"
#define BOLD        ESC "1m"
#define DIM         ESC "2m"
#define GREEN       ESC "32m"
#define YELLOW      ESC "33m"
#define RED         ESC "31m"
#define CYAN        ESC "36m"
#define BLUE        ESC "34m"
#define CLEAR_LINE  ESC "2K"
#define CURSOR_UP   ESC "1A"
#define HIDE_CURSOR ESC "?25l"
#define SHOW_CURSOR ESC "?25h"

/* ---- Platform helpers ---- */

static int get_term_width(void) {
#ifdef _WIN32
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    if (GetConsoleScreenBufferInfo(GetStdHandle(STD_ERROR_HANDLE), &csbi))
        return csbi.srWindow.Right - csbi.srWindow.Left + 1;
    return 80;
#else
    struct winsize ws;
    if (ioctl(STDERR_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0)
        return ws.ws_col;
    return 80;
#endif
}

static void enable_ansi(void) {
#ifdef _WIN32
    HANDLE h = GetStdHandle(STD_ERROR_HANDLE);
    DWORD mode = 0;
    GetConsoleMode(h, &mode);
    SetConsoleMode(h, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
#endif
}

static double now_seconds(void) {
    return (double)clock() / CLOCKS_PER_SEC;
}

/* ---- Progress bar ---- */

static void draw_bar(FILE *f, int done, int total, int width) {
    if (total <= 0 || width < 10) return;
    int bar_width = width - 2;  /* subtract [ and ] */
    int filled = (done * bar_width) / total;
    if (filled > bar_width) filled = bar_width;

    fputc('[', f);
    for (int i = 0; i < bar_width; i++) {
        if (i < filled) fputc('#', f);
        else fputc('.', f);
    }
    fputc(']', f);
}

/* ---- Public API ---- */

NOW_API void now_tui_init(NowTui *tui, const char *project, int total, int jobs) {
    if (!tui) return;
    memset(tui, 0, sizeof(*tui));
    tui->enabled = 1;
    tui->total_files = total;
    tui->jobs = jobs;
    tui->project = project;
    tui->start_time = now_seconds();
    tui->term_width = get_term_width();
    if (tui->term_width < 40) tui->term_width = 40;

    enable_ansi();
    fprintf(stderr, HIDE_CURSOR);

    /* Print header */
    fprintf(stderr, BOLD CYAN " now " RESET BOLD "%s" RESET DIM " (%d files, %d-way parallel)" RESET "\n",
            project ? project : "", total, jobs);
    fprintf(stderr, "\n\n");  /* reserve 2 lines for progress + status */
}

NOW_API void now_tui_update(NowTui *tui) {
    if (!tui || !tui->enabled) return;

    double elapsed = now_seconds() - tui->start_time;
    int done = tui->compiled + tui->skipped + tui->cache_hits + tui->remote_hits;
    int pct = tui->total_files > 0 ? (done * 100) / tui->total_files : 0;

    /* Move up 2 lines and redraw */
    fprintf(stderr, CURSOR_UP CURSOR_UP);

    /* Line 1: progress bar + percentage */
    fprintf(stderr, CLEAR_LINE " ");
    int bar_w = tui->term_width - 20;
    if (bar_w > 60) bar_w = 60;
    if (bar_w < 10) bar_w = 10;

    if (tui->phase == 0) {
        fprintf(stderr, GREEN);
        draw_bar(stderr, done, tui->total_files, bar_w);
        fprintf(stderr, RESET " %3d%% ", pct);
        fprintf(stderr, DIM "%.1fs" RESET, elapsed);
    } else if (tui->phase == 1) {
        fprintf(stderr, BLUE);
        draw_bar(stderr, tui->total_files, tui->total_files, bar_w);
        fprintf(stderr, RESET BOLD " linking " RESET DIM "%.1fs" RESET, elapsed);
    } else {
        fprintf(stderr, GREEN);
        draw_bar(stderr, tui->total_files, tui->total_files, bar_w);
        fprintf(stderr, RESET BOLD GREEN " done " RESET DIM "%.1fs" RESET, elapsed);
    }
    fprintf(stderr, "\n");

    /* Line 2: status details */
    fprintf(stderr, CLEAR_LINE " ");
    if (tui->compiled > 0)
        fprintf(stderr, GREEN "%d compiled " RESET, tui->compiled);
    if (tui->cache_hits > 0)
        fprintf(stderr, CYAN "%d cached " RESET, tui->cache_hits);
    if (tui->remote_hits > 0)
        fprintf(stderr, BLUE "%d remote " RESET, tui->remote_hits);
    if (tui->skipped > 0)
        fprintf(stderr, DIM "%d skipped " RESET, tui->skipped);
    if (tui->errors > 0)
        fprintf(stderr, RED "%d errors " RESET, tui->errors);

    if (tui->current && tui->phase == 0) {
        /* Show current file (truncated) */
        const char *basename = strrchr(tui->current, '/');
        if (!basename) basename = strrchr(tui->current, '\\');
        if (basename) basename++; else basename = tui->current;
        int max = tui->term_width - 50;
        if (max > 0)
            fprintf(stderr, DIM "| %.*s" RESET, max, basename);
    }
    fprintf(stderr, "\n");
    fflush(stderr);
}

NOW_API void now_tui_compile_start(NowTui *tui, const char *source) {
    if (!tui || !tui->enabled) return;
    tui->current = source;
    now_tui_update(tui);
}

NOW_API void now_tui_compile_done(NowTui *tui, const char *source, int ok) {
    if (!tui || !tui->enabled) return;
    (void)source;
    if (ok)
        tui->compiled++;
    else
        tui->errors++;
    now_tui_update(tui);
}

NOW_API void now_tui_cache_hit(NowTui *tui, int remote) {
    if (!tui || !tui->enabled) return;
    if (remote) tui->remote_hits++;
    else tui->cache_hits++;
    now_tui_update(tui);
}

NOW_API void now_tui_skip(NowTui *tui) {
    if (!tui || !tui->enabled) return;
    tui->skipped++;
    now_tui_update(tui);
}

NOW_API void now_tui_link(NowTui *tui) {
    if (!tui || !tui->enabled) return;
    tui->phase = 1;
    tui->current = NULL;
    now_tui_update(tui);
}

NOW_API void now_tui_error(NowTui *tui, const char *msg) {
    if (!tui || !tui->enabled || !msg) return;
    /* Print error above the progress area */
    fprintf(stderr, CURSOR_UP CURSOR_UP);
    fprintf(stderr, RED "  error: %s" RESET "\n\n\n", msg);
    now_tui_update(tui);
}

NOW_API void now_tui_finish(NowTui *tui, int success) {
    if (!tui || !tui->enabled) return;
    tui->phase = 2;
    now_tui_update(tui);

    double elapsed = now_seconds() - tui->start_time;

    /* Final summary line below the progress */
    if (success) {
        fprintf(stderr, "\n" BOLD GREEN " ok" RESET " %d compiled",
                tui->compiled);
        if (tui->cache_hits > 0) fprintf(stderr, ", %d cached", tui->cache_hits);
        if (tui->remote_hits > 0) fprintf(stderr, ", %d remote", tui->remote_hits);
        if (tui->skipped > 0) fprintf(stderr, ", %d skipped", tui->skipped);
        fprintf(stderr, " in %.1fs\n", elapsed);
    } else {
        fprintf(stderr, "\n" BOLD RED " build failed" RESET " (%d errors)\n",
                tui->errors);
    }

    fprintf(stderr, SHOW_CURSOR);
    tui->enabled = 0;
}

/*
 * now_build.h — Build and link phases
 *
 * Compiler/linker invocation, toolchain resolution, target directory setup.
 */
#ifndef NOW_BUILD_H
#define NOW_BUILD_H

#include "now_pom.h"
#include "now.h"
#include "now_fs.h"
#include "now_lang.h"

/* Resolved toolchain — concrete paths to tools */
typedef struct {
    char *cc;      /* C compiler */
    char *cxx;     /* C++ compiler */
    char *ar;      /* archiver */
    char *as;      /* assembler */
    char *ld;      /* linker (optional, usually NULL = use cc/cxx) */
    int   is_msvc; /* true if this is an MSVC toolchain */
    char *javac;   /* Java compiler */
    char *jar;     /* JAR tool */
    char *java;    /* Java runtime */
    char *rustc;   /* Rust compiler */
    char *go;      /* Go toolchain */
    char *julia;   /* Julia runtime (for embedding) */
} NowToolchain;

/* Initialize toolchain from environment / defaults */
NOW_API void now_toolchain_resolve(NowToolchain *tc, const NowProject *p);
NOW_API void now_toolchain_free(NowToolchain *tc);

/* Detect the number of logical CPU cores on the host.
 * Returns at least 1 even if detection fails. */
NOW_API int now_cpu_count(void);

/* Per-phase wall-clock instrumentation. Toggled by --timing on the CLI;
 * silent when disabled. now_timing_mark() prints the elapsed seconds
 * since the previous mark (or since now_timing_begin()) prefixed with
 * the given label, and resets the running anchor. */
NOW_API void   now_timing_set(int enabled);
NOW_API int    now_timing_enabled(void);
NOW_API double now_clock_secs(void);
NOW_API void   now_timing_begin(void);
NOW_API void   now_timing_mark(const char *label);

/* Build context — everything needed for a build */
typedef struct {
    const NowProject *project;
    const char       *basedir;     /* project root (absolute) */
    const char       *target_dir;  /* "target" */
    NowToolchain      toolchain;
    NowFileList       sources;     /* discovered source files */
    NowFileList       objects;     /* produced object files */
    NowFileList       dep_includes;/* -I paths from resolved deps */
    NowFileList       dep_libdirs; /* -L paths from resolved deps */
    NowFileList       dep_libs;    /* -l names from resolved deps */
    /* Raw lib dirs (no -L prefix). Used at test-run time on Windows to
     * stage *.dll files next to the test binary, since Windows has no
     * RPATH and the loader's search rules check the binary's own
     * directory first. POSIX gets RPATH at link time instead. */
    NowFileList       dep_lib_dirs_raw;
    NowStatCache      stat_cache;  /* memoize header-dep stat() per build */
    int               verbose;
    int               jobs;        /* max parallel jobs (0 = auto) */
    int               timing;      /* --timing: emit per-phase wall-clock */
} NowBuildCtx;

/* Initialize the build context and discover sources.
 * Returns 0 on success, -1 on error (fills result). */
/* NowResult defined in now.h */
NOW_API int now_build_init(NowBuildCtx *ctx, const NowProject *project,
                          const char *basedir, NowResult *result);

/* Run the build phase: compile all source files to objects.
 * Returns 0 on success, non-zero on compiler error. */
NOW_API int now_build_compile(NowBuildCtx *ctx, NowResult *result);

/* Run the link phase: link objects into final output.
 * Returns 0 on success, non-zero on linker error. */
NOW_API int now_build_link(NowBuildCtx *ctx, NowResult *result);

/* Run the test phase: compile test sources, link into test binary,
 * execute and check exit code. Returns 0 on success. */
NOW_API int now_build_test(NowBuildCtx *ctx, NowResult *result);

/* Clean up build context. */
NOW_API void now_build_free(NowBuildCtx *ctx);

/* Generate compile_commands.json at project root.
 * Returns the number of entries written, or -1 on error. */
NOW_API int now_compile_db(const NowProject *project, const char *basedir,
                            NowResult *result);

/* Run a subprocess and capture exit code. Returns exit code. */
NOW_API int now_exec(const char *const *argv, int verbose);

#endif /* NOW_BUILD_H */

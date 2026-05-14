/*
 * now.c — Public API implementation
 */
#include "now_pom.h"
#include "now_build.h"
#include "now_procure.h"
#include "now_plugin.h"
#include "now_version.h"
#include "now_fs.h"
#include "now_arch.h"
#include "now_tui.h"
#include "now.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

NOW_API const char *now_version(void) {
    return "1.0.0-rc2";
}

/* ---- Project accessors ---- */

NOW_API const char *now_project_group(const NowProject *p)    { return p ? p->group : NULL; }
NOW_API const char *now_project_artifact(const NowProject *p) { return p ? p->artifact : NULL; }
NOW_API const char *now_project_version(const NowProject *p)  { return p ? p->version : NULL; }
NOW_API const char *now_project_name(const NowProject *p)     { return p ? p->name : NULL; }
NOW_API const char *now_project_license(const NowProject *p)  { return p ? p->license : NULL; }
NOW_API const char *now_project_std(const NowProject *p)      { return p ? p->std : NULL; }

NOW_API size_t now_project_lang_count(const NowProject *p) {
    return p ? p->langs.count : 0;
}

NOW_API const char *now_project_lang(const NowProject *p, size_t index) {
    if (!p || index >= p->langs.count) return NULL;
    return p->langs.items[index];
}

NOW_API const char *now_project_output_type(const NowProject *p) {
    return p ? p->output.type : NULL;
}

NOW_API const char *now_project_output_name(const NowProject *p) {
    return p ? p->output.name : NULL;
}

NOW_API const char *now_project_source_dir(const NowProject *p) {
    return p ? p->sources.dir : NULL;
}

NOW_API const char *now_project_header_dir(const NowProject *p) {
    return p ? p->sources.headers : NULL;
}

NOW_API const char *now_project_test_dir(const NowProject *p) {
    return p ? p->tests.dir : NULL;
}

NOW_API size_t now_project_dep_count(const NowProject *p) {
    return p ? p->deps.count : 0;
}

NOW_API const char *now_project_dep_id(const NowProject *p, size_t index) {
    if (!p || index >= p->deps.count) return NULL;
    return p->deps.items[index].id;
}

NOW_API const char *now_project_dep_scope(const NowProject *p, size_t index) {
    if (!p || index >= p->deps.count) return NULL;
    return p->deps.items[index].scope;
}

NOW_API size_t now_project_warning_count(const NowProject *p) {
    return p ? p->compile.warnings.count : 0;
}

NOW_API const char *now_project_warning(const NowProject *p, size_t index) {
    if (!p || index >= p->compile.warnings.count) return NULL;
    return p->compile.warnings.items[index];
}

NOW_API size_t now_project_define_count(const NowProject *p) {
    return p ? p->compile.defines.count : 0;
}

NOW_API const char *now_project_define(const NowProject *p, size_t index) {
    if (!p || index >= p->compile.defines.count) return NULL;
    return p->compile.defines.items[index];
}

NOW_API const char *now_project_opt(const NowProject *p) {
    return p ? p->compile.opt : NULL;
}

NOW_API const char *now_project_convergence(const NowProject *p) {
    return p ? p->convergence : NULL;
}

/* ---- Procure + dep path injection ---- */

/* Run procure phase and populate build context dep paths.
 * Returns 0 on success. If project has no deps, this is a no-op.
 *
 * Even if procure fails (e.g. registry unreachable), we still try to
 * inject paths from the local repo — a previously-installed dep
 * should remain usable while offline. */
static int procure_and_inject_deps(const NowProject *project,
                                    NowBuildCtx *ctx, NowResult *result) {
    if (!project || project->deps.count == 0)
        return 0;

    /* Run procure (non-fatal — locally-installed deps still work) */
    NowProcureOpts opts = {0};
    NowResult procure_res;
    memset(&procure_res, 0, sizeof(procure_res));
    (void)now_procure(project, &opts, &procure_res);

    /* Determine repo root */
    const char *home = NULL;
#ifdef _WIN32
    home = getenv("USERPROFILE");
    if (!home) home = getenv("HOME");
#else
    home = getenv("HOME");
#endif
    if (!home) return 0;

    char *dot_now = now_path_join(home, ".now");
    if (!dot_now) return 0;
    char *repo_root = now_path_join(dot_now, "repo");
    free(dot_now);
    if (!repo_root) return 0;

    /* For each dep, add include and lib paths from local repo */
    for (size_t i = 0; i < project->deps.count; i++) {
        const char *dep_id = project->deps.items[i].id;
        if (!dep_id) continue;

        NowCoordinate coord;
        if (now_coord_parse(dep_id, &coord) != 0) continue;

        /* Load the lock file to get the resolved version */
        NowLockFile lf;
        now_lock_init(&lf);
        now_lock_load(&lf, "now.lock.pasta");
        const NowLockEntry *entry = now_lock_find(&lf, coord.group, coord.artifact);

        const char *version = entry ? entry->version : coord.version;
        if (!version || strlen(version) == 0) {
            now_coord_free(&coord);
            now_lock_free(&lf);
            continue;
        }

        char *dep_path = now_repo_dep_path(repo_root, coord.group,
                                            coord.artifact, version);
        if (dep_path) {
            int msvc = ctx->toolchain.is_msvc;

            /* Include: -I or /I */
            char *h_dir = now_path_join(dep_path, "h");
            if (h_dir && now_path_exists(h_dir)) {
                size_t len = strlen(h_dir) + 4;
                char *flag = (char *)malloc(len);
                if (flag) {
                    snprintf(flag, len, "%s%s", msvc ? "/I" : "-I", h_dir);
                    now_filelist_push(&ctx->dep_includes, flag);
                    free(flag);
                }
            }
            free(h_dir);

            /* Lib dir: -L or /LIBPATH:.  Installed deps land at
             * <repo>/group/artifact/version/lib/<triple>/<lib>, so we need
             * to point at the triple subdir, not its parent. Falls back to
             * <dep>/lib if no triple subdir exists (header-only or older
             * install layout). */
            char *lib_base = now_path_join(dep_path, "lib");
            char *lib_dir  = NULL;
            if (lib_base && now_path_exists(lib_base)) {
                char triple_buf[64] = {0};
                const NowTriple *host = now_host_triple_parsed();
                if (host) now_triple_dir(host, triple_buf, sizeof(triple_buf));
                if (triple_buf[0]) {
                    char *cand = now_path_join(lib_base, triple_buf);
                    if (cand && now_path_exists(cand)) lib_dir = cand;
                    else free(cand);
                }
                if (!lib_dir) lib_dir = strdup(lib_base);
            }
            free(lib_base);
            if (lib_dir) {
                size_t len = strlen(lib_dir) + 12;
                char *flag = (char *)malloc(len);
                if (flag) {
                    if (msvc)
                        snprintf(flag, len, "/LIBPATH:%s", lib_dir);
                    else
                        snprintf(flag, len, "-L%s", lib_dir);
                    now_filelist_push(&ctx->dep_libdirs, flag);
                    free(flag);
                }
                free(lib_dir);
            }

            /* Lib name: -l{name} or {name}.lib */
            {
                size_t len = strlen(coord.artifact) + 5;
                char *flag = (char *)malloc(len);
                if (flag) {
                    if (msvc)
                        snprintf(flag, len, "%s.lib", coord.artifact);
                    else
                        snprintf(flag, len, "-l%s", coord.artifact);
                    now_filelist_push(&ctx->dep_libs, flag);
                    free(flag);
                }
            }

            free(dep_path);
        }

        now_coord_free(&coord);
        now_lock_free(&lf);
    }

    free(repo_root);
    return 0;
}

/* Run generate-phase plugins and inject generated sources/includes/defines
 * into the build context. Returns 0 on success, -1 on plugin error. */
static int run_generate_phase(const NowProject *project,
                               const char *basedir,
                               NowBuildCtx *ctx,
                               int verbose,
                               NowResult *result) {
    if (!project || project->plugins.count == 0)
        return 0;

    NowPluginResult gen;
    now_plugin_result_init(&gen);

    int rc = now_plugin_run_hook(project, basedir, NOW_HOOK_GENERATE,
                                  verbose, &gen, result);

    /* Inject generated sources into build context file list */
    for (size_t i = 0; i < gen.sources.count; i++)
        now_filelist_push(&ctx->sources, gen.sources.items[i]);

    /* Inject generated include paths as dep_includes */
    for (size_t i = 0; i < gen.includes.count; i++) {
        int msvc = ctx->toolchain.is_msvc;
        const char *inc = gen.includes.items[i];
        size_t len = strlen(inc) + 4;
        char *flag = (char *)malloc(len);
        if (flag) {
            snprintf(flag, len, "%s%s", msvc ? "/I" : "-I", inc);
            now_filelist_push(&ctx->dep_includes, flag);
            free(flag);
        }
    }

    /* Inject generated defines — these get added to compile flags */
    for (size_t i = 0; i < gen.defines.count; i++) {
        int msvc = ctx->toolchain.is_msvc;
        const char *def = gen.defines.items[i];
        size_t len = strlen(def) + 4;
        char *flag = (char *)malloc(len);
        if (flag) {
            snprintf(flag, len, "%s%s", msvc ? "/D" : "-D", def);
            now_filelist_push(&ctx->dep_includes, flag);
            free(flag);
        }
    }

    now_plugin_result_free(&gen);
    return rc;
}

/* ---- Build operations ---- */

NOW_API int now_build(const NowProject *project, const char *basedir,
                      int verbose, int jobs, NowResult *result) {
    NowBuildCtx ctx;
    int rc = now_build_init(&ctx, project, basedir, result);
    if (rc != 0) return rc;
    ctx.verbose = verbose;
    ctx.jobs = jobs;

    /* Procure deps before compile */
    rc = procure_and_inject_deps(project, &ctx, result);
    if (rc != 0) { now_build_free(&ctx); return rc; }

    /* Generate phase: run plugins, inject sources/includes/defines */
    rc = run_generate_phase(project, basedir, &ctx, verbose, result);
    if (rc != 0) { now_build_free(&ctx); return rc; }

    rc = now_build_compile(&ctx, result);
    if (rc != 0) { now_build_free(&ctx); return rc; }

    if (now_tui_global) now_tui_link(now_tui_global);
    rc = now_build_link(&ctx, result);
    now_build_free(&ctx);
    return rc;
}

NOW_API int now_compile(const NowProject *project, const char *basedir,
                        int verbose, int jobs, NowResult *result) {
    NowBuildCtx ctx;
    int rc = now_build_init(&ctx, project, basedir, result);
    if (rc != 0) return rc;
    ctx.verbose = verbose;
    ctx.jobs = jobs;

    rc = procure_and_inject_deps(project, &ctx, result);
    if (rc != 0) { now_build_free(&ctx); return rc; }

    rc = run_generate_phase(project, basedir, &ctx, verbose, result);
    if (rc != 0) { now_build_free(&ctx); return rc; }

    rc = now_build_compile(&ctx, result);
    now_build_free(&ctx);
    return rc;
}

NOW_API int now_test(const NowProject *project, const char *basedir,
                     int verbose, int jobs, NowResult *result) {
    NowBuildCtx ctx;
    int rc = now_build_init(&ctx, project, basedir, result);
    if (rc != 0) return rc;
    ctx.verbose = verbose;
    ctx.jobs = jobs;

    rc = procure_and_inject_deps(project, &ctx, result);
    if (rc != 0) { now_build_free(&ctx); return rc; }

    rc = run_generate_phase(project, basedir, &ctx, verbose, result);
    if (rc != 0) { now_build_free(&ctx); return rc; }

    /* Build production code first */
    rc = now_build_compile(&ctx, result);
    if (rc != 0) { now_build_free(&ctx); return rc; }

    /* Run test phase */
    rc = now_build_test(&ctx, result);
    now_build_free(&ctx);
    return rc;
}










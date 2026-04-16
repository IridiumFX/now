/*
 * now_ci.c — CI integration implementation
 *
 * Structured output, CI environment detection, exit code mapping,
 * and the `now ci` composite lifecycle command.
 */
#include "now_ci.h"
#include "now_fs.h"
#include "now_build.h"
#include "now_package.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
  #include <io.h>
  #define isatty_compat _isatty
  #define fileno_compat _fileno
#else
  #include <unistd.h>
  #define isatty_compat isatty
  #define fileno_compat fileno
#endif

/* ---- Exit code mapping ---- */

NOW_API int now_exit_code(NowError err) {
    switch (err) {
        case NOW_OK:         return NOW_EXIT_OK;
        case NOW_ERR_TOOL:   return NOW_EXIT_BUILD;
        case NOW_ERR_TEST:   return NOW_EXIT_TEST;
        case NOW_ERR_SCHEMA: return NOW_EXIT_CONFIG;
        case NOW_ERR_SYNTAX: return NOW_EXIT_CONFIG;
        case NOW_ERR_IO:     return NOW_EXIT_IO;
        case NOW_ERR_NOT_FOUND: return NOW_EXIT_RESOLVE;
        case NOW_ERR_ALLOC:  return NOW_EXIT_IO;
        case NOW_ERR_AUTH:     return NOW_EXIT_AUTH;
        case NOW_ERR_ADVISORY: return NOW_EXIT_ADVISORY;
        default:               return NOW_EXIT_BUILD;
    }
}

/* ---- CI environment detection ---- */

static int env_is_truthy(const char *name) {
    const char *v = getenv(name);
    if (!v) return 0;
    return (strcmp(v, "1") == 0 || strcmp(v, "true") == 0 ||
            strcmp(v, "TRUE") == 0 || strcmp(v, "yes") == 0);
}

NOW_API void now_ci_detect(NowCIEnv *env) {
    memset(env, 0, sizeof(*env));

    env->is_ci     = env_is_truthy("CI");
    env->is_github = env_is_truthy("GITHUB_ACTIONS");
    env->is_gitlab = env_is_truthy("GITLAB_CI");

    if (env->is_github || env->is_gitlab)
        env->is_ci = 1;

    env->locked  = env_is_truthy("NOW_LOCKED");
    env->offline = env_is_truthy("NOW_OFFLINE");

    /* Color: disabled if NOW_COLOR=0 or NO_COLOR is set or not a TTY */
    const char *nc = getenv("NOW_COLOR");
    const char *no_color = getenv("NO_COLOR");
    if ((nc && strcmp(nc, "0") == 0) || no_color)
        env->no_color = 1;
    else if (!isatty_compat(fileno_compat(stdout)))
        env->no_color = 1;

    /* Output format: JSON for CI, text for TTY, pasta for piped */
    if (env->is_ci)
        env->format = NOW_OUTPUT_JSON;
    else if (isatty_compat(fileno_compat(stdout)))
        env->format = NOW_OUTPUT_TEXT;
    else
        env->format = NOW_OUTPUT_PASTA;
}

/* ---- CI annotations ---- */

NOW_API void now_ci_github_annotation(const char *level,
                                       const char *file, int line,
                                       const char *message) {
    /* GitHub Actions workflow command format */
    if (file && line > 0)
        fprintf(stdout, "::%s file=%s,line=%d::%s\n", level, file, line, message);
    else if (file)
        fprintf(stdout, "::%s file=%s::%s\n", level, file, message);
    else
        fprintf(stdout, "::%s::%s\n", level, message);
    fflush(stdout);
}

NOW_API void now_ci_gitlab_section(const char *name, int start) {
    if (start)
        fprintf(stdout, "\e[0Ksection_start:%ld:%s\r\e[0K%s\n",
                (long)time(NULL), name, name);
    else
        fprintf(stdout, "\e[0Ksection_end:%ld:%s\r\e[0K\n",
                (long)time(NULL), name);
    fflush(stdout);
}

/* ---- Structured output formatting ---- */

/* Simple JSON string escaping (no control chars expected) */
static void json_escape(char *dst, size_t dstlen, const char *src) {
    size_t j = 0;
    for (size_t i = 0; src[i] && j < dstlen - 2; i++) {
        if (src[i] == '"' || src[i] == '\\') {
            if (j + 2 >= dstlen) break;
            dst[j++] = '\\';
        }
        dst[j++] = src[i];
    }
    dst[j] = '\0';
}

NOW_API char *now_ci_format_build(const char *phase,
                                    int status,
                                    long duration_ms,
                                    int total, int compiled,
                                    int cached, int failed,
                                    NowOutputFormat format) {
    char *buf = (char *)malloc(1024);
    if (!buf) return NULL;

    if (format == NOW_OUTPUT_JSON) {
        snprintf(buf, 1024,
            "{\n"
            "  \"phase\": \"%s\",\n"
            "  \"status\": \"%s\",\n"
            "  \"duration_ms\": %ld,\n"
            "  \"steps\": {\n"
            "    \"total\": %d,\n"
            "    \"compiled\": %d,\n"
            "    \"cached\": %d,\n"
            "    \"failed\": %d\n"
            "  }\n"
            "}",
            phase, status == 0 ? "ok" : "error",
            duration_ms, total, compiled, cached, failed);
    } else if (format == NOW_OUTPUT_PASTA) {
        snprintf(buf, 1024,
            "{\n"
            "  phase: \"%s\",\n"
            "  status: \"%s\",\n"
            "  duration_ms: %ld,\n"
            "  steps: { total: %d, compiled: %d, cached: %d, failed: %d }\n"
            "}",
            phase, status == 0 ? "ok" : "error",
            duration_ms, total, compiled, cached, failed);
    } else {
        snprintf(buf, 1024,
            "%s: %s (%ld ms) — %d total, %d compiled, %d cached, %d failed",
            phase, status == 0 ? "ok" : "FAILED",
            duration_ms, total, compiled, cached, failed);
    }

    return buf;
}

NOW_API char *now_ci_format_test(int status,
                                   int total, int passed,
                                   int failed, int skipped,
                                   long duration_ms,
                                   NowOutputFormat format) {
    char *buf = (char *)malloc(1024);
    if (!buf) return NULL;

    if (format == NOW_OUTPUT_JSON) {
        snprintf(buf, 1024,
            "{\n"
            "  \"phase\": \"test\",\n"
            "  \"status\": \"%s\",\n"
            "  \"duration_ms\": %ld,\n"
            "  \"tests\": {\n"
            "    \"total\": %d,\n"
            "    \"passed\": %d,\n"
            "    \"failed\": %d,\n"
            "    \"skipped\": %d\n"
            "  }\n"
            "}",
            status == 0 ? "ok" : "fail",
            duration_ms, total, passed, failed, skipped);
    } else if (format == NOW_OUTPUT_PASTA) {
        snprintf(buf, 1024,
            "{\n"
            "  phase: \"test\",\n"
            "  status: \"%s\",\n"
            "  duration_ms: %ld,\n"
            "  tests: { total: %d, passed: %d, failed: %d, skipped: %d }\n"
            "}",
            status == 0 ? "ok" : "fail",
            duration_ms, total, passed, failed, skipped);
    } else {
        snprintf(buf, 1024,
            "test: %s (%ld ms) — %d total, %d passed, %d failed, %d skipped",
            status == 0 ? "ok" : "FAILED",
            duration_ms, total, passed, failed, skipped);
    }

    return buf;
}

/* ---- CI report writer ---- */

static int write_ci_report(const char *basedir,
                            const char *group, const char *artifact,
                            const char *version, const char *triple,
                            const char *started, const char *finished,
                            long duration_ms, int status,
                            int build_rc, long build_ms,
                            int test_rc, long test_ms) {
    char *target = now_path_join(basedir, "target");
    if (!target) return -1;
    now_mkdir_p(target);

    char *report_path = now_path_join(target, "now-ci-report.json");
    free(target);
    if (!report_path) return -1;

    FILE *fp = fopen(report_path, "w");
    free(report_path);
    if (!fp) return -1;

    fprintf(fp,
        "{\n"
        "  \"project\": \"%s:%s:%s\",\n"
        "  \"triple\": \"%s\",\n"
        "  \"started\": \"%s\",\n"
        "  \"finished\": \"%s\",\n"
        "  \"duration_ms\": %ld,\n"
        "  \"status\": \"%s\",\n"
        "  \"phases\": [\n"
        "    { \"name\": \"build\", \"status\": \"%s\", \"duration_ms\": %ld },\n"
        "    { \"name\": \"test\", \"status\": \"%s\", \"duration_ms\": %ld }\n"
        "  ]\n"
        "}\n",
        group ? group : "", artifact ? artifact : "", version ? version : "",
        triple ? triple : "unknown",
        started, finished, duration_ms,
        status == 0 ? "ok" : "fail",
        build_rc == 0 ? "ok" : "fail", build_ms,
        test_rc == 0 ? "ok" : "fail", test_ms);

    fclose(fp);
    return 0;
}

/* ---- now ci composite lifecycle ---- */

NOW_API int now_ci_run(NowProject *project, const char *basedir,
                        const NowCIEnv *env, int jobs,
                        NowResult *result) {
    int verbose = (env->format == NOW_OUTPUT_TEXT) ? 1 : 0;

    /* Timestamps */
    time_t t0 = time(NULL);
    struct tm *tm0 = gmtime(&t0);
    char started[32];
    strftime(started, sizeof(started), "%Y-%m-%dT%H:%M:%SZ", tm0);

    /* Detect triple */
    const char *triple = now_host_triple();

    /* GitHub section start */
    if (env->is_github)
        now_ci_github_annotation("notice", NULL, 0, "now ci: starting build");
    if (env->is_gitlab)
        now_ci_gitlab_section("build", 1);

    /* Build phase */
    clock_t build_start = clock();
    int build_rc = now_build(project, basedir, verbose, jobs, result);
    long build_ms = (long)((clock() - build_start) * 1000 / CLOCKS_PER_SEC);

    if (build_rc != 0) {
        if (env->is_github) {
            now_ci_github_annotation("error", NULL, 0, result->message);
        }
        if (env->format == NOW_OUTPUT_JSON || env->format == NOW_OUTPUT_PASTA) {
            char *out = now_ci_format_build("build", build_rc, build_ms,
                                             0, 0, 0, 1, env->format);
            if (out) { fprintf(stdout, "%s\n", out); free(out); }
        }

        if (env->is_gitlab) now_ci_gitlab_section("build", 0);

        time_t t1 = time(NULL);
        struct tm *tm1 = gmtime(&t1);
        char finished[32];
        strftime(finished, sizeof(finished), "%Y-%m-%dT%H:%M:%SZ", tm1);
        long total_ms = (long)(t1 - t0) * 1000;
        write_ci_report(basedir, project->group, project->artifact,
                        project->version, triple,
                        started, finished, total_ms, 1,
                        build_rc, build_ms, -1, 0);

        return now_exit_code(result->code);
    }

    if (env->is_gitlab) now_ci_gitlab_section("build", 0);

    /* Test phase */
    if (env->is_gitlab) now_ci_gitlab_section("test", 1);

    clock_t test_start = clock();
    NowResult test_result;
    memset(&test_result, 0, sizeof(test_result));
    int test_rc = now_test(project, basedir, verbose, jobs, &test_result);
    long test_ms = (long)((clock() - test_start) * 1000 / CLOCKS_PER_SEC);

    if (test_rc != 0 && env->is_github) {
        now_ci_github_annotation("error", NULL, 0, test_result.message);
    }

    if (env->is_gitlab) now_ci_gitlab_section("test", 0);

    /* Output structured results */
    if (env->format == NOW_OUTPUT_JSON || env->format == NOW_OUTPUT_PASTA) {
        char *bout = now_ci_format_build("build", 0, build_ms,
                                          0, 0, 0, 0, env->format);
        char *tout = now_ci_format_test(test_rc, 0, 0,
                                         test_rc != 0 ? 1 : 0, 0,
                                         test_ms, env->format);
        if (bout) { fprintf(stdout, "%s\n", bout); free(bout); }
        if (tout) { fprintf(stdout, "%s\n", tout); free(tout); }
    }

    /* Write report */
    time_t t1 = time(NULL);
    struct tm *tm1 = gmtime(&t1);
    char finished[32];
    strftime(finished, sizeof(finished), "%Y-%m-%dT%H:%M:%SZ", tm1);
    long total_ms = (long)(t1 - t0) * 1000;

    write_ci_report(basedir, project->group, project->artifact,
                    project->version, triple,
                    started, finished, total_ms,
                    test_rc,
                    build_rc, build_ms, test_rc, test_ms);

    if (test_rc != 0) {
        *result = test_result;
        return NOW_EXIT_TEST;
    }

    return NOW_EXIT_OK;
}

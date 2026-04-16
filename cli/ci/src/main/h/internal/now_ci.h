/*
 * now_ci.h — CI integration (§16)
 *
 * Structured output (JSON/Pasta/text), CI environment detection,
 * exit code mapping, and the `now ci` composite lifecycle.
 */
#ifndef NOW_CI_H
#define NOW_CI_H

#include "now_pom.h"
#include "now.h"

/* Output format */
typedef enum {
    NOW_OUTPUT_TEXT = 0,   /* Human-readable (default for TTY) */
    NOW_OUTPUT_JSON,       /* JSON (for CI, jq) */
    NOW_OUTPUT_PASTA       /* Pasta (default for piped output) */
} NowOutputFormat;

/* CI environment */
typedef struct {
    int  is_ci;            /* CI=true detected */
    int  is_github;        /* GITHUB_ACTIONS=true */
    int  is_gitlab;        /* GITLAB_CI=true */
    int  locked;           /* --locked or NOW_LOCKED=1 */
    int  offline;          /* --offline or NOW_OFFLINE=1 */
    int  no_color;         /* --no-color or NOW_COLOR=0 */
    NowOutputFormat format;
} NowCIEnv;

/* Detect CI environment from env vars */
NOW_API void now_ci_detect(NowCIEnv *env);

/* Emit a GitHub Actions annotation (error/warning) */
NOW_API void now_ci_github_annotation(const char *level,
                                       const char *file, int line,
                                       const char *message);

/* Emit a GitLab CI section (collapsible) */
NOW_API void now_ci_gitlab_section(const char *name, int start);

/* Format a build result as JSON string.
 * Caller must free() the returned string. */
NOW_API char *now_ci_format_build(const char *phase,
                                    int status,
                                    long duration_ms,
                                    int total, int compiled,
                                    int cached, int failed,
                                    NowOutputFormat format);

/* Format a test result as JSON string.
 * Caller must free() the returned string. */
NOW_API char *now_ci_format_test(int status,
                                   int total, int passed,
                                   int failed, int skipped,
                                   long duration_ms,
                                   NowOutputFormat format);

/* Run the full CI lifecycle: clean → build → test → package.
 * Writes report to target/now-ci-report.json.
 * Returns structured exit code. */
NOW_API int now_ci_run(NowProject *project, const char *basedir,
                        const NowCIEnv *env, int jobs,
                        NowResult *result);

#endif /* NOW_CI_H */

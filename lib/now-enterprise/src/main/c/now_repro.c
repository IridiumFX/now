/*
 * now_repro.c — Reproducible builds (§26)
 *
 * Determinism measures: timebase resolution, path prefix maps,
 * sorted inputs, date macro neutralization, verification pass.
 */
#include "now_repro.h"
#include "now_pom.h"
#include "now_fs.h"
#include "now_build.h"
#include "now_manifest.h"
#include "pasta.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
static char *strndup_compat(const char *s, size_t n) {
    size_t len = strlen(s);
    if (len > n) len = n;
    char *r = (char *)malloc(len + 1);
    if (r) { memcpy(r, s, len); r[len] = '\0'; }
    return r;
}
#define strndup strndup_compat
#endif

/* ---- Config ---- */

NOW_API void now_repro_init(NowReproConfig *cfg) {
    memset(cfg, 0, sizeof(*cfg));
}

NOW_API void now_repro_free(NowReproConfig *cfg) {
    free(cfg->timebase);
    memset(cfg, 0, sizeof(*cfg));
}

NOW_API void now_repro_from_project(NowReproConfig *cfg, const void *proj) {
    now_repro_init(cfg);
    if (!proj) return;
    const NowProject *p = (const NowProject *)proj;
    if (!p->_pasta_root) return;

    const PastaValue *root = (const PastaValue *)p->_pasta_root;
    const PastaValue *repro = pasta_map_get(root, "reproducible");
    if (!repro) return;

    if (pasta_type(repro) == PASTA_BOOL) {
        /* reproducible: true — enable all defaults */
        if (pasta_get_bool(repro)) {
            cfg->enabled          = 1;
            cfg->timebase         = strdup("git-commit");
            cfg->path_prefix_map  = 1;
            cfg->sort_inputs      = 1;
            cfg->no_date_macros   = 1;
            cfg->strip_metadata   = 1;
            cfg->verify           = 1;
        }
        return;
    }

    if (pasta_type(repro) != PASTA_MAP) return;

    cfg->enabled = 1;

    const PastaValue *v;
    v = pasta_map_get(repro, "timebase");
    if (v && pasta_type(v) == PASTA_STRING)
        cfg->timebase = strdup(pasta_get_string(v));
    else
        cfg->timebase = strdup("git-commit");

    v = pasta_map_get(repro, "path_prefix_map");
    cfg->path_prefix_map = (v && pasta_type(v) == PASTA_BOOL) ?
                            pasta_get_bool(v) : 1;

    v = pasta_map_get(repro, "sort_inputs");
    cfg->sort_inputs = (v && pasta_type(v) == PASTA_BOOL) ?
                        pasta_get_bool(v) : 1;

    v = pasta_map_get(repro, "no_date_macros");
    cfg->no_date_macros = (v && pasta_type(v) == PASTA_BOOL) ?
                           pasta_get_bool(v) : 1;

    v = pasta_map_get(repro, "strip_metadata");
    cfg->strip_metadata = (v && pasta_type(v) == PASTA_BOOL) ?
                           pasta_get_bool(v) : 1;

    v = pasta_map_get(repro, "verify");
    cfg->verify = (v && pasta_type(v) == PASTA_BOOL) ?
                   pasta_get_bool(v) : 1;
}

/* ---- Timebase resolution ---- */

/* Run a command and capture first line of output.
 * Returns malloc'd string or NULL. */
static char *capture_cmd(const char *cmd) {
    FILE *fp;
#ifdef _WIN32
    fp = _popen(cmd, "r");
#else
    fp = popen(cmd, "r");
#endif
    if (!fp) return NULL;
    char buf[256];
    char *line = NULL;
    if (fgets(buf, sizeof(buf), fp)) {
        /* Trim trailing newline */
        size_t len = strlen(buf);
        while (len > 0 && (buf[len-1] == '\n' || buf[len-1] == '\r'))
            buf[--len] = '\0';
        line = strdup(buf);
    }
#ifdef _WIN32
    _pclose(fp);
#else
    pclose(fp);
#endif
    return line;
}

NOW_API char *now_repro_resolve_timebase(const NowReproConfig *cfg,
                                          const char *basedir,
                                          NowResult *result) {
    if (!cfg || !cfg->timebase) {
        /* Default: current time (non-reproducible) */
        time_t now = time(NULL);
        struct tm *gm = gmtime(&now);
        char *buf = malloc(32);
        if (buf)
            strftime(buf, 32, "%Y-%m-%dT%H:%M:%SZ", gm);
        return buf;
    }

    const char *tb = cfg->timebase;

    if (strcmp(tb, "now") == 0) {
        time_t now = time(NULL);
        struct tm *gm = gmtime(&now);
        char *buf = malloc(32);
        if (buf)
            strftime(buf, 32, "%Y-%m-%dT%H:%M:%SZ", gm);
        return buf;
    }

    if (strcmp(tb, "zero") == 0) {
        return strdup("1970-01-01T00:00:00Z");
    }

    if (strcmp(tb, "git-commit") == 0) {
        /* Get HEAD commit timestamp in ISO 8601 */
        char *ts = capture_cmd("git log -1 --format=%cI 2>/dev/null");
        if (!ts || *ts == '\0') {
            free(ts);
            if (result) {
                result->code = NOW_ERR_IO;
                snprintf(result->message, sizeof(result->message),
                         "reproducible: timebase 'git-commit' requires a git repository");
            }
            return NULL;
        }
        return ts;
    }

    /* Otherwise treat as literal ISO 8601 timestamp */
    return strdup(tb);
}

/* ---- Compile flags ---- */

NOW_API int now_repro_compile_flags(const NowReproConfig *cfg,
                                     const char *basedir,
                                     const char *timestamp,
                                     int is_msvc,
                                     char ***out_flags,
                                     size_t *out_count) {
    if (!cfg || !cfg->enabled) {
        *out_flags = NULL;
        *out_count = 0;
        return 0;
    }

    /* Allocate generous buffer */
    size_t cap = 16;
    char **flags = calloc(cap, sizeof(char *));
    if (!flags) return -1;
    size_t n = 0;

    /* path_prefix_map: strip absolute build paths */
    if (cfg->path_prefix_map && basedir) {
        char buf[1024];
        if (is_msvc) {
            snprintf(buf, sizeof(buf), "/pathmap:%s=.", basedir);
        } else {
            snprintf(buf, sizeof(buf), "-fdebug-prefix-map=%s=.", basedir);
            flags[n++] = strdup(buf);
            snprintf(buf, sizeof(buf), "-fmacro-prefix-map=%s=.", basedir);
        }
        flags[n++] = strdup(buf);
    }

    /* no_date_macros: neutralize __DATE__ and __TIME__ */
    if (cfg->no_date_macros && timestamp) {
        /* Extract date and time from ISO 8601 timestamp */
        /* Expected: YYYY-MM-DDTHH:MM:SSZ or similar */
        char date_str[32] = {0};
        char time_str[16] = {0};

        /* Parse "YYYY-MM-DDTHH:MM:SS" */
        if (strlen(timestamp) >= 19) {
            /* Format as C __DATE__: "Mar  5 2026" */
            int year = 0, month = 0, day = 0, hour = 0, min = 0, sec = 0;
            if (sscanf(timestamp, "%d-%d-%dT%d:%d:%d",
                       &year, &month, &day, &hour, &min, &sec) >= 3) {
                static const char *months[] = {
                    "Jan","Feb","Mar","Apr","May","Jun",
                    "Jul","Aug","Sep","Oct","Nov","Dec"
                };
                if (month >= 1 && month <= 12) {
                    snprintf(date_str, sizeof(date_str), "\"%s %2d %d\"",
                             months[month-1], day, year);
                    snprintf(time_str, sizeof(time_str), "\"%02d:%02d:%02d\"",
                             hour, min, sec);
                }
            }
        }

        if (date_str[0]) {
            char buf[128];
            if (is_msvc) {
                snprintf(buf, sizeof(buf), "/D__DATE__=%s", date_str);
                flags[n++] = strdup(buf);
                snprintf(buf, sizeof(buf), "/D__TIME__=%s", time_str);
                flags[n++] = strdup(buf);
            } else {
                snprintf(buf, sizeof(buf), "-D__DATE__=%s", date_str);
                flags[n++] = strdup(buf);
                snprintf(buf, sizeof(buf), "-D__TIME__=%s", time_str);
                flags[n++] = strdup(buf);
            }
        }
    }

    *out_flags = flags;
    *out_count = n;
    return (int)n;
}

NOW_API void now_repro_free_flags(char **flags, size_t count) {
    if (!flags) return;
    for (size_t i = 0; i < count; i++)
        free(flags[i]);
    free(flags);
}

/* ---- Linker flags ---- */

NOW_API int now_repro_link_flags(const NowReproConfig *cfg, int is_msvc,
                                  char ***out_flags, size_t *out_count) {
    if (!cfg || !cfg->enabled || is_msvc) {
        /* MSVC doesn't have --build-id; PE timestamps handled separately */
        *out_flags = NULL;
        *out_count = 0;
        return 0;
    }

    if (!cfg->strip_metadata) {
        *out_flags = NULL;
        *out_count = 0;
        return 0;
    }

    char **flags = calloc(2, sizeof(char *));
    if (!flags) return -1;
    size_t n = 0;

    /* Use deterministic build ID instead of random */
    flags[n++] = strdup("-Wl,--build-id=sha1");

    *out_flags = flags;
    *out_count = n;
    return (int)n;
}

/* ---- Sort inputs ---- */

static int cmp_str(const void *a, const void *b) {
    const char *sa = *(const char **)a;
    const char *sb = *(const char **)b;
    return strcmp(sa, sb);
}

NOW_API void now_repro_sort_filelist(void *fl_ptr) {
    NowFileList *fl = (NowFileList *)fl_ptr;
    if (!fl || fl->count < 2) return;
    qsort(fl->paths, fl->count, sizeof(char *), cmp_str);
}

/* ---- Reproducibility check ---- */

/* Hash all files matching a pattern in a directory.
 * Returns a combined hash string, or NULL on error. */
static char *hash_output_dir(const char *dir) {
    if (!dir || !now_is_dir(dir)) return strdup("");

    NowFileList files;
    now_filelist_init(&files);

    /* Discover all files in dir */
    const char *all_exts[] = {
        ".o", ".obj", ".a", ".lib", ".so", ".dll", ".dylib", ".exe", "", NULL
    };
    now_discover_sources(".", dir, all_exts, &files);

    /* Sort for deterministic ordering */
    if (files.count > 1)
        qsort(files.paths, files.count, sizeof(char *), cmp_str);

    /* Hash each file and combine */
    size_t cap = 1024;
    char *combined = malloc(cap);
    if (!combined) { now_filelist_free(&files); return NULL; }
    combined[0] = '\0';
    size_t len = 0;

    for (size_t i = 0; i < files.count; i++) {
        char *full = now_path_join(dir, files.paths[i]);
        if (!full) continue;

        char *hash = now_sha256_file(full);
        free(full);
        if (!hash) continue;

        size_t hlen = strlen(hash) + strlen(files.paths[i]) + 3;
        while (len + hlen > cap) { cap *= 2; combined = realloc(combined, cap); }
        len += (size_t)snprintf(combined + len, cap - len, "%s:%s\n",
                                files.paths[i], hash);
        free(hash);
    }

    now_filelist_free(&files);

    /* Hash the combined string */
    char *final = now_sha256_string(combined, len);
    free(combined);
    return final;
}

NOW_API int now_repro_check(const void *project, const char *basedir,
                             int verbose, int jobs, NowResult *result) {
    const NowProject *p = (const NowProject *)project;
    if (!p || !basedir) {
        if (result) {
            result->code = NOW_ERR_SCHEMA;
            snprintf(result->message, sizeof(result->message),
                     "reproducible:check requires a project");
        }
        return -1;
    }

    if (verbose)
        fprintf(stderr, "reproducible:check — building first pass...\n");

    /* First build */
    int rc = now_build(p, basedir, verbose, jobs, result);
    if (rc != 0) return -1;

    /* Hash outputs */
    char *target = now_path_join(basedir, "target/bin");
    char *hash1 = hash_output_dir(target);
    free(target);

    if (!hash1) {
        if (result) {
            result->code = NOW_ERR_IO;
            snprintf(result->message, sizeof(result->message),
                     "reproducible:check — cannot hash first build output");
        }
        return -1;
    }

    if (verbose)
        fprintf(stderr, "reproducible:check — first build hash: %s\n", hash1);

    /* Delete objects to force a full rebuild */
    char *obj_dir = now_path_join(basedir, "target/obj");
    if (obj_dir) {
        /* Delete manifest to force rebuild */
        char *manifest = now_path_join(basedir, "target/.now-manifest");
        if (manifest) { remove(manifest); free(manifest); }
        free(obj_dir);
    }

    if (verbose)
        fprintf(stderr, "reproducible:check — building second pass...\n");

    /* Second build */
    rc = now_build(p, basedir, verbose, jobs, result);
    if (rc != 0) {
        free(hash1);
        return -1;
    }

    /* Hash outputs again */
    target = now_path_join(basedir, "target/bin");
    char *hash2 = hash_output_dir(target);
    free(target);

    if (!hash2) {
        free(hash1);
        if (result) {
            result->code = NOW_ERR_IO;
            snprintf(result->message, sizeof(result->message),
                     "reproducible:check — cannot hash second build output");
        }
        return -1;
    }

    if (verbose)
        fprintf(stderr, "reproducible:check — second build hash: %s\n", hash2);

    int match = (strcmp(hash1, hash2) == 0);
    free(hash1);
    free(hash2);

    if (match) {
        if (result) {
            result->code = NOW_OK;
            snprintf(result->message, sizeof(result->message),
                     "reproducible:check — outputs match");
        }
        return 0;
    }

    if (result) {
        result->code = NOW_ERR_IO;
        snprintf(result->message, sizeof(result->message),
                 "reproducible:check — outputs differ between builds");
    }
    return 1;
}

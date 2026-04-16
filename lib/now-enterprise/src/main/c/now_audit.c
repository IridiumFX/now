/*
 * now_audit.c — Client-side audit logging
 *
 * Append-only log at ~/.now/audit.pasta. One pasta map per line:
 *   { timestamp: "2026-03-20T12:00:00Z", event: "build", subject: "local",
 *     target: "com.example:mylib:1.0.0", result: "ok", detail: "41 compiled" }
 */
#include "now_audit.h"
#include "now_fs.h"

#include <pasta.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
  #include <windows.h>
#else
  #include <unistd.h>
  #include <pwd.h>
#endif

/* ---- Event name table ---- */

static const char *event_names[] = {
    "build", "publish", "yank", "procure",
    "auth_login", "auth_logout", "verify", "advisory"
};

NOW_API const char *now_audit_event_name(NowAuditEvent e) {
    if ((int)e >= 0 && (int)e < (int)(sizeof(event_names) / sizeof(event_names[0])))
        return event_names[(int)e];
    return "unknown";
}

NOW_API NowAuditEvent now_audit_event_parse(const char *name) {
    if (!name) return NOW_AUDIT_BUILD;
    for (int i = 0; i < (int)(sizeof(event_names) / sizeof(event_names[0])); i++) {
        if (strcmp(name, event_names[i]) == 0)
            return (NowAuditEvent)i;
    }
    return NOW_AUDIT_BUILD;
}

/* ---- Config parsing ---- */

static char *read_file_text(const char *path, size_t *out_len) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    if (sz < 0) { fclose(fp); return NULL; }
    fseek(fp, 0, SEEK_SET);
    char *buf = (char *)malloc((size_t)sz + 1);
    if (!buf) { fclose(fp); return NULL; }
    size_t n = fread(buf, 1, (size_t)sz, fp);
    fclose(fp);
    buf[n] = '\0';
    *out_len = n;
    return buf;
}

NOW_API int now_audit_config_parse(const char *pasta_str, size_t len,
                                    NowAuditConfig *cfg) {
    if (!cfg) return -1;
    memset(cfg, 0, sizeof(*cfg));

    if (!pasta_str || len == 0) return -1;

    PastaResult pr;
    PastaValue *root = pasta_parse(pasta_str, len, &pr);
    if (!root || pasta_type(root) != PASTA_MAP) {
        pasta_free(root);
        return -1;
    }

    const PastaValue *audit = pasta_map_get(root, "audit");
    if (!audit || pasta_type(audit) != PASTA_MAP) {
        pasta_free(root);
        return -1;
    }

    const PastaValue *v;

    v = pasta_map_get(audit, "enabled");
    if (v) {
        if (pasta_type(v) == PASTA_BOOL)
            cfg->enabled = pasta_get_bool(v) ? 1 : 0;
        else if (pasta_type(v) == PASTA_NUMBER)
            cfg->enabled = (int)pasta_get_number(v);
    }

    v = pasta_map_get(audit, "max_entries");
    if (v && pasta_type(v) == PASTA_NUMBER)
        cfg->max_entries = (int)pasta_get_number(v);

    v = pasta_map_get(audit, "log_path");
    if (v && pasta_type(v) == PASTA_STRING)
        cfg->log_path = strdup(pasta_get_string(v));

    pasta_free(root);
    return 0;
}

NOW_API int now_audit_config_load(NowAuditConfig *cfg) {
    if (!cfg) return -1;
    memset(cfg, 0, sizeof(*cfg));

    const char *home = NULL;
#ifdef _WIN32
    home = getenv("USERPROFILE");
    if (!home) home = getenv("HOME");
#else
    home = getenv("HOME");
#endif
    if (!home) return -1;

    char *dot_now = now_path_join(home, ".now");
    if (!dot_now) return -1;
    char *config_path = now_path_join(dot_now, "config.pasta");
    free(dot_now);
    if (!config_path) return -1;

    size_t flen;
    char *data = read_file_text(config_path, &flen);
    free(config_path);
    if (!data) return -1;

    int rc = now_audit_config_parse(data, flen, cfg);
    free(data);
    return rc;
}

NOW_API void now_audit_config_free(NowAuditConfig *cfg) {
    if (!cfg) return;
    free(cfg->log_path);
    cfg->log_path = NULL;
    cfg->enabled = 0;
    cfg->max_entries = 0;
}

/* ---- Default log path ---- */

static char *default_audit_path(void) {
    const char *home = NULL;
#ifdef _WIN32
    home = getenv("USERPROFILE");
    if (!home) home = getenv("HOME");
#else
    home = getenv("HOME");
#endif
    if (!home) return NULL;

    char *dot_now = now_path_join(home, ".now");
    if (!dot_now) return NULL;
    char *path = now_path_join(dot_now, "audit.pasta");
    free(dot_now);
    return path;
}

/* Resolve the effective log path (config override or default) */
static char *resolve_log_path(void) {
    NowAuditConfig cfg;
    if (now_audit_config_load(&cfg) == 0 && cfg.enabled) {
        char *path = cfg.log_path ? strdup(cfg.log_path) : default_audit_path();
        now_audit_config_free(&cfg);
        return path;
    }
    now_audit_config_free(&cfg);
    return NULL;  /* auditing disabled */
}

/* ---- ISO 8601 timestamp ---- */

static void now_timestamp(char *buf, size_t len) {
    time_t t = time(NULL);
    struct tm *tm = gmtime(&t);
    strftime(buf, len, "%Y-%m-%dT%H:%M:%SZ", tm);
}

/* ---- Write single audit entry ---- */

static int write_entry(const char *path, const char *timestamp,
                        const char *event, const char *subject,
                        const char *target, const char *result,
                        const char *detail) {
    /* Ensure parent dir exists */
    {
        char *parent = strdup(path);
        if (parent) {
            char *sep = strrchr(parent, '/');
            if (!sep) sep = strrchr(parent, '\\');
            if (sep) { *sep = '\0'; now_mkdir_p(parent); }
            free(parent);
        }
    }

    FILE *fp = fopen(path, "a");
    if (!fp) return -1;

    /* Write as a single-line pasta map */
    fprintf(fp, "{ timestamp: \"%s\", event: \"%s\", subject: \"%s\", target: \"%s\", result: \"%s\"",
            timestamp, event,
            subject ? subject : "local",
            target ? target : "",
            result ? result : "ok");

    if (detail && detail[0])
        fprintf(fp, ", detail: \"%s\"", detail);

    fprintf(fp, " }\n");
    fclose(fp);
    return 0;
}

/* ---- Rotation ---- */

static int count_lines(const char *path) {
    FILE *fp = fopen(path, "r");
    if (!fp) return 0;
    int count = 0;
    char buf[4096];
    while (fgets(buf, sizeof(buf), fp))
        if (buf[0] == '{') count++;
    fclose(fp);
    return count;
}

static void rotate_log(const char *path, int max_entries) {
    int count = count_lines(path);
    if (count <= max_entries) return;

    /* Read all lines, keep last max_entries/2 */
    int keep = max_entries / 2;
    if (keep < 1) keep = 1;
    int skip = count - keep;

    FILE *fp = fopen(path, "r");
    if (!fp) return;

    /* Collect lines to keep */
    char **lines = NULL;
    int kept = 0;
    int seen = 0;
    char buf[4096];
    while (fgets(buf, sizeof(buf), fp)) {
        if (buf[0] != '{') continue;
        seen++;
        if (seen <= skip) continue;
        char **tmp = (char **)realloc(lines, (kept + 1) * sizeof(char *));
        if (!tmp) break;
        lines = tmp;
        lines[kept++] = strdup(buf);
    }
    fclose(fp);

    /* Rewrite */
    fp = fopen(path, "w");
    if (fp) {
        for (int i = 0; i < kept; i++) {
            fputs(lines[i], fp);
            free(lines[i]);
        }
        fclose(fp);
    } else {
        for (int i = 0; i < kept; i++) free(lines[i]);
    }
    free(lines);
}

/* ---- Public API ---- */

NOW_API int now_audit_record(NowAuditEvent event,
                              const char *subject,
                              const char *target,
                              const char *result,
                              const char *detail) {
    char *path = resolve_log_path();
    if (!path) return 0;  /* disabled — no-op */

    char ts[32];
    now_timestamp(ts, sizeof(ts));

    int rc = write_entry(path, ts, now_audit_event_name(event),
                          subject, target, result, detail);

    /* Check rotation */
    NowAuditConfig cfg;
    if (now_audit_config_load(&cfg) == 0 && cfg.max_entries > 0) {
        char *log = cfg.log_path ? strdup(cfg.log_path) : default_audit_path();
        if (log) {
            rotate_log(log, cfg.max_entries);
            free(log);
        }
    }
    now_audit_config_free(&cfg);

    free(path);
    return rc;
}

NOW_API int now_audit_show(const char *filter_event, int last_n, int verbose) {
    NowAuditConfig cfg;
    char *path = NULL;

    if (now_audit_config_load(&cfg) == 0 && cfg.log_path)
        path = strdup(cfg.log_path);
    now_audit_config_free(&cfg);

    if (!path) path = default_audit_path();
    if (!path) {
        fprintf(stderr, "error: cannot determine audit log path\n");
        return -1;
    }

    FILE *fp = fopen(path, "r");
    if (!fp) {
        printf("No audit log found at %s\n", path);
        free(path);
        return 0;
    }

    /* Read and optionally filter entries */
    char **entries = NULL;
    int count = 0;
    char buf[4096];
    while (fgets(buf, sizeof(buf), fp)) {
        if (buf[0] != '{') continue;

        /* Filter by event type */
        if (filter_event) {
            char needle[64];
            snprintf(needle, sizeof(needle), "event: \"%s\"", filter_event);
            if (!strstr(buf, needle)) continue;
        }

        char **tmp = (char **)realloc(entries, (count + 1) * sizeof(char *));
        if (!tmp) break;
        entries = tmp;
        entries[count++] = strdup(buf);
    }
    fclose(fp);

    /* Apply --last filter */
    int start = 0;
    if (last_n > 0 && last_n < count)
        start = count - last_n;

    /* Print */
    if (count == 0) {
        printf("No audit entries%s%s\n",
               filter_event ? " matching event " : "",
               filter_event ? filter_event : "");
    } else {
        printf("Audit log: %s (%d entries%s)\n", path, count - start,
               filter_event ? ", filtered" : "");
        for (int i = start; i < count; i++) {
            if (verbose) {
                /* Print raw pasta */
                printf("  %s", entries[i]);
            } else {
                /* Parse and print human-readable */
                size_t elen = strlen(entries[i]);
                PastaResult pr;
                PastaValue *root = pasta_parse(entries[i], elen, &pr);
                if (root && pasta_type(root) == PASTA_MAP) {
                    const PastaValue *ts = pasta_map_get(root, "timestamp");
                    const PastaValue *ev = pasta_map_get(root, "event");
                    const PastaValue *su = pasta_map_get(root, "subject");
                    const PastaValue *tg = pasta_map_get(root, "target");
                    const PastaValue *re = pasta_map_get(root, "result");
                    const PastaValue *de = pasta_map_get(root, "detail");

                    printf("  %s  %-12s  %-10s  %s  %s",
                        ts ? pasta_get_string(ts) : "?",
                        ev ? pasta_get_string(ev) : "?",
                        su ? pasta_get_string(su) : "?",
                        re ? pasta_get_string(re) : "?",
                        tg ? pasta_get_string(tg) : "");
                    if (de && pasta_type(de) == PASTA_STRING)
                        printf("  (%s)", pasta_get_string(de));
                    printf("\n");
                } else {
                    printf("  %s", entries[i]);
                }
                pasta_free(root);
            }
        }
    }

    for (int i = 0; i < count; i++) free(entries[i]);
    free(entries);
    free(path);
    return 0;
}

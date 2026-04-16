/*
 * now_advisory.c — Security advisory database and checking (§26.4-26.6)
 *
 * Advisory database loading from Pasta format, severity-based blocking,
 * override mechanism with mandatory expiry, dep checking.
 */
#include "now_advisory.h"
#include "now_pom.h"
#include "now_fs.h"
#include "now_version.h"
#include "pasta.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

/* ---- Severity ---- */

NOW_API NowSeverity now_severity_parse(const char *str) {
    if (!str) return NOW_SEV_INFO;
    if (strcmp(str, "critical") == 0)    return NOW_SEV_CRITICAL;
    if (strcmp(str, "high") == 0)        return NOW_SEV_HIGH;
    if (strcmp(str, "medium") == 0)      return NOW_SEV_MEDIUM;
    if (strcmp(str, "low") == 0)         return NOW_SEV_LOW;
    if (strcmp(str, "info") == 0)        return NOW_SEV_INFO;
    if (strcmp(str, "blacklisted") == 0) return NOW_SEV_BLACKLISTED;
    return NOW_SEV_INFO;
}

NOW_API const char *now_severity_name(NowSeverity sev) {
    switch (sev) {
        case NOW_SEV_BLACKLISTED: return "blacklisted";
        case NOW_SEV_CRITICAL:    return "critical";
        case NOW_SEV_HIGH:        return "high";
        case NOW_SEV_MEDIUM:      return "medium";
        case NOW_SEV_LOW:         return "low";
        case NOW_SEV_INFO:        return "info";
    }
    return "info";
}

NOW_API int now_severity_blocks(NowSeverity sev) {
    return sev >= NOW_SEV_HIGH;
}

/* ---- Database operations ---- */

NOW_API void now_advisory_db_init(NowAdvisoryDB *db) {
    memset(db, 0, sizeof(*db));
}

static void advisory_entry_free(NowAdvisoryEntry *e) {
    free(e->id);
    free(e->title);
    free(e->description);
    for (size_t i = 0; i < e->cve_count; i++)
        free(e->cve[i]);
    free(e->cve);
    for (size_t i = 0; i < e->affects_count; i++) {
        free(e->affects[i].id);
        free(e->affects[i].versions);
    }
    free(e->affects);
    for (size_t i = 0; i < e->fixed_in_count; i++) {
        free(e->fixed_in[i].id);
        free(e->fixed_in[i].version);
    }
    free(e->fixed_in);
}

NOW_API void now_advisory_db_free(NowAdvisoryDB *db) {
    for (size_t i = 0; i < db->count; i++)
        advisory_entry_free(&db->entries[i]);
    free(db->entries);
    free(db->db_version);
    free(db->updated);
    memset(db, 0, sizeof(*db));
}

static int db_push(NowAdvisoryDB *db) {
    if (db->count >= db->capacity) {
        size_t new_cap = db->capacity ? db->capacity * 2 : 8;
        NowAdvisoryEntry *tmp = realloc(db->entries,
                                          new_cap * sizeof(NowAdvisoryEntry));
        if (!tmp) return -1;
        db->entries = tmp;
        db->capacity = new_cap;
    }
    memset(&db->entries[db->count], 0, sizeof(NowAdvisoryEntry));
    return (int)db->count++;
}

/* Parse a single advisory entry from a PastaValue map */
static int parse_advisory_entry(const PastaValue *map, NowAdvisoryEntry *out) {
    const PastaValue *v;

    v = pasta_map_get(map, "id");
    if (v && pasta_type(v) == PASTA_STRING)
        out->id = strdup(pasta_get_string(v));

    v = pasta_map_get(map, "severity");
    if (v && pasta_type(v) == PASTA_STRING)
        out->severity = now_severity_parse(pasta_get_string(v));

    v = pasta_map_get(map, "title");
    if (v && pasta_type(v) == PASTA_STRING)
        out->title = strdup(pasta_get_string(v));

    v = pasta_map_get(map, "description");
    if (v && pasta_type(v) == PASTA_STRING)
        out->description = strdup(pasta_get_string(v));

    v = pasta_map_get(map, "blacklisted");
    if (v && pasta_type(v) == PASTA_BOOL && pasta_get_bool(v)) {
        out->blacklisted = 1;
        out->severity = NOW_SEV_BLACKLISTED;
    }

    v = pasta_map_get(map, "affects_build_time");
    if (v && pasta_type(v) == PASTA_BOOL)
        out->affects_build_time = pasta_get_bool(v);

    v = pasta_map_get(map, "affects_runtime");
    if (v && pasta_type(v) == PASTA_BOOL)
        out->affects_runtime = pasta_get_bool(v);

    /* CVE list */
    v = pasta_map_get(map, "cve");
    if (v && pasta_type(v) == PASTA_ARRAY) {
        size_t n = pasta_count(v);
        out->cve = calloc(n, sizeof(char *));
        if (out->cve) {
            out->cve_count = n;
            for (size_t i = 0; i < n; i++) {
                const PastaValue *cv = pasta_array_get(v, i);
                if (cv && pasta_type(cv) == PASTA_STRING)
                    out->cve[i] = strdup(pasta_get_string(cv));
            }
        }
    }

    /* Affects list */
    v = pasta_map_get(map, "affects");
    if (v && pasta_type(v) == PASTA_ARRAY) {
        size_t n = pasta_count(v);
        out->affects = calloc(n, sizeof(NowAdvisoryAffects));
        if (out->affects) {
            out->affects_count = n;
            for (size_t i = 0; i < n; i++) {
                const PastaValue *ae = pasta_array_get(v, i);
                if (!ae || pasta_type(ae) != PASTA_MAP) continue;
                const PastaValue *aid = pasta_map_get(ae, "id");
                const PastaValue *av  = pasta_map_get(ae, "versions");
                if (aid && pasta_type(aid) == PASTA_STRING)
                    out->affects[i].id = strdup(pasta_get_string(aid));
                if (av && pasta_type(av) == PASTA_ARRAY && pasta_count(av) > 0) {
                    const PastaValue *first = pasta_array_get(av, 0);
                    if (first && pasta_type(first) == PASTA_STRING)
                        out->affects[i].versions = strdup(pasta_get_string(first));
                } else if (av && pasta_type(av) == PASTA_STRING) {
                    out->affects[i].versions = strdup(pasta_get_string(av));
                }
            }
        }
    }

    /* Fixed-in list */
    v = pasta_map_get(map, "fixed_in");
    if (v && pasta_type(v) == PASTA_ARRAY) {
        size_t n = pasta_count(v);
        out->fixed_in = calloc(n, sizeof(NowAdvisoryFix));
        if (out->fixed_in) {
            out->fixed_in_count = n;
            for (size_t i = 0; i < n; i++) {
                const PastaValue *fe = pasta_array_get(v, i);
                if (!fe || pasta_type(fe) != PASTA_MAP) continue;
                const PastaValue *fid = pasta_map_get(fe, "id");
                const PastaValue *fv  = pasta_map_get(fe, "version");
                if (fid && pasta_type(fid) == PASTA_STRING)
                    out->fixed_in[i].id = strdup(pasta_get_string(fid));
                if (fv && pasta_type(fv) == PASTA_STRING)
                    out->fixed_in[i].version = strdup(pasta_get_string(fv));
            }
        }
    }

    return 0;
}

NOW_API int now_advisory_db_load_string(NowAdvisoryDB *db, const char *input,
                                          size_t len, NowResult *result) {
    if (!db || !input) return -1;

    PastaResult pr;
    PastaValue *root = pasta_parse(input, len, &pr);
    if (!root || pr.code != PASTA_OK) {
        if (result) {
            result->code = NOW_ERR_SYNTAX;
            snprintf(result->message, sizeof(result->message),
                     "advisory db: %s (line %d)", pr.message, pr.line);
        }
        return -1;
    }

    const PastaValue *v = pasta_map_get(root, "version");
    if (v && pasta_type(v) == PASTA_STRING)
        db->db_version = strdup(pasta_get_string(v));

    v = pasta_map_get(root, "updated");
    if (v && pasta_type(v) == PASTA_STRING)
        db->updated = strdup(pasta_get_string(v));

    const PastaValue *advisories = pasta_map_get(root, "advisories");
    if (advisories && pasta_type(advisories) == PASTA_ARRAY) {
        size_t n = pasta_count(advisories);
        for (size_t i = 0; i < n; i++) {
            const PastaValue *entry = pasta_array_get(advisories, i);
            if (!entry || pasta_type(entry) != PASTA_MAP) continue;
            int idx = db_push(db);
            if (idx < 0) break;
            parse_advisory_entry(entry, &db->entries[idx]);
        }
    }

    pasta_free(root);
    if (result) result->code = NOW_OK;
    return 0;
}

static char *advisory_db_path(void) {
    const char *home = getenv("HOME");
#ifdef _WIN32
    if (!home) home = getenv("USERPROFILE");
#endif
    if (!home) return NULL;
    char *now_dir = now_path_join(home, ".now");
    if (!now_dir) return NULL;
    char *adv_dir = now_path_join(now_dir, "advisories");
    free(now_dir);
    if (!adv_dir) return NULL;
    char *path = now_path_join(adv_dir, "now-advisory-db.pasta");
    free(adv_dir);
    return path;
}

NOW_API int now_advisory_db_load(NowAdvisoryDB *db, NowResult *result) {
    if (!db) return -1;

    char *path = advisory_db_path();
    if (!path) return 0; /* no home — not an error */

    if (!now_path_exists(path)) {
        free(path);
        return 0; /* no db file — empty database */
    }

    FILE *fp = fopen(path, "rb");
    if (!fp) {
        free(path);
        return 0;
    }

    fseek(fp, 0, SEEK_END);
    long len = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    char *buf = malloc((size_t)len + 1);
    if (!buf) { fclose(fp); free(path); return -1; }
    size_t nread = fread(buf, 1, (size_t)len, fp);
    buf[nread] = '\0';
    fclose(fp);

    int rc = now_advisory_db_load_string(db, buf, nread, result);
    free(buf);
    free(path);
    return rc;
}

/* ---- Override operations ---- */

NOW_API void now_override_list_init(NowOverrideList *list) {
    memset(list, 0, sizeof(*list));
}

NOW_API void now_override_list_free(NowOverrideList *list) {
    for (size_t i = 0; i < list->count; i++) {
        free(list->items[i].advisory);
        free(list->items[i].dep);
        free(list->items[i].reason);
        free(list->items[i].expires);
        free(list->items[i].approved_by);
    }
    free(list->items);
    memset(list, 0, sizeof(*list));
}

static int override_push(NowOverrideList *list) {
    if (list->count >= list->capacity) {
        size_t new_cap = list->capacity ? list->capacity * 2 : 4;
        NowAdvisoryOverride *tmp = realloc(list->items,
                                             new_cap * sizeof(NowAdvisoryOverride));
        if (!tmp) return -1;
        list->items = tmp;
        list->capacity = new_cap;
    }
    memset(&list->items[list->count], 0, sizeof(NowAdvisoryOverride));
    return (int)list->count++;
}

NOW_API int now_advisory_overrides_from_project(NowOverrideList *list,
                                                  const void *proj,
                                                  NowResult *result) {
    if (!list || !proj) return -1;
    const NowProject *p = (const NowProject *)proj;
    if (!p->_pasta_root) return 0;

    const PastaValue *root = (const PastaValue *)p->_pasta_root;
    const PastaValue *adv = pasta_map_get(root, "advisories");
    if (!adv || pasta_type(adv) != PASTA_MAP) return 0;

    const PastaValue *allow = pasta_map_get(adv, "allow");
    if (!allow || pasta_type(allow) != PASTA_ARRAY) return 0;

    size_t n = pasta_count(allow);
    for (size_t i = 0; i < n; i++) {
        const PastaValue *entry = pasta_array_get(allow, i);
        if (!entry || pasta_type(entry) != PASTA_MAP) continue;

        const PastaValue *av = pasta_map_get(entry, "advisory");
        const PastaValue *dv = pasta_map_get(entry, "dep");
        const PastaValue *rv = pasta_map_get(entry, "reason");
        const PastaValue *ev = pasta_map_get(entry, "expires");
        const PastaValue *bv = pasta_map_get(entry, "approved_by");

        /* expires is mandatory */
        if (!ev || pasta_type(ev) != PASTA_STRING) {
            if (result) {
                result->code = NOW_ERR_SCHEMA;
                snprintf(result->message, sizeof(result->message),
                         "advisory override at index %zu missing 'expires'", i);
            }
            return -1;
        }

        int idx = override_push(list);
        if (idx < 0) return -1;

        NowAdvisoryOverride *o = &list->items[idx];
        if (av && pasta_type(av) == PASTA_STRING)
            o->advisory = strdup(pasta_get_string(av));
        if (dv && pasta_type(dv) == PASTA_STRING)
            o->dep = strdup(pasta_get_string(dv));
        if (rv && pasta_type(rv) == PASTA_STRING)
            o->reason = strdup(pasta_get_string(rv));
        o->expires = strdup(pasta_get_string(ev));
        if (bv && pasta_type(bv) == PASTA_STRING)
            o->approved_by = strdup(pasta_get_string(bv));
    }

    if (result) result->code = NOW_OK;
    return 0;
}

/* Parse "YYYY-MM-DD" to YYYYMMDD integer */
static int parse_date_yyyymmdd(const char *s) {
    if (!s || strlen(s) < 10) return -1;
    /* Accept YYYY-MM-DD (first 10 chars) */
    int y = 0, m = 0, d = 0;
    if (sscanf(s, "%d-%d-%d", &y, &m, &d) != 3) return -1;
    return y * 10000 + m * 100 + d;
}

NOW_API int now_advisory_override_expired(const NowAdvisoryOverride *ovr,
                                            int today_yyyymmdd) {
    if (!ovr || !ovr->expires) return -1;
    int exp = parse_date_yyyymmdd(ovr->expires);
    if (exp < 0) return -1;
    return (today_yyyymmdd > exp) ? 1 : 0;
}

NOW_API const NowAdvisoryOverride *now_advisory_find_override(
    const NowOverrideList *list, const char *advisory_id,
    const char *dep_id) {
    if (!list || !advisory_id) return NULL;
    for (size_t i = 0; i < list->count; i++) {
        const NowAdvisoryOverride *o = &list->items[i];
        if (!o->advisory || strcmp(o->advisory, advisory_id) != 0)
            continue;
        /* dep match: if override has dep, it must match; if no dep, matches all */
        if (o->dep && dep_id && strcmp(o->dep, dep_id) != 0)
            continue;
        return o;
    }
    return NULL;
}

/* ---- Advisory checking ---- */

NOW_API void now_advisory_report_init(NowAdvisoryReport *report) {
    memset(report, 0, sizeof(*report));
}

NOW_API void now_advisory_report_free(NowAdvisoryReport *report) {
    for (size_t i = 0; i < report->count; i++)
        free(report->hits[i].dep_id);
    free(report->hits);
    memset(report, 0, sizeof(*report));
}

static int report_push(NowAdvisoryReport *report) {
    if (report->count >= report->capacity) {
        size_t new_cap = report->capacity ? report->capacity * 2 : 8;
        NowAdvisoryHit *tmp = realloc(report->hits,
                                        new_cap * sizeof(NowAdvisoryHit));
        if (!tmp) return -1;
        report->hits = tmp;
        report->capacity = new_cap;
    }
    memset(&report->hits[report->count], 0, sizeof(NowAdvisoryHit));
    return (int)report->count++;
}

/* Check if dep_id "group:artifact:version" matches an affects entry.
 * affects.id is "group:artifact", dep version checked against affects.versions range. */
static int affects_matches(const NowAdvisoryAffects *aff, const char *dep_group,
                            const char *dep_artifact, const NowSemVer *dep_ver) {
    if (!aff->id) return 0;

    /* Parse affects id into group:artifact */
    const char *colon = strchr(aff->id, ':');
    if (!colon) return 0;

    size_t aff_glen = (size_t)(colon - aff->id);
    const char *aff_art = colon + 1;

    if (strlen(dep_group) != aff_glen ||
        strncmp(dep_group, aff->id, aff_glen) != 0)
        return 0;
    if (strcmp(dep_artifact, aff_art) != 0)
        return 0;

    /* Version range check */
    if (!aff->versions || strcmp(aff->versions, "*") == 0)
        return 1; /* wildcard — all versions affected */

    NowVersionRange range;
    if (now_range_parse(aff->versions, &range) != 0)
        return 0; /* unparseable range — skip */

    int sat = now_range_satisfies(&range, dep_ver);
    now_range_free(&range);
    return sat;
}

/* Parse "group:artifact:version" into components */
static int parse_dep_id(const char *dep_id, char **group, char **artifact,
                          char **version) {
    if (!dep_id) return -1;
    const char *c1 = strchr(dep_id, ':');
    if (!c1) return -1;
    const char *c2 = strchr(c1 + 1, ':');
    if (!c2) return -1;

    *group = strndup(dep_id, (size_t)(c1 - dep_id));
    *artifact = strndup(c1 + 1, (size_t)(c2 - c1 - 1));
    *version = strdup(c2 + 1);
    return 0;
}

NOW_API int now_advisory_check_dep(const NowAdvisoryDB *db,
                                     const NowOverrideList *overrides,
                                     const char *dep_id,
                                     const char *dep_scope,
                                     int today_yyyymmdd,
                                     NowAdvisoryReport *report) {
    if (!db || !dep_id || !report) return -1;

    char *group = NULL, *artifact = NULL, *ver_str = NULL;
    if (parse_dep_id(dep_id, &group, &artifact, &ver_str) != 0)
        return -1;

    NowSemVer ver;
    if (now_semver_parse(ver_str, &ver) != 0) {
        free(group); free(artifact); free(ver_str);
        return -1;
    }

    for (size_t i = 0; i < db->count; i++) {
        const NowAdvisoryEntry *e = &db->entries[i];

        /* Check if this advisory affects the dep */
        int matched = 0;
        for (size_t a = 0; a < e->affects_count; a++) {
            if (affects_matches(&e->affects[a], group, artifact, &ver)) {
                matched = 1;
                break;
            }
        }
        if (!matched) continue;

        /* Scope filtering: runtime-only vuln in test dep = warning only */
        int dominated = 0;
        if (e->affects_runtime && !e->affects_build_time && dep_scope) {
            if (strcmp(dep_scope, "test") == 0)
                dominated = 1; /* runtime vuln in test-only dep */
        }

        /* Check for override */
        int overridden = 0;
        int expired = 0;
        if (overrides) {
            const NowAdvisoryOverride *ovr =
                now_advisory_find_override(overrides, e->id, dep_id);
            if (ovr) {
                int exp = now_advisory_override_expired(ovr, today_yyyymmdd);
                if (exp == 0)
                    overridden = 1;
                else if (exp == 1)
                    expired = 1;
            }
        }

        /* Record the hit */
        int idx = report_push(report);
        if (idx < 0) break;

        NowAdvisoryHit *hit = &report->hits[idx];
        hit->advisory = e;
        hit->dep_id = strdup(dep_id);
        hit->overridden = overridden;
        hit->expired = expired;

        /* Determine if blocking */
        if (e->severity == NOW_SEV_BLACKLISTED) {
            /* Blacklisted cannot be overridden */
            report->blocked = 1;
        } else if (now_severity_blocks(e->severity) && !overridden && !dominated) {
            report->blocked = 1;
        }
    }

    now_semver_free(&ver);
    free(group);
    free(artifact);
    free(ver_str);
    return 0;
}

NOW_API int now_advisory_check_project(const NowAdvisoryDB *db,
                                         const void *proj,
                                         int today_yyyymmdd,
                                         NowAdvisoryReport *report,
                                         NowResult *result) {
    if (!db || !proj || !report) return -1;
    const NowProject *p = (const NowProject *)proj;

    /* Parse overrides from project */
    NowOverrideList overrides;
    now_override_list_init(&overrides);
    int rc = now_advisory_overrides_from_project(&overrides, proj, result);
    if (rc != 0) {
        now_override_list_free(&overrides);
        return -1;
    }

    /* Check each dep */
    for (size_t i = 0; i < p->deps.count; i++) {
        const NowDep *dep = &p->deps.items[i];
        if (!dep->id) continue;
        const char *scope = dep->scope ? dep->scope : "compile";
        now_advisory_check_dep(db, &overrides, dep->id, scope,
                                today_yyyymmdd, report);
    }

    now_override_list_free(&overrides);
    if (result) result->code = NOW_OK;
    return report->blocked ? 1 : 0;
}

/* ---- Report formatting ---- */

NOW_API char *now_advisory_report_format(const NowAdvisoryReport *report) {
    if (!report || report->count == 0) return strdup("No advisories found.\n");

    /* Estimate output size */
    size_t cap = 256 + report->count * 256;
    char *buf = malloc(cap);
    if (!buf) return NULL;
    size_t pos = 0;

    pos += (size_t)snprintf(buf + pos, cap - pos,
                             "Advisory check: %zu issue(s) found\n\n",
                             report->count);

    for (size_t i = 0; i < report->count; i++) {
        const NowAdvisoryHit *hit = &report->hits[i];
        const NowAdvisoryEntry *e = hit->advisory;
        if (!e) continue;

        const char *sev = now_severity_name(e->severity);
        const char *status = "";
        if (hit->overridden) status = " [OVERRIDDEN]";
        else if (hit->expired) status = " [OVERRIDE EXPIRED]";

        pos += (size_t)snprintf(buf + pos, cap - pos,
                                 "  %-12s %-30s %s%s\n",
                                 sev, hit->dep_id ? hit->dep_id : "?",
                                 e->id ? e->id : "?", status);
        if (e->title)
            pos += (size_t)snprintf(buf + pos, cap - pos,
                                     "              %s\n", e->title);
        if (e->cve_count > 0 && e->cve[0])
            pos += (size_t)snprintf(buf + pos, cap - pos,
                                     "              %s\n", e->cve[0]);

        /* Show fix if available */
        for (size_t f = 0; f < e->fixed_in_count; f++) {
            if (e->fixed_in[f].version)
                pos += (size_t)snprintf(buf + pos, cap - pos,
                                         "              fixed in %s %s\n",
                                         e->fixed_in[f].id ? e->fixed_in[f].id : "?",
                                         e->fixed_in[f].version);
        }
        pos += (size_t)snprintf(buf + pos, cap - pos, "\n");

        if (pos >= cap - 128) {
            cap *= 2;
            char *tmp = realloc(buf, cap);
            if (!tmp) break;
            buf = tmp;
        }
    }

    if (report->blocked)
        pos += (size_t)snprintf(buf + pos, cap - pos,
                                 "Build blocked by advisory. Update deps or add "
                                 "overrides with justification.\n");

    return buf;
}

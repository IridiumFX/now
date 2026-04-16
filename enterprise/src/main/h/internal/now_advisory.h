/*
 * now_advisory.h — Security advisory database and checking (§26.4-26.6)
 *
 * Advisory database loading, severity matching, override handling,
 * and build-phase guard enforcement.
 */
#ifndef NOW_ADVISORY_H
#define NOW_ADVISORY_H

#include "now.h"

/* Advisory severity levels (ascending) */
typedef enum {
    NOW_SEV_INFO = 0,
    NOW_SEV_LOW,
    NOW_SEV_MEDIUM,
    NOW_SEV_HIGH,
    NOW_SEV_CRITICAL,
    NOW_SEV_BLACKLISTED
} NowSeverity;

/* A single "affects" entry within an advisory */
typedef struct {
    char *id;            /* "group:artifact" */
    char *versions;      /* version range string, e.g. ">=1.2.0 <1.3.1" or "*" */
} NowAdvisoryAffects;

/* A single "fixed_in" entry */
typedef struct {
    char *id;            /* "group:artifact" */
    char *version;       /* fixed version string */
} NowAdvisoryFix;

/* An advisory entry from the database */
typedef struct {
    char *id;            /* "NOW-SA-2026-0042" */
    char **cve;          /* CVE identifiers (NULL-terminated) */
    size_t cve_count;
    NowSeverity severity;
    char *title;
    char *description;

    NowAdvisoryAffects *affects;
    size_t affects_count;

    NowAdvisoryFix *fixed_in;
    size_t fixed_in_count;

    int  blacklisted;        /* entire artifact banned */
    int  affects_build_time;
    int  affects_runtime;
} NowAdvisoryEntry;

/* The advisory database */
typedef struct {
    NowAdvisoryEntry *entries;
    size_t count;
    size_t capacity;
    char  *db_version;       /* database format version */
    char  *updated;          /* ISO 8601 last-updated timestamp */
} NowAdvisoryDB;

/* An override from now.pasta advisories: { allow: [...] } */
typedef struct {
    char *advisory;      /* advisory ID to override */
    char *dep;           /* "group:artifact:version" */
    char *reason;        /* justification */
    char *expires;       /* ISO 8601 date (mandatory) */
    char *approved_by;   /* audit trail */
} NowAdvisoryOverride;

typedef struct {
    NowAdvisoryOverride *items;
    size_t count;
    size_t capacity;
} NowOverrideList;

/* A single check result (advisory matched against a dep) */
typedef struct {
    const NowAdvisoryEntry *advisory;
    char *dep_id;        /* "group:artifact:version" that matched */
    int  overridden;     /* 1 if an active override exists */
    int  expired;        /* 1 if override exists but is expired */
} NowAdvisoryHit;

typedef struct {
    NowAdvisoryHit *hits;
    size_t count;
    size_t capacity;
    int    blocked;      /* 1 if any blocking advisory found */
} NowAdvisoryReport;

/* ---- Database operations ---- */

NOW_API void now_advisory_db_init(NowAdvisoryDB *db);
NOW_API void now_advisory_db_free(NowAdvisoryDB *db);

/* Load advisory database from ~/.now/advisories/now-advisory-db.pasta */
NOW_API int now_advisory_db_load(NowAdvisoryDB *db, NowResult *result);

/* Load advisory database from a Pasta string in memory */
NOW_API int now_advisory_db_load_string(NowAdvisoryDB *db, const char *input,
                                          size_t len, NowResult *result);

/* ---- Severity ---- */

NOW_API NowSeverity now_severity_parse(const char *str);
NOW_API const char *now_severity_name(NowSeverity sev);

/* Is this severity blocking by default? (critical, high, blacklisted) */
NOW_API int now_severity_blocks(NowSeverity sev);

/* ---- Override operations ---- */

NOW_API void now_override_list_init(NowOverrideList *list);
NOW_API void now_override_list_free(NowOverrideList *list);

/* Parse overrides from a NowProject's advisories: { allow: [...] } section */
NOW_API int now_advisory_overrides_from_project(NowOverrideList *list,
                                                  const void *project,
                                                  NowResult *result);

/* Check if an override is expired relative to today (YYYYMMDD integer).
 * Returns 1 if expired, 0 if still active, -1 on parse error. */
NOW_API int now_advisory_override_expired(const NowAdvisoryOverride *ovr,
                                            int today_yyyymmdd);

/* Find an override for a given advisory+dep. Returns pointer or NULL. */
NOW_API const NowAdvisoryOverride *now_advisory_find_override(
    const NowOverrideList *list, const char *advisory_id,
    const char *dep_id);

/* ---- Advisory checking ---- */

NOW_API void now_advisory_report_init(NowAdvisoryReport *report);
NOW_API void now_advisory_report_free(NowAdvisoryReport *report);

/* Check a single dep (group:artifact:version) against the advisory database.
 * Appends hits to the report. today is YYYYMMDD integer for override expiry. */
NOW_API int now_advisory_check_dep(const NowAdvisoryDB *db,
                                     const NowOverrideList *overrides,
                                     const char *dep_id,
                                     const char *dep_scope,
                                     int today_yyyymmdd,
                                     NowAdvisoryReport *report);

/* Check all project deps against the advisory database.
 * Returns 0 if no blocking advisories, 1 if build should be blocked. */
NOW_API int now_advisory_check_project(const NowAdvisoryDB *db,
                                         const void *project,
                                         int today_yyyymmdd,
                                         NowAdvisoryReport *report,
                                         NowResult *result);

/* Format a report for display */
NOW_API char *now_advisory_report_format(const NowAdvisoryReport *report);

#endif /* NOW_ADVISORY_H */

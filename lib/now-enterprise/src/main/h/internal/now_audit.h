/*
 * now_audit.h — Client-side audit logging for regulated environments
 *
 * Append-only audit trail at ~/.now/audit.pasta (one pasta map per line).
 * Schema matches cookbook's server-side audit format for unified log analysis.
 * Fire-and-forget: record() never causes the calling operation to fail.
 */
#ifndef NOW_AUDIT_H
#define NOW_AUDIT_H

#include "now.h"

/* Audit event types */
typedef enum {
    NOW_AUDIT_BUILD = 0,
    NOW_AUDIT_PUBLISH,
    NOW_AUDIT_YANK,
    NOW_AUDIT_PROCURE,
    NOW_AUDIT_AUTH_LOGIN,
    NOW_AUDIT_AUTH_LOGOUT,
    NOW_AUDIT_VERIFY,
    NOW_AUDIT_ADVISORY
} NowAuditEvent;

/* Audit config (from ~/.now/config.pasta -> audit section) */
typedef struct {
    int   enabled;      /* 0 = disabled (default), 1 = enabled */
    int   max_entries;  /* rotate after N entries, 0 = unlimited */
    char *log_path;     /* custom path (NULL = ~/.now/audit.pasta) */
} NowAuditConfig;

/* Load audit config from ~/.now/config.pasta.
 * Returns 0 if audit section found, -1 otherwise. */
NOW_API int now_audit_config_load(NowAuditConfig *cfg);

/* Parse audit config from a pasta string (for testing). */
NOW_API int now_audit_config_parse(const char *pasta_str, size_t len,
                                    NowAuditConfig *cfg);

/* Free config fields. Safe on zeroed struct. */
NOW_API void now_audit_config_free(NowAuditConfig *cfg);

/* Record an audit event. No-op if auditing disabled.
 * Returns 0 on success, -1 on error (never fails calling operation). */
NOW_API int now_audit_record(NowAuditEvent event,
                              const char *subject,
                              const char *target,
                              const char *result,
                              const char *detail);

/* Show audit log entries. filter_event=NULL for all, last_n=0 for all.
 * Returns 0 on success, -1 on error. */
NOW_API int now_audit_show(const char *filter_event, int last_n, int verbose);

/* Event name conversion */
NOW_API const char    *now_audit_event_name(NowAuditEvent e);
NOW_API NowAuditEvent  now_audit_event_parse(const char *name);

#endif /* NOW_AUDIT_H */

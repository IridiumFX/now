/*
 * now_plugin_registry.h — Plugin discovery, installation, and management
 *
 * CLI operations for the plugin ecosystem: search, install, list, info.
 * Builds on the existing procure infrastructure for registry access.
 */
#ifndef NOW_PLUGIN_REGISTRY_H
#define NOW_PLUGIN_REGISTRY_H

#include "now.h"
#include "now_pom.h"

/* Plugin metadata parsed from plugin.pasta manifest */
typedef struct {
    char *id;              /* group:artifact:version */
    char *name;            /* human-readable name */
    char *description;     /* one-line description */
    char *protocol;        /* protocol version (e.g. "1.0.0") */
    NowStrArray hooks;     /* lifecycle hooks this plugin handles */
    NowStrArray requires;  /* capabilities required from now */
    NowStrArray optional;  /* optional capabilities */
    int  network_required; /* 1 if plugin needs network access */
    char *requires_now;    /* minimum now version (e.g. ">=1.0.0") */
} NowPluginInfo;

NOW_API void now_plugin_info_free(NowPluginInfo *info);

/* Parse a plugin.pasta manifest file into NowPluginInfo.
 * Returns 0 on success, -1 on error. */
NOW_API int now_plugin_manifest_parse(const char *path, NowPluginInfo *out,
                                        NowResult *result);

/* Parse a plugin.pasta manifest from a string buffer.
 * Returns 0 on success, -1 on error. */
NOW_API int now_plugin_manifest_parse_string(const char *data, size_t len,
                                               NowPluginInfo *out,
                                               NowResult *result);

/* List all installed plugins in the local repo.
 * Scans ~/.now/repo/ for directories containing plugin.pasta.
 * Returns 0 on success. Caller must free each info and the array. */
NOW_API int now_plugin_list(const char *repo_root, NowPluginInfo **out,
                              size_t *count, NowResult *result);

/* Install a plugin from a registry into the local repo.
 * Resolves version, downloads package, extracts, validates manifest.
 * Returns 0 on success. */
NOW_API int now_plugin_install(const char *registry_url,
                                 const char *group, const char *artifact,
                                 const char *version,
                                 const char *repo_root,
                                 int verbose, NowResult *result);

/* Search locally installed plugins by keyword (substring match on
 * id, name, and description fields).
 * Returns 0 on success. */
NOW_API int now_plugin_search(const char *query, const char *repo_root,
                                NowPluginInfo **out, size_t *count,
                                NowResult *result);

/* Get info for a specific installed plugin.
 * Returns 0 on success, -1 if not found. */
NOW_API int now_plugin_get_info(const char *repo_root,
                                  const char *group, const char *artifact,
                                  const char *version,
                                  NowPluginInfo *out, NowResult *result);

/* Find the executable binary path for an installed plugin.
 * Returns malloc'd path or NULL if not found. */
NOW_API char *now_plugin_find_binary(const char *repo_root,
                                       const char *group,
                                       const char *artifact,
                                       const char *version);

#endif /* NOW_PLUGIN_REGISTRY_H */

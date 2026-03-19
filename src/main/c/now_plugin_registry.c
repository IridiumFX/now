/*
 * now_plugin_registry.c — Plugin discovery, installation, and management
 *
 * Provides CLI-facing operations for the plugin ecosystem.
 */
#include "now_plugin_registry.h"
#include "now_procure.h"
#include "now_package.h"
#include "now_version.h"
#include "now_fs.h"

#include "pasta.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
  #include <windows.h>
  #include <direct.h>
  #define PATH_SEP '\\'
  #define EXE_EXT ".exe"
  static char *strndup_compat(const char *s, size_t n) {
      size_t len = strlen(s);
      if (len > n) len = n;
      char *r = (char *)malloc(len + 1);
      if (r) { memcpy(r, s, len); r[len] = '\0'; }
      return r;
  }
  #define strndup strndup_compat
#else
  #include <unistd.h>
  #include <dirent.h>
  #include <sys/stat.h>
  #define PATH_SEP '/'
  #define EXE_EXT ""
#endif

/* ---- Helpers ---- */

static char *get_default_repo_root(void) {
    const char *home = NULL;
#ifdef _WIN32
    home = getenv("USERPROFILE");
    if (!home) home = getenv("HOME");
#else
    home = getenv("HOME");
#endif
    if (!home) return NULL;
    char *now_dir = now_path_join(home, ".now");
    if (!now_dir) return NULL;
    char *repo = now_path_join(now_dir, "repo");
    free(now_dir);
    return repo;
}

/* Convert dotted group to path: "org.now.plugins" -> "org/now/plugins" */
static char *group_to_path(const char *group) {
    if (!group) return NULL;
    char *path = strdup(group);
    if (!path) return NULL;
    for (char *p = path; *p; p++) {
        if (*p == '.') *p = '/';
    }
    return path;
}

/* Parse a Pasta string array into NowStrArray */
static void parse_str_array(const PastaValue *arr, NowStrArray *out) {
    if (!arr || pasta_type(arr) != PASTA_ARRAY) return;
    size_t n = pasta_count(arr);
    for (size_t i = 0; i < n; i++) {
        const PastaValue *v = pasta_array_get(arr, i);
        if (v && pasta_type(v) == PASTA_STRING)
            now_strarray_push(out, pasta_get_string(v));
    }
}

/* ---- Plugin info lifecycle ---- */

NOW_API void now_plugin_info_free(NowPluginInfo *info) {
    if (!info) return;
    free(info->id);
    free(info->name);
    free(info->description);
    free(info->protocol);
    free(info->requires_now);
    now_strarray_free(&info->hooks);
    now_strarray_free(&info->requires);
    now_strarray_free(&info->optional);
    memset(info, 0, sizeof(*info));
}

/* ---- Manifest parsing ---- */

NOW_API int now_plugin_manifest_parse_string(const char *data, size_t len,
                                               NowPluginInfo *out,
                                               NowResult *result) {
    if (!data || !out) {
        if (result) {
            result->code = NOW_ERR_SCHEMA;
            snprintf(result->message, sizeof(result->message),
                     "plugin manifest: NULL input");
        }
        return -1;
    }

    memset(out, 0, sizeof(*out));
    now_strarray_init(&out->hooks);
    now_strarray_init(&out->requires);
    now_strarray_init(&out->optional);

    PastaResult pr;
    PastaValue *root = pasta_parse(data, len, &pr);
    if (!root || pr.code != PASTA_OK) {
        if (result) {
            result->code = NOW_ERR_SYNTAX;
            snprintf(result->message, sizeof(result->message),
                     "plugin manifest parse error: %s", pr.message);
        }
        return -1;
    }

    if (pasta_type(root) != PASTA_MAP) {
        pasta_free(root);
        if (result) {
            result->code = NOW_ERR_SCHEMA;
            snprintf(result->message, sizeof(result->message),
                     "plugin manifest: root must be a map");
        }
        return -1;
    }

    /* id (required) */
    const PastaValue *v = pasta_map_get(root, "id");
    if (v && pasta_type(v) == PASTA_STRING)
        out->id = strdup(pasta_get_string(v));

    /* name */
    v = pasta_map_get(root, "name");
    if (v && pasta_type(v) == PASTA_STRING)
        out->name = strdup(pasta_get_string(v));

    /* description */
    v = pasta_map_get(root, "description");
    if (v && pasta_type(v) == PASTA_STRING)
        out->description = strdup(pasta_get_string(v));

    /* protocol */
    v = pasta_map_get(root, "protocol");
    if (v && pasta_type(v) == PASTA_STRING)
        out->protocol = strdup(pasta_get_string(v));

    /* requires_now */
    v = pasta_map_get(root, "requires_now");
    if (v && pasta_type(v) == PASTA_STRING)
        out->requires_now = strdup(pasta_get_string(v));

    /* hooks */
    parse_str_array(pasta_map_get(root, "hooks"), &out->hooks);

    /* requires */
    parse_str_array(pasta_map_get(root, "requires"), &out->requires);

    /* optional */
    parse_str_array(pasta_map_get(root, "optional"), &out->optional);

    /* network */
    const PastaValue *net = pasta_map_get(root, "network");
    if (net && pasta_type(net) == PASTA_MAP) {
        const PastaValue *req = pasta_map_get(net, "required");
        if (req && pasta_type(req) == PASTA_BOOL)
            out->network_required = pasta_get_bool(req) ? 1 : 0;
    }

    pasta_free(root);

    if (!out->id) {
        now_plugin_info_free(out);
        if (result) {
            result->code = NOW_ERR_SCHEMA;
            snprintf(result->message, sizeof(result->message),
                     "plugin manifest: 'id' field is required");
        }
        return -1;
    }

    if (result) {
        result->code = NOW_OK;
        snprintf(result->message, sizeof(result->message), "ok");
    }
    return 0;
}

NOW_API int now_plugin_manifest_parse(const char *path, NowPluginInfo *out,
                                        NowResult *result) {
    if (!path || !out) {
        if (result) {
            result->code = NOW_ERR_SCHEMA;
            snprintf(result->message, sizeof(result->message),
                     "plugin manifest: NULL path");
        }
        return -1;
    }

    FILE *fp = fopen(path, "rb");
    if (!fp) {
        if (result) {
            result->code = NOW_ERR_NOT_FOUND;
            snprintf(result->message, sizeof(result->message),
                     "plugin manifest not found: %s", path);
        }
        return -1;
    }

    fseek(fp, 0, SEEK_END);
    long flen = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    if (flen <= 0 || flen > 1024 * 1024) {
        fclose(fp);
        if (result) {
            result->code = NOW_ERR_IO;
            snprintf(result->message, sizeof(result->message),
                     "plugin manifest: invalid size %ld", flen);
        }
        return -1;
    }

    char *data = (char *)malloc((size_t)flen + 1);
    if (!data) { fclose(fp); return -1; }
    size_t nread = fread(data, 1, (size_t)flen, fp);
    data[nread] = '\0';
    fclose(fp);

    int rc = now_plugin_manifest_parse_string(data, nread, out, result);
    free(data);
    return rc;
}

/* ---- Binary location ---- */

NOW_API char *now_plugin_find_binary(const char *repo_root,
                                       const char *group,
                                       const char *artifact,
                                       const char *version) {
    if (!group || !artifact || !version) return NULL;

    const char *root = repo_root;
    char *default_root = NULL;
    if (!root) {
        default_root = get_default_repo_root();
        root = default_root;
    }
    if (!root) return NULL;

    char *gpath = group_to_path(group);
    if (!gpath) { free(default_root); return NULL; }

    /* Build: {repo}/{group_path}/{artifact}/{version}/bin/{artifact}[.exe] */
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s/%s/%s/bin/%s%s",
             root, gpath, artifact, version, artifact, EXE_EXT);

    free(gpath);
    free(default_root);

    if (now_path_exists(path))
        return strdup(path);

    return NULL;
}

/* ---- Plugin info for a specific installed plugin ---- */

NOW_API int now_plugin_get_info(const char *repo_root,
                                  const char *group, const char *artifact,
                                  const char *version,
                                  NowPluginInfo *out, NowResult *result) {
    if (!group || !artifact || !version || !out) {
        if (result) {
            result->code = NOW_ERR_SCHEMA;
            snprintf(result->message, sizeof(result->message),
                     "plugin:info: group, artifact, and version required");
        }
        return -1;
    }

    const char *root = repo_root;
    char *default_root = NULL;
    if (!root) {
        default_root = get_default_repo_root();
        root = default_root;
    }
    if (!root) {
        if (result) {
            result->code = NOW_ERR_NOT_FOUND;
            snprintf(result->message, sizeof(result->message),
                     "cannot determine repo root");
        }
        return -1;
    }

    char *gpath = group_to_path(group);
    if (!gpath) { free(default_root); return -1; }

    char manifest_path[1024];
    snprintf(manifest_path, sizeof(manifest_path), "%s/%s/%s/%s/plugin.pasta",
             root, gpath, artifact, version);

    free(gpath);
    free(default_root);

    return now_plugin_manifest_parse(manifest_path, out, result);
}

/* ---- List installed plugins ---- */

/* Recursive directory scanner to find plugin.pasta files */
static int scan_for_plugins(const char *dir, NowPluginInfo **out,
                              size_t *count, size_t *cap) {
#ifdef _WIN32
    char pattern[1024];
    snprintf(pattern, sizeof(pattern), "%s\\*", dir);

    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;

    do {
        if (fd.cFileName[0] == '.') continue;
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
            /* Check if this is a plugin.pasta */
            if (strcmp(fd.cFileName, "plugin.pasta") == 0) {
                char full[1024];
                snprintf(full, sizeof(full), "%s\\%s", dir, fd.cFileName);
                NowPluginInfo info;
                NowResult res;
                if (now_plugin_manifest_parse(full, &info, &res) == 0) {
                    if (*count >= *cap) {
                        *cap = (*cap == 0) ? 8 : *cap * 2;
                        *out = (NowPluginInfo *)realloc(*out,
                                    *cap * sizeof(NowPluginInfo));
                    }
                    (*out)[*count] = info;
                    (*count)++;
                }
            }
            continue;
        }
        /* Recurse into subdirectory */
        char sub[1024];
        snprintf(sub, sizeof(sub), "%s\\%s", dir, fd.cFileName);
        scan_for_plugins(sub, out, count, cap);
    } while (FindNextFileA(h, &fd));

    FindClose(h);
#else
    DIR *d = opendir(dir);
    if (!d) return 0;

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;

        char full[1024];
        snprintf(full, sizeof(full), "%s/%s", dir, ent->d_name);

        struct stat st;
        if (stat(full, &st) != 0) continue;

        if (S_ISDIR(st.st_mode)) {
            scan_for_plugins(full, out, count, cap);
        } else if (strcmp(ent->d_name, "plugin.pasta") == 0) {
            NowPluginInfo info;
            NowResult res;
            if (now_plugin_manifest_parse(full, &info, &res) == 0) {
                if (*count >= *cap) {
                    *cap = (*cap == 0) ? 8 : *cap * 2;
                    *out = (NowPluginInfo *)realloc(*out,
                                *cap * sizeof(NowPluginInfo));
                }
                (*out)[*count] = info;
                (*count)++;
            }
        }
    }

    closedir(d);
#endif
    return 0;
}

NOW_API int now_plugin_list(const char *repo_root, NowPluginInfo **out,
                              size_t *count, NowResult *result) {
    if (!out || !count) {
        if (result) {
            result->code = NOW_ERR_SCHEMA;
            snprintf(result->message, sizeof(result->message),
                     "plugin:list: out and count required");
        }
        return -1;
    }

    *out = NULL;
    *count = 0;

    const char *root = repo_root;
    char *default_root = NULL;
    if (!root) {
        default_root = get_default_repo_root();
        root = default_root;
    }
    if (!root || !now_path_exists(root)) {
        free(default_root);
        if (result) {
            result->code = NOW_OK;
            snprintf(result->message, sizeof(result->message),
                     "no plugins installed (repo not found)");
        }
        return 0;
    }

    size_t cap = 0;
    scan_for_plugins(root, out, count, &cap);

    free(default_root);

    if (result) {
        result->code = NOW_OK;
        snprintf(result->message, sizeof(result->message),
                 "found %zu plugin(s)", *count);
    }
    return 0;
}

/* ---- Search ---- */

static int str_contains_ci(const char *haystack, const char *needle) {
    if (!haystack || !needle) return 0;
    size_t hlen = strlen(haystack);
    size_t nlen = strlen(needle);
    if (nlen > hlen) return 0;
    for (size_t i = 0; i <= hlen - nlen; i++) {
        size_t j;
        for (j = 0; j < nlen; j++) {
            char h = haystack[i + j];
            char n = needle[j];
            if (h >= 'A' && h <= 'Z') h += 32;
            if (n >= 'A' && n <= 'Z') n += 32;
            if (h != n) break;
        }
        if (j == nlen) return 1;
    }
    return 0;
}

NOW_API int now_plugin_search(const char *query, const char *repo_root,
                                NowPluginInfo **out, size_t *count,
                                NowResult *result) {
    if (!out || !count) {
        if (result) {
            result->code = NOW_ERR_SCHEMA;
            snprintf(result->message, sizeof(result->message),
                     "plugin:search: out and count required");
        }
        return -1;
    }

    *out = NULL;
    *count = 0;

    /* First, get all installed plugins */
    NowPluginInfo *all = NULL;
    size_t all_count = 0;
    int rc = now_plugin_list(repo_root, &all, &all_count, result);
    if (rc != 0) return rc;

    if (!query || query[0] == '\0') {
        /* No query — return all */
        *out = all;
        *count = all_count;
        return 0;
    }

    /* Filter by query substring match on id, name, description */
    size_t cap = 0;
    for (size_t i = 0; i < all_count; i++) {
        if (str_contains_ci(all[i].id, query) ||
            str_contains_ci(all[i].name, query) ||
            str_contains_ci(all[i].description, query)) {
            if (*count >= cap) {
                cap = (cap == 0) ? 8 : cap * 2;
                *out = (NowPluginInfo *)realloc(*out,
                            cap * sizeof(NowPluginInfo));
            }
            (*out)[*count] = all[i];
            /* Zero out the source so free doesn't double-free */
            memset(&all[i], 0, sizeof(all[i]));
            (*count)++;
        } else {
            now_plugin_info_free(&all[i]);
        }
    }
    free(all);

    if (result) {
        result->code = NOW_OK;
        snprintf(result->message, sizeof(result->message),
                 "found %zu matching plugin(s)", *count);
    }
    return 0;
}

/* ---- Install ---- */

NOW_API int now_plugin_install(const char *registry_url,
                                 const char *group, const char *artifact,
                                 const char *version,
                                 const char *repo_root,
                                 int verbose, NowResult *result) {
    if (!registry_url || !group || !artifact || !version) {
        if (result) {
            result->code = NOW_ERR_SCHEMA;
            snprintf(result->message, sizeof(result->message),
                     "plugin:install: registry, group, artifact, version required");
        }
        return -1;
    }

    const char *root = repo_root;
    char *default_root = NULL;
    if (!root) {
        default_root = get_default_repo_root();
        root = default_root;
    }
    if (!root) {
        if (result) {
            result->code = NOW_ERR_NOT_FOUND;
            snprintf(result->message, sizeof(result->message),
                     "cannot determine repo root");
        }
        return -1;
    }

    /* Check if already installed */
    if (now_repo_is_installed(root, group, artifact, version)) {
        if (verbose)
            fprintf(stderr, "  plugin %s:%s:%s already installed\n",
                    group, artifact, version);
        free(default_root);
        if (result) {
            result->code = NOW_OK;
            snprintf(result->message, sizeof(result->message),
                     "already installed");
        }
        return 0;
    }

    if (verbose)
        fprintf(stderr, "  resolving %s:%s:%s from %s\n",
                group, artifact, version, registry_url);

    /* Resolve version from registry */
    NowRegistryVersion *versions = NULL;
    int vcount = now_registry_resolve(registry_url, group, artifact,
                                        version, &versions, result);
    if (vcount < 0) {
        free(default_root);
        return -1;
    }
    if (vcount == 0) {
        free(default_root);
        if (result) {
            result->code = NOW_ERR_NOT_FOUND;
            snprintf(result->message, sizeof(result->message),
                     "no matching version for %s:%s:%s",
                     group, artifact, version);
        }
        return -1;
    }

    /* Use the first (best) match */
    const char *resolved_version = versions[0].version;

    if (verbose)
        fprintf(stderr, "  resolved to %s:%s:%s\n",
                group, artifact, resolved_version);

    /* Download the .basta package */
    char filename[256];
    snprintf(filename, sizeof(filename), "%s-%s.basta", artifact, resolved_version);

    char *tmp_dir = now_path_join(root, ".tmp");
    if (tmp_dir) now_mkdir_p(tmp_dir);
    char *tmp_path = tmp_dir ? now_path_join(tmp_dir, filename) : NULL;

    if (!tmp_path) {
        now_registry_versions_free(versions, vcount);
        free(default_root);
        free(tmp_dir);
        return -1;
    }

    int rc = now_registry_download(registry_url, group, artifact,
                                     resolved_version, filename,
                                     tmp_path, NULL, result);
    if (rc != 0) {
        now_registry_versions_free(versions, vcount);
        free(tmp_path);
        free(tmp_dir);
        free(default_root);
        return -1;
    }

    if (verbose)
        fprintf(stderr, "  downloaded %s\n", filename);

    /* Extract to local repo */
    char *dest = now_repo_dep_path(root, group, artifact, resolved_version);
    if (!dest) {
        remove(tmp_path);
        now_registry_versions_free(versions, vcount);
        free(tmp_path);
        free(tmp_dir);
        free(default_root);
        return -1;
    }

    now_mkdir_p(dest);

    rc = now_basta_extract(tmp_path, dest, verbose, result);
    remove(tmp_path);

    if (rc != 0) {
        now_registry_versions_free(versions, vcount);
        free(dest);
        free(tmp_path);
        free(tmp_dir);
        free(default_root);
        return -1;
    }

    /* Validate plugin manifest */
    char manifest_path[1024];
    snprintf(manifest_path, sizeof(manifest_path), "%s/plugin.pasta", dest);

    if (now_path_exists(manifest_path)) {
        NowPluginInfo info;
        NowResult mres;
        if (now_plugin_manifest_parse(manifest_path, &info, &mres) == 0) {
            if (verbose) {
                fprintf(stderr, "  plugin manifest validated: %s\n",
                        info.name ? info.name : info.id);
                if (info.hooks.count > 0) {
                    fprintf(stderr, "  hooks:");
                    for (size_t i = 0; i < info.hooks.count; i++)
                        fprintf(stderr, " %s", info.hooks.items[i]);
                    fprintf(stderr, "\n");
                }
                if (info.network_required)
                    fprintf(stderr, "  WARNING: plugin requires network access\n");
            }
            now_plugin_info_free(&info);
        } else if (verbose) {
            fprintf(stderr, "  warning: no valid plugin.pasta manifest\n");
        }
    }

    if (verbose)
        fprintf(stderr, "  installed %s:%s:%s to %s\n",
                group, artifact, resolved_version, dest);

    now_registry_versions_free(versions, vcount);
    free(dest);
    free(tmp_path);
    free(tmp_dir);
    free(default_root);

    if (result) {
        result->code = NOW_OK;
        snprintf(result->message, sizeof(result->message),
                 "installed %s:%s:%s", group, artifact, resolved_version);
    }
    return 0;
}

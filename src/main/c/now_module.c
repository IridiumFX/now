/*
 * now_module.c — C++20 module pre-scan and dependency ordering
 *
 * Scans C++ source files for module declarations and import statements,
 * builds a dependency graph, and produces a topologically sorted
 * compilation order so module interfaces are compiled before consumers.
 */
#include "now_module.h"
#include "now_fs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

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

/* ---- Init / Free ---- */

NOW_API void now_module_scan_init(NowModuleScan *scan) {
    memset(scan, 0, sizeof(*scan));
}

NOW_API void now_module_scan_free(NowModuleScan *scan) {
    for (size_t i = 0; i < scan->unit_count; i++) {
        free(scan->units[i].name);
        free(scan->units[i].source_path);
        free(scan->units[i].bmi_path);
    }
    free(scan->units);
    for (size_t i = 0; i < scan->import_count; i++) {
        free(scan->imports[i].importer_path);
        free(scan->imports[i].module_name);
    }
    free(scan->imports);
    memset(scan, 0, sizeof(*scan));
}

NOW_API void now_module_order_free(NowModuleOrder *order) {
    for (size_t i = 0; i < order->count; i++)
        free(order->paths[i]);
    free(order->paths);
    memset(order, 0, sizeof(*order));
}

/* ---- Internal helpers ---- */

static int scan_push_unit(NowModuleScan *scan, const char *name,
                           const char *path, int is_interface) {
    if (scan->unit_count >= scan->unit_cap) {
        size_t new_cap = scan->unit_cap ? scan->unit_cap * 2 : 8;
        NowModuleUnit *tmp = realloc(scan->units, new_cap * sizeof(NowModuleUnit));
        if (!tmp) return -1;
        scan->units = tmp;
        scan->unit_cap = new_cap;
    }
    NowModuleUnit *u = &scan->units[scan->unit_count];
    memset(u, 0, sizeof(*u));
    u->name = strdup(name);
    u->source_path = strdup(path);
    u->is_interface = is_interface;
    scan->unit_count++;
    return 0;
}

static int scan_push_import(NowModuleScan *scan, const char *importer,
                              const char *module_name) {
    if (scan->import_count >= scan->import_cap) {
        size_t new_cap = scan->import_cap ? scan->import_cap * 2 : 16;
        NowModuleImport *tmp = realloc(scan->imports,
                                         new_cap * sizeof(NowModuleImport));
        if (!tmp) return -1;
        scan->imports = tmp;
        scan->import_cap = new_cap;
    }
    NowModuleImport *imp = &scan->imports[scan->import_count];
    imp->importer_path = strdup(importer);
    imp->module_name = strdup(module_name);
    scan->import_count++;
    return 0;
}

/* Skip whitespace (not newlines) */
static const char *skip_ws(const char *p) {
    while (*p == ' ' || *p == '\t') p++;
    return p;
}

/* Read a module name: identifiers separated by dots.
 * e.g. "mylib.core" or "std.io" */
static const char *read_module_name(const char *p, char *buf, size_t bufsize) {
    size_t len = 0;
    while (len < bufsize - 1 &&
           (isalnum((unsigned char)*p) || *p == '_' || *p == '.')) {
        buf[len++] = *p++;
    }
    buf[len] = '\0';
    return len > 0 ? p : NULL;
}

/* Check if line starts with a keyword at the beginning (after optional whitespace).
 * Handles: export module NAME; / module NAME; / import NAME;
 * Also handles: import <header>; (ignored — header units not tracked yet) */
static void scan_line(const char *line, const char *path, NowModuleScan *scan) {
    const char *p = skip_ws(line);

    /* Skip preprocessor directives and comments */
    if (*p == '#' || *p == '/' || *p == '\0' || *p == '\n') return;

    char module_name[256];

    /* export module NAME; */
    if (strncmp(p, "export", 6) == 0 && isspace((unsigned char)p[6])) {
        p = skip_ws(p + 6);
        if (strncmp(p, "module", 6) == 0 && isspace((unsigned char)p[6])) {
            p = skip_ws(p + 6);
            if (read_module_name(p, module_name, sizeof(module_name))) {
                scan_push_unit(scan, module_name, path, 1);
            }
            return;
        }
    }

    /* module NAME; (implementation unit or global module fragment) */
    if (strncmp(p, "module", 6) == 0 && isspace((unsigned char)p[6])) {
        p = skip_ws(p + 6);
        /* Skip "module;" (global module fragment) */
        if (*p == ';') return;
        if (read_module_name(p, module_name, sizeof(module_name))) {
            /* Check it's not "module :private;" */
            if (module_name[0] != ':') {
                scan_push_unit(scan, module_name, path, 0);
            }
        }
        return;
    }

    /* import NAME; */
    if (strncmp(p, "import", 6) == 0 && isspace((unsigned char)p[6])) {
        p = skip_ws(p + 6);
        /* Skip header unit imports: import <header>; or import "header"; */
        if (*p == '<' || *p == '"') return;
        if (read_module_name(p, module_name, sizeof(module_name))) {
            scan_push_import(scan, path, module_name);
        }
        return;
    }
}

/* ---- Public API ---- */

NOW_API int now_module_scan_file(NowModuleScan *scan, const char *path) {
    if (!scan || !path) return -1;

    FILE *fp = fopen(path, "r");
    if (!fp) return -1;

    char line[4096];
    int in_block_comment = 0;

    while (fgets(line, sizeof(line), fp)) {
        /* Simple block comment tracking */
        if (in_block_comment) {
            char *end = strstr(line, "*/");
            if (end) {
                in_block_comment = 0;
                /* Continue scanning after the comment */
                scan_line(end + 2, path, scan);
            }
            continue;
        }

        char *bc = strstr(line, "/*");
        if (bc) {
            /* Check if comment closes on same line */
            char *ec = strstr(bc + 2, "*/");
            if (!ec) {
                in_block_comment = 1;
                /* Scan the part before the comment */
                *bc = '\0';
            }
        }

        /* Strip // comments */
        char *lc = strstr(line, "//");
        if (lc) *lc = '\0';

        scan_line(line, path, scan);

        /* Stop scanning after the first non-preprocessor, non-import,
         * non-module line that looks like actual code. Module declarations
         * must appear before any other declarations. We use a simple
         * heuristic: stop after ~50 lines. */
    }

    fclose(fp);
    return 0;
}

NOW_API const NowModuleUnit *now_module_find(const NowModuleScan *scan,
                                              const char *name) {
    if (!scan || !name) return NULL;
    for (size_t i = 0; i < scan->unit_count; i++) {
        if (strcmp(scan->units[i].name, name) == 0 && scan->units[i].is_interface)
            return &scan->units[i];
    }
    /* Fall back to implementation unit if no interface found */
    for (size_t i = 0; i < scan->unit_count; i++) {
        if (strcmp(scan->units[i].name, name) == 0)
            return &scan->units[i];
    }
    return NULL;
}

NOW_API int now_module_is_module_file(const NowModuleScan *scan,
                                       const char *path) {
    if (!scan || !path) return 0;
    for (size_t i = 0; i < scan->unit_count; i++) {
        if (strcmp(scan->units[i].source_path, path) == 0)
            return 1;
    }
    return 0;
}

NOW_API char *now_module_bmi_path(const char *target_dir,
                                   const char *module_name,
                                   int is_msvc) {
    if (!target_dir || !module_name) return NULL;

    const char *ext = is_msvc ? ".ifc" : ".pcm";
    char *bmi_dir = now_path_join(target_dir, "bmi");
    if (!bmi_dir) return NULL;

    /* Replace dots in module name with directory separators for nested modules */
    char flat_name[256];
    snprintf(flat_name, sizeof(flat_name), "%s%s", module_name, ext);

    char *result = now_path_join(bmi_dir, flat_name);
    free(bmi_dir);
    return result;
}

/* ---- Topological sort for module compilation order ---- */

/* Find index of a source path in an array */
static int find_path_index(const char *const *paths, size_t count,
                            const char *path) {
    for (size_t i = 0; i < count; i++) {
        if (strcmp(paths[i], path) == 0) return (int)i;
    }
    return -1;
}

/* Find which source file provides a module */
static const char *find_provider(const NowModuleScan *scan,
                                  const char *module_name) {
    /* Prefer interface units */
    for (size_t i = 0; i < scan->unit_count; i++) {
        if (scan->units[i].is_interface &&
            strcmp(scan->units[i].name, module_name) == 0)
            return scan->units[i].source_path;
    }
    /* Fall back to implementation unit */
    for (size_t i = 0; i < scan->unit_count; i++) {
        if (strcmp(scan->units[i].name, module_name) == 0)
            return scan->units[i].source_path;
    }
    return NULL;
}

static int order_push(NowModuleOrder *order, const char *path) {
    if (order->count >= order->capacity) {
        size_t new_cap = order->capacity ? order->capacity * 2 : 16;
        char **tmp = realloc(order->paths, new_cap * sizeof(char *));
        if (!tmp) return -1;
        order->paths = tmp;
        order->capacity = new_cap;
    }
    order->paths[order->count++] = strdup(path);
    return 0;
}

NOW_API int now_module_order(const NowModuleScan *scan,
                              const char *const *all_sources,
                              size_t source_count,
                              NowModuleOrder *order) {
    if (!scan || !all_sources || !order) return -1;
    memset(order, 0, sizeof(*order));

    if (scan->unit_count == 0) {
        /* No modules — just copy all sources in original order */
        for (size_t i = 0; i < source_count; i++)
            order_push(order, all_sources[i]);
        return 0;
    }

    /* Build adjacency list for module files only.
     * Edge: provider_of(imported_module) → importer */
    size_t n = source_count;
    int *in_degree = (int *)calloc(n, sizeof(int));
    if (!in_degree) return -1;

    /* Adjacency list: edges[i] = list of indices that depend on i */
    size_t *adj_cap = (size_t *)calloc(n, sizeof(size_t));
    size_t *adj_count = (size_t *)calloc(n, sizeof(size_t));
    int **adj = (int **)calloc(n, sizeof(int *));
    if (!adj_cap || !adj_count || !adj) {
        free(in_degree); free(adj_cap); free(adj_count); free(adj);
        return -1;
    }

    /* Track which sources are module-involved */
    int *is_module = (int *)calloc(n, sizeof(int));
    if (!is_module) {
        free(in_degree); free(adj_cap); free(adj_count); free(adj);
        return -1;
    }

    for (size_t i = 0; i < scan->unit_count; i++) {
        int idx = find_path_index(all_sources, source_count,
                                   scan->units[i].source_path);
        if (idx >= 0) is_module[idx] = 1;
    }

    /* Also mark importers as module-involved */
    for (size_t i = 0; i < scan->import_count; i++) {
        int idx = find_path_index(all_sources, source_count,
                                   scan->imports[i].importer_path);
        if (idx >= 0) is_module[idx] = 1;
    }

    /* Build edges from imports */
    for (size_t i = 0; i < scan->import_count; i++) {
        const char *provider = find_provider(scan, scan->imports[i].module_name);
        if (!provider) continue; /* External module — skip */

        int prov_idx = find_path_index(all_sources, source_count, provider);
        int imp_idx = find_path_index(all_sources, source_count,
                                       scan->imports[i].importer_path);
        if (prov_idx < 0 || imp_idx < 0 || prov_idx == imp_idx) continue;

        /* Edge: prov_idx → imp_idx (importer depends on provider) */
        if (adj_count[prov_idx] >= adj_cap[prov_idx]) {
            size_t nc = adj_cap[prov_idx] ? adj_cap[prov_idx] * 2 : 4;
            int *tmp = realloc(adj[prov_idx], nc * sizeof(int));
            if (!tmp) goto cleanup;
            adj[prov_idx] = tmp;
            adj_cap[prov_idx] = nc;
        }
        adj[prov_idx][adj_count[prov_idx]++] = imp_idx;
        in_degree[imp_idx]++;
    }

    /* Kahn's algorithm on module-involved files */
    int *queue = (int *)malloc(n * sizeof(int));
    if (!queue) goto cleanup;
    int qfront = 0, qback = 0;

    /* Seed: module-involved files with in_degree 0 */
    for (size_t i = 0; i < n; i++) {
        if (is_module[i] && in_degree[i] == 0)
            queue[qback++] = (int)i;
    }

    int *visited = (int *)calloc(n, sizeof(int));
    if (!visited) { free(queue); goto cleanup; }

    /* Process in topological order */
    int processed = 0;
    while (qfront < qback) {
        int idx = queue[qfront++];
        order_push(order, all_sources[idx]);
        visited[idx] = 1;
        processed++;

        for (size_t j = 0; j < adj_count[idx]; j++) {
            int dep = adj[idx][j];
            if (--in_degree[dep] == 0 && is_module[dep])
                queue[qback++] = dep;
        }
    }

    /* Check for cycles among module files */
    int module_count = 0;
    for (size_t i = 0; i < n; i++)
        if (is_module[i]) module_count++;

    if (processed < module_count) {
        /* Cycle detected */
        free(queue); free(visited);
        goto cleanup_fail;
    }

    /* Append non-module sources (these can be compiled in parallel) */
    for (size_t i = 0; i < n; i++) {
        if (!visited[i])
            order_push(order, all_sources[i]);
    }

    free(queue);
    free(visited);
    /* Fall through to cleanup (success) */
    {
        int rc = 0;
        free(in_degree); free(is_module);
        for (size_t i = 0; i < n; i++) free(adj[i]);
        free(adj); free(adj_cap); free(adj_count);
        return rc;
    }

cleanup_fail:
    now_module_order_free(order);
cleanup:
    free(in_degree); free(is_module);
    for (size_t i = 0; i < n; i++) free(adj[i]);
    free(adj); free(adj_cap); free(adj_count);
    return -1;
}

/*
 * now_workspace.c — Workspace and module system
 *
 * Handles multi-module projects with topological build ordering
 * using Kahn's algorithm for wave-based parallel execution.
 */
#include "now_workspace.h"
#include "now_fs.h"
#include "now_build.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* ---- Workspace detection ---- */

NOW_API int now_is_workspace(const NowProject *project) {
    return (project && project->modules.count > 0) ? 1 : 0;
}

/* ---- Module graph construction ---- */

/* Upper-case + sanitize an artifact name to a C macro stem.
 * "basta" -> "BASTA", "starletc-cstar" -> "STARLETC_CSTAR".
 * Caller frees. Returns NULL on OOM/empty input. */
static char *upper_macro(const char *name) {
    if (!name || !*name) return NULL;
    size_t n = strlen(name);
    char *out = malloc(n + 1);
    if (!out) return NULL;
    for (size_t i = 0; i < n; i++) {
        char c = name[i];
        if (c >= 'a' && c <= 'z') out[i] = (char)(c - 32);
        else if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_') out[i] = c;
        else out[i] = '_';
    }
    out[n] = '\0';
    return out;
}

/* Find module index by artifact name. Returns -1 if not found. */
static int find_module(NowWorkspace *ws, const char *artifact) {
    for (size_t i = 0; i < ws->module_count; i++) {
        if (ws->modules[i].project && ws->modules[i].project->artifact &&
            strcmp(ws->modules[i].project->artifact, artifact) == 0)
            return (int)i;
        /* Also match by directory name */
        if (ws->modules[i].name &&
            strcmp(ws->modules[i].name, artifact) == 0)
            return (int)i;
    }
    return -1;
}

/* Check if a dependency ID references a sibling module.
 * Format: "group:artifact:version" — we match on artifact name.
 * If the root group matches, it's likely a sibling. */
static int find_sibling(NowWorkspace *ws, const char *dep_id) {
    if (!dep_id) return -1;

    /* Parse the artifact name from "group:artifact:version" */
    const char *first_colon = strchr(dep_id, ':');
    if (!first_colon) return find_module(ws, dep_id);

    const char *art_start = first_colon + 1;
    const char *second_colon = strchr(art_start, ':');

    char artifact[128];
    if (second_colon) {
        size_t len = (size_t)(second_colon - art_start);
        if (len >= sizeof(artifact)) return -1;
        memcpy(artifact, art_start, len);
        artifact[len] = '\0';
    } else {
        size_t len = strlen(art_start);
        if (len >= sizeof(artifact)) return -1;
        memcpy(artifact, art_start, len);
        artifact[len] = '\0';
    }

    return find_module(ws, artifact);
}

/* String-set helpers — open-addressed, linear probe, FNV-1a. */
static uint32_t ws_hash(const char *s) {
    uint32_t h = 0x811c9dc5u;
    while (*s) { h ^= (uint8_t)*s++; h *= 0x01000193u; }
    return h;
}

static void ws_set_free(NowWsStrSet *s) {
    free(s->slots);
    s->slots = NULL;
    s->capacity = s->count = 0;
}

static int ws_set_grow(NowWsStrSet *s) {
    size_t old_cap = s->capacity;
    char **old = s->slots;
    size_t new_cap = old_cap ? old_cap * 2 : 16;
    char **nslots = (char **)calloc(new_cap, sizeof(char *));
    if (!nslots) return -1;
    size_t mask = new_cap - 1;
    for (size_t i = 0; i < old_cap; i++) {
        if (!old[i]) continue;
        size_t j = ws_hash(old[i]) & mask;
        while (nslots[j]) j = (j + 1) & mask;
        nslots[j] = old[i];
    }
    free(old);
    s->slots = nslots;
    s->capacity = new_cap;
    return 0;
}

/* Returns 1 if `v` was added, 0 if it was already present (O(1) avg). */
static int ws_set_add(NowWsStrSet *s, char *v) {
    if (!v) return 0;
    if ((s->count + 1) * 2 > s->capacity) {
        if (ws_set_grow(s) != 0) return 0;
    }
    size_t mask = s->capacity - 1;
    size_t j = ws_hash(v) & mask;
    for (;;) {
        char *cur = s->slots[j];
        if (!cur) { s->slots[j] = v; s->count++; return 1; }
        if (strcmp(cur, v) == 0) return 0;
        j = (j + 1) & mask;
    }
}

/* Append-if-absent helper — dedups while preserving insertion order.
 * Transitive propagation can otherwise produce N² duplicates as each
 * level of the DAG inherits the prior level's full link.libs, which
 * blew past a fixed-size buffer cap in the link loop and silently
 * dropped legitimate entries near the tail (starletc FINDING-E).
 *
 * Uses the per-array NowWsStrSet for O(1) membership; falls back to
 * a linear scan if the set wasn't initialized (callers from outside
 * the workspace flow). */
static void push_unique(NowStrArray *a, NowWsStrSet *set, const char *s) {
    if (!s) return;
    if (set) {
        /* Membership check via set (O(1) avg) */
        if (set->capacity) {
            size_t mask = set->capacity - 1;
            size_t j = ws_hash(s) & mask;
            for (;;) {
                char *cur = set->slots[j];
                if (!cur) break;
                if (strcmp(cur, s) == 0) return;
                j = (j + 1) & mask;
            }
        }
        /* Not present — push to array, then record the strdup'd
         * pointer in the set so later checks see it. */
        if (now_strarray_push(a, s) == 0 && a->count > 0)
            ws_set_add(set, a->items[a->count - 1]);
        return;
    }
    /* No set — fall back to linear scan (external callers) */
    for (size_t i = 0; i < a->count; i++)
        if (strcmp(a->items[i], s) == 0) return;
    now_strarray_push(a, s);
}

/* For each workspace-sibling dep declared in `consumer`'s deps:,
 * inject the producer's public artifacts so the consumer compiles
 * and links without re-declaring them:
 *   compile.includes  += <sibling.dir>/src/main/h
 *   link.libdirs      += <sibling.dir>/target/bin
 *   link.libs         += sibling.output.name (or sibling.artifact)
 *   compile.defines   += <UPPER>_STATIC      (only if sibling is static)
 *
 * Header-only siblings get only the include path; executable
 * siblings are skipped (not linkable). Pushes are deduped — the
 * same lib never appears twice in the consumer's array. */
static void inject_sibling_artifacts(NowWorkspace *ws, int consumer_idx) {
    NowModule *consumer = &ws->modules[consumer_idx];
    NowProject *cp = consumer->project;
    if (!cp) return;

    for (size_t d = 0; d < cp->deps.count; d++) {
        const char *dep_id = cp->deps.items[d].id;
        int sib = find_sibling(ws, dep_id);
        if (sib < 0 || sib == consumer_idx) continue;

        /* Mark so procure's parallel injector skips this dep. */
        cp->deps.items[d].is_workspace_local = 1;

        NowProject *sp = ws->modules[sib].project;
        const char *sdir = ws->modules[sib].dir;
        if (!sp || !sdir) continue;

        const char *otype = sp->output.type;
        if (otype && strcmp(otype, "executable") == 0) continue;

        char path[1024];
        snprintf(path, sizeof(path), "%s/src/main/h", sdir);
        push_unique(&cp->compile.includes, &consumer->seen_includes, path);

        int is_header_only = otype && strcmp(otype, "header-only") == 0;
        if (!is_header_only) {
            snprintf(path, sizeof(path), "%s/target/bin", sdir);
            push_unique(&cp->link.libdirs, &consumer->seen_libdirs, path);

            const char *lib = sp->output.name ? sp->output.name : sp->artifact;
            if (lib) push_unique(&cp->link.libs, &consumer->seen_libs, lib);

            /* Transitive system/external link.libs (e.g. apennines's
             * ws2_32/bcrypt/winmm). Static-linking a sibling drags
             * everything the sibling references, so those have to
             * appear on the consumer's link line too. Already-OS-
             * filtered by the pom loader's OS-block pass. Deduped to
             * avoid N² growth as the DAG accumulates levels. */
            for (size_t k = 0; k < sp->link.libs.count; k++)
                push_unique(&cp->link.libs, &consumer->seen_libs,
                            sp->link.libs.items[k]);

            /* Transitive libdirs too — consumers two hops down the DAG
             * need every intermediate sibling's target/bin on the link
             * line, otherwise `-l<transitive>` resolves against nothing.
             * starletc workaround #2 (each host re-listing 27 transitive
             * sibling depends just for the libdir side-effect). */
            for (size_t k = 0; k < sp->link.libdirs.count; k++)
                push_unique(&cp->link.libdirs, &consumer->seen_libdirs,
                            sp->link.libdirs.items[k]);
        }

        if (otype && strcmp(otype, "static") == 0) {
            const char *base = sp->output.name ? sp->output.name : sp->artifact;
            char *m = upper_macro(base);
            if (m) {
                char def[256];
                snprintf(def, sizeof(def), "%s_STATIC", m);
                push_unique(&cp->compile.defines, &consumer->seen_defines, def);
                free(m);
            }
        }
    }
}

NOW_API int now_workspace_init(NowWorkspace *ws, NowProject *root,
                                const char *root_dir, NowResult *result) {
    memset(ws, 0, sizeof(*ws));
    ws->root = root;
    ws->root_dir = strdup(root_dir);

    size_t nmod = root->modules.count;
    if (nmod == 0) {
        if (result) {
            result->code = NOW_ERR_SCHEMA;
            snprintf(result->message, sizeof(result->message),
                     "workspace has no modules declared");
        }
        return -1;
    }

    ws->modules = (NowModule *)calloc(nmod, sizeof(NowModule));
    if (!ws->modules) return -1;
    ws->module_count = nmod;

    /* Load each module's descriptor */
    for (size_t i = 0; i < nmod; i++) {
        const char *mod_name = root->modules.items[i];
        ws->modules[i].name = strdup(mod_name);
        ws->modules[i].dir = now_path_join(root_dir, mod_name);

        /* Load module's now.pasta */
        char *desc_path = now_path_join(ws->modules[i].dir, "now.pasta");
        if (!desc_path || !now_path_exists(desc_path)) {
            if (result) {
                result->code = NOW_ERR_NOT_FOUND;
                snprintf(result->message, sizeof(result->message),
                         "module '%s' has no now.pasta at %s",
                         mod_name, desc_path ? desc_path : "?");
            }
            free(desc_path);
            return -1;
        }

        NowResult load_res;
        memset(&load_res, 0, sizeof(load_res));
        ws->modules[i].project = now_project_load(desc_path, &load_res);
        free(desc_path);

        if (!ws->modules[i].project) {
            if (result) {
                result->code = load_res.code;
                snprintf(result->message, sizeof(result->message),
                         "module '%s': %s", mod_name, load_res.message);
            }
            return -1;
        }
    }

    /* Build adjacency list for the DAG.
     * Edge: if module j depends on module i, add j to edges[i].
     * (i must be built before j) */
    ws->edges = (int **)calloc(nmod, sizeof(int *));
    ws->edge_counts = (int *)calloc(nmod, sizeof(int));
    if (!ws->edges || !ws->edge_counts) return -1;

    /* Temporary: allocate edge storage (max nmod edges per node) */
    for (size_t i = 0; i < nmod; i++) {
        ws->edges[i] = (int *)calloc(nmod, sizeof(int));
        if (!ws->edges[i]) return -1;
    }

    /* Scan dependencies and build edges */
    for (size_t j = 0; j < nmod; j++) {
        NowProject *mod = ws->modules[j].project;
        if (!mod) continue;

        for (size_t d = 0; d < mod->deps.count; d++) {
            const char *dep_id = mod->deps.items[d].id;
            int dep_idx = find_sibling(ws, dep_id);
            if (dep_idx >= 0 && dep_idx != (int)j) {
                /* Module j depends on module dep_idx.
                 * Add edge: dep_idx → j (dep_idx must build first) */
                int ec = ws->edge_counts[dep_idx];
                ws->edges[dep_idx][ec] = (int)j;
                ws->edge_counts[dep_idx] = ec + 1;
                ws->modules[j].in_degree++;
            }
        }
    }

    /* Auto-propagate each sibling's public artifacts (include dir,
     * libdir, lib name, and <UPPER>_STATIC define for static libs)
     * into every dependent consumer. Mirrors Maven `compile`-scope
     * transitive includes without per-consumer re-declaration.
     *
     * Multi-pass: a topologically-ordered workspace converges in one
     * pass, but workspace authors can declare modules in any order.
     * For an N-level DAG, N passes are sufficient for transitive
     * propagation to reach the deepest consumer. Pushes are deduped
     * by push_unique() so extra passes are safe (just no-ops). */
    for (size_t pass = 0; pass < nmod; pass++)
        for (size_t i = 0; i < nmod; i++)
            inject_sibling_artifacts(ws, (int)i);

    if (result) {
        result->code = NOW_OK;
        result->message[0] = '\0';
    }
    return 0;
}

/* ---- Topological sort (Kahn's algorithm) ---- */

NOW_API int now_workspace_topo_sort(NowWorkspace *ws,
                                     int ***waves_out, int **wave_sizes_out,
                                     NowResult *result) {
    size_t n = ws->module_count;

    /* Copy in-degrees (we'll modify them) */
    int *in_deg = (int *)malloc(n * sizeof(int));
    if (!in_deg) return -1;
    for (size_t i = 0; i < n; i++)
        in_deg[i] = ws->modules[i].in_degree;

    /* Allocate waves (max n waves of max n items each) */
    int **waves = (int **)calloc(n, sizeof(int *));
    int  *sizes = (int *)calloc(n, sizeof(int));
    if (!waves || !sizes) { free(in_deg); return -1; }

    for (size_t i = 0; i < n; i++) {
        waves[i] = (int *)malloc(n * sizeof(int));
        if (!waves[i]) { free(in_deg); return -1; }
    }

    int nwaves = 0;
    int total_sorted = 0;

    while (total_sorted < (int)n) {
        /* Collect all nodes with in_degree == 0 */
        int wave_size = 0;
        for (size_t i = 0; i < n; i++) {
            if (in_deg[i] == 0 && !ws->modules[i].built) {
                waves[nwaves][wave_size++] = (int)i;
            }
        }

        if (wave_size == 0) {
            /* Cycle detected! */
            if (result) {
                result->code = NOW_ERR_SCHEMA;
                snprintf(result->message, sizeof(result->message),
                         "cyclic dependency in module graph");
            }
            /* Clean up */
            for (size_t i = 0; i < n; i++) free(waves[i]);
            free(waves); free(sizes); free(in_deg);
            return -1;
        }

        sizes[nwaves] = wave_size;

        /* "Build" this wave: mark as built, decrement dependents */
        for (int w = 0; w < wave_size; w++) {
            int idx = waves[nwaves][w];
            ws->modules[idx].built = 1;
            in_deg[idx] = -1;  /* sentinel: already processed */

            /* Decrement in-degree of all dependents */
            for (int e = 0; e < ws->edge_counts[idx]; e++) {
                int dep = ws->edges[idx][e];
                in_deg[dep]--;
            }
        }

        total_sorted += wave_size;
        nwaves++;
    }

    free(in_deg);

    /* Reset built flags (caller will set them during actual build) */
    for (size_t i = 0; i < n; i++)
        ws->modules[i].built = 0;

    *waves_out = waves;
    *wave_sizes_out = sizes;
    return nwaves;
}

/* ---- Build all modules in order ---- */

NOW_API int now_workspace_build(NowWorkspace *ws, int verbose, int jobs,
                                 NowResult *result) {
    int **waves = NULL;
    int  *wave_sizes = NULL;

    int nwaves = now_workspace_topo_sort(ws, &waves, &wave_sizes, result);
    if (nwaves < 0) return -1;

    if (verbose)
        fprintf(stderr, "workspace: %zu modules in %d wave(s)\n",
                ws->module_count, nwaves);

    int rc = 0;
    for (int w = 0; w < nwaves && rc == 0; w++) {
        if (verbose) {
            fprintf(stderr, "\n--- wave %d/%d: ", w + 1, nwaves);
            for (int m = 0; m < wave_sizes[w]; m++) {
                if (m > 0) fprintf(stderr, ", ");
                fprintf(stderr, "%s", ws->modules[waves[w][m]].name);
            }
            fprintf(stderr, " ---\n");
        }

        /* Build each module in this wave.
         * TODO: build modules within a wave in parallel */
        for (int m = 0; m < wave_sizes[w] && rc == 0; m++) {
            int idx = waves[w][m];
            NowModule *mod = &ws->modules[idx];

            if (verbose)
                fprintf(stderr, "\n  building module: %s\n", mod->name);

            rc = now_build(mod->project, mod->dir, verbose, jobs, result);
            if (rc != 0) {
                if (result && result->code == NOW_OK) {
                    result->code = NOW_ERR_TOOL;
                    snprintf(result->message, sizeof(result->message),
                             "module '%s' build failed", mod->name);
                }
            } else {
                mod->built = 1;
            }
        }
    }

    /* Free wave arrays */
    for (size_t i = 0; i < ws->module_count; i++)
        free(waves[i]);
    free(waves);
    free(wave_sizes);

    return rc;
}

NOW_API int now_workspace_test(NowWorkspace *ws, int verbose, int jobs,
                                NowResult *result) {
    int **waves = NULL;
    int  *wave_sizes = NULL;

    int nwaves = now_workspace_topo_sort(ws, &waves, &wave_sizes, result);
    if (nwaves < 0) return -1;

    if (verbose)
        fprintf(stderr, "workspace: testing %zu modules in %d wave(s)\n",
                ws->module_count, nwaves);

    int rc = 0;
    int total_fail_modules = 0;

    for (int w = 0; w < nwaves; w++) {
        if (verbose) {
            fprintf(stderr, "\n--- wave %d/%d: ", w + 1, nwaves);
            for (int m = 0; m < wave_sizes[w]; m++) {
                if (m > 0) fprintf(stderr, ", ");
                fprintf(stderr, "%s", ws->modules[waves[w][m]].name);
            }
            fprintf(stderr, " ---\n");
        }

        for (int m = 0; m < wave_sizes[w]; m++) {
            int idx = waves[w][m];
            NowModule *mod = &ws->modules[idx];

            if (verbose)
                fprintf(stderr, "\n  testing module: %s\n", mod->name);
            else
                fprintf(stderr, "[%s]\n", mod->name);

            NowResult mres;
            memset(&mres, 0, sizeof(mres));
            int trc = now_test(mod->project, mod->dir, verbose, jobs, &mres);
            if (trc != 0) {
                fprintf(stderr, "  module '%s' tests failed: %s\n",
                        mod->name, mres.message[0] ? mres.message : "unknown");
                total_fail_modules++;
                rc = -1;
            }
        }
    }

    if (total_fail_modules > 0 && result) {
        result->code = NOW_ERR_TEST;
        snprintf(result->message, sizeof(result->message),
                 "%d module(s) had failing tests", total_fail_modules);
    }

    for (size_t i = 0; i < ws->module_count; i++)
        free(waves[i]);
    free(waves);
    free(wave_sizes);

    return rc;
}

/* ---- Cleanup ---- */

NOW_API void now_workspace_free(NowWorkspace *ws) {
    if (!ws) return;

    for (size_t i = 0; i < ws->module_count; i++) {
        free(ws->modules[i].name);
        free(ws->modules[i].dir);
        if (ws->modules[i].project)
            now_project_free(ws->modules[i].project);
        ws_set_free(&ws->modules[i].seen_includes);
        ws_set_free(&ws->modules[i].seen_libdirs);
        ws_set_free(&ws->modules[i].seen_libs);
        ws_set_free(&ws->modules[i].seen_defines);
    }
    free(ws->modules);

    if (ws->edges) {
        for (size_t i = 0; i < ws->module_count; i++)
            free(ws->edges[i]);
        free(ws->edges);
    }
    free(ws->edge_counts);
    free(ws->root_dir);

    memset(ws, 0, sizeof(*ws));
}

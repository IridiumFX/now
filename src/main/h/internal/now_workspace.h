/*
 * now_workspace.h — Workspace and module system (§1.11, §3.3)
 *
 * Handles multi-module projects: workspace root detection, module graph
 * construction, topological ordering (Kahn's algorithm), and wave-based
 * parallel execution.
 */
#ifndef NOW_WORKSPACE_H
#define NOW_WORKSPACE_H

#include "now_pom.h"
#include "now.h"

/* Open-addressed string set, used by workspace inject to make
 * push_unique O(1) instead of O(N) per call. Entries point at
 * strings owned by the corresponding NowStrArray; the set never
 * frees them. */
typedef struct {
    char  **slots;     /* NULL = empty */
    size_t  capacity;  /* power-of-2 */
    size_t  count;
} NowWsStrSet;

/* A single module in the workspace */
typedef struct {
    char       *name;       /* directory name (e.g. "core", "net") */
    char       *dir;        /* absolute path to module directory */
    NowProject *project;    /* loaded project descriptor */
    int         in_degree;  /* number of unbuilt deps (for Kahn's) */
    int         built;      /* 1 if already built */
    /* Membership-only mirrors of the four arrays workspace inject
     * accumulates into. Lifetime = workspace; freed in workspace_free. */
    NowWsStrSet seen_includes;
    NowWsStrSet seen_libdirs;
    NowWsStrSet seen_libs;
    NowWsStrSet seen_defines;
} NowModule;

/* The full workspace */
typedef struct {
    NowProject  *root;          /* workspace root project */
    char        *root_dir;      /* workspace root directory */
    NowModule   *modules;       /* array of modules */
    size_t       module_count;

    /* DAG adjacency: edges[i] is a list of module indices that depend on i.
     * When module i finishes, decrement in_degree of each edges[i][j]. */
    int        **edges;         /* edges[i] → array of dependent indices */
    int         *edge_counts;   /* number of edges from node i */
} NowWorkspace;

/* Detect whether a project is a workspace root (has modules: array).
 * Returns 1 if workspace, 0 if single project. */
NOW_API int now_is_workspace(const NowProject *project);

/* Initialize a workspace from a root project.
 * Loads all module descriptors, builds the dependency graph.
 * Returns 0 on success. */
NOW_API int now_workspace_init(NowWorkspace *ws, NowProject *root,
                                const char *root_dir, NowResult *result);

/* Topological sort: fills wave_out with arrays of module indices.
 * Each wave contains modules that can be built in parallel.
 * Returns number of waves, or -1 on cycle. */
NOW_API int now_workspace_topo_sort(NowWorkspace *ws,
                                     int ***waves_out, int **wave_sizes_out,
                                     NowResult *result);

/* Build all modules in topological order.
 * Returns 0 if all modules build successfully. */
NOW_API int now_workspace_build(NowWorkspace *ws, int verbose, int jobs,
                                 NowResult *result);

/* Build then run tests for every module in topological order.
 * Returns 0 if every module's tests pass. */
NOW_API int now_workspace_test(NowWorkspace *ws, int verbose, int jobs,
                                NowResult *result);

/* Free workspace resources. */
NOW_API void now_workspace_free(NowWorkspace *ws);

#endif /* NOW_WORKSPACE_H */

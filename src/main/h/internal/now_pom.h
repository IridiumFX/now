/*
 * now_pom.h — Internal Project Object Model structures and loader
 *
 * These structs represent the parsed contents of now.pasta.
 * The public API is in now.h; this header is for internal use.
 */
#ifndef NOW_POM_H
#define NOW_POM_H

#include <stddef.h>
#include "now.h"

/* Dynamic string array */
typedef struct {
    char  **items;
    size_t  count;
    size_t  capacity;
} NowStrArray;

NOW_API void now_strarray_init(NowStrArray *a);
NOW_API int  now_strarray_push(NowStrArray *a, const char *s);
NOW_API void now_strarray_free(NowStrArray *a);

/* Sources configuration (§1.3) */
typedef struct {
    char *dir;             /* source directory */
    char *headers;         /* public headers directory */
    char *private_headers; /* private headers directory */
    char *pattern;         /* glob pattern */
    NowStrArray include;   /* explicit additions */
    NowStrArray exclude;   /* glob exclusions */
} NowSources;

/* Compile configuration (§1.5) */
typedef struct {
    NowStrArray flags;
    NowStrArray warnings;
    NowStrArray defines;
    NowStrArray includes;
    char *std;             /* override std for this block */
    char *opt;             /* none | debug | size | speed | lto */
} NowCompile;

/* Link configuration (§1.5) */
typedef struct {
    NowStrArray flags;
    NowStrArray libs;
    NowStrArray libdirs;
    NowStrArray archives;  /* pre-built static archives (.a/.lib) */
    char *script;          /* linker script path */
    char *script_body;     /* inline linker script (multiline) */
} NowLink;

/* Output configuration (§1.4) */
typedef struct {
    char *type;            /* executable | static | shared | header-only */
    char *name;
    char *dir;
} NowOutput;

/* Dependency entry (§1.6) */
typedef struct {
    char *id;              /* group:artifact:version-or-range */
    char *scope;           /* compile | test | provided | runtime */
    int   optional;
    int   is_volatile;
    int   override;
    NowStrArray exclude;
} NowDep;

/* Dependency list */
typedef struct {
    NowDep *items;
    size_t  count;
    size_t  capacity;
} NowDepArray;

NOW_API void now_deparray_init(NowDepArray *a);
NOW_API int  now_deparray_push(NowDepArray *a);  /* push empty, returns index or -1 */
NOW_API void now_deparray_free(NowDepArray *a);

/* Repository entry (§1.7) */
typedef struct {
    char *url;
    char *id;
    int   release;
    int   snapshot;
    char *auth;
} NowRepo;

typedef struct {
    NowRepo *items;
    size_t   count;
    size_t   capacity;
} NowRepoArray;

NOW_API void now_repoarray_init(NowRepoArray *a);
NOW_API int  now_repoarray_push(NowRepoArray *a);
NOW_API void now_repoarray_free(NowRepoArray *a);

/* Plugin entry (§10) */
typedef struct {
    char *id;              /* group:artifact:version or "now:embed" etc. */
    char *type;            /* "plugin" | "external" (default: "plugin") */
    char *phase;           /* lifecycle hook name */
    char *timeout;         /* e.g. "120s" (default: "30s") */
    char *run;             /* command template for external tools */
    void *config;          /* PastaValue* — plugin-specific config, opaque */
} NowPlugin;

typedef struct {
    NowPlugin *items;
    size_t     count;
    size_t     capacity;
} NowPluginArray;

NOW_API void now_pluginarray_init(NowPluginArray *a);
NOW_API int  now_pluginarray_push(NowPluginArray *a);  /* push empty, returns index or -1 */
NOW_API void now_pluginarray_free(NowPluginArray *a);

/* Allocate a zero-initialized NowProject */
NOW_API NowProject *now_project_new(void);

/* Java-specific configuration */
typedef struct {
    char *main_class;        /* entry point for executable JARs */
    char *encoding;          /* source encoding (default: UTF-8) */
    NowStrArray classpath;   /* additional classpath entries */
} NowJava;

/* The full Project Object Model (matches forward decl in now.h) */
struct NowProject {
    /* Identity (§1.1) */
    char *group;
    char *artifact;
    char *version;
    char *name;
    char *description;
    char *url;
    char *license;

    /* Language (§1.2) */
    NowStrArray langs;
    char *std;

    /* Source layout (§1.3) */
    NowSources sources;
    NowSources tests;

    /* Output (§1.4) */
    NowOutput output;

    /* Compile & link (§1.5) */
    NowCompile compile;
    NowLink    link;

    /* Dependencies (§1.6) */
    NowDepArray deps;

    /* Repositories (§1.7) */
    NowRepoArray repos;

    /* Convergence policy (§6.11) */
    char *convergence;     /* lowest | highest | exact */

    /* Dep confusion protection (§8) */
    NowStrArray private_groups;  /* group prefixes that must not resolve from public registries */

    /* Plugins (§10) */
    NowPluginArray plugins;

    /* Java-specific (when langs includes "java") */
    NowJava java;

    /* Components — your own submodules (full control) */
    NowStrArray components;

    /* Vendored — external deps (read-only, sources discovered) */
    NowStrArray vendored;

    /* Workspace (§1.11) */
    NowStrArray modules;

    /* Raw Pasta tree — kept alive for the project lifetime */
    void *_pasta_root;
};

#endif /* NOW_POM_H */

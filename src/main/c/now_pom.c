/*
 * now_pom.c — Project Object Model loader
 *
 * Parses now.pasta into NowProject using the Pasta library.
 */
#include "now_pom.h"
#include "now.h"

#include "pasta.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ---- String array ---- */

NOW_API void now_strarray_init(NowStrArray *a) {
    a->items = NULL;
    a->count = 0;
    a->capacity = 0;
}

NOW_API int now_strarray_push(NowStrArray *a, const char *s) {
    if (a->count >= a->capacity) {
        size_t new_cap = a->capacity ? a->capacity * 2 : 4;
        char **tmp = realloc(a->items, new_cap * sizeof(char *));
        if (!tmp) return -1;
        a->items = tmp;
        a->capacity = new_cap;
    }
    a->items[a->count] = s ? strdup(s) : NULL;
    if (s && !a->items[a->count]) return -1;
    a->count++;
    return 0;
}

NOW_API void now_strarray_free(NowStrArray *a) {
    for (size_t i = 0; i < a->count; i++)
        free(a->items[i]);
    free(a->items);
    now_strarray_init(a);
}

/* ---- Dep array ---- */

NOW_API void now_deparray_init(NowDepArray *a) {
    a->items = NULL;
    a->count = 0;
    a->capacity = 0;
}

NOW_API int now_deparray_push(NowDepArray *a) {
    if (a->count >= a->capacity) {
        size_t new_cap = a->capacity ? a->capacity * 2 : 4;
        NowDep *tmp = realloc(a->items, new_cap * sizeof(NowDep));
        if (!tmp) return -1;
        a->items = tmp;
        a->capacity = new_cap;
    }
    memset(&a->items[a->count], 0, sizeof(NowDep));
    now_strarray_init(&a->items[a->count].exclude);
    return (int)a->count++;
}

static void now_dep_free(NowDep *d) {
    free(d->id);
    free(d->scope);
    now_strarray_free(&d->exclude);
}

NOW_API void now_deparray_free(NowDepArray *a) {
    for (size_t i = 0; i < a->count; i++)
        now_dep_free(&a->items[i]);
    free(a->items);
    now_deparray_init(a);
}

/* ---- Repo array ---- */

NOW_API void now_repoarray_init(NowRepoArray *a) {
    a->items = NULL;
    a->count = 0;
    a->capacity = 0;
}

NOW_API int now_repoarray_push(NowRepoArray *a) {
    if (a->count >= a->capacity) {
        size_t new_cap = a->capacity ? a->capacity * 2 : 4;
        NowRepo *tmp = realloc(a->items, new_cap * sizeof(NowRepo));
        if (!tmp) return -1;
        a->items = tmp;
        a->capacity = new_cap;
    }
    memset(&a->items[a->count], 0, sizeof(NowRepo));
    a->items[a->count].release = 1;  /* default: release=true */
    return (int)a->count++;
}

NOW_API void now_repoarray_free(NowRepoArray *a) {
    for (size_t i = 0; i < a->count; i++) {
        free(a->items[i].url);
        free(a->items[i].id);
        free(a->items[i].auth);
    }
    free(a->items);
    now_repoarray_init(a);
}

/* ---- Plugin array ---- */

NOW_API void now_pluginarray_init(NowPluginArray *a) {
    a->items = NULL;
    a->count = 0;
    a->capacity = 0;
}

NOW_API int now_pluginarray_push(NowPluginArray *a) {
    if (a->count >= a->capacity) {
        size_t new_cap = a->capacity ? a->capacity * 2 : 4;
        NowPlugin *tmp = realloc(a->items, new_cap * sizeof(NowPlugin));
        if (!tmp) return -1;
        a->items = tmp;
        a->capacity = new_cap;
    }
    memset(&a->items[a->count], 0, sizeof(NowPlugin));
    return (int)a->count++;
}

NOW_API void now_pluginarray_free(NowPluginArray *a) {
    for (size_t i = 0; i < a->count; i++) {
        free(a->items[i].id);
        free(a->items[i].type);
        free(a->items[i].phase);
        free(a->items[i].timeout);
        free(a->items[i].run);
        /* config is a PastaValue* owned by _pasta_root — do not free */
    }
    free(a->items);
    now_pluginarray_init(a);
}

/* ---- Helpers ---- */

static char *dup_string(const PastaValue *v) {
    if (!v || pasta_type(v) != PASTA_STRING) return NULL;
    return strdup(pasta_get_string(v));
}

static char *dup_map_str(const PastaValue *map, const char *key) {
    return dup_string(pasta_map_get(map, key));
}

static int get_map_bool(const PastaValue *map, const char *key, int def) {
    const PastaValue *v = pasta_map_get(map, key);
    if (!v) return def;
    if (pasta_type(v) == PASTA_BOOL) return pasta_get_bool(v);
    return def;
}

/* Load a string array from a Pasta array value */
static void load_strarray(NowStrArray *dst, const PastaValue *arr) {
    if (!arr || pasta_type(arr) != PASTA_ARRAY) return;
    size_t n = pasta_count(arr);
    for (size_t i = 0; i < n; i++) {
        const PastaValue *elem = pasta_array_get(arr, i);
        if (elem && pasta_type(elem) == PASTA_STRING)
            now_strarray_push(dst, pasta_get_string(elem));
    }
}

/* ---- Language-aware defaults ----
 *
 * Maven-style directory conventions, keyed off the primary language:
 *   c    -> src/main/c,   src/main/h,   src/main/h/internal,   src/test/c
 *   c++  -> src/main/cpp, src/main/hpp, src/main/hpp/internal, src/test/cpp
 *   java -> src/main/java,            (no headers)           src/test/java
 *   rust -> src/main/rust,            (no headers)           src/test/rust
 *   go   -> src/main/go,              (no headers)           src/test/go
 *
 * C++ gets fully distinct paths from C on purpose — they are different
 * languages with different idioms; sharing .h/.c roots muddles that.
 */
static void apply_maven_defaults(NowProject *p) {
    if (!p) return;
    const char *primary = (p->langs.count > 0) ? p->langs.items[0] : "c";
    int is_cpp  = (strcmp(primary, "c++")  == 0);
    int is_java = (strcmp(primary, "java") == 0);
    int is_rust = (strcmp(primary, "rust") == 0);
    int is_go   = (strcmp(primary, "go")   == 0);

    if (!p->sources.dir) {
        if      (is_cpp)  p->sources.dir = strdup("src/main/cpp");
        else if (is_java) p->sources.dir = strdup("src/main/java");
        else if (is_rust) p->sources.dir = strdup("src/main/rust");
        else if (is_go)   p->sources.dir = strdup("src/main/go");
        else              p->sources.dir = strdup("src/main/c");
    }

    if (!p->sources.headers) {
        if      (is_cpp)                      p->sources.headers = strdup("src/main/hpp");
        else if (is_java || is_rust || is_go) { /* no headers concept */ }
        else                                  p->sources.headers = strdup("src/main/h");
    }

    if (!p->sources.private_headers) {
        if      (is_cpp)                      p->sources.private_headers = strdup("src/main/hpp/internal");
        else if (is_java || is_rust || is_go) { /* none */ }
        else                                  p->sources.private_headers = strdup("src/main/h/internal");
    }

    if (!p->tests.dir) {
        if      (is_cpp)  p->tests.dir = strdup("src/test/cpp");
        else if (is_java) p->tests.dir = strdup("src/test/java");
        else if (is_rust) p->tests.dir = strdup("src/test/rust");
        else if (is_go)   p->tests.dir = strdup("src/test/go");
        else              p->tests.dir = strdup("src/test/c");
    }
}

/* ---- Section loaders ---- */

static void load_sources(NowSources *dst, const PastaValue *src) {
    if (!src || pasta_type(src) != PASTA_MAP) return;
    dst->dir             = dup_map_str(src, "dir");
    dst->headers         = dup_map_str(src, "headers");
    dst->private_headers = dup_map_str(src, "private");
    dst->pattern         = dup_map_str(src, "pattern");
    dst->mode            = dup_map_str(src, "mode");
    load_strarray(&dst->include, pasta_map_get(src, "include"));
    load_strarray(&dst->exclude, pasta_map_get(src, "exclude"));
}

static void load_compile(NowCompile *dst, const PastaValue *src) {
    if (!src || pasta_type(src) != PASTA_MAP) return;
    load_strarray(&dst->flags,    pasta_map_get(src, "flags"));
    load_strarray(&dst->warnings, pasta_map_get(src, "warnings"));
    load_strarray(&dst->defines,  pasta_map_get(src, "defines"));
    load_strarray(&dst->includes, pasta_map_get(src, "includes"));
    dst->std = dup_map_str(src, "std");
    dst->opt = dup_map_str(src, "opt");
}

static void load_link(NowLink *dst, const PastaValue *src) {
    if (!src || pasta_type(src) != PASTA_MAP) return;
    load_strarray(&dst->flags,   pasta_map_get(src, "flags"));
    load_strarray(&dst->libs,    pasta_map_get(src, "libs"));
    load_strarray(&dst->libdirs, pasta_map_get(src, "libdirs"));
    load_strarray(&dst->archives, pasta_map_get(src, "archives"));
    dst->script      = dup_map_str(src, "script");
    dst->script_body = dup_map_str(src, "script_body");
}

static void load_output(NowOutput *dst, const PastaValue *src) {
    if (!src || pasta_type(src) != PASTA_MAP) return;
    dst->type = dup_map_str(src, "type");
    dst->name = dup_map_str(src, "name");
    dst->dir  = dup_map_str(src, "dir");
}

static void load_deps(NowDepArray *dst, const PastaValue *arr) {
    if (!arr || pasta_type(arr) != PASTA_ARRAY) return;
    size_t n = pasta_count(arr);
    for (size_t i = 0; i < n; i++) {
        const PastaValue *elem = pasta_array_get(arr, i);
        if (!elem || pasta_type(elem) != PASTA_MAP) continue;
        int idx = now_deparray_push(dst);
        if (idx < 0) return;
        NowDep *d = &dst->items[idx];
        d->id          = dup_map_str(elem, "id");
        d->scope       = dup_map_str(elem, "scope");
        d->optional    = get_map_bool(elem, "optional", 0);
        d->is_volatile = get_map_bool(elem, "volatile", 0);
        d->override    = get_map_bool(elem, "override", 0);
        load_strarray(&d->exclude, pasta_map_get(elem, "exclude"));
    }
}

static void load_repos(NowRepoArray *dst, const PastaValue *arr) {
    if (!arr || pasta_type(arr) != PASTA_ARRAY) return;
    size_t n = pasta_count(arr);
    for (size_t i = 0; i < n; i++) {
        const PastaValue *elem = pasta_array_get(arr, i);
        if (!elem) continue;
        int idx = now_repoarray_push(dst);
        if (idx < 0) return;
        NowRepo *r = &dst->items[idx];
        if (pasta_type(elem) == PASTA_STRING) {
            /* String shorthand: just a URL */
            r->url = strdup(pasta_get_string(elem));
        } else if (pasta_type(elem) == PASTA_MAP) {
            r->url      = dup_map_str(elem, "url");
            r->id       = dup_map_str(elem, "id");
            r->release  = get_map_bool(elem, "release", 1);
            r->snapshot = get_map_bool(elem, "snapshot", 0);
            r->auth     = dup_map_str(elem, "auth");
        }
    }
}

static void load_plugins(NowPluginArray *dst, const PastaValue *arr) {
    if (!arr || pasta_type(arr) != PASTA_ARRAY) return;
    size_t n = pasta_count(arr);
    for (size_t i = 0; i < n; i++) {
        const PastaValue *elem = pasta_array_get(arr, i);
        if (!elem || pasta_type(elem) != PASTA_MAP) continue;
        int idx = now_pluginarray_push(dst);
        if (idx < 0) return;
        NowPlugin *pl = &dst->items[idx];
        pl->id      = dup_map_str(elem, "id");
        pl->type    = dup_map_str(elem, "type");
        pl->phase   = dup_map_str(elem, "phase");
        pl->timeout = dup_map_str(elem, "timeout");
        pl->run     = dup_map_str(elem, "run");
        /* Keep config as raw PastaValue* — owned by _pasta_root */
        pl->config  = (void *)pasta_map_get(elem, "config");
    }
}

/* Load langs — handles both "lang: 'c'" scalar and "langs: [...]" array */
static void load_langs(NowProject *p, const PastaValue *root) {
    const PastaValue *langs_val = pasta_map_get(root, "langs");
    const PastaValue *lang_val  = pasta_map_get(root, "lang");

    if (langs_val && pasta_type(langs_val) == PASTA_ARRAY) {
        /* langs: ["c", "c++"] — only string elements for now */
        size_t n = pasta_count(langs_val);
        for (size_t i = 0; i < n; i++) {
            const PastaValue *elem = pasta_array_get(langs_val, i);
            if (elem && pasta_type(elem) == PASTA_STRING)
                now_strarray_push(&p->langs, pasta_get_string(elem));
        }
    } else if (lang_val && pasta_type(lang_val) == PASTA_STRING) {
        /* lang: "c" or lang: "mixed" */
        const char *s = pasta_get_string(lang_val);
        if (strcmp(s, "mixed") == 0) {
            now_strarray_push(&p->langs, "c");
            now_strarray_push(&p->langs, "c++");
        } else {
            now_strarray_push(&p->langs, s);
        }
    }
}

/* ---- Public API ---- */

static void now_sources_init(NowSources *s) {
    memset(s, 0, sizeof(*s));
    now_strarray_init(&s->include);
    now_strarray_init(&s->exclude);
}

static void now_sources_free(NowSources *s) {
    free(s->dir);
    free(s->headers);
    free(s->private_headers);
    free(s->pattern);
    free(s->mode);
    now_strarray_free(&s->include);
    now_strarray_free(&s->exclude);
}

static void now_compile_init(NowCompile *c) {
    memset(c, 0, sizeof(*c));
    now_strarray_init(&c->flags);
    now_strarray_init(&c->warnings);
    now_strarray_init(&c->defines);
    now_strarray_init(&c->includes);
}

static void now_compile_free(NowCompile *c) {
    now_strarray_free(&c->flags);
    now_strarray_free(&c->warnings);
    now_strarray_free(&c->defines);
    now_strarray_free(&c->includes);
    free(c->std);
    free(c->opt);
}

static void now_link_init(NowLink *l) {
    memset(l, 0, sizeof(*l));
    now_strarray_init(&l->flags);
    now_strarray_init(&l->libs);
    now_strarray_init(&l->libdirs);
    now_strarray_init(&l->archives);
}

static void now_link_free(NowLink *l) {
    now_strarray_free(&l->flags);
    now_strarray_free(&l->libs);
    now_strarray_free(&l->libdirs);
    now_strarray_free(&l->archives);
    free(l->script);
    free(l->script_body);
}

NOW_API NowProject *now_project_new(void) {
    NowProject *p = calloc(1, sizeof(NowProject));
    if (!p) return NULL;
    now_strarray_init(&p->langs);
    now_sources_init(&p->sources);
    now_sources_init(&p->tests);
    now_compile_init(&p->compile);
    now_link_init(&p->link);
    now_deparray_init(&p->deps);
    now_repoarray_init(&p->repos);
    now_pluginarray_init(&p->plugins);
    now_strarray_init(&p->components);
    now_strarray_init(&p->vendored);
    now_strarray_init(&p->modules);
    now_strarray_init(&p->private_groups);
    return p;
}

NOW_API void now_project_free(NowProject *p) {
    if (!p) return;
    free(p->group);
    free(p->artifact);
    free(p->version);
    free(p->name);
    free(p->description);
    free(p->url);
    free(p->license);
    now_strarray_free(&p->langs);
    free(p->std);
    now_sources_free(&p->sources);
    now_sources_free(&p->tests);
    free(p->output.type);
    free(p->output.name);
    free(p->output.dir);
    now_compile_free(&p->compile);
    now_link_free(&p->link);
    now_deparray_free(&p->deps);
    now_repoarray_free(&p->repos);
    now_pluginarray_free(&p->plugins);
    free(p->convergence);
    now_strarray_free(&p->components);
    now_strarray_free(&p->vendored);
    now_strarray_free(&p->private_groups);
    now_strarray_free(&p->modules);
    free(p->java.main_class);
    free(p->java.encoding);
    now_strarray_free(&p->java.classpath);
    if (p->_pasta_root)
        pasta_free((PastaValue *)p->_pasta_root);
    free(p);
}

NOW_API NowProject *now_project_load(const char *path, NowResult *result) {
    /* Read file */
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        if (result) {
            result->code = NOW_ERR_IO;
            snprintf(result->message, sizeof(result->message),
                     "cannot open: %s", path);
        }
        return NULL;
    }

    fseek(fp, 0, SEEK_END);
    long len = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    char *buf = malloc((size_t)len + 1);
    if (!buf) {
        fclose(fp);
        if (result) {
            result->code = NOW_ERR_ALLOC;
            snprintf(result->message, sizeof(result->message), "out of memory");
        }
        return NULL;
    }
    size_t nread = fread(buf, 1, (size_t)len, fp);
    buf[nread] = '\0';
    fclose(fp);

    /* Parse Pasta */
    PastaResult pr;
    PastaValue *root = pasta_parse(buf, nread, &pr);
    free(buf);

    if (!root || pr.code != PASTA_OK) {
        if (result) {
            result->code = NOW_ERR_SYNTAX;
            result->line = pr.line;
            result->col  = pr.col;
            snprintf(result->message, sizeof(result->message),
                     "%s:%d:%d: %s", path, pr.line, pr.col, pr.message);
        }
        return NULL;
    }

    if (pasta_type(root) != PASTA_MAP) {
        pasta_free(root);
        if (result) {
            result->code = NOW_ERR_SCHEMA;
            snprintf(result->message, sizeof(result->message),
                     "now.pasta root must be a map");
        }
        return NULL;
    }

    /* Build project */
    NowProject *p = now_project_new();
    if (!p) {
        pasta_free(root);
        if (result) {
            result->code = NOW_ERR_ALLOC;
            snprintf(result->message, sizeof(result->message), "out of memory");
        }
        return NULL;
    }

    p->_pasta_root = root;

    /* Identity (§1.1) */
    p->group       = dup_map_str(root, "group");
    p->artifact    = dup_map_str(root, "artifact");
    p->version     = dup_map_str(root, "version");
    p->name        = dup_map_str(root, "name");
    p->description = dup_map_str(root, "description");
    p->url         = dup_map_str(root, "url");
    p->license     = dup_map_str(root, "license");

    /* Language (§1.2) */
    load_langs(p, root);
    p->std = dup_map_str(root, "std");

    /* Sources (§1.3) */
    load_sources(&p->sources, pasta_map_get(root, "sources"));
    load_sources(&p->tests,   pasta_map_get(root, "tests"));

    /* Output (§1.4) */
    load_output(&p->output, pasta_map_get(root, "output"));

    /* Compile & link (§1.5) */
    load_compile(&p->compile, pasta_map_get(root, "compile"));
    load_link(&p->link,       pasta_map_get(root, "link"));

    /* Dependencies (§1.6) */
    load_deps(&p->deps, pasta_map_get(root, "deps"));

    /* Repositories (§1.7) */
    load_repos(&p->repos, pasta_map_get(root, "repos"));

    /* Convergence (§6.11) */
    p->convergence = dup_map_str(root, "convergence");

    /* Plugins (§10) */
    load_plugins(&p->plugins, pasta_map_get(root, "plugins"));

    /* Dep confusion protection (§8) */
    load_strarray(&p->private_groups, pasta_map_get(root, "private_groups"));

    /* Components (own modules) and vendored (external deps) */
    load_strarray(&p->components, pasta_map_get(root, "components"));
    load_strarray(&p->vendored, pasta_map_get(root, "vendored"));

    /* Modules (§1.11) */
    load_strarray(&p->modules, pasta_map_get(root, "modules"));

    /* Java-specific config */
    const PastaValue *java_map = pasta_map_get(root, "java");
    if (java_map && pasta_type(java_map) == PASTA_MAP) {
        p->java.main_class = dup_map_str(java_map, "main_class");
        p->java.encoding   = dup_map_str(java_map, "encoding");
        load_strarray(&p->java.classpath, pasta_map_get(java_map, "classpath"));
    }

    apply_maven_defaults(p);

    if (result) {
        result->code = NOW_OK;
        result->line = 0;
        result->col  = 0;
        result->message[0] = '\0';
    }

    return p;
}

NOW_API NowProject *now_project_load_string(const char *input, size_t len,
                                     NowResult *result) {
    PastaResult pr;
    PastaValue *root = pasta_parse(input, len, &pr);

    if (!root || pr.code != PASTA_OK) {
        if (result) {
            result->code = NOW_ERR_SYNTAX;
            result->line = pr.line;
            result->col  = pr.col;
            snprintf(result->message, sizeof(result->message),
                     "%d:%d: %s", pr.line, pr.col, pr.message);
        }
        return NULL;
    }

    if (pasta_type(root) != PASTA_MAP) {
        pasta_free(root);
        if (result) {
            result->code = NOW_ERR_SCHEMA;
            snprintf(result->message, sizeof(result->message),
                     "now.pasta root must be a map");
        }
        return NULL;
    }

    NowProject *p = now_project_new();
    if (!p) {
        pasta_free(root);
        if (result) {
            result->code = NOW_ERR_ALLOC;
            snprintf(result->message, sizeof(result->message), "out of memory");
        }
        return NULL;
    }

    p->_pasta_root = root;

    /* Load all fields same as file-based loader */
    p->group       = dup_map_str(root, "group");
    p->artifact    = dup_map_str(root, "artifact");
    p->version     = dup_map_str(root, "version");
    p->name        = dup_map_str(root, "name");
    p->description = dup_map_str(root, "description");
    p->url         = dup_map_str(root, "url");
    p->license     = dup_map_str(root, "license");
    load_langs(p, root);
    p->std = dup_map_str(root, "std");
    load_sources(&p->sources, pasta_map_get(root, "sources"));
    load_sources(&p->tests,   pasta_map_get(root, "tests"));
    load_output(&p->output, pasta_map_get(root, "output"));
    load_compile(&p->compile, pasta_map_get(root, "compile"));
    load_link(&p->link,       pasta_map_get(root, "link"));
    load_deps(&p->deps, pasta_map_get(root, "deps"));
    load_repos(&p->repos, pasta_map_get(root, "repos"));
    p->convergence = dup_map_str(root, "convergence");
    load_plugins(&p->plugins, pasta_map_get(root, "plugins"));
    load_strarray(&p->private_groups, pasta_map_get(root, "private_groups"));
    load_strarray(&p->components, pasta_map_get(root, "components"));
    load_strarray(&p->vendored, pasta_map_get(root, "vendored"));
    load_strarray(&p->modules, pasta_map_get(root, "modules"));

    /* Java-specific config */
    {
        const PastaValue *java_map = pasta_map_get(root, "java");
        if (java_map && pasta_type(java_map) == PASTA_MAP) {
            p->java.main_class = dup_map_str(java_map, "main_class");
            p->java.encoding   = dup_map_str(java_map, "encoding");
            load_strarray(&p->java.classpath, pasta_map_get(java_map, "classpath"));
        }
    }

    apply_maven_defaults(p);

    if (result) {
        result->code = NOW_OK;
        result->line = 0;
        result->col  = 0;
        result->message[0] = '\0';
    }

    return p;
}

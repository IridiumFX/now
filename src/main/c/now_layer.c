/*
 * now_layer.c — Cascading configuration layers (§25)
 *
 * Implements layer loading, section merge, and audit trail.
 */
#include "now_layer.h"
#include "now_fs.h"
#include "pasta.h"
#include "alforno_internal.h"  /* alf_value_clone, alf_map_merge */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

#if !defined(PATH_MAX) || PATH_MAX < 4096
  #undef PATH_MAX
  #define PATH_MAX 4096
#endif

#ifdef _WIN32
  #include <direct.h>
  #define getcwd_compat _getcwd
#else
  #include <unistd.h>
  #define getcwd_compat getcwd
#endif

/* ---- Audit operations ---- */

NOW_API void now_audit_init(NowAuditReport *report) {
    memset(report, 0, sizeof(*report));
}

static void audit_violation_free(NowAuditViolation *v) {
    free(v->section);
    free(v->locked_by);
    free(v->overridden_by);
    free(v->field);
    free(v->override_reason);
    free(v->code);
}

NOW_API void now_audit_free(NowAuditReport *report) {
    for (size_t i = 0; i < report->count; i++)
        audit_violation_free(&report->items[i]);
    free(report->items);
    memset(report, 0, sizeof(*report));
}

static int audit_push(NowAuditReport *report, const char *section,
                       const char *locked_by, const char *overridden_by,
                       const char *field, const char *reason) {
    if (!report) return 0;
    if (report->count >= report->capacity) {
        size_t new_cap = report->capacity ? report->capacity * 2 : 8;
        NowAuditViolation *tmp = realloc(report->items,
                                          new_cap * sizeof(NowAuditViolation));
        if (!tmp) return -1;
        report->items = tmp;
        report->capacity = new_cap;
    }
    NowAuditViolation *v = &report->items[report->count];
    memset(v, 0, sizeof(*v));
    v->section       = section ? strdup(section) : NULL;
    v->locked_by     = locked_by ? strdup(locked_by) : NULL;
    v->overridden_by = overridden_by ? strdup(overridden_by) : NULL;
    v->field         = field ? strdup(field) : NULL;
    v->override_reason = reason ? strdup(reason) : NULL;
    v->code          = strdup("NOW-W0401");
    report->count++;
    return 0;
}

NOW_API char *now_audit_format(const NowAuditReport *report) {
    if (!report || report->count == 0) {
        char *s = strdup("No advisory lock violations.\n");
        return s;
    }

    /* Estimate buffer size */
    size_t bufsize = 256 + report->count * 512;
    char *buf = (char *)malloc(bufsize);
    if (!buf) return NULL;

    int pos = 0;
    pos += snprintf(buf + pos, bufsize - (size_t)pos,
                    "Advisory lock violations in effective configuration:\n\n");

    for (size_t i = 0; i < report->count; i++) {
        const NowAuditViolation *v = &report->items[i];
        pos += snprintf(buf + pos, bufsize - (size_t)pos,
                        "  Section: %s\n"
                        "  Locked by:     %s\n"
                        "  Overridden by: %s\n"
                        "  Field: %s\n",
                        v->section ? v->section : "?",
                        v->locked_by ? v->locked_by : "?",
                        v->overridden_by ? v->overridden_by : "?",
                        v->field ? v->field : "?");
        if (v->override_reason)
            pos += snprintf(buf + pos, bufsize - (size_t)pos,
                            "  Override reason: %s\n", v->override_reason);
        pos += snprintf(buf + pos, bufsize - (size_t)pos,
                        "  Code: %s\n\n", v->code ? v->code : "NOW-W0401");
    }

    pos += snprintf(buf + pos, bufsize - (size_t)pos,
                    "%zu violation(s).\n", report->count);
    return buf;
}

/* ---- Layer section operations ---- */

static void layer_section_free(NowLayerSection *s) {
    free(s->name);
    free(s->description);
    free(s->override_reason);
    /* data is owned by layer's _root — do not free */
}

static int layer_add_section(NowLayer *layer, const char *name,
                              NowSectionPolicy policy, const char *desc,
                              const char *override_reason, void *data) {
    if (layer->section_count >= layer->section_cap) {
        size_t new_cap = layer->section_cap ? layer->section_cap * 2 : 8;
        NowLayerSection *tmp = realloc(layer->sections,
                                        new_cap * sizeof(NowLayerSection));
        if (!tmp) return -1;
        layer->sections = tmp;
        layer->section_cap = new_cap;
    }
    NowLayerSection *s = &layer->sections[layer->section_count];
    memset(s, 0, sizeof(*s));
    s->name = strdup(name);
    s->policy = policy;
    s->description = desc ? strdup(desc) : NULL;
    s->override_reason = override_reason ? strdup(override_reason) : NULL;
    s->data = data;
    layer->section_count++;
    return 0;
}

/* ---- Layer operations ---- */

static void layer_free(NowLayer *layer) {
    free(layer->id);
    free(layer->path);
    for (size_t i = 0; i < layer->section_count; i++)
        layer_section_free(&layer->sections[i]);
    free(layer->sections);
    if (layer->_root)
        pasta_free((PastaValue *)layer->_root);
    memset(layer, 0, sizeof(*layer));
}

static int stack_push_layer(NowLayerStack *stack) {
    if (stack->count >= stack->capacity) {
        size_t new_cap = stack->capacity ? stack->capacity * 2 : 4;
        NowLayer *tmp = realloc(stack->layers, new_cap * sizeof(NowLayer));
        if (!tmp) return -1;
        stack->layers = tmp;
        stack->capacity = new_cap;
    }
    memset(&stack->layers[stack->count], 0, sizeof(NowLayer));
    return (int)stack->count++;
}

/* ---- Built-in baseline layer ---- */

static void init_baseline(NowLayer *layer) {
    layer->id = strdup("now-baseline");
    layer->source = NOW_LAYER_BUILTIN;

    /* Build baseline sections using Pasta values */
    PastaValue *root = pasta_new_map();
    layer->_root = root;

    /* @compile: open, defaults */
    PastaValue *compile = pasta_new_map();
    PastaValue *warnings = pasta_new_array();
    pasta_push(warnings, pasta_new_string("Wall"));
    pasta_push(warnings, pasta_new_string("Wextra"));
    pasta_set(compile, "warnings", warnings);
    pasta_set(compile, "opt", pasta_new_string("debug"));
    pasta_set(root, "compile", compile);
    layer_add_section(layer, "compile", NOW_POLICY_OPEN,
                      "Default compiler settings", NULL, compile);

    /* @repos: open, central registry */
    PastaValue *repos = pasta_new_map();
    PastaValue *registries = pasta_new_array();
    PastaValue *central = pasta_new_map();
    pasta_set(central, "url", pasta_new_string("https://repo.now.build"));
    pasta_set(central, "id", pasta_new_string("central"));
    pasta_set(central, "release", pasta_new_bool(1));
    pasta_set(central, "snapshot", pasta_new_bool(0));
    pasta_push(registries, central);
    pasta_set(repos, "registries", registries);
    pasta_set(root, "repos", repos);
    layer_add_section(layer, "repos", NOW_POLICY_OPEN,
                      "Default package registry", NULL, repos);

    /* @toolchain: open, gcc default */
    PastaValue *tc = pasta_new_map();
    pasta_set(tc, "preset", pasta_new_string("gcc"));
    pasta_set(root, "toolchain", tc);
    layer_add_section(layer, "toolchain", NOW_POLICY_OPEN,
                      "Default toolchain", NULL, tc);

    /* @advisory: locked, default phase guards */
    PastaValue *adv = pasta_new_map();
    PastaValue *guards = pasta_new_map();
    pasta_set(guards, "critical", pasta_new_string("error"));
    pasta_set(guards, "high", pasta_new_string("warn"));
    pasta_set(guards, "medium", pasta_new_string("note"));
    pasta_set(guards, "low", pasta_new_string("note"));
    pasta_set(adv, "phase_guards", guards);
    pasta_set(root, "advisory", adv);
    layer_add_section(layer, "advisory", NOW_POLICY_LOCKED,
                      "Default advisory phase guards", NULL, adv);

    /* @private_groups: open, empty */
    PastaValue *pg = pasta_new_map();
    PastaValue *groups = pasta_new_array();
    pasta_set(pg, "groups", groups);
    pasta_set(root, "private_groups", pg);
    layer_add_section(layer, "private_groups", NOW_POLICY_OPEN,
                      "Private group enforcement", NULL, pg);

    /* @link: open, empty */
    PastaValue *link = pasta_new_map();
    pasta_set(root, "link", link);
    layer_add_section(layer, "link", NOW_POLICY_OPEN, NULL, NULL, link);
}

/* ---- Public API ---- */

NOW_API void now_layer_stack_init(NowLayerStack *stack) {
    memset(stack, 0, sizeof(*stack));
    /* Push baseline as first layer */
    int idx = stack_push_layer(stack);
    if (idx >= 0)
        init_baseline(&stack->layers[idx]);
}

NOW_API void now_layer_stack_free(NowLayerStack *stack) {
    for (size_t i = 0; i < stack->count; i++)
        layer_free(&stack->layers[i]);
    free(stack->layers);
    memset(stack, 0, sizeof(*stack));
}

/* Parse a layer file: a Pasta map where top-level keys are section names.
 * Each section value is a map optionally containing _policy, _description. */
static int parse_layer_document(NowLayer *layer, PastaValue *root) {
    if (!root || pasta_type(root) != PASTA_MAP) return -1;

    layer->_root = root;

    size_t nkeys = pasta_count(root);
    for (size_t i = 0; i < nkeys; i++) {
        const char *key = pasta_map_key(root, i);
        const PastaValue *val = pasta_map_value(root, i);
        if (!key || !val || pasta_type(val) != PASTA_MAP) continue;

        NowSectionPolicy policy = NOW_POLICY_OPEN;
        const char *desc = NULL;
        const char *reason = NULL;

        const PastaValue *pv = pasta_map_get(val, "_policy");
        if (pv && pasta_type(pv) == PASTA_STRING) {
            const char *ps = pasta_get_string(pv);
            if (strcmp(ps, "locked") == 0)
                policy = NOW_POLICY_LOCKED;
        }

        const PastaValue *dv = pasta_map_get(val, "_description");
        if (dv && pasta_type(dv) == PASTA_STRING)
            desc = pasta_get_string(dv);

        const PastaValue *rv = pasta_map_get(val, "_override_reason");
        if (rv && pasta_type(rv) == PASTA_STRING)
            reason = pasta_get_string(rv);

        layer_add_section(layer, key, policy, desc, reason, (void *)val);
    }

    return 0;
}

NOW_API int now_layer_load_file(NowLayerStack *stack, const char *id,
                                 const char *path, NowResult *result) {
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        if (result) {
            result->code = NOW_ERR_IO;
            snprintf(result->message, sizeof(result->message),
                     "cannot open layer: %s", path);
        }
        return -1;
    }

    fseek(fp, 0, SEEK_END);
    long len = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    char *buf = malloc((size_t)len + 1);
    if (!buf) { fclose(fp); return -1; }
    size_t nread = fread(buf, 1, (size_t)len, fp);
    buf[nread] = '\0';
    fclose(fp);

    PastaResult pr;
    PastaValue *root = pasta_parse(buf, nread, &pr);
    free(buf);

    if (!root || pr.code != PASTA_OK) {
        if (result) {
            result->code = NOW_ERR_SYNTAX;
            snprintf(result->message, sizeof(result->message),
                     "layer %s: %s (line %d)", path, pr.message, pr.line);
        }
        return -1;
    }

    int idx = stack_push_layer(stack);
    if (idx < 0) { pasta_free(root); return -1; }

    NowLayer *layer = &stack->layers[idx];
    layer->id = strdup(id);
    layer->source = NOW_LAYER_FILE;
    layer->path = strdup(path);

    if (parse_layer_document(layer, root) != 0) {
        if (result) {
            result->code = NOW_ERR_SCHEMA;
            snprintf(result->message, sizeof(result->message),
                     "layer %s: invalid document format", path);
        }
        return -1;
    }

    return 0;
}

NOW_API int now_layer_push_project(NowLayerStack *stack,
                                    const NowProject *project) {
    if (!stack || !project) return -1;

    int idx = stack_push_layer(stack);
    if (idx < 0) return -1;

    NowLayer *layer = &stack->layers[idx];
    layer->id = strdup("project");
    layer->source = NOW_LAYER_FILE;

    /* Build sections from project fields */
    PastaValue *root = pasta_new_map();
    layer->_root = root;

    /* compile section */
    PastaValue *compile = pasta_new_map();
    if (project->compile.warnings.count > 0) {
        PastaValue *w = pasta_new_array();
        for (size_t i = 0; i < project->compile.warnings.count; i++)
            pasta_push(w, pasta_new_string(project->compile.warnings.items[i]));
        pasta_set(compile, "warnings", w);
    }
    if (project->compile.defines.count > 0) {
        PastaValue *d = pasta_new_array();
        for (size_t i = 0; i < project->compile.defines.count; i++)
            pasta_push(d, pasta_new_string(project->compile.defines.items[i]));
        pasta_set(compile, "defines", d);
    }
    if (project->compile.opt)
        pasta_set(compile, "opt", pasta_new_string(project->compile.opt));
    pasta_set(root, "compile", compile);
    layer_add_section(layer, "compile", NOW_POLICY_OPEN, NULL, NULL, compile);

    /* private_groups section */
    if (project->private_groups.count > 0) {
        PastaValue *pg = pasta_new_map();
        PastaValue *groups = pasta_new_array();
        for (size_t i = 0; i < project->private_groups.count; i++)
            pasta_push(groups, pasta_new_string(project->private_groups.items[i]));
        pasta_set(pg, "groups", groups);
        pasta_set(root, "private_groups", pg);
        layer_add_section(layer, "private_groups", NOW_POLICY_OPEN, NULL, NULL, pg);
    }

    return 0;
}

NOW_API int now_layer_discover(NowLayerStack *stack, const char *basedir,
                                NowResult *result) {
    if (!stack || !basedir) return -1;

    /* Collect .now-layer.pasta paths from basedir upward.
     * We need to load them farthest-first (lower priority) then
     * closest-to-project (higher priority), so collect all paths first. */
    char **paths = NULL;
    size_t npaths = 0;
    size_t path_cap = 0;

    char dir[PATH_MAX];
    snprintf(dir, sizeof(dir), "%s", basedir);

    /* Get home directory for stop condition */
    const char *home = getenv("HOME");
#ifdef _WIN32
    if (!home) home = getenv("USERPROFILE");
#endif

    for (int depth = 0; depth < 64; depth++) {
        /* Check for VCS root — stop */
        char *git = now_path_join(dir, ".git");
        int is_vcs = git && now_path_exists(git);
        free(git);

        /* Check for .now-layer.pasta */
        char *layer_path = now_path_join(dir, ".now-layer.pasta");
        if (layer_path && now_path_exists(layer_path)) {
            if (npaths >= path_cap) {
                path_cap = path_cap ? path_cap * 2 : 4;
                paths = realloc(paths, path_cap * sizeof(char *));
            }
            paths[npaths++] = layer_path;
        } else {
            free(layer_path);
        }

        if (is_vcs) break;

        /* Check home dir stop */
        if (home && strcmp(dir, home) == 0) break;

        /* Go up one directory */
        char *sep = strrchr(dir, '/');
#ifdef _WIN32
        char *sep2 = strrchr(dir, '\\');
        if (sep2 && (!sep || sep2 > sep)) sep = sep2;
#endif
        if (!sep || sep == dir) break;
        *sep = '\0';
    }

    /* Load in reverse order (farthest first = lowest priority) */
    for (int i = (int)npaths - 1; i >= 0; i--) {
        char id[256];
        snprintf(id, sizeof(id), "fs-layer-%d", (int)(npaths - 1 - (size_t)i));
        now_layer_load_file(stack, id, paths[i], result);
        free(paths[i]);
    }
    free(paths);

    return 0;
}

NOW_API const NowLayerSection *now_layer_find_section(const NowLayer *layer,
                                                       const char *name) {
    if (!layer || !name) return NULL;
    for (size_t i = 0; i < layer->section_count; i++) {
        if (strcmp(layer->sections[i].name, name) == 0)
            return &layer->sections[i];
    }
    return NULL;
}

/* ---- Section merge ---- */

NOW_API void now_layer_merge_strarray(NowStrArray *dst,
                                       const NowStrArray *src,
                                       NowSectionPolicy policy) {
    if (!dst || !src) return;
    for (size_t i = 0; i < src->count; i++) {
        const char *s = src->items[i];
        if (!s) continue;

        /* Handle !exclude: prefix in open sections */
        if (policy == NOW_POLICY_OPEN && strncmp(s, "!exclude:", 9) == 0) {
            const char *to_remove = s + 9;
            /* Remove from dst */
            for (size_t j = 0; j < dst->count; j++) {
                if (dst->items[j] && strcmp(dst->items[j], to_remove) == 0) {
                    free(dst->items[j]);
                    /* Shift remaining items down */
                    for (size_t k = j; k + 1 < dst->count; k++)
                        dst->items[k] = dst->items[k + 1];
                    dst->count--;
                    break;
                }
            }
            continue;
        }

        /* Don't add duplicates */
        int found = 0;
        for (size_t j = 0; j < dst->count; j++) {
            if (dst->items[j] && strcmp(dst->items[j], s) == 0) {
                found = 1;
                break;
            }
        }
        if (!found)
            now_strarray_push(dst, s);
    }
}

/* Deep-clone via alforno's utility (handles all types incl. labels) */
#define pasta_clone alf_value_clone

/* Check if a key exists in a map */
static int pasta_map_has(const PastaValue *map, const char *key) {
    return pasta_map_get(map, key) != NULL;
}

/* Merge overlay map into base, returning a NEW map (no duplicate keys).
 * Caller frees old base and uses returned map instead.
 * For locked policy, track overridden fields in audit. */
static PastaValue *merge_pasta_maps(const PastaValue *base,
                                     const PastaValue *overlay,
                                     NowSectionPolicy policy,
                                     const char *section_name,
                                     const char *base_layer_id,
                                     const char *overlay_layer_id,
                                     const char *override_reason,
                                     NowAuditReport *audit) {
    if (!base && !overlay) return pasta_new_map();
    if (!overlay) return pasta_clone(base);
    if (!base) return pasta_clone(overlay);
    if (pasta_type(base) != PASTA_MAP || pasta_type(overlay) != PASTA_MAP)
        return pasta_clone(base);

    PastaValue *result = pasta_new_map();

    /* First pass: copy base entries, possibly modified by overlay */
    size_t bn = pasta_count(base);
    for (size_t i = 0; i < bn; i++) {
        const char *bk = pasta_map_key(base, i);
        if (!bk) continue;

        const PastaValue *oval = pasta_map_get(overlay, bk);
        if (oval && bk[0] != '_') {
            /* Overlay has this key — it's an override */
            if (policy == NOW_POLICY_LOCKED) {
                audit_push(audit, section_name, base_layer_id,
                           overlay_layer_id, bk, override_reason);
            }

            const PastaValue *bval = pasta_map_value(base, i);

            if (pasta_type(oval) == PASTA_ARRAY) {
                PastaValue *merged = pasta_new_array();

                if (bval && pasta_type(bval) == PASTA_ARRAY &&
                    policy == NOW_POLICY_LOCKED) {
                    /* Locked: accumulate (keep base + add new from overlay) */
                    size_t blen = pasta_count(bval);
                    for (size_t j = 0; j < blen; j++)
                        pasta_push(merged, pasta_clone(pasta_array_get(bval, j)));

                    size_t olen = pasta_count(oval);
                    for (size_t j = 0; j < olen; j++) {
                        const PastaValue *oe = pasta_array_get(oval, j);
                        if (!oe) continue;
                        if (pasta_type(oe) == PASTA_STRING &&
                            strncmp(pasta_get_string(oe), "!exclude:", 9) == 0)
                            continue; /* !exclude: ignored in locked mode */
                        /* Dedup */
                        int dup = 0;
                        size_t mn = pasta_count(merged);
                        for (size_t k = 0; k < mn; k++) {
                            const PastaValue *me = pasta_array_get(merged, k);
                            if (me && oe && pasta_type(me) == pasta_type(oe) &&
                                pasta_type(me) == PASTA_STRING &&
                                strcmp(pasta_get_string(me), pasta_get_string(oe)) == 0) {
                                dup = 1; break;
                            }
                        }
                        if (!dup) pasta_push(merged, pasta_clone(oe));
                    }
                } else {
                    /* Open: base + overlay with !exclude: support */
                    if (bval && pasta_type(bval) == PASTA_ARRAY) {
                        size_t blen = pasta_count(bval);
                        for (size_t j = 0; j < blen; j++)
                            pasta_push(merged, pasta_clone(pasta_array_get(bval, j)));
                    }
                    size_t olen = pasta_count(oval);
                    for (size_t j = 0; j < olen; j++) {
                        const PastaValue *oe = pasta_array_get(oval, j);
                        if (!oe) continue;
                        if (pasta_type(oe) == PASTA_STRING &&
                            strncmp(pasta_get_string(oe), "!exclude:", 9) == 0) {
                            const char *to_rm = pasta_get_string(oe) + 9;
                            /* Remove from merged by rebuilding */
                            PastaValue *rebuilt = pasta_new_array();
                            size_t mn = pasta_count(merged);
                            for (size_t k = 0; k < mn; k++) {
                                const PastaValue *me = pasta_array_get(merged, k);
                                if (me && pasta_type(me) == PASTA_STRING &&
                                    strcmp(pasta_get_string(me), to_rm) == 0)
                                    continue;
                                pasta_push(rebuilt, pasta_clone(me));
                            }
                            pasta_free(merged);
                            merged = rebuilt;
                        } else {
                            /* Dedup */
                            int dup = 0;
                            size_t mn = pasta_count(merged);
                            for (size_t k = 0; k < mn; k++) {
                                const PastaValue *me = pasta_array_get(merged, k);
                                if (me && oe && pasta_type(me) == pasta_type(oe) &&
                                    pasta_type(me) == PASTA_STRING &&
                                    strcmp(pasta_get_string(me), pasta_get_string(oe)) == 0) {
                                    dup = 1; break;
                                }
                            }
                            if (!dup) pasta_push(merged, pasta_clone(oe));
                        }
                    }
                }
                pasta_set(result, bk, merged);
            } else if (pasta_type(oval) == PASTA_MAP) {
                if (policy == NOW_POLICY_OPEN) {
                    /* Delegate to alforno for open-policy map merge */
                    pasta_set(result, bk, alf_map_merge(bval, oval));
                } else {
                    /* Recursive merge for locked policy (audit violations) */
                    PastaValue *sub = merge_pasta_maps(bval, oval, policy,
                        section_name, base_layer_id, overlay_layer_id,
                        override_reason, audit);
                    pasta_set(result, bk, sub);
                }
            } else {
                /* Scalar: overlay wins */
                pasta_set(result, bk, pasta_clone(oval));
            }
        } else {
            /* No overlay for this key — keep base value */
            pasta_set(result, bk, pasta_clone(pasta_map_value(base, i)));
        }
    }

    /* Second pass: add overlay keys that weren't in base */
    size_t on = pasta_count(overlay);
    for (size_t i = 0; i < on; i++) {
        const char *ok = pasta_map_key(overlay, i);
        if (!ok || ok[0] == '_') continue;
        if (pasta_map_has(base, ok)) continue; /* already handled above */
        pasta_set(result, ok, pasta_clone(pasta_map_value(overlay, i)));
    }

    return result;
}

NOW_API void *now_layer_merge_section(const NowLayerStack *stack,
                                       const char *section_name,
                                       NowAuditReport *audit) {
    if (!stack || !section_name) return NULL;

    /* Start with empty map */
    PastaValue *effective = pasta_new_map();

    /* Track which layer locked this section */
    const char *locked_by_id = NULL;
    NowSectionPolicy effective_policy = NOW_POLICY_OPEN;

    /* Merge from lowest (baseline) to highest (project) */
    for (size_t i = 0; i < stack->count; i++) {
        const NowLayerSection *sec =
            now_layer_find_section(&stack->layers[i], section_name);
        if (!sec) continue;

        /* If this layer sets the policy to locked, record it */
        if (sec->policy == NOW_POLICY_LOCKED && !locked_by_id) {
            locked_by_id = stack->layers[i].id;
            effective_policy = NOW_POLICY_LOCKED;
        }

        /* If effective policy is locked and this is NOT the locking layer,
         * any changes are violations */
        NowSectionPolicy merge_policy = NOW_POLICY_OPEN;
        const char *violation_src = NULL;
        if (effective_policy == NOW_POLICY_LOCKED &&
            locked_by_id &&
            strcmp(stack->layers[i].id, locked_by_id) != 0) {
            merge_policy = NOW_POLICY_LOCKED;
            violation_src = locked_by_id;
        }

        PastaValue *merged = merge_pasta_maps(
            effective, (const PastaValue *)sec->data,
            merge_policy, section_name,
            violation_src ? violation_src : stack->layers[i].id,
            stack->layers[i].id,
            sec->override_reason, audit);
        pasta_free(effective);
        effective = merged;
    }

    return effective;
}

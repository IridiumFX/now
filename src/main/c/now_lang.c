/*
 * now_lang.c — Language type system
 *
 * Built-in language definitions for C, C++, asm-gas, asm-nasm (§4.2).
 */
#include "now_lang.h"

#include <stdlib.h>
#include <string.h>

/* ---- Built-in type definitions ---- */

/* C language (§4.2) */
static const char *c_source_exts[]       = { ".c", NULL };
static const char *c_preprocessed_exts[] = { ".i", NULL };
static const char *c_header_exts[]       = { ".h", NULL };

static const NowLangType c_types[] = {
    {
        .id         = "c-source",
        .extensions = c_source_exts,
        .role       = NOW_ROLE_SOURCE,
        .tool_var   = "${cc}",
        .produces   = NOW_PRODUCES_OBJECT,
        .output_ext = ".c.o"
    },
    {
        .id         = "c-preprocessed",
        .extensions = c_preprocessed_exts,
        .role       = NOW_ROLE_SOURCE,
        .tool_var   = "${cc}",
        .produces   = NOW_PRODUCES_OBJECT,
        .output_ext = ".i.o"
    },
    {
        .id         = "c-header",
        .extensions = c_header_exts,
        .role       = NOW_ROLE_HEADER,
        .tool_var   = NULL,
        .produces   = NOW_PRODUCES_NONE,
        .output_ext = NULL
    }
};

static const NowLangDef lang_c = {
    .id         = "c",
    .name       = "C",
    .std_flag   = "-std=${std}",
    .types      = c_types,
    .type_count = sizeof(c_types) / sizeof(c_types[0])
};

/* C++ language (§4.2) */
static const char *cxx_source_exts[] = { ".cpp", ".cxx", ".cc", ".C", ".c++", NULL };
static const char *cxx_module_exts[] = { ".cppm", ".ixx", ".ccm", NULL };
static const char *cxx_header_exts[] = { ".hpp", ".hh", ".hxx", ".H", NULL };

static const NowLangType cxx_types[] = {
    {
        .id         = "cxx-source",
        .extensions = cxx_source_exts,
        .role       = NOW_ROLE_SOURCE,
        .tool_var   = "${cxx}",
        .produces   = NOW_PRODUCES_OBJECT,
        .output_ext = ".cpp.o"
    },
    {
        .id         = "cxx-module",
        .extensions = cxx_module_exts,
        .role       = NOW_ROLE_SOURCE,
        .tool_var   = "${cxx}",
        .produces   = NOW_PRODUCES_OBJECT,
        .output_ext = ".cppm.o"
    },
    {
        .id         = "cxx-header",
        .extensions = cxx_header_exts,
        .role       = NOW_ROLE_HEADER,
        .tool_var   = NULL,
        .produces   = NOW_PRODUCES_NONE,
        .output_ext = NULL
    }
};

static const NowLangDef lang_cxx = {
    .id         = "c++",
    .name       = "C++",
    .std_flag   = "-std=${std}",
    .types      = cxx_types,
    .type_count = sizeof(cxx_types) / sizeof(cxx_types[0])
};

/* AT&T assembly (§4.2) */
static const char *gas_source_exts[]     = { ".s", NULL };
static const char *gas_cpp_source_exts[] = { ".S", NULL };
static const char *gas_include_exts[]    = { ".inc", NULL };

static const NowLangType gas_types[] = {
    {
        .id         = "gas-source",
        .extensions = gas_source_exts,
        .role       = NOW_ROLE_SOURCE,
        .tool_var   = "${as}",
        .produces   = NOW_PRODUCES_OBJECT,
        .output_ext = ".s.o"
    },
    {
        .id         = "gas-cpp-source",
        .extensions = gas_cpp_source_exts,
        .role       = NOW_ROLE_SOURCE,
        .tool_var   = "${cc}",
        .produces   = NOW_PRODUCES_OBJECT,
        .output_ext = ".S.o"
    },
    {
        .id         = "gas-include",
        .extensions = gas_include_exts,
        .role       = NOW_ROLE_HEADER,
        .tool_var   = NULL,
        .produces   = NOW_PRODUCES_NONE,
        .output_ext = NULL
    }
};

static const NowLangDef lang_gas = {
    .id         = "asm-gas",
    .name       = "Assembly (AT&T/GAS)",
    .std_flag   = NULL,
    .types      = gas_types,
    .type_count = sizeof(gas_types) / sizeof(gas_types[0])
};

/* NASM assembly (§4.2) */
static const char *nasm_source_exts[]  = { ".asm", NULL };
static const char *nasm_include_exts[] = { ".inc", ".asm.h", NULL };

static const NowLangType nasm_types[] = {
    {
        .id         = "nasm-source",
        .extensions = nasm_source_exts,
        .role       = NOW_ROLE_SOURCE,
        .tool_var   = "${asm}",
        .produces   = NOW_PRODUCES_OBJECT,
        .output_ext = ".asm.o"
    },
    {
        .id         = "nasm-include",
        .extensions = nasm_include_exts,
        .role       = NOW_ROLE_HEADER,
        .tool_var   = NULL,
        .produces   = NOW_PRODUCES_NONE,
        .output_ext = NULL
    }
};

static const NowLangDef lang_nasm = {
    .id         = "asm-nasm",
    .name       = "Assembly (NASM)",
    .std_flag   = NULL,
    .types      = nasm_types,
    .type_count = sizeof(nasm_types) / sizeof(nasm_types[0])
};

/* Java language */
static const char *java_source_exts[] = { ".java", NULL };

static const NowLangType java_types[] = {
    {
        .id         = "java-source",
        .extensions = java_source_exts,
        .role       = NOW_ROLE_SOURCE,
        .tool_var   = "${javac}",
        .produces   = NOW_PRODUCES_OBJECT,
        .output_ext = ".class"
    }
};

static const NowLangDef lang_java = {
    .id         = "java",
    .name       = "Java",
    .std_flag   = "--release ${std}",
    .types      = java_types,
    .type_count = sizeof(java_types) / sizeof(java_types[0])
};

/* ---- Registry ---- */

static const NowLangDef *registry[] = {
    &lang_c,
    &lang_cxx,
    &lang_gas,
    &lang_nasm,
    &lang_java,
    NULL
};

NOW_API void now_lang_registry_init(void) {
    /* Currently all built-in, nothing to initialize dynamically.
     * Future: load custom language definitions from now.pasta. */
}

NOW_API const NowLangDef *now_lang_find(const char *lang_id) {
    if (!lang_id) return NULL;
    for (const NowLangDef **def = registry; *def; def++) {
        if (strcmp((*def)->id, lang_id) == 0)
            return *def;
    }
    return NULL;
}

/* Match file extension against a type's extension list */
static int type_matches_ext(const NowLangType *t, const char *ext) {
    if (!t->extensions) return 0;
    for (const char **e = t->extensions; *e; e++) {
        if (strcmp(ext, *e) == 0) return 1;
    }
    return 0;
}

NOW_API const NowLangType *now_lang_classify(const char *path,
                                      const char *const *active_langs,
                                      size_t lang_count,
                                      const NowLangDef **out_lang) {
    if (!path || !active_langs) return NULL;

    /* Get extension */
    const char *ext = path;
    const char *dot = NULL;
    for (const char *p = path; *p; p++) {
        if (*p == '.') dot = p;
        else if (*p == '/' || *p == '\\') dot = NULL;
    }
    if (!dot) return NULL;
    ext = dot;

    /* Search active languages in declaration order (§4.6) */
    for (size_t i = 0; i < lang_count; i++) {
        const NowLangDef *lang = now_lang_find(active_langs[i]);
        if (!lang) continue;

        for (size_t j = 0; j < lang->type_count; j++) {
            if (type_matches_ext(&lang->types[j], ext)) {
                if (out_lang) *out_lang = lang;
                return &lang->types[j];
            }
        }
    }

    return NULL;
}

NOW_API const char **now_lang_source_exts(const char *const *active_langs,
                                          size_t lang_count) {
    /* Collect all source-role extensions */
    size_t cap = 16;
    size_t count = 0;
    const char **result = malloc(cap * sizeof(const char *));
    if (!result) return NULL;

    for (size_t i = 0; i < lang_count; i++) {
        const NowLangDef *lang = now_lang_find(active_langs[i]);
        if (!lang) continue;

        for (size_t j = 0; j < lang->type_count; j++) {
            if (lang->types[j].role != NOW_ROLE_SOURCE) continue;
            if (!lang->types[j].extensions) continue;

            for (const char **e = lang->types[j].extensions; *e; e++) {
                /* Check for duplicates */
                int dup = 0;
                for (size_t k = 0; k < count; k++) {
                    if (strcmp(result[k], *e) == 0) { dup = 1; break; }
                }
                if (dup) continue;

                if (count + 1 >= cap) {
                    cap *= 2;
                    const char **tmp = realloc(result, cap * sizeof(const char *));
                    if (!tmp) { free(result); return NULL; }
                    result = tmp;
                }
                result[count++] = *e;
            }
        }
    }

    result[count] = NULL;
    return result;
}

NOW_API const char **now_lang_all_exts(const char *const *active_langs,
                                       size_t lang_count) {
    size_t cap = 32;
    size_t count = 0;
    const char **result = malloc(cap * sizeof(const char *));
    if (!result) return NULL;

    for (size_t i = 0; i < lang_count; i++) {
        const NowLangDef *lang = now_lang_find(active_langs[i]);
        if (!lang) continue;

        for (size_t j = 0; j < lang->type_count; j++) {
            if (!lang->types[j].extensions) continue;

            for (const char **e = lang->types[j].extensions; *e; e++) {
                int dup = 0;
                for (size_t k = 0; k < count; k++) {
                    if (strcmp(result[k], *e) == 0) { dup = 1; break; }
                }
                if (dup) continue;

                if (count + 1 >= cap) {
                    cap *= 2;
                    const char **tmp = realloc(result, cap * sizeof(const char *));
                    if (!tmp) { free(result); return NULL; }
                    result = tmp;
                }
                result[count++] = *e;
            }
        }
    }

    result[count] = NULL;
    return result;
}

/*
 * now_import_cmake.c — Convert CMakeLists.txt to now.pasta
 *
 * Parses common CMake patterns (project, add_library, add_executable,
 * target_sources, target_include_directories, target_compile_definitions,
 * target_link_libraries, add_subdirectory) and generates a now.pasta.
 *
 * Not a full CMake evaluator — handles the 80% case for standard projects.
 */
#include "now_export.h"
#include "now_fs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* ---- Simple CMake token extraction ---- */

/* Skip whitespace and comments */
static const char *skip_ws(const char *p) {
    while (*p) {
        while (*p && isspace((unsigned char)*p)) p++;
        if (*p == '#') { while (*p && *p != '\n') p++; continue; }
        break;
    }
    return p;
}

/* Extract a CMake argument (handles quoted and unquoted) */
static const char *next_arg(const char *p, char *out, size_t cap) {
    p = skip_ws(p);
    if (!*p || *p == ')') return NULL;
    size_t i = 0;
    if (*p == '"') {
        p++;
        while (*p && *p != '"' && i < cap - 1) out[i++] = *p++;
        if (*p == '"') p++;
    } else {
        while (*p && !isspace((unsigned char)*p) && *p != ')' && *p != '(' && i < cap - 1)
            out[i++] = *p++;
    }
    out[i] = '\0';
    return p;
}

/* Find a CMake command and return pointer after '(' */
static const char *find_cmd(const char *text, const char *cmd) {
    size_t clen = strlen(cmd);
    const char *p = text;
    while ((p = strstr(p, cmd)) != NULL) {
        /* Check it's a standalone command (not part of a longer word) */
        if (p > text && (isalnum((unsigned char)p[-1]) || p[-1] == '_')) { p++; continue; }
        const char *after = p + clen;
        after = skip_ws(after);
        if (*after == '(') return after + 1;
        p++;
    }
    return NULL;
}

/* ---- Import ---- */

NOW_API int now_import_cmake(const char *cmake_path, const char *out_path,
                               NowResult *result) {
    if (!cmake_path || !out_path) {
        if (result) { result->code = NOW_ERR_SCHEMA; snprintf(result->message, sizeof(result->message), "NULL path"); }
        return -1;
    }

    /* Read CMakeLists.txt */
    FILE *fp = fopen(cmake_path, "rb");
    if (!fp) {
        if (result) { result->code = NOW_ERR_IO; snprintf(result->message, sizeof(result->message), "cannot open %s", cmake_path); }
        return -1;
    }
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    char *text = (char *)malloc((size_t)sz + 1);
    if (!text) { fclose(fp); return -1; }
    fread(text, 1, (size_t)sz, fp);
    text[sz] = '\0';
    fclose(fp);

    /* Extract fields */
    char name[128] = "myproject";
    char version[32] = "1.0.0";
    char output_type[32] = "executable";
    char lang[16] = "c";

    char libs[64][64];      int nlibs = 0;
    char defs[64][128];     int ndefs = 0;
    char incs[32][256];     int nincs = 0;
    char subdirs[32][256];  int nsubs = 0;

    const char *p;
    char arg[512];

    /* project(NAME VERSION X.Y.Z LANGUAGES C CXX) */
    p = find_cmd(text, "project");
    if (p) {
        p = next_arg(p, name, sizeof(name));
        if (p) {
            while ((p = next_arg(p, arg, sizeof(arg))) != NULL) {
                if (strcmp(arg, "VERSION") == 0)
                    p = next_arg(p, version, sizeof(version));
                else if (strcmp(arg, "CXX") == 0 || strcmp(arg, "C++") == 0)
                    strcpy(lang, "c++");
            }
        }
    }

    /* add_library / add_executable */
    p = find_cmd(text, "add_library");
    if (p) {
        p = next_arg(p, arg, sizeof(arg)); /* skip target name */
        if (p && (p = next_arg(p, arg, sizeof(arg)))) {
            if (strcmp(arg, "STATIC") == 0) strcpy(output_type, "static");
            else if (strcmp(arg, "SHARED") == 0) strcpy(output_type, "shared");
        }
    } else {
        p = find_cmd(text, "add_executable");
        if (p) strcpy(output_type, "executable");
    }

    /* target_link_libraries */
    p = find_cmd(text, "target_link_libraries");
    if (p) {
        p = next_arg(p, arg, sizeof(arg)); /* skip target */
        while (p && nlibs < 64) {
            p = next_arg(p, arg, sizeof(arg));
            if (!p) break;
            if (strcmp(arg, "PUBLIC") == 0 || strcmp(arg, "PRIVATE") == 0 ||
                strcmp(arg, "INTERFACE") == 0) continue;
            if (arg[0] == '$') continue; /* skip variables */
            strncpy(libs[nlibs++], arg, 63);
        }
    }

    /* target_compile_definitions */
    p = find_cmd(text, "target_compile_definitions");
    if (p) {
        p = next_arg(p, arg, sizeof(arg)); /* skip target */
        while (p && ndefs < 64) {
            p = next_arg(p, arg, sizeof(arg));
            if (!p) break;
            if (strcmp(arg, "PUBLIC") == 0 || strcmp(arg, "PRIVATE") == 0) continue;
            strncpy(defs[ndefs++], arg, 127);
        }
    }

    /* target_include_directories */
    p = find_cmd(text, "target_include_directories");
    if (p) {
        p = next_arg(p, arg, sizeof(arg)); /* skip target */
        while (p && nincs < 32) {
            p = next_arg(p, arg, sizeof(arg));
            if (!p) break;
            if (strcmp(arg, "PUBLIC") == 0 || strcmp(arg, "PRIVATE") == 0 ||
                strcmp(arg, "SYSTEM") == 0 || strcmp(arg, "INTERFACE") == 0) continue;
            if (arg[0] == '$') continue;
            strncpy(incs[nincs++], arg, 255);
        }
    }

    /* add_subdirectory */
    {
        const char *s = text;
        while ((p = find_cmd(s, "add_subdirectory")) != NULL) {
            if (next_arg(p, arg, sizeof(arg)) && nsubs < 32)
                strncpy(subdirs[nsubs++], arg, 255);
            s = p;
        }
    }

    free(text);

    /* Generate now.pasta */
    FILE *out = fopen(out_path, "w");
    if (!out) {
        if (result) { result->code = NOW_ERR_IO; snprintf(result->message, sizeof(result->message), "cannot write %s", out_path); }
        return -1;
    }

    /* Lowercase the name for artifact */
    char artifact[128];
    strncpy(artifact, name, sizeof(artifact) - 1);
    for (char *c = artifact; *c; c++) if (*c >= 'A' && *c <= 'Z') *c += 32;

    fprintf(out, "{\n");
    fprintf(out, "  group:    \"com.example\",\n");
    fprintf(out, "  artifact: \"%s\",\n", artifact);
    fprintf(out, "  version:  \"%s\",\n", version);
    fprintf(out, "  lang:     \"%s\",\n", lang);
    fprintf(out, "  output:   { type: \"%s\", name: \"%s\" }", output_type, artifact);

    /* Compile section */
    if (ndefs > 0 || nincs > 0) {
        fprintf(out, ",\n\n  compile: {\n");
        fprintf(out, "    warnings: [\"Wall\", \"Wextra\"]");
        if (ndefs > 0) {
            fprintf(out, ",\n    defines: [");
            for (int i = 0; i < ndefs; i++)
                fprintf(out, "%s\"%s\"", i ? ", " : "", defs[i]);
            fprintf(out, "]");
        }
        if (nincs > 0) {
            fprintf(out, ",\n    includes: [");
            for (int i = 0; i < nincs; i++)
                fprintf(out, "%s\"%s\"", i ? ", " : "", incs[i]);
            fprintf(out, "]");
        }
        fprintf(out, "\n  }");
    }

    /* Link section */
    if (nlibs > 0) {
        fprintf(out, ",\n\n  link: {\n    libs: [");
        for (int i = 0; i < nlibs; i++)
            fprintf(out, "%s\"%s\"", i ? ", " : "", libs[i]);
        fprintf(out, "]\n  }");
    }

    /* Vendored from add_subdirectory */
    if (nsubs > 0) {
        fprintf(out, ",\n\n  vendored: [");
        for (int i = 0; i < nsubs; i++)
            fprintf(out, "%s\n    \"%s\"", i ? "," : "", subdirs[i]);
        fprintf(out, "\n  ]");
    }

    fprintf(out, "\n}\n");
    fclose(out);

    if (result) { result->code = NOW_OK; result->message[0] = '\0'; }
    return 0;
}

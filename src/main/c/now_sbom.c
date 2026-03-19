/*
 * now_sbom.c — SBOM generation (Software Bill of Materials)
 *
 * Generates CycloneDX 1.5 JSON from the project descriptor
 * and resolved dependency graph (lock file).
 */

#include "now_pom.h"
#include "now_sbom.h"
#include "now_resolve.h"
#include "now_fs.h"
#include "now_version.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
  #include <windows.h>
  #include <wincrypt.h>
#else
  #include <fcntl.h>
  #include <unistd.h>
#endif

/* ---- helpers ---- */

static void json_escape(char *dst, size_t dstlen, const char *src) {
    size_t j = 0;
    if (!src) { dst[0] = '\0'; return; }
    for (size_t i = 0; src[i] && j < dstlen - 2; i++) {
        if (src[i] == '"' || src[i] == '\\') {
            if (j + 2 >= dstlen) break;
            dst[j++] = '\\';
        }
        dst[j++] = src[i];
    }
    dst[j] = '\0';
}

/* Generate a version-4 UUID (random) */
static void uuid_v4(char *buf, size_t buflen) {
    unsigned char raw[16];

#ifdef _WIN32
    HCRYPTPROV prov;
    if (CryptAcquireContextA(&prov, NULL, NULL, PROV_RSA_FULL,
                              CRYPT_VERIFYCONTEXT)) {
        CryptGenRandom(prov, 16, raw);
        CryptReleaseContext(prov, 0);
    } else {
        /* fallback: seed from time */
        srand((unsigned)time(NULL));
        for (int i = 0; i < 16; i++) raw[i] = (unsigned char)(rand() & 0xff);
    }
#else
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd >= 0) {
        (void)read(fd, raw, 16);
        close(fd);
    } else {
        srand((unsigned)time(NULL));
        for (int i = 0; i < 16; i++) raw[i] = (unsigned char)(rand() & 0xff);
    }
#endif

    /* Set version 4 and variant bits */
    raw[6] = (raw[6] & 0x0f) | 0x40;
    raw[8] = (raw[8] & 0x3f) | 0x80;

    snprintf(buf, buflen,
        "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        raw[0], raw[1], raw[2], raw[3],
        raw[4], raw[5],
        raw[6], raw[7],
        raw[8], raw[9],
        raw[10], raw[11], raw[12], raw[13], raw[14], raw[15]);
}

/* Map output type to CycloneDX component type */
static const char *component_type(const char *output_type) {
    if (!output_type) return "library";
    if (strcmp(output_type, "executable") == 0) return "application";
    return "library";
}

/* Build a purl for a now coordinate */
static void build_purl(char *buf, size_t buflen,
                        const char *group, const char *artifact,
                        const char *version) {
    char eg[256], ea[128], ev[64];
    json_escape(eg, sizeof(eg), group);
    json_escape(ea, sizeof(ea), artifact);
    json_escape(ev, sizeof(ev), version);
    snprintf(buf, buflen, "pkg:now/%s/%s@%s", eg, ea, ev);
}

/* ---- Dynamic buffer for JSON assembly ---- */

typedef struct {
    char  *data;
    size_t len;
    size_t cap;
} DynBuf;

static void dynbuf_init(DynBuf *b) {
    b->cap  = 4096;
    b->data = (char *)malloc(b->cap);
    b->len  = 0;
    if (b->data) b->data[0] = '\0';
}

static void dynbuf_append(DynBuf *b, const char *s) {
    if (!b->data || !s) return;
    size_t slen = strlen(s);
    while (b->len + slen + 1 > b->cap) {
        b->cap *= 2;
        char *tmp = (char *)realloc(b->data, b->cap);
        if (!tmp) return;
        b->data = tmp;
    }
    memcpy(b->data + b->len, s, slen + 1);
    b->len += slen;
}

static void dynbuf_printf(DynBuf *b, const char *fmt, ...) {
    char tmp[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    dynbuf_append(b, tmp);
}

static char *dynbuf_detach(DynBuf *b) {
    char *s = b->data;
    b->data = NULL;
    b->len = b->cap = 0;
    return s;
}

static void dynbuf_free(DynBuf *b) {
    free(b->data);
    b->data = NULL;
    b->len = b->cap = 0;
}

/* ---- Core SBOM generation ---- */

static char *sbom_build_json(const NowProject *project, const char *basedir) {
    if (!project) return NULL;

    DynBuf b;
    dynbuf_init(&b);
    if (!b.data) return NULL;

    /* UUID */
    char uuid[48];
    uuid_v4(uuid, sizeof(uuid));

    /* Timestamp */
    time_t t = time(NULL);
    struct tm *tm = gmtime(&t);
    char timestamp[32];
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%SZ", tm);

    /* Escaped project fields */
    char eg[256], ea[128], ev[64], en[256], ed[512], eu[256], el[128];
    json_escape(eg, sizeof(eg), project->group);
    json_escape(ea, sizeof(ea), project->artifact);
    json_escape(ev, sizeof(ev), project->version);
    json_escape(en, sizeof(en), project->name ? project->name : project->artifact);
    json_escape(ed, sizeof(ed), project->description ? project->description : "");
    json_escape(eu, sizeof(eu), project->url ? project->url : "");
    json_escape(el, sizeof(el), project->license ? project->license : "");

    /* Root purl */
    char root_purl[512];
    build_purl(root_purl, sizeof(root_purl), eg, ea, ev);

    const char *comp_type = component_type(project->output.type);

    /* ---- Header ---- */
    dynbuf_append(&b, "{\n");
    dynbuf_append(&b, "  \"bomFormat\": \"CycloneDX\",\n");
    dynbuf_append(&b, "  \"specVersion\": \"1.5\",\n");
    dynbuf_printf(&b,  "  \"serialNumber\": \"urn:uuid:%s\",\n", uuid);
    dynbuf_append(&b, "  \"version\": 1,\n");

    /* ---- Metadata ---- */
    dynbuf_append(&b, "  \"metadata\": {\n");
    dynbuf_printf(&b,  "    \"timestamp\": \"%s\",\n", timestamp);

    /* Tools */
    dynbuf_append(&b, "    \"tools\": [\n");
    dynbuf_append(&b, "      {\n");
    dynbuf_append(&b, "        \"vendor\": \"now\",\n");
    dynbuf_append(&b, "        \"name\": \"now\",\n");
    dynbuf_printf(&b,  "        \"version\": \"%s\"\n", now_version());
    dynbuf_append(&b, "      }\n");
    dynbuf_append(&b, "    ],\n");

    /* Component (the project itself) */
    dynbuf_append(&b, "    \"component\": {\n");
    dynbuf_printf(&b,  "      \"type\": \"%s\",\n", comp_type);
    dynbuf_printf(&b,  "      \"group\": \"%s\",\n", eg);
    dynbuf_printf(&b,  "      \"name\": \"%s\",\n", ea);
    dynbuf_printf(&b,  "      \"version\": \"%s\",\n", ev);

    if (ed[0]) {
        dynbuf_printf(&b, "      \"description\": \"%s\",\n", ed);
    }

    if (el[0]) {
        dynbuf_append(&b, "      \"licenses\": [\n");
        dynbuf_append(&b, "        {\n");
        dynbuf_printf(&b, "          \"license\": { \"id\": \"%s\" }\n", el);
        dynbuf_append(&b, "        }\n");
        dynbuf_append(&b, "      ],\n");
    }

    dynbuf_printf(&b,  "      \"purl\": \"%s\",\n", root_purl);
    dynbuf_printf(&b,  "      \"bom-ref\": \"%s:%s:%s\"\n", eg, ea, ev);
    dynbuf_append(&b, "    }\n");
    dynbuf_append(&b, "  },\n");

    /* ---- Components (dependencies) ---- */
    dynbuf_append(&b, "  \"components\": [");

    /* Try to load the lock file for resolved deps */
    NowLockFile lf;
    now_lock_init(&lf);
    int have_lock = 0;
    if (basedir) {
        char *lock_path = now_path_join(basedir, "target/now.lock.pasta");
        if (lock_path) {
            have_lock = (now_lock_load(&lf, lock_path) == 0 && lf.count > 0);
            free(lock_path);
        }
    }

    /* Also include declared deps if no lock file */
    if (have_lock) {
        for (size_t i = 0; i < lf.count; i++) {
            const NowLockEntry *e = &lf.entries[i];
            if (!e->group || !e->artifact) continue;

            char cg[256], ca[128], cv[64], cs[64];
            json_escape(cg, sizeof(cg), e->group);
            json_escape(ca, sizeof(ca), e->artifact);
            json_escape(cv, sizeof(cv), e->version ? e->version : "0.0.0");
            json_escape(cs, sizeof(cs), e->scope ? e->scope : "compile");

            char purl[512];
            build_purl(purl, sizeof(purl), cg, ca, cv);

            dynbuf_append(&b, i == 0 ? "\n" : ",\n");
            dynbuf_append(&b, "    {\n");
            dynbuf_append(&b, "      \"type\": \"library\",\n");
            dynbuf_printf(&b, "      \"group\": \"%s\",\n", cg);
            dynbuf_printf(&b, "      \"name\": \"%s\",\n", ca);
            dynbuf_printf(&b, "      \"version\": \"%s\",\n", cv);
            dynbuf_printf(&b, "      \"scope\": \"%s\",\n",
                          strcmp(cs, "test") == 0 ? "excluded" :
                          strcmp(cs, "provided") == 0 ? "optional" : "required");

            if (e->sha256) {
                char sh[128];
                json_escape(sh, sizeof(sh), e->sha256);
                dynbuf_append(&b, "      \"hashes\": [\n");
                dynbuf_append(&b, "        {\n");
                dynbuf_append(&b, "          \"alg\": \"SHA-256\",\n");
                dynbuf_printf(&b, "          \"content\": \"%s\"\n", sh);
                dynbuf_append(&b, "        }\n");
                dynbuf_append(&b, "      ],\n");
            }

            dynbuf_printf(&b, "      \"purl\": \"%s\",\n", purl);
            dynbuf_printf(&b, "      \"bom-ref\": \"%s:%s:%s\"\n", cg, ca, cv);
            dynbuf_append(&b, "    }");
        }
    } else {
        /* Fall back to declared deps from POM */
        for (size_t i = 0; i < project->deps.count; i++) {
            const NowDep *d = &project->deps.items[i];
            if (!d->id) continue;

            /* Parse coordinate "group:artifact:range" */
            NowCoordinate coord;
            if (now_coord_parse(d->id, &coord) != 0) continue;

            char cg[256], ca[128], cv[64];
            json_escape(cg, sizeof(cg), coord.group);
            json_escape(ca, sizeof(ca), coord.artifact);
            json_escape(cv, sizeof(cv), coord.version ? coord.version : "*");

            char cs[64];
            json_escape(cs, sizeof(cs), d->scope ? d->scope : "compile");

            char purl[512];
            build_purl(purl, sizeof(purl), cg, ca, cv);

            dynbuf_append(&b, i == 0 ? "\n" : ",\n");
            dynbuf_append(&b, "    {\n");
            dynbuf_append(&b, "      \"type\": \"library\",\n");
            dynbuf_printf(&b, "      \"group\": \"%s\",\n", cg);
            dynbuf_printf(&b, "      \"name\": \"%s\",\n", ca);
            dynbuf_printf(&b, "      \"version\": \"%s\",\n", cv);
            dynbuf_printf(&b, "      \"scope\": \"%s\",\n",
                          strcmp(cs, "test") == 0 ? "excluded" :
                          strcmp(cs, "provided") == 0 ? "optional" : "required");
            dynbuf_printf(&b, "      \"purl\": \"%s\",\n", purl);
            dynbuf_printf(&b, "      \"bom-ref\": \"%s:%s:%s\"\n", cg, ca, cv);
            dynbuf_append(&b, "    }");

            now_coord_free(&coord);
        }
    }

    dynbuf_append(&b, "\n  ],\n");

    /* ---- Dependencies (relationship graph) ---- */
    dynbuf_append(&b, "  \"dependencies\": [");

    /* Root project depends on all direct deps */
    dynbuf_append(&b, "\n    {\n");
    dynbuf_printf(&b, "      \"ref\": \"%s:%s:%s\",\n", eg, ea, ev);
    dynbuf_append(&b, "      \"dependsOn\": [");

    if (have_lock) {
        for (size_t i = 0; i < lf.count; i++) {
            const NowLockEntry *e = &lf.entries[i];
            if (!e->group || !e->artifact) continue;
            char dg[256], da[128], dv[64];
            json_escape(dg, sizeof(dg), e->group);
            json_escape(da, sizeof(da), e->artifact);
            json_escape(dv, sizeof(dv), e->version ? e->version : "0.0.0");
            dynbuf_printf(&b, "%s\n        \"%s:%s:%s\"",
                          i == 0 ? "" : ",", dg, da, dv);
        }
    } else {
        for (size_t i = 0; i < project->deps.count; i++) {
            const NowDep *d = &project->deps.items[i];
            if (!d->id) continue;
            char eid[512];
            json_escape(eid, sizeof(eid), d->id);
            dynbuf_printf(&b, "%s\n        \"%s\"",
                          i == 0 ? "" : ",", eid);
        }
    }

    dynbuf_append(&b, "\n      ]\n");
    dynbuf_append(&b, "    }");

    /* Per-dependency relationships from lock file */
    if (have_lock) {
        for (size_t i = 0; i < lf.count; i++) {
            const NowLockEntry *e = &lf.entries[i];
            if (!e->group || !e->artifact) continue;

            char dg[256], da[128], dv[64];
            json_escape(dg, sizeof(dg), e->group);
            json_escape(da, sizeof(da), e->artifact);
            json_escape(dv, sizeof(dv), e->version ? e->version : "0.0.0");

            dynbuf_append(&b, ",\n    {\n");
            dynbuf_printf(&b, "      \"ref\": \"%s:%s:%s\",\n", dg, da, dv);
            dynbuf_append(&b, "      \"dependsOn\": [");

            for (size_t j = 0; j < e->dep_count; j++) {
                char tdep[512];
                json_escape(tdep, sizeof(tdep), e->deps[j]);
                dynbuf_printf(&b, "%s\n        \"%s\"",
                              j == 0 ? "" : ",", tdep);
            }

            dynbuf_append(&b, "\n      ]\n");
            dynbuf_append(&b, "    }");
        }
    }

    dynbuf_append(&b, "\n  ]\n");
    dynbuf_append(&b, "}\n");

    now_lock_free(&lf);
    return dynbuf_detach(&b);
}

/* ---- Public API ---- */

NOW_API char *now_sbom_to_json(const NowProject *project, const char *basedir) {
    return sbom_build_json(project, basedir);
}

NOW_API int now_sbom_generate(const NowProject *project, const char *basedir,
                                const char *outpath, NowSbomFormat format,
                                NowResult *result) {
    (void)format; /* Only JSON for now; Pasta variant is future work */

    char *json = sbom_build_json(project, basedir);
    if (!json) {
        if (result) snprintf(result->message, sizeof(result->message),
                             "failed to generate SBOM");
        return -1;
    }

    if (!outpath) {
        /* stdout */
        fputs(json, stdout);
        free(json);
        return 0;
    }

    FILE *fp = fopen(outpath, "w");
    if (!fp) {
        if (result) snprintf(result->message, sizeof(result->message),
                             "cannot write %s", outpath);
        free(json);
        return -1;
    }
    fputs(json, fp);
    fclose(fp);
    free(json);
    return 0;
}

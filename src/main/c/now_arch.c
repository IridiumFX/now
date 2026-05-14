/*
 * now_arch.c — Multi-architecture platform triples (§11)
 */
#include "now_arch.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- Triple parsing ---- */

NOW_API int now_triple_parse(NowTriple *t, const char *str) {
    if (!t) return -1;
    memset(t, 0, sizeof(*t));
    if (!str || !*str) return 0;

    /* Parse "os:arch:variant" with colons as separators */
    const char *p = str;
    const char *c1 = strchr(p, ':');
    if (!c1) {
        /* Single component — treat as os */
        snprintf(t->os, sizeof(t->os), "%s", p);
        return 0;
    }

    /* os */
    size_t len = (size_t)(c1 - p);
    if (len > 0 && len < sizeof(t->os)) {
        memcpy(t->os, p, len);
        t->os[len] = '\0';
    }

    /* arch */
    p = c1 + 1;
    const char *c2 = strchr(p, ':');
    if (!c2) {
        /* Two components: os:arch */
        snprintf(t->arch, sizeof(t->arch), "%s", p);
        return 0;
    }

    len = (size_t)(c2 - p);
    if (len > 0 && len < sizeof(t->arch)) {
        memcpy(t->arch, p, len);
        t->arch[len] = '\0';
    }

    /* variant */
    p = c2 + 1;
    if (*p)
        snprintf(t->variant, sizeof(t->variant), "%s", p);

    return 0;
}

NOW_API void now_triple_fill_from_host(NowTriple *t) {
    if (!t) return;
    const NowTriple *host = now_host_triple_parsed();
    if (t->os[0] == '\0')
        memcpy(t->os, host->os, sizeof(t->os));
    if (t->arch[0] == '\0')
        memcpy(t->arch, host->arch, sizeof(t->arch));
    if (t->variant[0] == '\0')
        memcpy(t->variant, host->variant, sizeof(t->variant));
}

NOW_API void now_triple_format(const NowTriple *t, char *buf, size_t bufsize) {
    if (!t || !buf) return;
    snprintf(buf, bufsize, "%s:%s:%s",
             t->os[0] ? t->os : "unknown",
             t->arch[0] ? t->arch : "unknown",
             t->variant[0] ? t->variant : "none");
}

NOW_API void now_triple_dir(const NowTriple *t, char *buf, size_t bufsize) {
    if (!t || !buf) return;
    snprintf(buf, bufsize, "%s-%s-%s",
             t->os[0] ? t->os : "unknown",
             t->arch[0] ? t->arch : "unknown",
             t->variant[0] ? t->variant : "none");
}

NOW_API int now_triple_cmp(const NowTriple *a, const NowTriple *b) {
    if (!a || !b) return a ? 1 : (b ? -1 : 0);
    int r = strcmp(a->os, b->os);
    if (r != 0) return r;
    r = strcmp(a->arch, b->arch);
    if (r != 0) return r;
    return strcmp(a->variant, b->variant);
}

NOW_API int now_triple_match(const NowTriple *pattern, const NowTriple *concrete) {
    if (!pattern || !concrete) return 0;

    if (strcmp(pattern->os, "*") != 0 && pattern->os[0] != '\0' &&
        strcmp(pattern->os, concrete->os) != 0)
        return 0;

    if (strcmp(pattern->arch, "*") != 0 && pattern->arch[0] != '\0' &&
        strcmp(pattern->arch, concrete->arch) != 0)
        return 0;

    if (strcmp(pattern->variant, "*") != 0 && pattern->variant[0] != '\0' &&
        strcmp(pattern->variant, concrete->variant) != 0)
        return 0;

    return 1;
}

/* ---- Host triple detection ---- */

NOW_API const NowTriple *now_host_triple_parsed(void) {
    static NowTriple host;
    static int initialized = 0;
    if (initialized) return &host;
    memset(&host, 0, sizeof(host));

    /* OS */
#if defined(_WIN32)
    snprintf(host.os, sizeof(host.os), "windows");
#elif defined(__APPLE__)
    snprintf(host.os, sizeof(host.os), "macos");
#elif defined(__linux__)
    snprintf(host.os, sizeof(host.os), "linux");
#elif defined(__FreeBSD__)
    snprintf(host.os, sizeof(host.os), "freebsd");
#elif defined(__OpenBSD__)
    snprintf(host.os, sizeof(host.os), "openbsd");
#else
    snprintf(host.os, sizeof(host.os), "unknown");
#endif

    /* Architecture */
#if defined(__x86_64__) || defined(_M_X64)
    snprintf(host.arch, sizeof(host.arch), "amd64");
#elif defined(__aarch64__) || defined(_M_ARM64)
    snprintf(host.arch, sizeof(host.arch), "arm64");
#elif defined(__arm__) || defined(_M_ARM)
    snprintf(host.arch, sizeof(host.arch), "arm32");
#elif defined(__riscv) && (__riscv_xlen == 64)
    snprintf(host.arch, sizeof(host.arch), "riscv64");
#elif defined(__riscv) && (__riscv_xlen == 32)
    snprintf(host.arch, sizeof(host.arch), "riscv32");
#elif defined(__i386__) || defined(_M_IX86)
    snprintf(host.arch, sizeof(host.arch), "x86");
#elif defined(__wasm32__)
    snprintf(host.arch, sizeof(host.arch), "wasm32");
#else
    snprintf(host.arch, sizeof(host.arch), "unknown");
#endif

    /* Variant */
#if defined(_MSC_VER)
    snprintf(host.variant, sizeof(host.variant), "msvc");
#elif defined(__MINGW32__) || defined(__MINGW64__)
    snprintf(host.variant, sizeof(host.variant), "mingw");
#elif defined(__APPLE__)
    snprintf(host.variant, sizeof(host.variant), "none");
#elif defined(__linux__)
    /* Default to gnu; musl detection would require runtime check */
    snprintf(host.variant, sizeof(host.variant), "gnu");
#elif defined(__FreeBSD__) || defined(__OpenBSD__)
    snprintf(host.variant, sizeof(host.variant), "none");
#else
    snprintf(host.variant, sizeof(host.variant), "none");
#endif

    initialized = 1;
    return &host;
}

NOW_API int now_triple_is_native(const NowTriple *target) {
    if (!target) return 1;
    return now_triple_cmp(target, now_host_triple_parsed()) == 0;
}

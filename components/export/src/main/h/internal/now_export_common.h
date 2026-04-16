/*
 * now_export_common.h — Shared helpers for build system exporters
 */
#ifndef NOW_EXPORT_COMMON_H
#define NOW_EXPORT_COMMON_H

#include "now_pom.h"
#include <string.h>

static inline const char *export_opt_flags(const char *opt) {
    if (!opt) return NULL;
    if (strcmp(opt, "none") == 0)  return "-O0";
    if (strcmp(opt, "debug") == 0) return "-Og";
    if (strcmp(opt, "size") == 0)  return "-Os";
    if (strcmp(opt, "speed") == 0) return "-O2";
    if (strcmp(opt, "lto") == 0)   return "-O2 -flto";
    return NULL;
}

static inline int export_c_std(const char *std_str) {
    if (!std_str) return 11;
    if (strcmp(std_str, "c89") == 0 || strcmp(std_str, "c90") == 0) return 90;
    if (strcmp(std_str, "c99") == 0) return 99;
    if (strcmp(std_str, "c11") == 0) return 11;
    if (strcmp(std_str, "c17") == 0 || strcmp(std_str, "c18") == 0) return 17;
    if (strcmp(std_str, "c23") == 0) return 23;
    return 11;
}

static inline int export_cxx_std(const char *std_str) {
    if (!std_str) return 17;
    if (strcmp(std_str, "c++11") == 0) return 11;
    if (strcmp(std_str, "c++14") == 0) return 14;
    if (strcmp(std_str, "c++17") == 0) return 17;
    if (strcmp(std_str, "c++20") == 0) return 20;
    if (strcmp(std_str, "c++23") == 0) return 23;
    return 17;
}

static inline int export_has_cxx(const NowProject *p) {
    for (size_t i = 0; i < p->langs.count; i++) {
        if (strcmp(p->langs.items[i], "c++") == 0 ||
            strcmp(p->langs.items[i], "cxx") == 0 ||
            strcmp(p->langs.items[i], "cpp") == 0)
            return 1;
    }
    return 0;
}

#endif /* NOW_EXPORT_COMMON_H */

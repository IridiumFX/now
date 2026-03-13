/*
 * now_module.h — C++20 module pre-scan and dependency ordering
 *
 * Scans source files for module declarations (export module, import)
 * and builds a dependency graph to ensure correct compilation order.
 */
#ifndef NOW_MODULE_H
#define NOW_MODULE_H

#include "now.h"
#include <stddef.h>

/* A module unit discovered by pre-scan */
typedef struct {
    char *name;           /* module name (e.g. "mylib.core") */
    char *source_path;    /* path to the source file */
    int   is_interface;   /* 1 = export module (produces BMI), 0 = module impl */
    char *bmi_path;       /* computed BMI output path (set by build phase) */
} NowModuleUnit;

/* A module import edge */
typedef struct {
    char *importer_path;  /* source file that imports */
    char *module_name;    /* module being imported */
} NowModuleImport;

/* Result of module pre-scan */
typedef struct {
    NowModuleUnit   *units;
    size_t           unit_count;
    size_t           unit_cap;
    NowModuleImport *imports;
    size_t           import_count;
    size_t           import_cap;
} NowModuleScan;

/* Ordered compilation schedule */
typedef struct {
    char **paths;      /* source paths in dependency order */
    size_t count;
    size_t capacity;
} NowModuleOrder;

/* Initialize / free scan result */
NOW_API void now_module_scan_init(NowModuleScan *scan);
NOW_API void now_module_scan_free(NowModuleScan *scan);

/* Scan a source file for module declarations.
 * Looks for: export module name; / module name; / import name;
 * Returns 0 on success. */
NOW_API int now_module_scan_file(NowModuleScan *scan, const char *path);

/* Find a module unit by name. Returns NULL if not found. */
NOW_API const NowModuleUnit *now_module_find(const NowModuleScan *scan,
                                              const char *name);

/* Compute compilation order: module interfaces first (in topo order),
 * then implementation units, then regular sources.
 * non_module_sources are appended at the end (can be compiled in parallel).
 * Returns 0 on success, -1 on cycle. */
NOW_API int now_module_order(const NowModuleScan *scan,
                              const char *const *all_sources,
                              size_t source_count,
                              NowModuleOrder *order);

/* Free order result */
NOW_API void now_module_order_free(NowModuleOrder *order);

/* Check if a source file is a module unit (has export module or module decl) */
NOW_API int now_module_is_module_file(const NowModuleScan *scan,
                                       const char *path);

/* Get the BMI output path for a module.
 * Convention: target/bmi/<module_name>.pcm (or .gcm/.ifc per toolchain) */
NOW_API char *now_module_bmi_path(const char *target_dir,
                                   const char *module_name,
                                   int is_msvc);

#endif /* NOW_MODULE_H */

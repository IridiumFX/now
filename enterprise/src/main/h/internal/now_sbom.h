/*
 * now_sbom.h — SBOM generation (Software Bill of Materials)
 *
 * Generates CycloneDX 1.5 JSON from the project descriptor
 * and resolved dependency graph (lock file).
 */
#ifndef NOW_SBOM_H
#define NOW_SBOM_H

#include "now.h"
#include "now_pom.h"
#include "now_resolve.h"

/* SBOM output format */
typedef enum {
    NOW_SBOM_CYCLONEDX_JSON = 0,  /* CycloneDX 1.5 JSON (default) */
    NOW_SBOM_CYCLONEDX_PASTA      /* CycloneDX as Pasta (same fields) */
} NowSbomFormat;

/* Generate an SBOM from the project and its lock file.
 * If lock_path is NULL, looks for target/now.lock.pasta.
 * If no lock file exists, uses declared deps (unresolved).
 * outpath: destination file, or NULL for stdout.
 * Returns 0 on success. */
NOW_API int now_sbom_generate(const NowProject *project, const char *basedir,
                                const char *outpath, NowSbomFormat format,
                                NowResult *result);

/* Generate SBOM as a malloc'd JSON string.
 * Caller must free the returned string. */
NOW_API char *now_sbom_to_json(const NowProject *project, const char *basedir);

#endif /* NOW_SBOM_H */

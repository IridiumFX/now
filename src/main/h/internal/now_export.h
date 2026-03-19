/*
 * now_export.h — Export now.pasta as CMakeLists.txt (or other build systems)
 *
 * Generates a build system file from the NowProject descriptor,
 * providing a zero-lock-in escape hatch.
 */
#ifndef NOW_EXPORT_H
#define NOW_EXPORT_H

#include "now.h"

/* Generate a CMakeLists.txt from a project descriptor.
 * outpath: destination file path (e.g. "CMakeLists.txt").
 * Returns 0 on success. */
NOW_API int now_export_cmake(const NowProject *project, const char *basedir,
                              const char *outpath, NowResult *result);

/* Generate a Makefile from a project descriptor.
 * outpath: destination file path (e.g. "Makefile").
 * Returns 0 on success. */
NOW_API int now_export_make(const NowProject *project, const char *basedir,
                              const char *outpath, NowResult *result);

/* Generate a meson.build from a project descriptor.
 * outpath: destination file path (e.g. "meson.build").
 * Returns 0 on success. */
NOW_API int now_export_meson(const NowProject *project, const char *basedir,
                               const char *outpath, NowResult *result);

/* Generate a BUILD.bazel from a project descriptor.
 * outpath: destination file path (e.g. "BUILD.bazel").
 * Returns 0 on success. */
NOW_API int now_export_bazel(const NowProject *project, const char *basedir,
                               const char *outpath, NowResult *result);

/* Generate a pom.xml from a project descriptor.
 * outpath: destination file path (e.g. "pom.xml").
 * Returns 0 on success. */
NOW_API int now_export_maven(const NowProject *project, const char *basedir,
                               const char *outpath, NowResult *result);

/* Parse a pom.xml and produce a NowProject.
 * Returns allocated project or NULL on error. */
NOW_API NowProject *now_import_maven(const char *pom_path, NowResult *result);

/* Write a now.pasta file from a NowProject.
 * Returns 0 on success. */
NOW_API int now_import_maven_write(const NowProject *project,
                                     const char *outpath, NowResult *result);

#endif /* NOW_EXPORT_H */

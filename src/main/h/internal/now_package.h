/*
 * now_package.h — Package and install phases (§5.3, §5.5)
 *
 * Assembles build output into a distributable archive and
 * installs artifacts into the local repo (~/.now/repo/).
 */
#ifndef NOW_PACKAGE_H
#define NOW_PACKAGE_H

#include "now.h"

/* Get the platform triple string for the current host.
 * Returns a static string like "linux-x86_64-gnu" or "windows-x86_64-msvc". */
NOW_API const char *now_host_triple(void);

/* Package phase: assemble target/pkg/{artifact}-{version}-{triple}.basta
 * containing the descriptor, headers, and built libraries/executables
 * as a single Basta file with embedded binary blobs.
 * Requires a successful build first.
 * Returns 0 on success. */
NOW_API int now_package(const NowProject *project, const char *basedir,
                        int verbose, NowResult *result);

/* Extract a .basta package into a directory.
 * Writes descriptor (now.pasta), headers (h/), libraries (lib/), and
 * executables (bin/) into dest_dir.
 * Returns 0 on success. */
NOW_API int now_basta_extract(const char *basta_path, const char *dest_dir,
                               int verbose, NowResult *result);

/* Install phase: extract/copy the packaged artifact into
 * ~/.now/repo/{group-path}/{artifact}/{version}/
 * Returns 0 on success. */
NOW_API int now_install(const NowProject *project, const char *basedir,
                        int verbose, NowResult *result);

/* Publish phase: upload package archive, SHA-256, and descriptor to
 * a remote registry via HTTP PUT.
 * repo_url: explicit registry URL (e.g. from --repo flag), or NULL to
 *           use the first repo in the project descriptor.
 * Returns 0 on success. */
NOW_API int now_publish(const NowProject *project, const char *basedir,
                        const char *repo_url, int verbose, NowResult *result);

/* Yank a published artifact version.
 * POSTs to /artifact/{g}/{a}/{v}/{file}/yank with optional reason.
 * Returns 0 on success. */
NOW_API int now_publish_yank(const char *registry_url,
                              const char *group, const char *artifact,
                              const char *version, const char *reason,
                              int verbose, NowResult *result);

#endif /* NOW_PACKAGE_H */

/*
 * now_graph.h — Build graph cache for distributed builds
 *
 * Serializes the build manifest (source→object mappings with dep hashes)
 * as a pasta document and pushes/pulls from the remote graph cache.
 * Key = SHA-256(lockfile_hash + compiler + flags_hash).
 *
 * When a CI machine restores a graph, it gets the full manifest without
 * recompiling — then incremental build skips all unchanged sources.
 */
#ifndef NOW_GRAPH_H
#define NOW_GRAPH_H

#include "now_manifest.h"
#include "now_remote.h"
#include "now.h"

/* Compute the graph cache key from project inputs.
 * Key = SHA-256(lockfile_hash + compiler_path + flags_hash).
 * Returns malloc'd hex string, caller frees. */
NOW_API char *now_graph_key(const char *lockfile_path,
                             const char *compiler_path,
                             const char *flags_hash);

/* Serialize manifest to pasta string for caching.
 * Returns malloc'd string, caller frees. *out_len receives length. */
NOW_API char *now_graph_serialize(const NowManifest *manifest, size_t *out_len);

/* Deserialize pasta string into manifest.
 * Returns 0 on success, -1 on error. */
NOW_API int now_graph_deserialize(const char *data, size_t len,
                                   NowManifest *manifest);

/* Push manifest to remote graph cache.
 * Uses /objects/_graphs/{key} on the remote cache server.
 * Returns 0 on success, -1 on error (silent, never fails build). */
NOW_API int now_graph_push(const NowRemoteCacheConfig *cfg,
                            const char *graph_key,
                            const NowManifest *manifest);

/* Pull manifest from remote graph cache.
 * Returns 0 if found and deserialized, -1 on miss or error. */
NOW_API int now_graph_pull(const NowRemoteCacheConfig *cfg,
                            const char *graph_key,
                            NowManifest *manifest);

#endif /* NOW_GRAPH_H */

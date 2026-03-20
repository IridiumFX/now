/*
 * now_graph.c — Build graph cache for distributed builds
 *
 * Serializes/deserializes the build manifest as pasta for remote caching.
 * Enables CI machines to share build state across runs.
 */
#include "now_graph.h"
#include "now_fs.h"
#include "pico_http.h"
#include "now_auth.h"

#include <pasta.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- Graph cache key ---- */

NOW_API char *now_graph_key(const char *lockfile_path,
                             const char *compiler_path,
                             const char *flags_hash) {
    if (!compiler_path || !flags_hash) return NULL;

    /* Hash the lockfile if it exists, otherwise use empty string */
    char *lock_hash = NULL;
    if (lockfile_path && now_path_exists(lockfile_path))
        lock_hash = now_sha256_file(lockfile_path);

    const char *lh = lock_hash ? lock_hash : "no-lockfile";
    size_t total = strlen(lh) + 1 + strlen(compiler_path) + 1 + strlen(flags_hash);
    char *combined = (char *)malloc(total + 1);
    if (!combined) { free(lock_hash); return NULL; }

    snprintf(combined, total + 1, "%s\n%s\n%s", lh, compiler_path, flags_hash);
    char *key = now_sha256_string(combined, total);
    free(combined);
    free(lock_hash);
    return key;
}

/* ---- Serialize manifest to pasta string ---- */

NOW_API char *now_graph_serialize(const NowManifest *manifest, size_t *out_len) {
    if (!manifest) return NULL;

    PastaValue *root = pasta_new_map();
    if (!root) return NULL;

    pasta_set(root, "type", pasta_new_string("now-build-graph"));
    pasta_set(root, "version", pasta_new_number(1));

    if (manifest->link_flags_hash)
        pasta_set(root, "link_flags_hash",
                  pasta_new_string(manifest->link_flags_hash));

    PastaValue *entries = pasta_new_array();
    for (size_t i = 0; i < manifest->count; i++) {
        const NowManifestEntry *e = &manifest->entries[i];
        PastaValue *entry = pasta_new_map();

        if (e->source)      pasta_set(entry, "source", pasta_new_string(e->source));
        if (e->object)      pasta_set(entry, "object", pasta_new_string(e->object));
        if (e->source_hash) pasta_set(entry, "source_hash", pasta_new_string(e->source_hash));
        if (e->flags_hash)  pasta_set(entry, "flags_hash", pasta_new_string(e->flags_hash));
        pasta_set(entry, "mtime", pasta_new_number((double)e->mtime));

        if (e->dep_count > 0 && e->deps && e->dep_hashes) {
            PastaValue *dep_arr = pasta_new_array();
            for (size_t d = 0; d < e->dep_count; d++) {
                PastaValue *dep = pasta_new_map();
                pasta_set(dep, "path", pasta_new_string(e->deps[d]));
                pasta_set(dep, "hash", pasta_new_string(e->dep_hashes[d]));
                pasta_push(dep_arr, dep);
            }
            pasta_set(entry, "deps", dep_arr);
        }

        pasta_push(entries, entry);
    }
    pasta_set(root, "entries", entries);

    char *out = pasta_write(root, PASTA_SORTED);
    pasta_free(root);
    if (!out) return NULL;

    if (out_len) *out_len = strlen(out);
    return out;
}

/* ---- Deserialize pasta string into manifest ---- */

NOW_API int now_graph_deserialize(const char *data, size_t len,
                                   NowManifest *manifest) {
    if (!data || len == 0 || !manifest) return -1;

    now_manifest_init(manifest);

    PastaResult pr;
    PastaValue *root = pasta_parse(data, len, &pr);
    if (!root || pasta_type(root) != PASTA_MAP) {
        pasta_free(root);
        return -1;
    }

    /* Verify type marker */
    const PastaValue *type = pasta_map_get(root, "type");
    if (!type || pasta_type(type) != PASTA_STRING ||
        strcmp(pasta_get_string(type), "now-build-graph") != 0) {
        pasta_free(root);
        return -1;
    }

    /* Link flags hash */
    const PastaValue *lfh = pasta_map_get(root, "link_flags_hash");
    if (lfh && pasta_type(lfh) == PASTA_STRING)
        manifest->link_flags_hash = strdup(pasta_get_string(lfh));

    /* Entries */
    const PastaValue *entries = pasta_map_get(root, "entries");
    if (entries && pasta_type(entries) == PASTA_ARRAY) {
        size_t n = pasta_count(entries);
        for (size_t i = 0; i < n; i++) {
            const PastaValue *entry = pasta_array_get(entries, i);
            if (!entry || pasta_type(entry) != PASTA_MAP) continue;

            const PastaValue *v;
            const char *source = NULL, *object = NULL;
            const char *src_hash = NULL, *fhash = NULL;
            long long mtime = 0;

            v = pasta_map_get(entry, "source");
            if (v && pasta_type(v) == PASTA_STRING) source = pasta_get_string(v);
            v = pasta_map_get(entry, "object");
            if (v && pasta_type(v) == PASTA_STRING) object = pasta_get_string(v);
            v = pasta_map_get(entry, "source_hash");
            if (v && pasta_type(v) == PASTA_STRING) src_hash = pasta_get_string(v);
            v = pasta_map_get(entry, "flags_hash");
            if (v && pasta_type(v) == PASTA_STRING) fhash = pasta_get_string(v);
            v = pasta_map_get(entry, "mtime");
            if (v && pasta_type(v) == PASTA_NUMBER) mtime = (long long)pasta_get_number(v);

            if (source) {
                now_manifest_set(manifest, source, object, src_hash, fhash, mtime);

                /* Deps */
                const PastaValue *deps = pasta_map_get(entry, "deps");
                if (deps && pasta_type(deps) == PASTA_ARRAY) {
                    size_t dc = pasta_count(deps);
                    char **paths = (char **)calloc(dc, sizeof(char *));
                    char **hashes = (char **)calloc(dc, sizeof(char *));
                    size_t actual = 0;
                    if (paths && hashes) {
                        for (size_t d = 0; d < dc; d++) {
                            const PastaValue *dep = pasta_array_get(deps, d);
                            if (!dep || pasta_type(dep) != PASTA_MAP) continue;
                            const PastaValue *dp = pasta_map_get(dep, "path");
                            const PastaValue *dh = pasta_map_get(dep, "hash");
                            if (dp && dh && pasta_type(dp) == PASTA_STRING &&
                                pasta_type(dh) == PASTA_STRING) {
                                paths[actual] = strdup(pasta_get_string(dp));
                                hashes[actual] = strdup(pasta_get_string(dh));
                                actual++;
                            }
                        }
                        if (actual > 0)
                            now_manifest_set_deps(manifest, source,
                                (const char **)paths, (const char **)hashes, actual);
                    }
                    for (size_t d = 0; d < actual; d++) {
                        free(paths[d]);
                        free(hashes[d]);
                    }
                    free(paths);
                    free(hashes);
                }
            }
        }
    }

    pasta_free(root);
    return 0;
}

/* ---- Remote graph push/pull ---- */

NOW_API int now_graph_push(const NowRemoteCacheConfig *cfg,
                            const char *graph_key,
                            const NowManifest *manifest) {
    if (!cfg || !cfg->url || !cfg->push || !graph_key || !manifest) return -1;

    size_t data_len = 0;
    char *data = now_graph_serialize(manifest, &data_len);
    if (!data) return -1;

    /* Build URL: {base}/objects/_graphs/{key} */
    size_t blen = strlen(cfg->url);
    while (blen > 0 && cfg->url[blen - 1] == '/') blen--;
    size_t url_len = blen + 20 + strlen(graph_key);
    char *url = (char *)malloc(url_len + 1);
    if (!url) { free(data); return -1; }
    snprintf(url, url_len + 1, "%.*s/objects/_graphs/%s",
             (int)blen, cfg->url, graph_key);

    /* Build request with auth */
    PicoHttpOptions opts;
    memset(&opts, 0, sizeof(opts));
    opts.connect_timeout_ms = 3000;
    opts.timeout_ms = 30000;

    PicoHttpHeader auth_hdr;
    char auth_buf[512];
    memset(&auth_hdr, 0, sizeof(auth_hdr));

    if (cfg->token && cfg->token[0]) {
        snprintf(auth_buf, sizeof(auth_buf), "Bearer %s", cfg->token);
        auth_hdr.name = "Authorization";
        auth_hdr.value = auth_buf;
        opts.headers = &auth_hdr;
        opts.header_count = 1;
    } else if (cfg->url) {
        char *jwt = now_token_cache_get(cfg->url);
        if (jwt) {
            snprintf(auth_buf, sizeof(auth_buf), "Bearer %s", jwt);
            free(jwt);
            auth_hdr.name = "Authorization";
            auth_hdr.value = auth_buf;
            opts.headers = &auth_hdr;
            opts.header_count = 1;
        }
    }

    PicoHttpResponse res;
    memset(&res, 0, sizeof(res));
    int rc = pico_http_request("PUT", url, "application/x-pasta",
                                data, data_len, &opts, &res);
    free(url);
    free(data);

    int ok = (rc == PICO_OK && (res.status == 200 || res.status == 201));
    pico_http_response_free(&res);
    return ok ? 0 : -1;
}

NOW_API int now_graph_pull(const NowRemoteCacheConfig *cfg,
                            const char *graph_key,
                            NowManifest *manifest) {
    if (!cfg || !cfg->url || !graph_key || !manifest) return -1;

    /* Build URL */
    size_t blen = strlen(cfg->url);
    while (blen > 0 && cfg->url[blen - 1] == '/') blen--;
    size_t url_len = blen + 20 + strlen(graph_key);
    char *url = (char *)malloc(url_len + 1);
    if (!url) return -1;
    snprintf(url, url_len + 1, "%.*s/objects/_graphs/%s",
             (int)blen, cfg->url, graph_key);

    PicoHttpOptions opts;
    memset(&opts, 0, sizeof(opts));
    opts.connect_timeout_ms = 3000;
    opts.timeout_ms = 30000;

    PicoHttpResponse res;
    memset(&res, 0, sizeof(res));
    int rc = pico_http_request("GET", url, NULL, NULL, 0, &opts, &res);
    free(url);

    if (rc != PICO_OK || res.status != 200 || !res.body || res.body_len == 0) {
        pico_http_response_free(&res);
        return -1;
    }

    rc = now_graph_deserialize(res.body, res.body_len, manifest);
    pico_http_response_free(&res);
    return rc;
}

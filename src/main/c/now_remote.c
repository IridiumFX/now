/*
 * now_remote.c — Remote object cache for distributed builds
 *
 * HTTP-based remote object cache using the pico_http client.
 * Protocol: GET/PUT /objects/{cache_key}{.o|.obj}
 * Config: ~/.now/config.pasta -> object_cache section.
 */

#include "now_remote.h"
#include "now_fs.h"
#include "pico_http.h"

#include <pasta.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ---- File helpers ---- */

static char *read_file_bin(const char *path, size_t *out_len) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    if (sz < 0) { fclose(fp); return NULL; }
    fseek(fp, 0, SEEK_SET);
    char *buf = (char *)malloc((size_t)sz);
    if (!buf) { fclose(fp); return NULL; }
    size_t n = fread(buf, 1, (size_t)sz, fp);
    fclose(fp);
    *out_len = n;
    return buf;
}

static int ensure_parent_dir(const char *path) {
    /* Find last separator and create parent dirs */
    const char *sep = strrchr(path, '/');
    if (!sep) sep = strrchr(path, '\\');
    if (!sep) return 0;

    size_t parent_len = (size_t)(sep - path);
    char *parent = (char *)malloc(parent_len + 1);
    if (!parent) return -1;
    memcpy(parent, path, parent_len);
    parent[parent_len] = '\0';
    now_mkdir_p(parent);
    free(parent);
    return 0;
}

/* ---- Config parsing ---- */

NOW_API int now_remote_config_parse(const char *pasta_str, size_t len,
                                     NowRemoteCacheConfig *cfg) {
    if (!cfg) return -1;
    memset(cfg, 0, sizeof(*cfg));

    if (!pasta_str || len == 0) return -1;

    PastaResult pr;
    PastaValue *root = pasta_parse(pasta_str, len, &pr);
    if (!root || pasta_type(root) != PASTA_MAP) {
        pasta_free(root);
        return -1;
    }

    const PastaValue *oc = pasta_map_get(root, "object_cache");
    if (!oc || pasta_type(oc) != PASTA_MAP) {
        pasta_free(root);
        return -1;
    }

    const PastaValue *v;

    v = pasta_map_get(oc, "url");
    if (!v || pasta_type(v) != PASTA_STRING) {
        pasta_free(root);
        return -1;  /* url is required */
    }
    cfg->url = strdup(pasta_get_string(v));

    v = pasta_map_get(oc, "token");
    if (v && pasta_type(v) == PASTA_STRING)
        cfg->token = strdup(pasta_get_string(v));

    v = pasta_map_get(oc, "push");
    if (v && pasta_type(v) == PASTA_BOOL)
        cfg->push = pasta_get_bool(v) ? 1 : 0;

    pasta_free(root);
    return 0;
}

NOW_API int now_remote_config_load(NowRemoteCacheConfig *cfg) {
    if (!cfg) return -1;
    memset(cfg, 0, sizeof(*cfg));

    const char *home = NULL;
#ifdef _WIN32
    home = getenv("USERPROFILE");
    if (!home) home = getenv("HOME");
#else
    home = getenv("HOME");
#endif
    if (!home) return -1;

    char *dot_now = now_path_join(home, ".now");
    if (!dot_now) return -1;
    char *config_path = now_path_join(dot_now, "config.pasta");
    free(dot_now);
    if (!config_path) return -1;

    size_t flen;
    char *data = read_file_bin(config_path, &flen);
    free(config_path);
    if (!data) return -1;

    int rc = now_remote_config_parse(data, flen, cfg);
    free(data);
    return rc;
}

NOW_API void now_remote_config_free(NowRemoteCacheConfig *cfg) {
    if (!cfg) return;
    free(cfg->url);
    free(cfg->token);
    cfg->url = NULL;
    cfg->token = NULL;
    cfg->push = 0;
}

/* ---- HTTP helpers ---- */

/* Build an options struct with auth header and fast timeouts */
static void build_opts(const NowRemoteCacheConfig *cfg,
                        PicoHttpOptions *opts,
                        PicoHttpHeader *auth_hdr,
                        char *auth_buf, size_t auth_buf_len) {
    memset(opts, 0, sizeof(*opts));
    opts->connect_timeout_ms = 3000;   /* 3s connect — never block builds */
    opts->timeout_ms = 30000;          /* 30s read/write */

    if (cfg->token && cfg->token[0]) {
        snprintf(auth_buf, auth_buf_len, "Bearer %s", cfg->token);
        auth_hdr->name = "Authorization";
        auth_hdr->value = auth_buf;
        opts->headers = auth_hdr;
        opts->header_count = 1;
    }
}

/* Build full URL: {base_url}/objects/{key}{ext} */
static char *build_url(const char *base_url, const char *cache_key,
                        const char *obj_ext) {
    const char *ext = obj_ext ? obj_ext : ".o";
    /* Strip trailing slash from base_url */
    size_t blen = strlen(base_url);
    while (blen > 0 && base_url[blen - 1] == '/') blen--;

    size_t url_len = blen + 10 + strlen(cache_key) + strlen(ext) + 1;
    char *url = (char *)malloc(url_len);
    if (!url) return NULL;
    snprintf(url, url_len, "%.*s/objects/%s%s", (int)blen, base_url,
             cache_key, ext);
    return url;
}

/* ---- Circuit breaker ----
 * After one connection failure OR slow response (>1s), skip all subsequent
 * remote ops for this build. Avoids timeout penalties when server is
 * unreachable or experiencing high latency (e.g. IPv6 fallback). */
static int remote_tripped = 0;

NOW_API void now_remote_reset(void) { remote_tripped = 0; }

/* ---- Remote cache operations ---- */

NOW_API int now_remote_cache_restore(const NowRemoteCacheConfig *cfg,
                                      const char *cache_key,
                                      const char *dst_path,
                                      const char *obj_ext) {
    if (!cfg || !cfg->url || !cache_key || !dst_path) return -1;
    if (remote_tripped) return -1;

    char *url = build_url(cfg->url, cache_key, obj_ext);
    if (!url) return -1;

    PicoHttpOptions opts;
    PicoHttpHeader auth_hdr;
    char auth_buf[512];
    memset(&auth_hdr, 0, sizeof(auth_hdr));
    build_opts(cfg, &opts, &auth_hdr, auth_buf, sizeof(auth_buf));

    PicoHttpResponse res;
    memset(&res, 0, sizeof(res));
    clock_t t0 = clock();
    int rc = pico_http_request("GET", url, NULL, NULL, 0, &opts, &res);
    double elapsed = (double)(clock() - t0) / CLOCKS_PER_SEC;
    free(url);

    if (rc != PICO_OK) {
        /* Connection failed — trip circuit breaker */
        remote_tripped = 1;
        pico_http_response_free(&res);
        return -1;
    }
    if (elapsed > 1.0) {
        /* Slow response — trip circuit breaker (likely IPv6 fallback or WAN) */
        remote_tripped = 1;
    }
    if (res.status != 200 || !res.body || res.body_len == 0) {
        pico_http_response_free(&res);
        return -1;
    }

    /* Write response body to dst_path */
    ensure_parent_dir(dst_path);
    FILE *fp = fopen(dst_path, "wb");
    if (!fp) {
        pico_http_response_free(&res);
        return -1;
    }
    size_t written = fwrite(res.body, 1, res.body_len, fp);
    fclose(fp);
    pico_http_response_free(&res);

    return (written == res.body_len) ? 0 : -1;
}

NOW_API int now_remote_cache_store(const NowRemoteCacheConfig *cfg,
                                    const char *cache_key,
                                    const char *obj_path,
                                    const char *obj_ext) {
    if (!cfg || !cfg->push || !cfg->url || !cache_key || !obj_path) return -1;
    if (remote_tripped) return -1;

    /* Read the object file into memory */
    size_t file_len;
    char *file_data = read_file_bin(obj_path, &file_len);
    if (!file_data) return -1;

    char *url = build_url(cfg->url, cache_key, obj_ext);
    if (!url) { free(file_data); return -1; }

    PicoHttpOptions opts;
    PicoHttpHeader auth_hdr;
    char auth_buf[512];
    memset(&auth_hdr, 0, sizeof(auth_hdr));
    build_opts(cfg, &opts, &auth_hdr, auth_buf, sizeof(auth_buf));

    PicoHttpResponse res;
    memset(&res, 0, sizeof(res));
    int rc = pico_http_request("PUT", url, "application/octet-stream",
                                file_data, file_len, &opts, &res);
    free(url);
    free(file_data);

    if (rc != PICO_OK) {
        remote_tripped = 1;
        pico_http_response_free(&res);
        return -1;
    }
    int ok = (res.status == 200 || res.status == 201);
    pico_http_response_free(&res);
    return ok ? 0 : -1;
}

NOW_API int now_remote_cache_print_stats(const NowRemoteCacheConfig *cfg,
                                          int verbose) {
    if (!cfg || !cfg->url) {
        fprintf(stderr, "error: no remote cache configured\n");
        return -1;
    }

    printf("Remote object cache: %s\n", cfg->url);
    printf("  push: %s\n", cfg->push ? "enabled" : "disabled (read-only)");
    printf("  auth: %s\n", (cfg->token && cfg->token[0]) ? "bearer token" : "none");

    /* Connectivity check: GET /stats or HEAD /objects/ */
    size_t blen = strlen(cfg->url);
    while (blen > 0 && cfg->url[blen - 1] == '/') blen--;

    char stats_url[1024];
    snprintf(stats_url, sizeof(stats_url), "%.*s/stats", (int)blen, cfg->url);

    PicoHttpOptions opts;
    PicoHttpHeader auth_hdr;
    char auth_buf[512];
    memset(&auth_hdr, 0, sizeof(auth_hdr));
    build_opts(cfg, &opts, &auth_hdr, auth_buf, sizeof(auth_buf));

    PicoHttpResponse res;
    memset(&res, 0, sizeof(res));
    int rc = pico_http_request("GET", stats_url, NULL, NULL, 0, &opts, &res);

    if (rc == PICO_OK && res.status == 200) {
        printf("  status: reachable\n");
        if (res.body && res.body_len > 0 && verbose) {
            printf("  server response:\n    %.*s\n",
                   (int)res.body_len, res.body);
        }
    } else if (rc == PICO_OK) {
        printf("  status: reachable (HTTP %d)\n", res.status);
    } else {
        printf("  status: unreachable (%s)\n", pico_http_strerror(rc));
    }

    pico_http_response_free(&res);
    return (rc == PICO_OK) ? 0 : -1;
}

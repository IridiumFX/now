/*
 * now_trust.c — Signing, integrity, and trust (§17)
 *
 * Trust store management, scope matching, trust policy parsing,
 * and signature verification (delegates to minisign binary).
 */
#include "now_trust.h"
#include "now_pom.h"
#include "now_fs.h"
#include "pasta.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
static char *strndup_compat(const char *s, size_t n) {
    size_t len = strlen(s);
    if (len > n) len = n;
    char *r = (char *)malloc(len + 1);
    if (r) { memcpy(r, s, len); r[len] = '\0'; }
    return r;
}
#define strndup strndup_compat
#endif

/* ---- Trust store operations ---- */

NOW_API void now_trust_init(NowTrustStore *store) {
    memset(store, 0, sizeof(*store));
}

NOW_API void now_trust_free(NowTrustStore *store) {
    for (size_t i = 0; i < store->count; i++) {
        free(store->keys[i].scope);
        free(store->keys[i].key);
        free(store->keys[i].comment);
    }
    free(store->keys);
    memset(store, 0, sizeof(*store));
}

NOW_API int now_trust_add(NowTrustStore *store, const char *scope,
                           const char *key, const char *comment) {
    if (!store || !scope || !key) return -1;
    if (store->count >= store->capacity) {
        size_t new_cap = store->capacity ? store->capacity * 2 : 8;
        NowTrustKey *tmp = realloc(store->keys, new_cap * sizeof(NowTrustKey));
        if (!tmp) return -1;
        store->keys = tmp;
        store->capacity = new_cap;
    }
    NowTrustKey *k = &store->keys[store->count];
    k->scope   = strdup(scope);
    k->key     = strdup(key);
    k->comment = comment ? strdup(comment) : NULL;
    store->count++;
    return 0;
}

NOW_API int now_trust_scope_matches(const char *scope, const char *group,
                                      const char *artifact) {
    if (!scope || !group) return 0;

    /* Wildcard matches everything */
    if (strcmp(scope, "*") == 0) return 1;

    /* Check for group:artifact exact match */
    const char *colon = strchr(scope, ':');
    if (colon) {
        size_t glen = (size_t)(colon - scope);
        if (strlen(group) != glen || strncmp(scope, group, glen) != 0)
            return 0;
        if (!artifact) return 0;
        return strcmp(colon + 1, artifact) == 0;
    }

    /* Group prefix match (dot-boundary) */
    size_t slen = strlen(scope);
    size_t glen = strlen(group);
    if (glen < slen) return 0;
    if (strncmp(group, scope, slen) != 0) return 0;
    if (glen == slen) return 1;       /* exact */
    if (group[slen] == '.') return 1; /* dot boundary */
    return 0;
}

NOW_API const NowTrustKey *now_trust_find(const NowTrustStore *store,
                                            const char *group,
                                            const char *artifact) {
    if (!store || !group) return NULL;
    for (size_t i = 0; i < store->count; i++) {
        if (now_trust_scope_matches(store->keys[i].scope, group, artifact))
            return &store->keys[i];
    }
    return NULL;
}

/* ---- Trust store persistence ---- */

static char *trust_store_path(void) {
    const char *home = getenv("HOME");
#ifdef _WIN32
    if (!home) home = getenv("USERPROFILE");
#endif
    if (!home) return NULL;
    char *now_dir = now_path_join(home, ".now");
    if (!now_dir) return NULL;
    char *path = now_path_join(now_dir, "trust.pasta");
    free(now_dir);
    return path;
}

NOW_API int now_trust_load(NowTrustStore *store, NowResult *result) {
    if (!store) return -1;

    char *path = trust_store_path();
    if (!path) return 0; /* no home dir — not an error, just no store */

    if (!now_path_exists(path)) {
        free(path);
        return 0; /* file doesn't exist — empty store */
    }

    FILE *fp = fopen(path, "rb");
    if (!fp) {
        free(path);
        return 0;
    }

    fseek(fp, 0, SEEK_END);
    long len = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    char *buf = malloc((size_t)len + 1);
    if (!buf) { fclose(fp); free(path); return -1; }
    size_t nread = fread(buf, 1, (size_t)len, fp);
    buf[nread] = '\0';
    fclose(fp);

    PastaResult pr;
    PastaValue *root = pasta_parse(buf, nread, &pr);
    free(buf);

    if (!root || pr.code != PASTA_OK) {
        if (result) {
            result->code = NOW_ERR_SYNTAX;
            snprintf(result->message, sizeof(result->message),
                     "trust store: %s (line %d)", pr.message, pr.line);
        }
        free(path);
        return -1;
    }

    /* Parse { keys: [ { scope, key, comment }, ... ] } */
    const PastaValue *keys = pasta_map_get(root, "keys");
    if (keys && pasta_type(keys) == PASTA_ARRAY) {
        size_t n = pasta_count(keys);
        for (size_t i = 0; i < n; i++) {
            const PastaValue *entry = pasta_array_get(keys, i);
            if (!entry || pasta_type(entry) != PASTA_MAP) continue;

            const PastaValue *sv = pasta_map_get(entry, "scope");
            const PastaValue *kv = pasta_map_get(entry, "key");
            const PastaValue *cv = pasta_map_get(entry, "comment");

            const char *scope = (sv && pasta_type(sv) == PASTA_STRING) ?
                                 pasta_get_string(sv) : NULL;
            const char *key   = (kv && pasta_type(kv) == PASTA_STRING) ?
                                 pasta_get_string(kv) : NULL;
            const char *comment = (cv && pasta_type(cv) == PASTA_STRING) ?
                                   pasta_get_string(cv) : NULL;

            if (scope && key)
                now_trust_add(store, scope, key, comment);
        }
    }

    pasta_free(root);
    free(path);
    return 0;
}

NOW_API int now_trust_save(const NowTrustStore *store, NowResult *result) {
    if (!store) return -1;

    char *path = trust_store_path();
    if (!path) {
        if (result) {
            result->code = NOW_ERR_IO;
            snprintf(result->message, sizeof(result->message),
                     "cannot determine trust store path");
        }
        return -1;
    }

    /* Ensure ~/.now/ exists */
    const char *home = getenv("HOME");
#ifdef _WIN32
    if (!home) home = getenv("USERPROFILE");
#endif
    if (home) {
        char *now_dir = now_path_join(home, ".now");
        if (now_dir) { now_mkdir_p(now_dir); free(now_dir); }
    }

    /* Build Pasta document */
    PastaValue *root = pasta_new_map();
    PastaValue *keys = pasta_new_array();

    for (size_t i = 0; i < store->count; i++) {
        PastaValue *entry = pasta_new_map();
        pasta_set(entry, "scope", pasta_new_string(store->keys[i].scope));
        pasta_set(entry, "key", pasta_new_string(store->keys[i].key));
        if (store->keys[i].comment)
            pasta_set(entry, "comment", pasta_new_string(store->keys[i].comment));
        pasta_push(keys, entry);
    }
    pasta_set(root, "keys", keys);

    char *text = pasta_write(root, PASTA_PRETTY | PASTA_SORTED);
    pasta_free(root);

    if (!text) {
        free(path);
        if (result) {
            result->code = NOW_ERR_IO;
            snprintf(result->message, sizeof(result->message),
                     "failed to serialize trust store");
        }
        return -1;
    }

    FILE *fp = fopen(path, "w");
    if (!fp) {
        if (result) {
            result->code = NOW_ERR_IO;
            snprintf(result->message, sizeof(result->message),
                     "cannot write %s", path);
        }
        free(text);
        free(path);
        return -1;
    }
    fprintf(fp, "%s", text);
    fclose(fp);
    free(text);
    free(path);

    if (result) result->code = NOW_OK;
    return 0;
}

/* ---- Trust policy ---- */

NOW_API NowTrustPolicy now_trust_policy_from_project(const void *proj) {
    NowTrustPolicy policy = {0, 0};
    if (!proj) return policy;
    const NowProject *p = (const NowProject *)proj;
    if (!p->_pasta_root) return policy;

    const PastaValue *root = (const PastaValue *)p->_pasta_root;
    const PastaValue *trust = pasta_map_get(root, "trust");
    if (!trust || pasta_type(trust) != PASTA_MAP) return policy;

    const PastaValue *rs = pasta_map_get(trust, "require_signatures");
    if (rs && pasta_type(rs) == PASTA_BOOL)
        policy.require_signatures = pasta_get_bool(rs);

    const PastaValue *rk = pasta_map_get(trust, "require_known_keys");
    if (rk && pasta_type(rk) == PASTA_BOOL)
        policy.require_known_keys = pasta_get_bool(rk);

    return policy;
}

NOW_API NowTrustLevel now_trust_level(const NowTrustPolicy *policy) {
    if (!policy) return NOW_TRUST_NONE;
    if (policy->require_known_keys) return NOW_TRUST_TRUSTED;
    if (policy->require_signatures) return NOW_TRUST_SIGNED;
    return NOW_TRUST_NONE;
}

/* Ed25519 verify, sign, keypair, and now_verify_file are in now_ed25519.c */

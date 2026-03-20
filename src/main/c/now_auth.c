/*
 * now_auth.c — Shared authentication for registry operations
 *
 * Loads credentials from ~/.now/credentials.pasta, supports multiple
 * auth methods (token, LDAP, OIDC), caches JWTs with expiry tracking,
 * and provides registry discovery via /.well-known/now-registry.
 */

#include "now_auth.h"
#include "now_fs.h"
#include "pico_http.h"

#include <pasta.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#endif

/* ---- strndup compat for Windows ---- */
#ifdef _WIN32
static char *auth_strndup(const char *s, size_t n) {
    size_t len = 0;
    while (len < n && s[len]) len++;
    char *out = (char *)malloc(len + 1);
    if (out) { memcpy(out, s, len); out[len] = '\0'; }
    return out;
}
#else
#define auth_strndup strndup
#endif

/* ---- Base64 encoding (for Basic auth) ---- */

static const char b64_table[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static void base64_encode(const unsigned char *in, size_t len,
                           char *out, size_t out_cap) {
    size_t i = 0, j = 0;
    while (i < len && j + 4 < out_cap) {
        unsigned int a = in[i++];
        unsigned int b = (i < len) ? in[i++] : 0;
        unsigned int c = (i < len) ? in[i++] : 0;
        unsigned int triple = (a << 16) | (b << 8) | c;
        out[j++] = b64_table[(triple >> 18) & 0x3F];
        out[j++] = b64_table[(triple >> 12) & 0x3F];
        out[j++] = (i > len + 1) ? '=' : b64_table[(triple >> 6) & 0x3F];
        out[j++] = (i > len)     ? '=' : b64_table[triple & 0x3F];
    }
    out[j] = '\0';
}

/* ---- File reading helper ---- */

static char *read_file_all(const char *path, size_t *out_len) {
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

/* ---- Home directory helper ---- */

static char *get_dot_now_path(const char *filename) {
    const char *home = NULL;
#ifdef _WIN32
    home = getenv("USERPROFILE");
    if (!home) home = getenv("HOME");
#else
    home = getenv("HOME");
#endif
    if (!home) return NULL;

    char *dot_now = now_path_join(home, ".now");
    if (!dot_now) return NULL;
    char *path = now_path_join(dot_now, filename);
    free(dot_now);
    return path;
}

/* ---- JSON helper: extract string value by key ---- */

static char *json_extract_string(const char *json, size_t json_len,
                                  const char *key) {
    if (!json || !key) return NULL;
    /* Search for "key":"value" or "key": "value" */
    char pattern[256];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *kpos = strstr(json, pattern);
    if (!kpos) return NULL;
    kpos += strlen(pattern);
    /* Skip optional whitespace and colon */
    while (kpos < json + json_len && (*kpos == ' ' || *kpos == ':' || *kpos == '\t')) kpos++;
    if (kpos >= json + json_len || *kpos != '"') return NULL;
    kpos++; /* skip opening quote */
    const char *end = strchr(kpos, '"');
    if (!end) return NULL;
    return auth_strndup(kpos, (size_t)(end - kpos));
}

/* Extract a number value by key from JSON */
static long json_extract_number(const char *json, size_t json_len,
                                 const char *key) {
    if (!json || !key) return 0;
    char pattern[256];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *kpos = strstr(json, pattern);
    if (!kpos) return 0;
    kpos += strlen(pattern);
    while (kpos < json + json_len && (*kpos == ' ' || *kpos == ':' || *kpos == '\t')) kpos++;
    return strtol(kpos, NULL, 10);
}

/* ---- Auth method helpers ---- */

NOW_API NowAuthMethod now_auth_method_parse(const char *str) {
    if (!str) return NOW_AUTH_TOKEN;
    if (strcmp(str, "ldap") == 0) return NOW_AUTH_LDAP;
    if (strcmp(str, "oidc") == 0 || strcmp(str, "oauth2") == 0) return NOW_AUTH_OIDC;
    return NOW_AUTH_TOKEN;
}

NOW_API const char *now_auth_method_name(NowAuthMethod m) {
    switch (m) {
        case NOW_AUTH_LDAP: return "ldap";
        case NOW_AUTH_OIDC: return "oidc";
        default: return "token";
    }
}

/* ---- Credential loading ---- */

NOW_API int now_auth_load(const char *registry_url, NowCredentials *creds) {
    if (!creds) return -1;
    memset(creds, 0, sizeof(*creds));

    if (!registry_url) return -1;

    char *cred_path = get_dot_now_path("credentials.pasta");
    if (!cred_path) return -1;

    size_t cred_len;
    char *cred_data = read_file_all(cred_path, &cred_len);
    free(cred_path);
    if (!cred_data) return -1;

    PastaValue *root = pasta_parse(cred_data, cred_len, NULL);
    free(cred_data);
    if (!root || pasta_type(root) != PASTA_MAP) { pasta_free(root); return -1; }

    const PastaValue *registries = pasta_map_get(root, "registries");
    if (!registries || pasta_type(registries) != PASTA_ARRAY) {
        pasta_free(root);
        return -1;
    }

    int found = -1;
    size_t nregs = pasta_count(registries);
    for (size_t i = 0; i < nregs; i++) {
        const PastaValue *entry = pasta_array_get(registries, i);
        if (!entry || pasta_type(entry) != PASTA_MAP) continue;

        const PastaValue *url_val = pasta_map_get(entry, "url");
        if (!url_val || pasta_type(url_val) != PASTA_STRING) continue;

        const char *entry_url = pasta_get_string(url_val);
        if (!entry_url) continue;

        /* Match by URL prefix */
        if (strncmp(registry_url, entry_url, strlen(entry_url)) == 0) {
            /* Extract token */
            const PastaValue *tok = pasta_map_get(entry, "token");
            if (tok && pasta_type(tok) == PASTA_STRING) {
                const char *s = pasta_get_string(tok);
                if (s) creds->token = strdup(s);
            }

            /* Extract username */
            const PastaValue *user = pasta_map_get(entry, "username");
            if (user && pasta_type(user) == PASTA_STRING) {
                const char *s = pasta_get_string(user);
                if (s) creds->username = strdup(s);
            }

            /* Extract auth method (default: token) */
            const PastaValue *meth = pasta_map_get(entry, "method");
            if (meth && pasta_type(meth) == PASTA_STRING) {
                creds->method = now_auth_method_parse(pasta_get_string(meth));
            }

            /* OIDC-specific fields */
            const PastaValue *cid = pasta_map_get(entry, "client_id");
            if (cid && pasta_type(cid) == PASTA_STRING) {
                const char *s = pasta_get_string(cid);
                if (s) creds->client_id = strdup(s);
            }
            const PastaValue *iss = pasta_map_get(entry, "issuer");
            if (iss && pasta_type(iss) == PASTA_STRING) {
                const char *s = pasta_get_string(iss);
                if (s) creds->issuer = strdup(s);
            }

            found = 0;
            break;
        }
    }

    pasta_free(root);
    return found;
}

NOW_API void now_auth_creds_free(NowCredentials *creds) {
    if (!creds) return;
    free(creds->username);
    free(creds->token);
    free(creds->client_id);
    free(creds->issuer);
    memset(creds, 0, sizeof(*creds));
}

/* ---- JWT token exchange (Basic auth — original method) ---- */

NOW_API int now_auth_login(const char *host, int port, const char *path_prefix,
                           const NowCredentials *creds, int use_tls,
                           char **jwt_out, NowResult *result) {
    (void)use_tls;

    if (!jwt_out) return -1;
    *jwt_out = NULL;

    if (!host || !creds || !creds->username || !creds->token) {
        if (result) {
            result->code = NOW_ERR_AUTH;
            snprintf(result->message, sizeof(result->message),
                     "credentials missing username or token for auth login");
        }
        return -1;
    }

    /* Build Basic auth: base64(username:token) */
    size_t ulen = strlen(creds->username);
    size_t tlen = strlen(creds->token);
    size_t raw_len = ulen + 1 + tlen;
    char *raw = (char *)malloc(raw_len + 1);
    if (!raw) return -1;
    snprintf(raw, raw_len + 1, "%s:%s", creds->username, creds->token);

    size_t b64_cap = ((raw_len + 2) / 3) * 4 + 1;
    char *b64 = (char *)malloc(b64_cap);
    if (!b64) { free(raw); return -1; }
    base64_encode((const unsigned char *)raw, raw_len, b64, b64_cap);
    free(raw);

    char auth_buf[1024];
    snprintf(auth_buf, sizeof(auth_buf), "Basic %s", b64);
    free(b64);

    PicoHttpHeader headers[1];
    headers[0].name  = "Authorization";
    headers[0].value = auth_buf;

    PicoHttpOptions opts = {0};
    opts.headers = headers;
    opts.header_count = 1;

    char path[1024];
    snprintf(path, sizeof(path), "%s/auth/token",
             (path_prefix && *path_prefix) ? path_prefix : "");

    PicoHttpResponse res;
    memset(&res, 0, sizeof(res));

    int rc = pico_http_post(host, port, path,
                            NULL, NULL, 0, &opts, &res);

    if (rc != PICO_OK) {
        if (result) {
            result->code = NOW_ERR_IO;
            snprintf(result->message, sizeof(result->message),
                     "POST /auth/token failed: %s", pico_http_strerror(rc));
        }
        return -1;
    }

    if (res.status != 200) {
        if (result) {
            result->code = NOW_ERR_AUTH;
            snprintf(result->message, sizeof(result->message),
                     "auth failed: registry returned HTTP %d", res.status);
        }
        pico_http_response_free(&res);
        return -1;
    }

    /* Parse JWT from response: {"token":"<jwt>"} or {"access_token":"<jwt>"} */
    if (res.body && res.body_len > 0) {
        *jwt_out = json_extract_string(res.body, res.body_len, "token");
        if (!*jwt_out)
            *jwt_out = json_extract_string(res.body, res.body_len, "access_token");
    }

    pico_http_response_free(&res);

    if (!*jwt_out) {
        if (result) {
            result->code = NOW_ERR_AUTH;
            snprintf(result->message, sizeof(result->message),
                     "auth response missing token field");
        }
        return -1;
    }

    return 0;
}

/* ---- LDAP login ---- */

NOW_API int now_auth_login_ldap(const char *registry_url,
                                 const char *username, const char *password,
                                 char **jwt_out, NowResult *result) {
    if (!jwt_out) return -1;
    *jwt_out = NULL;

    if (!registry_url || !username || !password) {
        if (result) {
            result->code = NOW_ERR_AUTH;
            snprintf(result->message, sizeof(result->message),
                     "LDAP login requires registry URL, username, and password");
        }
        return -1;
    }

    /* Build JSON body: {"subject":"user","token":"pass","method":"ldap"} */
    char body[2048];
    snprintf(body, sizeof(body),
             "{\"subject\":\"%s\",\"token\":\"%s\",\"method\":\"ldap\"}",
             username, password);

    /* POST to registry's /auth/token endpoint */
    char url[1024];
    snprintf(url, sizeof(url), "%s/auth/token", registry_url);

    PicoHttpOptions opts = {0};
    opts.connect_timeout_ms = 5000;

    PicoHttpResponse res;
    memset(&res, 0, sizeof(res));

    int rc = pico_http_request("POST", url, "application/json",
                                body, strlen(body), &opts, &res);

    if (rc != PICO_OK) {
        if (result) {
            result->code = NOW_ERR_IO;
            snprintf(result->message, sizeof(result->message),
                     "LDAP auth request failed: %s", pico_http_strerror(rc));
        }
        return -1;
    }

    if (res.status == 401 || res.status == 403) {
        if (result) {
            result->code = NOW_ERR_AUTH;
            snprintf(result->message, sizeof(result->message),
                     "LDAP authentication failed (invalid credentials)");
        }
        pico_http_response_free(&res);
        return -1;
    }

    if (res.status != 200) {
        if (result) {
            result->code = NOW_ERR_AUTH;
            snprintf(result->message, sizeof(result->message),
                     "LDAP auth returned HTTP %d", res.status);
        }
        pico_http_response_free(&res);
        return -1;
    }

    if (res.body && res.body_len > 0) {
        *jwt_out = json_extract_string(res.body, res.body_len, "token");
        if (!*jwt_out)
            *jwt_out = json_extract_string(res.body, res.body_len, "access_token");
    }

    pico_http_response_free(&res);

    if (!*jwt_out) {
        if (result) {
            result->code = NOW_ERR_AUTH;
            snprintf(result->message, sizeof(result->message),
                     "LDAP auth response missing token");
        }
        return -1;
    }

    return 0;
}

/* ---- OIDC device code flow ---- */

NOW_API int now_auth_login_oidc_device(const char *registry_url,
                                        const NowCredentials *creds,
                                        char **jwt_out, NowResult *result) {
    if (!jwt_out) return -1;
    *jwt_out = NULL;

    if (!registry_url) {
        if (result) {
            result->code = NOW_ERR_AUTH;
            snprintf(result->message, sizeof(result->message),
                     "OIDC device flow requires registry URL");
        }
        return -1;
    }

    /* Step 1: Request device code from registry */
    char device_url[1024];
    snprintf(device_url, sizeof(device_url), "%s/auth/device", registry_url);

    char body[512];
    if (creds && creds->client_id)
        snprintf(body, sizeof(body), "{\"client_id\":\"%s\"}", creds->client_id);
    else
        snprintf(body, sizeof(body), "{}");

    PicoHttpOptions opts = {0};
    opts.connect_timeout_ms = 5000;

    PicoHttpResponse res;
    memset(&res, 0, sizeof(res));

    int rc = pico_http_request("POST", device_url, "application/json",
                                body, strlen(body), &opts, &res);

    if (rc != PICO_OK || res.status != 200) {
        if (result) {
            result->code = (rc != PICO_OK) ? NOW_ERR_IO : NOW_ERR_AUTH;
            snprintf(result->message, sizeof(result->message),
                     "OIDC device code request failed%s%s",
                     (rc != PICO_OK) ? ": " : "",
                     (rc != PICO_OK) ? pico_http_strerror(rc) : " (unsupported by registry)");
        }
        pico_http_response_free(&res);
        return -1;
    }

    /* Parse device code response:
     * {"device_code":"xxx","user_code":"ABCD-EFGH",
     *  "verification_uri":"https://...", "interval":5, "expires_in":900} */
    char *device_code = NULL;
    char *user_code = NULL;
    char *verify_uri = NULL;
    long interval = 5;

    if (res.body && res.body_len > 0) {
        device_code = json_extract_string(res.body, res.body_len, "device_code");
        user_code   = json_extract_string(res.body, res.body_len, "user_code");
        verify_uri  = json_extract_string(res.body, res.body_len, "verification_uri");
        interval    = json_extract_number(res.body, res.body_len, "interval");
        if (interval < 1) interval = 5;
    }

    pico_http_response_free(&res);

    if (!device_code || !user_code) {
        if (result) {
            result->code = NOW_ERR_AUTH;
            snprintf(result->message, sizeof(result->message),
                     "OIDC device code response missing required fields");
        }
        free(device_code);
        free(user_code);
        free(verify_uri);
        return -1;
    }

    /* Step 2: Display instructions to user */
    printf("To authenticate, visit:\n");
    if (verify_uri) printf("  %s\n", verify_uri);
    printf("and enter code: %s\n\n", user_code);
    printf("Waiting for authorization...\n");

    /* Step 3: Poll for token */
    char poll_url[1024];
    snprintf(poll_url, sizeof(poll_url), "%s/auth/device/token", registry_url);

    char poll_body[512];
    snprintf(poll_body, sizeof(poll_body),
             "{\"device_code\":\"%s\",\"grant_type\":\"urn:ietf:params:oauth:grant-type:device_code\"}",
             device_code);

    int max_polls = 180 / (int)interval; /* 3 minutes max */
    for (int attempt = 0; attempt < max_polls; attempt++) {
        /* Sleep for interval seconds */
#ifdef _WIN32
        Sleep((DWORD)(interval * 1000));
#else
        {
            struct timespec ts;
            ts.tv_sec = interval;
            ts.tv_nsec = 0;
            nanosleep(&ts, NULL);
        }
#endif

        memset(&res, 0, sizeof(res));
        rc = pico_http_request("POST", poll_url, "application/json",
                                poll_body, strlen(poll_body), &opts, &res);

        if (rc != PICO_OK) continue;

        if (res.status == 200 && res.body) {
            *jwt_out = json_extract_string(res.body, res.body_len, "token");
            if (!*jwt_out)
                *jwt_out = json_extract_string(res.body, res.body_len, "access_token");
            pico_http_response_free(&res);
            if (*jwt_out) {
                printf("Authenticated successfully.\n");
                break;
            }
        } else if (res.status == 428) {
            /* authorization_pending — keep polling */
            pico_http_response_free(&res);
            continue;
        } else if (res.status == 410) {
            /* expired_token */
            pico_http_response_free(&res);
            if (result) {
                result->code = NOW_ERR_AUTH;
                snprintf(result->message, sizeof(result->message),
                         "OIDC device code expired — please try again");
            }
            break;
        } else {
            pico_http_response_free(&res);
        }
    }

    free(device_code);
    free(user_code);
    free(verify_uri);

    if (!*jwt_out) {
        if (result && result->code == NOW_OK) {
            result->code = NOW_ERR_AUTH;
            snprintf(result->message, sizeof(result->message),
                     "OIDC device flow timed out");
        }
        return -1;
    }

    return 0;
}

/* ---- OIDC client credentials flow ---- */

NOW_API int now_auth_login_oidc_client(const char *registry_url,
                                        const char *client_id,
                                        const char *client_secret,
                                        char **jwt_out, NowResult *result) {
    if (!jwt_out) return -1;
    *jwt_out = NULL;

    if (!registry_url || !client_id || !client_secret) {
        if (result) {
            result->code = NOW_ERR_AUTH;
            snprintf(result->message, sizeof(result->message),
                     "OIDC client credentials requires registry URL, client_id, and client_secret");
        }
        return -1;
    }

    char body[2048];
    snprintf(body, sizeof(body),
             "{\"grant_type\":\"client_credentials\","
             "\"client_id\":\"%s\",\"client_secret\":\"%s\"}",
             client_id, client_secret);

    char url[1024];
    snprintf(url, sizeof(url), "%s/auth/token", registry_url);

    PicoHttpOptions opts = {0};
    opts.connect_timeout_ms = 5000;

    PicoHttpResponse res;
    memset(&res, 0, sizeof(res));

    int rc = pico_http_request("POST", url, "application/json",
                                body, strlen(body), &opts, &res);

    if (rc != PICO_OK) {
        if (result) {
            result->code = NOW_ERR_IO;
            snprintf(result->message, sizeof(result->message),
                     "OIDC client credentials request failed: %s",
                     pico_http_strerror(rc));
        }
        return -1;
    }

    if (res.status != 200) {
        if (result) {
            result->code = NOW_ERR_AUTH;
            snprintf(result->message, sizeof(result->message),
                     "OIDC client credentials failed: HTTP %d", res.status);
        }
        pico_http_response_free(&res);
        return -1;
    }

    if (res.body && res.body_len > 0) {
        *jwt_out = json_extract_string(res.body, res.body_len, "token");
        if (!*jwt_out)
            *jwt_out = json_extract_string(res.body, res.body_len, "access_token");
    }

    pico_http_response_free(&res);

    if (!*jwt_out) {
        if (result) {
            result->code = NOW_ERR_AUTH;
            snprintf(result->message, sizeof(result->message),
                     "OIDC client credentials response missing token");
        }
        return -1;
    }

    return 0;
}

/* ---- Registry discovery ---- */

NOW_API int now_auth_discover(const char *registry_url, NowRegistryInfo *info) {
    if (!info) return -1;
    memset(info, 0, sizeof(*info));

    if (!registry_url) return -1;

    char url[1024];
    snprintf(url, sizeof(url), "%s/.well-known/now-registry", registry_url);

    PicoHttpOptions opts = {0};
    opts.connect_timeout_ms = 3000;

    PicoHttpResponse res;
    memset(&res, 0, sizeof(res));

    int rc = pico_http_request("GET", url, NULL, NULL, 0, &opts, &res);

    if (rc != PICO_OK || res.status != 200 || !res.body) {
        pico_http_response_free(&res);
        /* Default: assume token auth only */
        info->supports_token = 1;
        return -1;
    }

    /* Parse JSON discovery document:
     * {"name":"cookbook","auth_methods":["token","ldap","oidc"],
     *  "oidc_issuer":"https://...","oidc_device_url":"..."} */

    info->registry_name = json_extract_string(res.body, res.body_len, "name");

    /* Check auth_methods array (simple substring search) */
    const char *methods = strstr(res.body, "\"auth_methods\"");
    if (methods) {
        const char *arr = strchr(methods, '[');
        const char *arr_end = arr ? strchr(arr, ']') : NULL;
        if (arr && arr_end) {
            size_t seg = (size_t)(arr_end - arr);
            char *segment = auth_strndup(arr, seg);
            if (segment) {
                info->supports_token = (strstr(segment, "\"token\"") != NULL);
                info->supports_ldap  = (strstr(segment, "\"ldap\"") != NULL);
                info->supports_oidc  = (strstr(segment, "\"oidc\"") != NULL);
                free(segment);
            }
        }
    } else {
        /* No methods listed — assume token */
        info->supports_token = 1;
    }

    info->oidc_issuer    = json_extract_string(res.body, res.body_len, "oidc_issuer");
    info->oidc_device_url = json_extract_string(res.body, res.body_len, "oidc_device_url");
    info->oidc_token_url = json_extract_string(res.body, res.body_len, "oidc_token_url");

    pico_http_response_free(&res);
    return 0;
}

NOW_API void now_auth_discovery_free(NowRegistryInfo *info) {
    if (!info) return;
    free(info->oidc_issuer);
    free(info->oidc_device_url);
    free(info->oidc_token_url);
    free(info->registry_name);
    memset(info, 0, sizeof(*info));
}

/* ---- Token caching ---- */

NOW_API int now_token_cache_load(NowTokenCache *cache) {
    if (!cache) return -1;
    cache->entries = NULL;
    cache->count = 0;

    char *path = get_dot_now_path("tokens.pasta");
    if (!path) return -1;

    size_t data_len;
    char *data = read_file_all(path, &data_len);
    free(path);
    if (!data) return -1;

    PastaValue *root = pasta_parse(data, data_len, NULL);
    free(data);
    if (!root || pasta_type(root) != PASTA_MAP) { pasta_free(root); return -1; }

    const PastaValue *tokens = pasta_map_get(root, "tokens");
    if (!tokens || pasta_type(tokens) != PASTA_ARRAY) {
        pasta_free(root);
        return 0; /* empty cache is valid */
    }

    size_t n = pasta_count(tokens);
    if (n == 0) { pasta_free(root); return 0; }

    cache->entries = (NowTokenCacheEntry *)calloc(n, sizeof(NowTokenCacheEntry));
    if (!cache->entries) { pasta_free(root); return -1; }

    for (size_t i = 0; i < n; i++) {
        const PastaValue *entry = pasta_array_get(tokens, i);
        if (!entry || pasta_type(entry) != PASTA_MAP) continue;

        const PastaValue *url_v = pasta_map_get(entry, "url");
        const PastaValue *jwt_v = pasta_map_get(entry, "jwt");
        const PastaValue *exp_v = pasta_map_get(entry, "expires_at");

        if (!url_v || !jwt_v) continue;

        NowTokenCacheEntry *e = &cache->entries[cache->count];
        const char *s;

        s = pasta_get_string(url_v);
        if (s) e->registry_url = strdup(s);
        s = pasta_get_string(jwt_v);
        if (s) e->jwt = strdup(s);
        if (exp_v && pasta_type(exp_v) == PASTA_NUMBER)
            e->expires_at = (long)pasta_get_number(exp_v);

        if (e->registry_url && e->jwt) cache->count++;
    }

    pasta_free(root);
    return 0;
}

NOW_API int now_token_cache_save(const NowTokenCache *cache) {
    if (!cache) return -1;

    char *path = get_dot_now_path("tokens.pasta");
    if (!path) return -1;

    /* Ensure ~/.now/ exists */
    char *dir = get_dot_now_path("");
    if (dir) {
        /* Trim trailing separator */
        size_t dlen = strlen(dir);
        if (dlen > 0 && (dir[dlen-1] == '/' || dir[dlen-1] == '\\'))
            dir[dlen-1] = '\0';
        now_mkdir_p(dir);
        free(dir);
    }

    FILE *fp = fopen(path, "wb");
    free(path);
    if (!fp) return -1;

    fprintf(fp, "{\n  tokens: [\n");
    for (size_t i = 0; i < cache->count; i++) {
        const NowTokenCacheEntry *e = &cache->entries[i];
        if (!e->registry_url || !e->jwt) continue;
        fprintf(fp, "    { url: \"%s\", jwt: \"%s\", expires_at: %ld }%s\n",
                e->registry_url, e->jwt, e->expires_at,
                (i + 1 < cache->count) ? "," : "");
    }
    fprintf(fp, "  ]\n}\n");
    fclose(fp);
    return 0;
}

NOW_API void now_token_cache_free(NowTokenCache *cache) {
    if (!cache) return;
    for (size_t i = 0; i < cache->count; i++) {
        free(cache->entries[i].registry_url);
        free(cache->entries[i].jwt);
    }
    free(cache->entries);
    cache->entries = NULL;
    cache->count = 0;
}

NOW_API char *now_token_cache_get(const char *registry_url) {
    if (!registry_url) return NULL;

    NowTokenCache cache;
    if (now_token_cache_load(&cache) != 0) return NULL;

    long now_ts = (long)time(NULL);
    char *jwt = NULL;

    for (size_t i = 0; i < cache.count; i++) {
        NowTokenCacheEntry *e = &cache.entries[i];
        if (!e->registry_url) continue;
        if (strcmp(e->registry_url, registry_url) == 0) {
            /* Check expiry (with 60s safety margin) */
            if (e->expires_at > now_ts + 60 && e->jwt) {
                jwt = strdup(e->jwt);
            }
            break;
        }
    }

    now_token_cache_free(&cache);
    return jwt;
}

NOW_API int now_token_cache_put(const char *registry_url, const char *jwt,
                                 long expires_in_sec) {
    if (!registry_url || !jwt) return -1;

    NowTokenCache cache;
    if (now_token_cache_load(&cache) != 0) {
        cache.entries = NULL;
        cache.count = 0;
    }

    long expires_at = (long)time(NULL) + expires_in_sec;

    /* Check if entry already exists */
    int found = 0;
    for (size_t i = 0; i < cache.count; i++) {
        if (cache.entries[i].registry_url &&
            strcmp(cache.entries[i].registry_url, registry_url) == 0) {
            free(cache.entries[i].jwt);
            cache.entries[i].jwt = strdup(jwt);
            cache.entries[i].expires_at = expires_at;
            found = 1;
            break;
        }
    }

    if (!found) {
        NowTokenCacheEntry *new_entries = (NowTokenCacheEntry *)realloc(
            cache.entries, (cache.count + 1) * sizeof(NowTokenCacheEntry));
        if (!new_entries) { now_token_cache_free(&cache); return -1; }
        cache.entries = new_entries;
        NowTokenCacheEntry *e = &cache.entries[cache.count];
        e->registry_url = strdup(registry_url);
        e->jwt = strdup(jwt);
        e->expires_at = expires_at;
        cache.count++;
    }

    int rc = now_token_cache_save(&cache);
    now_token_cache_free(&cache);
    return rc;
}

NOW_API int now_token_cache_remove(const char *registry_url) {
    if (!registry_url) return -1;

    NowTokenCache cache;
    if (now_token_cache_load(&cache) != 0) return -1;

    int found = -1;
    for (size_t i = 0; i < cache.count; i++) {
        if (cache.entries[i].registry_url &&
            strcmp(cache.entries[i].registry_url, registry_url) == 0) {
            free(cache.entries[i].registry_url);
            free(cache.entries[i].jwt);
            /* Shift remaining entries */
            for (size_t j = i; j + 1 < cache.count; j++)
                cache.entries[j] = cache.entries[j + 1];
            cache.count--;
            found = 0;
            break;
        }
    }

    if (found == 0)
        now_token_cache_save(&cache);

    now_token_cache_free(&cache);
    return found;
}

/* ---- Unified token acquisition ---- */

NOW_API char *now_auth_get_token(const char *registry_url, int verbose,
                                  NowResult *result) {
    if (!registry_url) return NULL;

    /* 1. Check token cache first */
    char *jwt = now_token_cache_get(registry_url);
    if (jwt) {
        if (verbose) printf("  auth: using cached token for %s\n", registry_url);
        return jwt;
    }

    /* 2. Load credentials */
    NowCredentials creds;
    if (now_auth_load(registry_url, &creds) != 0) {
        if (result) {
            result->code = NOW_ERR_AUTH;
            snprintf(result->message, sizeof(result->message),
                     "no credentials found for %s", registry_url);
        }
        return NULL;
    }

    if (verbose) printf("  auth: method=%s for %s\n",
                         now_auth_method_name(creds.method), registry_url);

    long expires_in = 3600; /* default 1 hour */

    /* 3. Authenticate based on method */
    switch (creds.method) {
    case NOW_AUTH_LDAP:
        if (now_auth_login_ldap(registry_url, creds.username, creds.token,
                                 &jwt, result) != 0) {
            now_auth_creds_free(&creds);
            return NULL;
        }
        break;

    case NOW_AUTH_OIDC:
        if (creds.client_id && creds.token) {
            /* Client credentials flow (CI/CD) */
            if (now_auth_login_oidc_client(registry_url, creds.client_id,
                                            creds.token, &jwt, result) != 0) {
                now_auth_creds_free(&creds);
                return NULL;
            }
        } else {
            /* Device code flow (interactive) */
            if (now_auth_login_oidc_device(registry_url, &creds,
                                            &jwt, result) != 0) {
                now_auth_creds_free(&creds);
                return NULL;
            }
        }
        break;

    default: /* NOW_AUTH_TOKEN */ {
        /* Original flow: Basic auth exchange or raw token */
        if (creds.username) {
            char *host = NULL;
            char *base_path = NULL;
            int port = 80, tls = 0;
            if (pico_http_parse_url_ex(registry_url, &host, &port,
                                        &base_path, &tls) == 0) {
                char *prefix = base_path ? base_path : strdup("");
                size_t plen = strlen(prefix);
                while (plen > 0 && prefix[plen - 1] == '/') prefix[--plen] = '\0';
                now_auth_login(host, port, prefix, &creds, tls, &jwt, result);
                free(prefix);
            }
            free(host);
        }
        if (!jwt && creds.token)
            jwt = strdup(creds.token);
        break;
    }
    }

    now_auth_creds_free(&creds);

    /* 4. Cache the token */
    if (jwt) {
        now_token_cache_put(registry_url, jwt, expires_in);
        if (verbose) printf("  auth: token cached (expires in %lds)\n", expires_in);
    }

    return jwt;
}

/* ---- CLI helpers ---- */

NOW_API int now_auth_print_status(const char *registry_url, int verbose) {
    NowTokenCache cache;
    if (now_token_cache_load(&cache) != 0) {
        cache.entries = NULL;
        cache.count = 0;
    }

    long now_ts = (long)time(NULL);
    int printed = 0;

    if (registry_url) {
        /* Show status for specific registry */
        NowCredentials creds;
        int has_creds = (now_auth_load(registry_url, &creds) == 0);

        printf("Registry: %s\n", registry_url);

        if (has_creds) {
            printf("  Method:   %s\n", now_auth_method_name(creds.method));
            if (creds.username)
                printf("  Username: %s\n", creds.username);
            if (creds.client_id)
                printf("  Client:   %s\n", creds.client_id);
            now_auth_creds_free(&creds);
        } else {
            printf("  Credentials: none\n");
        }

        /* Check cached token */
        for (size_t i = 0; i < cache.count; i++) {
            if (cache.entries[i].registry_url &&
                strcmp(cache.entries[i].registry_url, registry_url) == 0) {
                long remaining = cache.entries[i].expires_at - now_ts;
                if (remaining > 0)
                    printf("  Token:    cached (expires in %ldm %lds)\n",
                           remaining / 60, remaining % 60);
                else
                    printf("  Token:    expired\n");
                printed = 1;
                break;
            }
        }
        if (!printed) printf("  Token:    none\n");

        /* Try discovery */
        if (verbose) {
            NowRegistryInfo info;
            if (now_auth_discover(registry_url, &info) == 0) {
                printf("  Discovery:\n");
                if (info.registry_name) printf("    Name:  %s\n", info.registry_name);
                printf("    Methods:");
                if (info.supports_token) printf(" token");
                if (info.supports_ldap)  printf(" ldap");
                if (info.supports_oidc)  printf(" oidc");
                printf("\n");
                now_auth_discovery_free(&info);
            } else {
                printf("  Discovery: unavailable\n");
            }
        }
    } else {
        /* Show all cached tokens */
        if (cache.count == 0) {
            printf("No cached tokens.\n");
        } else {
            printf("Cached tokens:\n");
            for (size_t i = 0; i < cache.count; i++) {
                NowTokenCacheEntry *e = &cache.entries[i];
                if (!e->registry_url) continue;
                long remaining = e->expires_at - now_ts;
                if (remaining > 0)
                    printf("  %s — expires in %ldm %lds\n",
                           e->registry_url, remaining / 60, remaining % 60);
                else
                    printf("  %s — expired\n", e->registry_url);
            }
        }
    }

    now_token_cache_free(&cache);
    return 0;
}

NOW_API int now_auth_logout(const char *registry_url) {
    if (!registry_url) return -1;
    int rc = now_token_cache_remove(registry_url);
    if (rc == 0)
        printf("Logged out from %s\n", registry_url);
    else
        printf("No cached token for %s\n", registry_url);
    return rc;
}

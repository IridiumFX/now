/*
 * now_auth.h — Shared authentication for registry operations
 *
 * Loads credentials from ~/.now/credentials.pasta and exchanges
 * them for a JWT via POST /auth/token. Supports multiple auth methods:
 *   - token:  Static API key or username/password → Basic auth exchange
 *   - ldap:   Username/password → registry validates against LDAP directory
 *   - oidc:   OAuth2/OIDC device code flow (interactive) or client credentials (CI)
 *
 * Token caching: JWTs are cached in ~/.now/tokens.pasta with expiry tracking.
 * Registry discovery: GET /.well-known/now-registry → supported auth methods.
 */
#ifndef NOW_AUTH_H
#define NOW_AUTH_H

#include "now.h"

/* ---- Auth method ---- */

typedef enum {
    NOW_AUTH_TOKEN = 0,   /* Static token or username:password → Basic auth (default) */
    NOW_AUTH_LDAP  = 1,   /* Username/password → registry validates via LDAP bind */
    NOW_AUTH_OIDC  = 2    /* OAuth2/OIDC: device code (interactive) or client creds (CI) */
} NowAuthMethod;

/* Credentials loaded from ~/.now/credentials.pasta */
typedef struct {
    char *username;       /* registry username (may be NULL for static tokens) */
    char *token;          /* API key / password / client_secret */
    NowAuthMethod method; /* auth method for this registry */
    char *client_id;      /* OIDC client ID (NULL if not OIDC) */
    char *issuer;         /* OIDC issuer URL (NULL if not OIDC) */
} NowCredentials;

/* Cached JWT token with expiry */
typedef struct {
    char *registry_url;   /* registry URL this token is for */
    char *jwt;            /* cached JWT string */
    long  expires_at;     /* unix timestamp when token expires */
} NowTokenCacheEntry;

/* Token cache (loaded from ~/.now/tokens.pasta) */
typedef struct {
    NowTokenCacheEntry *entries;
    size_t count;
} NowTokenCache;

/* Registry discovery result */
typedef struct {
    int   supports_token;  /* 1 if registry supports token auth */
    int   supports_ldap;   /* 1 if registry supports LDAP auth */
    int   supports_oidc;   /* 1 if registry supports OIDC */
    char *oidc_issuer;     /* OIDC issuer URL (from discovery) */
    char *oidc_device_url; /* device authorization endpoint */
    char *oidc_token_url;  /* token endpoint */
    char *registry_name;   /* human-readable registry name */
} NowRegistryInfo;

/* ---- Credential loading ---- */

/* Load credentials for a registry URL from ~/.now/credentials.pasta.
 * Matches by URL prefix. Sets fields in *creds (caller must free via
 * now_auth_creds_free). Returns 0 if found, -1 if no match. */
NOW_API int now_auth_load(const char *registry_url, NowCredentials *creds);
NOW_API void now_auth_creds_free(NowCredentials *creds);

/* ---- Token exchange ---- */

/* Exchange credentials for a JWT via POST /auth/token.
 * Builds Authorization: Basic base64(username:token) header.
 * On success, returns 0 and sets *jwt_out to a malloc'd JWT string.
 * Caller must free *jwt_out. Returns -1 on error. */
NOW_API int now_auth_login(const char *host, int port, const char *path_prefix,
                           const NowCredentials *creds, int use_tls,
                           char **jwt_out, NowResult *result);

/* LDAP login: POST /auth/token with method=ldap in body.
 * Registry performs LDAP bind server-side. Returns JWT. */
NOW_API int now_auth_login_ldap(const char *registry_url,
                                 const char *username, const char *password,
                                 char **jwt_out, NowResult *result);

/* OIDC device code flow: request device code, print user instructions,
 * poll for token completion. Returns JWT. Interactive — prints to stdout. */
NOW_API int now_auth_login_oidc_device(const char *registry_url,
                                        const NowCredentials *creds,
                                        char **jwt_out, NowResult *result);

/* OIDC client credentials flow: exchange client_id + client_secret for JWT.
 * Non-interactive — suitable for CI/CD. */
NOW_API int now_auth_login_oidc_client(const char *registry_url,
                                        const char *client_id,
                                        const char *client_secret,
                                        char **jwt_out, NowResult *result);

/* ---- Unified token acquisition ---- */

/* Get a valid JWT for a registry URL. Checks cached tokens first,
 * then discovers auth method, authenticates, caches result.
 * Returns malloc'd JWT string, or NULL on error. */
NOW_API char *now_auth_get_token(const char *registry_url, int verbose,
                                  NowResult *result);

/* ---- Registry discovery ---- */

/* Discover registry capabilities via GET /.well-known/now-registry.
 * Returns 0 on success, -1 on error (unreachable or no discovery). */
NOW_API int now_auth_discover(const char *registry_url, NowRegistryInfo *info);
NOW_API void now_auth_discovery_free(NowRegistryInfo *info);

/* ---- Token caching ---- */

NOW_API int  now_token_cache_load(NowTokenCache *cache);
NOW_API int  now_token_cache_save(const NowTokenCache *cache);
NOW_API void now_token_cache_free(NowTokenCache *cache);

/* Find a non-expired cached token for a registry. Returns malloc'd JWT or NULL. */
NOW_API char *now_token_cache_get(const char *registry_url);

/* Store a token in the cache (overwrites existing entry for same URL). */
NOW_API int now_token_cache_put(const char *registry_url, const char *jwt,
                                 long expires_in_sec);

/* Remove cached token for a registry. Returns 0 if removed, -1 if not found. */
NOW_API int now_token_cache_remove(const char *registry_url);

/* ---- CLI helpers ---- */

/* Print current auth status for a registry (or all registries). */
NOW_API int now_auth_print_status(const char *registry_url, int verbose);

/* Logout: clear cached token for a registry. */
NOW_API int now_auth_logout(const char *registry_url);

/* Parse auth method string. Returns NOW_AUTH_TOKEN if unrecognized. */
NOW_API NowAuthMethod now_auth_method_parse(const char *str);
NOW_API const char   *now_auth_method_name(NowAuthMethod m);

#endif /* NOW_AUTH_H */

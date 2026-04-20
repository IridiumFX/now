/*
 * pico_http_apennines.c — Apennines HTTPS backend for pico_http API
 *
 * Implements the pico_http public API using the apennines HTTPS client
 * stack (TLS 1.3, HTTP/2, connection pooling). Only compiled when
 * PICO_HTTP_APENNINES is defined.
 *
 * License: MIT
 * Copyright (c) 2026 The now contributors
 */

#ifdef PICO_HTTP_APENNINES

#ifndef PICO_HTTP_BUILDING
  #define PICO_HTTP_BUILDING
#endif

#include "pico_http.h"
#include "apennines/t4/net/https_client.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- Version ---- */

PICO_API const char *pico_http_version(void) { return "0.3.0"; }

/* ---- Error strings ---- */

PICO_API const char *pico_http_strerror(int err) {
    switch (err) {
        case PICO_OK:                      return "success";
        case PICO_ERR_INVALID:             return "invalid arguments";
        case PICO_ERR_DNS:                 return "DNS resolution failed";
        case PICO_ERR_CONNECT:             return "connection failed";
        case PICO_ERR_SEND:                return "send failed";
        case PICO_ERR_RECV:                return "receive failed";
        case PICO_ERR_PARSE:               return "response parse error";
        case PICO_ERR_ALLOC:               return "memory allocation failed";
        case PICO_ERR_WINSOCK:             return "Winsock initialization failed";
        case PICO_ERR_TOO_MANY_REDIRECTS:  return "too many redirects";
        case PICO_ERR_TLS:                 return "TLS error";
        default:                           return "unknown error";
    }
}

/* ---- Response free ---- */

PICO_API void pico_http_response_free(PicoHttpResponse *resp) {
    if (!resp) return;
    free(resp->body);
    free(resp->status_text);
    if (resp->headers) {
        for (size_t i = 0; i < resp->header_count; i++) {
            free((char *)resp->headers[i].name);
            free((char *)resp->headers[i].value);
        }
        free(resp->headers);
    }
    memset(resp, 0, sizeof(*resp));
}

/* ---- URL parsing ---- */

PICO_API int pico_http_parse_url_ex(const char *url, char **host,
                                      int *port, char **path, int *tls_out) {
    if (!url || !host || !port || !path) return -1;
    if (tls_out) *tls_out = 0;
    const char *p = url;
    if (strncmp(p, "https://", 8) == 0) { if (tls_out) *tls_out = 1; p += 8; }
    else if (strncmp(p, "http://", 7) == 0) { p += 7; }
    else return -1;

    const char *slash = strchr(p, '/');
    const char *he = slash ? slash : p + strlen(p);
    const char *colon = (const char *)memchr(p, ':', (size_t)(he - p));

    if (colon) {
        *host = (char *)malloc((size_t)(colon - p) + 1);
        if (*host) { memcpy(*host, p, (size_t)(colon - p)); (*host)[colon - p] = '\0'; }
        *port = atoi(colon + 1);
    } else {
        *host = (char *)malloc((size_t)(he - p) + 1);
        if (*host) { memcpy(*host, p, (size_t)(he - p)); (*host)[he - p] = '\0'; }
        *port = (tls_out && *tls_out) ? 443 : 80;
    }
    *path = strdup(slash ? slash : "/");
    return 0;
}

PICO_API int pico_http_parse_url(const char *url, char **host_out,
                                  int *port_out, char **path_out) {
    return pico_http_parse_url_ex(url, host_out, port_out, path_out, NULL);
}

/* ---- Find header ---- */

PICO_API const char *pico_http_find_header(const PicoHttpResponse *resp,
                                             const char *name) {
    if (!resp || !resp->headers || !name) return NULL;
    for (size_t i = 0; i < resp->header_count; i++) {
        if (resp->headers[i].name &&
#ifdef _WIN32
            _stricmp(resp->headers[i].name, name) == 0)
#else
            strcasecmp(resp->headers[i].name, name) == 0)
#endif
            return resp->headers[i].value;
    }
    return NULL;
}

/* ---- Persistent client ---- */

static https_client *g_apn_client = NULL;

static https_client *get_client(void) {
    if (!g_apn_client) {
        if (https_client_create(&g_apn_client) != 0) return NULL;
        https_client_set_redirect(g_apn_client, 5);
    }
    return g_apn_client;
}

/* ---- Main request function ---- */

PICO_API int pico_http_request(const char *method, const char *url,
                                const char *content_type,
                                const void *body, size_t body_len,
                                const PicoHttpOptions *opts,
                                PicoHttpResponse *out) {
    if (!method || !url || !out) return PICO_ERR_INVALID;
    memset(out, 0, sizeof(*out));

    /* Validate URL scheme */
    if (strncmp(url, "http://", 7) != 0 && strncmp(url, "https://", 8) != 0)
        return PICO_ERR_INVALID;

    https_client *c = get_client();
    if (!c) return PICO_ERR_CONNECT;

    /* Apply options */
    if (opts) {
        if (opts->connect_timeout_ms > 0)
            https_client_set_timeout(c, (u64)opts->connect_timeout_ms);
        for (int i = 0; i < (int)opts->header_count; i++)
            https_client_set_header(c, opts->headers[i].name, opts->headers[i].value);
    }
    if (content_type)
        https_client_set_header(c, "Content-Type", content_type);

    /* Map method */
    int meth = HTTP_GET;
    if (strcmp(method, "POST") == 0) meth = HTTP_POST;
    else if (strcmp(method, "PUT") == 0) meth = HTTP_PUT;
    else if (strcmp(method, "DELETE") == 0) meth = HTTP_DELETE;
    else if (strcmp(method, "HEAD") == 0) meth = HTTP_HEAD;
    else if (strcmp(method, "PATCH") == 0) meth = HTTP_PATCH;

    https_response resp;
    memset(&resp, 0, sizeof(resp));
    unsigned long rc = https_client_request(&resp, c, meth, url,
                                             NULL, (const u8 *)body, (u64)body_len);
    if (rc != 0) {
        https_response_free(&resp);
        /* Map apennines hatch codes to pico_http errors.
         *   1-4      : URL/arg hatches from https_client_request itself
         *   21/22/23 : DNS (query failed / no records / unknown rdata type)
         *   24       : TCP connect failed
         *   25       : TLS handshake
         *   26-30    : request build
         *   31       : serialize alloc
         *   32       : send
         *   33       : read
         *   34       : parse
         *   35+      : output build / alloc */
        if      (rc >= 21 && rc <= 23) return PICO_ERR_DNS;
        else if (rc == 24)             return PICO_ERR_CONNECT;
        else if (rc == 25)             return PICO_ERR_TLS;
        else if (rc == 32)             return PICO_ERR_SEND;
        else if (rc == 33)             return PICO_ERR_RECV;
        else if (rc == 34)             return PICO_ERR_PARSE;
        else                           return PICO_ERR_CONNECT;
    }

    /* Copy response */
    out->status = (int)resp.status;

    if (resp.body && resp.body_len > 0) {
        out->body = (char *)malloc((size_t)resp.body_len + 1);
        if (out->body) {
            memcpy(out->body, resp.body, (size_t)resp.body_len);
            out->body[(size_t)resp.body_len] = '\0';
        }
        out->body_len = (size_t)resp.body_len;
    }

    /* Convert headers */
    if (resp.headers.count > 0) {
        out->headers = (PicoHttpHeader *)calloc((size_t)resp.headers.count,
                                                  sizeof(PicoHttpHeader));
        if (out->headers) {
            out->header_count = (size_t)resp.headers.count;
            for (u64 i = 0; i < resp.headers.count; i++) {
                out->headers[i].name = strdup(resp.headers.items[i].name);
                out->headers[i].value = strdup(resp.headers.items[i].value);
            }
        }
    }

    https_response_free(&resp);
    return PICO_OK;
}

/* ---- Per-method wrappers ---- */

static char *build_url(const char *host, int port, const char *path, int tls) {
    char url[2048];
    snprintf(url, sizeof(url), "%s://%s:%d%s",
             tls ? "https" : "http", host, port, path ? path : "/");
    return strdup(url);
}

PICO_API int pico_http_get(const char *host, int port, const char *path,
                            const PicoHttpOptions *opts, PicoHttpResponse *out) {
    if (!host || !path || !out) return PICO_ERR_INVALID;
    char *url = build_url(host, port, path, port == 443);
    int rc = pico_http_request("GET", url, NULL, NULL, 0, opts, out);
    free(url); return rc;
}

PICO_API int pico_http_head(const char *host, int port, const char *path,
                             const PicoHttpOptions *opts, PicoHttpResponse *out) {
    char *url = build_url(host, port, path, port == 443);
    int rc = pico_http_request("HEAD", url, NULL, NULL, 0, opts, out);
    free(url); return rc;
}

PICO_API int pico_http_delete(const char *host, int port, const char *path,
                               const PicoHttpOptions *opts, PicoHttpResponse *out) {
    char *url = build_url(host, port, path, port == 443);
    int rc = pico_http_request("DELETE", url, NULL, NULL, 0, opts, out);
    free(url); return rc;
}

PICO_API int pico_http_put(const char *host, int port, const char *path,
                            const char *ct, const void *body, size_t blen,
                            const PicoHttpOptions *opts, PicoHttpResponse *out) {
    char *url = build_url(host, port, path, port == 443);
    int rc = pico_http_request("PUT", url, ct, body, blen, opts, out);
    free(url); return rc;
}

PICO_API int pico_http_post(const char *host, int port, const char *path,
                             const char *ct, const void *body, size_t blen,
                             const PicoHttpOptions *opts, PicoHttpResponse *out) {
    char *url = build_url(host, port, path, port == 443);
    int rc = pico_http_request("POST", url, ct, body, blen, opts, out);
    free(url); return rc;
}

PICO_API int pico_http_patch(const char *host, int port, const char *path,
                              const char *ct, const void *body, size_t blen,
                              const PicoHttpOptions *opts, PicoHttpResponse *out) {
    char *url = build_url(host, port, path, port == 443);
    int rc = pico_http_request("PATCH", url, ct, body, blen, opts, out);
    free(url); return rc;
}

/* Streaming not supported in apennines backend */
PICO_API int pico_http_get_stream(const char *host, int port, const char *path,
                                    const PicoHttpOptions *opts,
                                    PicoHttpResponse *out,
                                    PicoHttpWriteFn write_fn, void *userdata) {
    (void)host; (void)port; (void)path; (void)opts;
    (void)out; (void)write_fn; (void)userdata;
    return PICO_ERR_INVALID;
}

#endif /* PICO_HTTP_APENNINES */

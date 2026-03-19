/*
 * pico_http.h — Minimal HTTP/1.1 client
 *
 * A small, self-contained HTTP client for C11. No dependencies beyond
 * the C standard library and platform sockets (POSIX or Winsock2).
 *
 * Designed to be extracted as a standalone MIT-licensed library.
 *
 * Usage:
 *
 *   PicoHttpResponse res;
 *   int rc = pico_http_get("localhost", 8080, "/healthz", NULL, &res);
 *   if (rc == 0 && res.status == 200) {
 *       printf("%.*s\n", (int)res.body_len, res.body);
 *   }
 *   pico_http_response_free(&res);
 *
 * License: MIT
 * Copyright (c) 2026 The now contributors
 */
#ifndef PICO_HTTP_H
#define PICO_HTTP_H

#include <stddef.h>

/* ---- Export macro ---- */

#ifdef PICO_HTTP_STATIC
  #define PICO_API
#elif defined(_WIN32)
  #ifdef PICO_HTTP_BUILDING
    #define PICO_API __declspec(dllexport)
  #else
    #define PICO_API __declspec(dllimport)
  #endif
#else
  #define PICO_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Error codes ---- */

typedef enum {
    PICO_OK             =  0,  /* Success — response received and parsed */
    PICO_ERR_INVALID    = -1,  /* Invalid arguments (NULL host/path/out) */
    PICO_ERR_DNS        = -2,  /* DNS resolution failed (getaddrinfo) */
    PICO_ERR_CONNECT    = -3,  /* TCP connection failed (connect refused/timeout) */
    PICO_ERR_SEND       = -4,  /* Send failed (socket write error) */
    PICO_ERR_RECV       = -5,  /* Receive failed (socket read error, timeout) */
    PICO_ERR_PARSE      = -6,  /* Response parse error (malformed status line) */
    PICO_ERR_ALLOC      = -7,  /* Memory allocation failed */
    PICO_ERR_WINSOCK    = -8,  /* Winsock initialization failed (Windows only) */
    PICO_ERR_TOO_MANY_REDIRECTS = -9,  /* Redirect limit exceeded */
    PICO_ERR_TLS        = -10  /* TLS handshake or I/O error */
} PicoHttpError;

/* Return a short human-readable string for an error code.
 * Returns a static string. Never returns NULL. */
PICO_API const char *pico_http_strerror(int err);

/* ---- Types ---- */

/* A single HTTP header */
typedef struct {
    char *name;
    char *value;
} PicoHttpHeader;

/* HTTP response */
typedef struct {
    int              status;       /* HTTP status code (e.g. 200, 404) */
    char            *status_text;  /* Status text (e.g. "OK") */
    PicoHttpHeader  *headers;      /* Response headers */
    size_t           header_count;
    char            *body;         /* Response body (malloc'd, may contain binary) */
    size_t           body_len;     /* Body length in bytes */
} PicoHttpResponse;

/* Request options (pass NULL for defaults) */
typedef struct {
    const PicoHttpHeader *headers;      /* Extra request headers */
    size_t                header_count;
    int                   connect_timeout_ms; /* Connect timeout (0 = 5000) */
    int                   timeout_ms;        /* Read/write timeout (0 = 30000) */
    int                   max_redirects;     /* Max redirects to follow (0 = 10, -1 = disable) */
    /* TLS certificate verification (HTTPS only).
     * Default (0): verify server certificate against system CA store.
     * Set tls_noverify=1 to skip verification (development/localhost only). */
    int                   tls_noverify;
    const char           *ca_file;      /* Path to PEM CA bundle (NULL = system store) */
    const unsigned char  *ca_data;      /* PEM CA certificate data (NULL = use file or system) */
    size_t                ca_data_len;  /* Length of ca_data in bytes */
} PicoHttpOptions;

/* ---- Core API ---- */

/* All request functions return PICO_OK (0) on success or a negative
 * PicoHttpError on failure. A successful return does NOT imply a 2xx
 * status code — check out->status. */

/* Perform an HTTP GET request. */
PICO_API int pico_http_get(const char *host, int port, const char *path,
                            const PicoHttpOptions *opts,
                            PicoHttpResponse *out);

/* Perform an HTTP HEAD request (like GET but no response body). */
PICO_API int pico_http_head(const char *host, int port, const char *path,
                             const PicoHttpOptions *opts,
                             PicoHttpResponse *out);

/* Perform an HTTP DELETE request. */
PICO_API int pico_http_delete(const char *host, int port, const char *path,
                               const PicoHttpOptions *opts,
                               PicoHttpResponse *out);

/* Perform an HTTP PUT request with a body.
 * content_type may be NULL (defaults to "application/octet-stream"). */
PICO_API int pico_http_put(const char *host, int port, const char *path,
                            const char *content_type,
                            const void *body, size_t body_len,
                            const PicoHttpOptions *opts,
                            PicoHttpResponse *out);

/* Perform an HTTP POST request with a body. */
PICO_API int pico_http_post(const char *host, int port, const char *path,
                             const char *content_type,
                             const void *body, size_t body_len,
                             const PicoHttpOptions *opts,
                             PicoHttpResponse *out);

/* Perform an HTTP PATCH request with a body. */
PICO_API int pico_http_patch(const char *host, int port, const char *path,
                              const char *content_type,
                              const void *body, size_t body_len,
                              const PicoHttpOptions *opts,
                              PicoHttpResponse *out);

/* Perform an HTTP request with a full URL (http://host:port/path).
 * Convenience wrapper that parses the URL and dispatches to the
 * appropriate method function. method is e.g. "GET", "POST", etc. */
PICO_API int pico_http_request(const char *method, const char *url,
                                const char *content_type,
                                const void *body, size_t body_len,
                                const PicoHttpOptions *opts,
                                PicoHttpResponse *out);

/* Free all memory owned by a response. Safe to call on a zeroed struct. */
PICO_API void pico_http_response_free(PicoHttpResponse *res);

/* ---- Streaming API ---- */

/* Callback invoked for each chunk of response body data.
 * Return 0 to continue, non-zero to abort the download.
 * data points to chunk_len bytes of body data (not null-terminated). */
typedef int (*PicoHttpWriteFn)(const void *data, size_t chunk_len,
                                void *userdata);

/* Perform a streaming HTTP GET request.
 * Headers are parsed into *out as usual. The response body is NOT buffered;
 * instead, write_fn is called for each received chunk of body data.
 * out->body will be NULL and out->body_len will be 0 on return.
 * Caller must still call pico_http_response_free(out) to free headers.
 *
 * Returns PICO_OK on success, or a negative PicoHttpError.
 * If write_fn returns non-zero, the download is aborted and this function
 * returns PICO_ERR_RECV. */
PICO_API int pico_http_get_stream(const char *host, int port, const char *path,
                                    const PicoHttpOptions *opts,
                                    PicoHttpResponse *out,
                                    PicoHttpWriteFn write_fn, void *userdata);

/* ---- URL parsing helper ---- */

/* Parse "http[s]://host:port/path" into components.
 * host_out and path_out are malloc'd strings; caller must free.
 * *tls_out is set to 1 for https, 0 for http.
 * Returns 0 on success. */
PICO_API int pico_http_parse_url(const char *url,
                                  char **host_out, int *port_out,
                                  char **path_out);

/* Extended URL parser that also reports TLS requirement. */
PICO_API int pico_http_parse_url_ex(const char *url,
                                     char **host_out, int *port_out,
                                     char **path_out, int *tls_out);

/* ---- Response header lookup ---- */

/* Find a response header by name (case-insensitive).
 * Returns the header value or NULL if not found.
 * The returned pointer is owned by the response — do not free. */
PICO_API const char *pico_http_find_header(const PicoHttpResponse *res,
                                            const char *name);

/* ---- Library info ---- */

PICO_API const char *pico_http_version(void);

#ifdef __cplusplus
}
#endif

#endif /* PICO_HTTP_H */

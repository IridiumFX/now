/*
 * pico_http.c — Minimal HTTP/1.1 client
 *
 * A small, self-contained HTTP client for C11. No dependencies beyond
 * the C standard library and platform sockets (POSIX or Winsock2).
 *
 * License: MIT
 * Copyright (c) 2026 The now contributors
 */

#ifndef PICO_HTTP_BUILDING
  #define PICO_HTTP_BUILDING
#endif

#include "pico_http.h"
#include "pico_internal.h"
#include "pico_h2.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#ifdef PICO_HTTP_TLS
  #include <mbedtls/error.h>
  #ifdef _WIN32
    #include <wincrypt.h>
    #pragma comment(lib, "crypt32.lib")
  #endif
#endif

#ifdef PICO_HTTP_APENNINES
/* ================================================================
 *  Apennines HTTPS backend — see pico_http_apennines.c
 *  This file is not compiled when using the apennines backend.
 *  The adapter is linked instead.
 * ================================================================ */
#else /* Native pico_http implementation */

/* ---- Constants ---- */

#define PICO_HTTP_VERSION_STR  "0.3.0"
#define PICO_DEFAULT_TIMEOUT   30000
#define PICO_DEFAULT_CONNECT_TIMEOUT 5000
#define PICO_DEFAULT_MAX_REDIRECTS   10
#define PICO_RECV_BUF_SIZE     4096
#define PICO_INITIAL_CAP       8192

/* Transport functions (pico_conn_*, pico_connect, etc.) now in pico_transport.c */

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

/* ---- Internal helpers ---- */

/* Growable buffer for accumulating received data */
typedef struct {
    char  *data;
    size_t len;
    size_t cap;
} PicoBuf;

static int picobuf_init(PicoBuf *b, size_t cap) {
    b->data = (char *)malloc(cap);
    if (!b->data) return -1;
    b->len = 0;
    b->cap = cap;
    return 0;
}

static int picobuf_append(PicoBuf *b, const char *src, size_t n) {
    if (b->len + n > b->cap) {
        size_t newcap = b->cap * 2;
        while (newcap < b->len + n) newcap *= 2;
        char *tmp = (char *)realloc(b->data, newcap);
        if (!tmp) return -1;
        b->data = tmp;
        b->cap = newcap;
    }
    memcpy(b->data + b->len, src, n);
    b->len += n;
    return 0;
}

static void picobuf_free(PicoBuf *b) {
    free(b->data);
    b->data = NULL;
    b->len = b->cap = 0;
}

/* Transport functions (pico_set_timeout, pico_connect) moved to pico_transport.c */
/* Find \r\n\r\n in buffer, return offset of body start or -1 */
static int pico_find_header_end(const char *buf, size_t len) {
    for (size_t i = 0; i + 3 < len; i++) {
        if (buf[i] == '\r' && buf[i+1] == '\n' &&
            buf[i+2] == '\r' && buf[i+3] == '\n')
            return (int)(i + 4);
    }
    return -1;
}

/* Case-insensitive header lookup in raw header block */
static const char *pico_find_header_value(const char *headers, size_t hdr_len,
                                           const char *name) {
    size_t name_len = strlen(name);
    const char *p = headers;
    const char *end = headers + hdr_len;

    while (p < end) {
        const char *line_end = (const char *)memchr(p, '\n', (size_t)(end - p));
        if (!line_end) line_end = end;

        size_t line_len = (size_t)(line_end - p);
        if (line_len > name_len + 1) {
            int match = 1;
            for (size_t i = 0; i < name_len; i++) {
                if (tolower((unsigned char)p[i]) != tolower((unsigned char)name[i])) {
                    match = 0;
                    break;
                }
            }
            if (match && p[name_len] == ':') {
                const char *val = p + name_len + 1;
                while (val < line_end && (*val == ' ' || *val == '\t')) val++;
                return val;
            }
        }
        p = line_end + 1;
    }
    return NULL;
}

/* Parse status line: "HTTP/1.x SSS Reason\r\n" */
static int pico_parse_status_line(const char *buf, size_t len,
                                   int *status, char **status_text) {
    const char *sp1 = (const char *)memchr(buf, ' ', len);
    if (!sp1) return -1;
    sp1++;

    if (sp1 + 3 > buf + len) return -1;
    *status = (sp1[0] - '0') * 100 + (sp1[1] - '0') * 10 + (sp1[2] - '0');

    const char *text_start = sp1 + 3;
    if (text_start < buf + len && *text_start == ' ') text_start++;

    const char *line_end = (const char *)memchr(text_start, '\r',
                                                 (size_t)(buf + len - text_start));
    if (!line_end) line_end = buf + len;

    size_t text_len = (size_t)(line_end - text_start);
    *status_text = (char *)malloc(text_len + 1);
    if (!*status_text) return -1;
    memcpy(*status_text, text_start, text_len);
    (*status_text)[text_len] = '\0';

    return 0;
}

/* Parse response headers into PicoHttpHeader array */
static int pico_parse_headers(const char *hdr_block, size_t hdr_len,
                               PicoHttpHeader **out, size_t *count) {
    const char *p = (const char *)memchr(hdr_block, '\n', hdr_len);
    if (!p) { *out = NULL; *count = 0; return 0; }
    p++;

    size_t n = 0;
    const char *scan = p;
    const char *end = hdr_block + hdr_len;
    while (scan < end) {
        const char *le = (const char *)memchr(scan, '\n', (size_t)(end - scan));
        if (!le) break;
        if (le == scan || (le == scan + 1 && *scan == '\r')) break;
        n++;
        scan = le + 1;
    }

    if (n == 0) { *out = NULL; *count = 0; return 0; }

    PicoHttpHeader *hdrs = (PicoHttpHeader *)calloc(n, sizeof(PicoHttpHeader));
    if (!hdrs) return -1;

    size_t idx = 0;
    scan = p;
    while (scan < end && idx < n) {
        const char *le = (const char *)memchr(scan, '\n', (size_t)(end - scan));
        if (!le) break;

        const char *le_trim = le;
        if (le_trim > scan && *(le_trim - 1) == '\r') le_trim--;
        if (le_trim == scan) break;

        const char *colon = (const char *)memchr(scan, ':', (size_t)(le_trim - scan));
        if (colon) {
            size_t nlen = (size_t)(colon - scan);
            hdrs[idx].name = (char *)malloc(nlen + 1);
            if (hdrs[idx].name) {
                memcpy(hdrs[idx].name, scan, nlen);
                hdrs[idx].name[nlen] = '\0';
            }

            const char *vstart = colon + 1;
            while (vstart < le_trim && (*vstart == ' ' || *vstart == '\t')) vstart++;
            size_t vlen = (size_t)(le_trim - vstart);
            hdrs[idx].value = (char *)malloc(vlen + 1);
            if (hdrs[idx].value) {
                memcpy(hdrs[idx].value, vstart, vlen);
                hdrs[idx].value[vlen] = '\0';
            }
            idx++;
        }
        scan = le + 1;
    }

    *out = hdrs;
    *count = idx;
    return 0;
}

/* Read chunked transfer-encoding body */
static int pico_read_chunked_conn(PicoConn *conn, PicoBuf *body,
                                   const char *initial, size_t initial_len) {
    PicoBuf raw;
    if (picobuf_init(&raw, PICO_INITIAL_CAP) != 0) return -1;

    if (initial_len > 0)
        picobuf_append(&raw, initial, initial_len);

    for (;;) {
        size_t pos = 0;
        int done = 0;

        body->len = 0;

        while (pos < raw.len) {
            const char *crlf = NULL;
            for (size_t i = pos; i + 1 < raw.len; i++) {
                if (raw.data[i] == '\r' && raw.data[i+1] == '\n') {
                    crlf = raw.data + i;
                    break;
                }
            }
            if (!crlf) break;

            unsigned long chunk_size = 0;
            const char *cp = raw.data + pos;
            while (cp < crlf) {
                char c = *cp++;
                unsigned int digit;
                if (c >= '0' && c <= '9')      digit = (unsigned int)(c - '0');
                else if (c >= 'a' && c <= 'f') digit = (unsigned int)(c - 'a' + 10);
                else if (c >= 'A' && c <= 'F') digit = (unsigned int)(c - 'A' + 10);
                else if (c == ';') break;
                else break;
                chunk_size = chunk_size * 16 + digit;
            }

            size_t data_start = (size_t)(crlf - raw.data) + 2;

            if (chunk_size == 0) {
                done = 1;
                break;
            }

            if (data_start + chunk_size + 2 > raw.len) break;

            picobuf_append(body, raw.data + data_start, chunk_size);
            pos = data_start + chunk_size + 2;
        }

        if (done) break;

        char buf[PICO_RECV_BUF_SIZE];
        int n = pico_conn_recv(conn, buf, sizeof(buf));
        if (n <= 0) break;
        picobuf_append(&raw, buf, (size_t)n);
    }

    picobuf_free(&raw);
    return 0;
}

/* Extract Location header value as a malloc'd string from raw headers.
 * Returns NULL if not found. */
static char *pico_extract_location(const char *headers, size_t hdr_len) {
    const char *val = pico_find_header_value(headers, hdr_len, "location");
    if (!val) return NULL;

    /* Find end of value (until \r or \n) */
    const char *end = val;
    while (*end && *end != '\r' && *end != '\n') end++;

    size_t len = (size_t)(end - val);
    char *loc = (char *)malloc(len + 1);
    if (!loc) return NULL;
    memcpy(loc, val, len);
    loc[len] = '\0';
    return loc;
}

/* Core request function — single attempt (no redirect following) */
static int pico_request_once(const char *method,
                              const char *host, int port, const char *path,
                              const char *content_type,
                              const void *req_body, size_t req_body_len,
                              const PicoHttpOptions *opts,
                              int is_head, int use_tls,
                              PicoHttpResponse *out) {
    if (!host || !path || !out) return PICO_ERR_INVALID;
    memset(out, 0, sizeof(*out));

    if (pico_wsa_init() != 0) return PICO_ERR_WINSOCK;

#ifndef PICO_HTTP_TLS
    if (use_tls) return PICO_ERR_TLS;  /* TLS not compiled in */
#endif

    int rw_timeout = PICO_DEFAULT_TIMEOUT;
    int conn_timeout = PICO_DEFAULT_CONNECT_TIMEOUT;
    if (opts) {
        if (opts->timeout_ms > 0) rw_timeout = opts->timeout_ms;
        if (opts->connect_timeout_ms > 0) conn_timeout = opts->connect_timeout_ms;
    }

    /* Connect */
    int conn_err = 0;
    PicoConn conn;
    pico_conn_init(&conn);
    conn.sock = pico_connect(host, port, conn_timeout, &conn_err);
    if (conn.sock == PICO_INVALID_SOCKET) return conn_err;
    pico_set_timeout(conn.sock, rw_timeout);

    /* TLS handshake if needed */
#ifdef PICO_HTTP_TLS
    if (use_tls) {
        /* Determine verification mode and load CA certificates */
        int noverify = (opts && opts->tls_noverify);
        conn.tls_verify = !noverify;
        if (conn.tls_verify) {
            if (opts && opts->ca_data && opts->ca_data_len > 0)
                pico_load_ca_data(&conn, opts->ca_data, opts->ca_data_len);
            else if (opts && opts->ca_file)
                pico_load_ca_file(&conn, opts->ca_file);
            else
                pico_load_system_ca(&conn);
        }
        int tls_rc = pico_tls_handshake(&conn, host);
        if (tls_rc != PICO_OK) {
            pico_conn_close(&conn);
            return tls_rc;
        }
    }
#endif

#ifdef PICO_HTTP_TLS
    /* HTTP/2 dispatch if ALPN negotiated h2 */
    if (conn.alpn_h2) {
        int h2rc = pico_h2_request(&conn, method, host, path,
                                    content_type, req_body, req_body_len,
                                    opts, is_head, out);
        pico_conn_close(&conn);
        return h2rc;
    }
#endif

    /* Build request */
    PicoBuf reqbuf;
    if (picobuf_init(&reqbuf, 1024) != 0) {
        pico_conn_close(&conn);
        return PICO_ERR_ALLOC;
    }

    int default_port = use_tls ? 443 : 80;
    char line[512];
    int llen;
    if (port == default_port) {
        llen = snprintf(line, sizeof(line), "%s %s HTTP/1.1\r\nHost: %s\r\n",
                        method, path, host);
    } else {
        llen = snprintf(line, sizeof(line), "%s %s HTTP/1.1\r\nHost: %s:%d\r\n",
                        method, path, host, port);
    }
    picobuf_append(&reqbuf, line, (size_t)llen);

    picobuf_append(&reqbuf, "Connection: close\r\n", 19);

    if (req_body && req_body_len > 0) {
        const char *ct = content_type ? content_type : "application/octet-stream";
        llen = snprintf(line, sizeof(line), "Content-Type: %s\r\n", ct);
        picobuf_append(&reqbuf, line, (size_t)llen);
        llen = snprintf(line, sizeof(line), "Content-Length: %zu\r\n",
                        req_body_len);
        picobuf_append(&reqbuf, line, (size_t)llen);
    }

    if (opts && opts->headers && opts->header_count > 0) {
        for (size_t i = 0; i < opts->header_count; i++) {
            llen = snprintf(line, sizeof(line), "%s: %s\r\n",
                            opts->headers[i].name, opts->headers[i].value);
            picobuf_append(&reqbuf, line, (size_t)llen);
        }
    }

    picobuf_append(&reqbuf, "\r\n", 2);

    if (pico_conn_send(&conn, reqbuf.data, reqbuf.len) != 0) {
        picobuf_free(&reqbuf);
        pico_conn_close(&conn);
        return PICO_ERR_SEND;
    }
    picobuf_free(&reqbuf);

    if (req_body && req_body_len > 0) {
        if (pico_conn_send(&conn, (const char *)req_body, req_body_len) != 0) {
            pico_conn_close(&conn);
            return PICO_ERR_SEND;
        }
    }

    /* Receive response */
    PicoBuf resp;
    if (picobuf_init(&resp, PICO_INITIAL_CAP) != 0) {
        pico_conn_close(&conn);
        return PICO_ERR_ALLOC;
    }

    int header_end = -1;
    char recvbuf[PICO_RECV_BUF_SIZE];

    while (header_end < 0) {
        int n = pico_conn_recv(&conn, recvbuf, sizeof(recvbuf));
        if (n <= 0) {
            if (resp.len > 0) break;
            picobuf_free(&resp);
            pico_conn_close(&conn);
            return PICO_ERR_RECV;
        }
        picobuf_append(&resp, recvbuf, (size_t)n);
        header_end = pico_find_header_end(resp.data, resp.len);
    }

    if (header_end < 0) {
        picobuf_free(&resp);
        pico_conn_close(&conn);
        return PICO_ERR_PARSE;
    }

    /* Parse status line */
    if (pico_parse_status_line(resp.data, (size_t)header_end,
                                &out->status, &out->status_text) != 0) {
        picobuf_free(&resp);
        pico_conn_close(&conn);
        return PICO_ERR_PARSE;
    }

    /* Parse response headers */
    pico_parse_headers(resp.data, (size_t)header_end,
                       &out->headers, &out->header_count);

    /* HEAD responses and 1xx/204/304 have no body */
    if (is_head || out->status == 204 || out->status == 304 ||
        (out->status >= 100 && out->status < 200)) {
        out->body = NULL;
        out->body_len = 0;
        picobuf_free(&resp);
        pico_conn_close(&conn);
        return PICO_OK;
    }

    /* Determine body reading strategy */
    const char *te = pico_find_header_value(resp.data, (size_t)header_end,
                                             "transfer-encoding");
    int chunked = 0;
    if (te) {
        if ((te[0] == 'c' || te[0] == 'C') &&
            (te[1] == 'h' || te[1] == 'H'))
            chunked = 1;
    }

    size_t body_already = resp.len - (size_t)header_end;

    if (chunked) {
        PicoBuf body;
        if (picobuf_init(&body, PICO_INITIAL_CAP) != 0) {
            picobuf_free(&resp);
            pico_conn_close(&conn);
            return PICO_ERR_ALLOC;
        }
        pico_read_chunked_conn(&conn, &body,
                               resp.data + header_end, body_already);
        out->body = body.data;
        out->body_len = body.len;
    } else {
        const char *cl = pico_find_header_value(resp.data, (size_t)header_end,
                                                 "content-length");
        size_t content_length = 0;
        int have_cl = 0;
        if (cl) {
            content_length = (size_t)strtoul(cl, NULL, 10);
            have_cl = 1;
        }

        PicoBuf body;
        if (picobuf_init(&body, have_cl ? content_length + 1 : PICO_INITIAL_CAP) != 0) {
            picobuf_free(&resp);
            pico_conn_close(&conn);
            return PICO_ERR_ALLOC;
        }

        if (body_already > 0)
            picobuf_append(&body, resp.data + header_end, body_already);

        if (have_cl) {
            while (body.len < content_length) {
                int n = pico_conn_recv(&conn, recvbuf, sizeof(recvbuf));
                if (n <= 0) break;
                picobuf_append(&body, recvbuf, (size_t)n);
            }
        } else {
            for (;;) {
                int n = pico_conn_recv(&conn, recvbuf, sizeof(recvbuf));
                if (n <= 0) break;
                picobuf_append(&body, recvbuf, (size_t)n);
            }
        }

        out->body = body.data;
        out->body_len = body.len;
    }

    picobuf_free(&resp);
    pico_conn_close(&conn);
    return PICO_OK;
}

/* Core request function with redirect following */
static int pico_request(const char *method,
                         const char *host, int port, const char *path,
                         const char *content_type,
                         const void *req_body, size_t req_body_len,
                         const PicoHttpOptions *opts,
                         int is_head, int use_tls,
                         PicoHttpResponse *out) {
    if (!method || !host || !path || !out)
        return PICO_ERR_INVALID;

    int max_redirects = PICO_DEFAULT_MAX_REDIRECTS;
    if (opts) {
        if (opts->max_redirects < 0)
            max_redirects = 0;  /* disabled */
        else if (opts->max_redirects > 0)
            max_redirects = opts->max_redirects;
    }

    /* Mutable copies for redirect chasing */
    char *cur_host = strdup(host);
    char *cur_path = strdup(path);
    int   cur_port = port;
    int   cur_tls  = use_tls;
    if (!cur_host || !cur_path) {
        free(cur_host);
        free(cur_path);
        return PICO_ERR_ALLOC;
    }

    int redirects = 0;
    int rc;

    for (;;) {
        rc = pico_request_once(method, cur_host, cur_port, cur_path,
                               content_type, req_body, req_body_len,
                               opts, is_head, cur_tls, out);
        if (rc != PICO_OK) break;

        /* Check for redirect */
        if (max_redirects > 0 &&
            (out->status == 301 || out->status == 302 ||
             out->status == 303 || out->status == 307 ||
             out->status == 308)) {

            if (++redirects > max_redirects) {
                pico_http_response_free(out);
                rc = PICO_ERR_TOO_MANY_REDIRECTS;
                break;
            }

            /* Find Location header from raw response — but we already
             * parsed headers into the struct, so use that */
            const char *location = pico_http_find_header(out, "Location");
            if (!location || !*location) {
                /* No Location header — return the redirect response as-is */
                break;
            }

            /* Parse the Location */
            char *new_host = NULL, *new_path = NULL;
            int new_port = 80;
            int new_tls = cur_tls;

            if (strncmp(location, "http://", 7) == 0 ||
                strncmp(location, "https://", 8) == 0) {
                /* Absolute URL */
                char *loc_copy = strdup(location);
                if (!loc_copy) { rc = PICO_ERR_ALLOC; break; }
                if (pico_http_parse_url_ex(loc_copy, &new_host, &new_port,
                                            &new_path, &new_tls) != 0) {
                    free(loc_copy);
                    break; /* Can't parse — return redirect response as-is */
                }
                free(loc_copy);
            } else if (location[0] == '/') {
                /* Absolute path — same host */
                new_host = strdup(cur_host);
                new_path = strdup(location);
                new_port = cur_port;
            } else {
                /* Relative path — not supported well, just prepend / */
                new_host = strdup(cur_host);
                new_port = cur_port;
                size_t llen = strlen(location);
                new_path = (char *)malloc(llen + 2);
                if (new_path) {
                    new_path[0] = '/';
                    memcpy(new_path + 1, location, llen + 1);
                }
            }

            if (!new_host || !new_path) {
                free(new_host);
                free(new_path);
                rc = PICO_ERR_ALLOC;
                pico_http_response_free(out);
                break;
            }

            /* 303 converts POST/PUT to GET */
            if (out->status == 303 && strcmp(method, "GET") != 0 &&
                strcmp(method, "HEAD") != 0) {
                method = "GET";
                req_body = NULL;
                req_body_len = 0;
                content_type = NULL;
                is_head = 0;
            }

            /* Clean up current response and redirect */
            pico_http_response_free(out);
            free(cur_host);
            free(cur_path);
            cur_host = new_host;
            cur_path = new_path;
            cur_port = new_port;
            cur_tls  = new_tls;
            continue;
        }

        /* Not a redirect — done */
        break;
    }

    free(cur_host);
    free(cur_path);
    return rc;
}

/* ---- Public API ---- */

PICO_API int pico_http_get(const char *host, int port, const char *path,
                            const PicoHttpOptions *opts,
                            PicoHttpResponse *out) {
    return pico_request("GET", host, port, path, NULL, NULL, 0, opts, 0, 0, out);
}

PICO_API int pico_http_head(const char *host, int port, const char *path,
                             const PicoHttpOptions *opts,
                             PicoHttpResponse *out) {
    return pico_request("HEAD", host, port, path, NULL, NULL, 0, opts, 1, 0, out);
}

PICO_API int pico_http_delete(const char *host, int port, const char *path,
                               const PicoHttpOptions *opts,
                               PicoHttpResponse *out) {
    return pico_request("DELETE", host, port, path, NULL, NULL, 0, opts, 0, 0, out);
}

PICO_API int pico_http_put(const char *host, int port, const char *path,
                            const char *content_type,
                            const void *body, size_t body_len,
                            const PicoHttpOptions *opts,
                            PicoHttpResponse *out) {
    return pico_request("PUT", host, port, path, content_type,
                        body, body_len, opts, 0, 0, out);
}

PICO_API int pico_http_post(const char *host, int port, const char *path,
                             const char *content_type,
                             const void *body, size_t body_len,
                             const PicoHttpOptions *opts,
                             PicoHttpResponse *out) {
    return pico_request("POST", host, port, path, content_type,
                        body, body_len, opts, 0, 0, out);
}

PICO_API int pico_http_patch(const char *host, int port, const char *path,
                              const char *content_type,
                              const void *body, size_t body_len,
                              const PicoHttpOptions *opts,
                              PicoHttpResponse *out) {
    return pico_request("PATCH", host, port, path, content_type,
                        body, body_len, opts, 0, 0, out);
}

PICO_API int pico_http_request(const char *method, const char *url,
                                const char *content_type,
                                const void *body, size_t body_len,
                                const PicoHttpOptions *opts,
                                PicoHttpResponse *out) {
    if (!method || !url || !out) return PICO_ERR_INVALID;

    char *host = NULL, *path = NULL;
    int port = 80, use_tls = 0;
    if (pico_http_parse_url_ex(url, &host, &port, &path, &use_tls) != 0) {
        return PICO_ERR_INVALID;
    }

    int is_head = (strcmp(method, "HEAD") == 0);
    int rc = pico_request(method, host, port, path, content_type,
                          body, body_len, opts, is_head, use_tls, out);
    free(host);
    free(path);
    return rc;
}

/* ---- Streaming support ---- */

/* Stream chunked body through callback */
static int pico_stream_chunked(PicoConn *conn, const char *initial,
                                size_t initial_len, PicoHttpWriteFn fn,
                                void *ud) {
    PicoBuf raw;
    if (picobuf_init(&raw, PICO_INITIAL_CAP) != 0) return -1;
    if (initial_len > 0)
        picobuf_append(&raw, initial, initial_len);

    for (;;) {
        size_t pos = 0;
        int done = 0;

        while (pos < raw.len) {
            const char *crlf = NULL;
            for (size_t i = pos; i + 1 < raw.len; i++) {
                if (raw.data[i] == '\r' && raw.data[i+1] == '\n') {
                    crlf = raw.data + i;
                    break;
                }
            }
            if (!crlf) break;

            unsigned long chunk_size = 0;
            const char *cp = raw.data + pos;
            while (cp < crlf) {
                char c = *cp++;
                unsigned int digit;
                if (c >= '0' && c <= '9')      digit = (unsigned int)(c - '0');
                else if (c >= 'a' && c <= 'f') digit = (unsigned int)(c - 'a' + 10);
                else if (c >= 'A' && c <= 'F') digit = (unsigned int)(c - 'A' + 10);
                else if (c == ';') break;
                else break;
                chunk_size = chunk_size * 16 + digit;
            }

            size_t data_start = (size_t)(crlf - raw.data) + 2;
            if (chunk_size == 0) { done = 1; break; }
            if (data_start + chunk_size + 2 > raw.len) break;

            if (fn(raw.data + data_start, chunk_size, ud) != 0) {
                picobuf_free(&raw);
                return -1;
            }
            pos = data_start + chunk_size + 2;
        }

        if (done) break;

        /* Compact consumed data */
        if (pos > 0 && pos < raw.len) {
            memmove(raw.data, raw.data + pos, raw.len - pos);
            raw.len -= pos;
        } else if (pos >= raw.len) {
            raw.len = 0;
        }

        char buf[PICO_RECV_BUF_SIZE];
        int n = pico_conn_recv(conn, buf, sizeof(buf));
        if (n <= 0) break;
        picobuf_append(&raw, buf, (size_t)n);
    }

    picobuf_free(&raw);
    return 0;
}

/* Core streaming request — single attempt, no redirects */
static int pico_request_once_stream(const char *host, int port, const char *path,
                                     const PicoHttpOptions *opts, int use_tls,
                                     PicoHttpResponse *out,
                                     PicoHttpWriteFn write_fn, void *userdata) {
    if (!host || !path || !out || !write_fn) return PICO_ERR_INVALID;
    memset(out, 0, sizeof(*out));

    if (pico_wsa_init() != 0) return PICO_ERR_WINSOCK;

#ifndef PICO_HTTP_TLS
    if (use_tls) return PICO_ERR_TLS;
#endif

    int rw_timeout = PICO_DEFAULT_TIMEOUT;
    int conn_timeout = PICO_DEFAULT_CONNECT_TIMEOUT;
    if (opts) {
        if (opts->timeout_ms > 0) rw_timeout = opts->timeout_ms;
        if (opts->connect_timeout_ms > 0) conn_timeout = opts->connect_timeout_ms;
    }

    int conn_err = 0;
    PicoConn conn;
    pico_conn_init(&conn);
    conn.sock = pico_connect(host, port, conn_timeout, &conn_err);
    if (conn.sock == PICO_INVALID_SOCKET) return conn_err;
    pico_set_timeout(conn.sock, rw_timeout);

#ifdef PICO_HTTP_TLS
    if (use_tls) {
        int noverify = (opts && opts->tls_noverify);
        conn.tls_verify = !noverify;
        if (conn.tls_verify) {
            if (opts && opts->ca_data && opts->ca_data_len > 0)
                pico_load_ca_data(&conn, opts->ca_data, opts->ca_data_len);
            else if (opts && opts->ca_file)
                pico_load_ca_file(&conn, opts->ca_file);
            else
                pico_load_system_ca(&conn);
        }
        int tls_rc = pico_tls_handshake(&conn, host);
        if (tls_rc != PICO_OK) {
            pico_conn_close(&conn);
            return tls_rc;
        }
    }
#endif

    /* Build GET request */
    PicoBuf reqbuf;
    if (picobuf_init(&reqbuf, 1024) != 0) {
        pico_conn_close(&conn);
        return PICO_ERR_ALLOC;
    }

    int default_port = use_tls ? 443 : 80;
    char line[512];
    int llen;
    if (port == default_port)
        llen = snprintf(line, sizeof(line), "GET %s HTTP/1.1\r\nHost: %s\r\n", path, host);
    else
        llen = snprintf(line, sizeof(line), "GET %s HTTP/1.1\r\nHost: %s:%d\r\n", path, host, port);
    picobuf_append(&reqbuf, line, (size_t)llen);
    picobuf_append(&reqbuf, "Connection: close\r\n", 19);

    if (opts && opts->headers && opts->header_count > 0) {
        for (size_t i = 0; i < opts->header_count; i++) {
            llen = snprintf(line, sizeof(line), "%s: %s\r\n",
                            opts->headers[i].name, opts->headers[i].value);
            picobuf_append(&reqbuf, line, (size_t)llen);
        }
    }

    picobuf_append(&reqbuf, "\r\n", 2);

    if (pico_conn_send(&conn, reqbuf.data, reqbuf.len) != 0) {
        picobuf_free(&reqbuf);
        pico_conn_close(&conn);
        return PICO_ERR_SEND;
    }
    picobuf_free(&reqbuf);

    /* Receive and parse headers */
    PicoBuf resp;
    if (picobuf_init(&resp, PICO_INITIAL_CAP) != 0) {
        pico_conn_close(&conn);
        return PICO_ERR_ALLOC;
    }

    int header_end = -1;
    char recvbuf[PICO_RECV_BUF_SIZE];

    while (header_end < 0) {
        int n = pico_conn_recv(&conn, recvbuf, sizeof(recvbuf));
        if (n <= 0) {
            if (resp.len > 0) break;
            picobuf_free(&resp);
            pico_conn_close(&conn);
            return PICO_ERR_RECV;
        }
        picobuf_append(&resp, recvbuf, (size_t)n);
        header_end = pico_find_header_end(resp.data, resp.len);
    }

    if (header_end < 0) {
        picobuf_free(&resp);
        pico_conn_close(&conn);
        return PICO_ERR_PARSE;
    }

    if (pico_parse_status_line(resp.data, (size_t)header_end,
                                &out->status, &out->status_text) != 0) {
        picobuf_free(&resp);
        pico_conn_close(&conn);
        return PICO_ERR_PARSE;
    }

    pico_parse_headers(resp.data, (size_t)header_end,
                       &out->headers, &out->header_count);

    out->body = NULL;
    out->body_len = 0;

    /* For redirect responses, buffer a small body so the redirect handler
     * can inspect Location header. Don't stream these. */
    if (out->status == 301 || out->status == 302 || out->status == 303 ||
        out->status == 307 || out->status == 308) {
        picobuf_free(&resp);
        pico_conn_close(&conn);
        return PICO_OK;
    }

    /* Stream body through callback */
    const char *te = pico_find_header_value(resp.data, (size_t)header_end,
                                             "transfer-encoding");
    int chunked = 0;
    if (te && (te[0] == 'c' || te[0] == 'C') &&
              (te[1] == 'h' || te[1] == 'H'))
        chunked = 1;

    size_t body_already = resp.len - (size_t)header_end;
    int stream_err = 0;

    if (chunked) {
        stream_err = pico_stream_chunked(&conn, resp.data + header_end,
                                          body_already, write_fn, userdata);
    } else {
        const char *cl = pico_find_header_value(resp.data, (size_t)header_end,
                                                 "content-length");
        size_t content_length = 0;
        int have_cl = 0;
        if (cl) {
            content_length = (size_t)strtoul(cl, NULL, 10);
            have_cl = 1;
        }

        /* Deliver initial body data */
        size_t delivered = 0;
        if (body_already > 0) {
            if (write_fn(resp.data + header_end, body_already, userdata) != 0) {
                picobuf_free(&resp);
                pico_conn_close(&conn);
                return PICO_ERR_RECV;
            }
            delivered = body_already;
        }

        /* Read remaining body */
        if (have_cl) {
            while (delivered < content_length) {
                int n = pico_conn_recv(&conn, recvbuf, sizeof(recvbuf));
                if (n <= 0) break;
                if (write_fn(recvbuf, (size_t)n, userdata) != 0) {
                    stream_err = -1;
                    break;
                }
                delivered += (size_t)n;
            }
        } else {
            for (;;) {
                int n = pico_conn_recv(&conn, recvbuf, sizeof(recvbuf));
                if (n <= 0) break;
                if (write_fn(recvbuf, (size_t)n, userdata) != 0) {
                    stream_err = -1;
                    break;
                }
            }
        }
    }

    picobuf_free(&resp);
    pico_conn_close(&conn);
    return stream_err ? PICO_ERR_RECV : PICO_OK;
}

PICO_API int pico_http_get_stream(const char *host, int port, const char *path,
                                    const PicoHttpOptions *opts,
                                    PicoHttpResponse *out,
                                    PicoHttpWriteFn write_fn, void *userdata) {
    if (!host || !path || !out || !write_fn)
        return PICO_ERR_INVALID;

    int max_redirects = PICO_DEFAULT_MAX_REDIRECTS;
    if (opts) {
        if (opts->max_redirects < 0)
            max_redirects = 0;
        else if (opts->max_redirects > 0)
            max_redirects = opts->max_redirects;
    }

    char *cur_host = strdup(host);
    char *cur_path = strdup(path);
    int   cur_port = port;
    int   cur_tls  = 0;
    if (!cur_host || !cur_path) {
        free(cur_host); free(cur_path);
        return PICO_ERR_ALLOC;
    }

    int redirects = 0;
    int rc;

    for (;;) {
        rc = pico_request_once_stream(cur_host, cur_port, cur_path,
                                       opts, cur_tls, out,
                                       write_fn, userdata);
        if (rc != PICO_OK) break;

        if (max_redirects > 0 &&
            (out->status == 301 || out->status == 302 ||
             out->status == 303 || out->status == 307 ||
             out->status == 308)) {

            if (++redirects > max_redirects) {
                pico_http_response_free(out);
                rc = PICO_ERR_TOO_MANY_REDIRECTS;
                break;
            }

            const char *location = pico_http_find_header(out, "Location");
            if (!location || !*location) break;

            char *new_host = NULL, *new_path = NULL;
            int new_port = 80, new_tls = cur_tls;

            if (strncmp(location, "http://", 7) == 0 ||
                strncmp(location, "https://", 8) == 0) {
                char *loc_copy = strdup(location);
                if (!loc_copy) { rc = PICO_ERR_ALLOC; break; }
                if (pico_http_parse_url_ex(loc_copy, &new_host, &new_port,
                                            &new_path, &new_tls) != 0) {
                    free(loc_copy);
                    break;
                }
                free(loc_copy);
            } else if (location[0] == '/') {
                new_host = strdup(cur_host);
                new_path = strdup(location);
                new_port = cur_port;
            } else {
                new_host = strdup(cur_host);
                new_port = cur_port;
                size_t llen2 = strlen(location);
                new_path = (char *)malloc(llen2 + 2);
                if (new_path) { new_path[0] = '/'; memcpy(new_path + 1, location, llen2 + 1); }
            }

            if (!new_host || !new_path) {
                free(new_host); free(new_path);
                rc = PICO_ERR_ALLOC;
                pico_http_response_free(out);
                break;
            }

            pico_http_response_free(out);
            free(cur_host); free(cur_path);
            cur_host = new_host;
            cur_path = new_path;
            cur_port = new_port;
            cur_tls  = new_tls;
            continue;
        }

        break;
    }

    free(cur_host);
    free(cur_path);
    return rc;
}

PICO_API void pico_http_response_free(PicoHttpResponse *res) {
    if (!res) return;
    free(res->status_text);
    if (res->headers) {
        for (size_t i = 0; i < res->header_count; i++) {
            free(res->headers[i].name);
            free(res->headers[i].value);
        }
        free(res->headers);
    }
    free(res->body);
    memset(res, 0, sizeof(*res));
}

PICO_API const char *pico_http_find_header(const PicoHttpResponse *res,
                                            const char *name) {
    if (!res || !name) return NULL;
    size_t name_len = strlen(name);
    for (size_t i = 0; i < res->header_count; i++) {
        if (!res->headers[i].name) continue;
        size_t hlen = strlen(res->headers[i].name);
        if (hlen != name_len) continue;
        int match = 1;
        for (size_t j = 0; j < name_len; j++) {
            if (tolower((unsigned char)res->headers[i].name[j]) !=
                tolower((unsigned char)name[j])) {
                match = 0;
                break;
            }
        }
        if (match) return res->headers[i].value;
    }
    return NULL;
}

PICO_API int pico_http_parse_url_ex(const char *url,
                                     char **host_out, int *port_out,
                                     char **path_out, int *tls_out) {
    if (!url || !host_out || !port_out || !path_out) return -1;

    *host_out = NULL;
    *path_out = NULL;
    int is_tls = 0;

    const char *p = url;
    if (strncmp(p, "https://", 8) == 0) {
        p += 8;
        is_tls = 1;
        *port_out = 443;
    } else if (strncmp(p, "http://", 7) == 0) {
        p += 7;
        *port_out = 80;
    } else {
        return -1;
    }

    if (tls_out) *tls_out = is_tls;

    const char *slash = strchr(p, '/');
    const char *host_end = slash ? slash : p + strlen(p);

    const char *colon = (const char *)memchr(p, ':', (size_t)(host_end - p));
    if (colon) {
        size_t hlen = (size_t)(colon - p);
        *host_out = (char *)malloc(hlen + 1);
        if (!*host_out) return -1;
        memcpy(*host_out, p, hlen);
        (*host_out)[hlen] = '\0';
        *port_out = atoi(colon + 1);
    } else {
        size_t hlen = (size_t)(host_end - p);
        *host_out = (char *)malloc(hlen + 1);
        if (!*host_out) return -1;
        memcpy(*host_out, p, hlen);
        (*host_out)[hlen] = '\0';
    }

    if (slash) {
        size_t plen = strlen(slash);
        *path_out = (char *)malloc(plen + 1);
        if (!*path_out) { free(*host_out); *host_out = NULL; return -1; }
        memcpy(*path_out, slash, plen);
        (*path_out)[plen] = '\0';
    } else {
        *path_out = (char *)malloc(2);
        if (!*path_out) { free(*host_out); *host_out = NULL; return -1; }
        (*path_out)[0] = '/';
        (*path_out)[1] = '\0';
    }

    return 0;
}

PICO_API int pico_http_parse_url(const char *url,
                                  char **host_out, int *port_out,
                                  char **path_out) {
    return pico_http_parse_url_ex(url, host_out, port_out, path_out, NULL);
}

PICO_API const char *pico_http_version(void) {
    return PICO_HTTP_VERSION_STR;
}

#endif /* !PICO_HTTP_APENNINES */

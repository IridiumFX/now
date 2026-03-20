/*
 * pico_ws.h — Minimal WebSocket client
 *
 * A small, synchronous WebSocket client for C11. Uses libwebsockets
 * internally as a temporary backend. Designed to be replaced by a
 * native implementation later.
 *
 * Usage:
 *
 *   PicoWs *ws = pico_ws_connect("ws://localhost:8080/stream", NULL);
 *   if (ws) {
 *       pico_ws_send(ws, "hello", 5, 0);
 *       char buf[4096];
 *       int n = pico_ws_recv(ws, buf, sizeof(buf), 1000);
 *       if (n > 0) printf("got %d bytes\n", n);
 *       pico_ws_close(ws);
 *   }
 *
 * License: MIT
 * Copyright (c) 2026 The now contributors
 */
#ifndef PICO_WS_H
#define PICO_WS_H

#include <stddef.h>

/* ---- Export macro (shared with pico_http) ---- */

#ifdef PICO_HTTP_STATIC
  #define PICO_WS_API
#elif defined(_WIN32)
  #ifdef PICO_HTTP_BUILDING
    #define PICO_WS_API __declspec(dllexport)
  #else
    #define PICO_WS_API __declspec(dllimport)
  #endif
#else
  #define PICO_WS_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Error codes ---- */

typedef enum {
    PICO_WS_OK             =  0,
    PICO_WS_ERR_INVALID    = -1,   /* NULL or bad arguments */
    PICO_WS_ERR_URL        = -2,   /* Malformed URL */
    PICO_WS_ERR_CONNECT    = -3,   /* Connection failed */
    PICO_WS_ERR_TLS        = -4,   /* TLS handshake failed */
    PICO_WS_ERR_HANDSHAKE  = -5,   /* WebSocket upgrade failed */
    PICO_WS_ERR_SEND       = -6,   /* Send failed */
    PICO_WS_ERR_RECV       = -7,   /* Receive failed or timeout */
    PICO_WS_ERR_CLOSED     = -8,   /* Connection closed by peer */
    PICO_WS_ERR_ALLOC      = -9,   /* Memory allocation failed */
    PICO_WS_ERR_NOTSUP     = -10   /* WebSocket support not compiled in */
} PicoWsError;

/* Return human-readable string for error code. Never returns NULL. */
PICO_WS_API const char *pico_ws_strerror(int err);

/* ---- Types ---- */

/* Opaque WebSocket connection handle */
typedef struct PicoWs PicoWs;

/* Connection options (pass NULL for defaults) */
typedef struct {
    int   connect_timeout_ms;   /* 0 = 5000 */
    int   recv_timeout_ms;      /* Default recv timeout; 0 = 30000 */
    const char *protocol;       /* Sub-protocol to request (NULL = none) */
    const char *const *extra_headers;  /* NULL-terminated array of "Name: Value" strings */
    /* TLS certificate verification (wss:// only).
     * Default (0): verify server certificate against system CA store.
     * Set tls_noverify=1 to skip verification (development/localhost only). */
    int                   tls_noverify;
    const char           *ca_file;      /* Path to PEM CA bundle (NULL = system store) */
    const unsigned char  *ca_data;      /* PEM CA certificate data */
    size_t                ca_data_len;
    int                   permessage_deflate;  /* 1 = negotiate permessage-deflate */
} PicoWsOptions;

/* ---- Core API ---- */

/* Connect to a WebSocket endpoint.
 * url: "ws://host:port/path" or "wss://host:port/path"
 * Returns connection handle on success, NULL on failure.
 * If err_out is non-NULL, receives the error code on failure. */
PICO_WS_API PicoWs *pico_ws_connect(const char *url,
                                     const PicoWsOptions *opts,
                                     int *err_out);

/* Send data on an open WebSocket connection.
 * is_binary: 0 for text frame, 1 for binary frame.
 * Returns PICO_WS_OK on success. */
PICO_WS_API int pico_ws_send(PicoWs *ws, const void *data, size_t len,
                              int is_binary);

/* Receive data from an open WebSocket connection.
 * Blocks up to timeout_ms (0 = use default from options).
 * Returns number of bytes received (> 0), or negative PicoWsError.
 * *is_binary_out is set to 1 for binary frames, 0 for text. */
PICO_WS_API int pico_ws_recv(PicoWs *ws, void *buf, size_t buflen,
                              int timeout_ms, int *is_binary_out);

/* Close the WebSocket connection and free all resources.
 * Safe to call with NULL. */
PICO_WS_API void pico_ws_close(PicoWs *ws);

/* ---- Library info ---- */

PICO_WS_API const char *pico_ws_version(void);

#ifdef __cplusplus
}
#endif

#endif /* PICO_WS_H */

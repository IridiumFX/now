/*
 * pico_h2.h — HTTP/2 client support for pico_http
 *
 * Internal header. Provides HPACK encoding/decoding, frame codec,
 * and single-stream HTTP/2 request handling over TLS (h2 via ALPN).
 *
 * Only compiled when PICO_HTTP_TLS is defined and PICO_HTTP_APENNINES is not.
 *
 * License: MIT
 * Copyright (c) 2026 The now contributors
 */
#ifndef PICO_H2_H
#define PICO_H2_H

#if defined(PICO_HTTP_TLS) && !defined(PICO_HTTP_APENNINES)

#include "pico_internal.h"
#include "pico_http.h"
#include <stdint.h>

/* ---- Frame types (RFC 7540 section 6) ---- */
#define H2_DATA          0x0
#define H2_HEADERS       0x1
#define H2_PRIORITY      0x2
#define H2_RST_STREAM    0x3
#define H2_SETTINGS      0x4
#define H2_PUSH_PROMISE  0x5
#define H2_PING          0x6
#define H2_GOAWAY        0x7
#define H2_WINDOW_UPDATE 0x8
#define H2_CONTINUATION  0x9

/* ---- Frame flags ---- */
#define H2_FLAG_END_STREAM  0x1
#define H2_FLAG_END_HEADERS 0x4
#define H2_FLAG_PADDED      0x8
#define H2_FLAG_ACK         0x1  /* for SETTINGS and PING */

/* ---- Frame ---- */
typedef struct {
    uint32_t length;
    uint8_t  type;
    uint8_t  flags;
    uint32_t stream_id;
    uint8_t *payload;
} PicoH2Frame;

/* Send a frame. Returns 0 on success. */
int pico_h2_frame_send(PicoConn *c, uint8_t type, uint8_t flags,
                        uint32_t stream_id, const uint8_t *payload,
                        uint32_t length);

/* Receive a frame. Caller must free frame->payload. Returns 0 on success. */
int pico_h2_frame_recv(PicoConn *c, PicoH2Frame *frame);

/* ---- HPACK ---- */

/* Encode request headers into HPACK block.
 * out/out_len receive malloc'd buffer. Returns 0 on success. */
int pico_hpack_encode(const char *method, const char *scheme,
                       const char *authority, const char *path,
                       const PicoHttpHeader *extra, size_t extra_count,
                       uint8_t **out, size_t *out_len);

/* Decode HPACK block into response headers.
 * Populates status and PicoHttpResponse headers. Returns 0 on success. */
int pico_hpack_decode(const uint8_t *buf, size_t len,
                       int *status_out,
                       PicoHttpHeader **headers_out, size_t *count_out);

/* ---- HTTP/2 request ---- */

/* Perform a single HTTP/2 request on an already-connected+TLS'd PicoConn.
 * Called from pico_request_once when ALPN negotiated h2. */
int pico_h2_request(PicoConn *conn,
                     const char *method, const char *host,
                     const char *path, const char *content_type,
                     const void *body, size_t body_len,
                     const PicoHttpOptions *opts,
                     int is_head, PicoHttpResponse *out);

#endif /* PICO_HTTP_TLS && !PICO_HTTP_APENNINES */
#endif /* PICO_H2_H */

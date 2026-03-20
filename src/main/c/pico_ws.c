/*
 * pico_ws.c — Minimal WebSocket client (native RFC 6455 implementation)
 *
 * Uses the shared PicoConn transport layer (raw sockets + optional
 * mbedTLS) from pico_internal.h. Implements WebSocket upgrade handshake
 * and frame encoding/decoding per RFC 6455.
 *
 * License: MIT
 * Copyright (c) 2026 The now contributors
 */

#ifndef PICO_HTTP_BUILDING
  #define PICO_HTTP_BUILDING
#endif

#include "pico_ws.h"
#include "pico_internal.h"
#include "pico_http.h"  /* for PICO_ERR codes used in pico_connect */
#include "apennines/t2/compress/compress.h"  /* permessage-deflate */
typedef buf apn_buf;  /* avoid clash with pico_ws_recv 'buf' parameter */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PICO_WS_VERSION_STR  "0.1.0"
#define PICO_WS_DEFAULT_CONNECT_TIMEOUT  5000
#define PICO_WS_DEFAULT_RECV_TIMEOUT     30000
#define PICO_WS_MAX_FRAME_HEADER  14  /* 2 + 8 + 4 (mask) */

/* ---- Error strings ---- */

PICO_WS_API const char *pico_ws_strerror(int err) {
    switch (err) {
        case PICO_WS_OK:            return "success";
        case PICO_WS_ERR_INVALID:   return "invalid arguments";
        case PICO_WS_ERR_URL:       return "malformed URL";
        case PICO_WS_ERR_CONNECT:   return "connection failed";
        case PICO_WS_ERR_TLS:       return "TLS error";
        case PICO_WS_ERR_HANDSHAKE: return "WebSocket handshake failed";
        case PICO_WS_ERR_SEND:      return "send failed";
        case PICO_WS_ERR_RECV:      return "receive failed or timeout";
        case PICO_WS_ERR_CLOSED:    return "connection closed";
        case PICO_WS_ERR_ALLOC:     return "memory allocation failed";
        case PICO_WS_ERR_NOTSUP:    return "WebSocket support not compiled in";
        default:                    return "unknown error";
    }
}

PICO_WS_API const char *pico_ws_version(void) {
    return PICO_WS_VERSION_STR;
}

/* ---- Base64 encoder (minimal, for Sec-WebSocket-Key) ---- */

static const char b64_table[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static void pico_base64_encode(const unsigned char *in, size_t len,
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

/* ---- Random bytes ---- */

static void pico_random_bytes(unsigned char *buf, size_t len) {
#ifdef _WIN32
    /* Use RtlGenRandom (available since XP) */
    extern unsigned char __stdcall SystemFunction036(void *, unsigned long);
    SystemFunction036(buf, (unsigned long)len);
#else
    FILE *f = fopen("/dev/urandom", "rb");
    if (f) {
        fread(buf, 1, len, f);
        fclose(f);
    } else {
        /* Fallback — not cryptographically secure but works */
        for (size_t i = 0; i < len; i++)
            buf[i] = (unsigned char)(rand() & 0xFF);
    }
#endif
}

/* ---- WebSocket connection state ---- */

struct PicoWs {
    PicoConn conn;
    int      closed;
    int      recv_timeout_ms;
    int      deflate;  /* 1 if permessage-deflate negotiated */
};

/* ---- WebSocket upgrade handshake ---- */

static int pico_ws_do_handshake(PicoWs *ws, const char *host, int port,
                                 const char *path, const char *protocol,
                                 int want_deflate) {
    /* Generate 16-byte random key and base64 encode */
    unsigned char key_raw[16];
    char key_b64[32];
    pico_random_bytes(key_raw, 16);
    pico_base64_encode(key_raw, 16, key_b64, sizeof(key_b64));

    /* Build upgrade request */
    char req[1024];
    int n;
    const char *ext_hdr = want_deflate
        ? "Sec-WebSocket-Extensions: permessage-deflate\r\n" : "";
    if (protocol) {
        n = snprintf(req, sizeof(req),
            "GET %s HTTP/1.1\r\n"
            "Host: %s:%d\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            "Sec-WebSocket-Key: %s\r\n"
            "Sec-WebSocket-Version: 13\r\n"
            "Sec-WebSocket-Protocol: %s\r\n"
            "%s"
            "\r\n",
            path, host, port, key_b64, protocol, ext_hdr);
    } else {
        n = snprintf(req, sizeof(req),
            "GET %s HTTP/1.1\r\n"
            "Host: %s:%d\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            "Sec-WebSocket-Key: %s\r\n"
            "Sec-WebSocket-Version: 13\r\n"
            "%s"
            "\r\n",
            path, host, port, key_b64, ext_hdr);
    }

    if (pico_conn_send(&ws->conn, req, (size_t)n) != 0)
        return PICO_WS_ERR_SEND;

    /* Read response until we see \r\n\r\n */
    char resp[4096];
    size_t total = 0;
    while (total < sizeof(resp) - 1) {
        int r = pico_conn_recv(&ws->conn, resp + total, (int)(sizeof(resp) - 1 - total));
        if (r <= 0) return PICO_WS_ERR_RECV;
        total += (size_t)r;
        resp[total] = '\0';
        if (strstr(resp, "\r\n\r\n")) break;
    }

    /* Check for "HTTP/1.1 101" */
    if (strncmp(resp, "HTTP/1.1 101", 12) != 0 &&
        strncmp(resp, "HTTP/1.0 101", 12) != 0)
        return PICO_WS_ERR_HANDSHAKE;

    /* We skip Sec-WebSocket-Accept validation — not security-critical
     * for our use case (localhost cookbook, trusted servers). */

    /* Check if server accepted permessage-deflate */
    if (want_deflate && strstr(resp, "permessage-deflate"))
        ws->deflate = 1;

    return PICO_WS_OK;
}

/* ---- Public API ---- */

PICO_WS_API PicoWs *pico_ws_connect(const char *url,
                                     const PicoWsOptions *opts,
                                     int *err_out) {
    if (!url) {
        if (err_out) *err_out = PICO_WS_ERR_INVALID;
        return NULL;
    }

    /* Parse URL scheme */
    int use_tls = 0;
    const char *p = url;
    if (strncmp(p, "wss://", 6) == 0) {
        use_tls = 1;
        p += 6;
    } else if (strncmp(p, "ws://", 5) == 0) {
        p += 5;
    } else {
        if (err_out) *err_out = PICO_WS_ERR_URL;
        return NULL;
    }

#ifndef PICO_HTTP_TLS
    if (use_tls) {
        if (err_out) *err_out = PICO_WS_ERR_TLS;
        return NULL;
    }
#endif

    /* Extract host, port, path */
    const char *slash = strchr(p, '/');
    const char *host_end = slash ? slash : p + strlen(p);
    const char *colon = (const char *)memchr(p, ':', (size_t)(host_end - p));

    char host[256] = {0};
    int port = use_tls ? 443 : 80;
    const char *path = slash ? slash : "/";

    if (colon) {
        size_t hlen = (size_t)(colon - p);
        if (hlen >= sizeof(host)) hlen = sizeof(host) - 1;
        memcpy(host, p, hlen);
        port = atoi(colon + 1);
    } else {
        size_t hlen = (size_t)(host_end - p);
        if (hlen >= sizeof(host)) hlen = sizeof(host) - 1;
        memcpy(host, p, hlen);
    }

    /* Timeouts */
    int conn_timeout = PICO_WS_DEFAULT_CONNECT_TIMEOUT;
    int recv_timeout = PICO_WS_DEFAULT_RECV_TIMEOUT;
    const char *protocol = NULL;
    if (opts) {
        if (opts->connect_timeout_ms > 0) conn_timeout = opts->connect_timeout_ms;
        if (opts->recv_timeout_ms > 0) recv_timeout = opts->recv_timeout_ms;
        protocol = opts->protocol;
    }

    /* Allocate */
    PicoWs *ws = (PicoWs *)calloc(1, sizeof(PicoWs));
    if (!ws) {
        if (err_out) *err_out = PICO_WS_ERR_ALLOC;
        return NULL;
    }
    ws->recv_timeout_ms = recv_timeout;
    pico_conn_init(&ws->conn);

    if (pico_wsa_init() != 0) {
        free(ws);
        if (err_out) *err_out = PICO_WS_ERR_CONNECT;
        return NULL;
    }

    /* TCP connect */
    int conn_err = 0;
    ws->conn.sock = pico_connect(host, port, conn_timeout, &conn_err);
    if (ws->conn.sock == PICO_INVALID_SOCKET) {
        free(ws);
        if (err_out) *err_out = (conn_err == PICO_ERR_DNS)
                                 ? PICO_WS_ERR_CONNECT : PICO_WS_ERR_CONNECT;
        return NULL;
    }
    pico_set_timeout(ws->conn.sock, recv_timeout);

    /* TLS handshake if wss:// */
#ifdef PICO_HTTP_TLS
    if (use_tls) {
        int noverify = (opts && opts->tls_noverify);
        ws->conn.tls_verify = !noverify;
        if (ws->conn.tls_verify) {
            if (opts && opts->ca_data && opts->ca_data_len > 0)
                pico_load_ca_data(&ws->conn, opts->ca_data, opts->ca_data_len);
            else if (opts && opts->ca_file)
                pico_load_ca_file(&ws->conn, opts->ca_file);
            else
                pico_load_system_ca(&ws->conn);
        }
        int tls_rc = pico_tls_handshake(&ws->conn, host);
        if (tls_rc != 0) {
            pico_conn_close(&ws->conn);
            free(ws);
            if (err_out) *err_out = PICO_WS_ERR_TLS;
            return NULL;
        }
    }
#endif

    /* WebSocket upgrade handshake */
    int want_deflate = opts ? opts->permessage_deflate : 0;
    int hs_rc = pico_ws_do_handshake(ws, host, port, path, protocol, want_deflate);
    if (hs_rc != PICO_WS_OK) {
        pico_conn_close(&ws->conn);
        free(ws);
        if (err_out) *err_out = hs_rc;
        return NULL;
    }

    if (err_out) *err_out = PICO_WS_OK;
    return ws;
}

PICO_WS_API int pico_ws_send(PicoWs *ws, const void *data, size_t len,
                              int is_binary) {
    if (!ws || !data) return PICO_WS_ERR_INVALID;
    if (ws->closed) return PICO_WS_ERR_CLOSED;

    /* Compress payload if permessage-deflate active */
    apn_buf deflated_buf;
    const void *send_data = data;
    size_t send_len = len;
    int compressed = 0;

    if (ws->deflate && len > 0) {
        buf_create(&deflated_buf, len);
        if (deflate_compress(&deflated_buf, (u8 *)data, (u64)len,
                              COMPRESS_LEVEL_FAST) == 0 && deflated_buf.len > 4) {
            /* Remove trailing 0x00 0x00 0xFF 0xFF (per RFC 7692 section 7.2.1) */
            if (deflated_buf.data[deflated_buf.len - 4] == 0x00 &&
                deflated_buf.data[deflated_buf.len - 3] == 0x00 &&
                deflated_buf.data[deflated_buf.len - 2] == 0xFF &&
                deflated_buf.data[deflated_buf.len - 1] == 0xFF) {
                deflated_buf.len -= 4;
            }
            send_data = deflated_buf.data;
            send_len = (size_t)deflated_buf.len;
            compressed = 1;
        } else {
            buf_destroy(&deflated_buf);
        }
    }

    /* Build frame header */
    unsigned char header[PICO_WS_MAX_FRAME_HEADER];
    size_t hlen = 0;

    /* Byte 0: FIN + RSV1 (if compressed) + opcode */
    unsigned char byte0 = (unsigned char)(0x80 | (is_binary ? 0x02 : 0x01));
    if (compressed) byte0 |= 0x40;  /* RSV1 = permessage-deflate */
    header[hlen++] = byte0;

    /* Byte 1+: MASK bit set (client must mask) + payload length */
    if (send_len <= 125) {
        header[hlen++] = (unsigned char)(0x80 | send_len);
    } else if (send_len <= 65535) {
        header[hlen++] = (unsigned char)(0x80 | 126);
        header[hlen++] = (unsigned char)((send_len >> 8) & 0xFF);
        header[hlen++] = (unsigned char)(send_len & 0xFF);
    } else {
        header[hlen++] = (unsigned char)(0x80 | 127);
        for (int i = 7; i >= 0; i--)
            header[hlen++] = (unsigned char)((send_len >> (i * 8)) & 0xFF);
    }

    /* Masking key */
    unsigned char mask[4];
    pico_random_bytes(mask, 4);
    memcpy(header + hlen, mask, 4);
    hlen += 4;

    /* Send header */
    if (pico_conn_send(&ws->conn, (const char *)header, hlen) != 0) {
        if (compressed) buf_destroy(&deflated_buf);
        return PICO_WS_ERR_SEND;
    }

    /* Send masked payload */
    const unsigned char *src = (const unsigned char *)send_data;
    size_t chunk = 4096;
    unsigned char mbuf[4096];
    size_t sent = 0;
    while (sent < send_len) {
        size_t n = (send_len - sent < chunk) ? (send_len - sent) : chunk;
        for (size_t i = 0; i < n; i++)
            mbuf[i] = src[sent + i] ^ mask[(sent + i) & 3];
        if (pico_conn_send(&ws->conn, (const char *)mbuf, n) != 0) {
            if (compressed) buf_destroy(&deflated_buf);
            return PICO_WS_ERR_SEND;
        }
        sent += n;
    }

    if (compressed) buf_destroy(&deflated_buf);
    return PICO_WS_OK;
}

PICO_WS_API int pico_ws_recv(PicoWs *ws, void *buf, size_t buflen,
                              int timeout_ms, int *is_binary_out) {
    if (!ws || !buf) return PICO_WS_ERR_INVALID;
    if (ws->closed) return PICO_WS_ERR_CLOSED;

    (void)timeout_ms;  /* timeout handled by socket SO_RCVTIMEO */

    /* Read frame header (at least 2 bytes) */
    unsigned char hdr[2];
    int r = pico_conn_recv(&ws->conn, (char *)hdr, 2);
    if (r <= 0) { ws->closed = 1; return PICO_WS_ERR_CLOSED; }
    if (r < 2) {
        /* Read second byte */
        int r2 = pico_conn_recv(&ws->conn, (char *)&hdr[1], 1);
        if (r2 <= 0) { ws->closed = 1; return PICO_WS_ERR_CLOSED; }
    }

    int opcode = hdr[0] & 0x0F;
    int rsv1   = (hdr[0] >> 6) & 1;  /* permessage-deflate compressed */
    int masked = (hdr[1] >> 7) & 1;
    size_t payload_len = hdr[1] & 0x7F;

    /* Extended payload length */
    if (payload_len == 126) {
        unsigned char ext[2];
        r = pico_conn_recv(&ws->conn, (char *)ext, 2);
        if (r < 2) { ws->closed = 1; return PICO_WS_ERR_RECV; }
        payload_len = ((size_t)ext[0] << 8) | ext[1];
    } else if (payload_len == 127) {
        unsigned char ext[8];
        r = pico_conn_recv(&ws->conn, (char *)ext, 8);
        if (r < 8) { ws->closed = 1; return PICO_WS_ERR_RECV; }
        payload_len = 0;
        for (int i = 0; i < 8; i++)
            payload_len = (payload_len << 8) | ext[i];
    }

    /* Masking key (server frames usually unmasked) */
    unsigned char mask[4] = {0};
    if (masked) {
        r = pico_conn_recv(&ws->conn, (char *)mask, 4);
        if (r < 4) { ws->closed = 1; return PICO_WS_ERR_RECV; }
    }

    /* Handle control frames */
    if (opcode == 0x08) {
        /* Close frame */
        ws->closed = 1;
        /* Send close frame back */
        unsigned char close_frame[6];
        close_frame[0] = 0x88;  /* FIN + close */
        close_frame[1] = 0x80;  /* masked, 0 length */
        pico_random_bytes(close_frame + 2, 4);
        pico_conn_send(&ws->conn, (const char *)close_frame, 6);
        return PICO_WS_ERR_CLOSED;
    }

    if (opcode == 0x09) {
        /* Ping — read payload and send pong */
        size_t plen = (payload_len > 125) ? 125 : payload_len;
        unsigned char ping_data[125];
        size_t got = 0;
        while (got < plen) {
            r = pico_conn_recv(&ws->conn, (char *)ping_data + got,
                               (int)(plen - got));
            if (r <= 0) break;
            got += (size_t)r;
        }
        /* Unmask */
        if (masked) {
            for (size_t i = 0; i < got; i++)
                ping_data[i] ^= mask[i & 3];
        }
        /* Send pong */
        unsigned char pong_hdr[6 + 125];
        pong_hdr[0] = 0x8A;  /* FIN + pong */
        pong_hdr[1] = (unsigned char)(0x80 | got);
        unsigned char pmask[4];
        pico_random_bytes(pmask, 4);
        memcpy(pong_hdr + 2, pmask, 4);
        for (size_t i = 0; i < got; i++)
            pong_hdr[6 + i] = ping_data[i] ^ pmask[i & 3];
        pico_conn_send(&ws->conn, (const char *)pong_hdr, 6 + got);
        /* Recurse to get the next data frame */
        return pico_ws_recv(ws, buf, buflen, timeout_ms, is_binary_out);
    }

    if (opcode == 0x0A) {
        /* Pong — skip payload and recurse */
        char skip[256];
        size_t remaining = payload_len;
        while (remaining > 0) {
            size_t chunk = remaining > sizeof(skip) ? sizeof(skip) : remaining;
            r = pico_conn_recv(&ws->conn, skip, (int)chunk);
            if (r <= 0) break;
            remaining -= (size_t)r;
        }
        return pico_ws_recv(ws, buf, buflen, timeout_ms, is_binary_out);
    }

    /* Data frame (text=0x01, binary=0x02, continuation=0x00) */
    if (is_binary_out)
        *is_binary_out = (opcode == 0x02) ? 1 : 0;

    /* Read payload (capped at buflen) */
    size_t to_read = (payload_len > buflen) ? buflen : payload_len;
    size_t total = 0;
    while (total < to_read) {
        r = pico_conn_recv(&ws->conn, (char *)buf + total,
                           (int)(to_read - total));
        if (r <= 0) { ws->closed = 1; return PICO_WS_ERR_RECV; }
        total += (size_t)r;
    }

    /* Unmask if needed */
    if (masked) {
        unsigned char *p = (unsigned char *)buf;
        for (size_t i = 0; i < total; i++)
            p[i] ^= mask[i & 3];
    }

    /* Discard any excess payload beyond buflen */
    if (payload_len > buflen) {
        char discard[256];
        size_t remaining = payload_len - buflen;
        while (remaining > 0) {
            size_t chunk = remaining > sizeof(discard) ? sizeof(discard) : remaining;
            r = pico_conn_recv(&ws->conn, discard, (int)chunk);
            if (r <= 0) break;
            remaining -= (size_t)r;
        }
    }

    /* Inflate if RSV1 set (permessage-deflate) */
    if (rsv1 && ws->deflate && total > 0) {
        /* Append 0x00 0x00 0xFF 0xFF trailer per RFC 7692 */
        unsigned char *compressed_data = (unsigned char *)malloc(total + 4);
        if (compressed_data) {
            memcpy(compressed_data, buf, total);
            compressed_data[total]     = 0x00;
            compressed_data[total + 1] = 0x00;
            compressed_data[total + 2] = 0xFF;
            compressed_data[total + 3] = 0xFF;

            apn_buf inf_buf;
            buf_create(&inf_buf, total * 4);
            if (deflate_decompress(&inf_buf, compressed_data, (u64)(total + 4)) == 0) {
                size_t copy_len = inf_buf.len < buflen ? (size_t)inf_buf.len : buflen;
                memcpy(buf, inf_buf.data, copy_len);
                total = copy_len;
            }
            buf_destroy(&inf_buf);
            free(compressed_data);
        }
    }

    return (int)total;
}

PICO_WS_API void pico_ws_close(PicoWs *ws) {
    if (!ws) return;

    if (!ws->closed && ws->conn.sock != PICO_INVALID_SOCKET) {
        /* Send close frame */
        unsigned char close_frame[6];
        close_frame[0] = 0x88;  /* FIN + close */
        close_frame[1] = 0x80;  /* masked, 0 length */
        pico_random_bytes(close_frame + 2, 4);
        pico_conn_send(&ws->conn, (const char *)close_frame, 6);
    }

    pico_conn_close(&ws->conn);
    free(ws);
}

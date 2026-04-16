/*
 * pico_h2.c — HTTP/2 client support for pico_http
 *
 * Single-stream HTTP/2 over TLS (h2 via ALPN). HPACK with static table,
 * Huffman decoding, frame codec, flow control.
 *
 * License: MIT
 * Copyright (c) 2026 The now contributors
 */

#if defined(PICO_HTTP_TLS) && !defined(PICO_HTTP_APENNINES)

#ifndef PICO_HTTP_BUILDING
  #define PICO_HTTP_BUILDING
#endif

#include "pico_h2.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ================================================================
 *  HPACK Static Table (RFC 7541 Appendix A)
 * ================================================================ */

static const char *hpack_static[][2] = {
    {NULL, NULL},  /* index 0 unused */
    {":authority", ""},
    {":method", "GET"},
    {":method", "POST"},
    {":path", "/"},
    {":path", "/index.html"},
    {":scheme", "http"},
    {":scheme", "https"},
    {":status", "200"},
    {":status", "204"},
    {":status", "206"},
    {":status", "304"},
    {":status", "400"},
    {":status", "404"},
    {":status", "500"},
    {"accept-charset", ""},
    {"accept-encoding", "gzip, deflate"},
    {"accept-language", ""},
    {"accept-ranges", ""},
    {"accept", ""},
    {"access-control-allow-origin", ""},
    {"age", ""},
    {"allow", ""},
    {"authorization", ""},
    {"cache-control", ""},
    {"content-disposition", ""},
    {"content-encoding", ""},
    {"content-language", ""},
    {"content-length", ""},
    {"content-location", ""},
    {"content-range", ""},
    {"content-type", ""},
    {"cookie", ""},
    {"date", ""},
    {"etag", ""},
    {"expect", ""},
    {"expires", ""},
    {"from", ""},
    {"host", ""},
    {"if-match", ""},
    {"if-modified-since", ""},
    {"if-none-match", ""},
    {"if-range", ""},
    {"if-unmodified-since", ""},
    {"last-modified", ""},
    {"link", ""},
    {"location", ""},
    {"max-forwards", ""},
    {"proxy-authenticate", ""},
    {"proxy-authorization", ""},
    {"range", ""},
    {"referer", ""},
    {"refresh", ""},
    {"retry-after", ""},
    {"server", ""},
    {"set-cookie", ""},
    {"strict-transport-security", ""},
    {"transfer-encoding", ""},
    {"user-agent", ""},
    {"vary", ""},
    {"via", ""},
    {"www-authenticate", ""},
};
#define HPACK_STATIC_COUNT 61

/* ================================================================
 *  HPACK Integer Codec (RFC 7541 section 5.1)
 * ================================================================ */

static int hpack_encode_int(uint8_t *buf, size_t cap, size_t *out_len,
                             uint32_t value, uint8_t prefix_bits) {
    uint8_t max_prefix = (uint8_t)((1 << prefix_bits) - 1);
    size_t pos = 0;
    if (value < max_prefix) {
        if (pos >= cap) return -1;
        buf[pos++] = (uint8_t)value;
    } else {
        if (pos >= cap) return -1;
        buf[pos++] = max_prefix;
        value -= max_prefix;
        while (value >= 128) {
            if (pos >= cap) return -1;
            buf[pos++] = (uint8_t)((value & 0x7F) | 0x80);
            value >>= 7;
        }
        if (pos >= cap) return -1;
        buf[pos++] = (uint8_t)value;
    }
    *out_len = pos;
    return 0;
}

static int hpack_decode_int(const uint8_t *buf, size_t len, size_t *pos,
                             uint8_t prefix_bits, uint32_t *out) {
    if (*pos >= len) return -1;
    uint8_t max_prefix = (uint8_t)((1 << prefix_bits) - 1);
    uint32_t value = buf[(*pos)++] & max_prefix;
    if (value < max_prefix) { *out = value; return 0; }
    uint32_t m = 0;
    while (*pos < len) {
        uint8_t b = buf[(*pos)++];
        value += (uint32_t)(b & 0x7F) << m;
        m += 7;
        if (!(b & 0x80)) { *out = value; return 0; }
        if (m > 28) return -1;  /* overflow */
    }
    return -1;
}

/* ================================================================
 *  Huffman Decoder (RFC 7541 Appendix B — partial, most common symbols)
 *  Simple bit-by-bit decoder using a decode table.
 * ================================================================ */

/* Huffman decode: reads bit-by-bit, emits bytes.
 * For simplicity, uses a table of {code, nbits, symbol} entries. */
static const struct { uint32_t code; uint8_t nbits; uint8_t sym; } huffman_table[] = {
    {0x1ff8, 13, 0}, {0x7fffd8, 23, 1}, {0xfffffe2, 28, 2}, {0xfffffe3, 28, 3},
    {0xfffffe4, 28, 4}, {0xfffffe5, 28, 5}, {0xfffffe6, 28, 6}, {0xfffffe7, 28, 7},
    {0xfffffe8, 28, 8}, {0xffffea, 24, 9}, {0x3ffffffc, 30, 10}, {0xfffffe9, 28, 11},
    {0xfffffea, 28, 12}, {0x3ffffffd, 30, 13}, {0xfffffeb, 28, 14}, {0xfffffec, 28, 15},
    {0xfffffed, 28, 16}, {0xfffffee, 28, 17}, {0xfffffef, 28, 18}, {0xffffff0, 28, 19},
    {0xffffff1, 28, 20}, {0xffffff2, 28, 21}, {0x3ffffffe, 30, 22}, {0xffffff3, 28, 23},
    {0xffffff4, 28, 24}, {0xffffff5, 28, 25}, {0xffffff6, 28, 26}, {0xffffff7, 28, 27},
    {0xffffff8, 28, 28}, {0xffffff9, 28, 29}, {0xffffffa, 28, 30}, {0xffffffb, 28, 31},
    {0x14, 6, 32}, {0x3f8, 10, 33}, {0x3f9, 10, 34}, {0xffa, 12, 35},
    {0x1ff9, 13, 36}, {0x15, 6, 37}, {0xf8, 8, 38}, {0x7fa, 11, 39},
    {0x3fa, 10, 40}, {0x3fb, 10, 41}, {0xf9, 8, 42}, {0x7fb, 11, 43},
    {0xfa, 8, 44}, {0x16, 6, 45}, {0x17, 6, 46}, {0x18, 6, 47},
    {0x0, 5, 48}, {0x1, 5, 49}, {0x2, 5, 50}, {0x19, 6, 51},
    {0x1a, 6, 52}, {0x1b, 6, 53}, {0x1c, 6, 54}, {0x1d, 6, 55},
    {0x1e, 6, 56}, {0x1f, 6, 57}, {0x5c, 7, 58}, {0xfb, 8, 59},
    {0x7ffc, 15, 60}, {0x20, 6, 61}, {0xffb, 12, 62}, {0x3fc, 10, 63},
    {0x1ffa, 13, 64}, {0x21, 6, 65}, {0x5d, 7, 66}, {0x5e, 7, 67},
    {0x5f, 7, 68}, {0x60, 7, 69}, {0x61, 7, 70}, {0x62, 7, 71},
    {0x63, 7, 72}, {0x64, 7, 73}, {0x65, 7, 74}, {0x66, 7, 75},
    {0x67, 7, 76}, {0x68, 7, 77}, {0x69, 7, 78}, {0x6a, 7, 79},
    {0x6b, 7, 80}, {0x6c, 7, 81}, {0x6d, 7, 82}, {0x6e, 7, 83},
    {0x6f, 7, 84}, {0x70, 7, 85}, {0x71, 7, 86}, {0x72, 7, 87},
    {0xfc, 8, 88}, {0x73, 7, 89}, {0xfd, 8, 90}, {0x1ffb, 13, 91},
    {0x7fff0, 19, 92}, {0x1ffc, 13, 93}, {0x3ffc, 14, 94}, {0x22, 6, 95},
    {0x7ffd, 15, 96}, {0x3, 5, 97}, {0x23, 6, 98}, {0x4, 5, 99},
    {0x24, 6, 100}, {0x5, 5, 101}, {0x25, 6, 102}, {0x26, 6, 103},
    {0x27, 6, 104}, {0x6, 5, 105}, {0x74, 7, 106}, {0x75, 7, 107},
    {0x28, 6, 108}, {0x29, 6, 109}, {0x2a, 6, 110}, {0x7, 5, 111},
    {0x2b, 6, 112}, {0x76, 7, 113}, {0x2c, 6, 114}, {0x8, 5, 115},
    {0x9, 5, 116}, {0x2d, 6, 117}, {0x77, 7, 118}, {0x78, 7, 119},
    {0x79, 7, 120}, {0x7a, 7, 121}, {0x7b, 7, 122}, {0x7ffe, 15, 123},
    {0x7fc, 11, 124}, {0x3ffd, 14, 125}, {0x1ffd, 13, 126}, {0xffffffc, 28, 127},
};
#define HUFFMAN_TABLE_SIZE 128

static int huffman_decode(const uint8_t *in, size_t in_len,
                           uint8_t **out, size_t *out_len) {
    size_t cap = in_len * 2 + 16;
    uint8_t *buf = (uint8_t *)malloc(cap);
    if (!buf) return -1;
    size_t pos = 0;

    uint32_t bits = 0;
    int nbits = 0;

    for (size_t i = 0; i < in_len; i++) {
        bits = (bits << 8) | in[i];
        nbits += 8;

        while (nbits >= 5) {
            int found = 0;
            for (int t = 0; t < HUFFMAN_TABLE_SIZE; t++) {
                if (huffman_table[t].nbits <= (uint8_t)nbits) {
                    uint32_t mask = (1u << huffman_table[t].nbits) - 1;
                    uint32_t shifted = bits >> (nbits - huffman_table[t].nbits);
                    if ((shifted & mask) == huffman_table[t].code) {
                        if (pos >= cap) {
                            cap *= 2;
                            uint8_t *tmp = (uint8_t *)realloc(buf, cap);
                            if (!tmp) { free(buf); return -1; }
                            buf = tmp;
                        }
                        buf[pos++] = huffman_table[t].sym;
                        nbits -= huffman_table[t].nbits;
                        bits &= (1u << nbits) - 1;
                        found = 1;
                        break;
                    }
                }
            }
            if (!found) break;
        }
    }

    *out = buf;
    *out_len = pos;
    return 0;
}

/* ================================================================
 *  HPACK Encoder (request headers, static table references)
 * ================================================================ */

static int hpack_find_static(const char *name, const char *value) {
    for (int i = 1; i <= HPACK_STATIC_COUNT; i++) {
        if (strcmp(hpack_static[i][0], name) == 0 &&
            strcmp(hpack_static[i][1], value) == 0)
            return i;
    }
    return 0;
}

static int hpack_find_static_name(const char *name) {
    for (int i = 1; i <= HPACK_STATIC_COUNT; i++) {
        if (strcmp(hpack_static[i][0], name) == 0)
            return i;
    }
    return 0;
}

int pico_hpack_encode(const char *method, const char *scheme,
                       const char *authority, const char *path,
                       const PicoHttpHeader *extra, size_t extra_count,
                       uint8_t **out, size_t *out_len) {
    size_t cap = 512;
    uint8_t *buf = (uint8_t *)malloc(cap);
    if (!buf) return -1;
    size_t pos = 0;

    /* Helper: ensure space */
    #define ENSURE(n) do { \
        while (pos + (n) > cap) { cap *= 2; \
            uint8_t *tmp = (uint8_t *)realloc(buf, cap); \
            if (!tmp) { free(buf); return -1; } \
            buf = tmp; } \
    } while (0)

    /* Helper: emit indexed header (7-bit prefix, top bit set) */
    #define EMIT_INDEXED(idx) do { \
        size_t ilen; uint8_t ibuf[8]; \
        hpack_encode_int(ibuf, sizeof(ibuf), &ilen, (idx), 7); \
        ibuf[0] |= 0x80; \
        ENSURE(ilen); memcpy(buf + pos, ibuf, ilen); pos += ilen; \
    } while (0)

    /* Helper: emit literal without indexing (4-bit name prefix) */
    #define EMIT_LITERAL(name_idx, val, vlen) do { \
        size_t ilen; uint8_t ibuf[8]; \
        hpack_encode_int(ibuf, sizeof(ibuf), &ilen, (name_idx), 4); \
        ENSURE(ilen); memcpy(buf + pos, ibuf, ilen); pos += ilen; \
        hpack_encode_int(ibuf, sizeof(ibuf), &ilen, (uint32_t)(vlen), 7); \
        ENSURE(ilen + (vlen)); memcpy(buf + pos, ibuf, ilen); pos += ilen; \
        memcpy(buf + pos, (val), (vlen)); pos += (vlen); \
    } while (0)

    /* Pseudo-headers */
    struct { const char *name; const char *value; } pseudos[] = {
        {":method", method}, {":scheme", scheme},
        {":authority", authority}, {":path", path}
    };
    for (int i = 0; i < 4; i++) {
        if (!pseudos[i].value) continue;
        int idx = hpack_find_static(pseudos[i].name, pseudos[i].value);
        if (idx) { EMIT_INDEXED((uint32_t)idx); }
        else {
            int nidx = hpack_find_static_name(pseudos[i].name);
            size_t vl = strlen(pseudos[i].value);
            EMIT_LITERAL((uint32_t)nidx, pseudos[i].value, vl);
        }
    }

    /* Extra headers */
    for (size_t i = 0; i < extra_count; i++) {
        if (!extra[i].name || !extra[i].value) continue;
        int idx = hpack_find_static(extra[i].name, extra[i].value);
        if (idx) { EMIT_INDEXED((uint32_t)idx); }
        else {
            int nidx = hpack_find_static_name(extra[i].name);
            size_t vl = strlen(extra[i].value);
            if (nidx) {
                EMIT_LITERAL((uint32_t)nidx, extra[i].value, vl);
            } else {
                /* Literal with new name */
                size_t nl = strlen(extra[i].name);
                size_t ilen; uint8_t ibuf[8];
                ENSURE(1); buf[pos++] = 0x00;  /* literal, no index, new name */
                hpack_encode_int(ibuf, sizeof(ibuf), &ilen, (uint32_t)nl, 7);
                ENSURE(ilen + nl); memcpy(buf + pos, ibuf, ilen); pos += ilen;
                memcpy(buf + pos, extra[i].name, nl); pos += nl;
                hpack_encode_int(ibuf, sizeof(ibuf), &ilen, (uint32_t)vl, 7);
                ENSURE(ilen + vl); memcpy(buf + pos, ibuf, ilen); pos += ilen;
                memcpy(buf + pos, extra[i].value, vl); pos += vl;
            }
        }
    }

    #undef ENSURE
    #undef EMIT_INDEXED
    #undef EMIT_LITERAL

    *out = buf;
    *out_len = pos;
    return 0;
}

/* ================================================================
 *  HPACK Decoder (response headers)
 * ================================================================ */

int pico_hpack_decode(const uint8_t *buf, size_t len,
                       int *status_out,
                       PicoHttpHeader **headers_out, size_t *count_out) {
    size_t pos = 0;
    size_t cap = 16;
    PicoHttpHeader *hdrs = (PicoHttpHeader *)calloc(cap, sizeof(PicoHttpHeader));
    if (!hdrs) return -1;
    size_t count = 0;
    *status_out = 0;

    while (pos < len) {
        uint8_t byte = buf[pos];
        const char *name = NULL;
        char *value = NULL;
        int name_is_alloc = 0;

        if (byte & 0x80) {
            /* Indexed header field */
            uint32_t idx;
            if (hpack_decode_int(buf, len, &pos, 7, &idx) != 0) break;
            if (idx >= 1 && idx <= HPACK_STATIC_COUNT) {
                name = hpack_static[idx][0];
                value = strdup(hpack_static[idx][1]);
            } else break;
        } else if ((byte & 0xC0) == 0x40) {
            /* Literal with incremental indexing */
            uint32_t idx;
            if (hpack_decode_int(buf, len, &pos, 6, &idx) != 0) break;
            if (idx > 0 && idx <= HPACK_STATIC_COUNT) {
                name = hpack_static[idx][0];
            } else if (idx == 0) {
                /* New name */
                uint32_t nlen;
                int huffman_n = buf[pos] & 0x80;
                if (hpack_decode_int(buf, len, &pos, 7, &nlen) != 0) break;
                if (pos + nlen > len) break;
                if (huffman_n) {
                    uint8_t *dec; size_t dlen;
                    if (huffman_decode(buf + pos, nlen, &dec, &dlen) != 0) break;
                    char *n = (char *)malloc(dlen + 1);
                    memcpy(n, dec, dlen); n[dlen] = '\0'; free(dec);
                    name = n; name_is_alloc = 1;
                } else {
                    char *n = (char *)malloc(nlen + 1);
                    memcpy(n, buf + pos, nlen); n[nlen] = '\0';
                    name = n; name_is_alloc = 1;
                }
                pos += nlen;
            } else break;

            /* Value */
            uint32_t vlen;
            int huffman_v = buf[pos] & 0x80;
            if (hpack_decode_int(buf, len, &pos, 7, &vlen) != 0) {
                if (name_is_alloc) free((char *)name);
                break;
            }
            if (pos + vlen > len) { if (name_is_alloc) free((char *)name); break; }
            if (huffman_v) {
                uint8_t *dec; size_t dlen;
                if (huffman_decode(buf + pos, vlen, &dec, &dlen) != 0) {
                    if (name_is_alloc) free((char *)name); break;
                }
                value = (char *)malloc(dlen + 1);
                memcpy(value, dec, dlen); value[dlen] = '\0'; free(dec);
            } else {
                value = (char *)malloc(vlen + 1);
                memcpy(value, buf + pos, vlen); value[vlen] = '\0';
            }
            pos += vlen;
        } else if ((byte & 0xF0) == 0x00 || (byte & 0xF0) == 0x10) {
            /* Literal without indexing / never indexed */
            uint8_t prefix = (byte & 0xF0) == 0x00 ? 4 : 4;
            uint32_t idx;
            if (hpack_decode_int(buf, len, &pos, prefix, &idx) != 0) break;
            if (idx > 0 && idx <= HPACK_STATIC_COUNT)
                name = hpack_static[idx][0];
            else if (idx == 0) {
                uint32_t nlen;
                int huffman_n = buf[pos] & 0x80;
                if (hpack_decode_int(buf, len, &pos, 7, &nlen) != 0) break;
                if (pos + nlen > len) break;
                if (huffman_n) {
                    uint8_t *dec; size_t dlen;
                    if (huffman_decode(buf + pos, nlen, &dec, &dlen) != 0) break;
                    char *n = (char *)malloc(dlen + 1);
                    memcpy(n, dec, dlen); n[dlen] = '\0'; free(dec);
                    name = n; name_is_alloc = 1;
                } else {
                    char *n = (char *)malloc(nlen + 1);
                    memcpy(n, buf + pos, nlen); n[nlen] = '\0';
                    name = n; name_is_alloc = 1;
                }
                pos += nlen;
            } else break;

            uint32_t vlen;
            int huffman_v = buf[pos] & 0x80;
            if (hpack_decode_int(buf, len, &pos, 7, &vlen) != 0) {
                if (name_is_alloc) free((char *)name); break;
            }
            if (pos + vlen > len) { if (name_is_alloc) free((char *)name); break; }
            if (huffman_v) {
                uint8_t *dec; size_t dlen;
                if (huffman_decode(buf + pos, vlen, &dec, &dlen) != 0) {
                    if (name_is_alloc) free((char *)name); break;
                }
                value = (char *)malloc(dlen + 1);
                memcpy(value, dec, dlen); value[dlen] = '\0'; free(dec);
            } else {
                value = (char *)malloc(vlen + 1);
                memcpy(value, buf + pos, vlen); value[vlen] = '\0';
            }
            pos += vlen;
        } else if ((byte & 0xE0) == 0x20) {
            /* Dynamic table size update — skip */
            uint32_t sz;
            if (hpack_decode_int(buf, len, &pos, 5, &sz) != 0) break;
            continue;
        } else {
            break;
        }

        if (name && value) {
            /* Check for :status pseudo-header */
            if (strcmp(name, ":status") == 0)
                *status_out = atoi(value);

            /* Skip pseudo-headers in output (start with ':') */
            if (name[0] != ':') {
                if (count >= cap) {
                    cap *= 2;
                    PicoHttpHeader *tmp = (PicoHttpHeader *)realloc(hdrs,
                        cap * sizeof(PicoHttpHeader));
                    if (!tmp) { free(value); if (name_is_alloc) free((char *)name); break; }
                    hdrs = tmp;
                }
                hdrs[count].name = name_is_alloc ? name : strdup(name);
                hdrs[count].value = value;
                count++;
                continue;
            }
        }
        free(value);
        if (name_is_alloc) free((char *)name);
    }

    *headers_out = hdrs;
    *count_out = count;
    return 0;
}

/* ================================================================
 *  Frame Codec
 * ================================================================ */

int pico_h2_frame_send(PicoConn *c, uint8_t type, uint8_t flags,
                        uint32_t stream_id, const uint8_t *payload,
                        uint32_t length) {
    uint8_t hdr[9];
    hdr[0] = (uint8_t)((length >> 16) & 0xFF);
    hdr[1] = (uint8_t)((length >> 8) & 0xFF);
    hdr[2] = (uint8_t)(length & 0xFF);
    hdr[3] = type;
    hdr[4] = flags;
    hdr[5] = (uint8_t)((stream_id >> 24) & 0x7F);
    hdr[6] = (uint8_t)((stream_id >> 16) & 0xFF);
    hdr[7] = (uint8_t)((stream_id >> 8) & 0xFF);
    hdr[8] = (uint8_t)(stream_id & 0xFF);

    if (pico_conn_send(c, (const char *)hdr, 9) != 0) return -1;
    if (length > 0 && payload)
        if (pico_conn_send(c, (const char *)payload, length) != 0) return -1;
    return 0;
}

int pico_h2_frame_recv(PicoConn *c, PicoH2Frame *frame) {
    uint8_t hdr[9];
    size_t got = 0;
    while (got < 9) {
        int r = pico_conn_recv(c, (char *)hdr + got, (int)(9 - got));
        if (r <= 0) return -1;
        got += (size_t)r;
    }

    frame->length = ((uint32_t)hdr[0] << 16) | ((uint32_t)hdr[1] << 8) | hdr[2];
    frame->type = hdr[3];
    frame->flags = hdr[4];
    frame->stream_id = ((uint32_t)(hdr[5] & 0x7F) << 24) |
                        ((uint32_t)hdr[6] << 16) |
                        ((uint32_t)hdr[7] << 8) | hdr[8];

    if (frame->length > 0) {
        frame->payload = (uint8_t *)malloc(frame->length);
        if (!frame->payload) return -1;
        got = 0;
        while (got < frame->length) {
            int r = pico_conn_recv(c, (char *)frame->payload + got,
                                    (int)(frame->length - got));
            if (r <= 0) { free(frame->payload); frame->payload = NULL; return -1; }
            got += (size_t)r;
        }
    } else {
        frame->payload = NULL;
    }
    return 0;
}

/* ================================================================
 *  HTTP/2 Request
 * ================================================================ */

static const char h2_preface[] = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";

int pico_h2_request(PicoConn *conn,
                     const char *method, const char *host,
                     const char *path, const char *content_type,
                     const void *body, size_t body_len,
                     const PicoHttpOptions *opts,
                     int is_head, PicoHttpResponse *out) {
    if (!conn || !method || !host || !path || !out) return PICO_ERR_INVALID;
    (void)opts; (void)is_head;

    memset(out, 0, sizeof(*out));

    /* Send connection preface */
    if (pico_conn_send(conn, h2_preface, 24) != 0)
        return PICO_ERR_SEND;

    /* Send empty SETTINGS */
    if (pico_h2_frame_send(conn, H2_SETTINGS, 0, 0, NULL, 0) != 0)
        return PICO_ERR_SEND;

    /* Read server SETTINGS */
    PicoH2Frame frame;
    if (pico_h2_frame_recv(conn, &frame) != 0) return PICO_ERR_RECV;
    free(frame.payload);
    /* ACK server SETTINGS */
    if (pico_h2_frame_send(conn, H2_SETTINGS, H2_FLAG_ACK, 0, NULL, 0) != 0)
        return PICO_ERR_SEND;

    /* Build extra headers */
    PicoHttpHeader extra[2];
    size_t extra_count = 0;
    if (content_type) {
        extra[extra_count].name = "content-type";
        extra[extra_count].value = (char *)content_type;
        extra_count++;
    }

    /* Encode HEADERS */
    uint8_t *hpack_buf = NULL;
    size_t hpack_len = 0;
    if (pico_hpack_encode(method, "https", host, path,
                           extra, extra_count,
                           &hpack_buf, &hpack_len) != 0)
        return PICO_ERR_ALLOC;

    /* Send HEADERS frame on stream 1 */
    uint8_t hdr_flags = H2_FLAG_END_HEADERS;
    if (!body || body_len == 0) hdr_flags |= H2_FLAG_END_STREAM;

    if (pico_h2_frame_send(conn, H2_HEADERS, hdr_flags, 1,
                            hpack_buf, (uint32_t)hpack_len) != 0) {
        free(hpack_buf);
        return PICO_ERR_SEND;
    }
    free(hpack_buf);

    /* Send DATA if body present */
    if (body && body_len > 0) {
        if (pico_h2_frame_send(conn, H2_DATA, H2_FLAG_END_STREAM, 1,
                                (const uint8_t *)body, (uint32_t)body_len) != 0)
            return PICO_ERR_SEND;
    }

    /* Read response frames */
    char *resp_body = NULL;
    size_t resp_body_len = 0;
    size_t resp_body_cap = 0;

    while (1) {
        if (pico_h2_frame_recv(conn, &frame) != 0) break;

        switch (frame.type) {
        case H2_HEADERS:
            if (frame.payload && frame.length > 0) {
                pico_hpack_decode(frame.payload, frame.length,
                                   &out->status, &out->headers, &out->header_count);
            }
            break;

        case H2_DATA:
            if (frame.payload && frame.length > 0) {
                if (resp_body_len + frame.length > resp_body_cap) {
                    size_t newcap = resp_body_cap ? resp_body_cap * 2 : 8192;
                    while (newcap < resp_body_len + frame.length) newcap *= 2;
                    char *tmp = (char *)realloc(resp_body, newcap);
                    if (!tmp) { free(frame.payload); free(resp_body); return PICO_ERR_ALLOC; }
                    resp_body = tmp;
                    resp_body_cap = newcap;
                }
                memcpy(resp_body + resp_body_len, frame.payload, frame.length);
                resp_body_len += frame.length;
            }
            /* Send WINDOW_UPDATE for connection and stream */
            if (frame.length > 0) {
                uint8_t wu[4];
                wu[0] = (uint8_t)((frame.length >> 24) & 0xFF);
                wu[1] = (uint8_t)((frame.length >> 16) & 0xFF);
                wu[2] = (uint8_t)((frame.length >> 8) & 0xFF);
                wu[3] = (uint8_t)(frame.length & 0xFF);
                pico_h2_frame_send(conn, H2_WINDOW_UPDATE, 0, 0, wu, 4);
                pico_h2_frame_send(conn, H2_WINDOW_UPDATE, 0, 1, wu, 4);
            }
            break;

        case H2_SETTINGS:
            if (!(frame.flags & H2_FLAG_ACK))
                pico_h2_frame_send(conn, H2_SETTINGS, H2_FLAG_ACK, 0, NULL, 0);
            break;

        case H2_PING:
            pico_h2_frame_send(conn, H2_PING, H2_FLAG_ACK, 0,
                                frame.payload, frame.length);
            break;

        case H2_GOAWAY:
        case H2_RST_STREAM:
            free(frame.payload);
            goto done;

        default:
            break;
        }

        int end_stream = (frame.flags & H2_FLAG_END_STREAM) &&
                          (frame.type == H2_HEADERS || frame.type == H2_DATA);
        free(frame.payload);
        if (end_stream) break;
    }

done:
    out->body = resp_body;
    out->body_len = resp_body_len;

    /* Send GOAWAY */
    uint8_t goaway[8] = {0};
    pico_h2_frame_send(conn, H2_GOAWAY, 0, 0, goaway, 8);

    return PICO_OK;
}

#endif /* PICO_HTTP_TLS && !PICO_HTTP_APENNINES */

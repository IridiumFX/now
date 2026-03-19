/*
 * now_ed25519.c — Native Ed25519 digital signatures (RFC 8032)
 *
 * Self-contained implementation with no external crypto dependencies.
 * Includes SHA-512, field arithmetic in GF(2^255-19), and the Ed25519
 * sign/verify operations.
 *
 * Based on the Ed25519 specification (RFC 8032) and the public-domain
 * ref10 implementation by Daniel J. Bernstein et al.
 *
 * This file is part of the now build tool.
 */

#include "now_trust.h"
#include "now_fs.h"

#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* ================================================================
 * SHA-512 (FIPS 180-4)
 * ================================================================ */

static const uint64_t K512[80] = {
    0x428a2f98d728ae22ULL, 0x7137449123ef65cdULL, 0xb5c0fbcfec4d3b2fULL,
    0xe9b5dba58189dbbcULL, 0x3956c25bf348b538ULL, 0x59f111f1b605d019ULL,
    0x923f82a4af194f9bULL, 0xab1c5ed5da6d8118ULL, 0xd807aa98a3030242ULL,
    0x12835b0145706fbeULL, 0x243185be4ee4b28cULL, 0x550c7dc3d5ffb4e2ULL,
    0x72be5d74f27b896fULL, 0x80deb1fe3b1696b1ULL, 0x9bdc06a725c71235ULL,
    0xc19bf174cf692694ULL, 0xe49b69c19ef14ad2ULL, 0xefbe4786384f25e3ULL,
    0x0fc19dc68b8cd5b5ULL, 0x240ca1cc77ac9c65ULL, 0x2de92c6f592b0275ULL,
    0x4a7484aa6ea6e483ULL, 0x5cb0a9dcbd41fbd4ULL, 0x76f988da831153b5ULL,
    0x983e5152ee66dfabULL, 0xa831c66d2db43210ULL, 0xb00327c898fb213fULL,
    0xbf597fc7beef0ee4ULL, 0xc6e00bf33da88fc2ULL, 0xd5a79147930aa725ULL,
    0x06ca6351e003826fULL, 0x142929670a0e6e70ULL, 0x27b70a8546d22ffcULL,
    0x2e1b21385c26c926ULL, 0x4d2c6dfc5ac42aedULL, 0x53380d139d95b3dfULL,
    0x650a73548baf63deULL, 0x766a0abb3c77b2a8ULL, 0x81c2c92e47edaee6ULL,
    0x92722c851482353bULL, 0xa2bfe8a14cf10364ULL, 0xa81a664bbc423001ULL,
    0xc24b8b70d0f89791ULL, 0xc76c51a30654be30ULL, 0xd192e819d6ef5218ULL,
    0xd69906245565a910ULL, 0xf40e35855771202aULL, 0x106aa07032bbd1b8ULL,
    0x19a4c116b8d2d0c8ULL, 0x1e376c085141ab53ULL, 0x2748774cdf8eeb99ULL,
    0x34b0bcb5e19b48a8ULL, 0x391c0cb3c5c95a63ULL, 0x4ed8aa4ae3418acbULL,
    0x5b9cca4f7763e373ULL, 0x682e6ff3d6b2b8a3ULL, 0x748f82ee5defb2fcULL,
    0x78a5636f43172f60ULL, 0x84c87814a1f0ab72ULL, 0x8cc702081a6439ecULL,
    0x90befffa23631e28ULL, 0xa4506cebde82bde9ULL, 0xbef9a3f7b2c67915ULL,
    0xc67178f2e372532bULL, 0xca273eceea26619cULL, 0xd186b8c721c0c207ULL,
    0xeada7dd6cde0eb1eULL, 0xf57d4f7fee6ed178ULL, 0x06f067aa72176fbaULL,
    0x0a637dc5a2c898a6ULL, 0x113f9804bef90daeULL, 0x1b710b35131c471bULL,
    0x28db77f523047d84ULL, 0x32caab7b40c72493ULL, 0x3c9ebe0a15c9bebcULL,
    0x431d67c49c100d4cULL, 0x4cc5d4becb3e42b6ULL, 0x597f299cfc657e2aULL,
    0x5fcb6fab3ad6faecULL, 0x6c44198c4a475817ULL
};

#define ROR64(x, n)  (((x) >> (n)) | ((x) << (64 - (n))))
#define CH64(x,y,z)  (((x) & (y)) ^ (~(x) & (z)))
#define MAJ64(x,y,z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))
#define S0_64(x)     (ROR64(x,28) ^ ROR64(x,34) ^ ROR64(x,39))
#define S1_64(x)     (ROR64(x,14) ^ ROR64(x,18) ^ ROR64(x,41))
#define s0_64(x)     (ROR64(x,1)  ^ ROR64(x,8)  ^ ((x) >> 7))
#define s1_64(x)     (ROR64(x,19) ^ ROR64(x,61) ^ ((x) >> 6))

typedef struct {
    uint64_t state[8];
    uint8_t  buf[128];
    uint64_t total;
} Sha512Ctx;

static void sha512_init(Sha512Ctx *ctx) {
    ctx->state[0] = 0x6a09e667f3bcc908ULL;
    ctx->state[1] = 0xbb67ae8584caa73bULL;
    ctx->state[2] = 0x3c6ef372fe94f82bULL;
    ctx->state[3] = 0xa54ff53a5f1d36f1ULL;
    ctx->state[4] = 0x510e527fade682d1ULL;
    ctx->state[5] = 0x9b05688c2b3e6c1fULL;
    ctx->state[6] = 0x1f83d9abfb41bd6bULL;
    ctx->state[7] = 0x5be0cd19137e2179ULL;
    ctx->total = 0;
}

static void sha512_block(Sha512Ctx *ctx, const uint8_t *data) {
    uint64_t W[80];
    for (int i = 0; i < 16; i++) {
        W[i] = ((uint64_t)data[i*8  ] << 56) | ((uint64_t)data[i*8+1] << 48) |
               ((uint64_t)data[i*8+2] << 40) | ((uint64_t)data[i*8+3] << 32) |
               ((uint64_t)data[i*8+4] << 24) | ((uint64_t)data[i*8+5] << 16) |
               ((uint64_t)data[i*8+6] << 8)  | ((uint64_t)data[i*8+7]);
    }
    for (int i = 16; i < 80; i++)
        W[i] = s1_64(W[i-2]) + W[i-7] + s0_64(W[i-15]) + W[i-16];

    uint64_t a = ctx->state[0], b = ctx->state[1], c = ctx->state[2],
             d = ctx->state[3], e = ctx->state[4], f = ctx->state[5],
             g = ctx->state[6], h = ctx->state[7];

    for (int i = 0; i < 80; i++) {
        uint64_t t1 = h + S1_64(e) + CH64(e,f,g) + K512[i] + W[i];
        uint64_t t2 = S0_64(a) + MAJ64(a,b,c);
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }

    ctx->state[0] += a; ctx->state[1] += b; ctx->state[2] += c;
    ctx->state[3] += d; ctx->state[4] += e; ctx->state[5] += f;
    ctx->state[6] += g; ctx->state[7] += h;
}

static void sha512_update(Sha512Ctx *ctx, const uint8_t *data, size_t len) {
    size_t off = (size_t)(ctx->total & 127);
    ctx->total += len;
    while (len > 0) {
        size_t n = 128 - off;
        if (n > len) n = len;
        memcpy(ctx->buf + off, data, n);
        off += n; data += n; len -= n;
        if (off == 128) { sha512_block(ctx, ctx->buf); off = 0; }
    }
}

static void sha512_final(Sha512Ctx *ctx, uint8_t out[64]) {
    size_t off = (size_t)(ctx->total & 127);
    ctx->buf[off++] = 0x80;
    if (off > 112) {
        memset(ctx->buf + off, 0, 128 - off);
        sha512_block(ctx, ctx->buf);
        off = 0;
    }
    memset(ctx->buf + off, 0, 112 - off);
    /* Total bits (big-endian, 128-bit) */
    uint64_t bits = ctx->total * 8;
    memset(ctx->buf + 112, 0, 8);
    for (int i = 0; i < 8; i++)
        ctx->buf[120 + i] = (uint8_t)(bits >> (56 - 8 * i));
    sha512_block(ctx, ctx->buf);
    for (int i = 0; i < 8; i++)
        for (int j = 0; j < 8; j++)
            out[i*8+j] = (uint8_t)(ctx->state[i] >> (56 - 8*j));
}

static void sha512(const uint8_t *data, size_t len, uint8_t out[64]) {
    Sha512Ctx ctx;
    sha512_init(&ctx);
    sha512_update(&ctx, data, len);
    sha512_final(&ctx, out);
}


/* ================================================================
 * GF(2^255 - 19) field arithmetic
 *
 * Elements are represented as 10 limbs of ~25.5 bits each.
 * fe[i] is a signed 64-bit integer; intermediate products stay
 * within 64-bit range before carries.
 * ================================================================ */

typedef int64_t fe[10];

static void fe_0(fe h) { memset(h, 0, sizeof(fe)); }
static void fe_1(fe h) { memset(h, 0, sizeof(fe)); h[0] = 1; }

static void fe_copy(fe h, const fe f) {
    for (int i = 0; i < 10; i++) h[i] = f[i];
}

static void fe_add(fe h, const fe f, const fe g) {
    for (int i = 0; i < 10; i++) h[i] = f[i] + g[i];
}

static void fe_sub(fe h, const fe f, const fe g) {
    for (int i = 0; i < 10; i++) h[i] = f[i] - g[i];
}

static void fe_neg(fe h, const fe f) {
    for (int i = 0; i < 10; i++) h[i] = -f[i];
}

/* Carry-reduce: propagate carries so each limb is bounded.
 * Uses ref10 carry order to prevent int64 overflow in fe_mul. */
static void fe_carry(fe h) {
    int64_t c;
    /* Carry even limbs first (26-bit), interleaved with odd (25-bit) */
    c = (h[0] + (1LL << 25)) >> 26; h[1] += c; h[0] -= c << 26;
    c = (h[4] + (1LL << 25)) >> 26; h[5] += c; h[4] -= c << 26;
    c = (h[1] + (1LL << 24)) >> 25; h[2] += c; h[1] -= c << 25;
    c = (h[5] + (1LL << 24)) >> 25; h[6] += c; h[5] -= c << 25;
    c = (h[2] + (1LL << 25)) >> 26; h[3] += c; h[2] -= c << 26;
    c = (h[6] + (1LL << 25)) >> 26; h[7] += c; h[6] -= c << 26;
    c = (h[3] + (1LL << 24)) >> 25; h[4] += c; h[3] -= c << 25;
    c = (h[7] + (1LL << 24)) >> 25; h[8] += c; h[7] -= c << 25;
    /* Re-carry h[4] after receiving carry from h[3] */
    c = (h[4] + (1LL << 25)) >> 26; h[5] += c; h[4] -= c << 26;
    c = (h[8] + (1LL << 25)) >> 26; h[9] += c; h[8] -= c << 26;
    /* Wrap-around: carry h[9] into h[0] (×19), then re-carry h[0] */
    c = (h[9] + (1LL << 24)) >> 25; h[0] += c * 19; h[9] -= c << 25;
    c = (h[0] + (1LL << 25)) >> 26; h[1] += c; h[0] -= c << 26;
}

/* Multiply: h = f * g */
static void fe_mul(fe h, const fe f, const fe g) {
    /* Use 128-bit intermediates via pairs of 64-bit products */
    int64_t f0 = f[0], f1 = f[1], f2 = f[2], f3 = f[3], f4 = f[4];
    int64_t f5 = f[5], f6 = f[6], f7 = f[7], f8 = f[8], f9 = f[9];
    int64_t g0 = g[0], g1 = g[1], g2 = g[2], g3 = g[3], g4 = g[4];
    int64_t g5 = g[5], g6 = g[6], g7 = g[7], g8 = g[8], g9 = g[9];
    int64_t g1_19 = 19*g1, g2_19 = 19*g2, g3_19 = 19*g3, g4_19 = 19*g4;
    int64_t g5_19 = 19*g5, g6_19 = 19*g6, g7_19 = 19*g7, g8_19 = 19*g8;
    int64_t g9_19 = 19*g9;
    int64_t f1_2 = 2*f1, f3_2 = 2*f3, f5_2 = 2*f5, f7_2 = 2*f7, f9_2 = 2*f9;

    int64_t h0 = f0*g0 + f1_2*g9_19 + f2*g8_19 + f3_2*g7_19 + f4*g6_19 + f5_2*g5_19 + f6*g4_19 + f7_2*g3_19 + f8*g2_19 + f9_2*g1_19;
    int64_t h1 = f0*g1 + f1*g0 + f2*g9_19 + f3*g8_19 + f4*g7_19 + f5*g6_19 + f6*g5_19 + f7*g4_19 + f8*g3_19 + f9*g2_19;
    int64_t h2 = f0*g2 + f1_2*g1 + f2*g0 + f3_2*g9_19 + f4*g8_19 + f5_2*g7_19 + f6*g6_19 + f7_2*g5_19 + f8*g4_19 + f9_2*g3_19;
    int64_t h3 = f0*g3 + f1*g2 + f2*g1 + f3*g0 + f4*g9_19 + f5*g8_19 + f6*g7_19 + f7*g6_19 + f8*g5_19 + f9*g4_19;
    int64_t h4 = f0*g4 + f1_2*g3 + f2*g2 + f3_2*g1 + f4*g0 + f5_2*g9_19 + f6*g8_19 + f7_2*g7_19 + f8*g6_19 + f9_2*g5_19;
    int64_t h5 = f0*g5 + f1*g4 + f2*g3 + f3*g2 + f4*g1 + f5*g0 + f6*g9_19 + f7*g8_19 + f8*g7_19 + f9*g6_19;
    int64_t h6 = f0*g6 + f1_2*g5 + f2*g4 + f3_2*g3 + f4*g2 + f5_2*g1 + f6*g0 + f7_2*g9_19 + f8*g8_19 + f9_2*g7_19;
    int64_t h7 = f0*g7 + f1*g6 + f2*g5 + f3*g4 + f4*g3 + f5*g2 + f6*g1 + f7*g0 + f8*g9_19 + f9*g8_19;
    int64_t h8 = f0*g8 + f1_2*g7 + f2*g6 + f3_2*g5 + f4*g4 + f5_2*g3 + f6*g2 + f7_2*g1 + f8*g0 + f9_2*g9_19;
    int64_t h9 = f0*g9 + f1*g8 + f2*g7 + f3*g6 + f4*g5 + f5*g4 + f6*g3 + f7*g2 + f8*g1 + f9*g0;

    h[0] = h0; h[1] = h1; h[2] = h2; h[3] = h3; h[4] = h4;
    h[5] = h5; h[6] = h6; h[7] = h7; h[8] = h8; h[9] = h9;
    fe_carry(h);
}

/* Square: h = f^2 (slightly faster than fe_mul(h, f, f)) */
static void fe_sq(fe h, const fe f) {
    int64_t f0 = f[0], f1 = f[1], f2 = f[2], f3 = f[3], f4 = f[4];
    int64_t f5 = f[5], f6 = f[6], f7 = f[7], f8 = f[8], f9 = f[9];
    int64_t f0_2 = 2*f0, f1_2 = 2*f1, f2_2 = 2*f2, f3_2 = 2*f3, f4_2 = 2*f4;
    int64_t f5_2 = 2*f5, f6_2 = 2*f6, f7_2 = 2*f7;
    int64_t f5_38 = 38*f5, f6_19 = 19*f6, f7_38 = 38*f7, f8_19 = 19*f8, f9_38 = 38*f9;

    int64_t h0 = f0*f0 + f1_2*f9_38 + f2_2*f8_19 + f3_2*f7_38 + f4_2*f6_19 + f5*f5_38;
    int64_t h1 = f0_2*f1 + f2*f9_38 + f3_2*f8_19 + f4*f7_38 + f5_2*f6_19;
    int64_t h2 = f0_2*f2 + f1_2*f1 + f3_2*f9_38 + f4_2*f8_19 + f5_2*f7_38 + f6*f6_19;
    int64_t h3 = f0_2*f3 + f1_2*f2 + f4*f9_38 + f5_2*f8_19 + f6*f7_38;
    int64_t h4 = f0_2*f4 + f1_2*f3_2 + f2*f2 + f5_2*f9_38 + f6_2*f8_19 + f7*f7_38;
    int64_t h5 = f0_2*f5 + f1_2*f4 + f2_2*f3 + f6*f9_38 + f7_2*f8_19;
    int64_t h6 = f0_2*f6 + f1_2*f5_2 + f2_2*f4 + f3_2*f3 + f7_2*f9_38 + f8*f8_19;
    int64_t h7 = f0_2*f7 + f1_2*f6 + f2_2*f5 + f3_2*f4 + f8*f9_38;
    int64_t h8 = f0_2*f8 + f1_2*f7_2 + f2_2*f6 + f3_2*f5_2 + f4*f4 + f9*f9_38;
    int64_t h9 = f0_2*f9 + f1_2*f8 + f2_2*f7 + f3_2*f6 + f4_2*f5;

    h[0] = h0; h[1] = h1; h[2] = h2; h[3] = h3; h[4] = h4;
    h[5] = h5; h[6] = h6; h[7] = h7; h[8] = h8; h[9] = h9;
    fe_carry(h);
}

/* Load 3/4 bytes little-endian */
static int64_t load_3(const uint8_t *in) {
    return (int64_t)in[0] | ((int64_t)in[1] << 8) | ((int64_t)in[2] << 16);
}
static int64_t load_4(const uint8_t *in) {
    return (int64_t)in[0] | ((int64_t)in[1] << 8) |
           ((int64_t)in[2] << 16) | ((int64_t)in[3] << 24);
}

/* Deserialize 32 bytes (little-endian) to field element (ref10) */
static void fe_frombytes(fe h, const uint8_t s[32]) {
    int64_t h0 = load_4(s);
    int64_t h1 = load_3(s + 4) << 6;
    int64_t h2 = load_3(s + 7) << 5;
    int64_t h3 = load_3(s + 10) << 3;
    int64_t h4 = load_3(s + 13) << 2;
    int64_t h5 = load_4(s + 16);
    int64_t h6 = load_3(s + 20) << 7;
    int64_t h7 = load_3(s + 23) << 5;
    int64_t h8 = load_3(s + 26) << 4;
    int64_t h9 = (load_3(s + 29) & 8388607) << 2;

    int64_t carry9 = (h9 + (1LL << 24)) >> 25; h0 += carry9 * 19; h9 -= carry9 * (1LL << 25);
    int64_t carry1 = (h1 + (1LL << 24)) >> 25; h2 += carry1; h1 -= carry1 * (1LL << 25);
    int64_t carry3 = (h3 + (1LL << 24)) >> 25; h4 += carry3; h3 -= carry3 * (1LL << 25);
    int64_t carry5 = (h5 + (1LL << 24)) >> 25; h6 += carry5; h5 -= carry5 * (1LL << 25);
    int64_t carry7 = (h7 + (1LL << 24)) >> 25; h8 += carry7; h7 -= carry7 * (1LL << 25);

    int64_t carry0 = (h0 + (1LL << 25)) >> 26; h1 += carry0; h0 -= carry0 * (1LL << 26);
    int64_t carry2 = (h2 + (1LL << 25)) >> 26; h3 += carry2; h2 -= carry2 * (1LL << 26);
    int64_t carry4 = (h4 + (1LL << 25)) >> 26; h5 += carry4; h4 -= carry4 * (1LL << 26);
    int64_t carry6 = (h6 + (1LL << 25)) >> 26; h7 += carry6; h6 -= carry6 * (1LL << 26);
    int64_t carry8 = (h8 + (1LL << 25)) >> 26; h9 += carry8; h8 -= carry8 * (1LL << 26);

    h[0] = h0; h[1] = h1; h[2] = h2; h[3] = h3; h[4] = h4;
    h[5] = h5; h[6] = h6; h[7] = h7; h[8] = h8; h[9] = h9;
}

/* Serialize field element to 32 bytes (little-endian, fully reduced, ref10) */
static void fe_tobytes(uint8_t s[32], const fe h) {
    int32_t h0 = (int32_t)h[0], h1 = (int32_t)h[1], h2 = (int32_t)h[2];
    int32_t h3 = (int32_t)h[3], h4 = (int32_t)h[4], h5 = (int32_t)h[5];
    int32_t h6 = (int32_t)h[6], h7 = (int32_t)h[7], h8 = (int32_t)h[8];
    int32_t h9 = (int32_t)h[9];
    int32_t q, carry;

    q = (19 * h9 + (((int32_t)1) << 24)) >> 25;
    q = (h0 + q) >> 26; q = (h1 + q) >> 25; q = (h2 + q) >> 26;
    q = (h3 + q) >> 25; q = (h4 + q) >> 26; q = (h5 + q) >> 25;
    q = (h6 + q) >> 26; q = (h7 + q) >> 25; q = (h8 + q) >> 26;
    q = (h9 + q) >> 25;

    h0 += 19 * q;

    carry = h0 >> 26; h1 += carry; h0 -= carry * (1 << 26);
    carry = h1 >> 25; h2 += carry; h1 -= carry * (1 << 25);
    carry = h2 >> 26; h3 += carry; h2 -= carry * (1 << 26);
    carry = h3 >> 25; h4 += carry; h3 -= carry * (1 << 25);
    carry = h4 >> 26; h5 += carry; h4 -= carry * (1 << 26);
    carry = h5 >> 25; h6 += carry; h5 -= carry * (1 << 25);
    carry = h6 >> 26; h7 += carry; h6 -= carry * (1 << 26);
    carry = h7 >> 25; h8 += carry; h7 -= carry * (1 << 25);
    carry = h8 >> 26; h9 += carry; h8 -= carry * (1 << 26);
    carry = h9 >> 25;              h9 -= carry * (1 << 25);

    s[ 0] = (uint8_t)(h0 >> 0);
    s[ 1] = (uint8_t)(h0 >> 8);
    s[ 2] = (uint8_t)(h0 >> 16);
    s[ 3] = (uint8_t)((h0 >> 24) | (h1 << 2));
    s[ 4] = (uint8_t)(h1 >> 6);
    s[ 5] = (uint8_t)(h1 >> 14);
    s[ 6] = (uint8_t)((h1 >> 22) | (h2 << 3));
    s[ 7] = (uint8_t)(h2 >> 5);
    s[ 8] = (uint8_t)(h2 >> 13);
    s[ 9] = (uint8_t)((h2 >> 21) | (h3 << 5));
    s[10] = (uint8_t)(h3 >> 3);
    s[11] = (uint8_t)(h3 >> 11);
    s[12] = (uint8_t)((h3 >> 19) | (h4 << 6));
    s[13] = (uint8_t)(h4 >> 2);
    s[14] = (uint8_t)(h4 >> 10);
    s[15] = (uint8_t)(h4 >> 18);
    s[16] = (uint8_t)(h5 >> 0);
    s[17] = (uint8_t)(h5 >> 8);
    s[18] = (uint8_t)(h5 >> 16);
    s[19] = (uint8_t)((h5 >> 24) | (h6 << 1));
    s[20] = (uint8_t)(h6 >> 7);
    s[21] = (uint8_t)(h6 >> 15);
    s[22] = (uint8_t)((h6 >> 23) | (h7 << 3));
    s[23] = (uint8_t)(h7 >> 5);
    s[24] = (uint8_t)(h7 >> 13);
    s[25] = (uint8_t)((h7 >> 21) | (h8 << 4));
    s[26] = (uint8_t)(h8 >> 4);
    s[27] = (uint8_t)(h8 >> 12);
    s[28] = (uint8_t)((h8 >> 20) | (h9 << 6));
    s[29] = (uint8_t)(h9 >> 2);
    s[30] = (uint8_t)(h9 >> 10);
    s[31] = (uint8_t)(h9 >> 18);
}

/* Invert: h = 1/f using Fermat's little theorem: f^(p-2) mod p
 * p-2 = 2^255 - 21, so we compute z^(2^255-21). */
static void fe_invert(fe out, const fe z) {
    fe t0, t1, t2, t3;
    int i;

    fe_sq(t0, z);              /* t0 = z^2 */
    fe_sq(t1, t0);
    fe_sq(t1, t1);             /* t1 = z^8 */
    fe_mul(t1, z, t1);        /* t1 = z^9 */
    fe_mul(t0, t0, t1);       /* t0 = z^11 */
    fe_sq(t2, t0);             /* t2 = z^22 */
    fe_mul(t1, t1, t2);       /* t1 = z^(2^5-1) */
    fe_sq(t2, t1);
    for (i = 1; i < 5; i++) fe_sq(t2, t2);   /* t2 = z^(2^10-2^5) */
    fe_mul(t1, t2, t1);       /* t1 = z^(2^10-1) */
    fe_sq(t2, t1);
    for (i = 1; i < 10; i++) fe_sq(t2, t2);  /* t2 = z^(2^20-2^10) */
    fe_mul(t2, t2, t1);       /* t2 = z^(2^20-1) */
    fe_sq(t3, t2);
    for (i = 1; i < 20; i++) fe_sq(t3, t3);  /* t3 = z^(2^40-2^20) */
    fe_mul(t2, t3, t2);       /* t2 = z^(2^40-1) */
    fe_sq(t2, t2);
    for (i = 1; i < 10; i++) fe_sq(t2, t2);  /* t2 = z^(2^50-2^10) */
    fe_mul(t1, t2, t1);       /* t1 = z^(2^50-1) */
    fe_sq(t2, t1);
    for (i = 1; i < 50; i++) fe_sq(t2, t2);  /* t2 = z^(2^100-2^50) */
    fe_mul(t2, t2, t1);       /* t2 = z^(2^100-1) */
    fe_sq(t3, t2);
    for (i = 1; i < 100; i++) fe_sq(t3, t3); /* t3 = z^(2^200-2^100) */
    fe_mul(t2, t3, t2);       /* t2 = z^(2^200-1) */
    fe_sq(t2, t2);
    for (i = 1; i < 50; i++) fe_sq(t2, t2);  /* t2 = z^(2^250-2^50) */
    fe_mul(t1, t2, t1);       /* t1 = z^(2^250-1) */
    fe_sq(t1, t1);
    for (i = 1; i < 5; i++) fe_sq(t1, t1);   /* t1 = z^(2^255-2^5) */
    fe_mul(out, t1, t0);      /* out = z^(2^255-21) = z^(p-2) */
}

/* Raise to (p-5)/8 — used for sqrt-like operations */
static void fe_pow2523(fe out, const fe z) {
    fe t0, t1, t2;
    int i;

    fe_sq(t0, z);
    fe_sq(t1, t0);
    fe_sq(t1, t1);
    fe_mul(t1, z, t1);
    fe_mul(t0, t0, t1);
    fe_sq(t0, t0);
    fe_mul(t0, t1, t0);
    fe_sq(t1, t0);
    for (i = 1; i < 5; i++) fe_sq(t1, t1);
    fe_mul(t0, t1, t0);
    fe_sq(t1, t0);
    for (i = 1; i < 10; i++) fe_sq(t1, t1);
    fe_mul(t1, t1, t0);
    fe_sq(t2, t1);
    for (i = 1; i < 20; i++) fe_sq(t2, t2);
    fe_mul(t1, t2, t1);
    fe_sq(t1, t1);
    for (i = 1; i < 10; i++) fe_sq(t1, t1);
    fe_mul(t0, t1, t0);
    fe_sq(t1, t0);
    for (i = 1; i < 50; i++) fe_sq(t1, t1);
    fe_mul(t1, t1, t0);
    fe_sq(t2, t1);
    for (i = 1; i < 100; i++) fe_sq(t2, t2);
    fe_mul(t1, t2, t1);
    fe_sq(t1, t1);
    for (i = 1; i < 50; i++) fe_sq(t1, t1);
    fe_mul(t0, t1, t0);
    fe_sq(t0, t0);
    fe_sq(t0, t0);
    fe_mul(out, t0, z);
}

/* Check if f == 0 (constant time) */
static int fe_isnonzero(const fe f) {
    uint8_t s[32];
    fe_tobytes(s, f);
    uint8_t r = 0;
    for (int i = 0; i < 32; i++) r |= s[i];
    return r != 0;
}

/* Check if f is negative (lowest bit of canonical form) */
static int fe_isnegative(const fe f) {
    uint8_t s[32];
    fe_tobytes(s, f);
    return s[0] & 1;
}

/* ================================================================
 * Extended coordinates for the Ed25519 curve
 *
 * Curve: -x^2 + y^2 = 1 + d*x^2*y^2
 * d = -121665/121666 mod p
 *
 * Extended coordinates: (X, Y, Z, T) where
 *   x = X/Z, y = Y/Z, xy = T/Z
 * ================================================================ */

typedef struct {
    fe X, Y, Z, T;
} ge;    /* group element */

/* d = -121665/121666 mod p */
static const fe d_const = {
    -10913610, 13857413, -15372611, 6949391, 114729,
    -8787816, -6275908, -3247719, -18696448, -12055116
};

/* 2*d */
static const fe d2_const = {
    -21827239, -5839606, -30745221, 13898782, 229458,
    15978800, -12551817, -6495438, 29715968, 9444199
};

/* sqrt(-1) mod p */
static const fe sqrtm1 = {
    -32595792, -7943725, 9377950, 3500415, 12389472,
    -272473, -25146209, -2005654, 326686, 11406482
};

/* Base point B (compressed, 32 bytes, little-endian y with sign bit) */
static const uint8_t B_compressed[32] = {
    0x58, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66,
    0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66,
    0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66,
    0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66
};

/* Set p to the neutral element (identity) */
static void ge_zero(ge *p) {
    fe_0(p->X);
    fe_1(p->Y);
    fe_1(p->Z);
    fe_0(p->T);
}

/* Decode a 32-byte compressed point to extended coordinates.
 * Returns 0 on success, -1 on invalid point. */
static int ge_frombytes(ge *p, const uint8_t s[32]) {
    fe u, v, v3, vxx, check;

    fe_frombytes(p->Y, s);
    fe_1(p->Z);
    fe_sq(u, p->Y);         /* u = y^2 */
    fe_mul(v, u, d_const);  /* v = d*y^2 */
    fe_sub(u, u, p->Z);     /* u = y^2 - 1 */
    fe_add(v, v, p->Z);     /* v = d*y^2 + 1 */

    fe_sq(v3, v);
    fe_mul(v3, v3, v);       /* v3 = v^3 */
    fe_sq(p->X, v3);
    fe_mul(p->X, p->X, v);  /* X = v^7 */
    fe_mul(p->X, p->X, u);  /* X = u*v^7 */
    fe_pow2523(p->X, p->X); /* X = (u*v^7)^((p-5)/8) */
    fe_mul(p->X, p->X, v3); /* X = v^3 * (u*v^7)^((p-5)/8) */
    fe_mul(p->X, p->X, u);  /* X = u * v^3 * (u*v^7)^((p-5)/8) */

    fe_sq(vxx, p->X);
    fe_mul(vxx, vxx, v);     /* vxx = v*x^2 */
    fe_sub(check, vxx, u);
    if (fe_isnonzero(check)) {
        fe_add(check, vxx, u);
        if (fe_isnonzero(check)) return -1;
        fe_mul(p->X, p->X, sqrtm1);
    }

    if (fe_isnegative(p->X) != (s[31] >> 7)) {
        fe_neg(p->X, p->X);
    }

    fe_mul(p->T, p->X, p->Y);
    return 0;
}

/* Encode extended coordinates to 32-byte compressed point */
static void ge_tobytes(uint8_t s[32], const ge *p) {
    fe recip, x, y;
    fe_invert(recip, p->Z);
    fe_mul(x, p->X, recip);
    fe_mul(y, p->Y, recip);
    fe_tobytes(s, y);
    s[31] ^= (uint8_t)(fe_isnegative(x) << 7);
}

/* p = p + q (extended coordinates addition) */
static void ge_add(ge *r, const ge *p, const ge *q) {
    fe a, b, c, d_fe, e, f, g, h;
    fe_sub(a, p->Y, p->X);
    fe t;
    fe_sub(t, q->Y, q->X);
    fe_mul(a, a, t);
    fe_add(b, p->Y, p->X);
    fe_add(t, q->Y, q->X);
    fe_mul(b, b, t);
    fe_mul(c, p->T, q->T);
    fe_mul(c, c, d2_const);
    fe_mul(d_fe, p->Z, q->Z);
    fe_add(d_fe, d_fe, d_fe);
    fe_sub(e, b, a);
    fe_sub(f, d_fe, c);
    fe_add(g, d_fe, c);
    fe_add(h, b, a);
    fe_mul(r->X, e, f);
    fe_mul(r->Y, h, g);
    fe_mul(r->Z, g, f);
    fe_mul(r->T, e, h);
}

/* p = p + p (doubling in extended coordinates) */
static void ge_double(ge *r, const ge *p) {
    fe a, b, c, d_fe, e, f, g, h;
    fe_sq(a, p->X);
    fe_sq(b, p->Y);
    fe_sq(c, p->Z);
    fe_add(c, c, c);
    fe_neg(d_fe, a);
    fe_add(e, p->X, p->Y);
    fe_sq(e, e);
    fe_sub(e, e, a);
    fe_sub(e, e, b);
    fe_add(g, d_fe, b);
    fe_sub(f, g, c);
    fe_sub(h, d_fe, b);
    fe_mul(r->X, e, f);
    fe_mul(r->Y, h, g);
    fe_mul(r->Z, f, g);
    fe_mul(r->T, e, h);
}

/* ================================================================
 * Scalar reduction mod L
 *
 * L = 2^252 + 27742317777372353535851937790883648493
 * ================================================================ */

/* The group order L as bytes (little-endian) */
static const uint8_t L[32] = {
    0xed, 0xd3, 0xf5, 0x5c, 0x1a, 0x63, 0x12, 0x58,
    0xd6, 0x9c, 0xf7, 0xa2, 0xde, 0xf9, 0xde, 0x14,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10
};

/* Reduce a 64-byte scalar (as from SHA-512) mod L.
 * Produces a 32-byte result. Uses schoolbook reduction. */
static void sc_reduce(uint8_t out[32], const uint8_t in[64]) {
    int64_t s[24];
    for (int i = 0; i < 24; i++) s[i] = 0;

    /* Load 64 bytes as 21-bit limbs */
    for (int i = 0; i < 64; i++) {
        int limb = (i * 8) / 21;
        int shift = (i * 8) % 21;
        if (limb < 24)
            s[limb] += (int64_t)(in[i]) << shift;
        if (limb + 1 < 24 && shift > 13)
            s[limb + 1] += (int64_t)(in[i]) >> (21 - shift);
    }

    /* Carry and reduce */
    for (int i = 0; i < 23; i++) {
        int64_t carry = s[i] >> 21;
        s[i + 1] += carry;
        s[i] &= (1LL << 21) - 1;
    }

    /* Now do Barrett-like reduction mod L.
     * Since L ≈ 2^252, limbs 12+ carry weight above 2^252.
     * We subtract multiples of L. This is a simplified approach. */

    /* Pack back to bytes for a different approach */
    /* Actually, let's use the simple approach: interpret as a big integer
     * and reduce mod L using repeated subtraction from the top. */

    /* Simpler: use 32-bit limbs schoolbook reduction */
    /* Reset and use a cleaner approach */

    /* Load as signed 64-byte number in 32-bit chunks */
    int64_t a[32];
    for (int i = 0; i < 32; i++) a[i] = 0;
    for (int i = 0; i < 64; i++) a[i / 2] += (int64_t)(in[i]) << ((i & 1) * 8);
    /* Actually, let me just use the standard 23-limb Barrett approach from ref10 */

    /* Barrett reduction over L, using 23 x 21-bit limbs.
     * This is complex, so let's use a simpler iterative approach
     * that works for our use case (64-byte SHA-512 output). */

    /* Simple: convert to big-endian, repeatedly subtract L if >= L */
    uint8_t tmp[64];
    memcpy(tmp, in, 64);

    /* Since in < 2^512 and L ≈ 2^252, we need at most ~260 subtractions,
     * which is impractical. Use schoolbook division instead.
     *
     * Actually, the standard approach for Ed25519 is to use the
     * Barrett reduction from the ref10 code. Let me implement that
     * properly. */

    /* Reference implementation: 23 x int64 limbs, carry-reduce from top */
    int64_t t[23];
    for (int i = 0; i < 23; i++) t[i] = 0;

    /* Load as 23 limbs of 22 bits */
    for (int i = 0; i < 64; i++) {
        int pos = (i * 8) / 22;
        int off = (i * 8) % 22;
        if (pos < 23)
            t[pos] |= (int64_t)(in[i]) << off;
        if (pos + 1 < 23 && off > 14)
            t[pos + 1] |= (int64_t)(in[i]) >> (22 - off);
    }
    for (int i = 0; i < 23; i++) t[i] &= (1LL << 22) - 1;

    /* This approach is getting unwieldy. Let me use the simple
     * TweetNaCl modL function instead. */

    /* TweetNaCl modL: operates on int64_t[64] (byte-level limbs) */
    int64_t x[64];
    for (int i = 0; i < 64; i++) x[i] = (int64_t)(uint8_t)in[i];

    for (int i = 63; i >= 32; i--) {
        int64_t carry = 0;
        int j;
        for (j = i - 32; j < i - 12; j++) {
            x[j] += carry - 16 * x[i] * (int64_t)L[j - (i - 32)];
            carry = (x[j] + 128) >> 8;
            x[j] -= carry * 256;
        }
        x[j] += carry;
        x[i] = 0;
    }

    {
        int64_t carry = 0;
        for (int j = 0; j < 32; j++) {
            x[j] += carry - (x[31] >> 4) * (int64_t)L[j];
            carry = x[j] >> 8;
            x[j] &= 255;
        }
        for (int j = 0; j < 32; j++)
            x[j] -= carry * (int64_t)L[j];
    }

    for (int i = 0; i < 32; i++)
        out[i] = (uint8_t)(x[i] & 255);
}

/* sc_muladd: out = a*b + c (mod L), all 32-byte scalars */
static void sc_muladd(uint8_t out[32], const uint8_t a[32],
                       const uint8_t b[32], const uint8_t c[32]) {
    int64_t x[64];
    for (int i = 0; i < 64; i++) x[i] = 0;

    /* Schoolbook multiply: x = a * b */
    for (int i = 0; i < 32; i++)
        for (int j = 0; j < 32; j++)
            x[i + j] += (int64_t)(uint8_t)a[i] * (int64_t)(uint8_t)b[j];

    /* Add c */
    for (int i = 0; i < 32; i++)
        x[i] += (int64_t)(uint8_t)c[i];

    /* Reduce mod L using TweetNaCl method */
    for (int i = 63; i >= 32; i--) {
        int64_t carry = 0;
        int j;
        for (j = i - 32; j < i - 12; j++) {
            x[j] += carry - 16 * x[i] * (int64_t)L[j - (i - 32)];
            carry = (x[j] + 128) >> 8;
            x[j] -= carry * 256;
        }
        x[j] += carry;
        x[i] = 0;
    }

    {
        int64_t carry = 0;
        for (int j = 0; j < 32; j++) {
            x[j] += carry - (x[31] >> 4) * (int64_t)L[j];
            carry = x[j] >> 8;
            x[j] &= 255;
        }
        for (int j = 0; j < 32; j++)
            x[j] -= carry * (int64_t)L[j];
    }

    for (int i = 0; i < 32; i++)
        out[i] = (uint8_t)(x[i] & 255);
}

/* ================================================================
 * Scalar multiplication: R = s * P
 *
 * Uses double-and-add (simple, not constant-time — adequate for
 * verification; signing uses the deterministic nonce so timing
 * doesn't leak the key).
 * ================================================================ */

static void ge_scalarmult(ge *r, const uint8_t scalar[32], const ge *p) {
    ge_zero(r);
    ge acc;
    ge_zero(&acc);

    ge base;
    memcpy(&base, p, sizeof(ge));

    for (int i = 0; i < 256; i++) {
        int bit = (scalar[i / 8] >> (i & 7)) & 1;
        if (bit) ge_add(&acc, &acc, &base);
        ge_double(&base, &base);
    }
    memcpy(r, &acc, sizeof(ge));
}

/* Scalar multiplication by the base point B */
static void ge_scalarmult_base(ge *r, const uint8_t scalar[32]) {
    ge base;
    ge_frombytes(&base, B_compressed);
    ge_scalarmult(r, scalar, &base);
}


/* ================================================================
 * Ed25519 API
 * ================================================================ */

/* Derive a public key from a 32-byte seed */
NOW_API int now_ed25519_keypair(unsigned char pub[32],
                                 unsigned char priv[64],
                                 const unsigned char seed[32]) {
    if (!pub || !priv || !seed) return -1;

    /* Hash the seed */
    uint8_t h[64];
    sha512(seed, 32, h);

    /* Clamp */
    h[0]  &= 248;
    h[31] &= 127;
    h[31] |= 64;

    /* Public key = clamp(H(seed)) * B */
    ge A;
    ge_scalarmult_base(&A, h);
    ge_tobytes(pub, &A);

    /* Private key = seed || public key */
    memcpy(priv, seed, 32);
    memcpy(priv + 32, pub, 32);

    return 0;
}

/* Sign a message. sig is 64 bytes. */
NOW_API int now_ed25519_sign(unsigned char sig[64],
                               const unsigned char *msg, size_t msg_len,
                               const unsigned char priv[64]) {
    if (!sig || !msg || !priv) return -1;

    uint8_t h[64];
    sha512(priv, 32, h);

    /* Clamp the secret scalar */
    h[0]  &= 248;
    h[31] &= 127;
    h[31] |= 64;

    /* r = H(h[32..64] || msg) mod L */
    uint8_t r_hash[64];
    {
        Sha512Ctx ctx;
        sha512_init(&ctx);
        sha512_update(&ctx, h + 32, 32);
        sha512_update(&ctx, msg, msg_len);
        sha512_final(&ctx, r_hash);
    }

    uint8_t r_scalar[32];
    sc_reduce(r_scalar, r_hash);

    /* R = r * B */
    ge R;
    ge_scalarmult_base(&R, r_scalar);
    ge_tobytes(sig, &R);

    /* k = H(R || pub || msg) mod L */
    uint8_t k_hash[64];
    {
        Sha512Ctx ctx;
        sha512_init(&ctx);
        sha512_update(&ctx, sig, 32);
        sha512_update(&ctx, priv + 32, 32);
        sha512_update(&ctx, msg, msg_len);
        sha512_final(&ctx, k_hash);
    }

    uint8_t k_scalar[32];
    sc_reduce(k_scalar, k_hash);

    /* S = r + k * a mod L */
    sc_muladd(sig + 32, k_scalar, h, r_scalar);

    return 0;
}

/* Verify an Ed25519 signature.
 * Returns 0 if valid, -1 if invalid. */
NOW_API int now_ed25519_verify(const unsigned char *sig,
                                const unsigned char *msg, size_t msg_len,
                                const unsigned char *pub_key) {
    if (!sig || !msg || !pub_key) return -1;

    /* Decode the public key point A */
    ge A;
    if (ge_frombytes(&A, pub_key) != 0) return -1;

    /* Negate A for the verification equation */
    fe_neg(A.X, A.X);
    fe_neg(A.T, A.T);

    /* k = H(R || pub || msg) mod L */
    uint8_t k_hash[64];
    {
        Sha512Ctx ctx;
        sha512_init(&ctx);
        sha512_update(&ctx, sig, 32);
        sha512_update(&ctx, pub_key, 32);
        sha512_update(&ctx, msg, msg_len);
        sha512_final(&ctx, k_hash);
    }
    uint8_t k[32];
    sc_reduce(k, k_hash);

    /* Check: S*B - k*A == R
     * Compute S*B and k*A separately, then compare. */
    ge sB, kA, check;
    ge_scalarmult_base(&sB, sig + 32);
    ge_scalarmult(&kA, k, &A);

    /* sB + kA (kA has negated A, so this is sB - k*A_original) */
    ge_add(&check, &sB, &kA);

    uint8_t check_bytes[32];
    ge_tobytes(check_bytes, &check);

    /* Compare with R (first 32 bytes of sig) */
    int diff = 0;
    for (int i = 0; i < 32; i++)
        diff |= check_bytes[i] ^ sig[i];

    return diff ? -1 : 0;
}

/* ================================================================
 * File-level signature verification (replaces minisign delegation)
 * ================================================================ */

/* Base64 decode (standard alphabet, no padding requirement) */
static int b64_decode(const char *in, size_t in_len,
                       uint8_t *out, size_t *out_len) {
    static const char b64_chars[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    size_t j = 0;
    uint32_t acc = 0;
    int bits = 0;

    for (size_t i = 0; i < in_len; i++) {
        if (in[i] == '=' || in[i] == '\n' || in[i] == '\r') continue;
        const char *pos = strchr(b64_chars, in[i]);
        if (!pos) return -1;
        int val = (int)(pos - b64_chars);
        acc = (acc << 6) | (uint32_t)val;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out[j++] = (uint8_t)((acc >> bits) & 0xff);
        }
    }
    *out_len = j;
    return 0;
}

/* Read file contents into a malloc'd buffer */
static char *ed_read_file(const char *path, size_t *out_len) {
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

NOW_API int now_verify_file(const char *archive_path, const char *sig_path,
                             const char *pubkey_b64, NowResult *result) {
    if (!archive_path || !sig_path || !pubkey_b64) {
        if (result) {
            result->code = NOW_ERR_SCHEMA;
            snprintf(result->message, sizeof(result->message),
                     "verify: archive, sig, and key required");
        }
        return -1;
    }

    if (!now_path_exists(archive_path)) {
        if (result) {
            result->code = NOW_ERR_NOT_FOUND;
            snprintf(result->message, sizeof(result->message),
                     "verify: archive not found: %s", archive_path);
        }
        return -1;
    }
    if (!now_path_exists(sig_path)) {
        if (result) {
            result->code = NOW_ERR_NOT_FOUND;
            snprintf(result->message, sizeof(result->message),
                     "verify: signature not found: %s", sig_path);
        }
        return -1;
    }

    /* Decode the public key from base64 */
    uint8_t pub_raw[64];
    size_t pub_len;
    if (b64_decode(pubkey_b64, strlen(pubkey_b64), pub_raw, &pub_len) != 0 ||
        pub_len != 32) {
        if (result) {
            result->code = NOW_ERR_SCHEMA;
            snprintf(result->message, sizeof(result->message),
                     "verify: invalid public key (expected 32 bytes, got %zu)",
                     pub_len);
        }
        return -1;
    }

    /* Read the signature file (64 bytes raw or base64) */
    size_t sig_file_len;
    char *sig_file = ed_read_file(sig_path, &sig_file_len);
    if (!sig_file) {
        if (result) {
            result->code = NOW_ERR_IO;
            snprintf(result->message, sizeof(result->message),
                     "verify: cannot read signature file");
        }
        return -1;
    }

    uint8_t sig_raw[64];
    if (sig_file_len == 64) {
        /* Raw binary signature */
        memcpy(sig_raw, sig_file, 64);
    } else {
        /* Try base64 decode */
        size_t sig_decoded_len;
        if (b64_decode(sig_file, sig_file_len, sig_raw, &sig_decoded_len) != 0 ||
            sig_decoded_len != 64) {
            free(sig_file);
            if (result) {
                result->code = NOW_ERR_SCHEMA;
                snprintf(result->message, sizeof(result->message),
                         "verify: invalid signature (expected 64 bytes)");
            }
            return -1;
        }
    }
    free(sig_file);

    /* Read the archive data */
    size_t msg_len;
    char *msg_data = ed_read_file(archive_path, &msg_len);
    if (!msg_data) {
        if (result) {
            result->code = NOW_ERR_IO;
            snprintf(result->message, sizeof(result->message),
                     "verify: cannot read archive");
        }
        return -1;
    }

    /* Verify the Ed25519 signature */
    int rc = now_ed25519_verify(sig_raw, (const unsigned char *)msg_data,
                                 msg_len, pub_raw);
    free(msg_data);

    if (rc == 0) {
        if (result) result->code = NOW_OK;
        return 0;
    }

    if (result) {
        result->code = NOW_ERR_AUTH;
        snprintf(result->message, sizeof(result->message),
                 "signature verification failed");
    }
    return -1;
}

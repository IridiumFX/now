#ifndef APENNINES_T2_COMPRESS_H
#define APENNINES_T2_COMPRESS_H

#include "apennines/export.h"
#include "apennines/types.h"
#include "apennines/t1/buffer/buf.h"

typedef enum {
    COMPRESS_LEVEL_FAST    = 1,
    COMPRESS_LEVEL_DEFAULT = 6,
    COMPRESS_LEVEL_BEST    = 9
} compress_level;

/* LZ4 block format (wire-compatible with reference lz4 implementation).
 * Verified against python-lz4 with inputs 10 B – 200 KB, both directions. */
APENNINES_API unsigned long lz4_compress(buf *out, u8 *data, u64 len);
APENNINES_API unsigned long lz4_decompress(buf *out, u8 *data, u64 len);

/* DEFLATE (RFC 1951) — full BTYPE coverage on both sides.
 *
 *  Compress — dispatched by level:
 *    COMPRESS_LEVEL_FAST    → BTYPE=0 (stored, no compression, 64KB blocks)
 *    COMPRESS_LEVEL_DEFAULT → BTYPE=1 (fixed Huffman, LZ77 matching)
 *    COMPRESS_LEVEL_BEST    → BTYPE=2 (dynamic Huffman, optimal entropy)
 *
 *  Decompress: BTYPE=0/1/2 all supported.
 *
 *  Wire-compatible with reference zlib — verified against python zlib.
 *  At LEVEL_BEST, compression ratio is typically within 1-5% of real zlib. */
APENNINES_API unsigned long deflate_compress(buf *out, u8 *data, u64 len, compress_level level);
APENNINES_API unsigned long deflate_decompress(buf *out, u8 *data, u64 len);

#endif /* APENNINES_T2_COMPRESS_H */

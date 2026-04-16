/*
 * pasta.h — Compatibility shim: maps pasta API to basta
 *
 * Basta is a strict superset of Pasta (adds BASTA_BLOB).
 * This header lets code written against the pasta API compile
 * against basta with zero source changes.
 */
#ifndef PASTA_COMPAT_H
#define PASTA_COMPAT_H

#include "basta.h"

/* Types */
typedef BastaValue  PastaValue;
typedef BastaResult PastaResult;
typedef BastaType   PastaType;

/* Type constants */
#define PASTA_NULL    BASTA_NULL
#define PASTA_BOOL    BASTA_BOOL
#define PASTA_NUMBER  BASTA_NUMBER
#define PASTA_STRING  BASTA_STRING
#define PASTA_ARRAY   BASTA_ARRAY
#define PASTA_MAP     BASTA_MAP
#define PASTA_LABEL   BASTA_LABEL

/* Write flags */
#define PASTA_PRETTY  BASTA_PRETTY
#define PASTA_SORTED  BASTA_SORTED

/* Parse */
#define pasta_parse      basta_parse
#define pasta_parse_cstr basta_parse_cstr
#define pasta_free       basta_free

/* Type checks */
#define pasta_type           basta_type
#define pasta_is_null        basta_is_null
#define pasta_get_bool       basta_get_bool
#define pasta_get_number     basta_get_number
#define pasta_get_number_fmt basta_get_number_fmt
#define pasta_get_string     basta_get_string
#define pasta_get_string_len basta_get_string_len
#define pasta_get_label      basta_get_label
#define pasta_get_label_len  basta_get_label_len

/* Collections */
#define pasta_count     basta_count
#define pasta_array_get basta_array_get
#define pasta_map_get   basta_map_get

/* Map iteration */
#define pasta_map_key   basta_map_key
#define pasta_map_value basta_map_value

/* Builders */
#define pasta_new_null    basta_new_null
#define pasta_new_bool    basta_new_bool
#define pasta_new_number  basta_new_number
#define pasta_new_string      basta_new_string
#define pasta_new_string_len  basta_new_string_len
#define pasta_new_array   basta_new_array
#define pasta_new_map     basta_new_map
#define pasta_new_label       basta_new_label
#define pasta_new_label_len   basta_new_label_len
#define pasta_push        basta_push
#define pasta_set         basta_set

/* Result codes */
#define PASTA_OK BASTA_OK

/* Number format */
#define PASTA_NUM_DEC BASTA_NUM_DEC
#define PASTA_NUM_HEX BASTA_NUM_HEX
#define PASTA_NUM_BIN BASTA_NUM_BIN

/* Write flags */
#define PASTA_SECTIONS BASTA_SECTIONS

/* Writer — basta_write takes extra out_len param, wrap for pasta compat */
static inline char *pasta_write(const PastaValue *v, int flags) {
    return basta_write(v, flags, NULL);
}

#endif /* PASTA_COMPAT_H */

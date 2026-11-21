/*
 * Copyright (c) 2026 CK Tan
 * SPDX-License-Identifier: MIT
 */

/*
 * ty_small.h -- the operations of three small types: tid (a block and an
 * offset), pg_lsn (a 64-bit log position) and "char" (one byte). The cases
 * and the error texts follow PostgreSQL's tid.c, pg_lsn.c and char.c.
 */
#ifndef TY_SMALL_H
#define TY_SMALL_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ty.h"
#include "ty_hash.h"

/*
 * tid. The layout is PostgreSQL's ItemPointerData: the block number as two
 * 16-bit halves, high first, then the offset.
 */
typedef struct ty_tid_t {
    uint16_t block_hi;
    uint16_t block_lo;
    uint16_t offset;
} ty_tid_t;

static inline uint32_t ty_tid_block(const ty_tid_t* t) {
    return ((uint32_t)t->block_hi << 16) | t->block_lo;
}

static inline void ty_tid_set(ty_tid_t* t, uint32_t block, uint16_t offset) {
    t->block_hi = (uint16_t)(block >> 16);
    t->block_lo = (uint16_t)block;
    t->offset = offset;
}

/* Compare: the block, then the offset; negative, zero or positive. */
static inline int ty_tid_cmp(const ty_tid_t* a, const ty_tid_t* b) {
    uint32_t ba = ty_tid_block(a), bb = ty_tid_block(b);

    if (ba != bb) {
        return ba < bb ? -1 : 1;
    }
    if (a->offset != b->offset) {
        return a->offset < b->offset ? -1 : 1;
    }
    return 0;
}

/* The hash of the 6 bytes, PostgreSQL's. */
static inline uint32_t ty_tid_hash(const ty_tid_t* t) {
    return ty_hash_bytes_std((const uint8_t*)t, 6);
}
static inline uint64_t ty_tid_hash_extended(const ty_tid_t* t, uint64_t seed) {
    return ty_hash_bytes((const uint8_t*)t, 6, seed);
}

/* Read "(block,offset)"; TY_ERR_INVALID for anything else. */
int ty_tid_parse(const char* s, ty_tid_t* out);

/* Write "(block,offset)" and a NUL: at most 17 bytes. */
#define TY_TID_TEXT_MAX 18
void ty_tid_format(const ty_tid_t* t, char* out);

/*
 * pg_lsn. Read "X/Y": 1 to 8 hex digits, a slash, 1 to 8 hex digits;
 * TY_ERR_INVALID for anything else.
 */
int ty_lsn_parse(const char* s, uint64_t* out);

/* Write it as hex, upper case, with the NUL: at most 18 bytes. */
#define TY_LSN_TEXT_MAX 18
void ty_lsn_format(uint64_t lsn, char* out);

/*
 * "char". Read: a backslash and three octal digits is that byte; else the
 * first byte of the text, or 0 for an empty text; the len bytes at s.
 */
static inline int8_t ty_char_parse(const char* s, size_t len) {
    if (len == 4 && s[0] == '\\' && s[1] >= '0' && s[1] <= '7' && s[2] >= '0' &&
        s[2] <= '7' && s[3] >= '0' && s[3] <= '7') {
        return (int8_t)(((s[1] - '0') << 6) + ((s[2] - '0') << 3) +
                        (s[3] - '0'));
    }
    return len > 0 ? (int8_t)s[0] : 0;
}

/*
 * Write it: a byte of 0x80 or more as a backslash and three octal digits;
 * else the byte itself, and 0 as nothing. out has 5 bytes; the length is
 * returned and a NUL follows.
 */
static inline size_t ty_char_format(int8_t c, char* out) {
    uint8_t u = (uint8_t)c;

    if (u & 0x80) {
        out[0] = '\\';
        out[1] = (char)('0' + (u >> 6));
        out[2] = (char)('0' + ((u >> 3) & 7));
        out[3] = (char)('0' + (u & 7));
        out[4] = '\0';
        return 4;
    }
    out[0] = (char)u;
    out[1] = '\0';
    return u ? 1 : 0;
}

/* The order is of the byte as unsigned; the integer is of it as signed. */
static inline int ty_char_cmp(int8_t a, int8_t b) {
    return (uint8_t)a == (uint8_t)b ? 0 : (uint8_t)a < (uint8_t)b ? -1 : 1;
}

/* An int32 as "char": TY_ERR_RANGE if it does not fit a signed byte. */
static inline int ty_char_from_int32(int32_t v, int8_t* out) {
    if (v < -128 || v > 127) {
        return ty_err_range();
    }
    *out = (int8_t)v;
    return 0;
}

#endif

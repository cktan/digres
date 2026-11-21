/*
 * Copyright (c) 2026 CK Tan
 * SPDX-License-Identifier: MIT
 */

/*
 * ty_bits.h -- the operations of bit and bit varying: strings of bits.
 * Written for ty from the cases and the errors of PostgreSQL's varbit.c.
 *
 * A bit string is its bytes and its length in bits, nbits: the first bit is
 * the high bit of the first byte, and the bits of the last byte past nbits
 * are zero (an input must keep them zero, and every result does). The bytes
 * a string of nbits takes is ty_bits_bytes(nbits). A function that makes a
 * string writes it to the caller's buffer, which has room for the bytes of the
 * result: the call says how many, as with bytea, and ty allocates nothing.
 *
 * A call that can fail returns 0, or -1 with the thread's error set; its
 * output is then not set.
 */
#ifndef TY_BITS_H
#define TY_BITS_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ty.h"

/* The longest bit string: PostgreSQL's VARBITMAXLEN. */
#define TY_BITS_MAX_LEN (INT32_MAX - 7)

/* The bytes of a bit string of nbits bits. */
static inline size_t ty_bits_bytes(int32_t nbits) {
    return ((size_t)nbits + 7) / 8;
}

/*
 * Read the text of a bit string: the len bytes at s, as binary digits (with
 * a b or B before them, or none), or as hexadecimal digits after an x or X,
 * four bits to the digit. out needs ty_bits_parse_room(len) bytes. A digit
 * that is no digit is TY_ERR_INVALID, with PostgreSQL's message naming the
 * character; hex that would pass TY_BITS_MAX_LEN is TY_ERR_RANGE.
 */
#define ty_bits_parse_room(len) (((len) + 1) / 2 + 1)
int ty_bits_parse(const char* s, size_t len, uint8_t* out, int32_t* nbits);

/* Write the bits as the characters 0 and 1, and a NUL: nbits + 1 bytes. */
void ty_bits_format(const uint8_t* a, int32_t nbits, char* out);

/*
 * Compare: negative, zero or positive; the bytes first, and of two that
 * agree as far as the shorter goes, the shorter is the smaller.
 */
static inline int ty_bits_cmp_bytes(const uint8_t* a, size_t ba,
                                    const int32_t* na, const uint8_t* b,
                                    size_t bb, const int32_t* nb) {
    int c = memcmp(a, b, ba < bb ? ba : bb);

    /* the lengths are read only if the bytes tie: they are in a header */
    if (c == 0 && *na != *nb) {
        c = *na < *nb ? -1 : 1;
    }
    return c;
}
/*
 * The same, for a caller that has the bytes of each string at hand, and
 * the lengths where they can be read: pointers, so that they are read
 * after the compare of the bytes, and need no register across it.
 */
static inline int ty_bits_cmp(const uint8_t* a, int32_t na, const uint8_t* b,
                              int32_t nb) {
    return ty_bits_cmp_bytes(a, ty_bits_bytes(na), &na, b, ty_bits_bytes(nb),
                             &nb);
}
int ty_bits_eq(const uint8_t* a, int32_t na, const uint8_t* b, int32_t nb);

/* a, then b. TY_ERR_RANGE if it passes TY_BITS_MAX_LEN. */
int ty_bits_cat(const uint8_t* a, int32_t na, const uint8_t* b, int32_t nb,
                uint8_t* out, int32_t* nout);

/*
 * The part SUBSTRING(a FROM start FOR len) names; start is one-based, and
 * may be below 1; has_len is 0 for FROM alone. A len below 0 is
 * TY_ERR_INVALID ("negative substring length not allowed"). out has room for
 * the bytes of a.
 */
int ty_bits_substr(const uint8_t* a, int32_t na, int32_t start, int32_t len,
                   int has_len, uint8_t* out, int32_t* nout);

/*
 * OVERLAY(a PLACING b FROM start FOR len): a with len bits from start (one
 * based) replaced by b. A start below 1 is TY_ERR_INVALID; start + len past
 * int32 is TY_ERR_RANGE; the result is TY_ERR_RANGE past TY_BITS_MAX_LEN. out
 * has room for the bytes of na + nb bits.
 */
int ty_bits_overlay(const uint8_t* a, int32_t na, const uint8_t* b, int32_t nb,
                    int32_t start, int32_t len, uint8_t* out, int32_t* nout);

/*
 * The one-based place of the first copy of b in a, 0 if there is none; an
 * empty b is at 1 (and 0 in an empty a).
 */
int32_t ty_bits_position(const uint8_t* a, int32_t na, const uint8_t* b,
                         int32_t nb);

/* How many bits are 1. */
int64_t ty_bits_count(const uint8_t* a, int32_t nbits);

/*
 * AND, OR and XOR of two strings of one length n, and NOT of one: out has
 * the bytes of n bits. Strings of two lengths are TY_ERR_INVALID with
 * PostgreSQL's message ("cannot AND bit strings of different sizes").
 */
int ty_bits_and(const uint8_t* a, int32_t na, const uint8_t* b, int32_t nb,
                uint8_t* out);
int ty_bits_or(const uint8_t* a, int32_t na, const uint8_t* b, int32_t nb,
               uint8_t* out);
int ty_bits_xor(const uint8_t* a, int32_t na, const uint8_t* b, int32_t nb,
                uint8_t* out);
void ty_bits_not(const uint8_t* a, int32_t nbits, uint8_t* out);

/*
 * Shift by n places, the length kept and zeros shifted in: left for a positive
 * n, right for a negative one in shl, and the other way in shr.
 */
void ty_bits_shl(const uint8_t* a, int32_t nbits, int32_t n, uint8_t* out);
void ty_bits_shr(const uint8_t* a, int32_t nbits, int32_t n, uint8_t* out);

/*
 * The bit at index (from 0, at the left), and a copy with it set to bit.
 * TY_ERR_RANGE if the index is outside, with PostgreSQL's message ("bit
 * index %d out of valid range (0..%d)"); set also TY_ERR_INVALID for a bit that
 * is not 0 or 1. out has the bytes of the string.
 */
int ty_bits_get(const uint8_t* a, int32_t nbits, int32_t index, int32_t* r);
int ty_bits_set(const uint8_t* a, int32_t nbits, int32_t index, int32_t bit,
                uint8_t* out);

/*
 * An integer as a bit string of length len (1 if len is 0 or less, or past
 * TY_BITS_MAX_LEN): its two's complement, sign-extended or cut at the left. out
 * has the bytes of len bits. And a string of at most 32 (64) bits as an
 * integer: its bits as an unsigned number, which then becomes the signed type;
 * longer is TY_ERR_RANGE.
 */
void ty_bits_from_int32_slow(int32_t a, int32_t len, uint8_t* out,
                             int32_t* nout);
void ty_bits_from_int64_slow(int64_t a, int32_t len, uint8_t* out,
                             int32_t* nout);

/*
 * The common case, in line: a length of 1 to the width of the integer is
 * its low len bits, moved to the top; the bits under them are zero.
 */
static inline void ty_bits_from_int32(int32_t a, int32_t len, uint8_t* out,
                                      int32_t* nout) {
    if (len < 1 || len > 32) {
        ty_bits_from_int32_slow(a, len, out, nout);
        return;
    }
    uint32_t top = (uint32_t)a << (32 - len);
    size_t n = ty_bits_bytes(len);

    out[0] = (uint8_t)(top >> 24);
    if (n > 1) {
        out[1] = (uint8_t)(top >> 16);
    }
    if (n > 2) {
        out[2] = (uint8_t)(top >> 8);
    }
    if (n > 3) {
        out[3] = (uint8_t)top;
    }
    *nout = len;
}

static inline void ty_bits_from_int64(int64_t a, int32_t len, uint8_t* out,
                                      int32_t* nout) {
    if (len < 1 || len > 64) {
        ty_bits_from_int64_slow(a, len, out, nout);
        return;
    }
    uint64_t top = (uint64_t)a << (64 - len);
    size_t n = ty_bits_bytes(len);

    for (size_t i = 0; i < n; i++) {
        out[i] = (uint8_t)(top >> (56 - 8 * i));
    }
    *nout = len;
}
int ty_bits_to_int32(const uint8_t* a, int32_t nbits, int32_t* r);
int ty_bits_to_int64(const uint8_t* a, int32_t nbits, int64_t* r);

/* The string cut to len bits, or padded with zeros to len. out has room. */
void ty_bits_resize(const uint8_t* a, int32_t nbits, int32_t len,
                    uint8_t* out);

#endif /* TY_BITS_H */

/*
 * Copyright (c) 2026 CK Tan
 * SPDX-License-Identifier: MIT
 */

/*
 * ty_bytea.h -- the operations of bytea (SQL's byte string). Written for
 * ty; the forms it reads and writes, and the cases of each function, are
 * those of PostgreSQL's bytea.
 *
 * A bytea is a pointer to its bytes and a length. A function that makes
 * bytes writes them to a buffer of the caller's, whose size the caller can
 * tell before the call, from the size function of the operation or from the
 * length of its input. ty does not allocate for them, so a host puts the
 * result where it keeps its own, with no copy: digres writes it after the
 * 4 bytes of a varlena's header. (ty_set_alloc is for a result whose size
 * only ty can know; none of bytea's is.)
 *
 * A function that can fail returns 0, or -1 with the thread's error set,
 * and sets its output only on success.
 */
#ifndef TY_BYTEA_H
#define TY_BYTEA_H

#include <stddef.h>
#include <stdint.h>

#include "ty.h"

/*
 * Record the error of a negative SUBSTRING length (TY_ERR_INVALID), or of an
 * index outside the bytes (TY_ERR_RANGE, with PostgreSQL's message), and
 * return -1. They are cold, out of line, for the in-line functions below.
 */
int ty_bytea_err_substr_length(void);
int ty_bytea_err_index(long long index, long long limit);

/* ---- text ---- */

/*
 * Read the len bytes at s as the text of a bytea, into out, which has room
 * for len bytes; *outlen is how many it wrote. Two forms:
 *
 *   - "\x" and then pairs of hexadecimal digits, any case. A space, a tab,
 *     a newline or a return may stand between pairs, not inside one.
 *   - Escaped: a byte as itself; or as a backslash and three octal digits,
 *     the first 0 to 3; or a backslash as two.
 *
 * Another text is TY_ERR_INVALID, and the message says which fault: for
 * the hex form, an invalid digit, with the character, or an odd number of
 * digits; for the escaped form, a backslash not followed so.
 */
int ty_bytea_parse(const char* s, size_t len, uint8_t* out, size_t* outlen);

/* True if the len bytes at s begin the hex form: "\x". */
static inline int ty_bytea_is_hex_text(const char* s, size_t len) {
    return len >= 2 && s[0] == '\\' && s[1] == 'x';
}

/*
 * The text of n bytes in the hex form, "\x" and lowercase digits, is
 * ty_bytea_hex_size(n) characters; format_hex writes them, with no NUL.
 */
static inline size_t ty_bytea_hex_size(size_t n) { return 2 * n + 2; }
void ty_bytea_format_hex(const uint8_t* data, size_t n, char* out);

/*
 * The escaped form: a backslash is two characters, a byte below 0x20 or
 * above 0x7e is a backslash and three octal digits, any other is itself.
 * escape_size says how many characters that is, and format_escape writes
 * them, with no NUL.
 */
size_t ty_bytea_escape_size(const uint8_t* data, size_t n);
void ty_bytea_format_escape(const uint8_t* data, size_t n, char* out);

/* ---- compare and hash ---- */

/*
 * Negative, zero or positive as a is below, equal to or above b: the
 * bytes compare, and a prefix is below what it begins.
 */
int ty_bytea_cmp(const uint8_t* a, size_t na, const uint8_t* b, size_t nb);

/*
 * A 64-bit hash of the bytes by a seed; the low 32 bits with seed 0 are
 * the standard 32-bit hash. It cannot fail.
 */
uint64_t ty_bytea_hash(const uint8_t* data, size_t n, uint64_t seed);

/* ---- making bytes ---- */

/* out gets a, then b: na + nb bytes. */
void ty_bytea_cat(const uint8_t* a, size_t na, const uint8_t* b, size_t nb,
                  uint8_t* out);

/* out gets the bytes of a in the other order: n bytes. */
void ty_bytea_reverse(const uint8_t* a, size_t n, uint8_t* out);

/*
 * The part of a bytea that SUBSTRING(s FROM start FOR len) names: start is
 * one-based and may be below 1, and then the part is cut so that it ends
 * where start + len says. Sets *off, the zero-based place of its first
 * byte, and *count, how many bytes, or -1 for all that follow. They do not
 * depend on the length of the bytea; a host that has it clamps *count to
 * the length less *off, and a host that fetches part of a stored value
 * fetches those. has_len is 0 for FROM alone. A len below 0 is
 * TY_ERR_INVALID ("negative substring length not allowed").
 */
static inline int ty_bytea_substr_range(int32_t start, int32_t len,
                                        int has_len, int32_t* off,
                                        int32_t* count) {
    int32_t first = start > 1 ? start : 1;
    int32_t end;

    if (!has_len) {
        *off = first - 1;
        *count = -1;
        return 0;
    }
    if (TY_UNLIKELY(len < 0)) {
        return ty_bytea_err_substr_length();
    }
    if (__builtin_add_overflow(start, len, &end)) {
        /* the part runs past any length */
        *off = first - 1;
        *count = -1;
        return 0;
    }
    if (end < 1) {
        *off = 0;
        *count = 0;
        return 0;
    }
    *off = first - 1;
    *count = end - first;
    return 0;
}

/*
 * OVERLAY(a PLACING b FROM start FOR len): a, with the len bytes from
 * start (one-based) replaced by b. overlay_size gives the length of the
 * result, and overlay writes it to out, which has that room. A start below
 * 1 is TY_ERR_INVALID; start + len past int32 is TY_ERR_RANGE.
 */
int ty_bytea_overlay_size(size_t na, size_t nb, int32_t start, int32_t len,
                          size_t* size);
int ty_bytea_overlay(const uint8_t* a, size_t na, const uint8_t* b, size_t nb,
                     int32_t start, int32_t len, uint8_t* out);

/* ---- looking inside ---- */

/*
 * The one-based place of the first copy of b in a, or 0 if there is none;
 * an empty b is at 1.
 */
int64_t ty_bytea_position(const uint8_t* a, size_t na, const uint8_t* b,
                          size_t nb);

/* How many bits of the bytes are set. */
int64_t ty_bytea_bit_count(const uint8_t* a, size_t n);

/*
 * The byte at index (from 0), or the bit at index (from 0, the least
 * significant of byte 0 first). TY_ERR_RANGE if the index is outside; the
 * message names the valid range.
 */
static inline int ty_bytea_get_byte(const uint8_t* a, size_t n, int32_t index,
                                    int32_t* r) {
    if (TY_UNLIKELY(index < 0 || (size_t)index >= n)) {
        return ty_bytea_err_index(index, (long long)n);
    }
    *r = a[index];
    return 0;
}

static inline int ty_bytea_get_bit(const uint8_t* a, size_t n, int64_t index,
                                   int32_t* r) {
    if (TY_UNLIKELY(index < 0 || (uint64_t)index >= (uint64_t)n * 8)) {
        return ty_bytea_err_index(index, (long long)n * 8);
    }
    *r = (a[index / 8] >> (index % 8)) & 1;
    return 0;
}

/*
 * Set the byte at index to value (its low 8 bits), or the bit at index to
 * 0 or 1, in the n bytes at a, which the caller has copied. TY_ERR_RANGE
 * for an index outside; TY_ERR_INVALID for a bit that is not 0 or 1.
 */
int ty_bytea_set_byte(uint8_t* a, size_t n, int32_t index, int32_t value);
int ty_bytea_set_bit(uint8_t* a, size_t n, int64_t index, int32_t bit);

/* ---- and integers ---- */

/*
 * The integer of at most N bytes, most significant first, as an unsigned
 * number of the type's width, which then becomes the signed type: so the two
 * bytes 0xFFFF are -1 as an int16 and 65535 as an int32. More than N bytes
 * is TY_ERR_RANGE. The other way, ty_intN_to_bytea writes the N bytes of an
 * integer, most significant first, to out.
 */
int ty_bytea_to_int16(const uint8_t* a, size_t n, int16_t* r);
int ty_bytea_to_int32(const uint8_t* a, size_t n, int32_t* r);
int ty_bytea_to_int64(const uint8_t* a, size_t n, int64_t* r);
void ty_int16_to_bytea(int16_t v, uint8_t out[2]);
void ty_int32_to_bytea(int32_t v, uint8_t out[4]);
void ty_int64_to_bytea(int64_t v, uint8_t out[8]);

#endif /* TY_BYTEA_H */

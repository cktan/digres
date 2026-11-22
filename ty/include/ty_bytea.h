/*
 * Copyright (c) 2026 CK Tan
 * SPDX-License-Identifier: MIT
 */

/*
 * ty_bytea.h -- the operations of bytea (SQL's byte string). Written for
 * ty; the forms it reads and writes, and the cases of each function, are
 * those of PostgreSQL's bytea.
 *
 * A bytea is a varlena (ty_varlena.h): the host gives the value as it has
 * it, with a short or a long header, and not toasted. A function that makes
 * a bytea allocates it with the thread's alloc function (ty_set_alloc), and
 * gives it in *out, with a long header. A text that these functions read or
 * write, the hex form and the escaped form of a bytea, is a C string: a
 * pointer and a length with no NUL.
 *
 * A function that can fail returns 0, or -1 with the thread's error set,
 * and sets its output only on success. A function that makes a value fails
 * with TY_ERR_RANGE ("requested length too large") if the value is past
 * TY_VARLENA_MAX_LEN, and TY_ERR_NOMEM if the alloc function returns NULL.
 */
#ifndef TY_BYTEA_H
#define TY_BYTEA_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ty.h"
#include "ty_varlena.h"

/*
 * Record the error of a negative SUBSTRING length (TY_ERR_INVALID), or of an
 * index outside the bytes (TY_ERR_RANGE, with PostgreSQL's message), and
 * return -1. They are cold, out of line, for the in-line functions below.
 */
int ty_bytea_err_substr_length(void);
int ty_bytea_err_index(long long index, long long limit);

/* ---- text ---- */

/*
 * Read the len bytes at s as the text of a bytea. Two forms:
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
int ty_bytea_parse(const char *s, size_t len, ty_varlena_t **out);

/* True if the len bytes at s begin the hex form: "\x". */
static inline int ty_bytea_is_hex_text(const char *s, size_t len) {
  return len >= 2 && s[0] == '\\' && s[1] == 'x';
}

/*
 * The text of a bytea in the hex form, "\x" and lowercase digits, is
 * ty_bytea_hex_size characters; format_hex writes them to out, with no NUL.
 */
size_t ty_bytea_hex_size(const ty_varlena_t *a);
void ty_bytea_format_hex(const ty_varlena_t *a, char *out);

/*
 * The escaped form: a backslash is two characters, a byte below 0x20 or
 * above 0x7e is a backslash and three octal digits, any other is itself.
 * escape_size says how many characters that is, and format_escape writes
 * them, with no NUL.
 */
size_t ty_bytea_escape_size(const ty_varlena_t *a);
void ty_bytea_format_escape(const ty_varlena_t *a, char *out);

/* ---- encode and decode ---- */

/*
 * The encodings of ENCODE and DECODE: hex (two lowercase digits a byte),
 * base64 (a line feed after each 76 characters), and escape (a byte as
 * itself, but 0 and a byte of 0x80 or more as a backslash and three octal
 * digits, and a backslash as two).
 */
typedef enum ty_bytea_enc_t {
  TY_BYTEA_ENC_HEX,
  TY_BYTEA_ENC_BASE64,
  TY_BYTEA_ENC_ESCAPE
} ty_bytea_enc_t;

/*
 * The encoding a name stands for, in any case. An unknown name is
 * TY_ERR_INVALID with the message unrecognized encoding: "name".
 */
int ty_bytea_encoding(const char *name, ty_bytea_enc_t *enc);

/*
 * ENCODE: the text, a varlena of the bytes of the encoding of data, with no
 * NUL.
 */
int ty_bytea_encode(ty_bytea_enc_t enc, const ty_varlena_t *data,
                    ty_varlena_t **out);

/*
 * DECODE: the bytea of the encoded text. A fault is TY_ERR_INVALID, with
 * PostgreSQL's message: for hex an invalid digit or an odd number of digits;
 * for base64 an unexpected "=", an invalid symbol, or "invalid base64 end
 * sequence"; for escape a stray backslash.
 */
int ty_bytea_decode(ty_bytea_enc_t enc, const ty_varlena_t *text,
                    ty_varlena_t **out);

/* ---- compare and hash ---- */

/*
 * Negative, zero or positive as a is below, equal to or above b: the
 * bytes compare, and a prefix is below what it begins.
 */
static inline int ty_bytea_cmp(const ty_varlena_t *a, const ty_varlena_t *b) {
  size_t na = ty_vl_len(a);
  size_t nb = ty_vl_len(b);
  size_t n = na < nb ? na : nb;
  int c = n > 0 ? memcmp(ty_vl_data(a), ty_vl_data(b), n) : 0;

  if (c == 0 && na != nb) {
    return na < nb ? -1 : 1;
  }
  return c;
}

/* The predicates of the compare, 1 or 0; eq is false at once for lengths. */
static inline int ty_bytea_eq(const ty_varlena_t *a, const ty_varlena_t *b) {
  size_t na = ty_vl_len(a);

  return na == ty_vl_len(b) &&
         (na == 0 || memcmp(ty_vl_data(a), ty_vl_data(b), na) == 0);
}
static inline int ty_bytea_ne(const ty_varlena_t *a, const ty_varlena_t *b) {
  return !ty_bytea_eq(a, b);
}
static inline int ty_bytea_lt(const ty_varlena_t *a, const ty_varlena_t *b) {
  return ty_bytea_cmp(a, b) < 0;
}
static inline int ty_bytea_le(const ty_varlena_t *a, const ty_varlena_t *b) {
  return ty_bytea_cmp(a, b) <= 0;
}
static inline int ty_bytea_gt(const ty_varlena_t *a, const ty_varlena_t *b) {
  return ty_bytea_cmp(a, b) > 0;
}
static inline int ty_bytea_ge(const ty_varlena_t *a, const ty_varlena_t *b) {
  return ty_bytea_cmp(a, b) >= 0;
}

/*
 * A 64-bit hash of the bytes by a seed; the low 32 bits with seed 0 are
 * the standard 32-bit hash. It cannot fail.
 */
uint64_t ty_bytea_hash(const ty_varlena_t *a, uint64_t seed);

/* ---- making bytes ---- */

/* a, then b. */
static inline int ty_bytea_cat(const ty_varlena_t *a, const ty_varlena_t *b,
                               ty_varlena_t **out) {
  size_t na = ty_vl_len(a);
  size_t nb = ty_vl_len(b);
  char *d;

  /* no sum can overflow: each is below 2^30 */
  if (ty_vl_new(na + nb, out, &d) != 0) {
    return -1;
  }
  memcpy(d, ty_vl_data(a), na);
  memcpy(d + na, ty_vl_data(b), nb);
  return 0;
}

/* The bytes of a in the other order. */
int ty_bytea_reverse(const ty_varlena_t *a, ty_varlena_t **out);

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
static inline int ty_bytea_substr_range(int32_t start, int32_t len, int has_len,
                                        int32_t *off, int32_t *count) {
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
 * start (one-based) replaced by b. A start below 1 is TY_ERR_INVALID;
 * start + len past int32 is TY_ERR_RANGE ("integer out of range").
 */
int ty_bytea_overlay(const ty_varlena_t *a, const ty_varlena_t *b,
                     int32_t start, int32_t len, ty_varlena_t **out);

/* ---- looking inside ---- */

/*
 * The one-based place of the first copy of b in a, or 0 if there is none;
 * an empty b is at 1.
 */
int64_t ty_bytea_position(const ty_varlena_t *a, const ty_varlena_t *b);

/* How many bits of the bytes are set. */
int64_t ty_bytea_bit_count(const ty_varlena_t *a);

/*
 * The byte at index (from 0), or the bit at index (from 0, the least
 * significant of byte 0 first). TY_ERR_RANGE if the index is outside; the
 * message names the valid range.
 */
static inline int ty_bytea_get_byte(const ty_varlena_t *a, int32_t index,
                                    int32_t *r) {
  size_t n = ty_vl_len(a);

  if (TY_UNLIKELY(index < 0 || (size_t)index >= n)) {
    return ty_bytea_err_index(index, (long long)n);
  }
  *r = (unsigned char)ty_vl_data(a)[index];
  return 0;
}

static inline int ty_bytea_get_bit(const ty_varlena_t *a, int64_t index,
                                   int32_t *r) {
  size_t n = ty_vl_len(a);

  if (TY_UNLIKELY(index < 0 || (uint64_t)index >= (uint64_t)n * 8)) {
    return ty_bytea_err_index(index, (long long)n * 8);
  }
  *r = ((unsigned char)ty_vl_data(a)[index / 8] >> (index % 8)) & 1;
  return 0;
}

/*
 * A copy of a with the byte at index set to value (its low 8 bits), or the
 * bit at index set to 0 or 1. TY_ERR_RANGE for an index outside;
 * TY_ERR_INVALID for a bit that is not 0 or 1.
 */
int ty_bytea_set_byte(const ty_varlena_t *a, int32_t index, int32_t value,
                      ty_varlena_t **out);
int ty_bytea_set_bit(const ty_varlena_t *a, int64_t index, int32_t bit,
                     ty_varlena_t **out);

/* ---- and integers ---- */

/*
 * The integer of at most N bytes, most significant first, as an unsigned
 * number of the type's width, which then becomes the signed type: so the two
 * bytes 0xFFFF are -1 as an int16 and 65535 as an int32. More than N bytes
 * is TY_ERR_RANGE. The other way, ty_intN_to_bytea makes the bytea of the N
 * bytes of an integer, most significant first.
 */
int ty_bytea_to_int16(const ty_varlena_t *a, int16_t *r);
int ty_bytea_to_int32(const ty_varlena_t *a, int32_t *r);
int ty_bytea_to_int64(const ty_varlena_t *a, int64_t *r);
int ty_int16_to_bytea(int16_t v, ty_varlena_t **out);
int ty_int32_to_bytea(int32_t v, ty_varlena_t **out);
int ty_int64_to_bytea(int64_t v, ty_varlena_t **out);

#endif /* TY_BYTEA_H */

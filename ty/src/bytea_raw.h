/*
 * Copyright (c) 2026 CK Tan
 * SPDX-License-Identifier: MIT
 */

/*
 * bytea_raw.h -- the operations of bytea, on bytes and a length (the public
 * ones, on a varlena, are in ty_bytea.h and call these) (SQL's byte string).
 * Written for ty; the forms it reads and writes, and the cases of each
 * function, are those of PostgreSQL's bytea.
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
#ifndef TY_SRC_BYTEA_RAW_H
#define TY_SRC_BYTEA_RAW_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ty.h"
#include "ty_bytea.h"

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
int tyr_bytea_parse(const char *s, size_t len, uint8_t *out, size_t *outlen);

/* True if the len bytes at s begin the hex form: "\x". */
static inline int tyr_bytea_is_hex_text(const char *s, size_t len) {
  return len >= 2 && s[0] == '\\' && s[1] == 'x';
}

/*
 * The text of n bytes in the hex form, "\x" and lowercase digits, is
 * tyr_bytea_hex_size(n) characters; format_hex writes them, with no NUL.
 */
static inline size_t tyr_bytea_hex_size(size_t n) { return 2 * n + 2; }
void tyr_bytea_format_hex(const uint8_t *data, size_t n, char *out);

/*
 * The escaped form: a backslash is two characters, a byte below 0x20 or
 * above 0x7e is a backslash and three octal digits, any other is itself.
 * escape_size says how many characters that is, and format_escape writes
 * them, with no NUL.
 */
size_t tyr_bytea_escape_size(const uint8_t *data, size_t n);
void tyr_bytea_format_escape(const uint8_t *data, size_t n, char *out);

/* ---- encode and decode ---- */

/*
 * The encodings of ENCODE and DECODE: hex (two lowercase digits a byte),
 * base64 (a line feed after each 76 characters), and escape (a byte as
 * itself, but 0 and a byte of 0x80 or more as a backslash and three octal
 * digits, and a backslash as two).
 */

/*
 * The encoding a name stands for, in any case. An unknown name is
 * TY_ERR_INVALID with the message unrecognized encoding: "name".
 */
int tyr_bytea_encoding(const char *name, ty_bytea_enc_t *enc);

/*
 * The text of n bytes in an encoding is at most encode_size characters;
 * encode writes it, with no NUL, to out of that size, and returns its length.
 */
size_t tyr_bytea_encode_size(ty_bytea_enc_t enc, const uint8_t *data, size_t n);
size_t tyr_bytea_encode(ty_bytea_enc_t enc, const uint8_t *data, size_t n,
                        char *out);

/*
 * The bytes of the n characters of text at s are at most decode_size: out
 * needs that room. decode writes them, sets *outlen to how many, and returns
 * 0. A fault is TY_ERR_INVALID, with PostgreSQL's message: for hex an
 * invalid digit or an odd number of digits; for base64 an unexpected "=", an
 * invalid symbol, or "invalid base64 end sequence"; for escape a stray
 * backslash. decode_size fails the same way for escape, which it must scan.
 */
int tyr_bytea_decode_size(ty_bytea_enc_t enc, const char *s, size_t n,
                          size_t *size);
int tyr_bytea_decode(ty_bytea_enc_t enc, const char *s, size_t n, uint8_t *out,
                     size_t *outlen);

/* ---- compare and hash ---- */

/*
 * Negative, zero or positive as a is below, equal to or above b: the
 * bytes compare, and a prefix is below what it begins.
 */
static inline int tyr_bytea_cmp(const uint8_t *a, size_t na, const uint8_t *b,
                                size_t nb) {
  size_t n = na < nb ? na : nb;
  int c = n > 0 ? memcmp(a, b, n) : 0;

  if (c == 0 && na != nb) {
    return na < nb ? -1 : 1;
  }
  return c;
}

/* The predicates of the compare, 1 or 0; eq is false at once for lengths. */
static inline int tyr_bytea_eq(const uint8_t *a, size_t na, const uint8_t *b,
                               size_t nb) {
  return na == nb && (na == 0 || memcmp(a, b, na) == 0);
}
static inline int tyr_bytea_ne(const uint8_t *a, size_t na, const uint8_t *b,
                               size_t nb) {
  return !tyr_bytea_eq(a, na, b, nb);
}
static inline int tyr_bytea_lt(const uint8_t *a, size_t na, const uint8_t *b,
                               size_t nb) {
  return tyr_bytea_cmp(a, na, b, nb) < 0;
}
static inline int tyr_bytea_le(const uint8_t *a, size_t na, const uint8_t *b,
                               size_t nb) {
  return tyr_bytea_cmp(a, na, b, nb) <= 0;
}
static inline int tyr_bytea_gt(const uint8_t *a, size_t na, const uint8_t *b,
                               size_t nb) {
  return tyr_bytea_cmp(a, na, b, nb) > 0;
}
static inline int tyr_bytea_ge(const uint8_t *a, size_t na, const uint8_t *b,
                               size_t nb) {
  return tyr_bytea_cmp(a, na, b, nb) >= 0;
}

/*
 * A 64-bit hash of the bytes by a seed; the low 32 bits with seed 0 are
 * the standard 32-bit hash. It cannot fail.
 */
uint64_t tyr_bytea_hash(const uint8_t *data, size_t n, uint64_t seed);

/* ---- making bytes ---- */

/* out gets a, then b: na + nb bytes. */
void tyr_bytea_cat(const uint8_t *a, size_t na, const uint8_t *b, size_t nb,
                   uint8_t *out);

/* out gets the bytes of a in the other order: n bytes. */
void tyr_bytea_reverse(const uint8_t *a, size_t n, uint8_t *out);

/*
 * OVERLAY(a PLACING b FROM start FOR len): a, with the len bytes from
 * start (one-based) replaced by b. overlay_size gives the length of the
 * result, and overlay writes it to out, which has that room. A start below
 * 1 is TY_ERR_INVALID; start + len past int32 is TY_ERR_RANGE.
 */
int tyr_bytea_overlay_size(size_t na, size_t nb, int32_t start, int32_t len,
                           size_t *size);
int tyr_bytea_overlay(const uint8_t *a, size_t na, const uint8_t *b, size_t nb,
                      int32_t start, int32_t len, uint8_t *out);

/* ---- looking inside ---- */

/*
 * The one-based place of the first copy of b in a, or 0 if there is none;
 * an empty b is at 1.
 */
int64_t tyr_bytea_position(const uint8_t *a, size_t na, const uint8_t *b,
                           size_t nb);

/* How many bits of the bytes are set. */
int64_t tyr_bytea_bit_count(const uint8_t *a, size_t n);

/*
 * btrim, ltrim and rtrim: the part of the n bytes at a that is left when
 * the bytes in the m bytes at set are taken from its start (left) and its
 * end (right): its place *off and its length *len.
 */
void tyr_bytea_trim(const uint8_t *a, size_t n, const uint8_t *set, size_t m,
                    int left, int right, size_t *off, size_t *len);

/* get_byte and get_bit are in ty_bytea.h. */

/*
 * Set the byte at index to value (its low 8 bits), or the bit at index to
 * 0 or 1, in the n bytes at a, which the caller has copied. TY_ERR_RANGE
 * for an index outside; TY_ERR_INVALID for a bit that is not 0 or 1.
 */
int tyr_bytea_set_byte(uint8_t *a, size_t n, int32_t index, int32_t value);
int tyr_bytea_set_bit(uint8_t *a, size_t n, int64_t index, int32_t bit);

/* ---- and integers ---- */

/*
 * The integer of at most N bytes, most significant first, as an unsigned
 * number of the type's width, which then becomes the signed type: so the two
 * bytes 0xFFFF are -1 as an int16 and 65535 as an int32. More than N bytes
 * is TY_ERR_RANGE. The other way, ty_intN_to_bytea writes the N bytes of an
 * integer, most significant first, to out.
 */
int tyr_bytea_to_int16(const uint8_t *a, size_t n, int16_t *r);
int tyr_bytea_to_int32(const uint8_t *a, size_t n, int32_t *r);
int tyr_bytea_to_int64(const uint8_t *a, size_t n, int64_t *r);
void tyr_int16_to_bytea(int16_t v, uint8_t out[2]);
void tyr_int32_to_bytea(int32_t v, uint8_t out[4]);
void tyr_int64_to_bytea(int64_t v, uint8_t out[8]);

#endif /* TY_SRC_BYTEA_RAW_H */

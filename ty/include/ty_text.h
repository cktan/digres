/*
 * Copyright (c) 2026 CK Tan
 * SPDX-License-Identifier: MIT
 */

/*
 * ty_text.h -- the operations of text: strings of UTF-8, ordered by their
 * bytes (the C collation). The cases and the errors follow PostgreSQL's
 * varlena.c and oracle_compat.c. The input is valid UTF-8: the host checks
 * it when the text comes in, as PostgreSQL does. A function never reads past
 * its string, whatever the bytes.
 *
 * A text, and every character(n) and character varying(n), is a varlena
 * (ty_varlena.h), as PostgreSQL has it: the host gives the value as it has
 * it, with a short or a long header, and not toasted. A function that makes
 * a text allocates it with the thread's alloc function (ty_set_alloc), and
 * gives it in *out, with a long header. A position or a count is in
 * characters (code points), as SQL says.
 *
 * A call that can fail returns 0, or -1 with the thread's error set; its
 * output is then not set. The message is PostgreSQL's. The kinds are:
 * TY_ERR_INVALID for a parameter that is wrong, TY_ERR_RANGE for a limit,
 * TY_ERR_NOMEM if the alloc function returns NULL.
 *
 * A function that may have nothing to change (replace, character(n)) can
 * give its input as the result: *out is then the input itself, not a copy.
 */
#ifndef TY_TEXT_H
#define TY_TEXT_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ty.h"
#include "ty_hash.h"
#include "ty_varlena.h"

#define TY_TEXT_MAX_LEN TY_VARLENA_MAX_LEN

/*
 * Compare, as bytes; a string that is a prefix of the other is the
 * smaller: negative, zero or positive. ty_text_cmp_bytes is for two strings
 * that are not varlenas, a pointer and a length each.
 */
static inline int ty_text_cmp_bytes(const char *a, size_t alen, const char *b,
                                    size_t blen) {
  int c = memcmp(a, b, alen < blen ? alen : blen);

  if (c == 0 && alen != blen) {
    c = alen < blen ? -1 : 1;
  }
  return c;
}

static inline int ty_text_cmp(const ty_varlena_t *a, const ty_varlena_t *b) {
  return ty_text_cmp_bytes(ty_vl_data(a), ty_vl_len(a), ty_vl_data(b),
                           ty_vl_len(b));
}

/* Equal, as bytes; the _bytes form is for strings that are not varlenas. */
static inline int ty_text_eq_bytes(const char *a, size_t alen, const char *b,
                                   size_t blen) {
  return alen == blen && memcmp(a, b, alen) == 0;
}

static inline int ty_text_eq(const ty_varlena_t *a, const ty_varlena_t *b) {
  return ty_text_eq_bytes(ty_vl_data(a), ty_vl_len(a), ty_vl_data(b),
                          ty_vl_len(b));
}

/*
 * s as a literal of SQL: in quotes, with each quote and backslash doubled,
 * and an E before it if it has a backslash, so that it is read the same
 * whatever standard_conforming_strings is. The bytes are written to out,
 * which has room for 2 * len + 3, and their count is returned; the second
 * form makes a text.
 */
size_t ty_quote_literal_bytes(const char *s, size_t len, char *out);
int ty_quote_literal(const ty_varlena_t *s, ty_varlena_t **out);

/* Whether the string begins with the prefix. */
static inline int ty_text_starts_with(const ty_varlena_t *s,
                                      const ty_varlena_t *prefix) {
  size_t plen = ty_vl_len(prefix);

  return plen <= ty_vl_len(s) &&
         memcmp(ty_vl_data(s), ty_vl_data(prefix), plen) == 0;
}

/* The hash of the bytes, PostgreSQL's. */
static inline uint32_t ty_text_hash(const ty_varlena_t *s) {
  return ty_hash_bytes_std((const uint8_t *)ty_vl_data(s), ty_vl_len(s));
}
static inline uint64_t ty_text_hash_extended(const ty_varlena_t *s,
                                             uint64_t seed) {
  return ty_hash_bytes((const uint8_t *)ty_vl_data(s), ty_vl_len(s), seed);
}

/*
 * How many characters the string has; the _bytes form is for a string that
 * is not a varlena.
 */
size_t ty_text_length(const ty_varlena_t *s);
size_t ty_text_length_bytes(const char *s, size_t len);

/*
 * a || b. TY_ERR_RANGE ("requested length too large") if the result passes
 * TY_VARLENA_MAX_LEN.
 */
static inline int ty_text_concat(const ty_varlena_t *a, const ty_varlena_t *b,
                                 ty_varlena_t **out) {
  size_t alen = ty_vl_len(a);
  size_t blen = ty_vl_len(b);
  char *d;

  /* no sum can overflow: each is below 2^30 */
  if (ty_vl_new(alen + blen, out, &d) != 0) {
    return -1;
  }
  memcpy(d, ty_vl_data(a), alen);
  memcpy(d + alen, ty_vl_data(b), blen);
  return 0;
}

/*
 * SUBSTRING(s FROM start FOR count): the characters from start (one-based,
 * and may be below 1) up to count of them; has_count is 0 for FROM alone.
 * A negative count is TY_ERR_INVALID ("negative substring length not
 * allowed").
 */
int ty_text_substr(const ty_varlena_t *s, int32_t start, int32_t count,
                   int has_count, ty_varlena_t **out);

/*
 * left(s, n) and right(s, n), by the rules of PostgreSQL: a negative n is
 * all but the last (left) or the first (right) -n characters.
 */
int ty_text_left(const ty_varlena_t *s, int32_t n, ty_varlena_t **out);
int ty_text_right(const ty_varlena_t *s, int32_t n, ty_varlena_t **out);

/* s reversed by characters. */
int ty_text_reverse(const ty_varlena_t *s, ty_varlena_t **out);

/*
 * OVERLAY(s PLACING r FROM start FOR count): s with count characters from
 * start replaced by r. A start of 0 or less is TY_ERR_INVALID ("negative
 * substring length not allowed"); start + count past int32 is TY_ERR_RANGE
 * ("integer out of range").
 */
int ty_text_overlay(const ty_varlena_t *s, const ty_varlena_t *r, int32_t start,
                    int32_t count, ty_varlena_t **out);

/*
 * The one-based character place of the first copy of needle in s, or 0 if
 * there is none; an empty needle is at 1.
 */
int32_t ty_text_position(const ty_varlena_t *s, const ty_varlena_t *needle);

/*
 * replace(s, from, to): every copy of from replaced. TY_ERR_RANGE ("string
 * buffer exceeds maximum allowed length (1073741823 bytes)") if the result
 * is too long. If there is nothing to replace, *out is s itself.
 */
int ty_text_replace(const ty_varlena_t *s, const ty_varlena_t *from,
                    const ty_varlena_t *to, ty_varlena_t **out);

/*
 * split_part(s, sep, n): the n-th field, from the end if n is negative. n of
 * 0 is TY_ERR_INVALID ("field position must not be zero").
 */
int ty_text_split_part(const ty_varlena_t *s, const ty_varlena_t *sep,
                       int32_t n, ty_varlena_t **out);

/*
 * repeat(s, n). A result past TY_VARLENA_MAX_LEN is TY_ERR_RANGE
 * ("requested length too large").
 */
int ty_text_repeat(const ty_varlena_t *s, int32_t count, ty_varlena_t **out);

/*
 * lpad(s, n, fill) and rpad(s, n, fill): s cut or padded to n characters,
 * the fill used again and again; with an empty fill s is only cut.
 * PostgreSQL's rule is that 4 bytes a character of the result are within
 * the limit: TY_ERR_RANGE ("requested length too large").
 */
int ty_text_lpad(const ty_varlena_t *s, int32_t n, const ty_varlena_t *fill,
                 ty_varlena_t **out);
int ty_text_rpad(const ty_varlena_t *s, int32_t n, const ty_varlena_t *fill,
                 ty_varlena_t **out);

/*
 * btrim, ltrim and rtrim: the characters that are in set taken from the
 * ends.
 */
int ty_text_trim(const ty_varlena_t *s, const ty_varlena_t *set, int left,
                 int right, ty_varlena_t **out);

/*
 * translate(s, from, to): each character of s that is in from becomes the
 * character of to at the same place, or nothing if to is shorter. 4 bytes a
 * character of s must be within the limit (TY_ERR_RANGE). If s is empty,
 * *out is s itself.
 */
int ty_text_translate(const ty_varlena_t *s, const ty_varlena_t *from,
                      const ty_varlena_t *to, ty_varlena_t **out);

/*
 * ascii(s): the code point of the first character, 0 for an empty string.
 * A first byte that begins no character is TY_ERR_INVALID.
 */
int ty_text_ascii(const ty_varlena_t *s, int32_t *out);

/*
 * chr(n): the character of code point n. A negative n is TY_ERR_INVALID
 * ("character number must be positive"); 0, a point past 0x10FFFF, and a
 * surrogate are TY_ERR_RANGE.
 */
int ty_text_chr(int32_t n, ty_varlena_t **out);

/*
 * character(n) and character varying(n). Fit s to maxchars characters: if it
 * has more, the part kept is cut off, and a character cut off that is not
 * a space is TY_ERR_RANGE ("value too long") unless force. If s has fewer
 * characters, character(n) (pad is 1) has spaces added to make n of them. If
 * s is as it should be, *out is s itself.
 */
int ty_bpchar_fit(const ty_varlena_t *s, size_t maxchars, int force, int pad,
                  ty_varlena_t **out);

/*
 * The same for a value that comes in as a C string of len bytes, which is
 * not a varlena: *out is always a new value.
 */
int ty_bpchar_in(const char *s, size_t len, size_t maxchars, int force, int pad,
                 ty_varlena_t **out);

/*
 * The length of a character(n) value without its trailing spaces; the _bytes
 * form is for a string that is not a varlena.
 */
static inline size_t ty_bpchar_truelen_bytes(const char *d, size_t len) {
  while (len > 0 && d[len - 1] == ' ') {
    len--;
  }
  return len;
}
static inline size_t ty_bpchar_truelen(const ty_varlena_t *s) {
  return ty_bpchar_truelen_bytes(ty_vl_data(s), ty_vl_len(s));
}

/*
 * The order and equality of two character(n) values: the C order of the
 * values without trailing spaces.
 */
int ty_bpchar_cmp(const ty_varlena_t *a, const ty_varlena_t *b);
int ty_bpchar_eq(const ty_varlena_t *a, const ty_varlena_t *b);

/* The hash of the value without its trailing spaces. */
uint32_t ty_bpchar_hash(const ty_varlena_t *s);
uint64_t ty_bpchar_hash_extended(const ty_varlena_t *s, uint64_t seed);

/*
 * name: NAMEDATALEN bytes, a NUL-ended string cut to 63 bytes at a character
 * boundary; it is not a varlena. Write the name from the text s to out
 * (zero-filled; for character(n), with the trailing spaces of s taken off
 * if trim is 1); or from the C string s of len bytes. The order is of the
 * bytes up to the NUL.
 */
#define TY_NAMEDATALEN 64
void ty_name_from_text(const ty_varlena_t *s, int trim, char *out);
void ty_name_parse(const char *s, size_t len, char *out);
static inline int ty_name_cmp(const char *a, const char *b) {
  return strncmp(a, b, TY_NAMEDATALEN);
}

/*
 * LIKE. Match t against the pattern p: % is any run of characters, _ is one
 * character, and a backslash makes the next byte plain. 1 for a match, 0 for
 * none. A pattern that ends in a backslash where the match reaches it is
 * TY_ERR_INVALID ("LIKE pattern must not end with escape character"), as
 * in PostgreSQL, which finds it only if it gets there; a pattern with so many
 * % that the nesting passes 5000 is TY_ERR_RANGE ("stack depth limit
 * exceeded"). utf8 is 1 for text, where _ and % step over a whole character,
 * and 0 for bytea, where they step over a byte. A match with the bytes of a
 * text that is not a varlena is ty_text_like_bytes.
 */
int ty_text_like(const ty_varlena_t *t, const ty_varlena_t *p, int utf8);
int ty_text_like_bytes(const char *t, size_t tlen, const char *p, size_t plen,
                       int utf8);

/*
 * The pattern of LIKE ... ESCAPE esc, made into the pattern of the default
 * escape, a backslash. An escape of more than one character is
 * TY_ERR_INVALID ("invalid escape string"). An empty escape makes every
 * backslash plain. utf8 is as in ty_text_like: a character is a whole UTF-8
 * character, or a byte.
 */
int ty_text_like_escape(const ty_varlena_t *pat, const ty_varlena_t *esc,
                        int utf8, ty_varlena_t **out);

/*
 * format(fmt, args...), as PostgreSQL's. Only the host has the arguments and
 * their text, so the host drives the call:
 * 1. ty_format_begin takes the format string.
 * 2. ty_format_next adds the text up to the next conversion (%s, %I or %L)
 *    to the result, a % for each %%, and gives the conversion: 1, or 0 at
 *    the end of the format.
 * 3. The host takes the width argument, if the conversion has one, and the
 *    argument; makes its text; quotes it for %I and %L; and gives it to
 *    ty_format_put, which pads it to the width and adds it.
 * 4. ty_format_finish gives the result, a text.
 *
 * The errors of ty_format_next, all TY_ERR_INVALID but the last:
 * "unterminated format() type specifier", "unrecognized format() type
 * specifier "c"", "format specifies argument 0, but arguments are numbered
 * from 1", "width argument position must be ended by "$"", and
 * TY_ERR_RANGE "number is out of range" for a number past int32.
 *
 * On an error, ty frees the result it was making. A host that stops between
 * the calls for an error of its own calls ty_format_abort, unless its memory
 * goes all at once, as a memory context's or an arena's does.
 */
typedef struct ty_format_t {
  const char *p;   /* the next byte of the format string */
  const char *end; /* the end of the format string */
  char *buf;       /* the result: 4 bytes for its header, then its text */
  size_t len;      /* the bytes of text in buf */
  size_t cap;      /* the bytes of text buf has room for */
  /*
   * On the error of the limit, PostgreSQL's StringInfo's "string buffer
   * exceeds maximum allowed length": the bytes there were, and the bytes
   * asked for, for the host's detail.
   */
  size_t had;
  size_t need;
} ty_format_t;

/* A conversion: %[argpos$][-][width or *[widthpos$]]conv. */
typedef struct ty_format_spec_t {
  char conv;        /* 's', 'I' or 'L' */
  int minus;        /* the '-' flag: justify to the left */
  int32_t argpos;   /* the argument's place, from 1; 0 for the next */
  int32_t widthpos; /* the width's: -1 for none, 0 for the next argument */
  int32_t width;    /* the width written in the format; 0 for none */
} ty_format_spec_t;

void ty_format_begin(ty_format_t *f, const char *fmt, size_t len);
int ty_format_next(ty_format_t *f, ty_format_spec_t *spec);

/*
 * Add s, of len bytes, padded with spaces to width characters: on the left,
 * or on the right if minus is 1 or width is negative. A width of INT32_MIN
 * is TY_ERR_RANGE ("number is out of range"); a result past 1073741822
 * bytes is TY_ERR_RANGE ("string buffer exceeds maximum allowed length
 * (1073741823 bytes)"), with f->had and f->need set; one past
 * TY_VARLENA_MAX_LEN is TY_ERR_RANGE ("requested length too large").
 * The case of no width and room for the text is in line; ty_format_put_full
 * is the whole of it.
 */
int ty_format_put_full(ty_format_t *f, const char *s, size_t len, int minus,
                       int32_t width);
static inline int ty_format_put(ty_format_t *f, const char *s, size_t len,
                                int minus, int32_t width) {
  if (0 == width && f->buf != NULL && f->cap - f->len >= len) {
    memcpy(f->buf + 4 + f->len, s, len);
    f->len += len;
    return 0;
  }
  return ty_format_put_full(f, s, len, minus, width);
}
int ty_format_finish(ty_format_t *f, ty_varlena_t **out);
void ty_format_abort(ty_format_t *f);

/*
 * The state of string_agg(value, delimiter), over text and over bytea: the
 * bytes of the values, each after its delimiter. The state keeps the first
 * value's delimiter too, so that the states of a parallel aggregate join up;
 * ty_string_agg_finish cuts it off. A caller zeroes the state, and passes
 * only the values that are not NULL; a NULL delimiter is d NULL and dlen 0.
 *
 * The bytes are the alloc function's. A host whose alloc function is a memory
 * context's makes the aggregate's context current around each call that adds
 * to a state.
 *
 * The errors: a state of 1073741823 bytes or more is TY_ERR_RANGE ("string
 * buffer exceeds maximum allowed length (1073741823 bytes)"), PostgreSQL's
 * StringInfo's, with had and need set for the host's detail; TY_ERR_NOMEM if
 * the alloc function returns NULL. On an error, the state is as it was.
 */
typedef struct ty_string_agg_t {
  char *buf;    /* the bytes: NULL until the first */
  size_t len;   /* the bytes in buf */
  size_t cap;   /* the bytes buf has room for */
  size_t first; /* the bytes of the first value's delimiter */
  int started;  /* a value, or a state, has entered */
  /* On the error of the limit: the bytes there were, and the bytes asked. */
  size_t had;
  size_t need;
} ty_string_agg_t;

/*
 * Add the delimiter d and the value v. The case of a state that has begun and
 * has room is in line; ty_string_agg_add_full is the whole of it.
 */
int ty_string_agg_add_full(ty_string_agg_t *st, const char *d, size_t dlen,
                           const char *v, size_t vlen);
static inline int ty_string_agg_add(ty_string_agg_t *st, const char *d,
                                    size_t dlen, const char *v, size_t vlen) {
  if (st->started && st->cap - st->len >= dlen + vlen) {
    if (0 != dlen) {
      memcpy(st->buf + st->len, d, dlen);
    }
    if (0 != vlen) {
      memcpy(st->buf + st->len + dlen, v, vlen);
    }
    st->len += dlen + vlen;
    return 0;
  }
  return ty_string_agg_add_full(st, d, dlen, v, vlen);
}

/*
 * Add the state b after the state a: the parts of a parallel aggregate. A
 * zeroed a takes b's first delimiter. b must have begun.
 */
int ty_string_agg_combine(ty_string_agg_t *a, const ty_string_agg_t *b);

/*
 * The state as a bytea, for a parallel aggregate's worker: the first
 * delimiter's length in 4 bytes, most significant first, then the bytes, as
 * PostgreSQL's string_agg_serialize.
 */
int ty_string_agg_serialize(const ty_string_agg_t *st, ty_varlena_t **out);

/*
 * Make the zeroed state st from the len bytes at p, which
 * ty_string_agg_serialize made. Fewer than 4 bytes is TY_ERR_INVALID
 * ("insufficient data left in message").
 */
int ty_string_agg_deserialize(ty_string_agg_t *st, const char *p, size_t len);

/*
 * The result, a text or a bytea: the bytes after the first delimiter. The
 * state does not change, so a window may call it again.
 */
int ty_string_agg_finish(const ty_string_agg_t *st, ty_varlena_t **out);

/*
 * The case of ASCII letters, for the C ctype: only A-Z and a-z change, any
 * other byte (a byte of a multibyte character included) stays. These work
 * on a C string, not a varlena. out may be s. initcap makes the first letter
 * of each word upper case and the rest lower; a word is a run of ASCII
 * letters and digits.
 */
static inline void ty_ascii_lower(const char *s, size_t len, char *out) {
  for (size_t i = 0; i < len; i++) {
    char c = s[i];

    out[i] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
  }
}
static inline void ty_ascii_upper(const char *s, size_t len, char *out) {
  for (size_t i = 0; i < len; i++) {
    char c = s[i];

    out[i] = (c >= 'a' && c <= 'z') ? (char)(c - 32) : c;
  }
}
static inline void ty_ascii_initcap(const char *s, size_t len, char *out) {
  int wasalnum = 0;

  for (size_t i = 0; i < len; i++) {
    char c = s[i];

    if (wasalnum) {
      c = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
    } else {
      c = (c >= 'a' && c <= 'z') ? (char)(c - 32) : c;
    }
    out[i] = c;
    wasalnum = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
               (c >= '0' && c <= '9');
  }
}

#endif

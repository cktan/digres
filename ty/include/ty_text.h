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
 * A string is a pointer and a length in bytes; it has no NUL. A function
 * that makes a string writes it to the caller's buffer; each says how large
 * the buffer must be, and ty allocates nothing. A position or a count is in
 * characters (code points), as SQL says; a length in bytes is a size_t.
 *
 * A call that can fail returns 0, or -1 with the thread's error set; its
 * output is then not set. The message is PostgreSQL's. The kinds are:
 * TY_ERR_INVALID for a parameter that is wrong, TY_ERR_RANGE for a limit.
 */
#ifndef TY_TEXT_H
#define TY_TEXT_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ty.h"
#include "ty_hash.h"

/* The most bytes a result may have: PostgreSQL's MaxAllocSize less its header. */
#define TY_TEXT_MAX_LEN ((size_t)0x3fffffff - 4)

/*
 * Compare, as bytes; a string that is a prefix of the other is the
 * smaller: negative, zero or positive.
 */
static inline int ty_text_cmp(const char* a, size_t alen, const char* b,
                              size_t blen) {
    int c = memcmp(a, b, alen < blen ? alen : blen);

    if (c == 0 && alen != blen) {
        c = alen < blen ? -1 : 1;
    }
    return c;
}

static inline int ty_text_eq(const char* a, size_t alen, const char* b,
                             size_t blen) {
    return alen == blen && memcmp(a, b, alen) == 0;
}

/* Whether the string begins with the prefix. */
static inline int ty_text_starts_with(const char* s, size_t len,
                                      const char* prefix, size_t plen) {
    return plen <= len && memcmp(s, prefix, plen) == 0;
}

/* The hash of the bytes, PostgreSQL's. */
static inline uint32_t ty_text_hash(const char* s, size_t len) {
    return ty_hash_bytes_std((const uint8_t*)s, len);
}
static inline uint64_t ty_text_hash_extended(const char* s, size_t len,
                                             uint64_t seed) {
    return ty_hash_bytes((const uint8_t*)s, len, seed);
}

/* How many characters the string has. */
size_t ty_text_length(const char* s, size_t len);

/*
 * SUBSTRING(s FROM start FOR count): the characters from start (one-based,
 * and may be below 1) up to count of them; has_count is 0 for FROM alone.
 * A negative count is TY_ERR_INVALID ("negative substring length not
 * allowed"). The result is a part of s: its place is *off and its size
 * *outlen, so that no copy is needed.
 */
int ty_text_substr(const char* s, size_t len, int32_t start, int32_t count,
                   int has_count, size_t* off, size_t* outlen);

/*
 * left(s, n) and right(s, n), by the rules of PostgreSQL: a negative n is
 * all but the last (left) or the first (right) -n characters. The result
 * is a part of s, as above.
 */
void ty_text_left(const char* s, size_t len, int32_t n, size_t* off,
                  size_t* outlen);
void ty_text_right(const char* s, size_t len, int32_t n, size_t* off,
                   size_t* outlen);

/* s reversed by characters; out has len bytes. */
void ty_text_reverse(const char* s, size_t len, char* out);

/*
 * OVERLAY(s PLACING r FROM start FOR count): s with count characters from
 * start replaced by r. A start of 0 or less is TY_ERR_INVALID ("negative
 * substring length not allowed"); start + count past int32 is TY_ERR_RANGE
 * ("integer out of range"). A negative count makes the head and the tail
 * overlap, so out has 2 * len + rlen bytes.
 */
int ty_text_overlay(const char* s, size_t len, const char* r, size_t rlen,
                    int32_t start, int32_t count, char* out, size_t* outlen);

/*
 * The one-based character place of the first copy of needle in s, or 0 if
 * there is none; an empty needle is at 1.
 */
int32_t ty_text_position(const char* s, size_t len, const char* needle,
                         size_t nlen);

/*
 * A search for needle (not empty) in s, copy after copy, none overlapping.
 * Make it with ty_text_find_init; ty_text_find_next gives the byte place of
 * the next copy, or -1.
 */
typedef struct ty_text_find_t {
    const char* s;
    size_t len;
    const char* needle;
    size_t nlen;
    size_t pos;
    int skipmask;
    int skip[256];
} ty_text_find_t;

void ty_text_find_init(ty_text_find_t* f, const char* s, size_t len,
                       const char* needle, size_t nlen);
int64_t ty_text_find_next(ty_text_find_t* f);

/*
 * replace(s, from, to): every copy of from replaced. The size of the result
 * first (TY_ERR_RANGE, past TY_TEXT_MAX_LEN, with PostgreSQL's message), then the
 * result, in out of that size. If *found is 0 there was nothing to replace:
 * the result is s itself, and the caller need not copy.
 */
int ty_text_replace_size(const char* s, size_t len, const char* from,
                         size_t flen, size_t tlen, size_t* outlen,
                         int* found);
void ty_text_replace(const char* s, size_t len, const char* from, size_t flen,
                     const char* to, size_t tlen, char* out);

/*
 * split_part(s, sep, n): the n-th field, from the end if n is negative. The
 * result is a part of s. n of 0 is TY_ERR_INVALID ("field position must not
 * be zero").
 */
int ty_text_split_part(const char* s, size_t len, const char* sep,
                       size_t seplen, int32_t n, size_t* off, size_t* outlen);

/*
 * repeat(s, n): the size of the result, then the result. A size past
 * TY_TEXT_MAX_LEN is TY_ERR_RANGE ("requested length too large").
 */
int ty_text_repeat_size(size_t len, int32_t count, size_t* outlen);
void ty_text_repeat(const char* s, size_t len, int32_t count, char* out);

/* TY_ERR_RANGE ("requested length too large") if bytes passes the limit. */
int ty_text_size_check(size_t bytes);

/*
 * lpad(s, n, fill) and rpad(s, n, fill): s cut or padded to n characters,
 * the fill used again and again; with an empty fill s is only cut.
 * ty_text_pad_prepare gives the number of characters of the result, *eff,
 * and checks PostgreSQL's rule that 4 * eff bytes are within the limit
 * (TY_ERR_RANGE, "requested length too large"); out then must hold 4 * eff
 * bytes.
 */
int ty_text_pad_prepare(const char* s, size_t len, int32_t n, size_t flen,
                        int32_t* eff);
void ty_text_lpad(const char* s, size_t len, int32_t eff, const char* fill,
                  size_t flen, char* out, size_t* outlen);
void ty_text_rpad(const char* s, size_t len, int32_t eff, const char* fill,
                  size_t flen, char* out, size_t* outlen);

/*
 * btrim, ltrim and rtrim: the characters that are in set taken from the
 * ends. The result is a part of s.
 */
void ty_text_trim(const char* s, size_t len, const char* set, size_t setlen,
                  int left, int right, size_t* off, size_t* outlen);

/*
 * translate(s, from, to): each character of s that is in from becomes the
 * character of to at the same place, or nothing if to is shorter. out has
 * 4 * len bytes, and the caller checks that with ty_text_size_check. The
 * result size is returned in *outlen.
 */
void ty_text_translate(const char* s, size_t len, const char* from,
                       size_t flen, const char* to, size_t tlen, char* out,
                       size_t* outlen);

/*
 * ascii(s): the code point of the first character, 0 for an empty string.
 * A first byte that begins no character is TY_ERR_INVALID.
 */
int ty_text_ascii(const char* s, size_t len, int32_t* out);

/*
 * chr(n): the character of code point n, in out of 4 bytes; the size is
 * returned in *outlen. A negative n is TY_ERR_INVALID ("character number
 * must be positive"); 0, a point past 0x10FFFF, and a surrogate are
 * TY_ERR_RANGE.
 */
int ty_text_chr(int32_t n, char* out, size_t* outlen);

/*
 * The bytes of the first nchars characters of s (all of s if it has fewer);
 * and the longest part of s of at most nbytes bytes that ends at a character
 * boundary (PostgreSQL's pg_mbcliplen).
 */
size_t ty_text_cliplen(const char* s, size_t len, size_t nchars);
size_t ty_text_clipbytes(const char* s, size_t len, size_t nbytes);

/*
 * character(n) and character varying(n). Fit s to maxchars characters: if it
 * has more, the part kept is *keep bytes, and a character cut off that is not
 * a space is TY_ERR_RANGE unless force (the host words "value too long").
 * *total is the bytes of the result: for character(n) the part kept, and
 * spaces to make n characters; pad is 1 for that and 0 for varying.
 */
int ty_bpchar_fit(const char* s, size_t len, size_t maxchars, int force,
                  int pad, size_t* keep, size_t* total);

/* The length of a character(n) value without its trailing spaces. */
static inline size_t ty_bpchar_truelen(const char* s, size_t len) {
    while (len > 0 && s[len - 1] == ' ') {
        len--;
    }
    return len;
}

/*
 * The order and equality of two character(n) values: the C order of the
 * values without trailing spaces.
 */
static inline int ty_bpchar_cmp(const char* a, size_t alen, const char* b,
                                size_t blen) {
    return ty_text_cmp(a, ty_bpchar_truelen(a, alen), b,
                       ty_bpchar_truelen(b, blen));
}

/* The hash of the value without its trailing spaces. */
static inline uint32_t ty_bpchar_hash(const char* s, size_t len) {
    return ty_text_hash(s, ty_bpchar_truelen(s, len));
}
static inline uint64_t ty_bpchar_hash_extended(const char* s, size_t len,
                                               uint64_t seed) {
    return ty_text_hash_extended(s, ty_bpchar_truelen(s, len), seed);
}

/*
 * name: NAMEDATALEN bytes, a NUL-ended string cut to 63 bytes at a character
 * boundary. Write the name from s to out (zero-filled); the order is of
 * the bytes up to the NUL.
 */
#define TY_NAMEDATALEN 64
void ty_name_parse(const char* s, size_t len, char* out);
static inline int ty_name_cmp(const char* a, const char* b) {
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
 * and 0 for bytea, where they step over a byte.
 */
int ty_text_like(const char* t, size_t tlen, const char* p, size_t plen,
                 int utf8);

/*
 * The pattern of LIKE ... ESCAPE esc, made into the pattern of the default
 * escape, a backslash: out has 2 * plen bytes, and *outlen is the result's
 * size. An escape of more than one character is TY_ERR_INVALID ("invalid
 * escape string"). An empty escape makes every backslash plain. utf8 is as
 * in ty_text_like: a character is a whole UTF-8 character, or a byte.
 */
int ty_text_like_escape(const char* pat, size_t plen, const char* esc,
                        size_t elen, int utf8, char* out, size_t* outlen);

#endif

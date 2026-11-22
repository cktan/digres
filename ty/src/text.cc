/*
 * Copyright (c) 2026 CK Tan
 * SPDX-License-Identifier: MIT
 */

/*
 * text.cc -- the operations of UTF-8 text.
 */
#include "text_raw.h"
#include "ty_text.h"

#include <stdio.h>
#include <string.h>

#include "error.h"
#include "mem.h"

namespace {

/* The bytes of the character that begins with byte b: 1 to 4. */
inline size_t clen(unsigned char b) {
  return (b & 0x80) == 0      ? 1
         : (b & 0xe0) == 0xc0 ? 2
         : (b & 0xf0) == 0xe0 ? 3
         : (b & 0xf8) == 0xf0 ? 4
                              : 1;
}

/* The place after the character at p, but not past end. */
inline const char *next_char(const char *p, const char *end) {
  size_t n = clen((unsigned char)*p);

  return (size_t)(end - p) < n ? end : p + n;
}

/* The place after n characters from p, but not past end. */
const char *advance(const char *p, const char *end, int64_t n) {
  while (n > 0 && p < end) {
    p = next_char(p, end);
    n--;
  }
  return p;
}

/* The error of a result that is too large. */
int too_large() {
  return ty_internal::set_error(TY_ERR_RANGE, "requested length too large");
}

} // namespace

/* Count the bytes that are not continuation bytes, 8 at a time. */
size_t tyr_text_length(const char *s, size_t len) {
  size_t chars = 0, i = 0;

  for (; i + 8 <= len; i += 8) {
    uint64_t w;

    memcpy(&w, s + i, 8);
    /* a continuation byte is 10xxxxxx: bit 7 set, bit 6 clear */
    uint64_t cont = (w >> 7) & ~(w >> 6) & 0x0101010101010101ULL;

    chars += 8 - (size_t)__builtin_popcountll(cont);
  }
  for (; i < len; i++) {
    chars += ((unsigned char)s[i] & 0xC0) != 0x80;
  }
  return chars;
}

/*
 * SUBSTRING.
 * 1. The first character is start, but not before 1.
 * 2. With a count, the end (excluded) is start + count; if that overflows, or
 *    there is no count, it is the end of the string. An end of 1 or less is
 *    an empty result.
 * 3. Skip the characters before the first, then take up to the count.
 */
int tyr_text_substr(const char *s, size_t len, int32_t start, int32_t count,
                    int has_count, size_t *off, size_t *outlen) {
  int32_t s1 = start > 1 ? start : 1, e = 0;
  bool to_end = !has_count;

  if (has_count) {
    if (count < 0) {
      return ty_internal::set_error(TY_ERR_INVALID,
                                    "negative substring length not allowed");
    }
    if (__builtin_add_overflow(start, count, &e)) {
      to_end = true;
    } else if (e <= 1) {
      *off = 0;
      *outlen = 0;
      return 0;
    }
  }
  const char *end = s + len;
  const char *p = advance(s, end, (int64_t)s1 - 1);
  const char *q = to_end ? end : advance(p, end, (int64_t)e - s1);

  *off = (size_t)(p - s);
  *outlen = (size_t)(q - p);
  return 0;
}

/* The first n characters, or all but the last -n. */
void tyr_text_left(const char *s, size_t len, int32_t n, size_t *off,
                   size_t *outlen) {
  int64_t k = n;

  if (n < 0) {
    k = (int64_t)tyr_text_length(s, len) + n;
  }
  *off = 0;
  *outlen = (size_t)(advance(s, s + len, k) - s);
}

/* The last n characters, or all but the first -n. */
void tyr_text_right(const char *s, size_t len, int32_t n, size_t *off,
                    size_t *outlen) {
  int64_t k;

  if (n < 0) {
    k = n == INT32_MIN ? INT32_MAX : -(int64_t)n;
  } else {
    k = (int64_t)tyr_text_length(s, len) - n;
  }
  *off = (size_t)(advance(s, s + len, k) - s);
  *outlen = len - *off;
}

/* Reverse by characters. */
void tyr_text_reverse(const char *s, size_t len, char *out) {
  const char *p = s, *end = s + len;
  char *dst = out + len;

  while (p < end) {
    const char *q = next_char(p, end);
    size_t sz = (size_t)(q - p);

    dst -= sz;
    memcpy(dst, p, sz);
    p = q;
  }
}

/* OVERLAY: the first start-1 characters, r, then the rest after count. */
int tyr_text_overlay(const char *s, size_t len, const char *r, size_t rlen,
                     int32_t start, int32_t count, char *out, size_t *outlen) {
  int32_t sum;
  size_t off1, len1, off2, len2;

  if (start <= 0) {
    return ty_internal::set_error(TY_ERR_INVALID,
                                  "negative substring length not allowed");
  }
  if (__builtin_add_overflow(start, count, &sum)) {
    return ty_err_range();
  }
  /* the head: the first start - 1 characters */
  tyr_text_substr(s, len, 1, start - 1, 1, &off1, &len1);
  /* the tail: from start + count to the end (a start below 1 is 1) */
  tyr_text_substr(s, len, sum, 0, 0, &off2, &len2);
  memcpy(out, s + off1, len1);
  memcpy(out + len1, r, rlen);
  memcpy(out + len1 + rlen, s + off2, len2);
  *outlen = len1 + rlen + len2;
  return 0;
}

/*
 * Set up a search. For a needle of 2 bytes or more, in a string at least as
 * long, make a skip table of a size that fits the length of the search
 * (Boyer-Moore-Horspool, as PostgreSQL does): a larger table skips more but
 * costs more to make.
 */
void tyr_text_find_init(tyr_text_find_t *f, const char *s, size_t len,
                        const char *needle, size_t nlen) {
  f->s = s;
  f->len = len;
  f->needle = needle;
  f->nlen = nlen;
  f->pos = 0;
  f->skipmask = 0;
  if (len >= nlen && nlen > 1) {
    size_t searchlength = len - nlen;
    int mask = searchlength < 16     ? 3
               : searchlength < 64   ? 7
               : searchlength < 128  ? 15
               : searchlength < 512  ? 31
               : searchlength < 2048 ? 63
               : searchlength < 4096 ? 127
                                     : 255;
    int last = (int)nlen - 1;

    f->skipmask = mask;
    for (int i = 0; i <= mask; i++) {
      f->skip[i] = (int)(nlen > 0x7fffffff ? 0x7fffffff : nlen);
    }
    for (int i = 0; i < last; i++) {
      f->skip[(unsigned char)needle[i] & mask] = last - i;
    }
  }
}

/* The next copy at or after pos; set pos after it. */
int64_t tyr_text_find_next(tyr_text_find_t *f) {
  const char *hay = f->s, *needle = f->needle;
  const char *end = hay + f->len;
  const char *hptr = hay + f->pos;

  if (f->nlen == 0 || f->len < f->nlen || f->pos > f->len - f->nlen) {
    return -1;
  }
  if (f->nlen == 1) {
    const void *m =
        memchr(hptr, (unsigned char)needle[0], (size_t)(end - hptr));

    if (m == nullptr) {
      return -1;
    }
    f->pos = (size_t)((const char *)m - hay) + 1;
    return (int64_t)((const char *)m - hay);
  }
  const char *needle_last = needle + f->nlen - 1;

  hptr += f->nlen - 1;
  while (hptr < end) {
    const char *nptr = needle_last, *p = hptr;

    while (*nptr == *p) {
      if (nptr == needle) {
        f->pos = (size_t)(p - hay) + f->nlen;
        return (int64_t)(p - hay);
      }
      nptr--, p--;
    }
    hptr += f->skip[(unsigned char)*hptr & f->skipmask];
  }
  return -1;
}

/* Find needle in s; the place is in characters. */
int32_t tyr_text_position(const char *s, size_t len, const char *needle,
                          size_t nlen) {
  if (nlen < 1) {
    return 1;
  }
  if (len < nlen) {
    return 0;
  }
  tyr_text_find_t f;
  int64_t m;

  tyr_text_find_init(&f, s, len, needle, nlen);
  m = tyr_text_find_next(&f);
  return m < 0 ? 0 : (int32_t)(tyr_text_length(s, (size_t)m) + 1);
}

/* The size of the result of replace: count the copies. */
int tyr_text_replace_size(const char *s, size_t len, const char *from,
                          size_t flen, size_t tlen, size_t *outlen,
                          int *found) {
  *found = 0;
  *outlen = len;
  if (len < 1 || flen < 1) {
    return 0;
  }
  tyr_text_find_t f;
  size_t n = 0;

  tyr_text_find_init(&f, s, len, from, flen);
  while (tyr_text_find_next(&f) >= 0) {
    n++;
  }
  if (n == 0) {
    return 0;
  }
  *found = 1;
  /* len + n * (tlen - flen) in signed 128 bits cannot overflow */
  __int128 total =
      (__int128)len + (__int128)n * ((__int128)tlen - (__int128)flen);

  if (total > (__int128)TY_TEXT_MAX_LEN) {
    return ty_internal::set_error(
        TY_ERR_RANGE,
        "string buffer exceeds maximum allowed length (1073741823 bytes)");
  }
  *outlen = (size_t)total;
  return 0;
}

/* Write the result of replace. */
void tyr_text_replace(const char *s, size_t len, const char *from, size_t flen,
                      const char *to, size_t tlen, char *out) {
  tyr_text_find_t f;
  size_t start = 0;
  int64_t m;

  tyr_text_find_init(&f, s, len, from, flen);
  while ((m = tyr_text_find_next(&f)) >= 0) {
    memcpy(out, s + start, (size_t)m - start);
    out += (size_t)m - start;
    memcpy(out, to, tlen);
    out += tlen;
    start = (size_t)m + flen;
  }
  memcpy(out, s + start, len - start);
}

/*
 * split_part.
 * 1. No fields in an empty string; with no separator the string is the one
 *    field.
 * 2. A negative n counts from the end: n becomes the number of fields + n + 1.
 * 3. The field is between the (n-1)th and the nth copy of the separator.
 */
int tyr_text_split_part(const char *s, size_t len, const char *sep,
                        size_t seplen, int32_t n, size_t *off, size_t *outlen) {
  *off = 0;
  *outlen = 0;
  if (n == 0) {
    return ty_internal::set_error(TY_ERR_INVALID,
                                  "field position must not be zero");
  }
  if (len < 1) {
    return 0;
  }
  if (seplen < 1) {
    if (n == 1 || n == -1) {
      *outlen = len;
    }
    return 0;
  }
  tyr_text_find_t f;
  int64_t k = n;

  if (n < 0) {
    int64_t fields = 1;

    tyr_text_find_init(&f, s, len, sep, seplen);
    while (tyr_text_find_next(&f) >= 0) {
      fields++;
    }
    k = fields + n + 1;
    if (k <= 0) {
      return 0;
    }
  }
  tyr_text_find_init(&f, s, len, sep, seplen);
  size_t start = 0;
  int64_t m = -1;

  /* walk past k - 1 copies: the field starts after the last of them */
  for (int64_t i = 1; i < k; i++) {
    m = tyr_text_find_next(&f);
    if (m < 0) {
      return 0; /* there is no such field */
    }
    start = (size_t)m + seplen;
  }
  m = tyr_text_find_next(&f);
  *off = start;
  *outlen = (m < 0 ? len : (size_t)m) - start;
  return 0;
}

/* The size of repeat. */
int tyr_text_repeat_size(size_t len, int32_t count, size_t *outlen) {
  if (count < 0) {
    count = 0;
  }
  __int128 total = (__int128)len * count;

  if (total > (__int128)TY_TEXT_MAX_LEN) {
    return too_large();
  }
  *outlen = (size_t)total;
  return 0;
}

/* repeat: copy once, then double what is there. */
void tyr_text_repeat(const char *s, size_t len, int32_t count, char *out) {
  if (count <= 0 || len == 0) {
    return;
  }
  size_t total = len * (size_t)count, done = len;

  memcpy(out, s, len);
  while (done < total) {
    size_t n = done < total - done ? done : total - done;

    memcpy(out + done, out, n);
    done += n;
  }
}

/* TY_ERR_RANGE if the result may not be so large. */
int tyr_text_size_check(size_t bytes) {
  return bytes > TY_TEXT_MAX_LEN ? too_large() : 0;
}

/*
 * The length of the result of lpad or rpad, in characters, and PostgreSQL's
 * limit: 4 bytes a character.
 */
int tyr_text_pad_prepare(const char *s, size_t len, int32_t n, size_t flen,
                         int32_t *eff) {
  int64_t want = n < 0 ? 0 : n;
  int64_t have = (int64_t)tyr_text_length(s, len);

  if (have > want) {
    have = want;
  }
  if (flen <= 0) {
    want = have; /* nothing to pad with, so do not pad */
  }
  if (tyr_text_size_check((size_t)want * 4) != 0) {
    return -1;
  }
  *eff = (int32_t)want;
  return 0;
}

namespace {

/* Write m characters of fill, used again and again; return the bytes. */
char *put_fill(char *dst, int64_t m, const char *fill, size_t flen) {
  const char *p = fill, *end = fill + flen;

  while (m-- > 0) {
    size_t n = (size_t)(next_char(p, end) - p);

    memcpy(dst, p, n);
    dst += n;
    p += n;
    if (p == end) {
      p = fill; /* wrap around at the end of the fill */
    }
  }
  return dst;
}

/* The first nchars characters of s, written to dst. */
char *put_head(char *dst, const char *s, size_t len, int64_t nchars) {
  const char *q = advance(s, s + len, nchars);

  memcpy(dst, s, (size_t)(q - s));
  return dst + (q - s);
}

} // namespace

/* lpad: the fill, then the first characters of s. */
void tyr_text_lpad(const char *s, size_t len, int32_t eff, const char *fill,
                   size_t flen, char *out, size_t *outlen) {
  int64_t have = (int64_t)tyr_text_length(s, len);

  if (have > eff) {
    have = eff;
  }
  char *p = put_fill(out, eff - have, fill, flen);

  p = put_head(p, s, len, have);
  *outlen = (size_t)(p - out);
}

/* rpad: the first characters of s, then the fill. */
void tyr_text_rpad(const char *s, size_t len, int32_t eff, const char *fill,
                   size_t flen, char *out, size_t *outlen) {
  int64_t have = (int64_t)tyr_text_length(s, len);

  if (have > eff) {
    have = eff;
  }
  char *p = put_head(out, s, len, have);

  p = put_fill(p, eff - have, fill, flen);
  *outlen = (size_t)(p - out);
}

namespace {

/* Whether the character at p, of n bytes, is one of the set. */
bool in_set(const char *p, size_t n, const char *set, size_t setlen) {
  const char *q = set, *end = set + setlen;

  while (q < end) {
    size_t m = (size_t)(next_char(q, end) - q);

    if (m == n && memcmp(p, q, n) == 0) {
      return true;
    }
    q += m;
  }
  return false;
}

} // namespace

/* Trim the characters in set from the left and the right. */
void tyr_text_trim(const char *s, size_t len, const char *set, size_t setlen,
                   int left, int right, size_t *off, size_t *outlen) {
  const char *p = s, *end = s + len;

  *off = 0;
  *outlen = len;
  if (len == 0 || setlen == 0) {
    return;
  }
  if (left) {
    while (p < end) {
      const char *q = next_char(p, end);

      if (!in_set(p, (size_t)(q - p), set, setlen)) {
        break;
      }
      p = q;
    }
  }
  if (right) {
    while (end > p) {
      /* the last character begins at the last byte that is not a continuation
       */
      const char *b = end - 1;

      while (b > p && ((unsigned char)*b & 0xC0) == 0x80) {
        b--;
      }
      if (!in_set(b, (size_t)(end - b), set, setlen)) {
        break;
      }
      end = b;
    }
  }
  *off = (size_t)(p - s);
  *outlen = (size_t)(end - p);
}

/* Translate: for each character of s, find it in from; use the same place in
 * to. */
void tyr_text_translate(const char *s, size_t len, const char *from,
                        size_t flen, const char *to, size_t tlen, char *out,
                        size_t *outlen) {
  const char *p = s, *end = s + len;
  const char *to_end = to + tlen;
  char *dst = out;

  while (p < end) {
    const char *q = next_char(p, end);
    size_t n = (size_t)(q - p);
    int64_t index = 0;
    const char *f = from, *from_end = from + flen;

    while (f < from_end) {
      const char *g = next_char(f, from_end);

      if ((size_t)(g - f) == n && memcmp(p, f, n) == 0) {
        break;
      }
      f = g;
      index++;
    }
    if (f < from_end) {
      const char *t = advance(to, to_end, index);

      if (t < to_end) {
        size_t m = (size_t)(next_char(t, to_end) - t);

        memcpy(dst, t, m);
        dst += m;
      }
    } else {
      memcpy(dst, p, n);
      dst += n;
    }
    p = q;
  }
  *outlen = (size_t)(dst - out);
}

/* The code point of the first character. */
int tyr_text_ascii(const char *s, size_t len, int32_t *out) {
  const unsigned char *d = (const unsigned char *)s;

  if (len == 0) {
    *out = 0;
    return 0;
  }
  if (*d <= 127) {
    *out = *d;
    return 0;
  }
  int result, tbytes;
  const char *msg = "invalid byte sequence for encoding \"UTF8\"";

  if (*d >= 0xF0) {
    result = *d & 0x07;
    tbytes = 3;
  } else if (*d >= 0xE0) {
    result = *d & 0x0F;
    tbytes = 2;
  } else if (*d > 0xC0) {
    result = *d & 0x1f;
    tbytes = 1;
  } else {
    return ty_internal::set_error(TY_ERR_INVALID, msg);
  }
  if ((size_t)tbytes >= len) {
    return ty_internal::set_error(TY_ERR_INVALID, msg);
  }
  for (int i = 1; i <= tbytes; i++) {
    if ((d[i] & 0xC0) != 0x80) {
      return ty_internal::set_error(TY_ERR_INVALID, msg);
    }
    result = (result << 6) + (d[i] & 0x3f);
  }
  *out = result;
  return 0;
}

/* The character of a code point. */
int tyr_text_chr(int32_t n, char *out, size_t *outlen) {
  char msg[96];

  if (n < 0) {
    return ty_internal::set_error(TY_ERR_INVALID,
                                  "character number must be positive");
  }
  if (n == 0) {
    return ty_internal::set_error(TY_ERR_RANGE, "null character not permitted");
  }
  uint32_t c = (uint32_t)n;

  if (c <= 127) {
    out[0] = (char)c;
    *outlen = 1;
    return 0;
  }
  if (c > 0x0010ffff) {
    snprintf(msg, sizeof msg, "requested character too large for encoding: %u",
             c);
    return ty_internal::set_error(TY_ERR_RANGE, msg);
  }
  /* a surrogate is no character */
  if (c >= 0xD800 && c <= 0xDFFF) {
    snprintf(msg, sizeof msg, "requested character not valid for encoding: %u",
             c);
    return ty_internal::set_error(TY_ERR_RANGE, msg);
  }
  unsigned char *w = (unsigned char *)out;

  if (c > 0xffff) {
    w[0] = (unsigned char)(0xF0 | ((c >> 18) & 0x07));
    w[1] = (unsigned char)(0x80 | ((c >> 12) & 0x3F));
    w[2] = (unsigned char)(0x80 | ((c >> 6) & 0x3F));
    w[3] = (unsigned char)(0x80 | (c & 0x3F));
    *outlen = 4;
  } else if (c > 0x07ff) {
    w[0] = (unsigned char)(0xE0 | ((c >> 12) & 0x0F));
    w[1] = (unsigned char)(0x80 | ((c >> 6) & 0x3F));
    w[2] = (unsigned char)(0x80 | (c & 0x3F));
    *outlen = 3;
  } else {
    w[0] = (unsigned char)(0xC0 | ((c >> 6) & 0x1F));
    w[1] = (unsigned char)(0x80 | (c & 0x3F));
    *outlen = 2;
  }
  return 0;
}

/* The bytes of the first nchars characters. */
size_t tyr_text_cliplen(const char *s, size_t len, size_t nchars) {
  return (size_t)(advance(s, s + len, (int64_t)nchars) - s);
}

/* The longest part of at most nbytes that ends at a character boundary. */
size_t tyr_text_clipbytes(const char *s, size_t len, size_t nbytes) {
  const char *p = s, *end = s + len;
  size_t kept = 0;

  while (p < end) {
    const char *q = next_char(p, end);

    if ((size_t)(q - s) > nbytes) {
      break;
    }
    kept = (size_t)(q - s);
    p = q;
  }
  return kept;
}

/*
 * Fit to maxchars characters.
 * 1. If s has more characters, keep the first maxchars; the rest must be
 *    spaces unless force is set.
 * 2. If not, keep all of s, and for character(n) add spaces up to maxchars.
 */
int tyr_bpchar_fit(const char *s, size_t len, size_t maxchars, int force,
                   int pad, size_t *keep, size_t *total) {
  size_t chars = tyr_text_length(s, len);

  if (chars > maxchars) {
    size_t k = tyr_text_cliplen(s, len, maxchars);

    if (!force) {
      for (size_t j = k; j < len; j++) {
        if (s[j] != ' ') {
          return ty_internal::set_error(TY_ERR_RANGE, "value too long");
        }
      }
    }
    *keep = k;
    *total = k;
  } else {
    *keep = len;
    *total = pad ? len + (maxchars - chars) : len;
  }
  return 0;
}

/* A name: cut to 63 bytes at a character, zero-filled to 64. */
void tyr_name_parse(const char *s, size_t len, char *out) {
  size_t n = len >= TY_NAMEDATALEN
                 ? tyr_text_clipbytes(s, len, TY_NAMEDATALEN - 1)
                 : len;

  memset(out, 0, TY_NAMEDATALEN);
  memcpy(out, s, n);
}

namespace {

const int k_like_true = 1, k_like_false = 0, k_like_abort = -1,
          k_like_error = -2;

/* Step over one character (utf8), or one byte. */
inline void like_next(const char *&t, size_t &tlen, int utf8) {
  t++, tlen--;
  if (utf8) {
    while (tlen > 0 && ((unsigned char)*t & 0xC0) == 0x80) {
      t++, tlen--;
    }
  }
}

/*
 * The match of LIKE, in three values: a match; no match here, so another
 * start of the text may do; and no match at any start, so the caller stops.
 * Each % calls this for the rest of the pattern, at each start of the text
 * that begins with the next literal.
 */
int like_match(const char *t, size_t tlen, const char *p, size_t plen, int utf8,
               int depth) {
  if (plen == 1 && *p == '%') {
    return k_like_true;
  }
  if (depth > 5000) {
    ty_internal::set_error(TY_ERR_RANGE, "stack depth limit exceeded");
    return k_like_error;
  }
  while (tlen > 0 && plen > 0) {
    if (*p == '%') {
      p++, plen--;
      /* a run of % and _ : each _ takes a character of the text */
      while (plen > 0) {
        if (*p == '%') {
          p++, plen--;
        } else if (*p == '_') {
          if (tlen <= 0) {
            return k_like_abort;
          }
          like_next(t, tlen, utf8);
          p++, plen--;
        } else {
          break;
        }
      }
      if (plen <= 0) {
        return k_like_true;
      }
      char first;

      if (*p == '\\') {
        if (plen < 2) {
          ty_internal::set_error(
              TY_ERR_INVALID,
              "LIKE pattern must not end with escape character");
          return k_like_error;
        }
        first = p[1];
      } else {
        first = *p;
      }
      /*
       * Try each start of the text that has the next literal. If the literal
       * is an ASCII byte (or the text is bytes), memchr finds the starts: an
       * ASCII byte is never inside a multibyte character.
       */
      if (!utf8 || (unsigned char)first < 0x80) {
        while (tlen > 0) {
          const char *m = (const char *)memchr(t, (unsigned char)first, tlen);

          if (m == nullptr) {
            break;
          }
          tlen -= (size_t)(m - t);
          t = m;
          int r = like_match(t, tlen, p, plen, utf8, depth + 1);

          if (r != k_like_false) {
            return r; /* a match, an abort or an error */
          }
          t++, tlen--;
        }
        return k_like_abort;
      }
      while (tlen > 0) {
        if (*t == first) {
          int m = like_match(t, tlen, p, plen, utf8, depth + 1);

          if (m != k_like_false) {
            return m; /* a match, an abort or an error */
          }
        }
        like_next(t, tlen, utf8);
      }
      return k_like_abort;
    } else if (*p == '_') {
      like_next(t, tlen, utf8);
      p++, plen--;
      continue;
    } else if (*p == '\\') {
      p++, plen--;
      if (plen <= 0) {
        ty_internal::set_error(
            TY_ERR_INVALID, "LIKE pattern must not end with escape character");
        return k_like_error;
      }
      if (*p != *t) {
        return k_like_false;
      }
    } else if (*p != *t) {
      return k_like_false;
    }
    t++, tlen--;
    p++, plen--;
  }
  if (tlen > 0) {
    return k_like_false; /* the end of the pattern, but not of the text */
  }
  while (plen > 0 && *p == '%') {
    p++, plen--;
  }
  return plen <= 0 ? k_like_true : k_like_abort;
}

} // namespace

/* LIKE: a match, or not, or an error. */
int tyr_text_like(const char *t, size_t tlen, const char *p, size_t plen,
                  int utf8) {
  int m = like_match(t, tlen, p, plen, utf8, 0);

  return m == k_like_error ? -1 : m == k_like_true ? 1 : 0;
}

/*
 * The pattern with the default escape.
 * 1. With no escape wanted, double each backslash, so it is plain.
 * 2. With a backslash as the escape, the pattern is as it is.
 * 3. With another character, change it to a backslash, and double a
 *    backslash, unless it comes right after the escape.
 */
int tyr_text_like_escape(const char *pat, size_t plen, const char *esc,
                         size_t elen, int utf8, char *out, size_t *outlen) {
  const char *p = pat, *pend = pat + plen;
  char *r = out;

  if (elen == 0) {
    while (p < pend) {
      if (*p == '\\') {
        *r++ = '\\';
      }
      const char *q = utf8 ? next_char(p, pend) : p + 1;

      memcpy(r, p, (size_t)(q - p));
      r += q - p;
      p = q;
    }
  } else {
    if ((utf8 ? next_char(esc, esc + elen) : esc + 1) != esc + elen) {
      return ty_internal::set_error(TY_ERR_INVALID, "invalid escape string");
    }
    if (*esc == '\\') {
      memcpy(out, pat, plen);
      *outlen = plen;
      return 0;
    }
    bool after = false;

    while (p < pend) {
      const char *q = utf8 ? next_char(p, pend) : p + 1;
      size_t n = (size_t)(q - p);

      if (n == elen && memcmp(p, esc, n) == 0 && !after) {
        *r++ = '\\';
        after = true;
      } else if (*p == '\\') {
        *r++ = '\\';
        if (!after) {
          *r++ = '\\';
        }
        after = false;
      } else {
        memcpy(r, p, n);
        r += n;
        after = false;
      }
      p = q;
    }
  }
  *outlen = (size_t)(r - out);
  return 0;
}

/* ---- the operations on a varlena: each reads its inputs' bytes, calls the
 * function above, and puts a result in memory from the thread's alloc ---- */

namespace {

/* The place and length of the bytes of v. */
struct bytes_t {
  const char *p;
  size_t n;
  explicit bytes_t(const ty_varlena_t *v) : p(ty_vl_data(v)), n(ty_vl_len(v)) {}
};

/* A result that is the bytes at off, n of them, of s. */
int part(const bytes_t &s, size_t off, size_t n, ty_varlena_t **out) {
  return ty_vl_new_copy(s.p + off, n, out);
}

} // namespace

size_t ty_text_length(const ty_varlena_t *s) {
  bytes_t a(s);

  return tyr_text_length(a.p, a.n);
}

size_t ty_text_length_bytes(const char *s, size_t len) {
  return tyr_text_length(s, len);
}

int ty_text_substr(const ty_varlena_t *s, int32_t start, int32_t count,
                   int has_count, ty_varlena_t **out) {
  bytes_t a(s);
  size_t off, n;

  if (tyr_text_substr(a.p, a.n, start, count, has_count, &off, &n) != 0) {
    return -1;
  }
  return part(a, off, n, out);
}

int ty_text_left(const ty_varlena_t *s, int32_t n, ty_varlena_t **out) {
  bytes_t a(s);
  size_t off, len;

  tyr_text_left(a.p, a.n, n, &off, &len);
  return part(a, off, len, out);
}

int ty_text_right(const ty_varlena_t *s, int32_t n, ty_varlena_t **out) {
  bytes_t a(s);
  size_t off, len;

  tyr_text_right(a.p, a.n, n, &off, &len);
  return part(a, off, len, out);
}

int ty_text_reverse(const ty_varlena_t *s, ty_varlena_t **out) {
  bytes_t a(s);
  char *d;

  if (ty_vl_new(a.n, out, &d) != 0) {
    return -1;
  }
  tyr_text_reverse(a.p, a.n, d);
  return 0;
}

/*
 * OVERLAY.
 * 1. Check the start, and that start + count fits an int32.
 * 2. The head is the first start - 1 characters, the tail all from start +
 *    count; their sizes and r's make the size of the result.
 * 3. Make the result, and have the raw function write it.
 */
int ty_text_overlay(const ty_varlena_t *s, const ty_varlena_t *r, int32_t start,
                    int32_t count, ty_varlena_t **out) {
  bytes_t a(s), b(r);
  int32_t sum;
  size_t off1, len1, off2, len2, outlen;
  char *d;

  if (start <= 0) {
    return ty_internal::set_error(TY_ERR_INVALID,
                                  "negative substring length not allowed");
  }
  if (__builtin_add_overflow(start, count, &sum)) {
    return ty_err_range();
  }
  tyr_text_substr(a.p, a.n, 1, start - 1, 1, &off1, &len1);
  tyr_text_substr(a.p, a.n, sum, 0, 0, &off2, &len2);
  if (ty_vl_new(len1 + b.n + len2, out, &d) != 0) {
    return -1;
  }
  tyr_text_overlay(a.p, a.n, b.p, b.n, start, count, d, &outlen);
  return 0;
}

int32_t ty_text_position(const ty_varlena_t *s, const ty_varlena_t *needle) {
  bytes_t a(s), b(needle);

  return tyr_text_position(a.p, a.n, b.p, b.n);
}

int ty_text_replace(const ty_varlena_t *s, const ty_varlena_t *from,
                    const ty_varlena_t *to, ty_varlena_t **out) {
  bytes_t a(s), f(from), t(to);
  size_t total;
  int found;
  char *d;

  if (tyr_text_replace_size(a.p, a.n, f.p, f.n, t.n, &total, &found) != 0) {
    return -1;
  }
  if (!found) {
    *out = (ty_varlena_t *)s;
    return 0;
  }
  if (ty_vl_new(total, out, &d) != 0) {
    return -1;
  }
  tyr_text_replace(a.p, a.n, f.p, f.n, t.p, t.n, d);
  return 0;
}

int ty_text_split_part(const ty_varlena_t *s, const ty_varlena_t *sep,
                       int32_t n, ty_varlena_t **out) {
  bytes_t a(s), b(sep);
  size_t off, len;

  if (tyr_text_split_part(a.p, a.n, b.p, b.n, n, &off, &len) != 0) {
    return -1;
  }
  return part(a, off, len, out);
}

int ty_text_repeat(const ty_varlena_t *s, int32_t count, ty_varlena_t **out) {
  bytes_t a(s);
  size_t total;
  char *d;

  if (tyr_text_repeat_size(a.n, count, &total) != 0 ||
      ty_vl_new(total, out, &d) != 0) {
    return -1;
  }
  tyr_text_repeat(a.p, a.n, count, d);
  return 0;
}

/* lpad and rpad: the length in characters, the check, then the pad. */
static int pad(const ty_varlena_t *s, int32_t n, const ty_varlena_t *fill,
               int left, ty_varlena_t **out) {
  bytes_t a(s), f(fill);
  int32_t eff;
  size_t outlen;
  char *d;

  if (tyr_text_pad_prepare(a.p, a.n, n, f.n, &eff) != 0 ||
      ty_vl_new((size_t)eff * 4, out, &d) != 0) {
    return -1;
  }
  if (left) {
    tyr_text_lpad(a.p, a.n, eff, f.p, f.n, d, &outlen);
  } else {
    tyr_text_rpad(a.p, a.n, eff, f.p, f.n, d, &outlen);
  }
  ty_vl_set_len(*out, outlen);
  return 0;
}

int ty_text_lpad(const ty_varlena_t *s, int32_t n, const ty_varlena_t *fill,
                 ty_varlena_t **out) {
  return pad(s, n, fill, 1, out);
}

int ty_text_rpad(const ty_varlena_t *s, int32_t n, const ty_varlena_t *fill,
                 ty_varlena_t **out) {
  return pad(s, n, fill, 0, out);
}

int ty_text_trim(const ty_varlena_t *s, const ty_varlena_t *set, int left,
                 int right, ty_varlena_t **out) {
  bytes_t a(s), b(set);
  size_t off, len;

  tyr_text_trim(a.p, a.n, b.p, b.n, left, right, &off, &len);
  return part(a, off, len, out);
}

int ty_text_translate(const ty_varlena_t *s, const ty_varlena_t *from,
                      const ty_varlena_t *to, ty_varlena_t **out) {
  bytes_t a(s), f(from), t(to);
  size_t outlen;
  char *d;

  if (a.n == 0) {
    *out = (ty_varlena_t *)s;
    return 0;
  }
  /* a character may be 4 bytes: PostgreSQL's limit is on 4 * the length */
  if (tyr_text_size_check(a.n * 4) != 0 || ty_vl_new(a.n * 4, out, &d) != 0) {
    return -1;
  }
  tyr_text_translate(a.p, a.n, f.p, f.n, t.p, t.n, d, &outlen);
  ty_vl_set_len(*out, outlen);
  return 0;
}

int ty_text_ascii(const ty_varlena_t *s, int32_t *out) {
  bytes_t a(s);

  return tyr_text_ascii(a.p, a.n, out);
}

int ty_text_chr(int32_t n, ty_varlena_t **out) {
  char buf[4];
  size_t len;

  if (tyr_text_chr(n, buf, &len) != 0) {
    return -1;
  }
  return ty_vl_new_copy(buf, len, out);
}

/*
 * Fit the bytes at s. If self is not NULL and s needs no change, the result
 * is self; else a new value.
 */
static int bpchar_make(const char *s, size_t len, size_t maxchars, int force,
                       int pad, const ty_varlena_t *self, ty_varlena_t **out) {
  size_t keep, total;
  char *d;

  if (tyr_bpchar_fit(s, len, maxchars, force, pad, &keep, &total) != 0) {
    return -1;
  }
  if (nullptr != self && keep == len && total == len) {
    *out = (ty_varlena_t *)self;
    return 0;
  }
  if (ty_vl_new(total, out, &d) != 0) {
    return -1;
  }
  memcpy(d, s, keep);
  if (total > keep) {
    memset(d + keep, ' ', total - keep);
  }
  return 0;
}

int ty_bpchar_in(const char *s, size_t len, size_t maxchars, int force, int pad,
                 ty_varlena_t **out) {
  return bpchar_make(s, len, maxchars, force, pad, nullptr, out);
}

int ty_bpchar_fit(const ty_varlena_t *s, size_t maxchars, int force, int pad,
                  ty_varlena_t **out) {
  bytes_t a(s);

  return bpchar_make(a.p, a.n, maxchars, force, pad, s, out);
}

int ty_bpchar_cmp(const ty_varlena_t *a, const ty_varlena_t *b) {
  bytes_t x(a), y(b);

  return tyr_bpchar_cmp(x.p, x.n, y.p, y.n);
}

int ty_bpchar_eq(const ty_varlena_t *a, const ty_varlena_t *b) {
  bytes_t x(a), y(b);

  return tyr_text_eq(x.p, tyr_bpchar_truelen(x.p, x.n), y.p,
                     tyr_bpchar_truelen(y.p, y.n));
}

uint32_t ty_bpchar_hash(const ty_varlena_t *s) {
  bytes_t a(s);

  return tyr_bpchar_hash(a.p, a.n);
}

uint64_t ty_bpchar_hash_extended(const ty_varlena_t *s, uint64_t seed) {
  bytes_t a(s);

  return tyr_bpchar_hash_extended(a.p, a.n, seed);
}

void ty_name_from_text(const ty_varlena_t *s, int trim, char *out) {
  bytes_t a(s);
  size_t n = a.n;

  if (n >= TY_NAMEDATALEN) {
    n = tyr_text_clipbytes(a.p, n, TY_NAMEDATALEN - 1);
  }
  if (trim) {
    n = tyr_bpchar_truelen(a.p, n);
  }
  memset(out, 0, TY_NAMEDATALEN);
  memcpy(out, a.p, n);
}

void ty_name_parse(const char *s, size_t len, char *out) {
  tyr_name_parse(s, len, out);
}

int ty_text_like(const ty_varlena_t *t, const ty_varlena_t *p, int utf8) {
  bytes_t a(t), b(p);

  return tyr_text_like(a.p, a.n, b.p, b.n, utf8);
}

int ty_text_like_bytes(const char *t, size_t tlen, const char *p, size_t plen,
                       int utf8) {
  return tyr_text_like(t, tlen, p, plen, utf8);
}

int ty_text_like_escape(const ty_varlena_t *pat, const ty_varlena_t *esc,
                        int utf8, ty_varlena_t **out) {
  bytes_t a(pat), e(esc);
  ty_varlena_t *r;
  size_t outlen;
  char *d;

  /* the result is at most twice as long: a backslash is doubled */
  if (ty_vl_new(a.n * 2, &r, &d) != 0) {
    return -1;
  }
  if (tyr_text_like_escape(a.p, a.n, e.p, e.n, utf8, d, &outlen) != 0) {
    ty_free(r);
    return -1;
  }
  ty_vl_set_len(r, outlen);
  *out = r;
  return 0;
}

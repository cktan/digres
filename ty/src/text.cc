/*
 * Copyright (c) 2026 CK Tan
 * SPDX-License-Identifier: MIT
 */

/*
 * text.cc -- the operations of UTF-8 text.
 */
#include "ty_text.h"

#include <stdio.h>
#include <string.h>

#include "error.h"

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
size_t ty_text_length(const char *s, size_t len) {
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
int ty_text_substr(const char *s, size_t len, int32_t start, int32_t count,
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
void ty_text_left(const char *s, size_t len, int32_t n, size_t *off,
                  size_t *outlen) {
  int64_t k = n;

  if (n < 0) {
    k = (int64_t)ty_text_length(s, len) + n;
  }
  *off = 0;
  *outlen = (size_t)(advance(s, s + len, k) - s);
}

/* The last n characters, or all but the first -n. */
void ty_text_right(const char *s, size_t len, int32_t n, size_t *off,
                   size_t *outlen) {
  int64_t k;

  if (n < 0) {
    k = n == INT32_MIN ? INT32_MAX : -(int64_t)n;
  } else {
    k = (int64_t)ty_text_length(s, len) - n;
  }
  *off = (size_t)(advance(s, s + len, k) - s);
  *outlen = len - *off;
}

/* Reverse by characters. */
void ty_text_reverse(const char *s, size_t len, char *out) {
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
int ty_text_overlay(const char *s, size_t len, const char *r, size_t rlen,
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
  ty_text_substr(s, len, 1, start - 1, 1, &off1, &len1);
  /* the tail: from start + count to the end (a start below 1 is 1) */
  ty_text_substr(s, len, sum, 0, 0, &off2, &len2);
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
void ty_text_find_init(ty_text_find_t *f, const char *s, size_t len,
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
int64_t ty_text_find_next(ty_text_find_t *f) {
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
int32_t ty_text_position(const char *s, size_t len, const char *needle,
                         size_t nlen) {
  if (nlen < 1) {
    return 1;
  }
  if (len < nlen) {
    return 0;
  }
  ty_text_find_t f;
  int64_t m;

  ty_text_find_init(&f, s, len, needle, nlen);
  m = ty_text_find_next(&f);
  return m < 0 ? 0 : (int32_t)(ty_text_length(s, (size_t)m) + 1);
}

/* The size of the result of replace: count the copies. */
int ty_text_replace_size(const char *s, size_t len, const char *from,
                         size_t flen, size_t tlen, size_t *outlen, int *found) {
  *found = 0;
  *outlen = len;
  if (len < 1 || flen < 1) {
    return 0;
  }
  ty_text_find_t f;
  size_t n = 0;

  ty_text_find_init(&f, s, len, from, flen);
  while (ty_text_find_next(&f) >= 0) {
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
void ty_text_replace(const char *s, size_t len, const char *from, size_t flen,
                     const char *to, size_t tlen, char *out) {
  ty_text_find_t f;
  size_t start = 0;
  int64_t m;

  ty_text_find_init(&f, s, len, from, flen);
  while ((m = ty_text_find_next(&f)) >= 0) {
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
int ty_text_split_part(const char *s, size_t len, const char *sep,
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
  ty_text_find_t f;
  int64_t k = n;

  if (n < 0) {
    int64_t fields = 1;

    ty_text_find_init(&f, s, len, sep, seplen);
    while (ty_text_find_next(&f) >= 0) {
      fields++;
    }
    k = fields + n + 1;
    if (k <= 0) {
      return 0;
    }
  }
  ty_text_find_init(&f, s, len, sep, seplen);
  size_t start = 0;
  int64_t m = -1;

  /* walk past k - 1 copies: the field starts after the last of them */
  for (int64_t i = 1; i < k; i++) {
    m = ty_text_find_next(&f);
    if (m < 0) {
      return 0; /* there is no such field */
    }
    start = (size_t)m + seplen;
  }
  m = ty_text_find_next(&f);
  *off = start;
  *outlen = (m < 0 ? len : (size_t)m) - start;
  return 0;
}

/* The size of repeat. */
int ty_text_repeat_size(size_t len, int32_t count, size_t *outlen) {
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
void ty_text_repeat(const char *s, size_t len, int32_t count, char *out) {
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
int ty_text_size_check(size_t bytes) {
  return bytes > TY_TEXT_MAX_LEN ? too_large() : 0;
}

/*
 * The length of the result of lpad or rpad, in characters, and PostgreSQL's
 * limit: 4 bytes a character.
 */
int ty_text_pad_prepare(const char *s, size_t len, int32_t n, size_t flen,
                        int32_t *eff) {
  int64_t want = n < 0 ? 0 : n;
  int64_t have = (int64_t)ty_text_length(s, len);

  if (have > want) {
    have = want;
  }
  if (flen <= 0) {
    want = have; /* nothing to pad with, so do not pad */
  }
  if (ty_text_size_check((size_t)want * 4) != 0) {
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
void ty_text_lpad(const char *s, size_t len, int32_t eff, const char *fill,
                  size_t flen, char *out, size_t *outlen) {
  int64_t have = (int64_t)ty_text_length(s, len);

  if (have > eff) {
    have = eff;
  }
  char *p = put_fill(out, eff - have, fill, flen);

  p = put_head(p, s, len, have);
  *outlen = (size_t)(p - out);
}

/* rpad: the first characters of s, then the fill. */
void ty_text_rpad(const char *s, size_t len, int32_t eff, const char *fill,
                  size_t flen, char *out, size_t *outlen) {
  int64_t have = (int64_t)ty_text_length(s, len);

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
void ty_text_trim(const char *s, size_t len, const char *set, size_t setlen,
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
void ty_text_translate(const char *s, size_t len, const char *from, size_t flen,
                       const char *to, size_t tlen, char *out, size_t *outlen) {
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
int ty_text_ascii(const char *s, size_t len, int32_t *out) {
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
int ty_text_chr(int32_t n, char *out, size_t *outlen) {
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

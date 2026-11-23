/*
 * Copyright (c) 2026 CK Tan
 * SPDX-License-Identifier: MIT
 */

/*
 * format.cc -- format(): the parse of its format string, the padding of an
 * argument, and the result. The host gives each argument's text
 * (ty_text.h). The cases and the errors are PostgreSQL's text_format().
 */
#include <stdio.h>
#include <string.h>

#include "error.h"
#include "text_raw.h"
#include "ty_text.h"

namespace {

/* PostgreSQL's MaxAllocSize: a StringInfo holds fewer bytes than this. */
constexpr size_t k_limit = 0x3fffffff;

/* The error of a conversion that the format string ends in. */
int unterminated() {
  return ty_internal::set_error(TY_ERR_INVALID,
                                "unterminated format() type specifier");
}

/* The error of an argument numbered 0. */
int argument_zero() {
  return ty_internal::set_error(
      TY_ERR_INVALID,
      "format specifies argument 0, but arguments are numbered from 1");
}

/* Free the result, which an error ends; returns -1. */
int fail(ty_format_t *f) {
  ty_format_abort(f);
  return -1;
}

/*
 * Whether a StringInfo of had bytes refuses need more: then note the two
 * for the host's detail, and set the error.
 */
bool past_limit(ty_format_t *f, size_t had, size_t need) {
  if (need < k_limit - had) {
    return false;
  }
  f->had = had;
  f->need = need;
  ty_internal::set_error(
      TY_ERR_RANGE,
      "string buffer exceeds maximum allowed length (1073741823 bytes)");
  return true;
}

/*
 * Make room for need more bytes of text.
 * 1. Refuse a result of k_limit bytes or more, as a StringInfo does
 *    (past_limit).
 * 2. Refuse a text longer than a varlena holds; that is the 3 bytes of a
 *    header PostgreSQL adds after the StringInfo.
 * 3. Take a buffer twice as large, or as large as the text needs, but no
 *    larger than a varlena holds, and copy the text into it: ty has no
 *    realloc.
 */
int grow(ty_format_t *f, size_t need) {
  if (past_limit(f, f->len, need)) {
    return -1;
  }
  if (f->len + need > TY_VARLENA_MAX_LEN) {
    return ty_vl_new_error(f->len + need);
  }
  size_t cap = f->cap * 2;

  if (cap < f->len + need) {
    cap = f->len + need;
  }
  if (cap < 64) {
    cap = 64;
  }
  if (cap > TY_VARLENA_MAX_LEN) {
    cap = TY_VARLENA_MAX_LEN;
  }
  char *buf = (char *)ty_alloc(cap + 4);

  if (nullptr == buf) {
    return ty_internal::set_error(TY_ERR_NOMEM, "out of memory");
  }
  if (nullptr != f->buf) {
    memcpy(buf + 4, f->buf + 4, f->len);
    ty_free(f->buf);
  }
  f->buf = buf;
  f->cap = cap;
  return 0;
}

/* Add len bytes at s to the result. */
inline int add(ty_format_t *f, const char *s, size_t len) {
  if (0 == len) {
    return 0;
  }
  if (f->cap - f->len < len && 0 != grow(f, len)) {
    return -1;
  }
  memcpy(f->buf + 4 + f->len, s, len);
  f->len += len;
  return 0;
}

/*
 * Step past the byte at *p. A format string that ends there is an error: a
 * conversion needs a byte after it.
 */
inline int step(const char **p, const char *end) {
  if (++*p >= end) {
    return unterminated();
  }
  return 0;
}

/*
 * Read the digits at *p as a number into *value. *found is 1 if there was
 * one. A number past int32 is TY_ERR_RANGE; a format that ends in the
 * digits is unterminated.
 */
inline int digits(const char **p, const char *end, int32_t *value, int *found) {
  int32_t v = 0;

  *found = 0;
  while (**p >= '0' && **p <= '9') {
    if (__builtin_mul_overflow(v, 10, &v) ||
        __builtin_add_overflow(v, **p - '0', &v)) {
      return ty_internal::set_error(TY_ERR_RANGE, "number is out of range");
    }
    if (0 != step(p, end)) {
      return -1;
    }
    *found = 1;
  }
  *value = v;
  return 0;
}

/*
 * Read [argpos$][-][width or *[widthpos$]] at *p, after the %; *p is then at
 * the conversion's letter.
 * 1. A number first is the argument's place if a $ follows it, else the
 *    width, and the letter follows.
 * 2. Each - sets the flag.
 * 3. A * takes the width from an argument: the next one, or the one a
 *    number and a $ name.
 * 4. Else a number is the width.
 */
int parse_spec(const char **p, const char *end, ty_format_spec_t *spec) {
  int32_t n;
  int found;

  spec->minus = 0;
  spec->argpos = 0;
  spec->widthpos = -1;
  spec->width = 0;
  if (0 != digits(p, end, &n, &found)) {
    return -1;
  }
  if (found) {
    if ('$' != **p) {
      spec->width = n;
      return 0;
    }
    if (0 == n) {
      return argument_zero();
    }
    spec->argpos = n;
    if (0 != step(p, end)) {
      return -1;
    }
  }
  while ('-' == **p) {
    spec->minus = 1;
    if (0 != step(p, end)) {
      return -1;
    }
  }
  if ('*' == **p) {
    if (0 != step(p, end) || 0 != digits(p, end, &n, &found)) {
      return -1;
    }
    if (!found) {
      spec->widthpos = 0;
      return 0;
    }
    if ('$' != **p) {
      return ty_internal::set_error(
          TY_ERR_INVALID, "width argument position must be ended by \"$\"");
    }
    if (0 == n) {
      return argument_zero();
    }
    spec->widthpos = n;
    return step(p, end);
  }
  if (0 != digits(p, end, &n, &found)) {
    return -1;
  }
  if (found) {
    spec->width = n;
  }
  return 0;
}

/* The bytes of the UTF-8 character that begins with byte b: 1 to 4. */
inline size_t char_len(unsigned char b) {
  return (b & 0x80) == 0      ? 1
         : (b & 0xe0) == 0xc0 ? 2
         : (b & 0xf0) == 0xe0 ? 3
         : (b & 0xf8) == 0xf0 ? 4
                              : 1;
}

/* The error of a letter that is not s, I or L: the character at p. */
int unrecognized(const char *p, const char *end) {
  char msg[64];
  size_t n = char_len((unsigned char)*p);

  if ((size_t)(end - p) < n) {
    n = (size_t)(end - p);
  }
  snprintf(msg, sizeof msg, "unrecognized format() type specifier \"%.*s\"",
           (int)n, p);
  return ty_internal::set_error(TY_ERR_INVALID, msg);
}

} // namespace

/* Start a format of the len bytes at fmt; nothing is allocated yet. */
void ty_format_begin(ty_format_t *f, const char *fmt, size_t len) {
  f->p = fmt;
  f->end = fmt + len;
  f->buf = nullptr;
  f->len = 0;
  f->cap = 0;
  f->had = 0;
  f->need = 0;
}

namespace {

/*
 * Read the conversion at f->p, the byte after its %, and check its letter:
 * 1, or -1 with the result freed.
 */
inline int conversion(ty_format_t *f, ty_format_spec_t *spec) {
  char c = *f->p;

  /* the usual %s, %I and %L: no place, flag or width to read */
  if ('s' == c || 'I' == c || 'L' == c) {
    spec->conv = c;
    spec->minus = 0;
    spec->argpos = 0;
    spec->widthpos = -1;
    spec->width = 0;
    f->p++;
    return 1;
  }
  if (0 != parse_spec(&f->p, f->end, spec)) {
    return fail(f);
  }
  if ('s' != *f->p && 'I' != *f->p && 'L' != *f->p) {
    unrecognized(f->p, f->end);
    return fail(f);
  }
  spec->conv = *f->p++;
  return 1;
}

/*
 * ty_format_next for a result that may pass the limit: each piece of text is
 * added as it is found, so the error comes where PostgreSQL's would.
 */
int next_near_limit(ty_format_t *f, ty_format_spec_t *spec) {
  for (;;) {
    size_t rest = (size_t)(f->end - f->p);
    const char *pct = (const char *)memchr(f->p, '%', rest);

    if (nullptr == pct) {
      if (0 != add(f, f->p, rest)) {
        return fail(f);
      }
      f->p = f->end;
      return 0;
    }
    if (0 != add(f, f->p, (size_t)(pct - f->p))) {
      return fail(f);
    }
    f->p = pct;
    if (0 != step(&f->p, f->end)) {
      return fail(f);
    }
    if ('%' != *f->p) {
      return conversion(f, spec);
    }
    if (0 != add(f, "%", 1)) {
      return fail(f);
    }
    f->p++;
  }
}

} // namespace

/*
 * Give the next conversion.
 * 1. Make room for the rest of the format: no call adds more text than that.
 *    A result that could then pass the limit takes next_near_limit.
 * 2. Copy the text up to the next %, a byte at a time: the pieces are short,
 *    and a call of memchr and memcpy costs more.
 * 3. Step past the %: %% adds a %, and the copy goes on.
 * 4. Read the conversion, and check its letter.
 */
int ty_format_next(ty_format_t *f, ty_format_spec_t *spec) {
  const char *p = f->p;
  const char *end = f->end;
  size_t rest = (size_t)(end - p);

  if (0 == rest) {
    return 0;
  }
  if (f->cap - f->len < rest) {
    if (f->len + rest > TY_VARLENA_MAX_LEN) {
      return next_near_limit(f, spec);
    }
    if (0 != grow(f, rest)) {
      return fail(f);
    }
  }
  char *base = f->buf + 4;
  char *d = base + f->len;

  for (;;) {
    while (p < end && '%' != *p) {
      *d++ = *p++;
    }
    if (p == end || p + 1 == end || '%' != p[1]) {
      break;
    }
    *d++ = '%';
    p += 2;
  }
  f->len = (size_t)(d - base);
  f->p = p;
  if (p == end) {
    return 0;
  }
  if (0 != step(&f->p, end)) {
    return fail(f);
  }
  return conversion(f, spec);
}

/*
 * Add an argument's text, padded.
 * 1. With no width, add the text.
 * 2. A negative width justifies to the left, as the flag does.
 * 3. The spaces are what the text's characters leave of the width.
 * 4. Check the limit as PostgreSQL's two appends would, the first and then
 *    the second, before anything is allocated.
 * 5. Add the text and the spaces: the spaces after the text on the left,
 *    before it on the right.
 */
int ty_format_put_full(ty_format_t *f, const char *s, size_t len, int minus,
                       int32_t width) {
  if (0 == width) {
    return 0 == add(f, s, len) ? 0 : fail(f);
  }
  if (width < 0) {
    if (INT32_MIN == width) {
      ty_internal::set_error(TY_ERR_RANGE, "number is out of range");
      return fail(f);
    }
    minus = 1;
    width = -width;
  }
  size_t chars = tyr_text_length(s, len);
  size_t pad = chars < (size_t)width ? (size_t)width - chars : 0;
  size_t first = minus ? len : pad;

  if (past_limit(f, f->len, first) ||
      past_limit(f, f->len + first, minus ? pad : len) ||
      (f->cap - f->len < len + pad && 0 != grow(f, len + pad))) {
    return fail(f);
  }
  char *d = f->buf + 4 + f->len;

  if (minus) {
    memcpy(d, s, len);
    memset(d + len, ' ', pad);
  } else {
    memset(d, ' ', pad);
    memcpy(d + pad, s, len);
  }
  f->len += len + pad;
  return 0;
}

/* Give the result as a text: the buffer itself, with its header. */
int ty_format_finish(ty_format_t *f, ty_varlena_t **out) {
  if (nullptr == f->buf) {
    return ty_vl_new_copy("", 0, out);
  }
  ty_vl_set_len((ty_varlena_t *)f->buf, f->len);
  *out = (ty_varlena_t *)f->buf;
  f->buf = nullptr;
  return 0;
}

/* Free the result a format was making, if any. */
void ty_format_abort(ty_format_t *f) {
  if (nullptr != f->buf) {
    ty_free(f->buf);
    f->buf = nullptr;
  }
  f->len = 0;
  f->cap = 0;
}

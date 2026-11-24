/*
 * Copyright (c) 2026 CK Tan
 * SPDX-License-Identifier: MIT
 */

/*
 * string_agg.cc -- the state of string_agg over text and bytea: the values
 * and their delimiters, its parts joined, written to bytes and read back,
 * and the result (ty_text.h). The cases and the errors are PostgreSQL's.
 */
#include <string.h>

#include "error.h"
#include "ty_text.h"

namespace {

/* PostgreSQL's MaxAllocSize: a StringInfo holds fewer bytes than this. */
constexpr size_t k_limit = 0x3fffffff;

/*
 * Whether a StringInfo of had bytes refuses need more: then note the two
 * for the host's detail, and set the error.
 */
bool past_limit(ty_string_agg_t *st, size_t had, size_t need) {
  if (need < k_limit - had) {
    return false;
  }
  st->had = had;
  st->need = need;
  ty_internal::set_error(
      TY_ERR_RANGE,
      "string buffer exceeds maximum allowed length (1073741823 bytes)");
  return true;
}

/*
 * Make room for need more bytes, which past_limit has allowed.
 * 1. Take a buffer twice as large, or as large as the bytes need, at least
 *    64 bytes, and less than the limit.
 * 2. Copy the bytes into it, and free the old one: ty has no realloc.
 */
int grow(ty_string_agg_t *st, size_t need) {
  size_t cap = st->cap * 2;

  if (cap < st->len + need) {
    cap = st->len + need;
  }
  if (cap < 64) {
    cap = 64;
  }
  if (cap > k_limit - 1) {
    cap = k_limit - 1;
  }
  char *buf = (char *)ty_alloc(cap);

  if (nullptr == buf) {
    return ty_internal::set_error(TY_ERR_NOMEM, "out of memory");
  }
  if (nullptr != st->buf) {
    memcpy(buf, st->buf, st->len);
    ty_free(st->buf);
  }
  st->buf = buf;
  st->cap = cap;
  return 0;
}

/* Add len bytes at s, which past_limit has allowed. */
int add(ty_string_agg_t *st, const char *s, size_t len) {
  if (0 == len) {
    return 0;
  }
  if (st->cap - st->len < len && 0 != grow(st, len)) {
    return -1;
  }
  memcpy(st->buf + st->len, s, len);
  st->len += len;
  return 0;
}

} // namespace

/*
 * Add a delimiter and a value.
 * 1. Check the limit as PostgreSQL's two appends would, the delimiter's and
 *    then the value's, before anything is allocated.
 * 2. The first value notes its delimiter's length, which finish cuts off.
 * 3. Add the two.
 */
int ty_string_agg_add_full(ty_string_agg_t *st, const char *d, size_t dlen,
                           const char *v, size_t vlen) {
  if (past_limit(st, st->len, dlen) || past_limit(st, st->len + dlen, vlen)) {
    return -1;
  }
  if (st->cap - st->len < dlen + vlen && 0 != grow(st, dlen + vlen)) {
    return -1;
  }
  if (!st->started) {
    st->started = 1;
    st->first = dlen;
  }
  return 0 == add(st, d, dlen) && 0 == add(st, v, vlen) ? 0 : -1;
}

/* Add b's bytes to a; a zeroed a takes b's first delimiter. */
int ty_string_agg_combine(ty_string_agg_t *a, const ty_string_agg_t *b) {
  if (past_limit(a, a->len, b->len) || 0 != add(a, b->buf, b->len)) {
    return -1;
  }
  if (!a->started) {
    a->started = 1;
    a->first = b->first;
  }
  return 0;
}

/* Write the first delimiter's length, most significant byte first. */
int ty_string_agg_serialize(const ty_string_agg_t *st, ty_varlena_t **out) {
  char *p;

  if (0 != ty_vl_new(4 + st->len, out, &p)) {
    return -1;
  }
  uint32_t n = (uint32_t)st->first;

  p[0] = (char)(n >> 24);
  p[1] = (char)(n >> 16);
  p[2] = (char)(n >> 8);
  p[3] = (char)n;
  if (0 != st->len) {
    memcpy(p + 4, st->buf, st->len);
  }
  return 0;
}

/* Read the first delimiter's length and the bytes that serialize wrote. */
int ty_string_agg_deserialize(ty_string_agg_t *st, const char *p, size_t len) {
  if (len < 4) {
    return ty_internal::set_error(TY_ERR_INVALID,
                                  "insufficient data left in message");
  }
  const unsigned char *u = (const unsigned char *)p;

  if (past_limit(st, 0, len - 4) || 0 != add(st, p + 4, len - 4)) {
    return -1;
  }
  st->started = 1;
  st->first = (size_t)u[0] << 24 | (size_t)u[1] << 16 | (size_t)u[2] << 8 |
              (size_t)u[3];
  return 0;
}

/* Copy the bytes after the first delimiter into a new value. */
int ty_string_agg_finish(const ty_string_agg_t *st, ty_varlena_t **out) {
  if (nullptr == st->buf) {
    return ty_vl_new_copy("", 0, out);
  }
  return ty_vl_new_copy(st->buf + st->first, st->len - st->first, out);
}

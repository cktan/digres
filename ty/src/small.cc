/*
 * Copyright (c) 2026 CK Tan
 * SPDX-License-Identifier: MIT
 */

/*
 * small.cc -- the text of tid and pg_lsn.
 */
#include "ty_small.h"

#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "error.h"
#include "ty_int.h"

namespace {

/* The error for a text that is not the type's. */
int bad_text(const char *what) {
  return ty_internal::set_error(TY_ERR_INVALID, what);
}

/* Write v as upper case hex; return the characters. */
size_t put_hex_upper(char *dst, uint32_t v) {
  static const char hex[] = "0123456789ABCDEF";
  char tmp[8];
  size_t n = 0, len;

  do {
    tmp[n++] = hex[v & 15];
    v >>= 4;
  } while (v != 0);
  for (len = 0; len < n; len++) {
    dst[len] = tmp[n - 1 - len];
  }
  return len;
}

} // namespace

/*
 * Read "(block,offset)", as PostgreSQL's tidin does.
 * 1. Find the places after the "(" and after the ",", up to the ")".
 * 2. The block is a number that fits 32 bits, or the 64-bit sign extension
 *    of one (strtoul takes a minus sign), then the ",".
 * 3. The offset is a number up to 65535, then the ")".
 */
int ty_tid_parse(const char *s, ty_tid_t *out) {
  const char *coord[2];
  const char *p;
  int i;
  char *badp;
  unsigned long cvt;

  for (i = 0, p = s; *p && i < 2 && *p != ')'; p++) {
    if (*p == ',' || (*p == '(' && i == 0)) {
      coord[i++] = p + 1;
    }
  }
  if (i < 2) {
    return bad_text("invalid tid text");
  }
  errno = 0;
  cvt = strtoul(coord[0], &badp, 10);
  if (errno || *badp != ',') {
    return bad_text("invalid tid text");
  }
  uint32_t block = (uint32_t)cvt;

  if (cvt != (unsigned long)block && cvt != (unsigned long)(int32_t)block) {
    return bad_text("invalid tid text");
  }
  cvt = strtoul(coord[1], &badp, 10);
  if (errno || *badp != ')' || cvt > USHRT_MAX) {
    return bad_text("invalid tid text");
  }
  ty_tid_set(out, block, (uint16_t)cvt);
  return 0;
}

/* Write "(block,offset)". */
void ty_tid_format(const ty_tid_t *t, char *out) {
  char *p = out;

  *p++ = '(';
  p += ty_uint32_format_n(ty_tid_block(t), p);
  *p++ = ',';
  p += ty_uint32_format_n(t->offset, p);
  *p++ = ')';
  *p = '\0';
}

/*
 * Read "X/Y".
 * 1. 1 to 8 hex digits, then a slash.
 * 2. 1 to 8 hex digits, then the end of the text.
 */
int ty_lsn_parse(const char *s, uint64_t *out) {
  static const char digits[] = "0123456789abcdefABCDEF";
  size_t len1 = strspn(s, digits), len2;

  if (len1 < 1 || len1 > 8 || s[len1] != '/') {
    return bad_text("invalid pg_lsn text");
  }
  len2 = strspn(s + len1 + 1, digits);
  if (len2 < 1 || len2 > 8 || s[len1 + 1 + len2] != '\0') {
    return bad_text("invalid pg_lsn text");
  }
  uint32_t id = (uint32_t)strtoul(s, nullptr, 16);
  uint32_t off = (uint32_t)strtoul(s + len1 + 1, nullptr, 16);

  *out = ((uint64_t)id << 32) | off;
  return 0;
}

/* Write "X/Y" in upper case hex. */
void ty_lsn_format(uint64_t lsn, char *out) {
  char *p = out;

  p += put_hex_upper(p, (uint32_t)(lsn >> 32));
  *p++ = '/';
  p += put_hex_upper(p, (uint32_t)lsn);
  *p = '\0';
}

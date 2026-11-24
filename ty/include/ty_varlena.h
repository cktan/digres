/*
 * Copyright (c) 2026 CK Tan
 * SPDX-License-Identifier: MIT
 */

/*
 * ty_varlena.h -- the varlena, the form of a value of variable width: text,
 * bytea, character(n) and character varying(n). It is PostgreSQL's, so that
 * digres passes its Datum to ty, and takes ty's result, with no copy.
 *
 * A varlena has a header, then the bytes:
 *   - a short header is 1 byte: the size of the whole varlena, header
 *     included, up to 127 (the byte is size << 1 | 1 on a little-endian
 *     machine, and size | 0x80 on a big-endian one);
 *   - a long header is 4 bytes: the size of the whole varlena, header
 *     included, up to 0x3fffffff (size << 2 on a little-endian machine).
 *
 * ty reads both. ty does not handle a toasted varlena, one that is stored
 * outside the row, or compressed: the host gives ty the bytes of the value
 * as they are, never the toast. A value that ty makes has a long header.
 * The function that makes a value allocates it with the thread's alloc
 * function (ty_set_alloc); the host frees it as its alloc function says.
 */
#ifndef TY_VARLENA_H
#define TY_VARLENA_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ty.h"

/* A varlena: only its address is used. */
typedef struct ty_varlena_t ty_varlena_t;

/* The most bytes of a value: PostgreSQL's MaxAllocSize less its header. */
#define TY_VARLENA_MAX_LEN ((size_t)0x3fffffff - 4)

#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#define TY_VL_SHORT(b0) (((b0) & 0x80) != 0)
#define TY_VL_SHORT_SIZE(b0) ((size_t)((b0) & 0x7f))
#define TY_VL_LONG_SIZE(h) ((size_t)((h) & 0x3fffffff))
#define TY_VL_LONG_HEADER(size) ((uint32_t)(size))
#else
#define TY_VL_SHORT(b0) (((b0) & 0x01) != 0)
#define TY_VL_SHORT_SIZE(b0) ((size_t)(((b0) >> 1) & 0x7f))
#define TY_VL_LONG_SIZE(h) ((size_t)(((h) >> 2) & 0x3fffffff))
#define TY_VL_LONG_HEADER(size) ((uint32_t)(size) << 2)
#endif

/* The bytes of the value, after the header. */
static inline const char *ty_vl_data(const ty_varlena_t *v) {
  const unsigned char *p = (const unsigned char *)v;

  return (const char *)p + (TY_VL_SHORT(p[0]) ? 1 : 4);
}

/* The number of bytes of the value, the header not counted. */
static inline size_t ty_vl_len(const ty_varlena_t *v) {
  const unsigned char *p = (const unsigned char *)v;
  uint32_t h;

  if (TY_VL_SHORT(p[0])) {
    return TY_VL_SHORT_SIZE(p[0]) - 1;
  }
  memcpy(&h, p, 4);
  return TY_VL_LONG_SIZE(h) - 4;
}

/*
 * The error of ty_vl_new: TY_ERR_RANGE ("requested length too large") if the
 * value may not be so long, TY_ERR_NOMEM ("out of memory") otherwise.
 */
int ty_vl_new_error(size_t len);

/*
 * A new value of len bytes, with a long header, in *out; *data is the place
 * of its bytes, which the caller writes. -1 with the thread's error set if
 * len passes TY_VARLENA_MAX_LEN or the alloc function returns NULL.
 */
static inline int ty_vl_new(size_t len, ty_varlena_t **out, char **data) {
  unsigned char *p;
  uint32_t h;

  if (TY_UNLIKELY(len > TY_VARLENA_MAX_LEN)) {
    ty_vl_new_error(len);
    return -1; /* a compiler sees that *out is set on success only */
  }
  p = (unsigned char *)ty_alloc(len + 4);
  if (TY_UNLIKELY(p == NULL)) {
    ty_vl_new_error(len);
    return -1;
  }
  h = TY_VL_LONG_HEADER(len + 4);
  memcpy(p, &h, 4);
  *out = (ty_varlena_t *)p;
  *data = (char *)p + 4;
  return 0;
}

/* Set the length of a value that ty_vl_new made, to len or less. */
static inline void ty_vl_set_len(ty_varlena_t *v, size_t len) {
  uint32_t h = TY_VL_LONG_HEADER(len + 4);

  memcpy((void *)v, &h, 4);
}

/* A new value that is a copy of len bytes at s; as ty_vl_new. */
static inline int ty_vl_new_copy(const char *s, size_t len,
                                 ty_varlena_t **out) {
  char *d;

  if (ty_vl_new(len, out, &d) != 0) {
    return -1;
  }
  memcpy(d, s, len);
  return 0;
}

#endif /* TY_VARLENA_H */

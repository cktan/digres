/*
 * Copyright (c) 2026 CK Tan
 * SPDX-License-Identifier: MIT
 */

/*
 * ty_vec.h -- the operations of ty on vectors: one call on the n values
 * of a column at once, for a columnar executor (../../REPORT.md §9).
 *
 * A vector is n values of one type, in an array, with a flag a row: an
 * array of n bytes. Bit 0 of a row's flag, TY_VEC_NOTVALID, is 1 if the
 * row is not valid, such as a row a filter dropped; bit 1, TY_VEC_NULL,
 * is 1 if it is null. The other bits are 0 for now. An operation's
 * inputs share one flag array, inflag: a row is null if one of its
 * inputs is.
 *
 * An operation computes each row with neither bit set, as the scalar
 * operation of ty_int.h would, and writes:
 *   - out, the n results. A row not valid, or null, gets a value that
 *     means nothing; it is never an error.
 *   - outflag, the n flags of the results, of the same shape as inflag.
 *     For add, each is the row's inflag: a null input gives a null sum.
 * It returns 0, or -1 at once on the first row computed, by index, whose
 * result does not fit: the thread's error is TY_ERR_RANGE, *bad is that
 * row's index, and out and outflag mean nothing.
 *
 * out may be a or b, and outflag may be inflag: a row's inputs are read
 * before its result is written. The additions run on the machine's
 * vector instructions (src/vec.cc).
 *
 *   ty_intN_add_vec(n, inflag, a, b, out, outflag, &bad)
 *       out[i] = a[i] + b[i]. RANGE on the first row computed that
 *       overflows.
 */
#ifndef TY_VEC_H
#define TY_VEC_H

#include <stdint.h>

#include "ty.h"

/* Bit 0 of a row's flag: 1 if the row is not valid. */
#define TY_VEC_NOTVALID 0x01
/* Bit 1 of a row's flag: 1 if the row is null. */
#define TY_VEC_NULL 0x02
/* The bits of a row that is not computed: not valid, or null. */
#define TY_VEC_SKIP (TY_VEC_NOTVALID | TY_VEC_NULL)

int ty_int16_add_vec(uint32_t n, const uint8_t *inflag, const int16_t *a,
                     const int16_t *b, int16_t *out, uint8_t *outflag,
                     uint32_t *bad);
int ty_int32_add_vec(uint32_t n, const uint8_t *inflag, const int32_t *a,
                     const int32_t *b, int32_t *out, uint8_t *outflag,
                     uint32_t *bad);
int ty_int64_add_vec(uint32_t n, const uint8_t *inflag, const int64_t *a,
                     const int64_t *b, int64_t *out, uint8_t *outflag,
                     uint32_t *bad);

#endif

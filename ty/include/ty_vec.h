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
 * inputs share one flag array, flag: a row is null if one of its inputs
 * is.
 *
 * An operation computes each row with neither bit set, as the scalar
 * operation of ty_int.h would, and writes:
 *   - out, the n results. A row not valid, or null, gets a value that
 *     means nothing; it is never an error. (IS NULL has a result for a null
 *     row.) An operation that cannot fail may compute every row, as that
 *     is cheaper.
 *   - flag, the flags of the results, over those of the inputs. Add
 *     leaves each as it is: a null input gives a null sum.
 * It returns 0, or -1 at once on the first row computed, by index, whose
 * result does not fit: the thread's error is TY_ERR_RANGE, ty_erridx() is
 * that row's index, and out and flag mean nothing.
 *
 * out may be a or b, but not both at once: a row's inputs are read before
 * its result is written. The additions run on the machine's vector instructions
 * (src/vec.cc).
 *
 *   ty_intN_add_vec(n, flag, a, b, out)
 *       out[i] = a[i] + b[i]. RANGE on the first row computed that
 *       overflows.
 *
 *   ty_flag_isnull(n, flag, out)
 *       SQL's IS NULL: out[i] = 1 if row i is null, or else 0, so out is a
 *       vector of bool. No result is null: each TY_VEC_NULL becomes 0, and
 *       the other bits stay. A caller that needs the input's nulls
 *       afterwards copies flag first. It cannot fail: it returns 0. flag
 *       and out may not overlap.
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

int ty_int16_add_vec(uint32_t n, uint8_t *flag, const int16_t *a,
                     const int16_t *b, int16_t *out);
int ty_int32_add_vec(uint32_t n, uint8_t *flag, const int32_t *a,
                     const int32_t *b, int32_t *out);
int ty_int64_add_vec(uint32_t n, uint8_t *flag, const int64_t *a,
                     const int64_t *b, int64_t *out);
int ty_flag_isnull(uint32_t n, uint8_t *flag, uint8_t *out);

#endif

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
 * is. A caller ORs the flags of two inputs into it before the call.
 *
 * An operation computes each row with neither bit set, as the scalar
 * operation of ty_int.h would, and writes:
 *   - out, the n results. A row not valid, or null, gets a value that
 *     means nothing; it is never an error. (An IS test has a result for a
 *     null row.) An operation that cannot fail may compute every row, as that
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
 *   tyvec_intN_add(n, flag, a, b, out)
 *       out[i] = a[i] + b[i]. RANGE on the first row computed that
 *       overflows.
 *
 *   tyvec_flag_isnull(n, flag, out)
 *   tyvec_flag_isnotnull(n, flag, out)
 *   tyvec_bool_istrue(n, flag, vec, out)
 *   tyvec_bool_isfalse(n, flag, vec, out)
 *       SQL's IS NULL, IS NOT NULL, and IS TRUE and IS FALSE of the bools
 *       of vec, which are 0 or 1 in each row computed: out[i] = 1 if it is
 *       so, or else 0, so out is a vector of bool. A null row is neither
 *       true nor false. No result is null: each TY_VEC_NULL becomes 0, and
 *       the other bits stay. A caller that needs the input's nulls
 *       afterwards copies flag first.
 *
 *   tyvec_bool_not(n, flag, vec, out)
 *       SQL's NOT: out[i] = 1 if vec[i] is 0, and 0 if it is 1. NOT of a
 *       null is null: flag stays as it is.
 *
 *   tyvec_bool_and(n, flag, a, b, out)
 *   tyvec_bool_or(n, flag, a, b, out)
 *       out[i] = a[i] AND b[i], or OR, of bools that are 0 or 1 in each row
 *       computed. flag stays as it is. A row with a null input is null,
 *       which is not SQL's answer for false AND null, which is false, and
 *       for true OR null, which is true: flag does not say which input is
 *       null. out may be a, b or both.
 *
 * The flag and bool operations cannot fail: they return 0. out may be vec;
 * flag may not overlap an input or out.
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

int tyvec_int16_add(uint32_t n, uint8_t *flag, const int16_t *a,
                    const int16_t *b, int16_t *out);
int tyvec_int32_add(uint32_t n, uint8_t *flag, const int32_t *a,
                    const int32_t *b, int32_t *out);
int tyvec_int64_add(uint32_t n, uint8_t *flag, const int64_t *a,
                    const int64_t *b, int64_t *out);
int tyvec_flag_isnull(uint32_t n, uint8_t *flag, uint8_t *out);
int tyvec_flag_isnotnull(uint32_t n, uint8_t *flag, uint8_t *out);
int tyvec_bool_istrue(uint32_t n, uint8_t *flag, const uint8_t *vec,
                      uint8_t *out);
int tyvec_bool_isfalse(uint32_t n, uint8_t *flag, const uint8_t *vec,
                       uint8_t *out);
int tyvec_bool_not(uint32_t n, uint8_t *flag, const uint8_t *vec, uint8_t *out);
int tyvec_bool_and(uint32_t n, uint8_t *flag, const uint8_t *a,
                   const uint8_t *b, uint8_t *out);
int tyvec_bool_or(uint32_t n, uint8_t *flag, const uint8_t *a, const uint8_t *b,
                  uint8_t *out);

#endif

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
 * is. A caller ORs the flags of two inputs into it before the call. AND
 * and OR are the exception: they take the flags of each input.
 *
 * An operation computes each row with neither bit set, as the scalar
 * operation of ty_int.h or ty_float.h would, and writes:
 *   - out, the n results. A row not valid, or null, gets a value that
 *     means nothing; it is never an error. (An IS test has a result for a
 *     null row.) An operation that cannot fail may compute every row, as that
 *     is cheaper.
 *   - flag, the flags of the results, over those of the inputs. An
 *     operation on numbers leaves each as it is: a null input gives a null
 *     result.
 * It returns 0, or -1 at once on the first row computed, by index, that
 * fails: the thread's error is TY_ERR_RANGE if the result does not fit,
 * TY_ERR_DIVZERO, or TY_ERR_UNDERFLOW; ty_erridx() is that row's index; and
 * out and flag mean nothing.
 *
 * The operations run on the machine's vector instructions (src/vec.cc).
 *
 *   tyvec_intN_add(n, flag, a, b, out)
 *   tyvec_intN_sub(n, flag, a, b, out)
 *   tyvec_intN_mul(n, flag, a, b, out)
 *   tyvec_intN_div(n, flag, a, b, out)
 *   tyvec_intN_mod(n, flag, a, b, out)
 *       out[i] = a[i] + b[i], a[i] - b[i], a[i] * b[i], a[i] / b[i], to
 *       zero, or a[i] % b[i], with the sign of a[i]; MIN % -1 is 0. RANGE
 *       on a row computed that overflows, as MIN / -1 does, and DIVZERO on
 *       one divided by 0; a row not computed may have a b of 0. out may be
 *       a or b, as a row's inputs are read before its result is written;
 *       add does not allow that it is both.
 *
 *   tyvec_fpN_add(n, flag, a, b, out), and _sub, _mul, _div
 *       The same of float (fp32) and double (fp64). RANGE on a row computed
 *       whose result is infinite when no input is; UNDERFLOW on a product
 *       that is 0 when neither factor is, or a quotient that is 0 when the
 *       dividend is not and the divisor is finite; DIVZERO on one divided
 *       by 0, unless a[i] is NaN. out may be a, b or both.
 *
 *   tyvec_T_eq(n, flag, a, b, out), and _ne, _lt, _le, _gt, _ge
 *       out[i] = a[i] = b[i], <>, <, <=, > or >=, a bool. T is int16, int32,
 *       int64, fp32, fp64 or bool. All NaNs are equal, and above every
 *       number; false is below true. They cannot fail: they return 0. flag
 *       stays as it is: a null input gives a null result. out may be a or b
 *       if T is bool, and may not overlap them if not.
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
 *   tyvec_bool_and(n, flag_a, flag_b, a, b, out)
 *   tyvec_bool_or(n, flag_a, flag_b, a, b, out)
 *       SQL's AND and OR of bools that are 0 or 1 in each row not null:
 *       out[i] = a[i] AND b[i], or OR. flag_a is the flags of a, and then
 *       of the result; flag_b is those of b, and stays. A row is null if an
 *       input is, but false AND null is false, and true OR null is true.
 *       Only flag_a's TY_VEC_NULL changes, and flag_b's other bits are not
 *       read. An input that settles a row alone, a false of AND or a true
 *       of OR, makes the other's byte and flag in that row mean nothing: a
 *       caller need not compute b there. out may be a, b or both, and
 *       flag_b may be flag_a.
 *
 * The flag and bool operations cannot fail: they return 0. out may be vec;
 * a flag array may not overlap an input or out.
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
int tyvec_int16_sub(uint32_t n, uint8_t *flag, const int16_t *a,
                    const int16_t *b, int16_t *out);
int tyvec_int32_sub(uint32_t n, uint8_t *flag, const int32_t *a,
                    const int32_t *b, int32_t *out);
int tyvec_int64_sub(uint32_t n, uint8_t *flag, const int64_t *a,
                    const int64_t *b, int64_t *out);
int tyvec_int16_mul(uint32_t n, uint8_t *flag, const int16_t *a,
                    const int16_t *b, int16_t *out);
int tyvec_int32_mul(uint32_t n, uint8_t *flag, const int32_t *a,
                    const int32_t *b, int32_t *out);
int tyvec_int64_mul(uint32_t n, uint8_t *flag, const int64_t *a,
                    const int64_t *b, int64_t *out);
int tyvec_int16_div(uint32_t n, uint8_t *flag, const int16_t *a,
                    const int16_t *b, int16_t *out);
int tyvec_int32_div(uint32_t n, uint8_t *flag, const int32_t *a,
                    const int32_t *b, int32_t *out);
int tyvec_int64_div(uint32_t n, uint8_t *flag, const int64_t *a,
                    const int64_t *b, int64_t *out);
int tyvec_int16_mod(uint32_t n, uint8_t *flag, const int16_t *a,
                    const int16_t *b, int16_t *out);
int tyvec_int32_mod(uint32_t n, uint8_t *flag, const int32_t *a,
                    const int32_t *b, int32_t *out);
int tyvec_int64_mod(uint32_t n, uint8_t *flag, const int64_t *a,
                    const int64_t *b, int64_t *out);
int tyvec_fp32_add(uint32_t n, uint8_t *flag, const float *a, const float *b,
                   float *out);
int tyvec_fp64_add(uint32_t n, uint8_t *flag, const double *a, const double *b,
                   double *out);
int tyvec_fp32_sub(uint32_t n, uint8_t *flag, const float *a, const float *b,
                   float *out);
int tyvec_fp64_sub(uint32_t n, uint8_t *flag, const double *a, const double *b,
                   double *out);
int tyvec_fp32_mul(uint32_t n, uint8_t *flag, const float *a, const float *b,
                   float *out);
int tyvec_fp64_mul(uint32_t n, uint8_t *flag, const double *a, const double *b,
                   double *out);
int tyvec_fp32_div(uint32_t n, uint8_t *flag, const float *a, const float *b,
                   float *out);
int tyvec_fp64_div(uint32_t n, uint8_t *flag, const double *a, const double *b,
                   double *out);
int tyvec_flag_isnull(uint32_t n, uint8_t *flag, uint8_t *out);
int tyvec_flag_isnotnull(uint32_t n, uint8_t *flag, uint8_t *out);
int tyvec_bool_istrue(uint32_t n, uint8_t *flag, const uint8_t *vec,
                      uint8_t *out);
int tyvec_bool_isfalse(uint32_t n, uint8_t *flag, const uint8_t *vec,
                       uint8_t *out);
int tyvec_bool_not(uint32_t n, uint8_t *flag, const uint8_t *vec, uint8_t *out);
int tyvec_bool_and(uint32_t n, uint8_t *flag_a, const uint8_t *flag_b,
                   const uint8_t *a, const uint8_t *b, uint8_t *out);
int tyvec_bool_or(uint32_t n, uint8_t *flag_a, const uint8_t *flag_b,
                  const uint8_t *a, const uint8_t *b, uint8_t *out);

/* The six compares of a type. */
#define TYVEC_CMP_FN(TYPE, T, NAME)                                            \
  int tyvec_##TYPE##_##NAME(uint32_t n, uint8_t *flag, const T *a, const T *b, \
                            uint8_t *out);
#define TYVEC_CMP_FNS(TYPE, T)                                                 \
  TYVEC_CMP_FN(TYPE, T, eq)                                                    \
  TYVEC_CMP_FN(TYPE, T, ne)                                                    \
  TYVEC_CMP_FN(TYPE, T, lt)                                                    \
  TYVEC_CMP_FN(TYPE, T, le)                                                    \
  TYVEC_CMP_FN(TYPE, T, gt)                                                    \
  TYVEC_CMP_FN(TYPE, T, ge)

TYVEC_CMP_FNS(int16, int16_t)
TYVEC_CMP_FNS(int32, int32_t)
TYVEC_CMP_FNS(int64, int64_t)
TYVEC_CMP_FNS(fp32, float)
TYVEC_CMP_FNS(fp64, double)
TYVEC_CMP_FNS(bool, uint8_t)

#undef TYVEC_CMP_FNS
#undef TYVEC_CMP_FN

#endif

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
 * is 1 if it is null; bit 2, TY_VEC_ERROR, is 1 if it failed. The other
 * bits are 0 for now. An operation's inputs share one flag array, flag: a
 * row is null if one of its inputs is. A caller merges the flags of two
 * inputs into it before the call, with tyvec_flag_or. AND and OR are the
 * exception: they take the flags of each input.
 *
 * An operation computes each row with none of the three bits set, as the
 * scalar operation of ty_int.h or ty_float.h would, and writes:
 *   - out, the n results. A row not valid, null or failed gets a value that
 *     means nothing. (An IS test has a result for a null row.) An operation
 *     that cannot fail may compute every row, as that is cheaper.
 *   - flag, the flags of the results, over those of the inputs. An
 *     operation on numbers leaves each as it is: a null input gives a null
 *     result, and a failed one a failed result.
 *
 * A row that fails does not fail the call: the operation marks it, and goes
 * on with the others. A mark is TY_VEC_ERROR in the row's flag, and an
 * entry in err, the row and its error: TY_ERR_RANGE if the result does not
 * fit, TY_ERR_DIVZERO, or TY_ERR_UNDERFLOW. Entries are appended in the
 * order the rows fail; nothing is written to err while no row fails. An
 * operation that can fail returns 0, or 1 if it marked a row. A caller
 * that wants the error at once calls tyvec_flag_error.
 *
 * The operations run on the machine's vector instructions (src/vec.cc).
 *
 *   tyvec_intN_add(n, flag, err, a, b, out)
 *   tyvec_intN_sub(n, flag, err, a, b, out)
 *   tyvec_intN_mul(n, flag, err, a, b, out)
 *   tyvec_intN_div(n, flag, err, a, b, out)
 *   tyvec_intN_mod(n, flag, err, a, b, out)
 *       out[i] = a[i] + b[i], a[i] - b[i], a[i] * b[i], a[i] / b[i], to
 *       zero, or a[i] % b[i], with the sign of a[i]; MIN % -1 is 0. RANGE
 *       on a row computed that overflows, as MIN / -1 does, and DIVZERO on
 *       one divided by 0; a row not computed may have a b of 0. out may be
 *       a or b, as a row's inputs are read before its result is written;
 *       add does not allow that it is both.
 *
 *   tyvec_fpN_add(n, flag, err, a, b, out), and _sub, _mul, _div
 *       The same of float (fp32) and double (fp64). RANGE on a row computed
 *       whose result is infinite when no input is; UNDERFLOW on a product
 *       that is 0 when neither factor is, or a quotient that is 0 when the
 *       dividend is not and the divisor is finite; DIVZERO on one divided
 *       by 0, unless a[i] is NaN. out may be a, b or both.
 *
 *   tyvec_flag_error(n, flag, err)
 *       -1, with ty_errnum(), ty_errmsg() and ty_erridx() of the lowest row
 *       that is valid and failed, by its entry in err; 0 if there is none.
 *
 *   tyvec_flag_or(n, flag_a, err_a, flag_b, err_b)
 *       The flags of a row of two inputs: flag_a |= flag_b, and to err_a
 *       the entries of err_b of a row that had not failed in flag_a, as a
 *       left input's error is raised first. err_b may be err_a.
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
 *       the other bits stay, so a failed row stays failed, as PostgreSQL
 *       raises before it tests. A caller that needs the input's nulls
 *       afterwards copies flag first.
 *
 *   tyvec_bool_not(n, flag, vec, out)
 *       SQL's NOT: out[i] = 1 if vec[i] is 0, and 0 if it is 1. NOT of a
 *       null is null: flag stays as it is.
 *
 *   tyvec_bool_and(n, flag_a, err_a, flag_b, err_b, a, b, out)
 *   tyvec_bool_or(n, flag_a, err_a, flag_b, err_b, a, b, out)
 *       SQL's AND and OR of bools that are 0 or 1 in each row computed:
 *       out[i] = a[i] AND b[i], or OR. flag_a and err_a are a's, and then
 *       the result's; flag_b and err_b are b's, and stay. A row is null if
 *       an input is, but false AND null is false, and true OR null is true.
 *       A row failed if an input did, but false AND <failed> is false, and
 *       true OR <failed> is true: a's false, or a's true, drops b's error,
 *       and b's never drops a's, as PostgreSQL evaluates a first. err_a
 *       takes the entries of err_b of the rows that failed by b. A row is
 *       not valid if an input is not. The other bits of flag_a stay. out
 *       may be a, b or both, flag_b may be flag_a, and err_b err_a.
 *
 * The flag and bool operations cannot fail: they return 0. out may be vec;
 * a flag array may not overlap an input or out.
 */
#ifndef TY_VEC_H
#define TY_VEC_H

#include <stdint.h>

#include <vector>

#include "ty.h"

/* Bit 0 of a row's flag: 1 if the row is not valid. */
#define TY_VEC_NOTVALID 0x01
/* Bit 1 of a row's flag: 1 if the row is null. */
#define TY_VEC_NULL 0x02
/* Bit 2 of a row's flag: 1 if the row failed; err has its error. */
#define TY_VEC_ERROR 0x04
/* The bits of a row that is not computed: not valid, null, or failed. */
#define TY_VEC_SKIP (TY_VEC_NOTVALID | TY_VEC_NULL | TY_VEC_ERROR)

/* The error of a row that failed. */
typedef struct ty_rowerr_t {
  uint32_t row;
  uint8_t code; /* a ty_errnum_t */
} ty_rowerr_t;

int tyvec_int16_add(uint32_t n, uint8_t *flag, std::vector<ty_rowerr_t> &err,
                    const int16_t *a, const int16_t *b, int16_t *out);
int tyvec_int32_add(uint32_t n, uint8_t *flag, std::vector<ty_rowerr_t> &err,
                    const int32_t *a, const int32_t *b, int32_t *out);
int tyvec_int64_add(uint32_t n, uint8_t *flag, std::vector<ty_rowerr_t> &err,
                    const int64_t *a, const int64_t *b, int64_t *out);
int tyvec_int16_sub(uint32_t n, uint8_t *flag, std::vector<ty_rowerr_t> &err,
                    const int16_t *a, const int16_t *b, int16_t *out);
int tyvec_int32_sub(uint32_t n, uint8_t *flag, std::vector<ty_rowerr_t> &err,
                    const int32_t *a, const int32_t *b, int32_t *out);
int tyvec_int64_sub(uint32_t n, uint8_t *flag, std::vector<ty_rowerr_t> &err,
                    const int64_t *a, const int64_t *b, int64_t *out);
int tyvec_int16_mul(uint32_t n, uint8_t *flag, std::vector<ty_rowerr_t> &err,
                    const int16_t *a, const int16_t *b, int16_t *out);
int tyvec_int32_mul(uint32_t n, uint8_t *flag, std::vector<ty_rowerr_t> &err,
                    const int32_t *a, const int32_t *b, int32_t *out);
int tyvec_int64_mul(uint32_t n, uint8_t *flag, std::vector<ty_rowerr_t> &err,
                    const int64_t *a, const int64_t *b, int64_t *out);
int tyvec_int16_div(uint32_t n, uint8_t *flag, std::vector<ty_rowerr_t> &err,
                    const int16_t *a, const int16_t *b, int16_t *out);
int tyvec_int32_div(uint32_t n, uint8_t *flag, std::vector<ty_rowerr_t> &err,
                    const int32_t *a, const int32_t *b, int32_t *out);
int tyvec_int64_div(uint32_t n, uint8_t *flag, std::vector<ty_rowerr_t> &err,
                    const int64_t *a, const int64_t *b, int64_t *out);
int tyvec_int16_mod(uint32_t n, uint8_t *flag, std::vector<ty_rowerr_t> &err,
                    const int16_t *a, const int16_t *b, int16_t *out);
int tyvec_int32_mod(uint32_t n, uint8_t *flag, std::vector<ty_rowerr_t> &err,
                    const int32_t *a, const int32_t *b, int32_t *out);
int tyvec_int64_mod(uint32_t n, uint8_t *flag, std::vector<ty_rowerr_t> &err,
                    const int64_t *a, const int64_t *b, int64_t *out);
int tyvec_fp32_add(uint32_t n, uint8_t *flag, std::vector<ty_rowerr_t> &err,
                   const float *a, const float *b, float *out);
int tyvec_fp64_add(uint32_t n, uint8_t *flag, std::vector<ty_rowerr_t> &err,
                   const double *a, const double *b, double *out);
int tyvec_fp32_sub(uint32_t n, uint8_t *flag, std::vector<ty_rowerr_t> &err,
                   const float *a, const float *b, float *out);
int tyvec_fp64_sub(uint32_t n, uint8_t *flag, std::vector<ty_rowerr_t> &err,
                   const double *a, const double *b, double *out);
int tyvec_fp32_mul(uint32_t n, uint8_t *flag, std::vector<ty_rowerr_t> &err,
                   const float *a, const float *b, float *out);
int tyvec_fp64_mul(uint32_t n, uint8_t *flag, std::vector<ty_rowerr_t> &err,
                   const double *a, const double *b, double *out);
int tyvec_fp32_div(uint32_t n, uint8_t *flag, std::vector<ty_rowerr_t> &err,
                   const float *a, const float *b, float *out);
int tyvec_fp64_div(uint32_t n, uint8_t *flag, std::vector<ty_rowerr_t> &err,
                   const double *a, const double *b, double *out);
int tyvec_flag_isnull(uint32_t n, uint8_t *flag, uint8_t *out);
int tyvec_flag_isnotnull(uint32_t n, uint8_t *flag, uint8_t *out);
int tyvec_bool_istrue(uint32_t n, uint8_t *flag, const uint8_t *vec,
                      uint8_t *out);
int tyvec_bool_isfalse(uint32_t n, uint8_t *flag, const uint8_t *vec,
                       uint8_t *out);
int tyvec_bool_not(uint32_t n, uint8_t *flag, const uint8_t *vec, uint8_t *out);
int tyvec_bool_and(uint32_t n, uint8_t *flag_a, std::vector<ty_rowerr_t> &err_a,
                   const uint8_t *flag_b, const std::vector<ty_rowerr_t> &err_b,
                   const uint8_t *a, const uint8_t *b, uint8_t *out);
int tyvec_bool_or(uint32_t n, uint8_t *flag_a, std::vector<ty_rowerr_t> &err_a,
                  const uint8_t *flag_b, const std::vector<ty_rowerr_t> &err_b,
                  const uint8_t *a, const uint8_t *b, uint8_t *out);
int tyvec_flag_error(uint32_t n, const uint8_t *flag,
                     const std::vector<ty_rowerr_t> &err);
int tyvec_flag_or(uint32_t n, uint8_t *flag_a, std::vector<ty_rowerr_t> &err_a,
                  const uint8_t *flag_b, const std::vector<ty_rowerr_t> &err_b);

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

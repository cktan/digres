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
 * bits are 0 for now. An operation of two inputs takes the flags of each,
 * flag_a and flag_b; flag_b may be NULL, for an input with no flags, such
 * as a constant. One of one input takes flag.
 *
 * An operation computes each row with none of the three bits set in its
 * inputs' flags, as the scalar operation of ty_int.h or ty_float.h would,
 * and writes:
 *   - out, the n results. A row not valid, null or failed gets a value that
 *     means nothing. (An IS test has a result for a null row.) An operation
 *     that cannot fail may compute every row, as that is cheaper.
 *   - flag_a, or flag, the flags of the results, over those of the first
 *     input. An operation on numbers ORs flag_b into them: a null input
 *     gives a null result, and a failed one a failed result.
 *
 * A row that fails does not fail the call: the operation marks it, and goes
 * on with the others. A mark is TY_VEC_ERROR in the row's flag, and an
 * entry in err, the row and its error: TY_ERR_RANGE if the result does not
 * fit, TY_ERR_DIVZERO, or TY_ERR_UNDERFLOW. An operation appends only its
 * own, in row order; nothing is written to err while no row fails. It does
 * not take its inputs' entries: a caller that keeps a list for each input
 * adds b's with tyvec_err_merge. An operation that can fail returns 0, or 1
 * if it marked a row. A caller that wants the error at once calls
 * tyvec_flag_error.
 *
 * The operations run on the machine's vector instructions (src/vec.cc).
 *
 *   tyvec_intN_add(n, flag_a, a, flag_b, b, out, err)
 *   tyvec_intN_sub(n, flag_a, a, flag_b, b, out, err)
 *   tyvec_intN_mul(n, flag_a, a, flag_b, b, out, err)
 *   tyvec_intN_div(n, flag_a, a, flag_b, b, out, err)
 *   tyvec_intN_mod(n, flag_a, a, flag_b, b, out, err)
 *       out[i] = a[i] + b[i], a[i] - b[i], a[i] * b[i], a[i] / b[i], to
 *       zero, or a[i] % b[i], with the sign of a[i]; MIN % -1 is 0. RANGE
 *       on a row computed that overflows, as MIN / -1 does, and DIVZERO on
 *       one divided by 0; a row not computed may have a b of 0. out may be
 *       a or b, as a row's inputs are read before its result is written;
 *       add does not allow that it is both.
 *
 *   tyvec_fpN_add(n, flag_a, a, flag_b, b, out, err), and _sub, _mul, _div
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
 *   tyvec_err_merge(n, flag, err, err_b)
 *       After an operation of two inputs, whose results' flags are flag and
 *       whose first input's entries, and its own, are in err: append to err
 *       the entries of err_b, b's, whose row the result has failed and err
 *       names not, as a first input's error is raised before a second's. An
 *       AND or an OR that dropped b's error left no mark there, so its entry
 *       is not taken. err_b may be err.
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
 *   tyvec_bool_and(n, flag_a, a, flag_b, b, out, err)
 *   tyvec_bool_or(n, flag_a, a, flag_b, b, out, err)
 *       SQL's AND and OR of bools that are 0 or 1 in each row computed:
 *       out[i] = a[i] AND b[i], or OR. flag_a is a's, and then the
 *       result's; flag_b is b's, and stays. A row is null if an input is,
 *       but false AND null is false, and true OR null is true. A row failed
 *       if an input did, but false AND <failed> is false, and true OR
 *       <failed> is true: a's false, or a's true, drops b's error, and b's
 *       never drops a's, as PostgreSQL evaluates a first. A row is not
 *       valid if an input is not. The other bits of flag_a stay. They cannot
 *       fail, so nothing is appended to err. out may be a, b or both, and
 *       flag_b may be flag_a.
 *
 * The flag and bool operations cannot fail: they return 0. out may be vec;
 * a flag array may not overlap an input or out.
 *
 * A utility, not an operation of SQL:
 *
 *   ty_bitor_vec(n, a, b, c)
 *       a[i] |= b[i] | c[i] of n bytes, such as flags; c may be NULL, and
 *       then a[i] |= b[i]. b and c may be a.
 *
 *   tyvec_squeeze(n, flag, vec)
 *       Drop each row whose flag has TY_VEC_NOTVALID: the other rows' values
 *       and flags move to the front of vec and flag, in their order, and
 *       the count of them is returned. A null row stays. No row may have
 *       failed: a caller raises a vector's errors (tyvec_flag_error) before
 *       it squeezes, so the vector has no error list whose rows would move.
 *       What is past that count means nothing. Only a value's width
 *       matters, so there is one for each C type of a column's vector:
 *         1 byte:   uint8_t: bool, "char"
 *         2 bytes:  int16_t
 *         4 bytes:  int32_t: int4, date; float; uint32_t: oid, a
 *                   dictionary's codes
 *         8 bytes:  int64_t: int8, time, timestamp, timestamptz, a
 *                   decimal64's value; double; const char *: a varlena
 *                   type's, such as text, bytea and numeric
 *         16 bytes: ty_int128_t: a decimal128's value; ty_uuid_t;
 *                   ty_interval_t; ty_timetz_t
 *
 *   tyvec_dict_mark(n, flag, codes, dict_n, dict, out_dict, dict_flag)
 *       Mark the entries of a dictionary that no row uses, for a column of
 *       n rows whose row i is entry codes[i] of dict_n. A row counts if
 *       its flag has neither TY_VEC_NOTVALID nor TY_VEC_NULL; flag may be
 *       NULL, and then every row counts. dict_flag gets dict_n flags: 0 for
 *       an entry used, TY_VEC_NOTVALID for one not, so the dictionary and
 *       dict_flag are a vector whose operations compute only the entries
 *       used. out_dict gets dict_n varlena addresses: dict's for an entry
 *       used, an empty varlena's for one not, so the dictionary sent over
 *       the wire has the bytes of the entries used only. The codes do not
 *       change, nor does dict. dict and out_dict may be NULL, for a type
 *       that is not a varlena: then only dict_flag is written.
 *
 *   tyvec_print(n, flag, values, out, err)
 *       Append to out n lines, one a row, for a test to compare: the row's
 *       index, a space, and its value by ty_print (ty_print.h), or
 *       "notvalid", "error" and its code's name from err, or "null", in
 *       that order. flag may be NULL. One for each type: int16_t, int32_t,
 *       int64_t, float, double, and bool as uint8_t.
 */
#ifndef TY_VEC_H
#define TY_VEC_H

#include <stdint.h>

#include <string>
#include <vector>

#include "ty.h"
#include "ty_datetime.h"
#include "ty_decimal.h"
#include "ty_interval.h"
#include "ty_uuid.h"

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

int tyvec_int16_add(uint32_t n, uint8_t *flag_a, const int16_t *a,
                    const uint8_t *flag_b, const int16_t *b, int16_t *out,
                    std::vector<ty_rowerr_t> &err);
int tyvec_int32_add(uint32_t n, uint8_t *flag_a, const int32_t *a,
                    const uint8_t *flag_b, const int32_t *b, int32_t *out,
                    std::vector<ty_rowerr_t> &err);
int tyvec_int64_add(uint32_t n, uint8_t *flag_a, const int64_t *a,
                    const uint8_t *flag_b, const int64_t *b, int64_t *out,
                    std::vector<ty_rowerr_t> &err);
int tyvec_int16_sub(uint32_t n, uint8_t *flag_a, const int16_t *a,
                    const uint8_t *flag_b, const int16_t *b, int16_t *out,
                    std::vector<ty_rowerr_t> &err);
int tyvec_int32_sub(uint32_t n, uint8_t *flag_a, const int32_t *a,
                    const uint8_t *flag_b, const int32_t *b, int32_t *out,
                    std::vector<ty_rowerr_t> &err);
int tyvec_int64_sub(uint32_t n, uint8_t *flag_a, const int64_t *a,
                    const uint8_t *flag_b, const int64_t *b, int64_t *out,
                    std::vector<ty_rowerr_t> &err);
int tyvec_int16_mul(uint32_t n, uint8_t *flag_a, const int16_t *a,
                    const uint8_t *flag_b, const int16_t *b, int16_t *out,
                    std::vector<ty_rowerr_t> &err);
int tyvec_int32_mul(uint32_t n, uint8_t *flag_a, const int32_t *a,
                    const uint8_t *flag_b, const int32_t *b, int32_t *out,
                    std::vector<ty_rowerr_t> &err);
int tyvec_int64_mul(uint32_t n, uint8_t *flag_a, const int64_t *a,
                    const uint8_t *flag_b, const int64_t *b, int64_t *out,
                    std::vector<ty_rowerr_t> &err);
int tyvec_int16_div(uint32_t n, uint8_t *flag_a, const int16_t *a,
                    const uint8_t *flag_b, const int16_t *b, int16_t *out,
                    std::vector<ty_rowerr_t> &err);
int tyvec_int32_div(uint32_t n, uint8_t *flag_a, const int32_t *a,
                    const uint8_t *flag_b, const int32_t *b, int32_t *out,
                    std::vector<ty_rowerr_t> &err);
int tyvec_int64_div(uint32_t n, uint8_t *flag_a, const int64_t *a,
                    const uint8_t *flag_b, const int64_t *b, int64_t *out,
                    std::vector<ty_rowerr_t> &err);
int tyvec_int16_mod(uint32_t n, uint8_t *flag_a, const int16_t *a,
                    const uint8_t *flag_b, const int16_t *b, int16_t *out,
                    std::vector<ty_rowerr_t> &err);
int tyvec_int32_mod(uint32_t n, uint8_t *flag_a, const int32_t *a,
                    const uint8_t *flag_b, const int32_t *b, int32_t *out,
                    std::vector<ty_rowerr_t> &err);
int tyvec_int64_mod(uint32_t n, uint8_t *flag_a, const int64_t *a,
                    const uint8_t *flag_b, const int64_t *b, int64_t *out,
                    std::vector<ty_rowerr_t> &err);
int tyvec_fp32_add(uint32_t n, uint8_t *flag_a, const float *a,
                   const uint8_t *flag_b, const float *b, float *out,
                   std::vector<ty_rowerr_t> &err);
int tyvec_fp64_add(uint32_t n, uint8_t *flag_a, const double *a,
                   const uint8_t *flag_b, const double *b, double *out,
                   std::vector<ty_rowerr_t> &err);
int tyvec_fp32_sub(uint32_t n, uint8_t *flag_a, const float *a,
                   const uint8_t *flag_b, const float *b, float *out,
                   std::vector<ty_rowerr_t> &err);
int tyvec_fp64_sub(uint32_t n, uint8_t *flag_a, const double *a,
                   const uint8_t *flag_b, const double *b, double *out,
                   std::vector<ty_rowerr_t> &err);
int tyvec_fp32_mul(uint32_t n, uint8_t *flag_a, const float *a,
                   const uint8_t *flag_b, const float *b, float *out,
                   std::vector<ty_rowerr_t> &err);
int tyvec_fp64_mul(uint32_t n, uint8_t *flag_a, const double *a,
                   const uint8_t *flag_b, const double *b, double *out,
                   std::vector<ty_rowerr_t> &err);
int tyvec_fp32_div(uint32_t n, uint8_t *flag_a, const float *a,
                   const uint8_t *flag_b, const float *b, float *out,
                   std::vector<ty_rowerr_t> &err);
int tyvec_fp64_div(uint32_t n, uint8_t *flag_a, const double *a,
                   const uint8_t *flag_b, const double *b, double *out,
                   std::vector<ty_rowerr_t> &err);
int tyvec_flag_isnull(uint32_t n, uint8_t *flag, uint8_t *out);
int tyvec_flag_isnotnull(uint32_t n, uint8_t *flag, uint8_t *out);
int tyvec_bool_istrue(uint32_t n, uint8_t *flag, const uint8_t *vec,
                      uint8_t *out);
int tyvec_bool_isfalse(uint32_t n, uint8_t *flag, const uint8_t *vec,
                       uint8_t *out);
int tyvec_bool_not(uint32_t n, uint8_t *flag, const uint8_t *vec, uint8_t *out);
int tyvec_bool_and(uint32_t n, uint8_t *flag_a, const uint8_t *a,
                   const uint8_t *flag_b, const uint8_t *b, uint8_t *out,
                   std::vector<ty_rowerr_t> &err);
int tyvec_bool_or(uint32_t n, uint8_t *flag_a, const uint8_t *a,
                  const uint8_t *flag_b, const uint8_t *b, uint8_t *out,
                  std::vector<ty_rowerr_t> &err);
void ty_bitor_vec(uint32_t n, uint8_t *a, const uint8_t *b, const uint8_t *c);

int tyvec_squeeze(uint32_t n, uint8_t *flag, int16_t *vec);
int tyvec_squeeze(uint32_t n, uint8_t *flag, int32_t *vec);
int tyvec_squeeze(uint32_t n, uint8_t *flag, int64_t *vec);
int tyvec_squeeze(uint32_t n, uint8_t *flag, float *vec);
int tyvec_squeeze(uint32_t n, uint8_t *flag, double *vec);
int tyvec_squeeze(uint32_t n, uint8_t *flag, uint8_t *vec);
int tyvec_squeeze(uint32_t n, uint8_t *flag, uint32_t *vec);
int tyvec_squeeze(uint32_t n, uint8_t *flag, const char **vec);
int tyvec_squeeze(uint32_t n, uint8_t *flag, ty_int128_t *vec);
int tyvec_squeeze(uint32_t n, uint8_t *flag, ty_uuid_t *vec);
int tyvec_squeeze(uint32_t n, uint8_t *flag, ty_interval_t *vec);
int tyvec_squeeze(uint32_t n, uint8_t *flag, ty_timetz_t *vec);
void tyvec_dict_mark(uint32_t n, const uint8_t *flag, const uint32_t *codes,
                     uint32_t dict_n, const char *const *dict,
                     const char **out_dict, uint8_t *dict_flag);
void tyvec_print(uint32_t n, const uint8_t *flag, const int16_t *values,
                 std::string &out, const std::vector<ty_rowerr_t> &err);
void tyvec_print(uint32_t n, const uint8_t *flag, const int32_t *values,
                 std::string &out, const std::vector<ty_rowerr_t> &err);
void tyvec_print(uint32_t n, const uint8_t *flag, const int64_t *values,
                 std::string &out, const std::vector<ty_rowerr_t> &err);
void tyvec_print(uint32_t n, const uint8_t *flag, const float *values,
                 std::string &out, const std::vector<ty_rowerr_t> &err);
void tyvec_print(uint32_t n, const uint8_t *flag, const double *values,
                 std::string &out, const std::vector<ty_rowerr_t> &err);
void tyvec_print(uint32_t n, const uint8_t *flag, const uint8_t *values,
                 std::string &out, const std::vector<ty_rowerr_t> &err);
int tyvec_flag_error(uint32_t n, const uint8_t *flag,
                     const std::vector<ty_rowerr_t> &err);
int tyvec_err_merge(uint32_t n, const uint8_t *flag,
                    std::vector<ty_rowerr_t> &err,
                    const std::vector<ty_rowerr_t> &err_b);

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

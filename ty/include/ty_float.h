/*
 * Copyright (c) 2026 CK Tan
 * SPDX-License-Identifier: MIT
 */

/*
 * ty_float.h -- the operations of float32 and float64 (SQL's real and
 * double precision). Written for ty from the cases and errors of PostgreSQL's
 * float.c; no code of it.
 *
 * The rule is PostgreSQL's: a result that is infinite when no operand was
 * is an overflow, TY_ERR_RANGE; one that is zero when no operand was is an
 * underflow, TY_ERR_UNDERFLOW; a divisor of zero is TY_ERR_DIVZERO unless the
 * dividend is NaN; and a NaN or infinity that comes in goes through. All NaNs
 * are equal and above every number, so that a sort has an order.
 *
 * A call that can fail returns 0, or -1 with the thread's error set, and
 * gives its value through its last parameter, set only on success; one that
 * cannot fail returns its result. The small ones are in line. N below is 32
 * or 64, T is float or double.
 *
 *   ty_fpN_add, _sub, _mul, _div (a, b, &r)  r = a op b, checked as above
 *   ty_fpN_neg(a), _abs(a)                   -a, |a|; cannot fail
 *   ty_fpN_cmp(a, b)                         -1, 0 or 1; NaN is last
 *   ty_fpN_eq, _ne, _lt, _le, _gt, _ge (a, b)   1 or 0
 *   ty_fpN_larger(a, b), _smaller(a, b)      the larger, the smaller
 *
 * Conversions:
 *
 *   ty_fp32_to_fp64(a)       exact; cannot fail
 *   ty_fp64_to_fp32(a, &r)   RANGE if it overflows a float32,
 *                                  UNDERFLOW if a nonzero rounds to zero
 *   ty_fpN_to_intM(a, &r)       rounded to the nearest, a tie to the even;
 *                                  RANGE for NaN or a value that does not fit
 *                                  (M is 16, 32 or 64)
 *   ty_intM_to_fpN(a)           the nearest; cannot fail
 */
#ifndef TY_FLOAT_H
#define TY_FLOAT_H

#include <errno.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>

#include "ty.h"
#include "ty_int.h"

#define TY_FLOAT_OPS(N, T, ZERO)                                              \
    static inline int ty_fp##N##_add(T a, T b, T* r) {                        \
        T v = a + b;                                                          \
        if (TY_UNLIKELY(__builtin_isinf(v) && !__builtin_isinf(a) &&          \
                        !__builtin_isinf(b))) {                               \
            return ty_err_range();                                            \
        }                                                                     \
        *r = v;                                                               \
        return 0;                                                             \
    }                                                                         \
    static inline int ty_fp##N##_sub(T a, T b, T* r) {                        \
        T v = a - b;                                                          \
        if (TY_UNLIKELY(__builtin_isinf(v) && !__builtin_isinf(a) &&          \
                        !__builtin_isinf(b))) {                               \
            return ty_err_range();                                            \
        }                                                                     \
        *r = v;                                                               \
        return 0;                                                             \
    }                                                                         \
    static inline int ty_fp##N##_mul(T a, T b, T* r) {                        \
        T v = a * b;                                                          \
        if (TY_UNLIKELY(__builtin_isinf(v) && !__builtin_isinf(a) &&          \
                        !__builtin_isinf(b))) {                               \
            return ty_err_range();                                            \
        }                                                                     \
        if (TY_UNLIKELY(v == ZERO && a != ZERO && b != ZERO)) {               \
            return ty_err_underflow();                                        \
        }                                                                     \
        *r = v;                                                               \
        return 0;                                                             \
    }                                                                         \
    static inline int ty_fp##N##_div(T a, T b, T* r) {                        \
        T v;                                                                  \
        if (TY_UNLIKELY(b == ZERO && !__builtin_isnan(a))) {                  \
            return ty_err_divzero();                                          \
        }                                                                     \
        v = a / b;                                                            \
        if (TY_UNLIKELY(__builtin_isinf(v) && !__builtin_isinf(a))) {         \
            return ty_err_range();                                            \
        }                                                                     \
        if (TY_UNLIKELY(v == ZERO && a != ZERO && !__builtin_isinf(b))) {     \
            return ty_err_underflow();                                        \
        }                                                                     \
        *r = v;                                                               \
        return 0;                                                             \
    }                                                                         \
    static inline T ty_fp##N##_neg(T a) { return -a; }                        \
    static inline T ty_fp##N##_abs(T a) { return __builtin_fabs(a); }          \
    static inline int ty_fp##N##_eq(T a, T b) {                               \
        return __builtin_isnan(a) ? __builtin_isnan(b)                        \
                                  : !__builtin_isnan(b) && a == b;            \
    }                                                                         \
    static inline int ty_fp##N##_ne(T a, T b) {                               \
        return __builtin_isnan(a) ? !__builtin_isnan(b)                       \
                                  : __builtin_isnan(b) || a != b;             \
    }                                                                         \
    static inline int ty_fp##N##_lt(T a, T b) {                               \
        return !__builtin_isnan(a) && (__builtin_isnan(b) || a < b);          \
    }                                                                         \
    static inline int ty_fp##N##_le(T a, T b) {                               \
        return __builtin_isnan(b) || (!__builtin_isnan(a) && a <= b);         \
    }                                                                         \
    static inline int ty_fp##N##_gt(T a, T b) {                               \
        return !__builtin_isnan(b) && (__builtin_isnan(a) || a > b);          \
    }                                                                         \
    static inline int ty_fp##N##_ge(T a, T b) {                               \
        return __builtin_isnan(a) || (!__builtin_isnan(b) && a >= b);         \
    }                                                                         \
    static inline int ty_fp##N##_cmp(T a, T b) {                              \
        return ty_fp##N##_gt(a, b) ? 1 : ty_fp##N##_lt(a, b) ? -1 : 0;        \
    }                                                                         \
    static inline T ty_fp##N##_larger(T a, T b) {                             \
        return ty_fp##N##_gt(a, b) ? a : b;                                   \
    }                                                                         \
    static inline T ty_fp##N##_smaller(T a, T b) {                            \
        return ty_fp##N##_lt(a, b) ? a : b;                                   \
    }

TY_FLOAT_OPS(32, float, 0.0f)
TY_FLOAT_OPS(64, double, 0.0)

#undef TY_FLOAT_OPS

/* A float32 as a float64: exact. */
static inline double ty_fp32_to_fp64(float a) { return (double)a; }

/* A float64 as a float32: the nearest; an overflow or an underflow fails. */
static inline int ty_fp64_to_fp32(double a, float *r) {
  float v = (float)a;

  if (TY_UNLIKELY(__builtin_isinf(v) && !__builtin_isinf(a))) {
    return ty_err_range();
  }
  if (TY_UNLIKELY(v == 0.0f && a != 0.0)) {
    return ty_err_underflow();
  }
  *r = v;
  return 0;
}

/* A float of either width as an integer: rounded, with a range check. */
int ty_fp64_to_int16(double a, int16_t *r);
int ty_fp64_to_int32(double a, int32_t *r);
static inline int ty_fp32_to_int16(float a, int16_t *r) {
  return ty_fp64_to_int16((double)a, r);
}
static inline int ty_fp32_to_int32(float a, int32_t *r) {
  return ty_fp64_to_int32((double)a, r);
}

/* A float64 as an int64 is ty_int64_from_double, which ty_int.h has. */
static inline int ty_fp64_to_int64(double a, int64_t *r) {
  return ty_int64_from_double(a, r);
}
static inline int ty_fp32_to_int64(float a, int64_t *r) {
  return ty_fp64_to_int64((double)a, r);
}

/* An integer as a float: the nearest, as C converts it. */
static inline float ty_int16_to_fp32(int16_t a) { return (float)a; }
static inline float ty_int32_to_fp32(int32_t a) { return (float)a; }
static inline float ty_int64_to_fp32(int64_t a) { return (float)a; }
static inline double ty_int16_to_fp64(int16_t a) { return (double)a; }
static inline double ty_int32_to_fp64(int32_t a) { return (double)a; }
static inline double ty_int64_to_fp64(int64_t a) { return (double)a; }

/*
 * Rounding of a float64: ty_fp64_round to the nearest, a tie to the even;
 * ceil toward +infinity; floor toward -infinity; trunc toward zero; sign -1,
 * 0 or 1 (NaN is 0). None fails; NaN and infinity go through.
 */
static inline double ty_fp64_round(double a) { return rint(a); }
static inline double ty_fp64_ceil(double a) { return ceil(a); }
static inline double ty_fp64_floor(double a) { return floor(a); }
static inline double ty_fp64_trunc(double a) {
  return a >= 0 ? floor(a) : -floor(-a);
}
static inline double ty_fp64_sign(double a) {
  return a > 0 ? 1.0 : a < 0 ? -1.0 : 0.0;
}

/*
 * Powers and logarithms of a float64, with PostgreSQL's errors:
 *
 *   sqrt(a)       TY_ERR_INVALID if a < 0
 *   cbrt(a)
 *   exp(a)        RANGE if it overflows, UNDERFLOW if it is zero; exp(-inf)
 *                 is 0 and exp(NaN) is NaN
 *   ln(a), log10(a)   TY_ERR_INVALID if a is 0 or negative
 *   pow(a, b)     NaN as POSIX says: NaN ^ 0 and 1 ^ NaN are 1, another NaN is
 *                 NaN; TY_ERR_INVALID for 0 ^ negative and for a negative a
 *                 to a b that is not an integer; the infinities as POSIX says;
 *                 RANGE or UNDERFLOW when the result passes the type
 *
 * The message of an INVALID is PostgreSQL's text.
 */
int ty_fp64_sqrt_slow(double a, double *r);
int ty_fp64_cbrt_slow(double a, double *r);
int ty_fp64_exp_slow(double a, double *r);
int ty_fp64_ln_slow(double a, double *r);
int ty_fp64_log10_slow(double a, double *r);
int ty_fp64_pow_slow(double a, double b, double *r);

/*
 * The common case of each is in line, so that a host's wrapper costs no call:
 * a finite argument inside the domain whose result is finite and not zero.
 * Anything else goes to the out-of-line _slow function, which decides it.
 */
static inline int ty_fp64_sqrt(double a, double *r) {
  double v = sqrt(a);

  if (TY_UNLIKELY(!(a > 0) || __builtin_isinf(a) || v == 0.0)) {
    return ty_fp64_sqrt_slow(a, r);
  }
  *r = v;
  return 0;
}

static inline int ty_fp64_cbrt(double a, double *r) {
  double v = cbrt(a);

  if (TY_UNLIKELY(v == 0.0 || !__builtin_isfinite(v) ||
                  !__builtin_isfinite(a))) {
    return ty_fp64_cbrt_slow(a, r);
  }
  *r = v;
  return 0;
}

static inline int ty_fp64_exp(double a, double *r) {
  double v;

  if (TY_UNLIKELY(!__builtin_isfinite(a))) {
    return ty_fp64_exp_slow(a, r);
  }
  errno = 0;
  v = exp(a);
  if (TY_UNLIKELY(errno != 0 || !__builtin_isfinite(v) || v == 0.0)) {
    return ty_fp64_exp_slow(a, r);
  }
  *r = v;
  return 0;
}

static inline int ty_fp64_ln(double a, double *r) {
  double v;

  if (TY_UNLIKELY(!(a > 0) || __builtin_isinf(a))) {
    return ty_fp64_ln_slow(a, r);
  }
  v = log(a);
  if (TY_UNLIKELY(v == 0.0 && a != 1.0)) {
    return ty_fp64_ln_slow(a, r);
  }
  *r = v;
  return 0;
}

static inline int ty_fp64_log10(double a, double *r) {
  double v;

  if (TY_UNLIKELY(!(a > 0) || __builtin_isinf(a))) {
    return ty_fp64_log10_slow(a, r);
  }
  v = log10(a);
  if (TY_UNLIKELY(v == 0.0 && a != 1.0)) {
    return ty_fp64_log10_slow(a, r);
  }
  *r = v;
  return 0;
}

static inline int ty_fp64_pow(double a, double b, double *r) {
  double v;

  if (TY_UNLIKELY(!(a > 0) || !__builtin_isfinite(a) ||
                  !__builtin_isfinite(b))) {
    return ty_fp64_pow_slow(a, b, r);
  }
  errno = 0;
  v = pow(a, b);
  if (TY_UNLIKELY(errno != 0 || !__builtin_isfinite(v) || v == 0.0)) {
    return ty_fp64_pow_slow(a, b, r);
  }
  *r = v;
  return 0;
}

/*
 * The text of a float. ty_fpN_format writes a float to out, which has room
 * for TY_FLOAT_TEXT_SIZE, and a NUL; it returns the length without the NUL. If
 * extra_digits is above 0 the text is the shortest that reads back to the
 * same value (PostgreSQL's Ryu, which is not always the very shortest);
 * otherwise it is "%.Ng" with N = 6 (float32) or 15 (float64) plus
 * extra_digits, held to 1 to 32. NaN is NaN, and infinity is Infinity or
 * -Infinity; -0 is -0.
 *
 * ty_fpN_parse reads a float from the NUL-terminated s: space, then what
 * strtod (strtof for float32) takes, or NaN, Infinity or inf with a sign,
 * in any case; then space. *end, if end is not NULL, is set to where the
 * number and the space after it stop, and what follows is not an error; with
 * end NULL, any text after is TY_ERR_INVALID. A number that overflows, or
 * underflows to zero, is TY_ERR_RANGE; one too small for full precision is
 * accepted. *len, if not NULL, is the length of the number itself, from its
 * first character that is not space, and is set for a RANGE too.
 */
#define TY_FLOAT_TEXT_SIZE 32
size_t ty_fp32_format(float a, int extra_digits, char *out);
size_t ty_fp64_format(double a, int extra_digits, char *out);
int ty_fp32_parse(const char *s, float *r, const char **end, size_t *len);
int ty_fp64_parse(const char *s, double *r, const char **end, size_t *len);

/*
 * Trigonometry, the hyperbolic functions, and the like, of a float64, with
 * PostgreSQL's cases. Each returns 0, or -1 with the thread's error set.
 * A NaN gives NaN. An argument outside the domain of the function, such as
 * acos(2), or an infinite angle, is TY_ERR_INVALID with the message
 * "input is out of range"; a result that is infinite when the argument was
 * not is TY_ERR_RANGE; and gamma and cosh can underflow.
 *
 *   acos, asin, atan, atan2(y, x), cos, sin, tan, cot       radians
 *   acosd, asind, atand, atan2d, cosd, sind, tand, cotd     degrees: exact at
 *                    the angles that have exact values (sind(30) is 0.5)
 *   sinh, cosh, tanh, asinh, acosh, atanh
 *   erf, erfc, gamma, lgamma
 *
 * degrees(a) and radians(a) convert, with the checks of div and mul;
 * ty_fp64_pi() is pi.
 */
#define TY_FLOAT_RADIANS_PER_DEGREE 0.0174532925199432957692

#define TY_FLOAT_UNARY(NAME) int ty_fp64_##NAME(double a, double* r)
TY_FLOAT_UNARY(cot);
int ty_fp64_acos_slow(double a, double *r);
int ty_fp64_asin_slow(double a, double *r);
int ty_fp64_atan_slow(double a, double *r);
int ty_fp64_sincos_slow(int which, double a, double *r);
TY_FLOAT_UNARY(acosd);
TY_FLOAT_UNARY(asind);
TY_FLOAT_UNARY(atand);
TY_FLOAT_UNARY(cosd);
TY_FLOAT_UNARY(sind);
TY_FLOAT_UNARY(tand);
TY_FLOAT_UNARY(cotd);
TY_FLOAT_UNARY(sinh);
TY_FLOAT_UNARY(cosh);
TY_FLOAT_UNARY(tanh);
TY_FLOAT_UNARY(asinh);
TY_FLOAT_UNARY(acosh);
TY_FLOAT_UNARY(atanh);
TY_FLOAT_UNARY(erf);
TY_FLOAT_UNARY(erfc);
TY_FLOAT_UNARY(gamma);
TY_FLOAT_UNARY(lgamma);

/*
 * acos, asin, atan, cos, sin and tan, in line for a finite argument (a NaN, an
 * infinity, and acos and asin outside [-1, 1] are decided out of line). The
 * libm sets errno for these only for an infinite angle, which is tested.
 */
static inline int ty_fp64_acos(double a, double *r) {
  if (TY_UNLIKELY(!(a >= -1.0 && a <= 1.0))) {
    return ty_fp64_acos_slow(a, r);
  }
  *r = acos(a);
  return 0;
}

static inline int ty_fp64_asin(double a, double *r) {
  if (TY_UNLIKELY(!(a >= -1.0 && a <= 1.0))) {
    return ty_fp64_asin_slow(a, r);
  }
  *r = asin(a);
  return 0;
}

static inline int ty_fp64_atan(double a, double *r) {
  if (TY_UNLIKELY(__builtin_isnan(a))) {
    return ty_fp64_atan_slow(a, r);
  }
  *r = atan(a);
  return 0;
}

static inline int ty_fp64_cos(double a, double *r) {
  if (TY_UNLIKELY(!__builtin_isfinite(a))) {
    return ty_fp64_sincos_slow(0, a, r);
  }
  *r = cos(a);
  return 0;
}

static inline int ty_fp64_sin(double a, double *r) {
  if (TY_UNLIKELY(!__builtin_isfinite(a))) {
    return ty_fp64_sincos_slow(1, a, r);
  }
  *r = sin(a);
  return 0;
}

static inline int ty_fp64_tan(double a, double *r) {
  if (TY_UNLIKELY(!__builtin_isfinite(a))) {
    return ty_fp64_sincos_slow(2, a, r);
  }
  *r = tan(a);
  return 0;
}

#undef TY_FLOAT_UNARY
int ty_fp64_atan2(double y, double x, double *r);
int ty_fp64_atan2d(double y, double x, double *r);
static inline int ty_fp64_degrees(double a, double *r) {
  return ty_fp64_div(a, TY_FLOAT_RADIANS_PER_DEGREE, r);
}
static inline int ty_fp64_radians(double a, double *r) {
  return ty_fp64_mul(a, TY_FLOAT_RADIANS_PER_DEGREE, r);
}
static inline double ty_fp64_pi(void) { return 3.14159265358979323846; }

/*
 * Statistical aggregates over float64, the arithmetic of avg, the variances,
 * the deviations and the regression family, on the state a host keeps:
 *
 *   ty_float_var_t   the count n, the sum sx and the sum of squares about the
 *                    mean sxx, as the Youngs-Cramer method keeps them
 *   ty_float_regr_t  the same for two variables, and their sum of products:
 *                    n, sx, sxx, sy, syy, sxy
 *
 * A state starts all zero. accum adds a value; combine adds another state,
 * for a parallel aggregate; each fails with TY_ERR_RANGE if finite values
 * make an infinite sum, and keeps sxx NaN when an input is infinite or NaN. A
 * final function sets *r and returns 0, or returns 1 for the SQL NULL it
 * gives when there are too few rows (it sets nothing then).
 */
typedef struct ty_float_var_t {
  double n, sx, sxx;
} ty_float_var_t;

typedef struct ty_float_regr_t {
  double n, sx, sxx, sy, syy, sxy;
} ty_float_regr_t;

/*
 * Add x to a variance state (in line, as it is the step of an aggregate):
 * count it and add it to the sum; if it is not the first, add its part to the
 * sum of squares about the mean, where an infinite sum made of finite values
 * is an overflow and an infinite sum of squares becomes NaN; if it is the
 * first, a NaN or infinite value makes the sum of squares NaN, so that no
 * variance of zero is reported.
 */
static inline int ty_float_var_accum(ty_float_var_t *st, double x) {
  double n = st->n + 1.0;
  double sx = st->sx + x;
  double sxx = st->sxx;

  if (st->n > 0.0) {
    double tmp = x * n - sx;

    sxx += tmp * tmp / (n * st->n);
    if (TY_UNLIKELY(__builtin_isinf(sx) || __builtin_isinf(sxx))) {
      if (!__builtin_isinf(st->sx) && !__builtin_isinf(x)) {
        return ty_err_range();
      }
      sxx = __builtin_nan("");
    }
  } else if (TY_UNLIKELY(__builtin_isnan(x) || __builtin_isinf(x))) {
    sxx = __builtin_nan("");
  }
  st->n = n;
  st->sx = sx;
  st->sxx = sxx;
  return 0;
}
int ty_float_var_combine(ty_float_var_t *a, const ty_float_var_t *b);
int ty_float_var_avg(const ty_float_var_t *st, double *r);
int ty_float_var_pop(const ty_float_var_t *st, double *r);
int ty_float_var_samp(const ty_float_var_t *st, double *r);
int ty_float_stddev_pop(const ty_float_var_t *st, double *r);
int ty_float_stddev_samp(const ty_float_var_t *st, double *r);

/* Add the pair (x, y) to a regression state: var_accum, for two (in line). */
static inline int ty_float_regr_accum(ty_float_regr_t *st, double y, double x) {
  double n = st->n + 1.0;
  double sx = st->sx + x;
  double sy = st->sy + y;
  double sxx = st->sxx, syy = st->syy, sxy = st->sxy;

  if (st->n > 0.0) {
    double tmp_x = x * n - sx;
    double tmp_y = y * n - sy;
    double scale = 1.0 / (n * st->n);

    sxx += tmp_x * tmp_x * scale;
    syy += tmp_y * tmp_y * scale;
    sxy += tmp_x * tmp_y * scale;
    if (__builtin_isinf(sx) || __builtin_isinf(sxx) || __builtin_isinf(sy) ||
        __builtin_isinf(syy) || __builtin_isinf(sxy)) {
      if (((__builtin_isinf(sx) || __builtin_isinf(sxx)) &&
           !__builtin_isinf(st->sx) && !__builtin_isinf(x)) ||
          ((__builtin_isinf(sy) || __builtin_isinf(syy)) &&
           !__builtin_isinf(st->sy) && !__builtin_isinf(y)) ||
          (__builtin_isinf(sxy) && !__builtin_isinf(st->sx) &&
           !__builtin_isinf(x) && !__builtin_isinf(st->sy) &&
           !__builtin_isinf(y))) {
        return ty_err_range();
      }
      if (__builtin_isinf(sxx)) {
        sxx = __builtin_nan("");
      }
      if (__builtin_isinf(syy)) {
        syy = __builtin_nan("");
      }
      if (__builtin_isinf(sxy)) {
        sxy = __builtin_nan("");
      }
    }
  } else {
    if (__builtin_isnan(x) || __builtin_isinf(x)) {
      sxx = sxy = __builtin_nan("");
    }
    if (__builtin_isnan(y) || __builtin_isinf(y)) {
      syy = sxy = __builtin_nan("");
    }
  }
  st->n = n;
  st->sx = sx;
  st->sxx = sxx;
  st->sy = sy;
  st->syy = syy;
  st->sxy = sxy;
  return 0;
}

int ty_float_regr_combine(ty_float_regr_t *a, const ty_float_regr_t *b);
int ty_float_regr_sxx(const ty_float_regr_t *st, double *r);
int ty_float_regr_syy(const ty_float_regr_t *st, double *r);
int ty_float_regr_sxy(const ty_float_regr_t *st, double *r);
int ty_float_regr_avgx(const ty_float_regr_t *st, double *r);
int ty_float_regr_avgy(const ty_float_regr_t *st, double *r);
int ty_float_covar_pop(const ty_float_regr_t *st, double *r);
int ty_float_covar_samp(const ty_float_regr_t *st, double *r);
int ty_float_corr(const ty_float_regr_t *st, double *r);
int ty_float_regr_r2(const ty_float_regr_t *st, double *r);
int ty_float_regr_slope(const ty_float_regr_t *st, double *r);
int ty_float_regr_intercept(const ty_float_regr_t *st, double *r);

/*
 * The bucket of operand among count equal parts between two bounds, 1 to
 * count, 0 below the first and count + 1 at or past the other (the bounds
 * may run either way). TY_ERR_INVALID, with PostgreSQL's message, for a count
 * of 0 or less, a NaN, an infinite bound, or equal bounds; RANGE for
 * count + 1 past int32.
 */
int ty_fp64_width_bucket(double operand, double bound1, double bound2,
                         int32_t count, int32_t *r);

/*
 * Whether val is within the offset of base, for a window frame of RANGE
 * offsets, as ty_int_in_range (ty_int.h): 1 or 0 in *r. A NaN is above every
 * other value, and equal to itself. Where base and offset are infinities
 * whose sum or difference has no value, every val is within. A negative or
 * NaN offset is TY_ERR_INVALID. The float32 values are widened to float64.
 */
static inline int ty_fp64_in_range(double val, double base, double offset,
                                   int sub, int less, int *r) {
  double sum;

  if (TY_UNLIKELY(!(offset >= 0))) {
    return ty_err_invalid("invalid preceding or following size");
  }
  if (TY_UNLIKELY(__builtin_isnan(val) || __builtin_isnan(base))) {
    *r = !__builtin_isnan(val) ? less : __builtin_isnan(base) || !less;
    return 0;
  }
  if (TY_UNLIKELY(__builtin_isinf(offset) && __builtin_isinf(base) &&
                  (sub ? base > 0 : base < 0))) {
    *r = 1;
    return 0;
  }
  /* past the largest double the sum is an infinity, which compares right */
  sum = sub ? base - offset : base + offset;
  *r = less ? val <= sum : val >= sum;
  return 0;
}

/*
 * The hash of a float by a seed, as PostgreSQL's: 0 and -0 are one value, whose
 * hash is the seed, and every NaN is the same NaN; a float32 hashes as the
 * float64 of its value, so the two types hash alike. The low 32 bits with seed
 * 0 are the standard hash. It cannot fail.
 */
uint64_t ty_fp64_hash(double a, uint64_t seed);
uint64_t ty_fp32_hash(float a, uint64_t seed);

#endif /* TY_FLOAT_H */

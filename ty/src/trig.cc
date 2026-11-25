/*
 * Copyright (c) 2026 CK Tan
 * SPDX-License-Identifier: MIT
 */

/*
 * trig.cc -- trigonometry, the hyperbolic functions, erf and gamma of a
 * float64 (ty_float.h). Written for ty from the cases and the errors of
 * PostgreSQL's float.c, with its care to make the degree functions exact at
 * the angles that have exact values.
 */
#include <errno.h>
#include <math.h>

#include "error.h"
#include "ty_float.h"

namespace {

/* Record the out-of-domain input; returns -1. */
int out_of_range_input() {
  return ty_internal::set_error(TY_ERR_INVALID, "input is out of range");
}

/*
 * The constants of the degree functions. They are computed from volatile
 * variables, so that a compiler does not fold them with another rounding
 * than the run-time library gives: the functions below divide by them, and
 * the angles 30, 45 and 60 must come out exact.
 */
struct degree_t {
  double sin_30, one_minus_cos_60, asin_0_5, acos_0_5, atan_1_0, tan_45, cot_45;
};

double sind_q1(const degree_t &c, double x);
double cosd_q1(const degree_t &c, double x);

/* The constants: made when the library loads, so a call has nothing to test. */
const degree_t degree_const = [] {
  volatile double thirty = 30.0, forty_five = 45.0, sixty = 60.0,
                  one_half = 0.5, one = 1.0;
  degree_t d{};

  d.sin_30 = sin(thirty * TY_FLOAT_RADIANS_PER_DEGREE);
  d.one_minus_cos_60 = 1.0 - cos(sixty * TY_FLOAT_RADIANS_PER_DEGREE);
  d.asin_0_5 = asin(one_half);
  d.acos_0_5 = acos(one_half);
  d.atan_1_0 = atan(one);
  d.tan_45 = sind_q1(d, forty_five) / cosd_q1(d, forty_five);
  d.cot_45 = cosd_q1(d, forty_five) / sind_q1(d, forty_five);
  return d;
}();

/* The constants. */
inline const degree_t &degree_constants() { return degree_const; }

/* sin of 0 to 30 degrees, scaled so that sin(30) is exactly 1/2. */
double sind_0_to_30(const degree_t &c, double x) {
  volatile double sin_x = sin(x * TY_FLOAT_RADIANS_PER_DEGREE);

  return (sin_x / c.sin_30) / 2.0;
}

/* cos of 0 to 60 degrees, scaled so that cos(60) is exactly 1/2. */
double cosd_0_to_60(const degree_t &c, double x) {
  volatile double one_minus_cos_x = 1.0 - cos(x * TY_FLOAT_RADIANS_PER_DEGREE);

  return 1.0 - (one_minus_cos_x / c.one_minus_cos_60) / 2.0;
}

/* sin of 0 to 90 degrees. */
double sind_q1(const degree_t &c, double x) {
  if (x <= 30.0) {
    return sind_0_to_30(c, x);
  }
  return cosd_0_to_60(c, 90.0 - x);
}

/* cos of 0 to 90 degrees. */
double cosd_q1(const degree_t &c, double x) {
  if (x <= 60.0) {
    return cosd_0_to_60(c, x);
  }
  return sind_0_to_30(c, 90.0 - x);
}

/* asin of 0 to 1, in degrees. */
double asind_q1(const degree_t &c, double x) {
  if (x <= 0.5) {
    volatile double asin_x = asin(x);

    return (asin_x / c.asin_0_5) * 30.0;
  }
  volatile double acos_x = acos(x);

  return 90.0 - (acos_x / c.acos_0_5) * 60.0;
}

/* acos of 0 to 1, in degrees. */
double acosd_q1(const degree_t &c, double x) {
  if (x <= 0.5) {
    volatile double asin_x = asin(x);

    return 90.0 - (asin_x / c.asin_0_5) * 30.0;
  }
  volatile double acos_x = acos(x);

  return (acos_x / c.acos_0_5) * 60.0;
}

/* Finish a result: an infinity is an overflow; else set it. */
int give(double v, double *r) {
  if (isinf(v)) {
    return ty_err_range();
  }
  *r = v;
  return 0;
}

/*
 * Bring an angle in degrees to 0 to 90, noting the sign: by the symmetry of
 * the function, as flags for what changes the sign (neg_at_neg: a negative
 * angle; neg_over_180: past 180; neg_over_90: past 90).
 */
void fold_degrees(double *x, int *sign, bool neg_at_neg, bool neg_over_180,
                  bool neg_over_90, bool sign_in_cos) {
  (void)sign_in_cos;
  *x = fmod(*x, 360.0);
  if (*x < 0.0) {
    *x = -*x;
    if (neg_at_neg) {
      *sign = -*sign;
    }
  }
  if (*x > 180.0) {
    *x = 360.0 - *x;
    if (neg_over_180) {
      *sign = -*sign;
    }
  }
  if (*x > 90.0) {
    *x = 180.0 - *x;
    if (neg_over_90) {
      *sign = -*sign;
    }
  }
}

} // namespace

/* acos outside the inline range: NaN is NaN, and [-1, 1] only. */
int ty_fp64_acos_slow(double a, double *r) {
  if (isnan(a)) {
    *r = a;
    return 0;
  }
  if (a < -1.0 || a > 1.0) {
    return out_of_range_input();
  }
  return give(acos(a), r);
}

/* asin outside the inline range. */
int ty_fp64_asin_slow(double a, double *r) {
  if (isnan(a)) {
    *r = a;
    return 0;
  }
  if (a < -1.0 || a > 1.0) {
    return out_of_range_input();
  }
  return give(asin(a), r);
}

/* atan of a NaN. */
int ty_fp64_atan_slow(double a, double *r) {
  *r = a;
  return 0;
}

/*
 * cos (0), sin (1) or tan (2) of a NaN or an infinity: NaN is NaN, an
 * infinite angle has none.
 */
int ty_fp64_sincos_slow(int which, double a, double *r) {
  (void)which;
  if (isnan(a)) {
    *r = a;
    return 0;
  }
  return out_of_range_input();
}

/* atan2(y, x), radians. */
int ty_fp64_atan2(double y, double x, double *r) {
  if (isnan(y) || isnan(x)) {
    *r = __builtin_nan("");
    return 0;
  }
  return give(atan2(y, x), r);
}

/* cotangent, radians: 1 / tan; cot(0) is infinite and not an error. */
int ty_fp64_cot(double a, double *r) {
  if (isnan(a)) {
    *r = a;
    return 0;
  }
  errno = 0;
  double v = tan(a);

  if (errno != 0 || isinf(a)) {
    return out_of_range_input();
  }
  *r = 1.0 / v;
  return 0;
}

/* arc cosine, degrees. */
int ty_fp64_acosd(double a, double *r) {
  if (isnan(a)) {
    *r = a;
    return 0;
  }
  const degree_t &c = degree_constants();

  if (a < -1.0 || a > 1.0) {
    return out_of_range_input();
  }
  return give(a >= 0.0 ? acosd_q1(c, a) : 90.0 + asind_q1(c, -a), r);
}

/* arc sine, degrees. */
int ty_fp64_asind(double a, double *r) {
  if (isnan(a)) {
    *r = a;
    return 0;
  }
  const degree_t &c = degree_constants();

  if (a < -1.0 || a > 1.0) {
    return out_of_range_input();
  }
  return give(a >= 0.0 ? asind_q1(c, a) : -asind_q1(c, -a), r);
}

/* arc tangent, degrees: atan scaled so that atan(1) is exactly 45. */
int ty_fp64_atand(double a, double *r) {
  if (isnan(a)) {
    *r = a;
    return 0;
  }
  const degree_t &c = degree_constants();
  volatile double atan_a = atan(a);

  return give((atan_a / c.atan_1_0) * 45.0, r);
}

/* atan2(y, x), degrees. */
int ty_fp64_atan2d(double y, double x, double *r) {
  if (isnan(y) || isnan(x)) {
    *r = __builtin_nan("");
    return 0;
  }
  const degree_t &c = degree_constants();
  volatile double v = atan2(y, x);

  return give((v / c.atan_1_0) * 45.0, r);
}

/*
 * cosine, degrees.
 * 1. NaN is NaN; an infinite angle is invalid.
 * 2. Fold the angle into 0 to 90 by the symmetries; the sign flips past 90.
 */
int ty_fp64_cosd(double a, double *r) {
  if (isnan(a)) {
    *r = a;
    return 0;
  }
  if (isinf(a)) {
    return out_of_range_input();
  }
  const degree_t &c = degree_constants();
  int sign = 1;

  fold_degrees(&a, &sign, false, false, true, true);
  return give(sign * cosd_q1(c, a), r);
}

/* sine, degrees. */
int ty_fp64_sind(double a, double *r) {
  if (isnan(a)) {
    *r = a;
    return 0;
  }
  if (isinf(a)) {
    return out_of_range_input();
  }
  const degree_t &c = degree_constants();
  int sign = 1;

  fold_degrees(&a, &sign, true, true, false, false);
  return give(sign * sind_q1(c, a), r);
}

/* tangent, degrees: exact at 45. */
int ty_fp64_tand(double a, double *r) {
  if (isnan(a)) {
    *r = a;
    return 0;
  }
  if (isinf(a)) {
    return out_of_range_input();
  }
  const degree_t &c = degree_constants();
  int sign = 1;

  fold_degrees(&a, &sign, true, true, true, false);
  volatile double tan_a = sind_q1(c, a) / cosd_q1(c, a);
  double v = sign * (tan_a / c.tan_45);

  *r = v == 0.0 ? 0.0 : v; /* no negative zero */
  return 0;
}

/* cotangent, degrees: exact at 45. */
int ty_fp64_cotd(double a, double *r) {
  if (isnan(a)) {
    *r = a;
    return 0;
  }
  if (isinf(a)) {
    return out_of_range_input();
  }
  const degree_t &c = degree_constants();
  int sign = 1;

  fold_degrees(&a, &sign, true, true, true, false);
  volatile double cot_a = cosd_q1(c, a) / sind_q1(c, a);
  double v = sign * (cot_a / c.cot_45);

  *r = v == 0.0 ? 0.0 : v;
  return 0;
}

/* hyperbolic sine; ERANGE gives the infinity of the sign. */
int ty_fp64_sinh(double a, double *r) {
  errno = 0;
  double v = sinh(a);

  if (errno == ERANGE) {
    v = a < 0 ? -INFINITY : INFINITY;
  }
  *r = v;
  return 0;
}

/* hyperbolic cosine; ERANGE gives infinity, and zero is an underflow. */
int ty_fp64_cosh(double a, double *r) {
  errno = 0;
  double v = cosh(a);

  if (errno == ERANGE) {
    v = INFINITY;
  }
  if (v == 0.0) {
    return ty_err_underflow();
  }
  *r = v;
  return 0;
}

/* hyperbolic tangent. */
int ty_fp64_tanh(double a, double *r) { return give(tanh(a), r); }

/* inverse hyperbolic sine. */
int ty_fp64_asinh(double a, double *r) {
  *r = asinh(a);
  return 0;
}

/* inverse hyperbolic cosine: 1 and up. */
int ty_fp64_acosh(double a, double *r) {
  if (a < 1.0) {
    return out_of_range_input();
  }
  *r = acosh(a);
  return 0;
}

/* inverse hyperbolic tangent: -1 to 1, infinite at the ends. */
int ty_fp64_atanh(double a, double *r) {
  if (a < -1.0 || a > 1.0) {
    return out_of_range_input();
  }
  *r = a == -1.0 ? -INFINITY : a == 1.0 ? INFINITY : atanh(a);
  return 0;
}

/* the error function. */
int ty_fp64_erf(double a, double *r) { return give(erf(a), r); }

/* the complementary error function. */
int ty_fp64_erfc(double a, double *r) { return give(erfc(a), r); }

/*
 * The gamma function.
 * 1. NaN is NaN; +infinity is itself; -infinity overflows.
 * 2. Else call tgamma: an error, or a result that is infinite or NaN, is
 *    an overflow, or an underflow if the result is zero; a zero result is
 *    an underflow.
 */
int ty_fp64_gamma(double a, double *r) {
  if (isnan(a)) {
    *r = a;
    return 0;
  }
  if (isinf(a)) {
    if (a < 0) {
      return ty_err_range();
    }
    *r = a;
    return 0;
  }
  errno = 0;
  double v = tgamma(a);

  if (errno != 0 || isinf(v) || isnan(v)) {
    return v != 0.0 ? ty_err_range() : ty_err_underflow();
  }
  if (v == 0.0) {
    return ty_err_underflow();
  }
  *r = v;
  return 0;
}

/* The log of the absolute value of gamma. */
int ty_fp64_lgamma(double a, double *r) {
  errno = 0;
  double v = lgamma(a);

  if (errno == ERANGE || (isinf(v) && !isinf(a))) {
    return ty_err_range();
  }
  *r = v;
  return 0;
}

/*
 * Copyright (c) 2026 CK Tan
 * SPDX-License-Identifier: MIT
 */

/*
 * math.cc -- the functions of the exponent of a decimal128: sqrt, exp,
 * ln, log and power. They take no more than 128 bits. A square root is
 * made a digit at a time; the others are computed in double, some 15 digits,
 * and rounded to the scale the caller picks. A result of more than 36 digits
 * is an error, and so is one a double cannot hold: too big, or too small.
 */

#include <math.h>

#include <cfloat>
#include <cmath>
#include <stdio.h>
#include <stdlib.h>

#include "error.h"
#include "ty_decimal.h"

namespace {

typedef unsigned __int128 u128;

/* 10^36 - 1: the largest value of a decimal128. */
constexpr u128 max_value = [] {
  u128 r = 1;
  for (int i = 0; i < TY_DEC128_MAX_DIGITS; i++) {
    r *= 10;
  }
  return r - 1;
}();

/* Record that a result does not fit a decimal128; returns -1. */
int range_error() {
  return ty_internal::set_error(TY_ERR_RANGE, "decimal128 result out of range");
}

/* The magnitude of v. */
u128 magnitude(ty_int128_t v) { return v < 0 ? -(u128)v : (u128)v; }

/* The decimal digits of v into out, most significant first; returns how many.
 */
int digits_of(u128 v, char *out) {
  char tmp[48];
  int n = 0;

  do {
    tmp[n++] = (char)('0' + (int)(v % 10));
    v /= 10;
  } while (v != 0);
  for (int i = 0; i < n; i++) {
    out[i] = tmp[n - 1 - i];
  }
  return n;
}

/*
 * The value of a text of digits with one point, such as "12.345", as a
 * value and a scale: false if it has more than 36 digits.
 */
bool parse_fixed(const char *buf, ty_int128_t *value, int *scale) {
  u128 v = 0;
  int frac = -1;
  int sig = 0;

  for (const char *p = buf; *p != '\0'; p++) {
    if (*p == '.') {
      frac = 0;
      continue;
    }
    v = v * 10 + (u128)(*p - '0');
    if (v != 0) {
      sig++;
    }
    if (sig > TY_DEC128_MAX_DIGITS) {
      return false;
    }
    if (frac >= 0) {
      frac++;
    }
  }
  *value = (ty_int128_t)v;
  *scale = frac < 0 ? 0 : frac;
  return true;
}

} // namespace

/*
 * a as a double, rounded once: a value of 53 bits and a scale of at most
 * 22 is one division of two exact doubles; else by its text.
 */
double ty_dec128_to_double(const ty_dec128_t *a) {
  static const double p10[] = {1e0,  1e1,  1e2,  1e3,  1e4,  1e5,  1e6,  1e7,
                               1e8,  1e9,  1e10, 1e11, 1e12, 1e13, 1e14, 1e15,
                               1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22};
  char buf[96];
  char dig[48];
  int n;
  int pos = 0;

  if (a->scale <= 22 && magnitude(a->value) < ((u128)1 << 53)) {
    return (double)(int64_t)a->value / p10[a->scale];
  }
  n = digits_of(magnitude(a->value), dig);
  if (a->value < 0) {
    buf[pos++] = '-';
  }
  if (n <= a->scale) {
    buf[pos++] = '0';
    buf[pos++] = '.';
    for (int i = n; i < a->scale; i++) {
      buf[pos++] = '0';
    }
    for (int i = 0; i < n; i++) {
      buf[pos++] = dig[i];
    }
  } else {
    int ip = n - a->scale;
    for (int i = 0; i < ip; i++) {
      buf[pos++] = dig[i];
    }
    if (a->scale > 0) {
      buf[pos++] = '.';
      for (int i = ip; i < n; i++) {
        buf[pos++] = dig[i];
      }
    }
  }
  buf[pos] = '\0';
  return strtod(buf, nullptr);
}

/*
 * *result = x at rscale places, rounded half away from zero.
 * 1. If x is not finite, or has 37 digits before the point, fail.
 * 2. Print it at rscale places.
 * 3. If that has more than 36 digits, print it at fewer places, so that
 *    it has 36, as the result of a function of the exponent is rounded.
 * 4. Take the digits as the value.
 */
int ty_dec128_from_double(double x, int rscale, ty_dec128_t *result) {
  char buf[512];
  ty_int128_t v;
  int scale = rscale;

  if (!std::isfinite(x) || fabs(x) >= 1e37) {
    return range_error();
  }
  for (;;) {
    snprintf(buf, sizeof(buf), "%.*f", scale, fabs(x));
    int len = 0;
    int point = -1;
    for (; buf[len] != '\0'; len++) {
      if (buf[len] == '.') {
        point = len;
      }
    }
    int int_digits = point < 0 ? len : point;
    if (buf[0] == '0') {
      int_digits = 0;
    }
    if (int_digits + scale <= TY_DEC128_MAX_DIGITS || scale == 0) {
      break;
    }
    scale = TY_DEC128_MAX_DIGITS - int_digits;
    if (scale < 0) {
      scale = 0;
    }
  }
  if (!parse_fixed(buf, &v, &scale)) {
    return range_error();
  }
  result->value = x < 0 ? -v : v;
  result->scale = scale;
  return 0;
}

/*
 * *result = sqrt(a) at rscale places, rounded half up.
 * 1. If a is negative, fail.
 * 2. Take N = |value| * 10^(2 * rscale - scale), as a stream of decimal
 *    digits: the digits of the value, then zeros.
 * 3. Bring down two digits at a time, as by hand: the remainder is
 *    100 * remainder + the pair; the next digit of the root is the
 *    largest d with (20 * root + d) * d at most the remainder. With a root
 *    below 10^36 nothing passes 128 bits.
 * 4. The root is floor(sqrt(N)); round it up if the remainder is above it.
 */
int ty_dec128_sqrt(const ty_dec128_t *a, int rscale, ty_dec128_t *result) {
  char dig[48];
  int nd = 0;
  int e = 2 * rscale - a->scale;
  u128 x = magnitude(a->value);

  if (a->value < 0) {
    return ty_internal::set_error(TY_ERR_INVALID,
                                  "square root of a negative number");
  }
  if (e < 0) {
    for (int i = 0; i < -e && x != 0; i++) {
      x /= 10;
    }
    e = 0;
  }
  if (x != 0) {
    nd = digits_of(x, dig);
  }
  u128 root = 0;
  u128 rem = 0;
  if (nd != 0) {
    int total = nd + e;
    int lead = total % 2;
    for (int pos = -lead; pos < total; pos += 2) {
      int hi = pos < 0 ? 0 : (pos < nd ? dig[pos] - '0' : 0);
      int lo = pos + 1 < nd ? dig[pos + 1] - '0' : 0;
      rem = rem * 100 + (u128)(hi * 10 + lo);
      int d = 9;
      while ((20 * root + (u128)d) * (u128)d > rem) {
        d--;
      }
      rem -= (20 * root + (u128)d) * (u128)d;
      root = root * 10 + (u128)d;
      if (root > max_value) {
        return range_error();
      }
    }
  }
  if (rem > root) {
    root++;
    if (root > max_value) {
      return range_error();
    }
  }
  result->value = (ty_int128_t)root;
  result->scale = rscale;
  return 0;
}

/*
 * A double result x of exp or a power, as a value at rscale places: overflow
 * and underflow are errors.
 */
static int from_big_small(double x, int rscale, ty_dec128_t *result) {
  if (std::isinf(x)) {
    return range_error();
  }
  if (x == 0 || fabs(x) < DBL_MIN) {
    return ty_internal::set_error(TY_ERR_UNDERFLOW, "result underflows");
  }
  return ty_dec128_from_double(x, rscale, result);
}

/* *result = e raised to a, at rscale places. */
int ty_dec128_exp(const ty_dec128_t *a, int rscale, ty_dec128_t *result) {
  return from_big_small(exp(ty_dec128_to_double(a)), rscale, result);
}

/* *result = ln(a) at rscale places; a must be above zero. */
int ty_dec128_ln(const ty_dec128_t *a, int rscale, ty_dec128_t *result) {
  if (a->value <= 0) {
    return ty_internal::set_error(TY_ERR_INVALID,
                                  "logarithm of a non-positive");
  }
  return ty_dec128_from_double(log(ty_dec128_to_double(a)), rscale, result);
}

/*
 * *result = log(base, a) at rscale places; both above zero. Fails with
 * TY_ERR_DIVZERO if the base is 1.
 */
int ty_dec128_log(const ty_dec128_t *base, const ty_dec128_t *a, int rscale,
                  ty_dec128_t *result) {
  if (base->value <= 0 || a->value <= 0) {
    return ty_internal::set_error(TY_ERR_INVALID,
                                  "logarithm of a non-positive");
  }
  double lb = ty_dec128_to_double(base);
  double la = ty_dec128_to_double(a);
  double q;

  /* log10 and log2 give a power of the base exactly; log(x) / log(b) not. */
  if (lb == 10) {
    q = log10(la);
  } else if (lb == 2) {
    q = log2(la);
  } else {
    double ln_base = log(lb);
    if (ln_base == 0) {
      return ty_internal::set_error(TY_ERR_DIVZERO, "division by zero");
    }
    q = log(la) / ln_base;
  }
  return ty_dec128_from_double(q, rscale, result);
}

/*
 * *result = base^n at rscale places, for an integer n.
 * 1. Multiply base's value n times in 128 bits, while it fits: the power
 *    is then exact, at the scale base's scale times |n|.
 * 2. Else take it in double.
 * 3. For a negative n, divide 1 by the exact power, or take the long
 *    double's.
 * The base is not zero.
 */
int ty_dec128_pow_int(const ty_dec128_t *base, int64_t n, int rscale,
                      ty_dec128_t *result) {
  u128 m = 1;
  u128 b = magnitude(base->value);
  uint64_t k = n < 0 ? (uint64_t)0 - (uint64_t)n : (uint64_t)n;
  bool fits = true;
  double scale_limit = 1e38;

  if ((double)k * (double)base->scale > TY_DEC128_MAX_SCALE * 1.0) {
    fits = false;
  }
  /* 1 to any power is 1; any other b soon does not fit */
  for (uint64_t i = 0; fits && b != 1 && i < k; i++) {
    if (__builtin_mul_overflow(m, b, &m) || (double)m > scale_limit) {
      fits = false;
    }
  }
  if (!fits) {
    return from_big_small(pow(ty_dec128_to_double(base), (double)n), rscale,
                          result);
  }
  ty_dec128_t p = {(ty_int128_t)m, (int)(base->scale * k)};
  if (base->value < 0 && (k & 1)) {
    p.value = -p.value;
  }
  if (p.value > (ty_int128_t)max_value || p.value < -(ty_int128_t)max_value) {
    return from_big_small(pow(ty_dec128_to_double(base), (double)n), rscale,
                          result);
  }
  if (n < 0) {
    ty_dec128_t one = {1, 0};
    return ty_dec128_div(&one, &p, rscale, false, result);
  }
  return ty_dec128_round(&p, rscale, result);
}

/*
 * *result = base^exp at rscale places, for a real exp: exp(ln|base| * exp),
 * with the sign of base if exp is an odd integer. The base is above zero,
 * or below it and exp an integer.
 */
int ty_dec128_pow_real(const ty_dec128_t *base, const ty_dec128_t *ex,
                       int rscale, ty_dec128_t *result) {
  ty_dec128_t abs_base = ty_dec128_abs(*base);
  double ln_num = log(ty_dec128_to_double(&abs_base)) * ty_dec128_to_double(ex);
  double x = exp(ln_num);
  bool odd = false;

  if (ex->scale == 0) {
    odd = (ex->value % 2) != 0;
  } else {
    ty_dec128_t whole;
    ty_dec128_t e = *ex;
    if (ty_dec128_trunc(&e, 0, &whole) == 0 && ty_dec128_cmp(whole, e) == 0) {
      odd = (whole.value % 2) != 0;
    }
  }
  if (base->value < 0 && odd) {
    x = -x;
  }
  return from_big_small(x, rscale, result);
}

/*
 * Copyright (c) 2026 CK Tan
 * SPDX-License-Identifier: MIT
 */

/*
 * decimal.cc -- the operations of decimal64 and decimal128.
 */

#include "ty_decimal.h"

#include "error.h"

namespace {

/* The largest value of a decimal64: 10^17 - 1. */
constexpr int64_t d64_max = 99999999999999999;

/* Record that a result does not fit a decimal64; returns -1. */
int range_error() {
  return ty_internal::set_error(TY_ERR_RANGE, "decimal64 result out of range");
}

/*
 * *result = a + b, or a - b if subtract, at the larger of the scales.
 * 1. Multiply the value of the smaller scale by 10 until the scales
 *    are equal; if that does not fit 64 bits, fail.
 * 2. Add or subtract them; if that does not fit 64 bits, fail.
 * 3. If the result has more than 17 digits, fail.
 * 4. Set the result.
 */
int add_or_sub(const ty_decimal64_t *a, const ty_decimal64_t *b, bool subtract,
               ty_decimal64_t *result) {
  int64_t x = a->value;
  int64_t y = b->value;
  int sa = a->scale;
  int sb = b->scale;

  for (; sa < sb; sa++) {
    if (__builtin_mul_overflow(x, 10, &x)) {
      return range_error();
    }
  }
  for (; sb < sa; sb++) {
    if (__builtin_mul_overflow(y, 10, &y)) {
      return range_error();
    }
  }
  int64_t r;
  bool overflow = subtract ? __builtin_sub_overflow(x, y, &r)
                           : __builtin_add_overflow(x, y, &r);
  if (overflow || r > d64_max || r < -d64_max) {
    return range_error();
  }
  result->value = r;
  result->scale = sa;
  return 0;
}

/* 10^36 - 1: the largest value of a decimal128. */
constexpr ty_int128_t d128_max = [] {
  ty_int128_t r = 1;
  for (int i = 0; i < TY_DECIMAL128_MAX_DIGITS; i++) {
    r *= 10;
  }
  return r - 1;
}();

/* Record that a result does not fit a decimal128; returns -1. */
int range_error128() {
  return ty_internal::set_error(TY_ERR_RANGE, "decimal128 result out of range");
}

/*
 * *result = a + b, or a - b if subtract, at the larger of the scales.
 * 1. Multiply the value of the smaller scale by 10 until the scales are
 *    equal; if that does not fit 128 bits, fail: the result would have
 *    more than 36 digits, as the other value cannot cancel it.
 * 2. Add or subtract them; if that does not fit 128 bits, fail.
 * 3. If the result has more than 36 digits, fail.
 * 4. Set the result.
 */
int add_or_sub128(const ty_decimal128_t *a, const ty_decimal128_t *b,
                  bool subtract, ty_decimal128_t *result) {
  ty_int128_t x = a->value;
  ty_int128_t y = b->value;
  int sa = a->scale;
  int sb = b->scale;

  for (; sa < sb; sa++) {
    if (__builtin_mul_overflow(x, (ty_int128_t)10, &x)) {
      return range_error128();
    }
  }
  for (; sb < sa; sb++) {
    if (__builtin_mul_overflow(y, (ty_int128_t)10, &y)) {
      return range_error128();
    }
  }
  ty_int128_t r;
  bool overflow = subtract ? __builtin_sub_overflow(x, y, &r)
                           : __builtin_add_overflow(x, y, &r);
  if (overflow || r > d128_max || r < -d128_max) {
    return range_error128();
  }
  result->value = r;
  result->scale = sa;
  return 0;
}

/* A value as unsigned: |v|. */
typedef unsigned __int128 u128;
u128 magnitude(ty_int128_t v) { return v < 0 ? -(u128)v : (u128)v; }

/* gcd of |x| and |y| by Euclid's algorithm; gcd(0, y) is |y|. */
u128 u128_gcd(ty_int128_t x, ty_int128_t y) {
  u128 a = magnitude(x);
  u128 b = magnitude(y);

  while (b != 0) {
    u128 t = a % b;
    a = b;
    b = t;
  }
  return a;
}

/*
 * a and b's values at the larger of their scales, in 128 bits: each
 * value is below 10^17 and a scale is at most 17, so they fit. Returns
 * the scale.
 */
int common_scale(const ty_decimal64_t *a, const ty_decimal64_t *b,
                 ty_int128_t *x, ty_int128_t *y) {
  int sa = a->scale;
  int sb = b->scale;

  *x = a->value;
  *y = b->value;
  for (; sa < sb; sa++) {
    *x *= 10;
  }
  for (; sb < sa; sb++) {
    *y *= 10;
  }
  return sa;
}

/* v / 10^drop, for drop up to 18: rounded half away from zero, or cut. */
int64_t drop_places64(int64_t v, int drop, bool cut) {
  int64_t d = 1;
  for (int i = 0; i < drop; i++) {
    d *= 10;
  }
  int64_t q = v / d;
  int64_t rem = v % d;

  if (rem < 0) {
    rem = -rem;
  }
  if (!cut && 2 * rem >= d) {
    q += v < 0 ? -1 : 1;
  }
  return q;
}

/*
 * *result = a rounded, or cut if cut, to n places.
 * 1. If n is not below the scale, add zeros: the scale becomes n, and
 *    fails if n is above 17 or the value has more than 17 digits.
 * 2. If n is at least 0, drop the places past n: the scale becomes n.
 * 3. Else drop the scale and the places left of the point, and put the
 *    zeros back: the scale is 0. A value below 10^17 drops to 0 once 18
 *    places are dropped, so that is where the value is cut off.
 */
int round_or_cut(const ty_decimal64_t *a, int n, bool cut,
                 ty_decimal64_t *result) {
  int64_t v = a->value;
  int s = a->scale;

  if (n >= s) {
    if (n > TY_DECIMAL64_MAX_SCALE) {
      return range_error();
    }
    ty_int128_t w = v;
    for (int i = s; i < n; i++) {
      w *= 10;
    }
    if (w > d64_max || w < -d64_max) {
      return range_error();
    }
    result->value = (int64_t)w;
    result->scale = n;
    return 0;
  }
  if (n >= 0) {
    result->value = drop_places64(v, s - n, cut);
    result->scale = n;
    return 0;
  }
  int drop = s - n;
  int64_t q = drop > 18 ? 0 : drop_places64(v, drop, cut);
  ty_int128_t w = q;
  for (int i = 0; i < -n && w != 0; i++) {
    w *= 10;
    if (w > d64_max || w < -d64_max) {
      return range_error();
    }
  }
  result->value = (int64_t)w;
  result->scale = 0;
  return 0;
}

/* The nearest whole number at or above a, or at or below if floor. */
ty_decimal64_t ceil_or_floor(ty_decimal64_t a, bool floor) {
  if (a.scale == 0) {
    return a;
  }
  int64_t p = 1;
  for (int i = 0; i < a.scale; i++) {
    p *= 10;
  }
  int64_t q = a.value / p;
  if (a.value % p != 0 && (a.value < 0) == floor) {
    q += floor ? -1 : 1;
  }
  return {q, 0};
}

} // namespace

/* *result = a + b; fails past 17 digits. */
int ty_decimal64_add(const ty_decimal64_t *a, const ty_decimal64_t *b,
                     ty_decimal64_t *result) {
  return add_or_sub(a, b, false, result);
}

/* *result = a - b; fails past 17 digits. */
int ty_decimal64_sub(const ty_decimal64_t *a, const ty_decimal64_t *b,
                     ty_decimal64_t *result) {
  return add_or_sub(a, b, true, result);
}

/*
 * *result = a * b, at the sum of the scales.
 * 1. Multiply in 128 bits: two values of 64 bits always fit.
 * 2. If the product has more than 17 digits, or the scale is above 17,
 *    fail.
 * 3. Set the result.
 */
int ty_decimal64_mul(const ty_decimal64_t *a, const ty_decimal64_t *b,
                     ty_decimal64_t *result) {
  ty_int128_t p = (ty_int128_t)a->value * b->value;
  int s = a->scale + b->scale;

  if (s > TY_DECIMAL64_MAX_SCALE || p > d64_max || p < -d64_max) {
    return range_error();
  }
  result->value = (int64_t)p;
  result->scale = s;
  return 0;
}

/*
 * *result = a % b, at the larger of the scales.
 * 1. Put both values at the larger scale, in 128 bits.
 * 2. If b is zero, fail.
 * 3. The remainder has the sign of a, and is below |b| and not above
 *    |a| at that scale, so it fits 64 bits.
 */
int ty_decimal64_mod(const ty_decimal64_t *a, const ty_decimal64_t *b,
                     ty_decimal64_t *result) {
  ty_int128_t x, y;
  int s = common_scale(a, b, &x, &y);

  if (y == 0) {
    return ty_internal::set_error(TY_ERR_DIVZERO, "division by zero");
  }
  result->value = (int64_t)(x % y);
  result->scale = s;
  return 0;
}

/*
 * *result = gcd(a, b), at the larger of the scales.
 * 1. Put both values at the larger scale, in 128 bits.
 * 2. Take their gcd.
 * 3. If it has more than 17 digits, fail: one is zero and the other
 *    is large at that scale.
 */
int ty_decimal64_gcd(const ty_decimal64_t *a, const ty_decimal64_t *b,
                     ty_decimal64_t *result) {
  ty_int128_t x, y;
  int s = common_scale(a, b, &x, &y);
  u128 g = u128_gcd(x, y);

  if (g > (u128)d64_max) {
    return range_error();
  }
  result->value = (int64_t)g;
  result->scale = s;
  return 0;
}

/*
 * *result = lcm(a, b), at the larger of the scales.
 * 1. Put both values at the larger scale, in 128 bits.
 * 2. If either is zero, the lcm is zero.
 * 3. Take |x| / gcd(x, y) * |y|; if that does not fit 128 bits, or has
 *    more than 17 digits, fail.
 */
int ty_decimal64_lcm(const ty_decimal64_t *a, const ty_decimal64_t *b,
                     ty_decimal64_t *result) {
  ty_int128_t x, y;
  int s = common_scale(a, b, &x, &y);
  u128 l = 0;

  if (x != 0 && y != 0) {
    u128 g = u128_gcd(x, y);
    if (__builtin_mul_overflow(magnitude(x) / g, magnitude(y), &l) ||
        l > (u128)d64_max) {
      return range_error();
    }
  }
  result->value = (int64_t)l;
  result->scale = s;
  return 0;
}

/*
 * a <=> b, of two scales.
 * 1. Put both values at the larger scale, in 128 bits.
 * 2. Compare them.
 */
int ty_decimal64_cmp_scaled(ty_decimal64_t a, ty_decimal64_t b) {
  ty_int128_t x, y;

  common_scale(&a, &b, &x, &y);
  return (x > y) - (x < y);
}

/* *result = a rounded to n places, half away from zero. */
int ty_decimal64_round(const ty_decimal64_t *a, int n, ty_decimal64_t *result) {
  return round_or_cut(a, n, false, result);
}

/* *result = a truncated to n places. */
int ty_decimal64_trunc(const ty_decimal64_t *a, int n, ty_decimal64_t *result) {
  return round_or_cut(a, n, true, result);
}

/* The nearest whole number at or above a. */
ty_decimal64_t ty_decimal64_ceil(ty_decimal64_t a) {
  return ceil_or_floor(a, false);
}

/* The nearest whole number at or below a. */
ty_decimal64_t ty_decimal64_floor(ty_decimal64_t a) {
  return ceil_or_floor(a, true);
}

/* *result = a + b; fails past 36 digits. */
int ty_decimal128_add(const ty_decimal128_t *a, const ty_decimal128_t *b,
                      ty_decimal128_t *result) {
  return add_or_sub128(a, b, false, result);
}

/* *result = a - b; fails past 36 digits. */
int ty_decimal128_sub(const ty_decimal128_t *a, const ty_decimal128_t *b,
                      ty_decimal128_t *result) {
  return add_or_sub128(a, b, true, result);
}

/*
 * *result = a * b, at the sum of the scales.
 * 1. Multiply in 128 bits; if that does not fit, the product has more
 *    than 36 digits: fail.
 * 2. If the product has more than 36 digits, or the scale is above 36,
 *    fail.
 * 3. Set the result.
 */
int ty_decimal128_mul(const ty_decimal128_t *a, const ty_decimal128_t *b,
                      ty_decimal128_t *result) {
  ty_int128_t p;
  int s = a->scale + b->scale;

  if (__builtin_mul_overflow(a->value, b->value, &p) ||
      s > TY_DECIMAL128_MAX_SCALE || p > d128_max || p < -d128_max) {
    return range_error128();
  }
  result->value = p;
  result->scale = s;
  return 0;
}

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
int add_or_sub(const ty_dec64_t *a, const ty_dec64_t *b, bool subtract,
               ty_dec64_t *result) {
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
  for (int i = 0; i < TY_DEC128_MAX_DIGITS; i++) {
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
int add_or_sub128(const ty_dec128_t *a, const ty_dec128_t *b, bool subtract,
                  ty_dec128_t *result) {
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

/* 10^n for n up to 38, as a 128-bit integer. */
ty_int128_t pow10_128(int n) {
  ty_int128_t r = 1;
  for (; n > 0; n--) {
    r *= 10;
  }
  return r;
}

/* v / 10^drop, for drop up to 36: rounded half away from zero, or cut. */
ty_int128_t drop_places128(ty_int128_t v, int drop, bool cut) {
  ty_int128_t d = pow10_128(drop);
  ty_int128_t q = v / d;
  ty_int128_t rem = v % d;

  if (rem < 0) {
    rem = -rem;
  }
  if (!cut && rem >= d - rem) {
    q += v < 0 ? -1 : 1;
  }
  return q;
}

/*
 * *result = a rounded, or cut if cut, to n places, of a decimal128.
 * 1. An n above 36 is 36.
 * 2. If n is not below the scale, add zeros: fail if the value passes
 *    36 digits.
 * 3. If n is at least 0, drop the places past n.
 * 4. Else drop the scale and the places left of the point, and put the
 *    zeros back: the scale is 0. A value below 10^36 is 0 once 37 places
 *    are dropped.
 */
int round_or_cut128(const ty_dec128_t *a, int n, bool cut,
                    ty_dec128_t *result) {
  ty_int128_t v = a->value;
  int s = a->scale;

  if (n > TY_DEC128_MAX_SCALE) {
    n = TY_DEC128_MAX_SCALE;
  }
  if (n >= s) {
    for (int i = s; i < n; i++) {
      if (__builtin_mul_overflow(v, (ty_int128_t)10, &v)) {
        return range_error128();
      }
    }
    if (v > d128_max || v < -d128_max) {
      return range_error128();
    }
    result->value = v;
    result->scale = n;
    return 0;
  }
  if (n >= 0) {
    result->value = drop_places128(v, s - n, cut);
    result->scale = n;
    return 0;
  }
  int drop = s - n;
  ty_int128_t q =
      drop > TY_DEC128_MAX_DIGITS ? 0 : drop_places128(v, drop, cut);
  for (int i = 0; i < -n && q != 0; i++) {
    if (__builtin_mul_overflow(q, (ty_int128_t)10, &q) || q > d128_max ||
        q < -d128_max) {
      return range_error128();
    }
  }
  result->value = q;
  result->scale = 0;
  return 0;
}

/* The nearest whole number at or above a, or at or below if floor. */
ty_dec128_t ceil_or_floor128(ty_dec128_t a, bool floor) {
  if (a.scale == 0) {
    return a;
  }
  ty_int128_t p = pow10_128(a.scale);
  ty_int128_t q = a.value / p;
  if (a.value % p != 0 && (a.value < 0) == floor) {
    q += floor ? -1 : 1;
  }
  return {q, 0};
}

/* A value as unsigned: |v|. */
typedef unsigned __int128 u128;
u128 magnitude(ty_int128_t v) { return v < 0 ? -(u128)v : (u128)v; }

/* gcd of a and b by Euclid's algorithm; gcd(0, b) is b. */
u128 gcd_u128(u128 a, u128 b) {
  while (b != 0) {
    u128 t = a % b;
    a = b;
    b = t;
  }
  return a;
}

/* gcd of |x| and |y|. */
u128 u128_gcd(ty_int128_t x, ty_int128_t y) {
  return gcd_u128(magnitude(x), magnitude(y));
}

/*
 * v * 10^k mod m, for the magnitudes v and m (m not zero, below 10^37):
 * one place at a time, so that no step passes 128 bits.
 */
u128 mulpow10_mod(u128 v, int k, u128 m) {
  u128 r = v % m;

  for (; k > 0; k--) {
    r = r * 10 % m;
  }
  return r;
}

/*
 * a and b's values at the larger scale, as magnitudes, if both fit 128
 * bits there: the scale is returned. Else *over is 1 if a's does not, and
 * 2 if b's does not, and the magnitudes are the originals.
 */
int align128(const ty_dec128_t *a, const ty_dec128_t *b, u128 *x, u128 *y,
             int *over, int *k) {
  int sa = a->scale;
  int sb = b->scale;

  *over = 0;
  *x = magnitude(a->value);
  *y = magnitude(b->value);
  *k = sa < sb ? sb - sa : sa - sb;
  for (int i = 0; i < *k; i++) {
    if (sa < sb) {
      if (__builtin_mul_overflow(*x, (u128)10, x)) {
        *over = 1;
        break;
      }
    } else if (__builtin_mul_overflow(*y, (u128)10, y)) {
      *over = 2;
      break;
    }
  }
  if (*over != 0) {
    *x = magnitude(a->value);
    *y = magnitude(b->value);
  }
  return sa > sb ? sa : sb;
}

/*
 * a and b's values at the larger of their scales, in 128 bits: each
 * value is below 10^17 and a scale is at most 17, so they fit. Returns
 * the scale.
 */
int common_scale(const ty_dec64_t *a, const ty_dec64_t *b, ty_int128_t *x,
                 ty_int128_t *y) {
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
int round_or_cut(const ty_dec64_t *a, int n, bool cut, ty_dec64_t *result) {
  int64_t v = a->value;
  int s = a->scale;

  if (n >= s) {
    if (n > TY_DEC64_MAX_SCALE) {
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
ty_dec64_t ceil_or_floor(ty_dec64_t a, bool floor) {
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
int ty_dec64_add_scaled(const ty_dec64_t *a, const ty_dec64_t *b,
                        ty_dec64_t *result) {
  return add_or_sub(a, b, false, result);
}

/* *result = a - b; fails past 17 digits. */
int ty_dec64_sub_scaled(const ty_dec64_t *a, const ty_dec64_t *b,
                        ty_dec64_t *result) {
  return add_or_sub(a, b, true, result);
}

/*
 * *result = a % b, at the larger of the scales.
 * 1. Put both values at the larger scale, in 128 bits.
 * 2. If b is zero, fail.
 * 3. The remainder has the sign of a, and is below |b| and not above
 *    |a| at that scale, so it fits 64 bits.
 */
int ty_dec64_mod(const ty_dec64_t *a, const ty_dec64_t *b, ty_dec64_t *result) {
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
int ty_dec64_gcd(const ty_dec64_t *a, const ty_dec64_t *b, ty_dec64_t *result) {
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
int ty_dec64_lcm(const ty_dec64_t *a, const ty_dec64_t *b, ty_dec64_t *result) {
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
int ty_dec64_cmp_scaled(ty_dec64_t a, ty_dec64_t b) {
  ty_int128_t x, y;

  common_scale(&a, &b, &x, &y);
  return (x > y) - (x < y);
}

/* *result = a rounded to n places, half away from zero. */
int ty_dec64_round(const ty_dec64_t *a, int n, ty_dec64_t *result) {
  return round_or_cut(a, n, false, result);
}

/* *result = a truncated to n places. */
int ty_dec64_trunc(const ty_dec64_t *a, int n, ty_dec64_t *result) {
  return round_or_cut(a, n, true, result);
}

/* The nearest whole number at or above a. */
ty_dec64_t ty_dec64_ceil(ty_dec64_t a) { return ceil_or_floor(a, false); }

/* The nearest whole number at or below a. */
ty_dec64_t ty_dec64_floor(ty_dec64_t a) { return ceil_or_floor(a, true); }

/* *result = a + b; fails past 36 digits. */
int ty_dec128_add_scaled(const ty_dec128_t *a, const ty_dec128_t *b,
                         ty_dec128_t *result) {
  return add_or_sub128(a, b, false, result);
}

/* *result = a - b; fails past 36 digits. */
int ty_dec128_sub_scaled(const ty_dec128_t *a, const ty_dec128_t *b,
                         ty_dec128_t *result) {
  return add_or_sub128(a, b, true, result);
}

/* *result = a rounded to n places, half away from zero. */
int ty_dec128_round(const ty_dec128_t *a, int n, ty_dec128_t *result) {
  return round_or_cut128(a, n, false, result);
}

/* *result = a truncated to n places. */
int ty_dec128_trunc(const ty_dec128_t *a, int n, ty_dec128_t *result) {
  return round_or_cut128(a, n, true, result);
}

/* The nearest whole number at or above a. */
ty_dec128_t ty_dec128_ceil(ty_dec128_t a) { return ceil_or_floor128(a, false); }

/* The nearest whole number at or below a. */
ty_dec128_t ty_dec128_floor(ty_dec128_t a) { return ceil_or_floor128(a, true); }

/*
 * a <=> b of a decimal128, of two scales.
 * 1. Multiply the value of the smaller scale by 10 until the scales are
 *    equal. If that does not fit 128 bits, its magnitude is above that of
 *    any value of 36 digits: its sign decides.
 * 2. Compare them.
 */
int ty_dec128_cmp_scaled(ty_dec128_t a, ty_dec128_t b) {
  ty_int128_t x = a.value;
  ty_int128_t y = b.value;
  int sa = a.scale;
  int sb = b.scale;

  for (; sa < sb; sa++) {
    if (__builtin_mul_overflow(x, (ty_int128_t)10, &x)) {
      return a.value > 0 ? 1 : -1;
    }
  }
  for (; sb < sa; sb++) {
    if (__builtin_mul_overflow(y, (ty_int128_t)10, &y)) {
      return b.value > 0 ? -1 : 1;
    }
  }
  return (x > y) - (x < y);
}

/*
 * *result = a % b, at the larger scale, the sign of a.
 * 1. If b is zero, fail.
 * 2. If a has the smaller scale, the remainder is the one of |a| * 10^k
 *    by |b|: take it a place at a time. Else if b at the larger scale does
 *    not fit 128 bits, |a| is below it, and is the remainder.
 * 3. Else both fit: take the remainder.
 */
int ty_dec128_mod(const ty_dec128_t *a, const ty_dec128_t *b,
                  ty_dec128_t *result) {
  u128 x, y;
  int over, k;
  int s = align128(a, b, &x, &y, &over, &k);
  u128 r;

  if (b->value == 0) {
    return ty_internal::set_error(TY_ERR_DIVZERO, "division by zero");
  }
  if (over == 1) {
    r = mulpow10_mod(x, k, y);
  } else if (over == 2) {
    r = x;
  } else {
    r = x % y;
  }
  result->value = a->value < 0 ? -(ty_int128_t)r : (ty_int128_t)r;
  result->scale = s;
  return 0;
}

/*
 * *result = gcd(a, b), at the larger scale.
 * 1. If both fit 128 bits at the larger scale, take their gcd.
 * 2. Else the one that does not is a multiple of 10^k of a value below
 *    10^36: take its remainder by the other a place at a time, and the
 *    gcd of that and the other. If the other is zero, the gcd is the
 *    one that does not fit: fail.
 * 3. If the gcd has more than 36 digits, fail.
 */
int ty_dec128_gcd(const ty_dec128_t *a, const ty_dec128_t *b,
                  ty_dec128_t *result) {
  u128 x, y;
  int over, k;
  int s = align128(a, b, &x, &y, &over, &k);
  u128 g;

  if (over == 0) {
    g = gcd_u128(x, y);
  } else {
    u128 big = over == 1 ? x : y;
    u128 other = over == 1 ? y : x;

    if (other == 0) {
      return range_error128();
    }
    g = gcd_u128(other, mulpow10_mod(big, k, other));
  }
  if (g > (u128)d128_max) {
    return range_error128();
  }
  result->value = (ty_int128_t)g;
  result->scale = s;
  return 0;
}

/*
 * *result = lcm(a, b), at the larger scale.
 * 1. If either is zero, the lcm is zero.
 * 2. If either does not fit 128 bits at the larger scale, the lcm is above
 *    10^36: fail.
 * 3. Take |x| / gcd * |y|; if it does not fit 128 bits, or has more than
 *    36 digits, fail.
 */
int ty_dec128_lcm(const ty_dec128_t *a, const ty_dec128_t *b,
                  ty_dec128_t *result) {
  u128 x, y;
  int over, k;
  int s = align128(a, b, &x, &y, &over, &k);
  u128 l = 0;

  if (a->value != 0 && b->value != 0) {
    if (over != 0) {
      return range_error128();
    }
    u128 g = gcd_u128(x, y);
    if (__builtin_mul_overflow(x / g, y, &l) || l > (u128)d128_max) {
      return range_error128();
    }
  }
  result->value = (ty_int128_t)l;
  result->scale = s;
  return 0;
}

/*
 * *result = a / b at rscale places.
 * 1. If b is zero, fail.
 * 2. The quotient is |a| * 10^e / |b|, with e = rscale + sb - sa.
 * 3. If e is not below 0: take |a| / |b| and its remainder, then bring
 *    down e places in chunks: each time, the remainder times 10^k, where
 *    |b| * 10^k fits 128 bits, gives the next k places and a new remainder.
 *    If the quotient passes 36 digits, fail.
 * 4. Else divide |a| by |b| * 10^-e; if that passes 128 bits, the quotient
 *    is 0, as |a| is below it.
 * 5. Round up, unless cut, if the remainder is half of the divisor or more.
 * 6. Give the quotient the sign of a times the sign of b.
 */
int ty_dec128_div(const ty_dec128_t *a, const ty_dec128_t *b, int rscale,
                  bool cut, ty_dec128_t *result) {
  u128 x = magnitude(a->value);
  u128 y = magnitude(b->value);
  int e = rscale + b->scale - a->scale;
  u128 q, rem, den = y;

  if (y == 0) {
    return ty_internal::set_error(TY_ERR_DIVZERO, "division by zero");
  }
  if (e >= 0) {
    q = x / y;
    rem = x % y;
    if (q > (u128)d128_max) {
      return range_error128();
    }
    while (e > 0) {
      int k = 1;
      u128 p = 10;
      /* The most places at once with rem * 10^k below 2^128. */
      while (k < e && y < (~(u128)0) / (p * 10)) {
        p *= 10;
        k++;
      }
      u128 t = rem * p;
      u128 d = t / y;
      rem = t % y;
      if (__builtin_mul_overflow(q, p, &q) || q > (u128)d128_max ||
          (q += d) > (u128)d128_max) {
        return range_error128();
      }
      e -= k;
    }
  } else {
    q = 0;
    rem = x;
    if (!(e < -38)) {
      u128 p = 1;
      for (int i = 0; i < -e; i++) {
        p *= 10;
      }
      if (!__builtin_mul_overflow(y, p, &den)) {
        q = x / den;
        rem = x % den;
      } else {
        /* The divisor passes 128 bits: x is below half of it. */
        den = ~(u128)0;
      }
    }
  }
  if (!cut && rem >= den - rem) {
    q++;
    if (q > (u128)d128_max) {
      return range_error128();
    }
  }
  bool neg = (a->value < 0) != (b->value < 0) && q != 0;
  result->value = neg ? -(ty_int128_t)q : (ty_int128_t)q;
  result->scale = rscale;
  return 0;
}

/*
 * *result = a / b of two decimal64, fitted to a decimal64.
 * 1. Divide in 128 bits at rscale places.
 * 2. If the scale is above 17, round to 17 places.
 * 3. If there are more than 17 digits and places to spare, round away the
 *    extra digits as places, and one place more if rounding up adds a digit.
 * 4. If there are still more than 17 digits, fail.
 */
int ty_dec64_div(const ty_dec64_t *a, const ty_dec64_t *b, int rscale,
                 ty_dec64_t *result) {
  ty_dec128_t x = {a->value, a->scale};
  ty_dec128_t y = {b->value, b->scale};
  ty_dec128_t q;

  if (ty_dec128_div(&x, &y, rscale, false, &q) != 0) {
    return -1;
  }
  return ty_dec64_from_dec128(&q, result);
}

/*
 * *result = a, a decimal128, fitted to a decimal64.
 * 1. If the scale is above 17, round to 17 places.
 * 2. If there are more than 17 digits and places to spare, round away the
 *    extra digits as places, and one place more if rounding up adds a digit.
 * 3. If there are still more than 17 digits, fail.
 */
int ty_dec64_from_dec128(const ty_dec128_t *a, ty_dec64_t *result) {
  ty_int128_t v = a->value;
  int s = a->scale;

  if (s > TY_DEC64_MAX_SCALE) {
    v = drop_places128(v, s - TY_DEC64_MAX_SCALE, false);
    s = TY_DEC64_MAX_SCALE;
  }
  int digits = 1;
  for (ty_int128_t t = v; t >= 10 || t <= -10; t /= 10) {
    digits++;
  }
  if (digits > TY_DEC64_MAX_DIGITS && digits - TY_DEC64_MAX_DIGITS <= s) {
    int excess = digits - TY_DEC64_MAX_DIGITS;
    v = drop_places128(v, excess, false);
    s -= excess;
    digits = 1;
    for (ty_int128_t t = v; t >= 10 || t <= -10; t /= 10) {
      digits++;
    }
    if (digits > TY_DEC64_MAX_DIGITS && s > 0) {
      v = drop_places128(v, 1, false);
      s--;
    }
  }
  if (v > d64_max || v < -d64_max) {
    return range_error();
  }
  result->value = (int64_t)v;
  result->scale = s;
  return 0;
}

/*
 * n! as a decimal128.
 * 1. If n is negative, fail.
 * 2. Multiply 2 to n in 128 bits; if that does not fit, or has more than
 *    36 digits, fail.
 */
int ty_dec128_factorial(int64_t n, ty_dec128_t *result) {
  u128 r = 1;

  if (n < 0) {
    return ty_internal::set_error(TY_ERR_INVALID,
                                  "factorial of a negative number");
  }
  for (int64_t i = 2; i <= n; i++) {
    if (__builtin_mul_overflow(r, (u128)i, &r) || r > (u128)d128_max) {
      return range_error128();
    }
  }
  result->value = (ty_int128_t)r;
  result->scale = 0;
  return 0;
}

/*
 * The bucket of operand: width_bucket.
 * 1. Check count and the bounds.
 * 2. Before the low bound is 0, and at or after the high bound count + 1.
 * 3. Else num = |operand - low| and den = |high - low|, put to one scale.
 * 4. floor(num * count / den) is made by the bits of count: a quotient
 *    and a remainder, doubled each bit, with num added for a one bit.
 *    num is below den, so no step passes 128 bits.
 */
int ty_dec128_width_bucket(const ty_dec128_t *operand, const ty_dec128_t *b1,
                           const ty_dec128_t *b2, int32_t count,
                           int64_t *result) {
  int c = ty_dec128_cmp(*b1, *b2);

  if (count <= 0 || c == 0) {
    return ty_internal::set_error(TY_ERR_INVALID, "bad width bucket bounds");
  }
  if (c < 0 ? ty_dec128_cmp(*operand, *b1) < 0
            : ty_dec128_cmp(*operand, *b1) > 0) {
    *result = 0;
    return 0;
  }
  if (c < 0 ? ty_dec128_cmp(*operand, *b2) >= 0
            : ty_dec128_cmp(*operand, *b2) <= 0) {
    *result = (int64_t)count + 1;
    return 0;
  }
  ty_dec128_t num, den;
  if ((c < 0 ? ty_dec128_sub(operand, b1, &num)
             : ty_dec128_sub(b1, operand, &num)) != 0 ||
      (c < 0 ? ty_dec128_sub(b2, b1, &den) : ty_dec128_sub(b1, b2, &den)) !=
          0) {
    return -1;
  }
  u128 x = magnitude(num.value);
  u128 y = magnitude(den.value);
  for (int sn = num.scale, sd = den.scale; sn != sd;) {
    u128 *t = sn < sd ? &x : &y;
    if (__builtin_mul_overflow(*t, (u128)10, t)) {
      return range_error128();
    }
    (sn < sd ? sn : sd)++;
  }
  u128 q = 0;
  u128 rem = 0;
  for (int bit = 30; bit >= 0; bit--) {
    q *= 2;
    rem *= 2;
    if (rem >= y) {
      rem -= y;
      q++;
    }
    if ((count >> bit) & 1) {
      rem += x;
      if (rem >= y) {
        rem -= y;
        q++;
      }
    }
  }
  *result = (int64_t)q + 1;
  return 0;
}

namespace {

/* One round of a 64-bit mix, murmur3's finalizer: h folded with w. */
uint64_t mix64(uint64_t h, uint64_t w) {
  h ^= w;
  h ^= h >> 33;
  h *= 0xff51afd7ed558ccdULL;
  h ^= h >> 33;
  h *= 0xc4ceb9fe1a85ec53ULL;
  h ^= h >> 33;
  return h;
}

} // namespace

/*
 * The hash of a decimal128.
 * 1. Drop the zeros at the end, past the point: the value and scale of
 *    its shortest form, and a zero has scale 0.
 * 2. Mix the seed, then the two halves of the value, then the scale.
 */
uint64_t ty_dec128_hash(const ty_dec128_t *a, uint64_t seed) {
  ty_int128_t v = a->value;
  int s = a->scale;

  while (s > 0 && v % 10 == 0) {
    v /= 10;
    s--;
  }
  if (v == 0) {
    s = 0;
  }
  uint64_t h = mix64(seed + 0x9e3779b97f4a7c15ULL, 0x5bd1e9955bd1e995ULL);
  h = mix64(h, (uint64_t)(u128)v);
  h = mix64(h, (uint64_t)((u128)v >> 64));
  return mix64(h, (uint64_t)s);
}

/* The hash of a decimal64: that of the decimal128 of its value. */
uint64_t ty_dec64_hash(const ty_dec64_t *a, uint64_t seed) {
  ty_dec128_t x = {a->value, a->scale};
  return ty_dec128_hash(&x, seed);
}

/* Record the range error that an inline operation found; returns -1. */
int ty_err_range(void) {
  return ty_internal::set_error(TY_ERR_RANGE, "result out of range");
}

/*
 * Copyright (c) 2026 CK Tan
 * SPDX-License-Identifier: MIT
 */

/*
 * int.cc -- the out-of-line operations of int16, int32 and int64: gcd,
 * lcm and the conversion from a double. The rest are in ty_int.h.
 */
#include "ty_int.h"

#include <limits>

#include <math.h>

namespace {

/*
 * The greatest common divisor of two values of T, never negative.
 * 1. Put the value of the greater magnitude in a. The compare is in
 *    negative space, which holds MIN.
 * 2. If a is MIN, the answer is 2^(N-1) when b is 0 or MIN: RANGE. When
 *    b is -1 it is 1, and a % b would trap on some machines.
 * 3. Run Euclid's algorithm.
 * 4. Return the magnitude of what is left; it is not MIN now.
 */
template <typename T> int gcd_of(T a, T b, T *r) {
  const T min = std::numeric_limits<T>::min();
  T na = a < 0 ? a : (T)-a;
  T nb = b < 0 ? b : (T)-b;

  if (na > nb) {
    T t = a;
    a = b;
    b = t;
  }
  if (a == min) {
    if (b == 0 || b == min) {
      return ty_err_range();
    }
    if (b == -1) {
      *r = 1;
      return 0;
    }
  }
  while (b != 0) {
    T t = b;
    b = (T)(a % b);
    a = t;
  }
  *r = a < 0 ? (T)-a : a;
  return 0;
}

/*
 * The least common multiple of two values of T, never negative.
 * 1. If a or b is 0, the answer is 0; gcd is not called, as it would
 *    fail for MIN.
 * 2. Divide a by gcd(a, b), then multiply by b; RANGE on overflow.
 * 3. MIN is a RANGE error too, as its magnitude does not fit; else
 *    return the magnitude.
 */
template <typename T> int lcm_of(T a, T b, T *r) {
  const T min = std::numeric_limits<T>::min();
  T g, v;

  if (a == 0 || b == 0) {
    *r = 0;
    return 0;
  }
  if (gcd_of<T>(a, b, &g) != 0) {
    return -1;
  }
  a = (T)(a / g);
  if (__builtin_mul_overflow(a, b, &v) || v == min) {
    return ty_err_range();
  }
  *r = v < 0 ? (T)-v : v;
  return 0;
}

} // namespace

/* gcd of two int16. */
int ty_int16_gcd(int16_t a, int16_t b, int16_t *r) {
  return gcd_of<int16_t>(a, b, r);
}

/* gcd of two int32. */
int ty_int32_gcd(int32_t a, int32_t b, int32_t *r) {
  return gcd_of<int32_t>(a, b, r);
}

/* gcd of two int64. */
int ty_int64_gcd(int64_t a, int64_t b, int64_t *r) {
  return gcd_of<int64_t>(a, b, r);
}

/* lcm of two int16. */
int ty_int16_lcm(int16_t a, int16_t b, int16_t *r) {
  return lcm_of<int16_t>(a, b, r);
}

/* lcm of two int32. */
int ty_int32_lcm(int32_t a, int32_t b, int32_t *r) {
  return lcm_of<int32_t>(a, b, r);
}

/* lcm of two int64. */
int ty_int64_lcm(int64_t a, int64_t b, int64_t *r) {
  return lcm_of<int64_t>(a, b, r);
}

/*
 * An int64 from a double, rounded to the nearest; a tie goes to the even.
 * 1. Round d; NaN stays NaN.
 * 2. RANGE if it is NaN, or below -2^63, or not below 2^63.
 */
int ty_int64_from_double(double d, int64_t *r) {
  double n = rint(d);

  if (isnan(n) || n < -9223372036854775808.0 || n >= 9223372036854775808.0) {
    return ty_err_range();
  }
  *r = (int64_t)n;
  return 0;
}

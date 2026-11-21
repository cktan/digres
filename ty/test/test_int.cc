/*
 * Copyright (c) 2026 CK Tan
 * SPDX-License-Identifier: MIT
 */

/*
 * Tests of the integer operations.
 */
#include <stdint.h>

#include "doctest.h"
#include "ty_int.h"

// Verify that an int32 operation is exact, or an error that is not wrapped.
// 1. Add 1 + 2, and INT32_MAX + 1: 3, then TY_ERR_RANGE with r unchanged.
// 2. Subtract and multiply at the edges: both overflow.
// 3. Negate and take abs of INT32_MIN: both TY_ERR_RANGE.
// 4. Increment INT32_MAX and decrement INT32_MIN: TY_ERR_RANGE.
TEST_CASE("int32 add, sub, mul, neg, abs, inc and dec are exact or an error") {
  int32_t r = 77;

  // 1. Add 1 + 2, and INT32_MAX + 1: 3, then TY_ERR_RANGE with r unchanged.
  CHECK(0 == ty_int32_add(1, 2, &r));
  CHECK(3 == r);
  CHECK(-1 == ty_int32_add(INT32_MAX, 1, &r));
  CHECK(TY_ERR_RANGE == ty_errnum());
  CHECK(3 == r);

  // 2. Subtract and multiply at the edges: both overflow.
  CHECK(-1 == ty_int32_sub(INT32_MIN, 1, &r));
  CHECK(-1 == ty_int32_mul(65536, 32768, &r));
  CHECK(0 == ty_int32_mul(65536, -32768, &r));
  CHECK(INT32_MIN == r);

  // 3. Negate and take abs of INT32_MIN: both TY_ERR_RANGE.
  CHECK(-1 == ty_int32_neg(INT32_MIN, &r));
  CHECK(-1 == ty_int32_abs(INT32_MIN, &r));
  CHECK(0 == ty_int32_abs(-5, &r));
  CHECK(5 == r);

  // 4. Increment INT32_MAX and decrement INT32_MIN: TY_ERR_RANGE.
  CHECK(-1 == ty_int32_inc(INT32_MAX, &r));
  CHECK(-1 == ty_int32_dec(INT32_MIN, &r));
}

// Verify division: a divisor of zero, MIN / -1, and the sign of the rest.
// 1. Divide by zero: TY_ERR_DIVZERO, for div and for mod.
// 2. Divide INT64_MIN by -1: TY_ERR_RANGE; the remainder of it is 0.
// 3. Divide -7 by 2: -3, as C does, and -7 % 2 is -1.
TEST_CASE("division cuts toward zero and has the sign of the dividend") {
  int64_t r = 0;

  // 1. Divide by zero: TY_ERR_DIVZERO, for div and for mod.
  CHECK(-1 == ty_int64_div(1, 0, &r));
  CHECK(TY_ERR_DIVZERO == ty_errnum());
  CHECK(-1 == ty_int64_mod(1, 0, &r));
  CHECK(TY_ERR_DIVZERO == ty_errnum());

  // 2. Divide INT64_MIN by -1: TY_ERR_RANGE; the remainder of it is 0.
  CHECK(-1 == ty_int64_div(INT64_MIN, -1, &r));
  CHECK(TY_ERR_RANGE == ty_errnum());
  CHECK(0 == ty_int64_mod(INT64_MIN, -1, &r));
  CHECK(0 == r);

  // 3. Divide -7 by 2: -3, as C does, and -7 % 2 is -1.
  CHECK(0 == ty_int64_div(-7, 2, &r));
  CHECK(-3 == r);
  CHECK(0 == ty_int64_mod(-7, 2, &r));
  CHECK(-1 == r);
}

// Verify gcd and lcm, with the cases of the minimum.
// 1. gcd(12, -18) is 6; gcd(0, 0) is 0; gcd(INT32_MIN, -1) is 1.
// 2. gcd(INT32_MIN, 0) and gcd(INT32_MIN, INT32_MIN) are TY_ERR_RANGE.
// 3. lcm(4, 6) is 12; lcm(0, INT32_MIN) is 0; lcm(INT32_MAX, 2) is a range
//    error.
TEST_CASE("gcd and lcm never return a negative, and fail at the minimum") {
  int32_t r = 0;

  // 1. gcd(12, -18) is 6; gcd(0, 0) is 0; gcd(INT32_MIN, -1) is 1.
  CHECK(0 == ty_int32_gcd(12, -18, &r));
  CHECK(6 == r);
  CHECK(0 == ty_int32_gcd(0, 0, &r));
  CHECK(0 == r);
  CHECK(0 == ty_int32_gcd(INT32_MIN, -1, &r));
  CHECK(1 == r);

  // 2. gcd(INT32_MIN, 0) and gcd(INT32_MIN, INT32_MIN) are TY_ERR_RANGE.
  CHECK(-1 == ty_int32_gcd(INT32_MIN, 0, &r));
  CHECK(TY_ERR_RANGE == ty_errnum());
  CHECK(-1 == ty_int32_gcd(INT32_MIN, INT32_MIN, &r));

  // 3. lcm(4, 6) is 12; lcm(0, INT32_MIN) is 0; lcm(INT32_MAX, 2) is a range
  //    error.
  CHECK(0 == ty_int32_lcm(4, 6, &r));
  CHECK(12 == r);
  CHECK(0 == ty_int32_lcm(0, INT32_MIN, &r));
  CHECK(0 == r);
  CHECK(-1 == ty_int32_lcm(INT32_MAX, 2, &r));
}

// Verify shifts: in range they work, outside they are an error.
// 1. 1 << 31 of an int32 is INT32_MIN: the bit goes to the sign, as in C.
// 2. -8 >> 1 is -4: the sign is copied in.
// 3. A count of -1, or of the width, is TY_ERR_RANGE, for both shifts.
TEST_CASE("a shift is by 0 up to the width less one") {
  int32_t r = 0;

  // 1. 1 << 31 of an int32 is INT32_MIN: the bit goes to the sign, as in C.
  CHECK(0 == ty_int32_shl(1, 31, &r));
  CHECK(INT32_MIN == r);

  // 2. -8 >> 1 is -4: the sign is copied in.
  CHECK(0 == ty_int32_shr(-8, 1, &r));
  CHECK(-4 == r);

  // 3. A count of -1, or of the width, is TY_ERR_RANGE, for both shifts.
  CHECK(-1 == ty_int32_shl(1, -1, &r));
  CHECK(-1 == ty_int32_shl(1, 32, &r));
  CHECK(-1 == ty_int32_shr(1, 32, &r));
  CHECK(TY_ERR_RANGE == ty_errnum());
}

// Verify the narrowing, and the double conversion.
// 1. int64 to int32 and int16 at their edges: fit, and one past: range.
// 2. A double rounds to the nearest, a tie to the even: 2.5 is 2, 3.5 is 4.
// 3. A NaN, and 2^63, are TY_ERR_RANGE; -2^63 fits.
TEST_CASE("narrowing and double conversions are checked") {
  int16_t r16 = 0;
  int32_t r32 = 0;
  int64_t r64 = 0;

  // 1. int64 to int32 and int16 at their edges: fit, and one past: range.
  CHECK(0 == ty_int64_to_int32(INT32_MAX, &r32));
  CHECK(-1 == ty_int64_to_int32((int64_t)INT32_MAX + 1, &r32));
  CHECK(0 == ty_int64_to_int16(INT16_MIN, &r16));
  CHECK(-1 == ty_int64_to_int16((int64_t)INT16_MIN - 1, &r16));
  CHECK(0 == ty_int32_to_int16(INT16_MAX, &r16));
  CHECK(-1 == ty_int32_to_int16(INT16_MAX + 1, &r16));

  // 2. A double rounds to the nearest, a tie to the even: 2.5 is 2, 3.5 is 4.
  CHECK(0 == ty_int64_from_double(2.5, &r64));
  CHECK(2 == r64);
  CHECK(0 == ty_int64_from_double(3.5, &r64));
  CHECK(4 == r64);

  // 3. A NaN, and 2^63, are TY_ERR_RANGE; -2^63 fits.
  CHECK(-1 == ty_int64_from_double(__builtin_nan(""), &r64));
  CHECK(-1 == ty_int64_from_double(9223372036854775808.0, &r64));
  CHECK(0 == ty_int64_from_double(-9223372036854775808.0, &r64));
  CHECK(INT64_MIN == r64);
}

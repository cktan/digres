/*
 * Copyright (c) 2026 CK Tan
 * SPDX-License-Identifier: MIT
 */

/*
 * Tests of the decimal operations.
 */
#include <limits.h>
#include <math.h>

#include <algorithm>
#include <vector>

#include "doctest.h"
#include "ty_decimal.h"

// Verify that decimal64 addition is exact, and takes the larger scale.
// 1. Add two values of one scale: 1.25 + 2.50.
// 2. Add two of different scales, in both orders: 1.5 + 0.025.
// 3. Add a negative: 1.5 + -2.
// 4. Add up to the largest value, 17 digits, which fits.
TEST_CASE("decimal64 add is exact, at the larger scale") {
  ty_dec64_t r;

  // 1. Add two values of one scale: 1.25 + 2.50.
  ty_dec64_t a = {125, 2};
  ty_dec64_t b = {250, 2};
  CHECK(0 == ty_dec64_add(&a, &b, &r));
  CHECK(375 == r.value);
  CHECK(2 == r.scale);

  // 2. Add two of different scales, in both orders: 1.5 + 0.025.
  a = {15, 1};
  b = {25, 3};
  CHECK(0 == ty_dec64_add(&a, &b, &r));
  CHECK(1525 == r.value);
  CHECK(3 == r.scale);
  CHECK(0 == ty_dec64_add(&b, &a, &r));
  CHECK(1525 == r.value);
  CHECK(3 == r.scale);

  // 3. Add a negative: 1.5 + -2.
  a = {15, 1};
  b = {-2, 0};
  CHECK(0 == ty_dec64_add(&a, &b, &r));
  CHECK(-5 == r.value);
  CHECK(1 == r.scale);

  // 4. Add up to the largest value, 17 digits, which fits.
  a = {99999999999999998, 0};
  b = {1, 0};
  CHECK(0 == ty_dec64_add(&a, &b, &r));
  CHECK(99999999999999999 == r.value);
  a = {-99999999999999998, 0};
  b = {-1, 0};
  CHECK(0 == ty_dec64_add(&a, &b, &r));
  CHECK(-99999999999999999 == r.value);
}

// Verify that a sum of more than 17 digits fails, and sets no result.
// 1. Add 1 to the largest value: it fails with TY_ERR_RANGE.
// 2. Add the same on the negative side.
// 3. Add values whose scales do not align in 64 bits.
// 4. Check that the result was not set by any of them.
TEST_CASE("decimal64 add fails past 17 digits, and sets no result") {
  ty_dec64_t r = {7, 7};

  // 1. Add 1 to the largest value: it fails with TY_ERR_RANGE.
  ty_dec64_t a = {99999999999999999, 0};
  ty_dec64_t b = {1, 0};
  CHECK(-1 == ty_dec64_add(&a, &b, &r));
  CHECK(TY_ERR_RANGE == ty_errnum());

  // 2. Add the same on the negative side.
  a = {-99999999999999999, 0};
  b = {-1, 0};
  CHECK(-1 == ty_dec64_add(&a, &b, &r));
  CHECK(TY_ERR_RANGE == ty_errnum());

  // 3. Add values whose scales do not align in 64 bits.
  a = {99999999999999999, 0};
  b = {1, 5};
  CHECK(-1 == ty_dec64_add(&a, &b, &r));
  CHECK(TY_ERR_RANGE == ty_errnum());
  a = {1, 0};
  b = {1, 31};
  CHECK(-1 == ty_dec64_add(&a, &b, &r));
  CHECK(TY_ERR_RANGE == ty_errnum());

  // 4. Check that the result was not set by any of them.
  CHECK(7 == r.value);
  CHECK(7 == r.scale);
}

// Verify that decimal64 subtraction is add's mirror.
// 1. Subtract at one scale, and at two: 2.50 - 1.25, and 1.5 - 0.025.
// 2. Subtract to a negative: 1 - 3.
// 3. Subtract past 17 digits: it fails with TY_ERR_RANGE.
TEST_CASE("decimal64 sub is exact, at the larger scale, and fails past 17") {
  ty_dec64_t r;

  // 1. Subtract at one scale, and at two: 2.50 - 1.25, and 1.5 - 0.025.
  ty_dec64_t a = {250, 2};
  ty_dec64_t b = {125, 2};
  CHECK(0 == ty_dec64_sub(&a, &b, &r));
  CHECK(125 == r.value);
  CHECK(2 == r.scale);
  a = {15, 1};
  b = {25, 3};
  CHECK(0 == ty_dec64_sub(&a, &b, &r));
  CHECK(1475 == r.value);
  CHECK(3 == r.scale);

  // 2. Subtract to a negative: 1 - 3.
  a = {1, 0};
  b = {3, 0};
  CHECK(0 == ty_dec64_sub(&a, &b, &r));
  CHECK(-2 == r.value);

  // 3. Subtract past 17 digits: it fails with TY_ERR_RANGE.
  a = {-99999999999999999, 0};
  b = {1, 0};
  CHECK(-1 == ty_dec64_sub(&a, &b, &r));
  CHECK(TY_ERR_RANGE == ty_errnum());
}

// Verify decimal64 multiplication: exact, or an error.
// 1. Multiply at the sum of the scales: 1.5 * 2.25, and with a negative.
// 2. Multiply to the largest value, 17 digits, which fits.
// 3. Multiply to 18 digits: it fails with TY_ERR_RANGE.
// 4. Multiply to 18 places: it fails with TY_ERR_RANGE.
// 5. Check that a failure sets no result.
TEST_CASE("decimal64 mul is exact, and fails past 17 digits or places") {
  ty_dec64_t r = {7, 7};

  // 1. Multiply at the sum of the scales: 1.5 * 2.25, and with a negative.
  ty_dec64_t a = {15, 1};
  ty_dec64_t b = {225, 2};
  CHECK(0 == ty_dec64_mul(&a, &b, &r));
  CHECK(3375 == r.value);
  CHECK(3 == r.scale);
  a = {-15, 1};
  CHECK(0 == ty_dec64_mul(&a, &b, &r));
  CHECK(-3375 == r.value);

  // 2. Multiply to the largest value, 17 digits, which fits.
  a = {99999999999999999, 0};
  b = {1, 0};
  CHECK(0 == ty_dec64_mul(&a, &b, &r));
  CHECK(99999999999999999 == r.value);
  CHECK(0 == r.scale);

  r = {7, 7};
  // 3. Multiply to 18 digits: it fails with TY_ERR_RANGE.
  a = {99999999999999999, 0};
  b = {10, 0};
  CHECK(-1 == ty_dec64_mul(&a, &b, &r));
  CHECK(TY_ERR_RANGE == ty_errnum());

  // 4. Multiply to 18 places: it fails with TY_ERR_RANGE.
  a = {1, 9};
  b = {1, 9};
  CHECK(-1 == ty_dec64_mul(&a, &b, &r));
  CHECK(TY_ERR_RANGE == ty_errnum());

  // 5. Check that a failure sets no result.
  CHECK(7 == r.value);
  CHECK(7 == r.scale);
}

// Verify negate, absolute value and sign of a decimal64.
// 1. Negate a positive, a negative, and zero.
// 2. Take the absolute value of a positive, a negative, and zero.
// 3. Take the sign of each: 1, -1 and 0, at any scale.
TEST_CASE("decimal64 neg, abs and sign keep the scale") {
  ty_dec64_t pos = {125, 2};
  ty_dec64_t neg = {-125, 2};
  ty_dec64_t zero = {0, 5};

  // 1. Negate a positive, a negative, and zero.
  CHECK(-125 == ty_dec64_neg(pos).value);
  CHECK(2 == ty_dec64_neg(pos).scale);
  CHECK(125 == ty_dec64_neg(neg).value);
  CHECK(0 == ty_dec64_neg(zero).value);
  CHECK(5 == ty_dec64_neg(zero).scale);

  // 2. Take the absolute value of a positive, a negative, and zero.
  CHECK(125 == ty_dec64_abs(pos).value);
  CHECK(125 == ty_dec64_abs(neg).value);
  CHECK(2 == ty_dec64_abs(neg).scale);
  CHECK(0 == ty_dec64_abs(zero).value);

  // 3. Take the sign of each: 1, -1 and 0, at any scale.
  CHECK(1 == ty_dec64_sign(pos));
  CHECK(-1 == ty_dec64_sign(neg));
  CHECK(0 == ty_dec64_sign(zero));
}

// Verify decimal64 remainder: the sign of a, at the larger scale.
// 1. Take 7 % 3, and -7 % 3, and 7 % -3.
// 2. Take 7.5 % 2.25, at two scales.
// 3. Take a % 0: it fails with TY_ERR_DIVZERO, and sets no result.
TEST_CASE("decimal64 mod has the sign of a, and fails on zero") {
  ty_dec64_t r = {7, 7};

  // 1. Take 7 % 3, and -7 % 3, and 7 % -3.
  ty_dec64_t a = {7, 0};
  ty_dec64_t b = {3, 0};
  CHECK(0 == ty_dec64_mod(&a, &b, &r));
  CHECK(1 == r.value);
  a = {-7, 0};
  CHECK(0 == ty_dec64_mod(&a, &b, &r));
  CHECK(-1 == r.value);
  a = {7, 0};
  b = {-3, 0};
  CHECK(0 == ty_dec64_mod(&a, &b, &r));
  CHECK(1 == r.value);

  // 2. Take 7.5 % 2.25, at two scales.
  a = {75, 1};
  b = {225, 2};
  CHECK(0 == ty_dec64_mod(&a, &b, &r));
  CHECK(75 == r.value);
  CHECK(2 == r.scale);

  // 3. Take a % 0: it fails with TY_ERR_DIVZERO, and sets no result.
  r = {7, 7};
  b = {0, 3};
  CHECK(-1 == ty_dec64_mod(&a, &b, &r));
  CHECK(TY_ERR_DIVZERO == ty_errnum());
  CHECK(7 == r.value);
}

// Verify decimal64 gcd: at the larger scale, and with zero.
// 1. Take gcd(12, 18), and with a negative.
// 2. Take gcd(1.5, 0.25), at two scales.
// 3. Take gcd(0, 5) and gcd(0, 0).
// 4. Take gcd(0, a large value) at a scale that makes it 34 digits: it
//    fails with TY_ERR_RANGE.
TEST_CASE("decimal64 gcd is at the larger scale") {
  ty_dec64_t r;

  // 1. Take gcd(12, 18), and with a negative.
  ty_dec64_t a = {12, 0};
  ty_dec64_t b = {18, 0};
  CHECK(0 == ty_dec64_gcd(&a, &b, &r));
  CHECK(6 == r.value);
  a = {-12, 0};
  CHECK(0 == ty_dec64_gcd(&a, &b, &r));
  CHECK(6 == r.value);

  // 2. Take gcd(1.5, 0.25), at two scales.
  a = {15, 1};
  b = {25, 2};
  CHECK(0 == ty_dec64_gcd(&a, &b, &r));
  CHECK(25 == r.value);
  CHECK(2 == r.scale);

  // 3. Take gcd(0, 5) and gcd(0, 0).
  a = {0, 0};
  b = {5, 0};
  CHECK(0 == ty_dec64_gcd(&a, &b, &r));
  CHECK(5 == r.value);
  b = {0, 0};
  CHECK(0 == ty_dec64_gcd(&a, &b, &r));
  CHECK(0 == r.value);

  // 4. Take gcd(0, a large value) at a scale that makes it 34 digits.
  a = {0, 17};
  b = {99999999999999999, 0};
  CHECK(-1 == ty_dec64_gcd(&a, &b, &r));
  CHECK(TY_ERR_RANGE == ty_errnum());
}

// Verify decimal64 lcm: at the larger scale, and an error past 17 digits.
// 1. Take lcm(4, 6), and with a negative.
// 2. Take lcm(1.5, 0.25), at two scales.
// 3. Take lcm(0, 5): zero.
// 4. Take lcm of two large coprime values: it fails with TY_ERR_RANGE,
//    and sets no result.
TEST_CASE("decimal64 lcm is at the larger scale, and fails past 17 digits") {
  ty_dec64_t r = {7, 7};

  // 1. Take lcm(4, 6), and with a negative.
  ty_dec64_t a = {4, 0};
  ty_dec64_t b = {6, 0};
  CHECK(0 == ty_dec64_lcm(&a, &b, &r));
  CHECK(12 == r.value);
  a = {-4, 0};
  CHECK(0 == ty_dec64_lcm(&a, &b, &r));
  CHECK(12 == r.value);

  // 2. Take lcm(1.5, 0.25), at two scales.
  a = {15, 1};
  b = {25, 2};
  CHECK(0 == ty_dec64_lcm(&a, &b, &r));
  CHECK(150 == r.value);
  CHECK(2 == r.scale);

  // 3. Take lcm(0, 5): zero.
  a = {0, 0};
  b = {5, 0};
  CHECK(0 == ty_dec64_lcm(&a, &b, &r));
  CHECK(0 == r.value);

  // 4. Take lcm of two large coprime values: it fails with TY_ERR_RANGE,
  //    and sets no result.
  r = {7, 7};
  a = {99999999999999999, 0};
  b = {99999999999999998, 0};
  CHECK(-1 == ty_dec64_lcm(&a, &b, &r));
  CHECK(TY_ERR_RANGE == ty_errnum());
  CHECK(7 == r.value);
}

// Verify decimal64 comparison at one scale and at two.
// 1. Compare values of one scale: less, equal, greater.
// 2. Compare values of two scales that are equal: 1.5 and 1.50.
// 3. Compare values of two scales that differ, in both orders.
// 4. Compare negatives, and the largest values at the farthest scales.
TEST_CASE("decimal64 cmp is exact at any scales") {
  // 1. Compare values of one scale: less, equal, greater.
  CHECK(-1 == ty_dec64_cmp({1, 2}, {2, 2}));
  CHECK(0 == ty_dec64_cmp({2, 2}, {2, 2}));
  CHECK(1 == ty_dec64_cmp({3, 2}, {2, 2}));

  // 2. Compare values of two scales that are equal: 1.5 and 1.50.
  CHECK(0 == ty_dec64_cmp({15, 1}, {150, 2}));

  // 3. Compare values of two scales that differ, in both orders.
  CHECK(-1 == ty_dec64_cmp({15, 1}, {151, 2}));
  CHECK(1 == ty_dec64_cmp({151, 2}, {15, 1}));

  // 4. Compare negatives, and the largest values at the farthest scales.
  CHECK(-1 == ty_dec64_cmp({-151, 2}, {-15, 1}));
  CHECK(1 == ty_dec64_cmp({99999999999999999, 0}, {99999999999999999, 17}));
  CHECK(-1 == ty_dec64_cmp({-99999999999999999, 0}, {0, 17}));
}

namespace {

/* ty_dec64_round of the decimal64 {v, s} to n places. */
int round_of(int64_t v, int s, int n, ty_dec64_t *r) {
  ty_dec64_t a = {v, s};
  return ty_dec64_round(&a, n, r);
}

/* ty_dec64_trunc of the decimal64 {v, s} to n places. */
int trunc_of(int64_t v, int s, int n, ty_dec64_t *r) {
  ty_dec64_t a = {v, s};
  return ty_dec64_trunc(&a, n, r);
}

} // namespace

// Verify decimal64 round and trunc: places, zeros, and tens.
// 1. Round to fewer places, half away from zero, both signs.
// 2. Truncate to fewer places, both signs.
// 3. Round to more places: zeros are added, and past 17 it fails.
// 4. Round to tens and hundreds: the scale is 0; a carry that makes 18
//    digits fails with TY_ERR_RANGE.
// 5. Round to tens past the value: zero.
// 6. Round and truncate to the lowest counts of places, down to INT_MIN, of
//    a value of each scale: zero.
TEST_CASE("decimal64 round and trunc") {
  ty_dec64_t r;

  // 1. Round to fewer places, half away from zero, both signs.
  CHECK(0 == round_of(1235, 3, 2, &r));
  CHECK(124 == r.value);
  CHECK(2 == r.scale);
  CHECK(0 == round_of(-1235, 3, 2, &r));
  CHECK(-124 == r.value);
  CHECK(0 == round_of(1234, 3, 0, &r));
  CHECK(1 == r.value);
  CHECK(0 == r.scale);

  // 2. Truncate to fewer places, both signs.
  CHECK(0 == trunc_of(1239, 3, 2, &r));
  CHECK(123 == r.value);
  CHECK(0 == trunc_of(-1239, 3, 2, &r));
  CHECK(-123 == r.value);

  // 3. Round to more places: zeros are added, and past 17 it fails.
  CHECK(0 == round_of(15, 1, 3, &r));
  CHECK(1500 == r.value);
  CHECK(3 == r.scale);
  CHECK(-1 == round_of(15, 1, 18, &r));
  CHECK(TY_ERR_RANGE == ty_errnum());
  CHECK(-1 == round_of(99999999999999999, 0, 1, &r));
  CHECK(TY_ERR_RANGE == ty_errnum());

  // 4. Round to tens and hundreds: the scale is 0; a carry that makes 18
  //    digits fails with TY_ERR_RANGE.
  CHECK(0 == round_of(14949, 2, -2, &r));
  CHECK(100 == r.value);
  CHECK(0 == r.scale);
  CHECK(0 == round_of(15049, 2, -2, &r));
  CHECK(200 == r.value);
  CHECK(0 == round_of(-149, 0, -2, &r));
  CHECK(-100 == r.value);
  CHECK(0 == trunc_of(-19999, 2, -2, &r));
  CHECK(-100 == r.value);
  CHECK(-1 == round_of(99999999999999999, 0, -1, &r));
  CHECK(TY_ERR_RANGE == ty_errnum());

  // 5. Round to tens past the value: zero.
  CHECK(0 == round_of(12345, 0, -40, &r));
  CHECK(0 == r.value);
  CHECK(0 == r.scale);

  // 6. Round and truncate to the lowest counts of places, down to INT_MIN,
  //    of a value of each scale: zero.
  for (int s = 0; s <= TY_DEC64_MAX_SCALE; s++) {
    for (int n : {INT_MIN, INT_MIN + 1, INT_MIN + 17, INT_MIN + 18, -19, -18}) {
      CHECK(0 == round_of(99999999999999999, s, n, &r));
      CHECK(0 == r.value);
      CHECK(0 == r.scale);
      CHECK(0 == trunc_of(-99999999999999999, s, n, &r));
      CHECK(0 == r.value);
      CHECK(0 == r.scale);
    }
  }
}

// Verify decimal64 ceil and floor.
// 1. Take them of a value with places, positive and negative.
// 2. Take them of a whole number: it is itself.
TEST_CASE("decimal64 ceil and floor give a whole number") {
  // 1. Take them of a value with places, positive and negative.
  CHECK(2 == ty_dec64_ceil({15, 1}).value);
  CHECK(1 == ty_dec64_floor({15, 1}).value);
  CHECK(-1 == ty_dec64_ceil({-15, 1}).value);
  CHECK(-2 == ty_dec64_floor({-15, 1}).value);
  CHECK(0 == ty_dec64_ceil({15, 1}).scale);

  // 2. Take them of a whole number: it is itself.
  CHECK(7 == ty_dec64_ceil({700, 2}).value);
  CHECK(7 == ty_dec64_floor({700, 2}).value);
  CHECK(7 == ty_dec64_ceil({7, 0}).value);
}

namespace {

/* 10^n as a 128-bit integer. */
ty_int128_t ten_to(int n) {
  ty_int128_t r = 1;
  for (; n > 0; n--) {
    r *= 10;
  }
  return r;
}

} // namespace

// Verify decimal128 + - *: exact, or an error past 36 digits.
// 1. Add and subtract at one scale and at two.
// 2. Add up to the largest value, 36 digits, which fits; one more fails.
// 3. Add where aligning the scales passes 128 bits: it fails.
// 4. Multiply, with a negative; to the largest value, which fits.
// 5. Multiply past 36 digits, and past 128 bits, and to 37 places: each
//    fails with TY_ERR_RANGE and sets no result.
TEST_CASE("decimal128 add, sub and mul are exact, or fail past 36 digits") {
  ty_dec128_t r = {7, 7};
  ty_int128_t big = ten_to(36) - 1;

  // 1. Add and subtract at one scale and at two.
  ty_dec128_t a = {125, 2};
  ty_dec128_t b = {250, 2};
  CHECK(0 == ty_dec128_add(&a, &b, &r));
  CHECK(375 == r.value);
  a = {15, 1};
  b = {25, 3};
  CHECK(0 == ty_dec128_sub(&a, &b, &r));
  CHECK(1475 == r.value);
  CHECK(3 == r.scale);

  // 2. Add up to the largest value, 36 digits, which fits; one more fails.
  a = {big - 1, 0};
  b = {1, 0};
  CHECK(0 == ty_dec128_add(&a, &b, &r));
  CHECK(big == r.value);
  r = {7, 7};
  a = {big, 0};
  CHECK(-1 == ty_dec128_add(&a, &b, &r));
  CHECK(TY_ERR_RANGE == ty_errnum());

  // 3. Add where aligning the scales passes 128 bits: it fails.
  a = {big, 0};
  b = {1, 36};
  CHECK(-1 == ty_dec128_add(&a, &b, &r));
  CHECK(TY_ERR_RANGE == ty_errnum());

  // 4. Multiply, with a negative; to the largest value, which fits.
  a = {-15, 1};
  b = {225, 2};
  CHECK(0 == ty_dec128_mul(&a, &b, &r));
  CHECK(-3375 == r.value);
  CHECK(3 == r.scale);
  a = {big, 0};
  b = {1, 0};
  CHECK(0 == ty_dec128_mul(&a, &b, &r));
  CHECK(big == r.value);

  // 5. Multiply past 36 digits, and past 128 bits, and to 37 places.
  r = {7, 7};
  b = {10, 0};
  CHECK(-1 == ty_dec128_mul(&a, &b, &r));
  CHECK(TY_ERR_RANGE == ty_errnum());
  a = {big, 0};
  b = {big, 0};
  CHECK(-1 == ty_dec128_mul(&a, &b, &r));
  CHECK(TY_ERR_RANGE == ty_errnum());
  a = {1, 20};
  b = {1, 17};
  CHECK(-1 == ty_dec128_mul(&a, &b, &r));
  CHECK(TY_ERR_RANGE == ty_errnum());
  CHECK(7 == r.value);
}

namespace {

/* ty_dec128_round of {v, s} to n places. */
int round128_of(ty_int128_t v, int s, int n, ty_dec128_t *r) {
  ty_dec128_t a = {v, s};
  return ty_dec128_round(&a, n, r);
}

/* ty_dec128_trunc of {v, s} to n places. */
int trunc128_of(ty_int128_t v, int s, int n, ty_dec128_t *r) {
  ty_dec128_t a = {v, s};
  return ty_dec128_trunc(&a, n, r);
}

} // namespace

// Verify decimal128 neg, abs, sign, ceil, floor and compare.
// 1. Negate, take the absolute value and the sign of both signs and zero.
// 2. Take ceil and floor of values with places, and of a whole number.
// 3. Compare at one scale, at two, and where aligning passes 128 bits.
TEST_CASE("decimal128 neg, abs, sign, ceil, floor and cmp") {
  ty_int128_t big = ten_to(36) - 1;

  // 1. Negate, take the absolute value and the sign of both signs and zero.
  CHECK(-125 == ty_dec128_neg({125, 2}).value);
  CHECK(125 == ty_dec128_abs({-125, 2}).value);
  CHECK(2 == ty_dec128_abs({-125, 2}).scale);
  CHECK(1 == ty_dec128_sign({125, 2}));
  CHECK(-1 == ty_dec128_sign({-125, 2}));
  CHECK(0 == ty_dec128_sign({0, 9}));

  // 2. Take ceil and floor of values with places, and of a whole number.
  CHECK(2 == ty_dec128_ceil({15, 1}).value);
  CHECK(1 == ty_dec128_floor({15, 1}).value);
  CHECK(-1 == ty_dec128_ceil({-15, 1}).value);
  CHECK(-2 == ty_dec128_floor({-15, 1}).value);
  CHECK(7 == ty_dec128_ceil({7, 0}).value);
  CHECK(big == ty_dec128_floor({big * 10 + 5 > 0 ? big : 0, 0}).value);

  // 3. Compare at one scale, at two, and where aligning passes 128 bits.
  CHECK(-1 == ty_dec128_cmp({1, 2}, {2, 2}));
  CHECK(0 == ty_dec128_cmp({15, 1}, {150, 2}));
  CHECK(1 == ty_dec128_cmp({151, 2}, {15, 1}));
  CHECK(1 == ty_dec128_cmp({big, 0}, {big, 36}));
  CHECK(-1 == ty_dec128_cmp({-big, 0}, {big, 36}));
  CHECK(-1 == ty_dec128_cmp({big, 36}, {big, 0}));
}

// Verify decimal128 round and trunc.
// 1. Round to fewer places, half away from zero, both signs; and cut.
// 2. Round to more places: zeros are added; to 40 places is 36.
// 3. Round to tens and hundreds: the scale is 0.
// 4. Round past 36 digits: it fails with TY_ERR_RANGE.
// 5. Round and truncate to the lowest counts of places, down to INT_MIN, of
//    a value of each scale: zero.
TEST_CASE("decimal128 round and trunc") {
  ty_dec128_t r;
  ty_int128_t big = ten_to(36) - 1;

  // 1. Round to fewer places, half away from zero, both signs; and cut.
  CHECK(0 == round128_of(1235, 3, 2, &r));
  CHECK(124 == r.value);
  CHECK(0 == round128_of(-1235, 3, 2, &r));
  CHECK(-124 == r.value);
  CHECK(0 == trunc128_of(-1239, 3, 2, &r));
  CHECK(-123 == r.value);

  // 2. Round to more places: zeros are added; to 40 places is 36.
  CHECK(0 == round128_of(15, 1, 3, &r));
  CHECK(1500 == r.value);
  CHECK(3 == r.scale);
  CHECK(0 == round128_of(5, 1, 40, &r));
  CHECK(36 == r.scale);
  CHECK(-1 == round128_of(15, 1, 40, &r));
  CHECK(TY_ERR_RANGE == ty_errnum());

  // 3. Round to tens and hundreds: the scale is 0.
  CHECK(0 == round128_of(14949, 2, -2, &r));
  CHECK(100 == r.value);
  CHECK(0 == r.scale);
  CHECK(0 == round128_of(12345, 0, -40, &r));
  CHECK(0 == r.value);

  // 4. Round past 36 digits: it fails with TY_ERR_RANGE.
  CHECK(-1 == round128_of(big, 0, 1, &r));
  CHECK(TY_ERR_RANGE == ty_errnum());
  CHECK(-1 == round128_of(big, 0, -1, &r));
  CHECK(TY_ERR_RANGE == ty_errnum());

  // 5. Round and truncate to the lowest counts of places, down to INT_MIN,
  //    of a value of each scale: zero.
  for (int s = 0; s <= TY_DEC128_MAX_SCALE; s++) {
    for (int n : {INT_MIN, INT_MIN + 1, INT_MIN + 36, INT_MIN + 37, -38, -37}) {
      CHECK(0 == round128_of(big, s, n, &r));
      CHECK(0 == r.value);
      CHECK(0 == r.scale);
      CHECK(0 == trunc128_of(-big, s, n, &r));
      CHECK(0 == r.value);
      CHECK(0 == r.scale);
    }
  }
}

// Verify decimal128 %, gcd and lcm, including scales that do not align in
// 128 bits.
// 1. Take % at one scale, with signs, and at two scales; by zero it fails.
// 2. Take % where aligning a passes 128 bits, and where aligning b does.
// 3. Take gcd, at two scales, with zero, and where aligning passes 128 bits.
// 4. Take lcm, with zero, at two scales; of large coprimes it fails.
TEST_CASE("decimal128 mod, gcd and lcm") {
  ty_dec128_t r;
  ty_int128_t big = ten_to(36) - 1;

  // 1. Take % at one scale, with signs, and at two scales; by zero it fails.
  ty_dec128_t a = {7, 0};
  ty_dec128_t b = {3, 0};
  CHECK(0 == ty_dec128_mod(&a, &b, &r));
  CHECK(1 == r.value);
  a = {-7, 0};
  CHECK(0 == ty_dec128_mod(&a, &b, &r));
  CHECK(-1 == r.value);
  a = {75, 1};
  b = {225, 2};
  CHECK(0 == ty_dec128_mod(&a, &b, &r));
  CHECK(75 == r.value);
  CHECK(2 == r.scale);
  b = {0, 5};
  CHECK(-1 == ty_dec128_mod(&a, &b, &r));
  CHECK(TY_ERR_DIVZERO == ty_errnum());

  // 2. Take % where aligning a passes 128 bits, and where aligning b does.
  //    big * 10^36 mod 7 is 0, as 10^6 - 1 divides big. Then 5 mod 11
  //    with a at 10^36 scale: (5 * 10^0 at scale 36 vs 11 at scale 0).
  a = {big, 0};
  b = {7, 36};
  CHECK(0 == ty_dec128_mod(&a, &b, &r));
  CHECK(0 == r.value);
  CHECK(36 == r.scale);
  a = {big - 1, 0};
  b = {11, 36};
  CHECK(0 == ty_dec128_mod(&a, &b, &r));
  CHECK(((big - 1) % 11 * ten_to(36) % 11) == r.value);
  a = {5, 36};
  b = {big, 0};
  CHECK(0 == ty_dec128_mod(&a, &b, &r));
  CHECK(5 == r.value);

  // 3. Take gcd, at two scales, with zero, and where aligning passes 128
  //    bits: big * 10^36 is a multiple of 14, so the gcd with 14 is 14.
  a = {15, 1};
  b = {25, 2};
  CHECK(0 == ty_dec128_gcd(&a, &b, &r));
  CHECK(25 == r.value);
  a = {0, 0};
  b = {5, 0};
  CHECK(0 == ty_dec128_gcd(&a, &b, &r));
  CHECK(5 == r.value);
  a = {big, 0};
  b = {14, 36};
  CHECK(0 == ty_dec128_gcd(&a, &b, &r));
  CHECK(14 == r.value);
  a = {0, 36};
  b = {big, 0};
  CHECK(-1 == ty_dec128_gcd(&a, &b, &r));
  CHECK(TY_ERR_RANGE == ty_errnum());

  // 4. Take lcm, with zero, at two scales; of large coprimes it fails.
  a = {4, 0};
  b = {6, 0};
  CHECK(0 == ty_dec128_lcm(&a, &b, &r));
  CHECK(12 == r.value);
  a = {15, 1};
  b = {25, 2};
  CHECK(0 == ty_dec128_lcm(&a, &b, &r));
  CHECK(150 == r.value);
  a = {0, 3};
  CHECK(0 == ty_dec128_lcm(&a, &b, &r));
  CHECK(0 == r.value);
  r = {7, 7};
  a = {big, 0};
  b = {big - 1, 0};
  CHECK(-1 == ty_dec128_lcm(&a, &b, &r));
  CHECK(TY_ERR_RANGE == ty_errnum());
  CHECK(7 == r.value);
}

namespace {

/* ty_dec128_div of {v1, s1} by {v2, s2} at rscale places. */
int div128_of(ty_int128_t v1, int s1, ty_int128_t v2, int s2, int rscale,
              bool cut, ty_dec128_t *r) {
  ty_dec128_t a = {v1, s1};
  ty_dec128_t b = {v2, s2};
  return ty_dec128_div(&a, &b, rscale, cut, r);
}

/* ty_dec64_div of {v1, s1} by {v2, s2} at rscale places. */
int div64_of(int64_t v1, int s1, int64_t v2, int s2, int rscale,
             ty_dec64_t *r) {
  ty_dec64_t a = {v1, s1};
  ty_dec64_t b = {v2, s2};
  return ty_dec64_div(&a, &b, rscale, r);
}

} // namespace

// Verify decimal128 division in 128 bits: places, signs, rounding, cut.
// 1. Divide 1 by 3 at 20 places, and 2 by 3: rounded half away from zero.
// 2. Divide with signs, and with scales that differ.
// 3. Cut at rscale places: 2 / 3 at 3 places gives 0.666.
// 4. Divide where the dividend is at a larger scale than the divisor needs
//    at rscale: e is below 0.
// 5. Divide by zero: TY_ERR_DIVZERO. Divide to more than 36 digits:
//    TY_ERR_RANGE.
TEST_CASE("decimal128 div is made a place at a time, in 128 bits") {
  ty_dec128_t r;
  ty_int128_t big = ten_to(36) - 1;

  // 1. Divide 1 by 3 at 20 places, and 2 by 3: rounded half away from zero.
  CHECK(0 == div128_of(1, 0, 3, 0, 20, false, &r));
  CHECK(ten_to(20) / 3 == r.value);
  CHECK(20 == r.scale);
  CHECK(0 == div128_of(2, 0, 3, 0, 3, false, &r));
  CHECK(667 == r.value);

  // 2. Divide with signs, and with scales that differ: -7.5 / 2.5 = -3.
  CHECK(0 == div128_of(-75, 1, 25, 1, 2, false, &r));
  CHECK(-300 == r.value);
  CHECK(0 == div128_of(75, 1, -25, 1, 0, false, &r));
  CHECK(-3 == r.value);
  CHECK(0 == div128_of(1500, 3, 4, 0, 2, false, &r));
  CHECK(38 == r.value);

  // 3. Cut at rscale places: 2 / 3 at 3 places gives 0.666.
  CHECK(0 == div128_of(2, 0, 3, 0, 3, true, &r));
  CHECK(666 == r.value);
  CHECK(0 == div128_of(-2, 0, 3, 0, 3, true, &r));
  CHECK(-666 == r.value);

  // 4. Divide where rscale is below the scales: e is below 0.
  CHECK(0 == div128_of(12345, 4, 5, 0, 1, false, &r));
  CHECK(2 == r.value);
  CHECK(0 == div128_of(1, 36, big, 0, 0, false, &r));
  CHECK(0 == r.value);

  // 5. Divide by zero: TY_ERR_DIVZERO. Divide to more than 36 digits:
  //    TY_ERR_RANGE.
  CHECK(-1 == div128_of(1, 0, 0, 0, 2, false, &r));
  CHECK(TY_ERR_DIVZERO == ty_errnum());
  CHECK(-1 == div128_of(big, 0, 1, 0, 2, false, &r));
  CHECK(TY_ERR_RANGE == ty_errnum());
}

// Verify decimal64 division, fitted to a decimal64.
// 1. Divide 1 by 3 at 20 places: it is 17 places.
// 2. Divide 70.0 by 70 at 16 places: 1.0000000000000000.
// 3. Divide 100 by 3 at 20 places: 33.333333333333333, 17 digits.
// 4. Divide by zero: TY_ERR_DIVZERO.
TEST_CASE("decimal64 div is rounded to fit 17 digits") {
  ty_dec64_t r;

  // 1. Divide 1 by 3 at 20 places: it is 17 places.
  CHECK(0 == div64_of(1, 0, 3, 0, 20, &r));
  CHECK(33333333333333333 == r.value);
  CHECK(17 == r.scale);

  // 2. Divide 70.0 by 70 at 16 places: 1.0000000000000000.
  CHECK(0 == div64_of(700, 1, 70, 0, 16, &r));
  CHECK(10000000000000000 == r.value);
  CHECK(16 == r.scale);

  // 3. Divide 100 by 3 at 20 places: 33.333333333333333, 17 digits.
  CHECK(0 == div64_of(100, 0, 3, 0, 20, &r));
  CHECK(33333333333333333 == r.value);
  CHECK(15 == r.scale);

  // 4. Divide by zero: TY_ERR_DIVZERO.
  CHECK(-1 == div64_of(1, 0, 0, 0, 2, &r));
  CHECK(TY_ERR_DIVZERO == ty_errnum());
}

// Verify the square root: a digit at a time, rounded half up.
// 1. Take sqrt(2) at 20 places, and of a square, and of zero.
// 2. Take it of a value at a scale, and round: sqrt(3) at 1 place.
// 3. Take it of a negative: TY_ERR_INVALID.
// 4. Take it at fewer places than half the value's: the digits past them
//    count in the rounding, at a half, above it and below it, and of 36
//    digits.
TEST_CASE("decimal128 sqrt is made a digit at a time") {
  ty_dec128_t r;

  // 1. Take sqrt(2) at 20 places, and of a square, and of zero.
  ty_dec128_t a = {2, 0};
  CHECK(0 == ty_dec128_sqrt(&a, 20, &r));
  CHECK((ty_int128_t)141421356237309 * ten_to(6) + 504880 == r.value);
  CHECK(20 == r.scale);
  a = {1600, 2};
  CHECK(0 == ty_dec128_sqrt(&a, 2, &r));
  CHECK(400 == r.value);
  a = {0, 3};
  CHECK(0 == ty_dec128_sqrt(&a, 3, &r));
  CHECK(0 == r.value);

  // 2. Take it of a value at a scale, and round: sqrt(3) at 1 place.
  a = {1, 4};
  CHECK(0 == ty_dec128_sqrt(&a, 4, &r));
  CHECK(100 == r.value);
  a = {3, 0};
  CHECK(0 == ty_dec128_sqrt(&a, 1, &r));
  CHECK(17 == r.value);
  a = {2, 0};
  CHECK(0 == ty_dec128_sqrt(&a, 1, &r));
  CHECK(14 == r.value);

  // 3. Take it of a negative: TY_ERR_INVALID.
  a = {-4, 0};
  CHECK(-1 == ty_dec128_sqrt(&a, 2, &r));
  CHECK(TY_ERR_INVALID == ty_errnum());

  // 4. Take it at fewer places than half the value's: the digits past them
  //    count in the rounding, at a half, above it and below it, and of 36
  //    digits.
  auto root = [&r](ty_int128_t v, int s, int rscale) {
    ty_dec128_t x = {v, s};
    REQUIRE(0 == ty_dec128_sqrt(&x, rscale, &r));
    REQUIRE(rscale == r.scale);
    return (int64_t)r.value;
  };
  CHECK(0 == root(24, 2, 0)); /* 0.4899 */
  CHECK(1 == root(25, 2, 0)); /* 0.5 */
  CHECK(1 == root(30, 2, 0)); /* 0.5477 */
  CHECK(1 == root(224, 2, 0));
  CHECK(2 == root(225, 2, 0)); /* 1.5 */
  CHECK(2 == root(249, 2, 0));
  CHECK(0 == root(249, 3, 0));  /* three places: 0.49899 */
  CHECK(1 == root(250, 3, 0));  /* 0.5 */
  CHECK(0 == root(2025, 4, 0)); /* 0.45 */
  CHECK(1 == root(25, 4, 1));   /* 0.05 */
  CHECK(1 == root(ten_to(36) - 1, 36, 0));
  CHECK(0 == root(ten_to(34) * 25 - 1, 36, 0));
  CHECK(1 == root(ten_to(34) * 25, 36, 0));
}

// Verify a double as a decimal is rounded half away from zero, at each kind
// of tie.
// 1. Ties at scale 0, whose whole part is even and odd, of both signs.
// 2. A tie at 2 places, and a tie of 53 bits, whose next double is far.
// 3. Numbers one double below a tie and one above, and whole numbers.
// 4. Every tie m / 2^(s+1) of small m at scale s, by integers.
TEST_CASE("a decimal128 of a double rounds half away from zero") {
  ty_dec128_t r;
  auto of = [&r](double x, int s) {
    REQUIRE(0 == ty_dec128_from_double(x, s, &r));
    REQUIRE(s == r.scale);
    return (int64_t)r.value;
  };

  // 1. Ties at scale 0, whose whole part is even and odd, of both signs.
  CHECK(1 == of(0.5, 0));
  CHECK(2 == of(1.5, 0));
  CHECK(3 == of(2.5, 0));
  CHECK(-3 == of(-2.5, 0));
  CHECK(-4 == of(-3.5, 0));

  // 2. A tie at 2 places, and a tie of 53 bits, whose next double is far.
  CHECK(13 == of(0.125, 2));
  CHECK(38 == of(0.375, 2));
  CHECK(11258999068426243 == of(1125899906842624.25, 1));
  CHECK(11258999068426248 == of(1125899906842624.75, 1));

  // 3. Numbers one double below a tie and one above, and whole numbers.
  CHECK(2 == of(nextafter(2.5, 0), 0));
  CHECK(3 == of(nextafter(2.5, 3), 0));
  CHECK(12 == of(nextafter(0.125, 0), 2));
  CHECK(1 == of(1.0, 0));
  CHECK(0 == of(0.0, 0));
  CHECK(4503599627370497 == of(4503599627370497.0, 0));

  // 4. Every tie m / 2^(s+1) of small m at scale s, by integers.
  int64_t p5 = 1;
  for (int s = 0; s <= 12; s++, p5 *= 5) {
    for (int64_t m = 1; m < 2000; m += 2) {
      CHECK((m * p5 + 1) / 2 == of(ldexp((double)m, -(s + 1)), s));
      CHECK(-(m * p5 + 1) / 2 == of(-ldexp((double)m, -(s + 1)), s));
    }
  }
}

// Verify exp, ln, log and the powers, in double.
// 1. Take exp(1) and ln(10) at 12 places; log(2, 8) at 10 places.
// 2. Take a zero base's log, and a base of 1: TY_ERR_INVALID, DIVZERO.
// 3. Take integer powers: 2^100 exactly, 2^-3, and a power too large for
//    128 bits, which is rounded from double.
// 4. Take a real power: 2^0.5; and (-2)^3 by its sign.
// 5. Take an overflow and an underflow of a double: errors.
TEST_CASE("decimal128 exp, ln, log and powers") {
  ty_dec128_t r;

  // 1. Take exp(1) and ln(10) at 12 places; log(2, 8) at 10 places.
  ty_dec128_t a = {1, 0};
  CHECK(0 == ty_dec128_exp(&a, 12, &r));
  CHECK(2718281828459 == r.value);
  a = {10, 0};
  CHECK(0 == ty_dec128_ln(&a, 12, &r));
  CHECK(2302585092994 == r.value);
  ty_dec128_t base = {2, 0};
  ty_dec128_t eight = {8, 0};
  CHECK(0 == ty_dec128_log(&base, &eight, 10, &r));
  CHECK(30000000000 == r.value);

  // 2. Take a zero base's log, and a base of 1: TY_ERR_INVALID, DIVZERO.
  ty_dec128_t zero = {0, 0};
  CHECK(-1 == ty_dec128_ln(&zero, 5, &r));
  CHECK(TY_ERR_INVALID == ty_errnum());
  ty_dec128_t one = {1, 0};
  CHECK(-1 == ty_dec128_log(&one, &eight, 5, &r));
  CHECK(TY_ERR_DIVZERO == ty_errnum());

  // 3. Take integer powers: 2^100 exactly, 2^-3, and a power too large for
  //    128 bits, which is rounded from double.
  ty_int128_t p2 = 1;
  for (int i = 0; i < 100; i++) {
    p2 *= 2;
  }
  CHECK(0 == ty_dec128_pow_int(&base, 100, 0, &r));
  CHECK(p2 == r.value);
  CHECK(0 == ty_dec128_pow_int(&base, -3, 4, &r));
  CHECK(1250 == r.value);
  CHECK(4 == r.scale);
  ty_dec128_t b = {3789, 3};
  CHECK(0 == ty_dec128_pow_int(&b, 21, 5, &r));
  CHECK(1409343026052 == r.value / ten_to(5));

  // 4. Take a real power: 2^0.5; and (-2)^3 by its sign.
  ty_dec128_t half = {5, 1};
  CHECK(0 == ty_dec128_pow_real(&base, &half, 10, &r));
  CHECK(14142135624 == r.value);
  ty_dec128_t neg2 = {-2, 0};
  ty_dec128_t three = {3, 0};
  CHECK(0 == ty_dec128_pow_real(&neg2, &three, 3, &r));
  CHECK(-8000 == r.value);

  // 5. Take an overflow and an underflow of a double: errors.
  ty_dec128_t big = {1000, 0};
  ty_dec128_t small = {-1000, 0};
  CHECK(-1 == ty_dec128_exp(&big, 5, &r));
  CHECK(TY_ERR_RANGE == ty_errnum());
  CHECK(-1 == ty_dec128_exp(&small, 5, &r));
  CHECK(TY_ERR_UNDERFLOW == ty_errnum());
  ty_dec128_t ten = {10, 0};
  CHECK(-1 == ty_dec128_pow_int(&ten, 400, 0, &r));
  CHECK(TY_ERR_RANGE == ty_errnum());
}

// Verify factorial and width_bucket.
// 1. Take 0!, 5! and 32!; 33! and a negative fail.
// 2. Take width_bucket: 5.35 in 0.024 to 10.06 in 5 is 3; before is 0;
//    at the top is count + 1; a reversed range counts down.
// 3. Take bad bounds and a bad count: TY_ERR_INVALID.
TEST_CASE("decimal128 factorial and width_bucket") {
  ty_dec128_t r;
  int64_t bucket = -1;

  // 1. Take 0!, 5! and 32!; 33! and a negative fail.
  CHECK(0 == ty_dec128_factorial(0, &r));
  CHECK(1 == r.value);
  CHECK(0 == ty_dec128_factorial(5, &r));
  CHECK(120 == r.value);
  CHECK(0 == ty_dec128_factorial(32, &r));
  CHECK(-1 == ty_dec128_factorial(33, &r));
  CHECK(TY_ERR_RANGE == ty_errnum());
  CHECK(-1 == ty_dec128_factorial(-1, &r));
  CHECK(TY_ERR_INVALID == ty_errnum());

  // 2. Take width_bucket: 5.35 in 0.024 to 10.06 in 5 is 3; before is 0;
  //    at the top is count + 1; a reversed range counts down.
  ty_dec128_t op = {535, 2};
  ty_dec128_t lo = {24, 3};
  ty_dec128_t hi = {1006, 2};
  CHECK(0 == ty_dec128_width_bucket(&op, &lo, &hi, 5, &bucket));
  CHECK(3 == bucket);
  op = {0, 0};
  CHECK(0 == ty_dec128_width_bucket(&op, &lo, &hi, 5, &bucket));
  CHECK(0 == bucket);
  op = {1006, 2};
  CHECK(0 == ty_dec128_width_bucket(&op, &lo, &hi, 5, &bucket));
  CHECK(6 == bucket);
  op = {535, 2};
  CHECK(0 == ty_dec128_width_bucket(&op, &hi, &lo, 5, &bucket));
  CHECK(3 == bucket);
  ty_dec128_t one = {1, 0};
  ty_dec128_t ten = {10, 0};
  op = {55, 1};
  CHECK(0 == ty_dec128_width_bucket(&op, &one, &ten, 9, &bucket));
  CHECK(5 == bucket);

  // 3. Take bad bounds and a bad count: TY_ERR_INVALID.
  CHECK(-1 == ty_dec128_width_bucket(&op, &one, &one, 5, &bucket));
  CHECK(TY_ERR_INVALID == ty_errnum());
  CHECK(-1 == ty_dec128_width_bucket(&op, &one, &ten, 0, &bucket));
  CHECK(TY_ERR_INVALID == ty_errnum());
}

// Verify the fit of a decimal128 to a decimal64.
// 1. A value that fits is itself.
// 2. Places past 17 are rounded; digits past 17 push out places.
// 3. An integer part of more than 17 digits fails with TY_ERR_RANGE.
TEST_CASE("decimal64 from decimal128 rounds places to fit 17 digits") {
  ty_dec64_t r;
  ty_int128_t big = ten_to(36) - 1;

  // 1. A value that fits is itself.
  ty_dec128_t a = {12345, 3};
  CHECK(0 == ty_dec64_from_dec128(&a, &r));
  CHECK(12345 == r.value);
  CHECK(3 == r.scale);

  // 2. Places past 17 are rounded; digits past 17 push out places.
  a = {ten_to(19) + 5 * ten_to(2), 20};
  CHECK(0 == ty_dec64_from_dec128(&a, &r));
  CHECK(0 <= r.scale);
  CHECK(r.scale <= 17);
  a = {big, 20};
  CHECK(0 == ty_dec64_from_dec128(&a, &r));
  CHECK(10000000000000000 == r.value);

  // 3. An integer part of more than 17 digits fails with TY_ERR_RANGE.
  a = {ten_to(18), 0};
  CHECK(-1 == ty_dec64_from_dec128(&a, &r));
  CHECK(TY_ERR_RANGE == ty_errnum());
}

// Verify the hash: equal values hash alike, a decimal64 as a decimal128,
// and the seed and the value change it.
// 1. Hash 1.5, 1.50 and 1.500000: one hash. A zero at any scale: one.
// 2. Hash a decimal64 and the decimal128 of its value: one hash.
// 3. Hash 1.5 and 1.6, and 1.5 and 15, and -1.5: all different.
// 4. Hash with seeds 0 and 1: different; and seed 0 twice: the same.
// 5. Hash 100,000 values: no two the same.
TEST_CASE("the hash is the same for equal values, at any scale") {
  // 1. Hash 1.5, 1.50 and 1.500000: one hash. A zero at any scale: one.
  ty_dec128_t a = {15, 1}, b = {150, 2}, c = {1500000, 6};
  CHECK(ty_dec128_hash(&a, 0) == ty_dec128_hash(&b, 0));
  CHECK(ty_dec128_hash(&a, 0) == ty_dec128_hash(&c, 0));
  ty_dec128_t z0 = {0, 0}, z5 = {0, 5};
  CHECK(ty_dec128_hash(&z0, 0) == ty_dec128_hash(&z5, 0));

  // 2. Hash a decimal64 and the decimal128 of its value: one hash.
  ty_dec64_t d = {150, 2};
  CHECK(ty_dec64_hash(&d, 0) == ty_dec128_hash(&a, 0));
  CHECK(ty_dec64_hash(&d, 7) == ty_dec128_hash(&a, 7));

  // 3. Hash 1.5 and 1.6, and 1.5 and 15, and -1.5: all different.
  ty_dec128_t e = {16, 1}, f = {15, 0}, g = {-15, 1};
  CHECK(ty_dec128_hash(&a, 0) != ty_dec128_hash(&e, 0));
  CHECK(ty_dec128_hash(&a, 0) != ty_dec128_hash(&f, 0));
  CHECK(ty_dec128_hash(&a, 0) != ty_dec128_hash(&g, 0));

  // 4. Hash with seeds 0 and 1: different; and seed 0 twice: the same.
  CHECK(ty_dec128_hash(&a, 0) != ty_dec128_hash(&a, 1));
  CHECK((uint32_t)ty_dec128_hash(&a, 0) != (uint32_t)ty_dec128_hash(&a, 1));
  CHECK(ty_dec128_hash(&a, 0) == ty_dec128_hash(&a, 0));

  // 5. Hash 100,000 values: no two the same.
  std::vector<uint64_t> seen;
  for (int i = 0; i < 100000; i++) {
    ty_dec128_t x = {i * 7 + 1, i % 5};
    seen.push_back(ty_dec128_hash(&x, 0));
  }
  std::sort(seen.begin(), seen.end());
  CHECK(std::adjacent_find(seen.begin(), seen.end()) == seen.end());
}

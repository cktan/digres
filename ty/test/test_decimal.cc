/*
 * Copyright (c) 2026 CK Tan
 * SPDX-License-Identifier: MIT
 */

/*
 * Tests of the decimal operations.
 */
#include "doctest.h"
#include "ty_decimal.h"

// Verify that decimal64 addition is exact, and takes the larger scale.
// 1. Add two values of one scale: 1.25 + 2.50.
// 2. Add two of different scales, in both orders: 1.5 + 0.025.
// 3. Add a negative: 1.5 + -2.
// 4. Add up to the largest value, 17 digits, which fits.
TEST_CASE("decimal64 add is exact, at the larger scale") {
  ty_decimal64_t r;

  // 1. Add two values of one scale: 1.25 + 2.50.
  ty_decimal64_t a = {125, 2};
  ty_decimal64_t b = {250, 2};
  CHECK(0 == ty_decimal64_add(&a, &b, &r));
  CHECK(375 == r.value);
  CHECK(2 == r.scale);

  // 2. Add two of different scales, in both orders: 1.5 + 0.025.
  a = {15, 1};
  b = {25, 3};
  CHECK(0 == ty_decimal64_add(&a, &b, &r));
  CHECK(1525 == r.value);
  CHECK(3 == r.scale);
  CHECK(0 == ty_decimal64_add(&b, &a, &r));
  CHECK(1525 == r.value);
  CHECK(3 == r.scale);

  // 3. Add a negative: 1.5 + -2.
  a = {15, 1};
  b = {-2, 0};
  CHECK(0 == ty_decimal64_add(&a, &b, &r));
  CHECK(-5 == r.value);
  CHECK(1 == r.scale);

  // 4. Add up to the largest value, 17 digits, which fits.
  a = {99999999999999998, 0};
  b = {1, 0};
  CHECK(0 == ty_decimal64_add(&a, &b, &r));
  CHECK(99999999999999999 == r.value);
  a = {-99999999999999998, 0};
  b = {-1, 0};
  CHECK(0 == ty_decimal64_add(&a, &b, &r));
  CHECK(-99999999999999999 == r.value);
}

// Verify that a sum of more than 17 digits fails, and sets no result.
// 1. Add 1 to the largest value: it fails with TY_ERR_RANGE.
// 2. Add the same on the negative side.
// 3. Add values whose scales do not align in 64 bits.
// 4. Check that the result was not set by any of them.
TEST_CASE("decimal64 add fails past 17 digits, and sets no result") {
  ty_decimal64_t r = {7, 7};

  // 1. Add 1 to the largest value: it fails with TY_ERR_RANGE.
  ty_decimal64_t a = {99999999999999999, 0};
  ty_decimal64_t b = {1, 0};
  CHECK(-1 == ty_decimal64_add(&a, &b, &r));
  CHECK(TY_ERR_RANGE == ty_errnum());

  // 2. Add the same on the negative side.
  a = {-99999999999999999, 0};
  b = {-1, 0};
  CHECK(-1 == ty_decimal64_add(&a, &b, &r));
  CHECK(TY_ERR_RANGE == ty_errnum());

  // 3. Add values whose scales do not align in 64 bits.
  a = {99999999999999999, 0};
  b = {1, 5};
  CHECK(-1 == ty_decimal64_add(&a, &b, &r));
  CHECK(TY_ERR_RANGE == ty_errnum());
  a = {1, 0};
  b = {1, 31};
  CHECK(-1 == ty_decimal64_add(&a, &b, &r));
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
  ty_decimal64_t r;

  // 1. Subtract at one scale, and at two: 2.50 - 1.25, and 1.5 - 0.025.
  ty_decimal64_t a = {250, 2};
  ty_decimal64_t b = {125, 2};
  CHECK(0 == ty_decimal64_sub(&a, &b, &r));
  CHECK(125 == r.value);
  CHECK(2 == r.scale);
  a = {15, 1};
  b = {25, 3};
  CHECK(0 == ty_decimal64_sub(&a, &b, &r));
  CHECK(1475 == r.value);
  CHECK(3 == r.scale);

  // 2. Subtract to a negative: 1 - 3.
  a = {1, 0};
  b = {3, 0};
  CHECK(0 == ty_decimal64_sub(&a, &b, &r));
  CHECK(-2 == r.value);

  // 3. Subtract past 17 digits: it fails with TY_ERR_RANGE.
  a = {-99999999999999999, 0};
  b = {1, 0};
  CHECK(-1 == ty_decimal64_sub(&a, &b, &r));
  CHECK(TY_ERR_RANGE == ty_errnum());
}

// Verify decimal64 multiplication: exact, or an error.
// 1. Multiply at the sum of the scales: 1.5 * 2.25, and with a negative.
// 2. Multiply to the largest value, 17 digits, which fits.
// 3. Multiply to 18 digits: it fails with TY_ERR_RANGE.
// 4. Multiply to 18 places: it fails with TY_ERR_RANGE.
// 5. Check that a failure sets no result.
TEST_CASE("decimal64 mul is exact, and fails past 17 digits or places") {
  ty_decimal64_t r = {7, 7};

  // 1. Multiply at the sum of the scales: 1.5 * 2.25, and with a negative.
  ty_decimal64_t a = {15, 1};
  ty_decimal64_t b = {225, 2};
  CHECK(0 == ty_decimal64_mul(&a, &b, &r));
  CHECK(3375 == r.value);
  CHECK(3 == r.scale);
  a = {-15, 1};
  CHECK(0 == ty_decimal64_mul(&a, &b, &r));
  CHECK(-3375 == r.value);

  // 2. Multiply to the largest value, 17 digits, which fits.
  a = {99999999999999999, 0};
  b = {1, 0};
  CHECK(0 == ty_decimal64_mul(&a, &b, &r));
  CHECK(99999999999999999 == r.value);
  CHECK(0 == r.scale);

  r = {7, 7};
  // 3. Multiply to 18 digits: it fails with TY_ERR_RANGE.
  a = {99999999999999999, 0};
  b = {10, 0};
  CHECK(-1 == ty_decimal64_mul(&a, &b, &r));
  CHECK(TY_ERR_RANGE == ty_errnum());

  // 4. Multiply to 18 places: it fails with TY_ERR_RANGE.
  a = {1, 9};
  b = {1, 9};
  CHECK(-1 == ty_decimal64_mul(&a, &b, &r));
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
  ty_decimal64_t pos = {125, 2};
  ty_decimal64_t neg = {-125, 2};
  ty_decimal64_t zero = {0, 5};

  // 1. Negate a positive, a negative, and zero.
  CHECK(-125 == ty_decimal64_neg(pos).value);
  CHECK(2 == ty_decimal64_neg(pos).scale);
  CHECK(125 == ty_decimal64_neg(neg).value);
  CHECK(0 == ty_decimal64_neg(zero).value);
  CHECK(5 == ty_decimal64_neg(zero).scale);

  // 2. Take the absolute value of a positive, a negative, and zero.
  CHECK(125 == ty_decimal64_abs(pos).value);
  CHECK(125 == ty_decimal64_abs(neg).value);
  CHECK(2 == ty_decimal64_abs(neg).scale);
  CHECK(0 == ty_decimal64_abs(zero).value);

  // 3. Take the sign of each: 1, -1 and 0, at any scale.
  CHECK(1 == ty_decimal64_sign(pos));
  CHECK(-1 == ty_decimal64_sign(neg));
  CHECK(0 == ty_decimal64_sign(zero));
}

// Verify decimal64 remainder: the sign of a, at the larger scale.
// 1. Take 7 % 3, and -7 % 3, and 7 % -3.
// 2. Take 7.5 % 2.25, at two scales.
// 3. Take a % 0: it fails with TY_ERR_DIVZERO, and sets no result.
TEST_CASE("decimal64 mod has the sign of a, and fails on zero") {
  ty_decimal64_t r = {7, 7};

  // 1. Take 7 % 3, and -7 % 3, and 7 % -3.
  ty_decimal64_t a = {7, 0};
  ty_decimal64_t b = {3, 0};
  CHECK(0 == ty_decimal64_mod(&a, &b, &r));
  CHECK(1 == r.value);
  a = {-7, 0};
  CHECK(0 == ty_decimal64_mod(&a, &b, &r));
  CHECK(-1 == r.value);
  a = {7, 0};
  b = {-3, 0};
  CHECK(0 == ty_decimal64_mod(&a, &b, &r));
  CHECK(1 == r.value);

  // 2. Take 7.5 % 2.25, at two scales.
  a = {75, 1};
  b = {225, 2};
  CHECK(0 == ty_decimal64_mod(&a, &b, &r));
  CHECK(75 == r.value);
  CHECK(2 == r.scale);

  // 3. Take a % 0: it fails with TY_ERR_DIVZERO, and sets no result.
  r = {7, 7};
  b = {0, 3};
  CHECK(-1 == ty_decimal64_mod(&a, &b, &r));
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
  ty_decimal64_t r;

  // 1. Take gcd(12, 18), and with a negative.
  ty_decimal64_t a = {12, 0};
  ty_decimal64_t b = {18, 0};
  CHECK(0 == ty_decimal64_gcd(&a, &b, &r));
  CHECK(6 == r.value);
  a = {-12, 0};
  CHECK(0 == ty_decimal64_gcd(&a, &b, &r));
  CHECK(6 == r.value);

  // 2. Take gcd(1.5, 0.25), at two scales.
  a = {15, 1};
  b = {25, 2};
  CHECK(0 == ty_decimal64_gcd(&a, &b, &r));
  CHECK(25 == r.value);
  CHECK(2 == r.scale);

  // 3. Take gcd(0, 5) and gcd(0, 0).
  a = {0, 0};
  b = {5, 0};
  CHECK(0 == ty_decimal64_gcd(&a, &b, &r));
  CHECK(5 == r.value);
  b = {0, 0};
  CHECK(0 == ty_decimal64_gcd(&a, &b, &r));
  CHECK(0 == r.value);

  // 4. Take gcd(0, a large value) at a scale that makes it 34 digits.
  a = {0, 17};
  b = {99999999999999999, 0};
  CHECK(-1 == ty_decimal64_gcd(&a, &b, &r));
  CHECK(TY_ERR_RANGE == ty_errnum());
}

// Verify decimal64 lcm: at the larger scale, and an error past 17 digits.
// 1. Take lcm(4, 6), and with a negative.
// 2. Take lcm(1.5, 0.25), at two scales.
// 3. Take lcm(0, 5): zero.
// 4. Take lcm of two large coprime values: it fails with TY_ERR_RANGE,
//    and sets no result.
TEST_CASE("decimal64 lcm is at the larger scale, and fails past 17 digits") {
  ty_decimal64_t r = {7, 7};

  // 1. Take lcm(4, 6), and with a negative.
  ty_decimal64_t a = {4, 0};
  ty_decimal64_t b = {6, 0};
  CHECK(0 == ty_decimal64_lcm(&a, &b, &r));
  CHECK(12 == r.value);
  a = {-4, 0};
  CHECK(0 == ty_decimal64_lcm(&a, &b, &r));
  CHECK(12 == r.value);

  // 2. Take lcm(1.5, 0.25), at two scales.
  a = {15, 1};
  b = {25, 2};
  CHECK(0 == ty_decimal64_lcm(&a, &b, &r));
  CHECK(150 == r.value);
  CHECK(2 == r.scale);

  // 3. Take lcm(0, 5): zero.
  a = {0, 0};
  b = {5, 0};
  CHECK(0 == ty_decimal64_lcm(&a, &b, &r));
  CHECK(0 == r.value);

  // 4. Take lcm of two large coprime values: it fails with TY_ERR_RANGE,
  //    and sets no result.
  r = {7, 7};
  a = {99999999999999999, 0};
  b = {99999999999999998, 0};
  CHECK(-1 == ty_decimal64_lcm(&a, &b, &r));
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
  CHECK(-1 == ty_decimal64_cmp({1, 2}, {2, 2}));
  CHECK(0 == ty_decimal64_cmp({2, 2}, {2, 2}));
  CHECK(1 == ty_decimal64_cmp({3, 2}, {2, 2}));

  // 2. Compare values of two scales that are equal: 1.5 and 1.50.
  CHECK(0 == ty_decimal64_cmp({15, 1}, {150, 2}));

  // 3. Compare values of two scales that differ, in both orders.
  CHECK(-1 == ty_decimal64_cmp({15, 1}, {151, 2}));
  CHECK(1 == ty_decimal64_cmp({151, 2}, {15, 1}));

  // 4. Compare negatives, and the largest values at the farthest scales.
  CHECK(-1 == ty_decimal64_cmp({-151, 2}, {-15, 1}));
  CHECK(1 == ty_decimal64_cmp({99999999999999999, 0}, {99999999999999999, 17}));
  CHECK(-1 == ty_decimal64_cmp({-99999999999999999, 0}, {0, 17}));
}

namespace {

/* ty_decimal64_round of the decimal64 {v, s} to n places. */
int round_of(int64_t v, int s, int n, ty_decimal64_t *r) {
  ty_decimal64_t a = {v, s};
  return ty_decimal64_round(&a, n, r);
}

/* ty_decimal64_trunc of the decimal64 {v, s} to n places. */
int trunc_of(int64_t v, int s, int n, ty_decimal64_t *r) {
  ty_decimal64_t a = {v, s};
  return ty_decimal64_trunc(&a, n, r);
}

} // namespace

// Verify decimal64 round and trunc: places, zeros, and tens.
// 1. Round to fewer places, half away from zero, both signs.
// 2. Truncate to fewer places, both signs.
// 3. Round to more places: zeros are added, and past 17 it fails.
// 4. Round to tens and hundreds: the scale is 0; a carry that makes 18
//    digits fails with TY_ERR_RANGE.
// 5. Round to tens past the value: zero.
TEST_CASE("decimal64 round and trunc") {
  ty_decimal64_t r;

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
}

// Verify decimal64 ceil and floor.
// 1. Take them of a value with places, positive and negative.
// 2. Take them of a whole number: it is itself.
TEST_CASE("decimal64 ceil and floor give a whole number") {
  // 1. Take them of a value with places, positive and negative.
  CHECK(2 == ty_decimal64_ceil({15, 1}).value);
  CHECK(1 == ty_decimal64_floor({15, 1}).value);
  CHECK(-1 == ty_decimal64_ceil({-15, 1}).value);
  CHECK(-2 == ty_decimal64_floor({-15, 1}).value);
  CHECK(0 == ty_decimal64_ceil({15, 1}).scale);

  // 2. Take them of a whole number: it is itself.
  CHECK(7 == ty_decimal64_ceil({700, 2}).value);
  CHECK(7 == ty_decimal64_floor({700, 2}).value);
  CHECK(7 == ty_decimal64_ceil({7, 0}).value);
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
  ty_decimal128_t r = {7, 7};
  ty_int128_t big = ten_to(36) - 1;

  // 1. Add and subtract at one scale and at two.
  ty_decimal128_t a = {125, 2};
  ty_decimal128_t b = {250, 2};
  CHECK(0 == ty_decimal128_add(&a, &b, &r));
  CHECK(375 == r.value);
  a = {15, 1};
  b = {25, 3};
  CHECK(0 == ty_decimal128_sub(&a, &b, &r));
  CHECK(1475 == r.value);
  CHECK(3 == r.scale);

  // 2. Add up to the largest value, 36 digits, which fits; one more fails.
  a = {big - 1, 0};
  b = {1, 0};
  CHECK(0 == ty_decimal128_add(&a, &b, &r));
  CHECK(big == r.value);
  r = {7, 7};
  a = {big, 0};
  CHECK(-1 == ty_decimal128_add(&a, &b, &r));
  CHECK(TY_ERR_RANGE == ty_errnum());

  // 3. Add where aligning the scales passes 128 bits: it fails.
  a = {big, 0};
  b = {1, 36};
  CHECK(-1 == ty_decimal128_add(&a, &b, &r));
  CHECK(TY_ERR_RANGE == ty_errnum());

  // 4. Multiply, with a negative; to the largest value, which fits.
  a = {-15, 1};
  b = {225, 2};
  CHECK(0 == ty_decimal128_mul(&a, &b, &r));
  CHECK(-3375 == r.value);
  CHECK(3 == r.scale);
  a = {big, 0};
  b = {1, 0};
  CHECK(0 == ty_decimal128_mul(&a, &b, &r));
  CHECK(big == r.value);

  // 5. Multiply past 36 digits, and past 128 bits, and to 37 places.
  r = {7, 7};
  b = {10, 0};
  CHECK(-1 == ty_decimal128_mul(&a, &b, &r));
  CHECK(TY_ERR_RANGE == ty_errnum());
  a = {big, 0};
  b = {big, 0};
  CHECK(-1 == ty_decimal128_mul(&a, &b, &r));
  CHECK(TY_ERR_RANGE == ty_errnum());
  a = {1, 20};
  b = {1, 17};
  CHECK(-1 == ty_decimal128_mul(&a, &b, &r));
  CHECK(TY_ERR_RANGE == ty_errnum());
  CHECK(7 == r.value);
}

namespace {

/* ty_decimal128_round of {v, s} to n places. */
int round128_of(ty_int128_t v, int s, int n, ty_decimal128_t *r) {
  ty_decimal128_t a = {v, s};
  return ty_decimal128_round(&a, n, r);
}

/* ty_decimal128_trunc of {v, s} to n places. */
int trunc128_of(ty_int128_t v, int s, int n, ty_decimal128_t *r) {
  ty_decimal128_t a = {v, s};
  return ty_decimal128_trunc(&a, n, r);
}

} // namespace

// Verify decimal128 neg, abs, sign, ceil, floor and compare.
// 1. Negate, take the absolute value and the sign of both signs and zero.
// 2. Take ceil and floor of values with places, and of a whole number.
// 3. Compare at one scale, at two, and where aligning passes 128 bits.
TEST_CASE("decimal128 neg, abs, sign, ceil, floor and cmp") {
  ty_int128_t big = ten_to(36) - 1;

  // 1. Negate, take the absolute value and the sign of both signs and zero.
  CHECK(-125 == ty_decimal128_neg({125, 2}).value);
  CHECK(125 == ty_decimal128_abs({-125, 2}).value);
  CHECK(2 == ty_decimal128_abs({-125, 2}).scale);
  CHECK(1 == ty_decimal128_sign({125, 2}));
  CHECK(-1 == ty_decimal128_sign({-125, 2}));
  CHECK(0 == ty_decimal128_sign({0, 9}));

  // 2. Take ceil and floor of values with places, and of a whole number.
  CHECK(2 == ty_decimal128_ceil({15, 1}).value);
  CHECK(1 == ty_decimal128_floor({15, 1}).value);
  CHECK(-1 == ty_decimal128_ceil({-15, 1}).value);
  CHECK(-2 == ty_decimal128_floor({-15, 1}).value);
  CHECK(7 == ty_decimal128_ceil({7, 0}).value);
  CHECK(big == ty_decimal128_floor({big * 10 + 5 > 0 ? big : 0, 0}).value);

  // 3. Compare at one scale, at two, and where aligning passes 128 bits.
  CHECK(-1 == ty_decimal128_cmp({1, 2}, {2, 2}));
  CHECK(0 == ty_decimal128_cmp({15, 1}, {150, 2}));
  CHECK(1 == ty_decimal128_cmp({151, 2}, {15, 1}));
  CHECK(1 == ty_decimal128_cmp({big, 0}, {big, 36}));
  CHECK(-1 == ty_decimal128_cmp({-big, 0}, {big, 36}));
  CHECK(-1 == ty_decimal128_cmp({big, 36}, {big, 0}));
}

// Verify decimal128 round and trunc.
// 1. Round to fewer places, half away from zero, both signs; and cut.
// 2. Round to more places: zeros are added; to 40 places is 36.
// 3. Round to tens and hundreds: the scale is 0.
// 4. Round past 36 digits: it fails with TY_ERR_RANGE.
TEST_CASE("decimal128 round and trunc") {
  ty_decimal128_t r;
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
}

// Verify decimal128 %, gcd and lcm, including scales that do not align in
// 128 bits.
// 1. Take % at one scale, with signs, and at two scales; by zero it fails.
// 2. Take % where aligning a passes 128 bits, and where aligning b does.
// 3. Take gcd, at two scales, with zero, and where aligning passes 128 bits.
// 4. Take lcm, with zero, at two scales; of large coprimes it fails.
TEST_CASE("decimal128 mod, gcd and lcm") {
  ty_decimal128_t r;
  ty_int128_t big = ten_to(36) - 1;

  // 1. Take % at one scale, with signs, and at two scales; by zero it fails.
  ty_decimal128_t a = {7, 0};
  ty_decimal128_t b = {3, 0};
  CHECK(0 == ty_decimal128_mod(&a, &b, &r));
  CHECK(1 == r.value);
  a = {-7, 0};
  CHECK(0 == ty_decimal128_mod(&a, &b, &r));
  CHECK(-1 == r.value);
  a = {75, 1};
  b = {225, 2};
  CHECK(0 == ty_decimal128_mod(&a, &b, &r));
  CHECK(75 == r.value);
  CHECK(2 == r.scale);
  b = {0, 5};
  CHECK(-1 == ty_decimal128_mod(&a, &b, &r));
  CHECK(TY_ERR_DIVZERO == ty_errnum());

  // 2. Take % where aligning a passes 128 bits, and where aligning b does.
  //    big * 10^36 mod 7 is 0, as 10^6 - 1 divides big. Then 5 mod 11
  //    with a at 10^36 scale: (5 * 10^0 at scale 36 vs 11 at scale 0).
  a = {big, 0};
  b = {7, 36};
  CHECK(0 == ty_decimal128_mod(&a, &b, &r));
  CHECK(0 == r.value);
  CHECK(36 == r.scale);
  a = {big - 1, 0};
  b = {11, 36};
  CHECK(0 == ty_decimal128_mod(&a, &b, &r));
  CHECK(((big - 1) % 11 * ten_to(36) % 11) == r.value);
  a = {5, 36};
  b = {big, 0};
  CHECK(0 == ty_decimal128_mod(&a, &b, &r));
  CHECK(5 == r.value);

  // 3. Take gcd, at two scales, with zero, and where aligning passes 128
  //    bits: big * 10^36 is a multiple of 14, so the gcd with 14 is 14.
  a = {15, 1};
  b = {25, 2};
  CHECK(0 == ty_decimal128_gcd(&a, &b, &r));
  CHECK(25 == r.value);
  a = {0, 0};
  b = {5, 0};
  CHECK(0 == ty_decimal128_gcd(&a, &b, &r));
  CHECK(5 == r.value);
  a = {big, 0};
  b = {14, 36};
  CHECK(0 == ty_decimal128_gcd(&a, &b, &r));
  CHECK(14 == r.value);
  a = {0, 36};
  b = {big, 0};
  CHECK(-1 == ty_decimal128_gcd(&a, &b, &r));
  CHECK(TY_ERR_RANGE == ty_errnum());

  // 4. Take lcm, with zero, at two scales; of large coprimes it fails.
  a = {4, 0};
  b = {6, 0};
  CHECK(0 == ty_decimal128_lcm(&a, &b, &r));
  CHECK(12 == r.value);
  a = {15, 1};
  b = {25, 2};
  CHECK(0 == ty_decimal128_lcm(&a, &b, &r));
  CHECK(150 == r.value);
  a = {0, 3};
  CHECK(0 == ty_decimal128_lcm(&a, &b, &r));
  CHECK(0 == r.value);
  r = {7, 7};
  a = {big, 0};
  b = {big - 1, 0};
  CHECK(-1 == ty_decimal128_lcm(&a, &b, &r));
  CHECK(TY_ERR_RANGE == ty_errnum());
  CHECK(7 == r.value);
}

namespace {

/* ty_decimal128_div of {v1, s1} by {v2, s2} at rscale places. */
int div128_of(ty_int128_t v1, int s1, ty_int128_t v2, int s2, int rscale,
              bool cut, ty_decimal128_t *r) {
  ty_decimal128_t a = {v1, s1};
  ty_decimal128_t b = {v2, s2};
  return ty_decimal128_div(&a, &b, rscale, cut, r);
}

/* ty_decimal64_div of {v1, s1} by {v2, s2} at rscale places. */
int div64_of(int64_t v1, int s1, int64_t v2, int s2, int rscale,
             ty_decimal64_t *r) {
  ty_decimal64_t a = {v1, s1};
  ty_decimal64_t b = {v2, s2};
  return ty_decimal64_div(&a, &b, rscale, r);
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
  ty_decimal128_t r;
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
  ty_decimal64_t r;

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

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

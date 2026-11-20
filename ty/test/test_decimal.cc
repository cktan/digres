/*
 * Copyright (c) 2026 CK Tan
 * SPDX-License-Identifier: MIT
 */

/*
 * Tests of the decimal operations.
 */
#include "doctest.h"
#include "ty_decimal.h"

namespace {

/* 10^n as a 128-bit integer. */
ty_int128_t pow10_of(int n) {
  ty_int128_t r = 1;
  for (; n > 0; n--) {
    r *= 10;
  }
  return r;
}

} // namespace

// Verify that decimal64 addition is exact, and takes the larger scale.
// 1. Add two values of one scale: 1.25 + 2.50.
// 2. Add two of different scales, in both orders: 1.5 + 0.025.
// 3. Add a negative: 1.5 + -2.
// 4. Add the largest values at the largest scales, 17 digits each.
TEST_CASE("decimal64 add is exact, at the larger scale") {
  ty_decimal128_t r;

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

  // 4. Add the largest values at the largest scales, 17 digits each.
  ty_int128_t big = pow10_of(17) - 1;
  a = {(int64_t)big, 17};
  b = {(int64_t)big, 0};
  CHECK(0 == ty_decimal64_add(&a, &b, &r));
  CHECK(big + big * pow10_of(17) == r.value);
  CHECK(17 == r.scale);
}

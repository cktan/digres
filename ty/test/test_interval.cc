/*
 * Copyright (c) 2026 CK Tan
 * SPDX-License-Identifier: MIT
 */

/*
 * Tests of the interval arithmetic; the rest are in the vectors.
 */
#include <string>

#include "doctest.h"
#include "ty_interval.h"

// Verify the order: a month is 30 days and a day 24 hours.
// 1. 1 month equals 30 days; 1 month is below 31 days.
// 2. 1 day equals 24 hours.
// 3. Infinity is above every finite value.
TEST_CASE("interval order") {
  ty_interval_t month = {0, 0, 1}, days30 = {0, 30, 0}, days31 = {0, 31, 0};
  ty_interval_t day = {0, 1, 0}, hours24 = {24 * 3600 * INT64_C(1000000), 0, 0};
  ty_interval_t inf;

  // 1. 1 month equals 30 days; 1 month is below 31 days.
  ty_interval_set_noend(&inf);
  month.month = 1;
  month.time = 0;
  CHECK(0 == ty_interval_cmp(&month, &days30));
  CHECK(ty_interval_cmp(&month, &days31) < 0);

  // 2. 1 day equals 24 hours.
  CHECK(0 == ty_interval_cmp(&day, &hours24));

  // 3. Infinity is above every finite value.
  CHECK(ty_interval_cmp(&inf, &days31) > 0);
}

// Verify the errors of the arithmetic.
// 1. Infinity plus minus infinity is out of range.
// 2. A month past int32 is out of range.
// 3. A division by 0 is TY_ERR_DIVZERO; by NaN, out of range.
TEST_CASE("interval errors") {
  ty_interval_t a, b, r;

  // 1. Infinity plus minus infinity is out of range.
  ty_interval_set_noend(&a);
  ty_interval_set_nobegin(&b);
  CHECK(-1 == ty_interval_add(&a, &b, &r));
  CHECK(TY_ERR_RANGE == ty_errnum());

  // 2. A month past int32 is out of range.
  a = {0, 0, 0};
  a.month = INT32_MAX;
  b = {0, 0, 0};
  b.month = 1;
  CHECK(-1 == ty_interval_add(&a, &b, &r));
  CHECK(std::string(ty_errmsg()) == "interval out of range");

  // 3. A division by 0 is TY_ERR_DIVZERO; by NaN, out of range.
  CHECK(-1 == ty_interval_div(&b, 0.0, &r));
  CHECK(TY_ERR_DIVZERO == ty_errnum());
  CHECK(-1 == ty_interval_div(&b, __builtin_nan(""), &r));
  CHECK(TY_ERR_RANGE == ty_errnum());
}

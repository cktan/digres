/*
 * Copyright (c) 2026 CK Tan
 * SPDX-License-Identifier: MIT
 */

/*
 * Tests of the float operations.
 */
#include <math.h>
#include <string.h>

#include <string>

#include "doctest.h"
#include "ty_float.h"

// Verify that arithmetic reports overflow, underflow and division by zero.
// 1. DBL_MAX + DBL_MAX overflows: TY_ERR_RANGE; with an infinity it does not.
// 2. 1e-200 * 1e-200 underflows: TY_ERR_UNDERFLOW.
// 3. 1 / 0 is TY_ERR_DIVZERO, but NaN / 0 is NaN.
// 4. A float32 product past FLT_MAX overflows.
TEST_CASE("float arithmetic checks overflow, underflow and division by zero") {
  double r = 0;
  float f = 0;

  // 1. DBL_MAX + DBL_MAX overflows: TY_ERR_RANGE; with an infinity it does not.
  CHECK(-1 ==
        ty_float64_add(1.7976931348623157e308, 1.7976931348623157e308, &r));
  CHECK(TY_ERR_RANGE == ty_errnum());
  CHECK(0 == ty_float64_add(INFINITY, 1.0, &r));
  CHECK(std::isinf(r));

  // 2. 1e-200 * 1e-200 underflows: TY_ERR_UNDERFLOW.
  CHECK(-1 == ty_float64_mul(1e-200, 1e-200, &r));
  CHECK(TY_ERR_UNDERFLOW == ty_errnum());
  CHECK(0 == ty_float64_mul(0.0, 1e-200, &r));

  // 3. 1 / 0 is TY_ERR_DIVZERO, but NaN / 0 is NaN.
  CHECK(-1 == ty_float64_div(1.0, 0.0, &r));
  CHECK(TY_ERR_DIVZERO == ty_errnum());
  CHECK(0 == ty_float64_div(NAN, 0.0, &r));
  CHECK(std::isnan(r));

  // 4. A float32 product past FLT_MAX overflows.
  CHECK(-1 == ty_float32_mul(3e38f, 3e38f, &f));
  CHECK(TY_ERR_RANGE == ty_errnum());
}

// Verify that every NaN is equal, and above every number.
// 1. NaN equals NaN, and is not less than it.
// 2. NaN is above infinity, in cmp, lt, gt, larger and smaller.
// 3. 0 and -0 are equal.
TEST_CASE("float order puts NaN last and equal to itself") {
  // 1. NaN equals NaN, and is not less than it.
  CHECK(ty_float64_eq(NAN, NAN));
  CHECK(!ty_float64_lt(NAN, NAN));
  CHECK(ty_float64_le(NAN, NAN));

  // 2. NaN is above infinity, in cmp, lt, gt, larger and smaller.
  CHECK(ty_float64_cmp(NAN, INFINITY) == 1);
  CHECK(ty_float64_cmp(INFINITY, NAN) == -1);
  CHECK(ty_float64_gt(NAN, INFINITY));
  CHECK(std::isnan(ty_float64_larger(1.0, NAN)));
  CHECK(ty_float64_smaller(1.0, NAN) == 1.0);

  // 3. 0 and -0 are equal.
  CHECK(ty_float64_eq(0.0, -0.0));
  CHECK(ty_float64_cmp(0.0, -0.0) == 0);
}

// Verify the conversions: rounding to the nearest even, and the range.
// 1. 2.5 and 3.5 round to 2 and 4; -0.5 to 0.
// 2. 32767.5 rounds to 32768, which is out of range for an int16.
// 3. NaN and 2^63 are out of range for an int64.
// 4. A float64 too large for a float32 overflows; one too small underflows.
TEST_CASE("float to integer rounds to the nearest even and checks the range") {
  int16_t i16 = 0;
  int64_t i64 = 0;
  float f = 0;

  // 1. 2.5 and 3.5 round to 2 and 4; -0.5 to 0.
  CHECK(0 == ty_float64_to_int16(2.5, &i16));
  CHECK(2 == i16);
  CHECK(0 == ty_float64_to_int16(3.5, &i16));
  CHECK(4 == i16);
  CHECK(0 == ty_float64_to_int16(-0.5, &i16));
  CHECK(0 == i16);

  // 2. 32767.5 rounds to 32768, which is out of range for an int16.
  CHECK(-1 == ty_float64_to_int16(32767.5, &i16));
  CHECK(TY_ERR_RANGE == ty_errnum());

  // 3. NaN and 2^63 are out of range for an int64.
  CHECK(-1 == ty_float64_to_int64(NAN, &i64));
  CHECK(-1 == ty_float64_to_int64(9223372036854775808.0, &i64));

  // 4. A float64 too large for a float32 overflows; one too small underflows.
  CHECK(-1 == ty_float64_to_float32(1e39, &f));
  CHECK(TY_ERR_RANGE == ty_errnum());
  CHECK(-1 == ty_float64_to_float32(1e-50, &f));
  CHECK(TY_ERR_UNDERFLOW == ty_errnum());
}

// Verify the domain errors and their messages.
// 1. sqrt(-1), ln(0), ln(-1), 0 ^ -1 and (-8) ^ 0.5 are TY_ERR_INVALID,
//    each with PostgreSQL's text.
// 2. NaN ^ 0 and 1 ^ NaN are 1; 2 ^ 0.5 is sqrt(2).
// 3. exp(1000) overflows, exp(-1000) underflows, exp(-inf) is 0.
TEST_CASE("powers and logarithms refuse what has no real value") {
  double r = 0;

  // 1. sqrt(-1), ln(0), ln(-1), 0 ^ -1 and (-8) ^ 0.5 are TY_ERR_INVALID,
  //    each with PostgreSQL's text.
  CHECK(-1 == ty_float64_sqrt(-1.0, &r));
  CHECK(TY_ERR_INVALID == ty_errnum());
  CHECK(std::string(ty_errmsg()) ==
        "cannot take square root of a negative number");
  CHECK(-1 == ty_float64_ln(0.0, &r));
  CHECK(std::string(ty_errmsg()) == "cannot take logarithm of zero");
  CHECK(-1 == ty_float64_ln(-1.0, &r));
  CHECK(std::string(ty_errmsg()) ==
        "cannot take logarithm of a negative number");
  CHECK(-1 == ty_float64_pow(0.0, -1.0, &r));
  CHECK(std::string(ty_errmsg()) ==
        "zero raised to a negative power is undefined");
  CHECK(-1 == ty_float64_pow(-8.0, 0.5, &r));
  CHECK(TY_ERR_INVALID == ty_errnum());

  // 2. NaN ^ 0 and 1 ^ NaN are 1; 2 ^ 0.5 is sqrt(2).
  CHECK(0 == ty_float64_pow(NAN, 0.0, &r));
  CHECK(r == 1.0);
  CHECK(0 == ty_float64_pow(1.0, NAN, &r));
  CHECK(r == 1.0);
  CHECK(0 == ty_float64_pow(2.0, 0.5, &r));
  CHECK(r == sqrt(2.0));

  // 3. exp(1000) overflows, exp(-1000) underflows, exp(-inf) is 0.
  CHECK(-1 == ty_float64_exp(1000.0, &r));
  CHECK(TY_ERR_RANGE == ty_errnum());
  CHECK(-1 == ty_float64_exp(-1000.0, &r));
  CHECK(TY_ERR_UNDERFLOW == ty_errnum());
  CHECK(0 == ty_float64_exp(-INFINITY, &r));
  CHECK(r == 0.0);
}

// Verify the text of a float.
// 1. extra_float_digits 3 prints the shortest text: 0.1 is 0.1, 1e22 is 1e+22;
//    PostgreSQL prints 1e23 as 9.999999999999999e+22.
// 2. extra_float_digits 0 prints 15 digits: 0.1 + 0.2 is 0.3; a float32 6.
// 3. NaN, Infinity, -Infinity and -0.
// 4. Reading: space, a sign, an exponent, and the names in any case.
// 5. Out of range is TY_ERR_RANGE with the length of the number; junk after
//    is TY_ERR_INVALID unless the caller asks where the number stopped.
TEST_CASE("float text is read and written as PostgreSQL does") {
  char out[TY_FLOAT_TEXT_SIZE];
  double d = 0;
  float f = 0;
  const char *end = nullptr;
  size_t len = 0;

  // 1. extra_float_digits 3 prints the shortest text: 0.1 is 0.1, 1e23 is
  // 1e+23.
  ty_float64_format(0.1, 3, out);
  CHECK(std::string(out) == "0.1");
  ty_float64_format(1e22, 3, out);
  CHECK(std::string(out) == "1e+22");
  ty_float64_format(1e23, 3, out);
  CHECK(std::string(out) == "9.999999999999999e+22");

  // 2. extra_float_digits 0 prints 15 digits: 0.1 + 0.2 is 0.3; a float32 6.
  ty_float64_format(0.1 + 0.2, 0, out);
  CHECK(std::string(out) == "0.3");
  ty_float32_format(1.0f / 3.0f, 0, out);
  CHECK(std::string(out) == "0.333333");

  // 3. NaN, Infinity, -Infinity and -0.
  ty_float64_format(NAN, 0, out);
  CHECK(std::string(out) == "NaN");
  ty_float64_format(INFINITY, 3, out);
  CHECK(std::string(out) == "Infinity");
  ty_float64_format(-INFINITY, 0, out);
  CHECK(std::string(out) == "-Infinity");
  ty_float64_format(-0.0, 0, out);
  CHECK(std::string(out) == "-0");

  // 4. Reading: space, a sign, an exponent, and the names in any case.
  CHECK(0 == ty_float64_parse("  -1.5e2 \n", &d, nullptr, nullptr));
  CHECK(d == -150.0);
  CHECK(0 == ty_float64_parse("-iNfInItY", &d, nullptr, nullptr));
  CHECK(d == -INFINITY);
  CHECK(0 == ty_float32_parse("NaN", &f, nullptr, nullptr));
  CHECK(std::isnan(f));

  // 5. Out of range is TY_ERR_RANGE with the length of the number; junk after
  //    is TY_ERR_INVALID unless the caller asks where the number stopped.
  CHECK(-1 == ty_float64_parse(" 1e999 x", &d, &end, &len));
  CHECK(TY_ERR_RANGE == ty_errnum());
  CHECK(len == 5);
  CHECK(-1 == ty_float64_parse("1.5 x", &d, nullptr, nullptr));
  CHECK(TY_ERR_INVALID == ty_errnum());
  CHECK(0 == ty_float64_parse("1.5 x", &d, &end, &len));
  CHECK(std::string(end) == "x");
}

// Verify the hash: 0 and -0 alike, all NaNs alike, float32 as float64.
// 1. hash(0) = hash(-0) = the seed.
// 2. Two NaNs of different bits hash alike.
// 3. A float32 and the float64 of its value hash alike.
TEST_CASE("the float hash treats equal values alike") {
  // 1. hash(0) = hash(-0) = the seed.
  CHECK(ty_float64_hash(0.0, 7) == 7);
  CHECK(ty_float64_hash(-0.0, 7) == 7);

  // 2. Two NaNs of different bits hash alike.
  uint64_t bits = 0x7ff8000000000123ULL;
  double other;
  memcpy(&other, &bits, 8);
  CHECK(ty_float64_hash(NAN, 0) == ty_float64_hash(other, 0));

  // 3. A float32 and the float64 of its value hash alike.
  CHECK(ty_float32_hash(1.5f, 3) == ty_float64_hash(1.5, 3));
}

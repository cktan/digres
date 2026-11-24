/*
 * Copyright (c) 2026 CK Tan
 * SPDX-License-Identifier: MIT
 */

/*
 * Tests of the scale of a decimal's results that are not exact, the power,
 * a typmod, and the casts to the integers (ty_decimal.h), by hand and by the
 * vectors of test/gen_decscale.py.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>

#include "doctest.h"
#include "ty_decimal.h"

namespace {

/* The text of the decimal128 d. */
std::string text(const ty_dec128_t &d) {
  char buf[TY_DEC_TEXT_SIZE];

  ty_dec128_format(&d, buf);
  return buf;
}

/* The decimal128 that s is. */
ty_dec128_t dec(const char *s) {
  ty_dec128_t d;
  int why;

  REQUIRE(0 == ty_dec128_parse(s, -1, 0, &d, &why));
  return d;
}

/* The result of a call that returned rc, or err. */
std::string result(int rc, const ty_dec128_t &r) {
  return 0 == rc ? text(r) : "err";
}

/* a rounded to an integer from lo to hi. */
std::string to_int(const ty_dec128_t &a, int64_t lo, int64_t hi) {
  int64_t v;

  return 0 == ty_dec128_to_int64(&a, lo, hi, &v) ? std::to_string(v) : "err";
}

/* How many places the number s has. */
std::string places(const std::string &s) {
  size_t dot = s.find('.');

  return "places " +
         std::to_string(std::string::npos == dot ? 0 : s.size() - dot - 1);
}

/*
 * The places of the sample variance of a and b, not zero: about the number
 * digres's aggregate makes, whose sums differ in their last bits.
 */
std::string variance(const ty_dec128_t &a, const ty_dec128_t &b) {
  ty_dec128_t r = {0, 0};
  int scale = a.scale > b.scale ? a.scale : b.scale;
  double d = ty_dec128_to_double(&a) - ty_dec128_to_double(&b);
  int rscale = ty_dec128_var_scale(d * d, 2 * scale, 2);

  return 0 == ty_dec128_from_double(d * d / 2, rscale, &r) ? places(text(r))
                                                           : "err";
}

} // namespace

// Verify the scales, the power, a typmod and the casts by hand.
// 1. The scale of a quotient, a root and a logarithm: 16 digits, and not
//    below the arguments' scales.
// 2. The power: zero's, a whole one, 1 and -1 to a large one (at once, not
//    by a billion products), a real one, and its errors.
// 3. A typmod rounds, and refuses too many digits.
// 4. The casts round, and refuse what does not fit; the digits of a value.
TEST_CASE("a decimal's scales, power, typmod and casts") {
  ty_dec128_t a, b, r;

  // 1. The scales.
  a = dec("1"), b = dec("3");
  CHECK(20 == ty_dec128_div_scale(&a, &b));
  a = dec("2");
  CHECK(15 == ty_dec128_sqrt_scale(&a));
  a = dec("2.00000000000000000000");
  CHECK(20 == ty_dec128_sqrt_scale(&a));
  a = dec("10");
  CHECK(16 == ty_dec128_ln_scale(&a));

  // 2. The power.
  a = dec("0"), b = dec("0");
  CHECK(result(ty_dec128_power(&a, &b, &r), r) == "1.0000000000000000");
  b = dec("-1");
  CHECK(-1 == ty_dec128_power(&a, &b, &r));
  CHECK(std::string(ty_errmsg()) ==
        "zero raised to a negative power is undefined");
  a = dec("2"), b = dec("10");
  CHECK(result(ty_dec128_power(&a, &b, &r), r) == "1024.0000000000000");
  a = dec("1"), b = dec("999999999");
  CHECK(result(ty_dec128_power(&a, &b, &r), r) == "1.0000000000000000");
  a = dec("-1");
  CHECK(result(ty_dec128_power(&a, &b, &r), r) == "-1.0000000000000000");
  a = dec("-2"), b = dec("0.5");
  CHECK(-1 == ty_dec128_power(&a, &b, &r));
  CHECK(TY_ERR_INVALID == ty_errnum());
  a = dec("10"), b = dec("100");
  CHECK(-1 == ty_dec128_power(&a, &b, &r));
  CHECK(TY_ERR_RANGE == ty_errnum());

  // 3. A typmod.
  a = dec("1.25");
  CHECK(0 == ty_dec128_typmod(&a, 2, 1));
  CHECK(text(a) == "1.3");
  a = dec("9.95");
  CHECK(-1 == ty_dec128_typmod(&a, 2, 1));
  CHECK(text(a) == "9.95");

  // 4. The casts, and the digits of a value.
  CHECK(to_int(dec("2.5"), -10, 10) == "3");
  CHECK(to_int(dec("-2.5"), -10, 10) == "-3");
  CHECK(to_int(dec("10.5"), -10, 10) == "err");
  CHECK(1 == ty_dec128_digits(0));
  CHECK(3 == ty_dec128_digits(-100));
}

// Verify they are digres's, by what a digres built before they moved gave.
// 1. Read test/decscale_known.txt.
// 2. The operation, at the scale ty gives it; of a variance, its places.
// 3. An error is an error; digres's SQLSTATEs are its own.
TEST_CASE("a decimal's scales, power, typmod and casts are digres's") {
  const char *env = getenv("TY_VECTORS");
  std::string dir = env ? env : "test/vectors.txt";
  FILE *fp = fopen(
      (dir.substr(0, dir.rfind('/') + 1) + "decscale_known.txt").c_str(), "r");
  char line[4096];
  int n = 0, bad = 0;

  REQUIRE(nullptr != fp);

  // 1. Read test/decscale_known.txt.
  while (fgets(line, sizeof line, fp)) {
    char op[16], a[512], b[512], want[1024];
    ty_dec128_t y = {0, 0}, r = {0, 0};
    std::string got;

    if (4 != sscanf(line, "%15s %511s %511s => %1023s", op, a, b, want)) {
      continue;
    }
    ty_dec128_t x = dec(a);
    if (0 != strcmp(b, "-") && 0 != strcmp(op, "typmod")) {
      y = dec(b);
    }

    // 2. The operation, at the scale ty gives it.
    if (0 == strcmp(op, "div")) {
      got = result(
          0 == y.value
              ? -1
              : ty_dec128_div(&x, &y, ty_dec128_div_scale(&x, &y), false, &r),
          r);
    } else if (0 == strcmp(op, "sqrt")) {
      got = result(ty_dec128_sqrt(&x, ty_dec128_sqrt_scale(&x), &r), r);
    } else if (0 == strcmp(op, "exp")) {
      got = result(ty_dec128_exp(&x, ty_dec128_exp_scale(&x), &r), r);
    } else if (0 == strcmp(op, "ln")) {
      got = result(ty_dec128_ln(&x, ty_dec128_ln_scale(&x), &r), r);
    } else if (0 == strcmp(op, "log")) {
      got = result(ty_dec128_log(&x, &y, ty_dec128_log_scale(&x, &y), &r), r);
    } else if (0 == strcmp(op, "pow")) {
      got = result(ty_dec128_power(&x, &y, &r), r);
    } else if (0 == strcmp(op, "int2")) {
      got = to_int(x, INT16_MIN, INT16_MAX);
    } else if (0 == strcmp(op, "int4")) {
      got = to_int(x, INT32_MIN, INT32_MAX);
    } else if (0 == strcmp(op, "int8")) {
      got = to_int(x, INT64_MIN, INT64_MAX);
    } else if (0 == strcmp(op, "typmod")) {
      int p, s;

      REQUIRE(2 == sscanf(b, "%d,%d", &p, &s));
      got = result(ty_dec128_typmod(&x, p, s), x);
    } else if (0 == strcmp(op, "var")) {
      if (0 == strcmp(want, "0") || 0 == strcmp(want, "err")) {
        continue; /* the aggregate's own rules */
      }
      got = variance(x, y);
      snprintf(want, sizeof want, "%s", places(want).c_str());
    } else {
      continue;
    }
    n++;
    if (got != want && bad++ < 20) {
      INFO(line);
      CHECK(got == want);
    }
  }
  fclose(fp);
  CHECK(0 == bad);
  CHECK(n > 4900);
}

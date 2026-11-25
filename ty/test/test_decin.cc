/*
 * Copyright (c) 2026 CK Tan
 * SPDX-License-Identifier: MIT
 */

/*
 * Tests of a decimal's text (ty_decimal.h): numeric's input and output,
 * the binary form's digits, the scale functions and the float casts, by
 * hand and by the vectors of test/gen_decin.py.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>

#include "doctest.h"
#include "ty_decimal.h"
#include "ty_float.h"

namespace {

/* The bytes that the hex digits h stand for; "-" is empty. */
std::string unhex(const std::string &h) {
  std::string s;

  if ("-" == h) {
    return s;
  }
  for (size_t i = 0; i + 1 < h.size(); i += 2) {
    s += (char)strtol(h.substr(i, 2).c_str(), nullptr, 16);
  }
  return s;
}

/* The text of the decimal128 d. */
std::string text(const ty_dec128_t &d) {
  char buf[TY_DEC_TEXT_SIZE];

  ty_dec128_format(&d, buf);
  return buf;
}

/* s read under a typmod (precision -1 for none): its text, or "ERR why". */
std::string in(const char *s, int precision = -1, int scale = 0) {
  ty_dec128_t d;
  int why = 0;

  if (0 != ty_dec128_parse(s, precision, scale, &d, &why)) {
    return "ERR " + std::to_string(why);
  }
  return text(d);
}

/* The kind a vector's error is, as ty_dec128_parse's why. */
int why_of(const std::string &state, const std::string &kind) {
  if ("nan" == kind) {
    return TY_DEC_IN_NAN;
  }
  if ("digits" == kind) {
    return TY_DEC_IN_DIGITS;
  }
  if ("field" == kind) {
    return TY_DEC_IN_FIELD;
  }
  return "22P02" == state ? TY_DEC_IN_SYNTAX : TY_DEC_IN_RANGE;
}

/* The path of a vector file beside the test's vectors. */
std::string vector_path(const char *name) {
  const char *env = getenv("TY_VECTORS");
  std::string dir = env ? env : "test/vectors.txt";

  return dir.substr(0, dir.rfind('/') + 1) + name;
}

} // namespace

// Verify numeric's text in and out by hand: the forms, the rounding, the
// limits and each error's kind.
// 1. Numbers in each form: a sign, spaces, a point, an exponent, the
//    underscores, and bases 16, 8 and 2.
// 2. A typmod rounds half away from zero, and refuses too many digits.
// 3. Past 36 digits: zeros dropped, others refused, not rounded.
// 4. Each kind of error.
TEST_CASE("numeric's text in and out") {
  // 1. Numbers in each form.
  CHECK(in("0") == "0");
  CHECK(in("-0") == "0");
  CHECK(in("  -12.50  ") == "-12.50");
  CHECK(in(".5") == "0.5");
  CHECK(in("5.") == "5");
  CHECK(in("1_000.5") == "1000.5");
  CHECK(in("1.5e2") == "150");
  CHECK(in("1.5e-2") == "0.015");
  CHECK(in("0x1F") == "31");
  CHECK(in("-0o17") == "-15");
  CHECK(in("0b101") == "5");

  // 2. A typmod rounds half away from zero, and refuses too many digits.
  CHECK(in("1.25", 3, 1) == "1.3");
  CHECK(in("-1.25", 3, 1) == "-1.3");
  CHECK(in("99.94", 3, 1) == "99.9");
  CHECK(in("99.95", 3, 1) == "ERR " + std::to_string(TY_DEC_IN_FIELD));
  CHECK(in("0.5", 2, 2) == "0.50");

  // 3. Past 36 digits: zeros dropped, others refused.
  CHECK(in("123456789012345678901234567890123456.0000") ==
        "123456789012345678901234567890123456");
  CHECK(in("1.0000000000000000000000000000000000005") ==
        "ERR " + std::to_string(TY_DEC_IN_DIGITS));
  CHECK(in("1234567890123456789012345678901234567") ==
        "ERR " + std::to_string(TY_DEC_IN_DIGITS));

  // 4. Each kind of error.
  CHECK(in("abc") == "ERR " + std::to_string(TY_DEC_IN_SYNTAX));
  CHECK(TY_ERR_INVALID == ty_errnum());
  CHECK(in("1 2") == "ERR " + std::to_string(TY_DEC_IN_SYNTAX));
  CHECK(in("NaN") == "ERR " + std::to_string(TY_DEC_IN_NAN));
  CHECK(in("-inf") == "ERR " + std::to_string(TY_DEC_IN_NAN));
  CHECK(in("1e1001") == "ERR " + std::to_string(TY_DEC_IN_RANGE));
  CHECK(TY_ERR_RANGE == ty_errnum());
  CHECK(in("0x") == "ERR " + std::to_string(TY_DEC_IN_SYNTAX));
}

// Verify the digits a binary form is read into, and the scale functions.
// 1. drop rounds half away from zero, or cuts; a carry adds a digit.
// 2. pad adds zeros; typmod and pack round and refuse as the text does.
// 3. min_scale and trim_scale.
TEST_CASE("a decimal's digits and its scale") {
  ty_dec_digits_t d;
  ty_dec128_t v;

  // 1. drop rounds half away from zero, or cuts; a carry adds a digit.
  d.nd = 3;
  d.scale = 2;
  memcpy(d.dig, "995", 3); /* 9.95 */
  ty_dec_digits_drop(&d, 1, 0);
  REQUIRE(0 == ty_dec_digits_pack(&d, 0, &v));
  CHECK(text(v) == "10.0");
  d.nd = 3;
  d.scale = 2;
  memcpy(d.dig, "995", 3);
  ty_dec_digits_drop(&d, 1, 1);
  REQUIRE(0 == ty_dec_digits_pack(&d, 1, &v));
  CHECK(text(v) == "-9.9");

  // 2. pad adds zeros; typmod and pack round and refuse as the text does.
  d.nd = 2;
  d.scale = 1;
  memcpy(d.dig, "15", 2); /* 1.5 */
  REQUIRE(0 == ty_dec_digits_pad(&d, 3));
  REQUIRE(0 == ty_dec_digits_pack(&d, 0, &v));
  CHECK(text(v) == "1.500");
  d.nd = 3;
  d.scale = 1;
  memcpy(d.dig, "999", 3); /* 99.9 */
  CHECK(-1 == ty_dec_digits_typmod(&d, 2, 0));
  d.nd = 37;
  d.scale = 0;
  memset(d.dig, '9', 37);
  CHECK(-1 == ty_dec_digits_pack(&d, 0, &v));

  // 3. min_scale and trim_scale.
  v = {12500, 4}; /* 1.2500 */
  CHECK(2 == ty_dec128_min_scale(&v));
  CHECK(text(ty_dec128_trim_scale(v)) == "1.25");
  v = {0, 3};
  CHECK(0 == ty_dec128_min_scale(&v));
  CHECK(text(ty_dec128_trim_scale(v)) == "0");
}

// Verify numeric's text, its scales and its float casts are digres's, by
// what a digres built before they moved gave.
// 1. Read test/decin_known.txt.
// 2. decin: the text under each typmod, or the kind of its error; a
//    bare numeric, and a typmod of precision 17 or less, is a decimal64,
//    which holds 17 digits.
// 3. scale: min_scale and trim_scale.
// 4. tof8: the float8 and float4 printed, as PostgreSQL prints them.
// 5. fromf8, fromf4: the float as a numeric, by 15 or 6 digits; err if the
//    float has no numeric value, or the decimal64 that digres's cast makes
//    cannot hold it.
TEST_CASE("numeric's text and float casts are digres's") {
  FILE *fp = fopen(vector_path("decin_known.txt").c_str(), "r");
  char line[8192];
  int n = 0, bad = 0;

  REQUIRE(nullptr != fp);

  // 1. Read test/decin_known.txt.
  while (fgets(line, sizeof line, fp)) {
    char kind[16], a[4096], b[4096], c[4096], d[4096], e[64];
    std::string got, want;

    line[strcspn(line, "\n")] = '\0';
    if (1 != sscanf(line, "%15s", kind)) {
      continue;
    }
    n++;
    if (0 == strcmp(kind, "decin")) {
      // 2. decin: the text under each typmod, or the kind of its error.
      int p, s, why = 0;
      ty_dec128_t v;
      ty_dec64_t v64;

      REQUIRE(5 <= sscanf(line, "decin %4095s %d %d => %4095s %4095s %63s", a,
                          &p, &s, b, c, e));
      std::string x = unhex(a);
      int rc = ty_dec128_parse(x.c_str(), p, s, &v, &why);

      if (0 == strcmp(b, "ok")) {
        want = unhex(c);
        got = 0 == rc ? text(v) : "ERR " + std::to_string(why);
      } else if (0 == strcmp(e, "d64")) {
        want = "d64";
        got = 0 == rc && p <= 17 && 0 != ty_dec64_from_dec128(&v, &v64)
                  ? "d64"
                  : "not d64: " + std::to_string(rc);
      } else {
        want = "ERR " + std::to_string(why_of(c, e));
        got = 0 == rc ? text(v) : "ERR " + std::to_string(why);
      }
    } else if (0 == strcmp(kind, "scale")) {
      // 3. scale: min_scale and trim_scale.
      ty_dec128_t v;
      int why;

      REQUIRE(3 == sscanf(line, "scale %4095s => %4095s %4095s", a, b, c));
      REQUIRE(0 == ty_dec128_parse(a, -1, 0, &v, &why));
      want = std::string(b) + " " + c;
      got = std::to_string(ty_dec128_min_scale(&v)) + " " +
            text(ty_dec128_trim_scale(v));
    } else if (0 == strcmp(kind, "tof8")) {
      // 4. tof8: the float8 and float4 printed, as PostgreSQL prints them.
      ty_dec128_t v;
      int why;
      char f8[64], f4[64];

      REQUIRE(3 == sscanf(line, "tof8 %4095s => %4095s %4095s", a, b, c));
      REQUIRE(0 == ty_dec128_parse(a, -1, 0, &v, &why));
      f8[ty_fp64_format(ty_dec128_to_fp64(&v), 1, f8)] = '\0';
      f4[ty_fp32_format(ty_dec128_to_fp32(&v), 1, f4)] = '\0';
      want = std::string(b) + " " + c;
      got = std::string(f8) + " " + (0 == strcmp(c, "-") ? "-" : f4);
    } else if (0 == strcmp(kind, "fromf8") || 0 == strcmp(kind, "fromf4")) {
      // 5. fromf8, fromf4: the float as a numeric, by 15 or 6 digits.
      bool f4 = 0 == strcmp(kind, "fromf4");
      ty_dec128_t v;
      ty_dec64_t v64;
      int why;

      REQUIRE(3 == sscanf(line, "%15s %4095s => %4095s", kind, a, d));
      double x = f4 ? (double)strtof(a, nullptr) : strtod(a, nullptr);
      want = d;
      if (0 != ty_dec128_from_float(x, f4 ? 6 : 15, &v, &why) ||
          0 != ty_dec64_from_dec128(&v, &v64)) {
        got = "err";
      } else {
        got = text(ty_dec128_t{v64.value, v64.scale});
      }
    } else {
      continue;
    }
    if (got != want && bad++ < 20) {
      INFO(line);
      CHECK(got == want);
    }
  }
  fclose(fp);
  CHECK(0 == bad);
  CHECK(n > 6000);
}

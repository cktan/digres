/*
 * Copyright (c) 2026 CK Tan
 * SPDX-License-Identifier: MIT
 */

/*
 * Tests of the money operations; the rest are in the vectors.
 */
#include <string.h>

#include <string>

#include "doctest.h"
#include "ty_money.h"

namespace {

/* The text of a value in the C locale. */
std::string text(int64_t v) {
  char buf[128];

  ty_money_format(v, &ty_money_fmt_c, buf, sizeof buf);
  return buf;
}

} // namespace

// Verify the text in the C locale.
// 1. 123 is "$1.23"; -123 is "-$1.23"; 0 is "$0.00".
// 2. Thousands are grouped by 3: 123456789 is "$1,234,567.89".
// 3. "($1.23)" is -123; "1.235" rounds up to 1.24; "1.234" to 1.23.
// 4. "abc" is invalid; a value past int64 is out of range.
TEST_CASE("money text in the C locale") {
  // 1. 123 is "$1.23"; -123 is "-$1.23"; 0 is "$0.00".
  CHECK(text(123) == "$1.23");
  CHECK(text(-123) == "-$1.23");
  CHECK(text(0) == "$0.00");

  // 2. Thousands are grouped by 3: 123456789 is "$1,234,567.89".
  CHECK(text(123456789) == "$1,234,567.89");

  // 3. "($1.23)" is -123; "1.235" rounds up to 1.24; "1.234" to 1.23.
  int64_t v = 0;
  CHECK(0 == ty_money_parse("($1.23)", &ty_money_fmt_c, &v));
  CHECK(v == -123);
  CHECK(0 == ty_money_parse("1.235", &ty_money_fmt_c, &v));
  CHECK(v == 124);
  CHECK(0 == ty_money_parse("1.234", &ty_money_fmt_c, &v));
  CHECK(v == 123);

  // 4. "abc" is invalid; a value past int64 is out of range.
  CHECK(-1 == ty_money_parse("abc", &ty_money_fmt_c, &v));
  CHECK(TY_ERR_INVALID == ty_errnum());
  CHECK(-1 == ty_money_parse("92233720368547758.08", &ty_money_fmt_c, &v));
  CHECK(TY_ERR_RANGE == ty_errnum());
}

// Verify a locale other than C: the symbols and the places come from it.
// 1. A locale with "kr" after the value, a comma point and a space separator.
// 2. The same text is read back.
TEST_CASE("money text in another locale") {
  ty_money_fmt_t f = {2, ",", " ", "kr", "", "-", 3, 0, 0, 1, 1, 1, 1};
  char buf[64];

  // 1. A locale with "kr" after the value, a comma point and a space
  //    separator.
  size_t n = ty_money_format(123456789, &f, buf, sizeof buf);
  CHECK(std::string(buf, n) == "1 234 567,89 kr");

  // 2. The same text is read back.
  int64_t v = 0;
  CHECK(0 == ty_money_parse(buf, &f, &v));
  CHECK(v == 123456789);
}

// Verify the arithmetic and the words at the ends.
// 1. INT64_MAX + 1 is out of range; the quotient by -1 of INT64_MIN too.
// 2. A division by zero is TY_ERR_DIVZERO.
// 3. The words of $1,234.56 and of the smallest value.
TEST_CASE("money arithmetic and words") {
  int64_t r = 0;

  // 1. INT64_MAX + 1 is out of range; the quotient by -1 of INT64_MIN too.
  CHECK(-1 == ty_money_add(INT64_MAX, 1, &r));
  CHECK(TY_ERR_RANGE == ty_errnum());
  CHECK(-1 == ty_money_div_int64(INT64_MIN, -1, &r));
  CHECK(TY_ERR_RANGE == ty_errnum());

  // 2. A division by zero is TY_ERR_DIVZERO.
  CHECK(-1 == ty_money_div_int64(5, 0, &r));
  CHECK(TY_ERR_DIVZERO == ty_errnum());

  // 3. The words of $1,234.56 and of the smallest value.
  char buf[256];
  size_t n = ty_money_words(123456, buf);

  CHECK(std::string(buf, n) ==
        "One thousand two hundred thirty four dollars and fifty six cents");
  n = ty_money_words(INT64_MIN, buf);
  CHECK(std::string(buf, n).compare(0, 31, "Minus ninety two quadrillion tw") ==
        0);
}

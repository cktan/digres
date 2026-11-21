/*
 * Copyright (c) 2026 CK Tan
 * SPDX-License-Identifier: MIT
 */

/*
 * Tests of the calendar and the date, time and timestamp arithmetic; the
 * rest are in the vectors.
 */
#include <string>

#include "doctest.h"
#include "ty_datetime.h"

// Verify the calendar at the epochs.
// 1. 2000-01-01 is Julian day 2451545, a Saturday (6).
// 2. 1970-01-01 is Julian day 2440588, a Thursday (4).
// 3. The Julian day converts back to the date.
TEST_CASE("the calendar at the epochs") {
  int y, m, d;

  // 1. 2000-01-01 is Julian day 2451545, a Saturday (6).
  CHECK(2451545 == ty_date2j(2000, 1, 1));
  CHECK(6 == ty_j2day(2451545));

  // 2. 1970-01-01 is Julian day 2440588, a Thursday (4).
  CHECK(2440588 == ty_date2j(1970, 1, 1));
  CHECK(4 == ty_j2day(2440588));

  // 3. The Julian day converts back to the date.
  ty_j2date(2451545, &y, &m, &d);
  CHECK(y == 2000);
  CHECK(m == 1);
  CHECK(d == 1);
}

// Verify that a month added to the 31st stops at the end of the month.
// 1. 2000-01-31 + 1 month is 2000-02-29 (a leap year).
// 2. 2001-01-31 + 1 month is 2001-02-28.
TEST_CASE("timestamp plus months stops at the end of the month") {
  ty_interval_t month = {0, 0, 1};
  ty_tm_t tm = {2000, 1, 31, 12, 0, 0};
  int64_t ts, r;
  int32_t f;

  // 1. 2000-01-31 + 1 month is 2000-02-29 (a leap year).
  REQUIRE(0 == ty_tm_to_timestamp(&tm, 0, &ts));
  REQUIRE(0 == ty_timestamp_add_interval(ts, &month, &r));
  REQUIRE(0 == ty_timestamp_to_tm(r, &tm, &f));
  CHECK(tm.mon == 2);
  CHECK(tm.mday == 29);

  // 2. 2001-01-31 + 1 month is 2001-02-28.
  tm = {2001, 1, 31, 12, 0, 0};
  REQUIRE(0 == ty_tm_to_timestamp(&tm, 0, &ts));
  REQUIRE(0 == ty_timestamp_add_interval(ts, &month, &r));
  REQUIRE(0 == ty_timestamp_to_tm(r, &tm, &f));
  CHECK(tm.mday == 28);
}

// Verify the errors.
// 1. make_date of month 13 is a field error; of year 5874899, a range error.
// 2. date + days past the end is a range error.
// 3. The infinities of a date have no difference.
TEST_CASE("date and time errors") {
  int32_t d;

  // 1. make_date of month 13 is a field error; of year 5874899, a range
  //    error.
  CHECK(-1 == ty_date_make(2000, 13, 1, &d));
  CHECK(TY_ERR_INVALID == ty_errnum());
  CHECK(std::string(ty_errmsg()) ==
        "date field value out of range: 2000-13-01");
  CHECK(-1 == ty_date_make(5874899, 1, 1, &d));
  CHECK(TY_ERR_RANGE == ty_errnum());

  // 2. date + days past the end is a range error.
  CHECK(0 == ty_date_make(5874897, 12, 31, &d));
  int32_t r;
  CHECK(-1 == ty_date_add_days(d, 1, &r));
  CHECK(std::string(ty_errmsg()) == "date out of range");

  // 3. The infinities of a date have no difference.
  CHECK(-1 == ty_date_diff(TY_DATE_NOEND, 0, &r));
  CHECK(std::string(ty_errmsg()) == "cannot subtract infinite dates");
}

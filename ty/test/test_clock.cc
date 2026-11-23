/*
 * Copyright (c) 2026 CK Tan
 * SPDX-License-Identifier: MIT
 */

/*
 * Tests of ty_set_xact_clock() and ty_xact_start(): the pointer to the
 * transaction's start time that the host gives ty, per thread.
 */
#include <stdint.h>

#include <string>
#include <thread>

#include "doctest.h"
#include "ty.h"
#include "ty_timezone.h"

// Verify that a thread with no clock cannot read the start time.
// 1. Make a new thread, which has set no clock.
// 2. Read the start time: it fails with TY_ERR_INVALID.
TEST_CASE("ty_xact_start fails on a thread with no clock") {
  std::thread t([] {
    int64_t ts = 7;
    // 2. Read the start time: it fails with TY_ERR_INVALID.
    CHECK(-1 == ty_xact_start(&ts));
    CHECK(TY_ERR_INVALID == ty_errnum());
    CHECK(7 == ts);
  });
  t.join();
}

// Verify that ty reads the host's value at each call, and that a value of
// INT64_MIN and a NULL pointer both mean no transaction.
// 1. Set a clock whose value is INT64_MIN: the read fails.
// 2. Change the value, as a new transaction does: the read gives it.
// 3. Change the value again: the read gives the new one, with no call to ty.
// 4. Set the pointer to NULL: the read fails.
TEST_CASE("ty_xact_start reads the host's value at each call") {
  std::thread t([] {
    int64_t clock = INT64_MIN;
    int64_t ts = 0;
    // 1. Set a clock whose value is INT64_MIN: the read fails.
    ty_set_xact_clock(&clock);
    CHECK(-1 == ty_xact_start(&ts));
    // 2. Change the value, as a new transaction does: the read gives it.
    clock = 1234567890123456;
    CHECK(0 == ty_xact_start(&ts));
    CHECK(1234567890123456 == ts);
    // 3. Change the value again: the read gives the new one.
    clock = 42;
    CHECK(0 == ty_xact_start(&ts));
    CHECK(42 == ts);
    // 4. Set the pointer to NULL: the read fails.
    ty_set_xact_clock(nullptr);
    CHECK(-1 == ty_xact_start(&ts));
  });
  t.join();
}

// Verify that a clock set on one thread is not read on another.
// 1. Set a clock on this thread.
// 2. On a new thread, read the start time: it fails.
// 3. Clear the clock of this thread.
TEST_CASE("the clock of one thread is not the clock of another") {
  int64_t clock = 99;
  // 1. Set a clock on this thread.
  ty_set_xact_clock(&clock);
  // 2. On a new thread, read the start time: it fails.
  std::thread t([] {
    int64_t ts = 0;
    CHECK(-1 == ty_xact_start(&ts));
  });
  t.join();
  // 3. Clear the clock of this thread.
  ty_set_xact_clock(nullptr);
}

// Verify that ty gives the transaction's start time as local fields of the
// session's zone, and fails with no clock or no zone.
// 1. With a clock and no session zone, ty_xact_now fails with TY_ERR_INVALID.
// 2. Set the clock to 2024-07-04 12:34:56.789012 UTC and the zone to UTC:
//    the fields are those, and the offset is 0.
// 3. Set the zone to America/New_York: the fields are 08:34:56 in daylight
//    time, and the offset is 14400 seconds west.
// 4. Change the clock to 2024-01-15 03:05:06.000005 UTC: the fields are
//    22:05:06 on January 14 in standard time, and the offset is 18000.
// 5. Set the clock pointer to NULL: ty_xact_now fails.
TEST_CASE("ty_xact_now gives the transaction time in the session's zone") {
  std::thread t([] {
    int64_t clock = 773411696789012;
    ty_tm_t tm;
    int32_t fsec = 0;
    int west = 0;
    ty_set_xact_clock(&clock);
    // 1. With a clock and no session zone, ty_xact_now fails.
    CHECK(-1 == ty_xact_now(&tm, &fsec, &west));
    CHECK(TY_ERR_INVALID == ty_errnum());
    // 2. UTC.
    const ty_tz_t *utc = ty_tz_find("UTC");
    REQUIRE(nullptr != utc);
    ty_set_session_zone(utc);
    CHECK(utc == ty_session_zone());
    REQUIRE(0 == ty_xact_now(&tm, &fsec, &west));
    CHECK(2024 == tm.year);
    CHECK(7 == tm.mon);
    CHECK(4 == tm.mday);
    CHECK(12 == tm.hour);
    CHECK(34 == tm.min);
    CHECK(56 == tm.sec);
    CHECK(789012 == fsec);
    CHECK(0 == west);
    // 3. America/New_York, in daylight time.
    const ty_tz_t *ny = ty_tz_find("America/New_York");
    REQUIRE(nullptr != ny);
    ty_set_session_zone(ny);
    REQUIRE(0 == ty_xact_now(&tm, &fsec, &west));
    CHECK(8 == tm.hour);
    CHECK(34 == tm.min);
    CHECK(4 == tm.mday);
    CHECK(14400 == west);
    // 4. A winter instant: standard time, the day before.
    clock = 758603106000005;
    REQUIRE(0 == ty_xact_now(&tm, &fsec, nullptr));
    CHECK(2024 == tm.year);
    CHECK(1 == tm.mon);
    CHECK(14 == tm.mday);
    CHECK(22 == tm.hour);
    CHECK(5 == tm.min);
    CHECK(6 == tm.sec);
    CHECK(5 == fsec);
    REQUIRE(0 == ty_xact_now(&tm, &fsec, &west));
    CHECK(18000 == west);
    // 5. Set the clock pointer to NULL: ty_xact_now fails.
    ty_set_xact_clock(nullptr);
    CHECK(-1 == ty_xact_now(&tm, &fsec, &west));
    ty_set_session_zone(nullptr);
  });
  t.join();
}

// Verify CURRENT_DATE, CURRENT_TIME and LOCALTIME in the session's zone, the
// typmod, and that the cache follows the instant and the zone.
// 1. With a clock of 2024-07-04 12:34:56.789012 UTC and the zone UTC, the date
//    is 2024-07-04, the time of day is 12:34:56.789012, and the offset is 0.
// 2. A typmod of 2 rounds the time to 12:34:56.79; a typmod below 0 does not.
// 3. In America/New_York, the date and the time are those of 08:34:56 in
//    daylight time, and the offset is 14400.
// 4. A later instant gives a later time; the date moves at local midnight.
// 5. With no clock, each call fails.
TEST_CASE(
    "CURRENT_DATE, CURRENT_TIME and LOCALTIME read the transaction time") {
  std::thread t([] {
    int64_t clock = 773411696789012;
    int32_t d, want;
    ty_timetz_t tt;
    int64_t lt;
    ty_set_xact_clock(&clock);
    ty_set_session_zone(ty_tz_find("UTC"));
    // 1. UTC.
    REQUIRE(0 == ty_current_date(&d));
    REQUIRE(0 == ty_date_make(2024, 7, 4, &want));
    CHECK(want == d);
    REQUIRE(0 == ty_current_time(-1, &tt));
    CHECK(((12 * 60 + 34) * 60 + 56) * INT64_C(1000000) + 789012 == tt.time);
    CHECK(0 == tt.zone);
    REQUIRE(0 == ty_local_time(-1, &lt));
    CHECK(tt.time == lt);
    // 2. The typmod.
    REQUIRE(0 == ty_current_time(2, &tt));
    CHECK(((12 * 60 + 34) * 60 + 56) * INT64_C(1000000) + 790000 == tt.time);
    REQUIRE(0 == ty_local_time(0, &lt));
    CHECK(((12 * 60 + 34) * 60 + 57) * INT64_C(1000000) == lt);
    // 3. America/New_York.
    ty_set_session_zone(ty_tz_find("America/New_York"));
    REQUIRE(0 == ty_current_time(-1, &tt));
    CHECK(((8 * 60 + 34) * 60 + 56) * INT64_C(1000000) + 789012 == tt.time);
    CHECK(14400 == tt.zone);
    REQUIRE(0 == ty_current_date(&d));
    CHECK(want == d);
    // 4. 2024-07-05 03:30:00 UTC is 2024-07-04 23:30 in New York, the same
    //    date; 2024-07-05 04:30:00 UTC is 2024-07-05 00:30, the next date.
    clock = INT64_C(773465400000000);
    REQUIRE(0 == ty_current_date(&d));
    CHECK(want == d);
    REQUIRE(0 == ty_current_time(-1, &tt));
    CHECK((23 * 60 + 30) * INT64_C(60000000) == tt.time);
    clock = INT64_C(773469000000000);
    REQUIRE(0 == ty_current_date(&d));
    CHECK(want + 1 == d);
    REQUIRE(0 == ty_current_time(-1, &tt));
    CHECK(30 * INT64_C(60000000) == tt.time);
    // 5. With no clock, each call fails.
    ty_set_xact_clock(nullptr);
    CHECK(-1 == ty_current_date(&d));
    CHECK(-1 == ty_current_time(-1, &tt));
    CHECK(-1 == ty_local_time(-1, &lt));
    ty_set_session_zone(nullptr);
  });
  t.join();
}

// Verify time to timetz: the offset of the session's zone at the time of day,
// on today's date in that zone, across a daylight change.
// 1. In UTC, the offset is 0, also for 24:00:00.
// 2. In New York on 2024-07-04, 08:00 is 14400 west.
// 3. On 2024-01-14 (local), 08:00 is 18000 west.
// 4. On the day of the spring change (2024-03-10), 01:30 is 18000, 02:30 (a
//    time the change skips) is the offset before it, 18000, and 03:30 is 14400.
// 5. On the day of the autumn change (2024-11-03), 00:30 is 14400, 01:30 (a
//    time it repeats) is the offset after it, 18000, and 02:30 is 18000.
// 6. With no clock, it fails.
TEST_CASE("ty_time_to_timetz gives the offset of the zone at the time, today") {
  std::thread t([] {
    int64_t clock = 773411696789012;
    ty_timetz_t tt;
    auto at = [](int h, int m) {
      return ((int64_t)h * 60 + m) * INT64_C(60000000);
    };
    ty_set_xact_clock(&clock);
    // 1. UTC.
    ty_set_session_zone(ty_tz_find("UTC"));
    REQUIRE(0 == ty_time_to_timetz(at(12, 0), &tt));
    CHECK(at(12, 0) == tt.time);
    CHECK(0 == tt.zone);
    REQUIRE(0 == ty_time_to_timetz(at(24, 0), &tt));
    CHECK(at(24, 0) == tt.time);
    CHECK(0 == tt.zone);
    // 2. New York in July.
    ty_set_session_zone(ty_tz_find("America/New_York"));
    REQUIRE(0 == ty_time_to_timetz(at(8, 0), &tt));
    CHECK(14400 == tt.zone);
    // 3. New York in January.
    clock = 758603106000005;
    REQUIRE(0 == ty_time_to_timetz(at(8, 0), &tt));
    CHECK(18000 == tt.zone);
    // 4. The spring change.
    clock = INT64_C(763398000000000);
    REQUIRE(0 == ty_time_to_timetz(at(1, 30), &tt));
    CHECK(18000 == tt.zone);
    REQUIRE(0 == ty_time_to_timetz(at(2, 30), &tt));
    CHECK(18000 == tt.zone);
    REQUIRE(0 == ty_time_to_timetz(at(3, 30), &tt));
    CHECK(14400 == tt.zone);
    // 5. The autumn change.
    clock = INT64_C(783961200000000);
    REQUIRE(0 == ty_time_to_timetz(at(0, 30), &tt));
    CHECK(14400 == tt.zone);
    REQUIRE(0 == ty_time_to_timetz(at(1, 30), &tt));
    CHECK(18000 == tt.zone);
    REQUIRE(0 == ty_time_to_timetz(at(2, 30), &tt));
    CHECK(18000 == tt.zone);
    // 6. With no clock, it fails.
    ty_set_xact_clock(nullptr);
    CHECK(-1 == ty_time_to_timetz(at(8, 0), &tt));
    ty_set_session_zone(nullptr);
  });
  t.join();
}

// Verify timestamptz to timetz, and date and timetz to timestamptz.
// 1. The local time of an instant in New York and in UTC, with the offset.
// 2. A date and a timetz make the instant back.
// 3. An infinite date is the matching infinity.
// 4. The date 294277-01-01 and a zone that takes the result past the end fail
//    with "date out of range for timestamp".
TEST_CASE("timestamptz to timetz and date and timetz to timestamptz") {
  ty_timetz_t tt;
  int64_t ts = 0;
  int32_t d;
  const int64_t instant = 773411696789012; /* 2024-07-04 12:34:56.789012 UTC */

  // 1. The local time of an instant.
  REQUIRE(0 == ty_timestamptz_to_timetz(ty_tz_find("UTC"), instant, &tt));
  CHECK(((12 * 60 + 34) * 60 + 56) * INT64_C(1000000) + 789012 == tt.time);
  CHECK(0 == tt.zone);
  REQUIRE(0 == ty_timestamptz_to_timetz(ty_tz_find("America/New_York"), instant,
                                        &tt));
  CHECK(((8 * 60 + 34) * 60 + 56) * INT64_C(1000000) + 789012 == tt.time);
  CHECK(14400 == tt.zone);
  // 2. A date and a timetz make the instant back.
  REQUIRE(0 == ty_date_make(2024, 7, 4, &d));
  REQUIRE(0 == ty_datetimetz_to_timestamptz(d, &tt, &ts));
  CHECK(instant == ts);
  // 3. An infinite date is the matching infinity.
  REQUIRE(0 == ty_datetimetz_to_timestamptz(TY_DATE_NOBEGIN, &tt, &ts));
  CHECK(TY_TIMESTAMP_NOBEGIN == ts);
  REQUIRE(0 == ty_datetimetz_to_timestamptz(TY_DATE_NOEND, &tt, &ts));
  CHECK(TY_TIMESTAMP_NOEND == ts);
  // 4. Out of range.
  CHECK(-1 == ty_datetimetz_to_timestamptz(
                  TY_TIMESTAMP_END_JULIAN - TY_POSTGRES_EPOCH_JDATE, &tt, &ts));
  CHECK(TY_ERR_RANGE == ty_errnum());
  CHECK(std::string("date out of range for timestamp") == ty_errmsg());
  ty_timetz_t late = {INT64_C(86399999999), 43200};
  CHECK(-1 ==
        ty_datetimetz_to_timestamptz(
            TY_TIMESTAMP_END_JULIAN - TY_POSTGRES_EPOCH_JDATE - 1, &late, &ts));
  CHECK(TY_ERR_RANGE == ty_errnum());
}

// Verify timetz AT TIME ZONE: a fixed offset, a zone at the start of the
// transaction, and a dynamic abbreviation.
// 1. The same instant at another offset: 04:05:06+02 at 5 hours west is
//    21:05:06, and the time wraps round the day both ways.
// 2. At a zone: New York in July is 4 hours west, in January 5.
// 3. At a dynamic abbreviation: "EST" in New York is 5 hours west even in
//    July, "EDT" 4 hours in January, and one the zone never used takes the
//    zone's own offset then.
// 4. With no clock, the zone and the abbreviation fail; the offset does not.
TEST_CASE("timetz AT TIME ZONE moves the time by the offsets") {
  std::thread t([] {
    ty_timetz_t in, out;
    int64_t clock = 773411696789012; /* 2024-07-04 12:34:56.789012 UTC */
    auto at = [](int h, int m, int s) {
      return (((int64_t)h * 60 + m) * 60 + s) * INT64_C(1000000);
    };
    const ty_tz_t *ny = ty_tz_find("America/New_York");
    ty_set_xact_clock(&clock);
    // 1. A fixed offset.
    in = {at(4, 5, 6), -7200};
    REQUIRE(0 == ty_timetz_at_offset(&in, 18000, &out));
    CHECK(at(21, 5, 6) == out.time);
    CHECK(18000 == out.zone);
    in = {at(23, 0, 0), 18000};
    REQUIRE(0 == ty_timetz_at_offset(&in, -3600, &out));
    CHECK(at(5, 0, 0) == out.time);
    CHECK(-3600 == out.zone);
    // 2. A zone, at the start of the transaction.
    in = {at(12, 0, 0), 0};
    REQUIRE(0 == ty_timetz_at_zone(&in, ny, &out));
    CHECK(at(8, 0, 0) == out.time);
    CHECK(14400 == out.zone);
    clock = 758603106000005; /* 2024-01-15 03:05:06 UTC */
    REQUIRE(0 == ty_timetz_at_zone(&in, ny, &out));
    CHECK(at(7, 0, 0) == out.time);
    CHECK(18000 == out.zone);
    // 3. A dynamic abbreviation.
    clock = 773411696789012;
    REQUIRE(0 == ty_timetz_at_abbrev(&in, ny, "EST", &out));
    CHECK(at(7, 0, 0) == out.time);
    CHECK(18000 == out.zone);
    clock = 758603106000005;
    REQUIRE(0 == ty_timetz_at_abbrev(&in, ny, "EDT", &out));
    CHECK(at(8, 0, 0) == out.time);
    CHECK(14400 == out.zone);
    clock = 773411696789012;
    REQUIRE(0 == ty_timetz_at_abbrev(&in, ny, "XYZ", &out));
    CHECK(14400 == out.zone);
    // 4. With no clock.
    ty_set_xact_clock(nullptr);
    CHECK(-1 == ty_timetz_at_zone(&in, ny, &out));
    CHECK(-1 == ty_timetz_at_abbrev(&in, ny, "EST", &out));
    CHECK(0 == ty_timetz_at_offset(&in, 3600, &out));
  });
  t.join();
}

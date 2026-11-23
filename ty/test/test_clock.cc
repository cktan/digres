/*
 * Copyright (c) 2026 CK Tan
 * SPDX-License-Identifier: MIT
 */

/*
 * Tests of ty_set_xact_clock() and ty_xact_start(): the pointer to the
 * transaction's start time that the host gives ty, per thread.
 */
#include <stdint.h>

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

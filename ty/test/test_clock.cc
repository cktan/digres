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

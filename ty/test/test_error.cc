/*
 * Copyright (c) 2026 CK Tan
 * SPDX-License-Identifier: MIT
 */

/*
 * Tests of the error state: ty_errnum(), ty_errmsg(), ty_erridx(),
 * ty_errname().
 */
#include <string.h>

#include <string>
#include <thread>

#include "doctest.h"
#include "error.h"
#include "ty.h"

// Verify that the error state is per thread, and a message is kept.
// 1. Set an error on this thread.
// 2. Read it back here, and on a second thread read none.
TEST_CASE("the error state belongs to one thread") {
  // 1. Set an error on this thread.
  CHECK(-1 == ty_internal::set_error(TY_ERR_NOMEM, "out of memory"));
  // 2. Read it back here, and on a second thread read none.
  CHECK(TY_ERR_NOMEM == ty_errnum());
  CHECK(0 == strcmp("out of memory", ty_errmsg()));
  ty_errnum_t other = TY_ERR_LAST;
  std::thread t([&] { other = ty_errnum(); });
  t.join();
  CHECK(TY_OK == other);
}

// Verify that an error's row is kept, and belongs to one thread.
// 1. Set an error of row 7, and read the row here, and on a second thread
//    read 0.
// 2. Set an error of no row: the row is 0.
TEST_CASE("an error has a row") {
  // 1. Set an error of row 7, and read it on two threads.
  CHECK(-1 == ty_internal::set_error(TY_ERR_RANGE, "out of range", 7));
  CHECK(7 == ty_erridx());
  uint32_t other = 1;
  std::thread t([&] { other = ty_erridx(); });
  t.join();
  CHECK(0 == other);
  // 2. Set an error of no row.
  ty_err_range();
  CHECK(0 == ty_erridx());
}

// Verify that a message too long is cut, and the code has a name.
// 1. Set an error whose message is longer than the buffer.
// 2. Check the message is cut and ends in a NUL.
// 3. Check the names of two codes.
TEST_CASE("a long message is cut, and codes have names") {
  // 1. Set an error whose message is longer than the buffer.
  std::string big(1000, 'x');
  ty_internal::set_error(TY_ERR_INVALID, big.c_str());
  // 2. Check the message is cut and ends in a NUL.
  CHECK(strlen(ty_errmsg()) < big.size());
  CHECK(TY_ERR_INVALID == ty_errnum());
  // 3. Check the names of two codes.
  CHECK(0 == strcmp("TY_OK", ty_errname(TY_OK)));
  CHECK(0 == strcmp("TY_ERR_NOMEM", ty_errname(TY_ERR_NOMEM)));
}

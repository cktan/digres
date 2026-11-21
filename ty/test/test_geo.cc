/*
 * Copyright (c) 2026 CK Tan
 * SPDX-License-Identifier: MIT
 */

#include <string.h>

#include <vector>

#include "doctest.h"
#include "ty.h"
#include "ty_geo.h"

namespace {

int stop_now() { return 1; }
int never_stop() { return 0; }

} // namespace

// Verify that the poll function of a long polygon test can stop it.
// 1. Make two squares, one in the other, and their bounding boxes.
// 2. Test contain with no poll function, and with one that never stops: true.
// 3. Test contain with a poll function that stops: -1, and TY_ERR_ABORTED.
TEST_CASE("a poll function stops the test that a polygon contains another") {
  // 1. Make two squares, one in the other, and their bounding boxes.
  std::vector<ty_point_t> big = {{0, 0}, {4, 0}, {4, 4}, {0, 4}};
  std::vector<ty_point_t> small = {{1, 1}, {2, 1}, {2, 2}, {1, 2}};
  ty_box_t bb, sb;
  int r = 0;

  ty_polygon_bound_box(4, big.data(), &bb);
  ty_polygon_bound_box(4, small.data(), &sb);
  // 2. Test contain with no poll function, and with one that never stops.
  CHECK(0 == ty_polygon_contain(4, big.data(), &bb, 4, small.data(), &sb,
                                nullptr, &r));
  CHECK(1 == r);
  CHECK(0 == ty_polygon_contain(4, big.data(), &bb, 4, small.data(), &sb,
                                never_stop, &r));
  CHECK(1 == r);
  // 3. Test contain with a poll function that stops.
  CHECK(-1 == ty_polygon_contain(4, big.data(), &bb, 4, small.data(), &sb,
                                 stop_now, &r));
  CHECK(TY_ERR_ABORTED == ty_errnum());
  CHECK(0 == strcmp("TY_ERR_ABORTED", ty_errname(TY_ERR_ABORTED)));
}

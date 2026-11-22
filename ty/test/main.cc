/*
 * Copyright (c) 2026 CK Tan
 * SPDX-License-Identifier: MIT
 */

/*
 * main.cc -- the doctest runner. Every other .cc file in test/
 * contributes test cases; this one supplies main().
 *
 * The test of ty_tz_use_data (test_timezone.cc) needs a process in which no
 * zone has been found: it runs this program again with --tz-use-data-child.
 */

#define DOCTEST_CONFIG_IMPLEMENT
#include "doctest.h"
#include "doctest_active.h"

#include <string.h>

int tz_use_data_child(const char *path);

int main(int argc, char **argv) {
  // A child of the test of ty_tz_use_data: not a test run.
  if (argc == 3 && strcmp(argv[1], "--tz-use-data-child") == 0) {
    return tz_use_data_child(argv[2]);
  }
  doctest::Context ctx;

  ctx.applyCommandLine(argc, argv);
  return ctx.run();
}

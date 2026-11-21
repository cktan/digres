/*
 * Copyright (c) 2026 CK Tan
 * SPDX-License-Identifier: MIT
 */

/*
 * Tests of the boolean operations.
 */
#include <initializer_list>

#include <string.h>

#include "doctest.h"
#include "ty_bool.h"

// Verify the boolean parse takes the words, cut short, in any case.
// 1. "true", "YES", "On" and "1" are true; "false", "no", "OFF", "0" false.
// 2. A cut-short word is accepted: "t", "fals", "ye", "of".
// 3. "o" and "" are not, and "truee" and "10" are not.
// 4. A space is not skipped: " t" fails, and so does "t ".
// 5. The text need not end in NUL: only len bytes are read.
TEST_CASE("the boolean parse takes the words of true and false") {
  bool b = false;

  // 1. "true", "YES", "On" and "1" are true; "false", "no", "OFF", "0" false.
  for (const char *s : {"true", "YES", "On", "1"}) {
    CHECK(0 == ty_bool_parse(s, strlen(s), &b));
    CHECK(b);
  }
  for (const char *s : {"false", "no", "OFF", "0"}) {
    CHECK(0 == ty_bool_parse(s, strlen(s), &b));
    CHECK(!b);
  }

  // 2. A cut-short word is accepted: "t", "fals", "ye", "of".
  CHECK(0 == ty_bool_parse("t", 1, &b));
  CHECK(b);
  CHECK(0 == ty_bool_parse("fals", 4, &b));
  CHECK(!b);
  CHECK(0 == ty_bool_parse("ye", 2, &b));
  CHECK(b);
  CHECK(0 == ty_bool_parse("of", 2, &b));
  CHECK(!b);

  // 3. "o" and "" are not, and "truee" and "10" are not.
  for (const char *s : {"o", "", "truee", "10"}) {
    CHECK(-1 == ty_bool_parse(s, strlen(s), &b));
    CHECK(TY_ERR_INVALID == ty_errnum());
  }

  // 4. A space is not skipped: " t" fails, and so does "t ".
  CHECK(-1 == ty_bool_parse(" t", 2, &b));
  CHECK(-1 == ty_bool_parse("t ", 2, &b));

  // 5. The text need not end in NUL: only len bytes are read.
  CHECK(0 == ty_bool_parse("tXYZ", 1, &b));
  CHECK(b);
}

/*
 * Copyright (c) 2026 CK Tan
 * SPDX-License-Identifier: MIT
 */

/*
 * Tests of the boolean operations.
 */
#include <initializer_list>

#include <string.h>

#include <string>

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

// Verify the boolean input skips the space at both ends, and the text of
// a bool, its compare and its logic.
// 1. " yes ", a tab and newline around "f", and an all-space text.
// 2. The output is "t" and "f"; the cast to text is "true" and "false".
// 3. false is below true, in the compare and in each predicate.
// 4. AND, OR and the casts with int32.
TEST_CASE("the boolean input skips space, and a bool has text, order, logic") {
  bool b = false;

  // 1. " yes ", a tab and newline around "f", and an all-space text.
  CHECK(0 == ty_bool_parse_text(" yes ", 5, &b));
  CHECK(b);
  CHECK(0 == ty_bool_parse_text("\t\nf\n", 4, &b));
  CHECK(!b);
  CHECK(-1 == ty_bool_parse_text("   ", 3, &b));
  CHECK(TY_ERR_INVALID == ty_errnum());
  CHECK(-1 == ty_bool_parse_text("t t", 3, &b));

  // 2. The output is "t" and "f"; the cast to text is "true" and "false".
  CHECK(std::string(ty_bool_out(true)) == "t");
  CHECK(std::string(ty_bool_out(false)) == "f");
  CHECK(std::string(ty_bool_text(true)) == "true");
  CHECK(std::string(ty_bool_text(false)) == "false");

  // 3. false is below true, in the compare and in each predicate.
  CHECK(ty_bool_cmp(false, true) == -1);
  CHECK(ty_bool_cmp(true, true) == 0);
  CHECK(ty_bool_cmp(true, false) == 1);
  CHECK(ty_bool_lt(false, true));
  CHECK(!ty_bool_lt(true, true));
  CHECK(ty_bool_le(true, true));
  CHECK(!ty_bool_gt(false, true));
  CHECK(ty_bool_gt(true, false));
  CHECK(ty_bool_ge(true, true));
  CHECK(ty_bool_eq(true, true));
  CHECK(ty_bool_ne(true, false));

  // 4. AND, OR and the casts with int32.
  CHECK(ty_bool_and(true, true));
  CHECK(!ty_bool_and(true, false));
  CHECK(ty_bool_or(false, true));
  CHECK(!ty_bool_or(false, false));
  CHECK(ty_bool_from_int32(-5));
  CHECK(!ty_bool_from_int32(0));
  CHECK(ty_bool_to_int32(true) == 1);
  CHECK(ty_bool_to_int32(false) == 0);
}

// Verify the state of the aggregates EVERY and ANY: values enter and leave it,
// and the result is NULL while no value is in it.
// 1. An empty state gives NULL for both.
// 2. With true, true: every is true and any is true.
// 3. Add false: every is false, any is true.
// 4. Remove the false: every is true again (the moving frame).
// 5. Remove the two true: the state is empty again, and NULL.
// 6. A state of false alone: every is false, and any is false.
TEST_CASE("the bool aggregate state accumulates and takes values out") {
  ty_bool_agg_t st = {};
  bool r = true;

  // 1. An empty state gives NULL for both.
  CHECK(0 == ty_bool_agg_every(&st, &r));
  CHECK(0 == ty_bool_agg_any(&st, &r));
  // 2. With true, true.
  ty_bool_agg_accum(&st, true);
  ty_bool_agg_accum(&st, true);
  CHECK(1 == ty_bool_agg_every(&st, &r));
  CHECK(r);
  CHECK(1 == ty_bool_agg_any(&st, &r));
  CHECK(r);
  // 3. Add false.
  ty_bool_agg_accum(&st, false);
  CHECK(1 == ty_bool_agg_every(&st, &r));
  CHECK(!r);
  CHECK(1 == ty_bool_agg_any(&st, &r));
  CHECK(r);
  CHECK(3 == st.count);
  CHECK(2 == st.ntrue);
  // 4. Remove the false.
  ty_bool_agg_accum_inv(&st, false);
  CHECK(1 == ty_bool_agg_every(&st, &r));
  CHECK(r);
  // 5. Remove the two true.
  ty_bool_agg_accum_inv(&st, true);
  ty_bool_agg_accum_inv(&st, true);
  CHECK(0 == ty_bool_agg_every(&st, &r));
  CHECK(0 == ty_bool_agg_any(&st, &r));
  // 6. A state of false alone.
  ty_bool_agg_accum(&st, false);
  CHECK(1 == ty_bool_agg_every(&st, &r));
  CHECK(!r);
  CHECK(1 == ty_bool_agg_any(&st, &r));
  CHECK(!r);
}

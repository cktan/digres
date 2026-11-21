/*
 * Copyright (c) 2026 CK Tan
 * SPDX-License-Identifier: MIT
 */

/*
 * Tests of tid, pg_lsn and "char"; the text is also in test/known.txt.
 */
#include <string.h>

#include <string>

#include "doctest.h"
#include "ty_small.h"

// Verify the tid layout, order and text.
// 1. The struct is 6 bytes, the block high half first.
// 2. The order is the block then the offset.
// 3. (4294967295,65535) parses and formats back.
TEST_CASE("tid layout, order and text") {
  // 1. The struct is 6 bytes, the block high half first.
  CHECK(6 == sizeof(ty_tid_t));
  ty_tid_t a, b, c;

  ty_tid_set(&a, 0x00010002, 3);
  CHECK(a.block_hi == 1);
  CHECK(a.block_lo == 2);
  CHECK(ty_tid_block(&a) == 0x00010002);

  // 2. The order is the block then the offset.
  ty_tid_set(&b, 0x00010002, 4);
  ty_tid_set(&c, 0x00010003, 0);
  CHECK(ty_tid_cmp(&a, &b) < 0);
  CHECK(ty_tid_cmp(&b, &c) < 0);
  CHECK(ty_tid_cmp(&a, &a) == 0);

  // 3. (4294967295,65535) parses and formats back.
  char buf[TY_TID_TEXT_MAX];
  ty_tid_t d;

  CHECK(0 == ty_tid_parse("(4294967295,65535)", &d));
  ty_tid_format(&d, buf);
  CHECK(std::string(buf) == "(4294967295,65535)");
  CHECK(-1 == ty_tid_parse("(1,65536)", &d));
  CHECK(TY_ERR_INVALID == ty_errnum());
}

// Verify the pg_lsn text.
// 1. "16/B374D848" is 0x16B374D848.
// 2. It is written back in upper case.
// 3. Nine digits are an error.
TEST_CASE("pg_lsn text") {
  uint64_t v = 0;
  char buf[TY_LSN_TEXT_MAX];

  // 1. "16/B374D848" is 0x16B374D848.
  CHECK(0 == ty_lsn_parse("16/B374D848", &v));
  CHECK(v == 0x16B374D848ULL);

  // 2. It is written back in upper case.
  ty_lsn_format(0x0000000A0000000BULL, buf);
  CHECK(std::string(buf) == "A/B");
  ty_lsn_format(0, buf);
  CHECK(std::string(buf) == "0/0");

  // 3. Nine digits are an error.
  CHECK(-1 == ty_lsn_parse("123456789/1", &v));
  CHECK(TY_ERR_INVALID == ty_errnum());
}

// Verify "char": octal escapes, the unsigned order, the signed integer.
// 1. "\101" is 'A'; the first byte of "ab" is 'a'; "" is 0.
// 2. 0x80 and above are written as three octal digits.
// 3. 0x80 is above 0x7F as "char", but -128 as an integer.
// 4. 200 does not fit.
TEST_CASE("char text, order and range") {
  char buf[8];

  // 1. "\101" is 'A'; the first byte of "ab" is 'a'; "" is 0.
  CHECK(ty_char_parse("\\101", 4) == 'A');
  CHECK(ty_char_parse("ab", 2) == 'a');
  CHECK(ty_char_parse("", 0) == 0);

  // 2. 0x80 and above are written as three octal digits.
  CHECK(4 == ty_char_format((int8_t)0xFF, buf));
  CHECK(std::string(buf) == "\\377");
  CHECK(0 == ty_char_format(0, buf));

  // 3. 0x80 is above 0x7F as "char", but -128 as an integer.
  CHECK(ty_char_cmp((int8_t)0x80, 0x7F) > 0);
  CHECK((int32_t)(int8_t)0x80 == -128);

  // 4. 200 does not fit.
  int8_t r = 0;
  CHECK(-1 == ty_char_from_int32(200, &r));
  CHECK(TY_ERR_RANGE == ty_errnum());
  CHECK(0 == ty_char_from_int32(-128, &r));
  CHECK(r == -128);
}

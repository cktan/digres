/*
 * Copyright (c) 2026 CK Tan
 * SPDX-License-Identifier: MIT
 */

/*
 * Tests of the bytea operations.
 */
#include <string.h>

#include <string>
#include <vector>

#include "doctest.h"
#include "ty_bytea.h"
#include "vl.h"

namespace {

/* A bytea of the given bytes. */
vl_t bv(const std::vector<uint8_t> &b) {
  return vl_t(std::string(b.begin(), b.end()));
}

/* The bytes of a varlena. */
std::vector<uint8_t> bytes(const ty_varlena_t *v) {
  std::string t = vl_str(v);

  return std::vector<uint8_t>(t.begin(), t.end());
}

/* Parse text into bytes; the return of ty_bytea_parse. */
int parse(const std::string &text, std::vector<uint8_t> *out) {
  vl_out_t r;
  int rc = ty_bytea_parse(text.data(), text.size(), &r);

  if (rc == 0) {
    *out = bytes(r.p);
  } else {
    out->clear();
  }
  return rc;
}

} // namespace

// Verify the text forms: hex with space between pairs, and escaped.
// 1. Hex text reads in any case, and skips a space, tab, newline or return
//    between pairs.
// 2. A space inside a pair, or an odd digit, or a bad digit, fails with a
//    message that names the fault.
// 3. Escaped text reads a byte, an octal, and a doubled backslash; a stray
//    backslash and an octal past 377 fail.
// 4. Writing then reading gives the bytes back, in both forms, for every
//    byte.
TEST_CASE("the text forms of a bytea are read and written") {
  std::vector<uint8_t> b;

  // 1. Hex text reads in any case, and skips a space, tab, newline or return
  //    between pairs.
  CHECK(0 == parse("\\xAbCd", &b));
  CHECK(b == std::vector<uint8_t>{0xab, 0xcd});
  CHECK(0 == parse("\\x61 62\t63\n64\r65", &b));
  CHECK(b == std::vector<uint8_t>{0x61, 0x62, 0x63, 0x64, 0x65});
  CHECK(0 == parse("\\x", &b));
  CHECK(b.empty());

  // 2. A space inside a pair, or an odd digit, or a bad digit, fails with a
  //    message that names the fault.
  CHECK(-1 == parse("\\x6 1", &b));
  CHECK(std::string(ty_errmsg()) == "invalid hexadecimal digit: \" \"");
  CHECK(-1 == parse("\\x61 6", &b));
  CHECK(std::string(ty_errmsg()) ==
        "invalid hexadecimal data: odd number of digits");
  CHECK(-1 == parse("\\x6g", &b));
  CHECK(std::string(ty_errmsg()) == "invalid hexadecimal digit: \"g\"");
  CHECK(TY_ERR_INVALID == ty_errnum());
  CHECK(-1 == parse("\\x\xc3\xa9", &b));
  CHECK(std::string(ty_errmsg()) == "invalid hexadecimal digit: \"\xc3\xa9\"");

  // 3. Escaped text reads a byte, an octal, and a doubled backslash; a stray
  //    backslash and an octal past 377 fail.
  CHECK(0 == parse("a\\141\\\\\\377", &b));
  CHECK(b == std::vector<uint8_t>{'a', 'a', '\\', 0xff});
  for (const char *bad : {"\\", "a\\", "\\1", "\\12", "\\400", "\\8",
                          "\\x"
                          "g"}) {
    CHECK(-1 == parse(bad, &b));
    CHECK(TY_ERR_INVALID == ty_errnum());
  }

  // 4. Writing then reading gives the bytes back, in both forms, for every
  //    byte.
  std::vector<uint8_t> all;
  for (int i = 0; i < 256; i++) {
    all.push_back((uint8_t)i);
  }
  vl_t allv = bv(all);
  std::string hex(ty_bytea_hex_size(allv), '\0');
  ty_bytea_format_hex(allv, &hex[0]);
  CHECK(0 == parse(hex, &b));
  CHECK(b == all);
  std::string esc(ty_bytea_escape_size(allv), '\0');
  ty_bytea_format_escape(allv, &esc[0]);
  CHECK(0 == parse(esc, &b));
  CHECK(b == all);
}

// Verify the escaped form's choices: a space is itself, a backslash two
// characters, a control byte an octal of three digits.
// 1. Write 'a', space, backslash, 0x01 and 0x7f.
// 2. The length said before is the length written.
TEST_CASE("the escaped form writes printable bytes as themselves") {
  vl_t data = bv({'a', ' ', '\\', 0x01, 0x7f});
  size_t size = ty_bytea_escape_size(data);
  std::string out(size, '\0');

  // 1. Write 'a', space, backslash, 0x01 and 0x7f.
  ty_bytea_format_escape(data, &out[0]);
  CHECK(out == "a \\\\\\001\\177");

  // 2. The length said before is the length written.
  CHECK(size == out.size());
}

// Verify that a substring and an overlay give PostgreSQL's parts.
// 1. FROM 2 for 'abcde': from the second byte on.
// 2. FROM -1 FOR 4: the part ends before 3, so two bytes.
// 3. FROM 4 FOR 100 runs to the end; the length need not be clamped to the
//    bytea.
// 4. A negative length is TY_ERR_INVALID.
// 5. Overlay 'abcde' with 'XY' FROM 2 FOR 2 is 'aXYde'.
// 6. Overlay FROM 0 is TY_ERR_INVALID; FROM 2 FOR int32 max is
//    TY_ERR_RANGE.
TEST_CASE("a substring and an overlay name the part as SQL does") {
  int32_t off, count;

  // 1. FROM 2 for 'abcde': from the second byte on.
  CHECK(0 == ty_bytea_substr_range(2, 0, 0, &off, &count));
  CHECK(1 == off);
  CHECK(-1 == count);

  // 2. FROM -1 FOR 4: the part ends before 3, so two bytes.
  CHECK(0 == ty_bytea_substr_range(-1, 4, 1, &off, &count));
  CHECK(0 == off);
  CHECK(2 == count);

  // 3. FROM 4 FOR 100 runs to the end; the length need not be clamped to the
  //    bytea.
  CHECK(0 == ty_bytea_substr_range(4, 100, 1, &off, &count));
  CHECK(3 == off);
  CHECK(100 == count);

  // 4. A negative length is TY_ERR_INVALID.
  CHECK(-1 == ty_bytea_substr_range(1, -1, 1, &off, &count));
  CHECK(TY_ERR_INVALID == ty_errnum());

  // 5. Overlay 'abcde' with 'XY' FROM 2 FOR 2 is 'aXYde'.
  vl_t a("abcde"), b("XY");
  vl_out_t out;
  CHECK(0 == ty_bytea_overlay(a, b, 2, 2, &out));
  CHECK(out.str() == "aXYde");

  // 6. Overlay FROM 0 is TY_ERR_INVALID; FROM 2 FOR int32 max is
  //    TY_ERR_RANGE.
  vl_out_t bad;
  CHECK(-1 == ty_bytea_overlay(a, b, 0, 1, &bad));
  CHECK(TY_ERR_INVALID == ty_errnum());
  CHECK(-1 == ty_bytea_overlay(a, b, 2, INT32_MAX, &bad));
  CHECK(TY_ERR_RANGE == ty_errnum());
}

// Verify bits and bytes: the order of bits, the messages, and no change on
// an error.
// 1. Bit 0 is the least significant bit of byte 0; bit 8 is byte 1's.
// 2. Setting a bit changes only it; a bit of 2 fails after the index check.
// 3. An index outside gives PostgreSQL's message with the valid range.
TEST_CASE("bits count from the least significant bit of byte 0") {
  vl_t a = bv({0x01, 0x80});
  int32_t r = 0;

  // 1. Bit 0 is the least significant bit of byte 0; bit 8 is byte 1's.
  CHECK(0 == ty_bytea_get_bit(a, 0, &r));
  CHECK(1 == r);
  CHECK(0 == ty_bytea_get_bit(a, 8, &r));
  CHECK(0 == r);
  CHECK(0 == ty_bytea_get_bit(a, 15, &r));
  CHECK(1 == r);

  // 2. Setting a bit changes only it, in a copy; a bit of 2 fails after the
  //    index check.
  vl_out_t c1;
  CHECK(0 == ty_bytea_set_bit(a, 1, 1, &c1));
  CHECK(bytes(c1.p) == std::vector<uint8_t>{0x03, 0x80});
  CHECK(bytes(a) == std::vector<uint8_t>{0x01, 0x80});
  vl_out_t c2;
  CHECK(0 == ty_bytea_set_bit(c1.p, 0, 0, &c2));
  CHECK(bytes(c2.p) == std::vector<uint8_t>{0x02, 0x80});
  vl_out_t bad;
  CHECK(-1 == ty_bytea_set_bit(a, 1, 2, &bad));
  CHECK(TY_ERR_INVALID == ty_errnum());
  CHECK(-1 == ty_bytea_set_bit(a, 16, 2, &bad));
  CHECK(TY_ERR_RANGE == ty_errnum());

  // 3. An index outside gives PostgreSQL's message with the valid range.
  CHECK(-1 == ty_bytea_get_bit(a, 16, &r));
  CHECK(std::string(ty_errmsg()) == "index 16 out of valid range, 0..15");
  CHECK(-1 == ty_bytea_get_byte(vl_t(""), 0, &r));
  CHECK(std::string(ty_errmsg()) == "index 0 out of valid range, 0..-1");
}

// Verify the integers: a short bytea is an unsigned number of its own
// width in the type, and a long one is an error.
// 1. Two bytes 0xFFFF are -1 as an int16 and 65535 as an int32.
// 2. Nine bytes do not fit an int64.
// 3. An integer and its bytea convert back and forth.
TEST_CASE("a bytea and an integer convert by their bytes") {
  vl_t ff2 = bv({0xff, 0xff}), nine = bv({1, 2, 3, 4, 5, 6, 7, 8, 9});
  int16_t r16 = 0;
  int32_t r32 = 0;
  int64_t r64 = 0;

  // 1. Two bytes 0xFFFF are -1 as an int16 and 65535 as an int32.
  CHECK(0 == ty_bytea_to_int16(ff2, &r16));
  CHECK(-1 == r16);
  CHECK(0 == ty_bytea_to_int32(ff2, &r32));
  CHECK(65535 == r32);

  // 2. Nine bytes do not fit an int64.
  CHECK(-1 == ty_bytea_to_int64(nine, &r64));
  CHECK(TY_ERR_RANGE == ty_errnum());

  // 3. An integer and its bytea convert back and forth.
  vl_out_t out;
  REQUIRE(0 == ty_int64_to_bytea(INT64_MIN, &out));
  CHECK(bytes(out.p)[0] == 0x80);
  CHECK(0 == ty_bytea_to_int64(out.p, &r64));
  CHECK(INT64_MIN == r64);
}

// Verify the hash and the compare.
// 1. Equal bytes hash alike, in either form of header; one more byte, or a
//    changed one, or another seed, hashes otherwise; zeros at the end count.
// 2. A prefix is less than what it begins; the compare is by bytes first.
TEST_CASE("the bytea hash and compare use every byte") {
  vl_t a = bv({1, 2, 3, 0}), b = bv({1, 2, 3}), c = bv({1, 2, 4});
  vl_t blong(std::string("\1\2\3", 3), true), empty = bv({});

  // 1. Equal bytes hash alike, in either form of header; one more byte, or a
  //    changed one, or another seed, hashes otherwise; zeros at the end
  //    count.
  CHECK(ty_bytea_hash(b, 0) == ty_bytea_hash(blong, 0));
  CHECK(ty_bytea_hash(b, 0) != ty_bytea_hash(a, 0));
  CHECK(ty_bytea_hash(b, 0) != ty_bytea_hash(c, 0));
  CHECK(ty_bytea_hash(b, 0) != ty_bytea_hash(b, 1));
  CHECK(ty_bytea_hash(empty, 0) != ty_bytea_hash(bv({0}), 0));

  // 2. A prefix is less than what it begins; the compare is by bytes first.
  CHECK(ty_bytea_cmp(b, a) < 0);
  CHECK(ty_bytea_cmp(a, b) > 0);
  CHECK(ty_bytea_cmp(b, c) < 0);
  CHECK(ty_bytea_cmp(b, b) == 0);
  CHECK(ty_bytea_cmp(b, blong) == 0);
  CHECK(ty_bytea_cmp(empty, b) < 0);
}

/*
 * Copyright (c) 2026 CK Tan
 * SPDX-License-Identifier: MIT
 */

/*
 * Tests of the bit string operations; the rest are in the vectors.
 */
#include <string.h>

#include <string>

#include "doctest.h"
#include "ty_bits.h"

namespace {

/* A bit string from 0s and 1s, with room after it. */
struct bs_t {
  uint8_t data[64] = {};
  int32_t n = 0;

  explicit bs_t(const char *s) {
    n = (int32_t)strlen(s);
    for (int32_t i = 0; i < n; i++) {
      if (s[i] == '1') {
        data[i >> 3] |= (uint8_t)(0x80 >> (i & 7));
      }
    }
  }

  /* The 0s and 1s. */
  std::string text() const {
    std::string t(n + 1, '0');

    ty_bits_format(data, n, &t[0]);
    t.resize(n);
    return t;
  }
};

} // namespace

// Verify the text: binary with or without b, hex after x, and the errors.
// 1. "101", "b101" and "B101" are the same 3 bits; "x5" is 0101.
// 2. A digit that is none is TY_ERR_INVALID and names the character.
// 3. The pad of the last byte is zero: the bytes of "1" are 0x80.
TEST_CASE("bit text is read in binary and in hex") {
  uint8_t out[32];
  int32_t n = 0;

  // 1. "101", "b101" and "B101" are the same 3 bits; "x5" is 0101.
  for (const char *t : {"101", "b101", "B101"}) {
    CHECK(0 == ty_bits_parse(t, strlen(t), out, &n));
    CHECK(3 == n);
    CHECK(out[0] == 0xA0);
  }
  CHECK(0 == ty_bits_parse("x5", 2, out, &n));
  CHECK(4 == n);
  CHECK(out[0] == 0x50);

  // 2. A digit that is none is TY_ERR_INVALID and names the character.
  CHECK(-1 == ty_bits_parse("b12", 3, out, &n));
  CHECK(TY_ERR_INVALID == ty_errnum());
  CHECK(std::string(ty_errmsg()) == "\"2\" is not a valid binary digit");
  CHECK(-1 == ty_bits_parse("xg", 2, out, &n));
  CHECK(std::string(ty_errmsg()) == "\"g\" is not a valid hexadecimal digit");

  // 3. The pad of the last byte is zero: the bytes of "1" are 0x80.
  CHECK(0 == ty_bits_parse("1", 1, out, &n));
  CHECK(out[0] == 0x80);
}

// Verify that results keep the pad bits zero, so memcmp orders strings.
// 1. NOT of "101" is "010", and its byte is 0x40, not 0x5F.
// 2. A shift left of "1011" by 1 is "0110": the pad is zero.
// 3. A substring that ends mid-byte has a zero pad.
// 4. An int4 as a bit(3) keeps its low 3 bits.
TEST_CASE("results keep the pad bits zero") {
  bs_t a("101"), b("1011"), c("11111111");
  uint8_t out[16];
  int32_t n = 0;

  // 1. NOT of "101" is "010", and its byte is 0x40, not 0x5F.
  ty_bits_not(a.data, a.n, out);
  CHECK(out[0] == 0x40);

  // 2. A shift left of "1011" by 1 is "0110": the pad is zero.
  ty_bits_shl(b.data, b.n, 1, out);
  CHECK(out[0] == 0x60);

  // 3. A substring that ends mid-byte has a zero pad.
  CHECK(0 == ty_bits_substr(c.data, c.n, 2, 3, 1, out, &n));
  CHECK(3 == n);
  CHECK(out[0] == 0xE0);

  // 4. An int4 as a bit(3) keeps its low 3 bits.
  ty_bits_from_int32(5, 3, out, &n);
  CHECK(3 == n);
  CHECK(out[0] == 0xA0);
  ty_bits_from_int32(13, 3, out, &n);
  CHECK(out[0] == 0xA0);
}

// Verify the errors of a bit operation, with PostgreSQL's text.
// 1. AND of two lengths: "cannot AND bit strings of different sizes".
// 2. A negative substring length, and an overlay from 0.
// 3. An index outside: "bit index 3 out of valid range (0..2)".
// 4. A string past 32 bits is TY_ERR_RANGE as an int32.
TEST_CASE("bit operations refuse what has no value") {
  bs_t a("101"), b("11");
  uint8_t out[16];
  int32_t n = 0, bit = 0, i32 = 0;

  // 1. AND of two lengths: "cannot AND bit strings of different sizes".
  CHECK(-1 == ty_bits_and(a.data, a.n, b.data, b.n, out));
  CHECK(std::string(ty_errmsg()) ==
        "cannot AND bit strings of different sizes");

  // 2. A negative substring length, and an overlay from 0.
  CHECK(-1 == ty_bits_substr(a.data, a.n, 1, -1, 1, out, &n));
  CHECK(TY_ERR_INVALID == ty_errnum());
  CHECK(-1 == ty_bits_overlay(a.data, a.n, b.data, b.n, 0, 1, out, &n));
  CHECK(TY_ERR_INVALID == ty_errnum());

  // 3. An index outside: "bit index 3 out of valid range (0..2)".
  CHECK(-1 == ty_bits_get(a.data, a.n, 3, &bit));
  CHECK(std::string(ty_errmsg()) == "bit index 3 out of valid range (0..2)");
  CHECK(-1 == ty_bits_set(a.data, a.n, 0, 2, out));
  CHECK(TY_ERR_INVALID == ty_errnum());

  // 4. A string past 32 bits is TY_ERR_RANGE as an int32.
  bs_t big("111111111111111111111111111111111");
  CHECK(-1 == ty_bits_to_int32(big.data, big.n, &i32));
  CHECK(TY_ERR_RANGE == ty_errnum());
}

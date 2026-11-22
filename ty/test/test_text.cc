/*
 * Copyright (c) 2026 CK Tan
 * SPDX-License-Identifier: MIT
 */

/*
 * Tests of the text operations; the rest are in the vectors.
 */
#include <string.h>

#include <string>

#include "doctest.h"
#include "text_raw.h"
#include "ty_text.h"
#include "vl.h"

// Verify that a position and a length count characters, not bytes.
// 1. "日本語" is 9 bytes and 3 characters.
// 2. "a😀b" has the 4-byte character at 2; b is at 3.
// 3. The substring from 2 for 1 of "a😀b" is the 4-byte character.
TEST_CASE("text counts characters") {
  // 1. "日本語" is 9 bytes and 3 characters.
  std::string cjk = "日本語";
  CHECK(9 == cjk.size());
  CHECK(3 == ty_text_length(vl_t(cjk)));

  // 2. "a😀b" has the 4-byte character at 2; b is at 3.
  std::string s = "a😀b";
  CHECK(3 == ty_text_position(vl_t(s), vl_t("b")));
  CHECK(2 == ty_text_position(vl_t(s), vl_t("😀")));

  // 3. The substring from 2 for 1 of "a😀b" is the 4-byte character.
  vl_t v(s);
  vl_out_t r;
  CHECK(0 == ty_text_substr(v, 2, 1, 1, &r));
  CHECK(r.str() == "😀");
}

// Verify the compare and the hash.
// 1. A prefix is below the longer string.
// 2. Bytes decide: "é" (0xC3 0xA9) is above "z".
// 3. Equal strings hash alike.
TEST_CASE("text order and hash") {
  // 1. A prefix is below the longer string.
  CHECK(ty_text_cmp(vl_t("ab"), vl_t("abc")) < 0);
  CHECK(ty_text_cmp(vl_t("abc"), vl_t("abc")) == 0);
  // the two forms of a header are the same value
  CHECK(ty_text_cmp(vl_t("abc"), vl_t("abc", true)) == 0);
  CHECK(ty_text_eq(vl_t("abc"), vl_t("abc", true)));

  // 2. Bytes decide: "é" (0xC3 0xA9) is above "z".
  CHECK(ty_text_cmp(vl_t("é"), vl_t("z")) > 0);

  // 3. Equal strings hash alike.
  CHECK(ty_text_hash(vl_t("abc")) == ty_text_hash(vl_t("abc", true)));
  CHECK(ty_text_hash(vl_t("abc")) != ty_text_hash(vl_t("abd")));
  CHECK((uint32_t)ty_text_hash_extended(vl_t("abc"), 0) ==
        ty_text_hash(vl_t("abc")));
}

// Verify the errors.
// 1. A negative substring length is TY_ERR_INVALID.
// 2. split_part with field 0 is TY_ERR_INVALID.
// 3. repeat past the limit is TY_ERR_RANGE, with PostgreSQL's message.
// 4. chr(0) is no character; chr of a surrogate is no character.
TEST_CASE("text errors") {
  vl_out_t r;

  // 1. A negative substring length is TY_ERR_INVALID.
  CHECK(-1 == ty_text_substr(vl_t("abc"), 1, -1, 1, &r));
  CHECK(TY_ERR_INVALID == ty_errnum());

  // 2. split_part with field 0 is TY_ERR_INVALID.
  CHECK(-1 == ty_text_split_part(vl_t("a,b"), vl_t(","), 0, &r));
  CHECK(std::string(ty_errmsg()) == "field position must not be zero");

  // 3. repeat past the limit is TY_ERR_RANGE, with PostgreSQL's message.
  CHECK(-1 == ty_text_repeat(vl_t("abc"), 2147483647, &r));
  CHECK(TY_ERR_RANGE == ty_errnum());
  CHECK(std::string(ty_errmsg()) == "requested length too large");

  // 4. chr(0) is no character; chr of a surrogate is no character.
  CHECK(-1 == ty_text_chr(0, &r));
  CHECK(std::string(ty_errmsg()) == "null character not permitted");
  CHECK(-1 == ty_text_chr(0xD800, &r));
  CHECK(std::string(ty_errmsg()) ==
        "requested character not valid for encoding: 55296");
}

// Verify that a search finds copies that do not overlap.
// 1. "aaaa" has two copies of "aa", at 0 and at 2.
// 2. A long needle in a long text is found by the skip table.
TEST_CASE("text search finds copies that do not overlap") {
  // 1. "aaaa" has two copies of "aa", at 0 and at 2.
  tyr_text_find_t f;

  tyr_text_find_init(&f, "aaaa", 4, "aa", 2);
  CHECK(0 == tyr_text_find_next(&f));
  CHECK(2 == tyr_text_find_next(&f));
  CHECK(-1 == tyr_text_find_next(&f));

  // 2. A long needle in a long text is found by the skip table.
  std::string hay(5000, 'x'), needle = "needle";
  hay += needle;
  hay += std::string(100, 'y');
  tyr_text_find_init(&f, hay.data(), hay.size(), needle.data(), needle.size());
  CHECK(5000 == tyr_text_find_next(&f));
  CHECK(-1 == tyr_text_find_next(&f));
}

// Verify character(n), character varying(n) and name.
// 1. char(5) of "ab" is 5 characters: 2 kept and 3 spaces added.
// 2. "abcdef" to 3 characters: the rest is not spaces, so it is an error
//    unless forced.
// 3. "ab   " to 2 characters is fine: the rest is spaces.
// 4. Trailing spaces do not count in the order or the hash.
// 5. A name is cut to 63 bytes at a character boundary.
TEST_CASE("character(n) and name") {
  vl_out_t r;

  // 1. char(5) of "ab" is 5 characters: 2 kept and 3 spaces added.
  CHECK(0 == ty_bpchar_fit(vl_t("ab"), 5, 0, 1, &r));
  CHECK(r.str() == "ab   ");

  // 2. "abcdef" to 3 characters: the rest is not spaces, so it is an error
  //    unless forced.
  CHECK(-1 == ty_bpchar_fit(vl_t("abcdef"), 3, 0, 1, &r));
  CHECK(TY_ERR_RANGE == ty_errnum());
  vl_out_t r2;
  CHECK(0 == ty_bpchar_fit(vl_t("abcdef"), 3, 1, 1, &r2));
  CHECK(r2.str() == "abc");

  // 3. "ab   " to 2 characters is fine: the rest is spaces.
  vl_out_t r3;
  CHECK(0 == ty_bpchar_fit(vl_t("ab   "), 2, 0, 1, &r3));
  CHECK(r3.str() == "ab");

  // 4. Trailing spaces do not count in the order or the hash.
  CHECK(ty_bpchar_cmp(vl_t("ab  "), vl_t("ab")) == 0);
  CHECK(ty_bpchar_cmp(vl_t("ab"), vl_t("abc")) < 0);
  CHECK(ty_bpchar_eq(vl_t("ab  "), vl_t("ab")));
  CHECK(ty_bpchar_hash(vl_t("ab  ")) == ty_bpchar_hash(vl_t("ab")));

  // 5. A name is cut to 63 bytes at a character boundary.
  std::string longname(21, 'x');
  longname += "日日日日日日日日日日日日日日日"; // 21 + 45 = 66 bytes
  char name[TY_NAMEDATALEN];

  ty_name_parse(longname.data(), longname.size(), name);
  CHECK(strlen(name) == 63);
  CHECK(strlen(name) % 3 == 0);
}

// Verify LIKE: wildcards over characters, the escape, and the lazy error.
// 1. _ is one character: it matches the 4-byte "😀" whole, not a byte.
// 2. % matches any run, and the pattern may have many of them.
// 3. A backslash makes % plain.
// 4. A trailing backslash is an error only if the match gets to it.
// 5. The escape translation: # becomes a backslash.
TEST_CASE("LIKE") {
  // 1. _ is one character: it matches the 4-byte "😀" whole, not a byte.
  CHECK(1 == ty_text_like(vl_t("a😀b"), vl_t("a_b"), 1));
  CHECK(0 == ty_text_like(vl_t("a😀b"), vl_t("a__b"), 1));
  CHECK(1 ==
        ty_text_like(vl_t("a😀b"), vl_t("a____b"), 0)); // as bytes: 4 of them

  // 2. % matches any run, and the pattern may have many of them.
  CHECK(1 == ty_text_like(vl_t("abcabc"), vl_t("%bc%bc"), 1));
  CHECK(0 == ty_text_like(vl_t("aaaa"), vl_t("%a%a%a%a%a"), 1));

  // 3. A backslash makes % plain.
  CHECK(1 == ty_text_like(vl_t("a%c"), vl_t("a\\%c"), 1));
  CHECK(0 == ty_text_like(vl_t("abc"), vl_t("a\\%c"), 1));

  // 4. A trailing backslash is an error only if the match gets to it.
  CHECK(0 == ty_text_like(vl_t("abc"), vl_t("x\\"), 1));
  CHECK(-1 == ty_text_like(vl_t("abc"), vl_t("a\\"), 1));
  CHECK(std::string(ty_errmsg()) ==
        "LIKE pattern must not end with escape character");

  // 5. The escape translation: # becomes a backslash.
  vl_out_t out;

  CHECK(0 == ty_text_like_escape(vl_t("a#%c"), vl_t("#"), 1, &out));
  CHECK(out.str() == "a\\%c");
  vl_out_t bad;
  CHECK(-1 == ty_text_like_escape(vl_t("a"), vl_t("ab"), 1, &bad));
}

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
#include "ty_text.h"

namespace {

/* A part of s, as the result of a function that gives its place. */
std::string part(const std::string &s, size_t off, size_t len) {
  return s.substr(off, len);
}

} // namespace

// Verify that a position and a length count characters, not bytes.
// 1. "日本語" is 9 bytes and 3 characters.
// 2. "a😀b" has the 4-byte character at 2; b is at 3.
// 3. The substring from 2 for 1 of "a😀b" is the 4-byte character.
TEST_CASE("text counts characters") {
  // 1. "日本語" is 9 bytes and 3 characters.
  std::string cjk = "日本語";
  CHECK(9 == cjk.size());
  CHECK(3 == ty_text_length(cjk.data(), cjk.size()));

  // 2. "a😀b" has the 4-byte character at 2; b is at 3.
  std::string s = "a😀b";
  CHECK(3 == ty_text_position(s.data(), s.size(), "b", 1));
  CHECK(2 == ty_text_position(s.data(), s.size(), "😀", 4));

  // 3. The substring from 2 for 1 of "a😀b" is the 4-byte character.
  size_t off, len;
  CHECK(0 == ty_text_substr(s.data(), s.size(), 2, 1, 1, &off, &len));
  CHECK(part(s, off, len) == "😀");
}

// Verify the compare and the hash.
// 1. A prefix is below the longer string.
// 2. Bytes decide: "é" (0xC3 0xA9) is above "z".
// 3. Equal strings hash alike.
TEST_CASE("text order and hash") {
  // 1. A prefix is below the longer string.
  CHECK(ty_text_cmp("ab", 2, "abc", 3) < 0);
  CHECK(ty_text_cmp("abc", 3, "abc", 3) == 0);

  // 2. Bytes decide: "é" (0xC3 0xA9) is above "z".
  CHECK(ty_text_cmp("é", 2, "z", 1) > 0);

  // 3. Equal strings hash alike.
  CHECK(ty_text_hash("abc", 3) == ty_text_hash("abc", 3));
  CHECK(ty_text_hash("abc", 3) != ty_text_hash("abd", 3));
  CHECK((uint32_t)ty_text_hash_extended("abc", 3, 0) == ty_text_hash("abc", 3));
}

// Verify the errors.
// 1. A negative substring length is TY_ERR_INVALID.
// 2. split_part with field 0 is TY_ERR_INVALID.
// 3. repeat past the limit is TY_ERR_RANGE, with PostgreSQL's message.
// 4. chr(0) is no character; chr of a surrogate is no character.
TEST_CASE("text errors") {
  size_t off, len;
  char buf[4];

  // 1. A negative substring length is TY_ERR_INVALID.
  CHECK(-1 == ty_text_substr("abc", 3, 1, -1, 1, &off, &len));
  CHECK(TY_ERR_INVALID == ty_errnum());

  // 2. split_part with field 0 is TY_ERR_INVALID.
  CHECK(-1 == ty_text_split_part("a,b", 3, ",", 1, 0, &off, &len));
  CHECK(std::string(ty_errmsg()) == "field position must not be zero");

  // 3. repeat past the limit is TY_ERR_RANGE, with PostgreSQL's message.
  CHECK(-1 == ty_text_repeat_size(3, 2147483647, &len));
  CHECK(TY_ERR_RANGE == ty_errnum());
  CHECK(std::string(ty_errmsg()) == "requested length too large");

  // 4. chr(0) is no character; chr of a surrogate is no character.
  CHECK(-1 == ty_text_chr(0, buf, &len));
  CHECK(std::string(ty_errmsg()) == "null character not permitted");
  CHECK(-1 == ty_text_chr(0xD800, buf, &len));
  CHECK(std::string(ty_errmsg()) ==
        "requested character not valid for encoding: 55296");
}

// Verify that a search finds copies that do not overlap.
// 1. "aaaa" has two copies of "aa", at 0 and at 2.
// 2. A long needle in a long text is found by the skip table.
TEST_CASE("text search finds copies that do not overlap") {
  // 1. "aaaa" has two copies of "aa", at 0 and at 2.
  ty_text_find_t f;

  ty_text_find_init(&f, "aaaa", 4, "aa", 2);
  CHECK(0 == ty_text_find_next(&f));
  CHECK(2 == ty_text_find_next(&f));
  CHECK(-1 == ty_text_find_next(&f));

  // 2. A long needle in a long text is found by the skip table.
  std::string hay(5000, 'x'), needle = "needle";
  hay += needle;
  hay += std::string(100, 'y');
  ty_text_find_init(&f, hay.data(), hay.size(), needle.data(), needle.size());
  CHECK(5000 == ty_text_find_next(&f));
  CHECK(-1 == ty_text_find_next(&f));
}

/*
 * Copyright (c) 2026 CK Tan
 * SPDX-License-Identifier: MIT
 */

/*
 * Tests of the host's regular expression engine (ty_regex.h), by a test
 * engine that matches a pattern as a substring.
 */
#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include <string>

#include "doctest.h"
#include "ty_regex.h"
#include "vl.h"

namespace {

/* What the test engine was last given. */
struct seen_t {
  std::string pat, s;
  int flags = -1;
  uint32_t collation = 0;
} g_seen;

/*
 * A test engine: a pattern matches a text it is in, in any case with
 * TY_REGEX_ICASE; "(" is an error, as an engine's bad pattern is.
 */
int substring_engine(const char *pat, size_t plen, const char *s, size_t slen,
                     int flags, uint32_t collation, int *matched, char *errmsg,
                     size_t errlen) {
  g_seen = {std::string(pat, plen), std::string(s, slen), flags, collation};
  std::string p(pat, plen), t(s, slen);

  if ("(" == p) {
    snprintf(errmsg, errlen,
             "invalid regular expression: parentheses () not "
             "balanced");
    return -1;
  }
  if (flags & TY_REGEX_ICASE) {
    for (char &c : p) {
      c = (char)tolower((unsigned char)c);
    }
    for (char &c : t) {
      c = (char)tolower((unsigned char)c);
    }
  }
  *matched = std::string::npos != t.find(p);
  return 0;
}

/* Set an engine for a test, and take it away at its end. */
struct engine_t {
  explicit engine_t(ty_regex_fn fn) { ty_set_regex(fn); }
  ~engine_t() { ty_set_regex(nullptr); }
};

/* ~ of two strings through ty: 1, 0, or "ERR " and ty's message. */
std::string match(const std::string &s, const std::string &pat, int flags = 0,
                  uint32_t collation = 0) {
  vl_t t(s), p(pat);
  int matched = -1;

  if (0 != ty_text_regex(t, p, flags, collation, &matched)) {
    return "ERR " + std::string(ty_errmsg());
  }
  return std::to_string(matched);
}

} // namespace

// Verify ~ reaches the host's engine, and what it says comes back.
// 1. With no engine set, ~ is an error.
// 2. With one set, ~ and ~* are its answers, and it gets the texts' bytes,
//    NULs included, the flags and the collation as they were given.
// 3. An error of the engine comes back with its message.
// 4. Taking the engine away makes ~ an error again.
TEST_CASE("~ goes through the host's engine") {
  // 1. With no engine set, ~ is an error.
  ty_set_regex(nullptr);
  CHECK(nullptr == ty_regex());
  CHECK(match("abc", "b") == "ERR no regular expression engine is set");
  CHECK(TY_ERR_INVALID == ty_errnum());

  // 2. With one set, ~ and ~* are its answers, and it gets the texts'
  //    bytes, NULs included, the flags and the collation as they were given.
  {
    engine_t e(substring_engine);

    CHECK(substring_engine == ty_regex());
    CHECK(match("abc", "b") == "1");
    CHECK(match("abc", "B") == "0");
    CHECK(match("abc", "B", TY_REGEX_ICASE, 950) == "1");
    CHECK(g_seen.flags == TY_REGEX_ICASE);
    CHECK(g_seen.collation == 950);
    CHECK(match(std::string("a\0b", 3), std::string("\0", 1)) == "1");
    CHECK(g_seen.s == std::string("a\0b", 3));
    CHECK(match("", "") == "1");

    // 3. An error of the engine comes back with its message.
    CHECK(match("abc", "(") ==
          "ERR invalid regular expression: parentheses () not balanced");
  }

  // 4. Taking the engine away makes ~ an error again.
  CHECK(match("abc", "b") == "ERR no regular expression engine is set");
}

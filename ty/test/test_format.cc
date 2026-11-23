/*
 * Copyright (c) 2026 CK Tan
 * SPDX-License-Identifier: MIT
 */

/*
 * Tests of format(): ty_format_begin, _next, _put, _finish and _abort,
 * driven by a host whose arguments are strings, as digres's are after their
 * output functions.
 */
#include <stdlib.h>
#include <string.h>

#include <string>
#include <vector>

#include "doctest.h"
#include "ty_text.h"
#include "vl.h"

namespace {

/* The allocations a test's thread made and has not freed. */
thread_local int t_live = 0;

/* An alloc that counts what is live, from malloc. */
void *live_alloc(size_t size) {
  t_live++;
  return malloc(size);
}

/* A free that counts what is live. */
void live_free(void *ptr) {
  t_live--;
  free(ptr);
}

/* Set the counting functions for a test; the default again at its end. */
struct live_t {
  live_t() {
    t_live = 0;
    ty_set_alloc(live_alloc, live_free);
  }
  ~live_t() { ty_set_alloc(nullptr, nullptr); }
};

/*
 * format(fmt, args...) as a host runs it. %s, %I and %L all put the argument
 * as it is: the quoting is the host's. A width argument is a number in
 * text. The result, "too few" for a missing argument, or "ERR " and ty's
 * message.
 */
std::string run(const std::string &fmt, const std::vector<std::string> &args) {
  ty_format_t f;
  ty_format_spec_t sp;
  size_t arg = 0;
  int rc;

  ty_format_begin(&f, fmt.data(), fmt.size());
  while (1 == (rc = ty_format_next(&f, &sp))) {
    int32_t width = sp.width;

    if (sp.widthpos >= 0) {
      if (sp.widthpos > 0) {
        arg = (size_t)sp.widthpos - 1;
      }
      if (arg >= args.size()) {
        ty_format_abort(&f);
        return "too few";
      }
      width = (int32_t)strtol(args[arg++].c_str(), nullptr, 10);
    }
    if (sp.argpos > 0) {
      arg = (size_t)sp.argpos - 1;
    }
    if (arg >= args.size()) {
      ty_format_abort(&f);
      return "too few";
    }
    const std::string &a = args[arg++];

    if (0 != ty_format_put(&f, a.data(), a.size(), sp.minus, width)) {
      return std::string("ERR ") + ty_errmsg();
    }
  }
  if (rc < 0) {
    return std::string("ERR ") + ty_errmsg();
  }
  vl_out_t r;
  CHECK(0 == ty_format_finish(&f, &r));
  return r.str();
}

} // namespace

// Verify the text and the arguments of a format, as PostgreSQL gives them.
// 1. Text with no conversion, and %% for one %.
// 2. Arguments in order, by place, and the two mixed.
// 3. An empty format, and one that is all conversion.
TEST_CASE("format puts the arguments") {
  live_t live;

  // 1. Text with no conversion, and %% for one %.
  CHECK(run("Hello", {}) == "Hello");
  CHECK(run("Hello %%", {}) == "Hello %");
  CHECK(run("Hello %%%%", {}) == "Hello %%");
  CHECK(run("%%s", {"x"}) == "%s");

  // 2. Arguments in order, by place, and the two mixed.
  CHECK(run("Hello %s", {"World"}) == "Hello World");
  CHECK(run("INSERT INTO %I VALUES(%L,%L)", {"mytab", "10", "Hello"}) ==
        "INSERT INTO mytab VALUES(10,Hello)");
  CHECK(run("%1$s %3$s", {"1", "2", "3"}) == "1 3");
  CHECK(run("Hello %s %1$s %s", {"World", "Hello again"}) ==
        "Hello World World Hello again");
  CHECK(run("Hello %s %s, %2$s %2$s", {"World", "Hello again"}) ==
        "Hello World Hello again, Hello again Hello again");
  CHECK(run("Hello %s %s", {"World"}) == "too few");

  // 3. An empty format, and one that is all conversion.
  CHECK(run("", {}) == "");
  CHECK(run("%s", {"日本語"}) == "日本語");
  CHECK(0 == t_live);
}

// Verify the width: in characters, from the format or from an argument.
// 1. A width pads on the left; - pads on the right.
// 2. A width counts characters, not bytes.
// 3. A width from an argument: the next one, or one by place; a negative
//    one pads on the right.
// 4. A text longer than the width is not cut.
TEST_CASE("format pads to the width") {
  live_t live;

  // 1. A width pads on the left; - pads on the right.
  CHECK(run(">>%10s<<", {"Hello"}) == ">>     Hello<<");
  CHECK(run(">>%-10s<<", {"Hello"}) == ">>Hello     <<");
  CHECK(run(">>%--10s<<", {"Hello"}) == ">>Hello     <<");
  CHECK(run(">>%-s<<", {"Hello"}) == ">>Hello<<");
  CHECK(run(">>%10s<<", {""}) == ">>          <<");
  CHECK(run(">>%1$10s<<", {"Hello"}) == ">>     Hello<<");

  // 2. A width counts characters, not bytes.
  CHECK(run("%5s|", {"日本"}) == "   日本|");
  CHECK(run("%-4s|", {"a😀"}) == "a😀  |");

  // 3. A width from an argument: the next one, or one by place; a negative
  //    one pads on the right.
  CHECK(run(">>%*s<<", {"10", "Hello"}) == ">>     Hello<<");
  CHECK(run(">>%*1$s<<", {"10", "Hello"}) == ">>     Hello<<");
  CHECK(run(">>%2$*1$L<<", {"10", "Hello"}) == ">>     Hello<<");
  CHECK(run(">>%2$*1$L<<", {"-10", "NULL"}) == ">>NULL      <<");
  CHECK(run(">>%2$*1$L<<", {"0", "Hello"}) == ">>Hello<<");

  // 4. A text longer than the width is not cut.
  CHECK(run("%2s", {"Hello"}) == "Hello");
  CHECK(0 == t_live);
}

// Verify each error of a format string, and that none leaks.
// 1. A conversion the format ends in, at each place it can end.
// 2. A letter that is not s, I or L, a multibyte one whole.
// 3. Argument 0, and a width's place with no $.
// 4. A number past int32, and a width of INT32_MIN.
// 5. An error after text was added frees it.
TEST_CASE("format errors") {
  live_t live;
  const std::string unterminated = "ERR unterminated format() type specifier";

  // 1. A conversion the format ends in, at each place it can end.
  for (const char *f :
       {"%", "abc%", "%1", "%1$", "%-", "%*", "%*1", "%*1$", "%12", "%1$1"}) {
    INFO(f);
    CHECK(run(f, {"x"}) == unterminated);
  }

  // 2. A letter that is not s, I or L, a multibyte one whole.
  CHECK(run("Hello %x", {"20"}) ==
        "ERR unrecognized format() type specifier \"x\"");
  CHECK(run("%é", {"1"}) == "ERR unrecognized format() type specifier \"é\"");
  CHECK(run("%1$-5d", {"1"}) ==
        "ERR unrecognized format() type specifier \"d\"");

  // 3. Argument 0, and a width's place with no $.
  const std::string zero =
      "ERR format specifies argument 0, but arguments are numbered from 1";
  CHECK(run("%0$s", {"Hello"}) == zero);
  CHECK(run("%*0$s", {"Hello"}) == zero);
  CHECK(run("%*1s", {"1", "x"}) ==
        "ERR width argument position must be ended by \"$\"");

  // 4. A number past int32, and a width of INT32_MIN.
  CHECK(run("%2147483648s", {"x"}) == "ERR number is out of range");
  CHECK(run("%2147483647$s", {"x"}) == "too few");
  CHECK(run("%*s", {"-2147483648", "x"}) == "ERR number is out of range");

  // 5. An error after text was added frees it.
  CHECK(run("a long text before the error %s %", {"x"}) == unterminated);
  CHECK(0 == t_live);
}

// Verify the limit of a result, without allocating it.
// 1. A width that makes the result 1073741823 bytes or more is the error of
//    PostgreSQL's StringInfo, with the bytes there were and the bytes asked.
// 2. Nothing is allocated for it, and what was is freed.
// 3. A host that stops between the calls frees by ty_format_abort.
TEST_CASE("format limit and abort") {
  live_t live;
  ty_format_t f;
  ty_format_spec_t sp;

  // 1. A width that makes the result 1073741823 bytes or more is the error of
  //    PostgreSQL's StringInfo, with the bytes there were and the bytes asked.
  ty_format_begin(&f, "ab%s", 4);
  REQUIRE(1 == ty_format_next(&f, &sp));
  CHECK(-1 == ty_format_put(&f, "x", 1, 0, 1073741822));
  CHECK(TY_ERR_RANGE == ty_errnum());
  CHECK(std::string(ty_errmsg()) ==
        "string buffer exceeds maximum allowed length (1073741823 bytes)");
  CHECK(2 == f.had);
  CHECK(1073741821 == f.need);
  // PostgreSQL pads first, then fails on the text: its detail is of the text
  ty_format_begin(&f, "%s", 2);
  REQUIRE(1 == ty_format_next(&f, &sp));
  CHECK(-1 == ty_format_put(&f, "x", 1, 0, 1073741823));
  CHECK(1073741822 == f.had);
  CHECK(1 == f.need);
  // past what a varlena holds, within what a StringInfo does
  ty_format_begin(&f, "%s", 2);
  REQUIRE(1 == ty_format_next(&f, &sp));
  CHECK(-1 == ty_format_put(&f, "x", 1, 1, 1073741821));
  CHECK(std::string(ty_errmsg()) == "requested length too large");

  // 2. Nothing is allocated for it, and what was is freed.
  CHECK(nullptr == f.buf);
  CHECK(0 == t_live);

  // 3. A host that stops between the calls frees by ty_format_abort.
  ty_format_begin(&f, "text %s", 7);
  REQUIRE(1 == ty_format_next(&f, &sp));
  CHECK(1 == t_live);
  ty_format_abort(&f);
  CHECK(0 == t_live);
}

// Verify a result that grows past its first buffer many times.
// 1. Format 2,000 arguments of 1 to 3 bytes, with text between.
// 2. Compare with the string made by hand; nothing is left allocated.
TEST_CASE("format grows its result") {
  live_t live;
  std::string fmt, want;
  std::vector<std::string> args;

  // 1. Format 2,000 arguments of 1 to 3 bytes, with text between.
  for (int i = 0; i < 2000; i++) {
    args.push_back(std::to_string(i % 1000));
    fmt += "<%s>";
    want += "<" + args.back() + ">";
  }

  // 2. Compare with the string made by hand; nothing is left allocated.
  CHECK(run(fmt, args) == want);
  CHECK(0 == t_live);
}

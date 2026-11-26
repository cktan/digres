/*
 * Copyright (c) 2026 CK Tan
 * SPDX-License-Identifier: MIT
 */

/*
 * test_eval_read.cc -- the reader of the evaluation engine (src/eval_form.h):
 * Janet's syntax into forms, and its errors with their line and column.
 */
#include "doctest.h"
#include "eval_form.h"
#include "ty.h"

#include <string>
#include <vector>

using ty_internal::form_kind_t;
using ty_internal::form_t;

namespace {

/*
 * A form as text, to compare: a list (a b), a bracket list [a b], a symbol
 * by its name, an integer #123, a real ~1.5, true, false and nil.
 */
std::string show(const form_t &f) {
  switch (f.kind) {
  case form_kind_t::list:
  case form_kind_t::bracket: {
    std::string s = form_kind_t::list == f.kind ? "(" : "[";
    for (size_t i = 0; i < f.items.size(); i++) {
      s += (i ? " " : "") + show(f.items[i]);
    }
    return s + (form_kind_t::list == f.kind ? ")" : "]");
  }
  case form_kind_t::symbol:
    return f.text;
  case form_kind_t::integer:
    return "#" + f.text;
  case form_kind_t::real:
    return "~" + f.text;
  case form_kind_t::boolean:
    return f.value ? "true" : "false";
  case form_kind_t::nil:
    return "nil";
  }
  return "?";
}

/* The forms of text, each shown, a space apart; or "ERR message". */
std::string read(const std::string &text) {
  std::vector<form_t> forms;
  if (0 != ty_internal::read_forms(text, forms)) {
    return std::string("ERR ") + ty_errmsg();
  }
  std::string s;
  for (size_t i = 0; i < forms.size(); i++) {
    s += (i ? " " : "") + show(forms[i]);
  }
  return s;
}

} // namespace

// Verify that a program of EVAL_PLAN.md §1 reads into its forms, each with
// its line and column.
// 1. Read the program, over three lines.
// 2. Its forms, shown.
// 3. The line and column of the fn, of the bracket list, of a symbol on the
//    second line, and of a number on the third.
TEST_CASE("the reader reads a program into forms, with their places") {
  // 1. The program.
  const std::string text = "(fn [x:i32.v lim:i32]\n"
                           "  (cmp.gt.i32.v x\n"
                           "     (splat.i32 (add.i32 lim 1))))";
  std::vector<form_t> forms;
  REQUIRE(0 == ty_internal::read_forms(text, forms));
  // 2. Its forms.
  REQUIRE(1 == forms.size());
  CHECK(show(forms[0]) == "(fn [x:i32.v lim:i32] (cmp.gt.i32.v x (splat.i32 "
                          "(add.i32 lim #1))))");
  // 3. The places.
  const form_t &fn = forms[0];
  CHECK((1 == fn.line && 1 == fn.col));
  CHECK((1 == fn.items[1].line && 5 == fn.items[1].col));
  const form_t &cmp = fn.items[2];
  CHECK((2 == cmp.line && 3 == cmp.col));
  CHECK((2 == cmp.items[1].line && 17 == cmp.items[1].col));
  const form_t &one = cmp.items[2].items[1].items[2];
  CHECK((3 == one.line && 30 == one.col));
  CHECK(form_kind_t::integer == one.kind);
}

// Verify the tokens: numbers, symbols, true, false and nil, space and
// comments.
// 1. Integers and reals, with signs, points and exponents; a sign alone,
//    and a sign before a letter, are symbols.
// 2. true, false, nil, and names that only begin with them.
// 3. Space of every kind, comments to the end of a line, forms with no
//    space between them, several forms, and none.
TEST_CASE("the reader reads numbers, symbols, space and comments") {
  // 1. Numbers.
  CHECK(read("0 -4 +7 123456789012345678901234") ==
        "#0 #-4 #+7 #123456789012345678901234");
  CHECK(read("1.5 -0.25 1e10 2.5E-3 -1e+2") ==
        "~1.5 ~-0.25 ~1e10 ~2.5E-3 ~-1e+2");
  CHECK(read("- + -x +y ... a.b x:i32.v <= !$%&*/?^_") ==
        "- + -x +y ... a.b x:i32.v <= !$%&*/?^_");
  // 2. The words.
  CHECK(read("true false nil truex nil? falsey") ==
        "true false nil truex nil? falsey");
  // 3. Space and comments.
  CHECK(read(" \t\r\n\f\v(a)# a comment (b\n[ b ]# to the end") == "(a) [b]");
  CHECK(read("(a(b)c)[d[e]]") == "(a (b) c) [d [e]]");
  CHECK(read("a b (c)") == "a b (c)");
  CHECK(read("(a# a comment right after a token\n)") == "(a)");
  CHECK(read("") == "");
  CHECK(read("  # nothing but a comment") == "");
}

// Verify that each malformed text fails with its line and column and why.
// 1. A list not closed, a closer with no opener, and one that does not
//    match.
// 2. A bad number.
// 3. What Janet has and this reader does not take: a string, a keyword, a
//    quote, a brace, an array, a comma; and a byte that is not ASCII, and
//    NUL.
// 4. Lists nested deeper than form_max_depth, and as deep as it.
// 5. The thread's error is TY_ERR_INVALID.
TEST_CASE("the reader names each error's line and column") {
  // 1. The lists.
  CHECK(read("(a\n (b c)") == "ERR 1:1: ( is not closed");
  CHECK(read("  [a") == "ERR 1:3: [ is not closed");
  CHECK(read("a\n  )") == "ERR 2:3: ) has no (");
  CHECK(read("]") == "ERR 1:1: ] has no [");
  CHECK(read("(a ]") == "ERR 1:4: ( is closed by ]");
  CHECK(read("[a)") == "ERR 1:3: [ is closed by )");
  // 2. Numbers.
  for (const char *bad :
       {"1x", "1.", "1.2.3", "1e", "1e+", "-1a", ".5", "0x1F", "1_000"}) {
    CAPTURE(bad);
    CHECK(read(std::string("(a ") + bad + ")") == "ERR 1:4: a bad number");
  }
  // 3. Not taken.
  CHECK(read("(a \"s\")") == "ERR 1:4: a string is not taken");
  CHECK(read(":k") == "ERR 1:1: a keyword is not taken");
  CHECK(read("'a") == "ERR 1:1: a character that is not taken");
  CHECK(read("{a 1}") == "ERR 1:1: a character that is not taken");
  CHECK(read("@[1]") == "ERR 1:1: a character that is not taken");
  CHECK(read("(a, b)") == "ERR 1:3: a character that is not taken");
  CHECK(read("(a \xc3\xa9)") == "ERR 1:4: a character that is not taken");
  CHECK(read(std::string("a\0b", 3)) ==
        "ERR 1:2: a character that is not taken");
  // 4. The depth.
  const uint32_t d = ty_internal::form_max_depth;
  CHECK(read(std::string(d + 1, '(') + std::string(d + 1, ')')) ==
        "ERR 1:" + std::to_string(d + 1) + ": lists are nested too deeply");
  CHECK(read(std::string(d, '(') + std::string(d, ')')).size() == 2 * d);
  // 5. The error.
  CHECK(read("(") == "ERR 1:1: ( is not closed");
  CHECK(TY_ERR_INVALID == ty_errnum());
}

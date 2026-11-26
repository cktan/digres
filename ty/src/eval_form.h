/*
 * Copyright (c) 2026 CK Tan
 * SPDX-License-Identifier: MIT
 */

/*
 * eval_form.h -- the forms of the evaluation engine's text (../EVAL_PLAN.md
 * §4): Janet's syntax, read into a tree. A form is a list, (a b), a bracket
 * list, [a b], a symbol, a number, true, false or nil. Each knows its line
 * and column, for the compiler's errors.
 */
#ifndef TY_SRC_EVAL_FORM_H
#define TY_SRC_EVAL_FORM_H

#include <stdint.h>

#include <string>
#include <string_view>
#include <vector>

namespace ty_internal {

/* What a form is. */
enum class form_kind_t : uint8_t {
  list,    /* (a b) */
  bracket, /* [a b] */
  symbol,  /* add.i32.v, x:i32.v, -, + */
  integer, /* 123, -4: digits, no point and no exponent */
  real,    /* 1.5, 1e10, -0.25 */
  boolean, /* true, false */
  nil,     /* nil */
};

/*
 * One form. text is a symbol's name, or a number's text as written, which
 * the compiler reads by the type its operator takes; items are a list's.
 */
struct form_t {
  form_kind_t kind = form_kind_t::nil;
  bool value = false; /* a boolean's */
  uint32_t line = 0;  /* from 1 */
  uint32_t col = 0;   /* from 1, in bytes */
  std::string text;
  std::vector<form_t> items;
};

/* The deepest a list may be nested in a text. */
constexpr uint32_t form_max_depth = 256;

/*
 * Read every form of text into out, in order. Returns 0, or -1 with
 * TY_ERR_INVALID and "line:col: why" as the thread's error. A string, a
 * keyword, a quote, a brace, a table or an array of Janet's is not taken,
 * nor is a byte that is not ASCII. It allocates with new: the caller
 * catches std::bad_alloc.
 */
int read_forms(std::string_view text, std::vector<form_t> &out);

} // namespace ty_internal

#endif /* TY_SRC_EVAL_FORM_H */

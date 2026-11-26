/*
 * Copyright (c) 2026 CK Tan
 * SPDX-License-Identifier: MIT
 */

/*
 * eval_read.cc -- the reader of the evaluation engine (eval_form.h): text
 * of Janet's syntax into forms. Written for ty: it takes the part of
 * Janet's syntax that ../EVAL_PLAN.md §4 needs, and refuses the rest.
 */
#include "eval_form.h"

#include <stdio.h>

#include "error.h"

namespace ty_internal {
namespace {

/* Whether c is space, which ends a token. */
bool is_space(char c) {
  return ' ' == c || '\t' == c || '\n' == c || '\r' == c || '\f' == c ||
         '\v' == c;
}

/* Whether c may be in a symbol or a number: Janet's symbol characters. */
bool is_token_char(char c) {
  if (('a' <= c && c <= 'z') || ('A' <= c && c <= 'Z') ||
      ('0' <= c && c <= '9')) {
    return true;
  }
  for (const char *p = "!$%&*+-./:<=>?^_"; *p; p++) {
    if (*p == c) {
      return true;
    }
  }
  return false;
}

/* Whether c is a decimal digit. */
bool is_digit(char c) { return '0' <= c && c <= '9'; }

/*
 * The kind of a number's text t, which begins with a digit, or a sign and a
 * digit: integer, real, or symbol if it is no number.
 * 1. A sign.
 * 2. Digits, then a point and digits, then e or E, a sign and digits; each
 *    part that is there has a digit.
 */
form_kind_t number_kind(std::string_view t) {
  size_t i = 0;
  bool real = false;
  // 1. A sign.
  if ('+' == t[i] || '-' == t[i]) {
    i++;
  }
  // 2. The digits.
  auto digits = [&]() {
    const size_t from = i;
    while (i < t.size() && is_digit(t[i])) {
      i++;
    }
    return i > from;
  };
  if (!digits()) {
    return form_kind_t::symbol;
  }
  if (i < t.size() && '.' == t[i]) {
    i++;
    real = true;
    if (!digits()) {
      return form_kind_t::symbol;
    }
  }
  if (i < t.size() && ('e' == t[i] || 'E' == t[i])) {
    i++;
    real = true;
    if (i < t.size() && ('+' == t[i] || '-' == t[i])) {
      i++;
    }
    if (!digits()) {
      return form_kind_t::symbol;
    }
  }
  if (i != t.size()) {
    return form_kind_t::symbol;
  }
  return real ? form_kind_t::real : form_kind_t::integer;
}

/* The state of a read: the text, and where it is. */
struct reader_t {
  std::string_view text;
  size_t at = 0;
  uint32_t line = 1;
  size_t line_start = 0;

  /* The column of byte at, from 1. */
  uint32_t col(size_t pos) const { return uint32_t(pos - line_start + 1); }

  /* Fail at line and col, for why. */
  int fail(uint32_t l, uint32_t c, const char *why) {
    char msg[160];
    snprintf(msg, sizeof msg, "%u:%u: %s", l, c, why);
    return set_error(TY_ERR_INVALID, msg);
  }

  /* Pass space and comments, which run from # to the end of the line. */
  void skip() {
    while (at < text.size()) {
      const char c = text[at];
      if ('\n' == c) {
        at++;
        line++;
        line_start = at;
      } else if (is_space(c)) {
        at++;
      } else if ('#' == c) {
        while (at < text.size() && '\n' != text[at]) {
          at++;
        }
      } else {
        return;
      }
    }
  }

  /*
   * Read one form at at, which is not space, into f, at depth.
   * 1. A list or a bracket list: its forms, to the closer that matches.
   * 2. A token: a number, true, false, nil, or a symbol.
   * 3. Anything else is not taken.
   */
  int form(form_t &f, uint32_t depth) {
    const char c = text[at];
    f.line = line;
    f.col = col(at);
    // 1. A list.
    if ('(' == c || '[' == c) {
      if (depth >= form_max_depth) {
        return fail(f.line, f.col, "lists are nested too deeply");
      }
      const char close = '(' == c ? ')' : ']';
      f.kind = '(' == c ? form_kind_t::list : form_kind_t::bracket;
      at++;
      for (;;) {
        skip();
        if (at >= text.size()) {
          return fail(f.line, f.col,
                      '(' == c ? "( is not closed" : "[ is not closed");
        }
        if (')' == text[at] || ']' == text[at]) {
          if (close != text[at]) {
            return fail(line, col(at),
                        '(' == c ? "( is closed by ]" : "[ is closed by )");
          }
          at++;
          return 0;
        }
        f.items.emplace_back();
        if (0 != form(f.items.back(), depth + 1)) {
          return -1;
        }
      }
    }
    // 2. A token.
    if (is_token_char(c)) {
      const size_t from = at;
      while (at < text.size() && is_token_char(text[at])) {
        at++;
      }
      if (at < text.size() && !is_space(text[at]) && '(' != text[at] &&
          ')' != text[at] && '[' != text[at] && ']' != text[at] &&
          '#' != text[at]) {
        return fail(line, col(at), "a character that is not taken");
      }
      f.text.assign(text.substr(from, at - from));
      const bool numeric =
          is_digit(c) || (('+' == c || '-' == c || '.' == c) &&
                          f.text.size() > 1 && is_digit(f.text[1]));
      if (numeric) {
        f.kind = number_kind(f.text);
        if (form_kind_t::symbol == f.kind) {
          return fail(f.line, f.col, "a bad number");
        }
      } else if (':' == c) {
        return fail(f.line, f.col, "a keyword is not taken");
      } else if ("true" == f.text || "false" == f.text) {
        f.kind = form_kind_t::boolean;
        f.value = "true" == f.text;
      } else if ("nil" == f.text) {
        f.kind = form_kind_t::nil;
      } else {
        f.kind = form_kind_t::symbol;
      }
      return 0;
    }
    // 3. Anything else.
    if (')' == c || ']' == c) {
      return fail(f.line, f.col, ')' == c ? ") has no (" : "] has no [");
    }
    if ('"' == c) {
      return fail(f.line, f.col, "a string is not taken");
    }
    return fail(f.line, f.col, "a character that is not taken");
  }

  /* Pass on to ty's error state. */
  static int set_error(ty_errnum_t code, const char *msg) {
    return ty_internal::set_error(code, msg);
  }
};

} // namespace

/* Read every form of text; see eval_form.h. */
int read_forms(std::string_view text, std::vector<form_t> &out) {
  reader_t r;
  r.text = text;
  out.clear();
  for (;;) {
    r.skip();
    if (r.at >= text.size()) {
      return 0;
    }
    out.emplace_back();
    if (0 != r.form(out.back(), 0)) {
      return -1;
    }
  }
}

} // namespace ty_internal

/*
 * Copyright (c) 2026 CK Tan
 * SPDX-License-Identifier: MIT
 */

/*
 * bool.cc -- the operations of bool.
 */
#include "ty_bool.h"

#include "error.h"
#include "ty_hash.h"

namespace {

/* True if the len bytes at s are the first len letters of word, any case. */
bool is_prefix_of(const char *s, size_t len, const char *word, size_t wlen) {
  if (len == 0 || len > wlen) {
    return false;
  }
  for (size_t i = 0; i < len; i++) {
    char c = s[i];
    if (c >= 'A' && c <= 'Z') {
      c = (char)(c - 'A' + 'a');
    }
    if (c != word[i]) {
      return false;
    }
  }
  return true;
}

} // namespace

/* True for the bytes C's isspace() takes in the "C" locale. */
static bool is_space(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' ||
         c == '\r';
}

/*
 * Parse SQL's boolean input.
 * 1. Skip the space at the start, and at the end.
 * 2. Parse what is left, as ty_bool_parse() does.
 */
int ty_bool_parse_text(const char *s, size_t len, bool *r) {
  while (len > 0 && is_space(*s)) {
    s++;
    len--;
  }
  while (len > 0 && is_space(s[len - 1])) {
    len--;
  }
  return ty_bool_parse(s, len, r);
}

/*
 * Parse a boolean.
 * 1. Match the words of true: true, yes and on, each cut short but for
 *    "on", which must be whole as "o" begins "off" too; then "1".
 * 2. Match the words of false: false, no, off (two letters at least), "0".
 * 3. Anything else is TY_ERR_INVALID.
 */
int ty_bool_parse(const char *s, size_t len, bool *r) {
  if (is_prefix_of(s, len, "true", 4) || is_prefix_of(s, len, "yes", 3) ||
      (len == 2 && is_prefix_of(s, len, "on", 2)) ||
      (len == 1 && s[0] == '1')) {
    *r = true;
    return 0;
  }
  if (is_prefix_of(s, len, "false", 5) || is_prefix_of(s, len, "no", 2) ||
      (len >= 2 && is_prefix_of(s, len, "off", 3)) ||
      (len == 1 && s[0] == '0')) {
    *r = false;
    return 0;
  }
  return ty_internal::set_error(TY_ERR_INVALID, "invalid boolean");
}

/* The hash of a bool, as PostgreSQL's: that of the 32-bit 0 or 1. */
uint64_t ty_bool_hash(bool b, uint64_t seed) {
  return ty_hash_u32(b ? 1 : 0, seed);
}

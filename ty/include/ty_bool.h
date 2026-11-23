/*
 * Copyright (c) 2026 CK Tan
 * SPDX-License-Identifier: MIT
 */

/*
 * ty_bool.h -- the operations of bool (SQL's boolean). Written for ty,
 * with the accepted words of PostgreSQL's boolean input; no code of it.
 */
#ifndef TY_BOOL_H
#define TY_BOOL_H

#include <stdbool.h>
#include <stddef.h>

#include <stdint.h>

#include "ty.h"

/*
 * Parse the len bytes at s as a boolean. True is "true", "yes", "on" or
 * "1"; false is "false", "no", "off" or "0". A word may be cut short, if
 * what is left names one word: "t", "tr", "y", "n", "of", "fals". "o"
 * does not, as "on" and "off" both begin with it; "on" is whole, and "1"
 * and "0" are whole. Case does not matter. Nothing is skipped: a space
 * before or after fails, so a caller that allows one trims first. s need
 * not end in NUL. Sets *r and returns 0; or returns -1 with
 * TY_ERR_INVALID, and *r is not set.
 */
int ty_bool_parse(const char *s, size_t len, bool *r);

/*
 * Parse SQL's boolean input: the len bytes at s with the space at both ends
 * skipped (a space, tab, newline, vertical tab, form feed or return), then
 * as ty_bool_parse() reads them.
 */
int ty_bool_parse_text(const char *s, size_t len, bool *r);

/*
 * The text of a bool: "t" or "f" as the type's output (ty_bool_out), and
 * "true" or "false" as the cast to text (ty_bool_text). They are literals,
 * not to be freed.
 */
static inline const char *ty_bool_out(bool b) { return b ? "t" : "f"; }
static inline const char *ty_bool_text(bool b) { return b ? "true" : "false"; }

/* Compare two bools, false below true: -1, 0 or 1; and the predicates. */
static inline int ty_bool_cmp(bool a, bool b) { return (int)a - (int)b; }
static inline int ty_bool_eq(bool a, bool b) { return a == b; }
static inline int ty_bool_ne(bool a, bool b) { return a != b; }
static inline int ty_bool_lt(bool a, bool b) { return !a && b; }
static inline int ty_bool_le(bool a, bool b) { return !a || b; }
static inline int ty_bool_gt(bool a, bool b) { return a && !b; }
static inline int ty_bool_ge(bool a, bool b) { return a || !b; }

/* The logic of AND and OR of two bools, and the casts with int32. */
static inline bool ty_bool_and(bool a, bool b) { return a && b; }
static inline bool ty_bool_or(bool a, bool b) { return a || b; }
static inline bool ty_bool_from_int32(int32_t a) { return a != 0; }
static inline int32_t ty_bool_to_int32(bool a) { return a ? 1 : 0; }

/*
 * The state of the aggregates EVERY (bool_and) and ANY (bool_or) over a
 * window, and of bool_and and bool_or in a moving frame: the number of values
 * that are not NULL, and how many are true. A caller keeps the state, zeroed,
 * and passes only the values that are not NULL.
 */
typedef struct ty_bool_agg_t {
  int64_t count; /* the values aggregated, which are not NULL */
  int64_t ntrue; /* of them, the true ones */
} ty_bool_agg_t;

/* A value enters the state, and leaves it (the moving frame). */
static inline void ty_bool_agg_accum(ty_bool_agg_t *st, bool v) {
  st->count++;
  if (v) {
    st->ntrue++;
  }
}
static inline void ty_bool_agg_accum_inv(ty_bool_agg_t *st, bool v) {
  st->count--;
  if (v) {
    st->ntrue--;
  }
}

/*
 * The result of EVERY and of ANY over the state: 1 and *out set, or 0 for
 * NULL, when no value that is not NULL was aggregated.
 */
static inline int ty_bool_agg_every(const ty_bool_agg_t *st, bool *out) {
  if (st->count == 0) {
    return 0;
  }
  *out = st->ntrue == st->count;
  return 1;
}
static inline int ty_bool_agg_any(const ty_bool_agg_t *st, bool *out) {
  if (st->count == 0) {
    return 0;
  }
  *out = st->ntrue > 0;
  return 1;
}

/*
 * A 64-bit hash of a bool by a seed: that of the int64 0 or 1 (ty_int.h).
 * The low 32 bits with seed 0 are the standard 32-bit hash.
 */
uint64_t ty_bool_hash(bool b, uint64_t seed);

#endif /* TY_BOOL_H */

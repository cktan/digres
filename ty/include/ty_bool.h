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
int ty_bool_parse(const char* s, size_t len, bool* r);

#endif /* TY_BOOL_H */

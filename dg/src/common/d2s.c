/*-------------------------------------------------------------------------
 *
 * d2s.c
 *	  The shortest decimal that reads back to the same float or double.
 *
 * Portions Copyright (c) 2018-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 2026, CK Tan: the changes for digres,
 * under the MIT License (https://github.com/cktan/digres/blob/main/LICENSE)
 *
 * This file held PostgreSQL's Ryu code (src/common/d2s.c and f2s.c, a
 * modification of github.com/ulfjack/ryu under the Boost license). That
 * code is ty's now, in ty/src/ryu/, with its notices; these functions call
 * ty, which prints a float as the text of float4out and float8out with
 * extra_float_digits above 0.
 *
 * IDENTIFICATION
 *	  src/common/d2s.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "common/shortest_dec.h"
#include "ty_float.h"

/* The shortest decimal of f, in result, with a NUL; returns its length. */
int double_to_shortest_decimal_bufn(double f, char *result) {
  return (int)ty_float64_format(f, 1, result);
}

/* The same; the result has room for DOUBLE_SHORTEST_DECIMAL_LEN bytes. */
int double_to_shortest_decimal_buf(double f, char *result) {
  return (int)ty_float64_format(f, 1, result);
}

/* The shortest decimal of f, in memory from palloc. */
char *double_to_shortest_decimal(double f) {
  char *const result = (char *)palloc(DOUBLE_SHORTEST_DECIMAL_LEN);

  double_to_shortest_decimal_buf(f, result);
  return result;
}

/* The shortest decimal of the float f, in result, with a NUL. */
int float_to_shortest_decimal_bufn(float f, char *result) {
  return (int)ty_float32_format(f, 1, result);
}

/* The same; the result has room for FLOAT_SHORTEST_DECIMAL_LEN bytes. */
int float_to_shortest_decimal_buf(float f, char *result) {
  return (int)ty_float32_format(f, 1, result);
}

/* The shortest decimal of the float f, in memory from palloc. */
char *float_to_shortest_decimal(float f) {
  char *const result = (char *)palloc(FLOAT_SHORTEST_DECIMAL_LEN);

  float_to_shortest_decimal_buf(f, result);
  return result;
}

/*
 * Copyright (c) 2026 CK Tan
 * SPDX-License-Identifier: MIT
 */

/*
 * decimal.cc -- the operations of decimal64 and decimal128.
 */

#include "ty_decimal.h"

/*
 * *result = a + b, at the larger of the two scales.
 * 1. Widen both values to 128 bits.
 * 2. Multiply the one of the smaller scale by 10 until the scales are
 *    equal. A decimal64 has at most 17 digits and a scale of at most 17,
 *    so 128 bits hold the result.
 * 3. Add them.
 */
int ty_decimal64_add(const ty_decimal64_t *a, const ty_decimal64_t *b,
                     ty_decimal128_t *result) {
  ty_int128_t x = a->value;
  ty_int128_t y = b->value;
  int sa = a->scale;
  int sb = b->scale;

  for (; sa < sb; sa++) {
    x *= 10;
  }
  for (; sb < sa; sb++) {
    y *= 10;
  }
  result->value = x + y;
  result->scale = sa;
  return 0;
}

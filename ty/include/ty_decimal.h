/*
 * Copyright (c) 2026 CK Tan
 * SPDX-License-Identifier: MIT
 */

/*
 * ty_decimal.h -- the operations of decimal64 and decimal128, in C
 * style, compiled as C++ (ty.h has the conventions).
 *
 * A decimal here is a value and a scale: value / 10^scale. How a
 * host packs one into a Datum or into a row is the host's: digres
 * packs a decimal64 as (value << 5) | scale. ty takes and gives
 * the two parts.
 *
 * A decimal64 holds at most 17 digits, so its value is below 10^17
 * and its scale is at most 17. A decimal128 holds at most 36.
 */
#ifndef TY_DECIMAL_H
#define TY_DECIMAL_H

#include <stdint.h>

#include "ty.h"

/* A signed 128-bit integer, GCC's extension. */
__extension__ typedef __int128 ty_int128_t;

typedef struct ty_decimal64_t {
    int64_t value;
    int     scale;
} ty_decimal64_t;

typedef struct ty_decimal128_t {
    ty_int128_t value;
    int         scale;
} ty_decimal128_t;

/*
 * *result = a + b, at the larger of their scales. Two decimal64 add
 * exactly in 128 bits, so it cannot fail and returns 0.
 */
int ty_decimal64_add(const ty_decimal64_t* a, const ty_decimal64_t* b,
                     ty_decimal128_t* result);

#endif /* TY_DECIMAL_H */

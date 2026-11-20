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
 * A call that cannot fail returns its result, and is inline: the host's
 * wrapper then costs no call. Its argument is a decimal64 that holds at
 * most 17 digits.
 */

/* -a. */
static inline ty_decimal64_t ty_decimal64_neg(ty_decimal64_t a) {
    return {-a.value, a.scale};
}

/* |a|. */
static inline ty_decimal64_t ty_decimal64_abs(ty_decimal64_t a) {
    return {a.value < 0 ? -a.value : a.value, a.scale};
}

/* -1, 0 or 1: the sign of a. */
static inline int ty_decimal64_sign(ty_decimal64_t a) {
    return (a.value > 0) - (a.value < 0);
}

/* The most digits, and the most places, a decimal64 holds. */
#define TY_DECIMAL64_MAX_DIGITS 17
#define TY_DECIMAL64_MAX_SCALE  17

/*
 * *result = a + b, at the larger of their scales. Fails with
 * TY_ERR_RANGE if the sum needs more than 17 digits: *result is then
 * not set. To add without that limit, the caller makes one operand a
 * decimal128 and adds a decimal128 to a decimal64.
 */
int ty_decimal64_add(const ty_decimal64_t* a, const ty_decimal64_t* b,
                     ty_decimal64_t* result);

/* *result = a - b, as ty_decimal64_add() adds. */
int ty_decimal64_sub(const ty_decimal64_t* a, const ty_decimal64_t* b,
                     ty_decimal64_t* result);

/*
 * *result = a * b, at the sum of their scales. Fails with TY_ERR_RANGE
 * if the product needs more than 17 digits, or more than 17 places:
 * *result is then not set. It is exact or an error, as add and sub are.
 */
int ty_decimal64_mul(const ty_decimal64_t* a, const ty_decimal64_t* b,
                     ty_decimal64_t* result);

#endif /* TY_DECIMAL_H */

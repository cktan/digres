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

/*
 * a <=> b at any scales: -1, 0 or 1. The scales of both are at most 17.
 * Two values of one scale, the usual case, compare in line; others are
 * put to one scale in 128 bits, out of line.
 */
int ty_decimal64_cmp_scaled(ty_decimal64_t a, ty_decimal64_t b);

static inline int ty_decimal64_cmp(ty_decimal64_t a, ty_decimal64_t b) {
    if (a.scale == b.scale) {
        return (a.value > b.value) - (a.value < b.value);
    }
    return ty_decimal64_cmp_scaled(a, b);
}

/* The most digits, and the most places, a decimal128 holds. */
#define TY_DECIMAL128_MAX_DIGITS 36
#define TY_DECIMAL128_MAX_SCALE  36

/*
 * A decimal128 works as an int64 does beside an int32: a + b, a - b and
 * a * b of two decimal128 are a decimal128, exact, or TY_ERR_RANGE if the
 * result needs more than 36 digits, or a product more than 36 places. The
 * caller who wants more casts an operand to a wider type. *result is then
 * not set. The scales of both operands are at most 36.
 */
int ty_decimal128_add(const ty_decimal128_t* a, const ty_decimal128_t* b,
                      ty_decimal128_t* result);
int ty_decimal128_sub(const ty_decimal128_t* a, const ty_decimal128_t* b,
                      ty_decimal128_t* result);
int ty_decimal128_mul(const ty_decimal128_t* a, const ty_decimal128_t* b,
                      ty_decimal128_t* result);

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
 * *result = a % b, the remainder of the division of a by b, with the
 * sign of a, at the larger of their scales. Fails with TY_ERR_DIVZERO
 * if b is zero. The scales of both are at most 17.
 */
int ty_decimal64_mod(const ty_decimal64_t* a, const ty_decimal64_t* b,
                     ty_decimal64_t* result);

/*
 * *result = gcd(a, b): the largest value that both are whole multiples
 * of, at the larger of their scales. It is below 10^17 unless one operand
 * is zero and the other's value, put to the larger scale, is not: it then
 * fails with TY_ERR_RANGE. The scales of both are at most 17.
 */
int ty_decimal64_gcd(const ty_decimal64_t* a, const ty_decimal64_t* b,
                     ty_decimal64_t* result);

/*
 * *result = lcm(a, b): the smallest value that is a whole multiple of
 * both, at the larger of their scales; 0 if either is zero. Fails with
 * TY_ERR_RANGE if it needs more than 17 digits. The scales of both are
 * at most 17.
 */
int ty_decimal64_lcm(const ty_decimal64_t* a, const ty_decimal64_t* b,
                     ty_decimal64_t* result);

/*
 * *result = a rounded to n places, half away from zero: n is the result's
 * scale, as round(numeric, int) says. A larger n adds zeros. A negative n
 * rounds to tens, hundreds and so on, and gives scale 0. Fails with
 * TY_ERR_RANGE if the result needs more than 17 digits or n is above 17:
 * *result is then not set. The scale of a is at most 17.
 */
int ty_decimal64_round(const ty_decimal64_t* a, int n, ty_decimal64_t* result);

/* *result = a truncated to n places, as ty_decimal64_round() rounds. */
int ty_decimal64_trunc(const ty_decimal64_t* a, int n, ty_decimal64_t* result);

/*
 * The nearest whole number at or above a, and at or below a, at scale 0.
 * They cannot fail: a whole number has no more digits than a. The scale
 * of a is at most 17.
 */
ty_decimal64_t ty_decimal64_ceil(ty_decimal64_t a);
ty_decimal64_t ty_decimal64_floor(ty_decimal64_t a);

/*
 * *result = a * b, at the sum of their scales. Fails with TY_ERR_RANGE
 * if the product needs more than 17 digits, or more than 17 places:
 * *result is then not set. It is exact or an error, as add and sub are.
 */
int ty_decimal64_mul(const ty_decimal64_t* a, const ty_decimal64_t* b,
                     ty_decimal64_t* result);

#endif /* TY_DECIMAL_H */

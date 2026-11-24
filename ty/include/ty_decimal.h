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

typedef struct ty_dec64_t {
  int64_t value;
  int scale;
} ty_dec64_t;

typedef struct ty_dec128_t {
  ty_int128_t value;
  int scale;
} ty_dec128_t;

/* The largest value of a decimal64: 10^17 - 1; of a decimal128: 10^36 - 1. */
#define TY_DEC64_MAX_VALUE 99999999999999999LL
#define TY_DEC128_MAX_VALUE \
    ((ty_int128_t)1000000000000000000LL * 1000000000000000000LL - 1)

/*
 * A call that cannot fail returns its result, and is inline: the host's
 * wrapper then costs no call. Its argument is a decimal64 that holds at
 * most 17 digits.
 */

/* -a. */
static inline ty_dec64_t ty_dec64_neg(ty_dec64_t a) {
  return {-a.value, a.scale};
}

/* |a|. */
static inline ty_dec64_t ty_dec64_abs(ty_dec64_t a) {
  return {a.value < 0 ? -a.value : a.value, a.scale};
}

/* -1, 0 or 1: the sign of a. */
static inline int ty_dec64_sign(ty_dec64_t a) {
  return (a.value > 0) - (a.value < 0);
}

/*
 * a <=> b at any scales: -1, 0 or 1. The scales of both are at most 17.
 * Two values of one scale, the usual case, compare in line; others are
 * put to one scale in 128 bits, out of line.
 */
int ty_dec64_cmp_scaled(ty_dec64_t a, ty_dec64_t b);

static inline int ty_dec64_cmp(ty_dec64_t a, ty_dec64_t b) {
  if (a.scale == b.scale) {
    return (a.value > b.value) - (a.value < b.value);
  }
  return ty_dec64_cmp_scaled(a, b);
}

/* The most digits, and the most places, a decimal128 holds. */
#define TY_DEC128_MAX_DIGITS 36
#define TY_DEC128_MAX_SCALE  36

/*
 * A decimal128 works as an int64 does beside an int32: a + b, a - b and
 * a * b of two decimal128 are a decimal128, exact, or TY_ERR_RANGE if the
 * result needs more than 36 digits, or a product more than 36 places. The
 * caller who wants more casts an operand to a wider type. *result is then
 * not set. The scales of both operands are at most 36.
 */
int ty_dec128_add_scaled(const ty_dec128_t *a, const ty_dec128_t *b,
                         ty_dec128_t *result);
int ty_dec128_sub_scaled(const ty_dec128_t *a, const ty_dec128_t *b,
                         ty_dec128_t *result);

/*
 * The three are in line, as a host's wrapper then costs no call: two values
 * of one scale, the usual case, in the header; two scales, out of line.
 */
static inline int ty_dec128_add(const ty_dec128_t *a, const ty_dec128_t *b,
                                ty_dec128_t *result) {
  ty_int128_t r;

  if (a->scale != b->scale) {
    return ty_dec128_add_scaled(a, b, result);
  }
  if (TY_UNLIKELY(__builtin_add_overflow(a->value, b->value, &r) ||
                  r > TY_DEC128_MAX_VALUE || r < -TY_DEC128_MAX_VALUE)) {
    return ty_err_range();
  }
  result->value = r;
  result->scale = a->scale;
  return 0;
}

static inline int ty_dec128_sub(const ty_dec128_t *a, const ty_dec128_t *b,
                                ty_dec128_t *result) {
  ty_int128_t r;

  if (a->scale != b->scale) {
    return ty_dec128_sub_scaled(a, b, result);
  }
  if (TY_UNLIKELY(__builtin_sub_overflow(a->value, b->value, &r) ||
                  r > TY_DEC128_MAX_VALUE || r < -TY_DEC128_MAX_VALUE)) {
    return ty_err_range();
  }
  result->value = r;
  result->scale = a->scale;
  return 0;
}

static inline int ty_dec128_mul(const ty_dec128_t *a, const ty_dec128_t *b,
                                ty_dec128_t *result) {
  ty_int128_t p;
  int s = a->scale + b->scale;

  if (TY_UNLIKELY(__builtin_mul_overflow(a->value, b->value, &p) ||
                  s > TY_DEC128_MAX_SCALE || p > TY_DEC128_MAX_VALUE ||
                  p < -TY_DEC128_MAX_VALUE)) {
    return ty_err_range();
  }
  result->value = p;
  result->scale = s;
  return 0;
}

/* -a, |a| and the sign of a, of a decimal128. */
static inline ty_dec128_t ty_dec128_neg(ty_dec128_t a) {
  return {-a.value, a.scale};
}

static inline ty_dec128_t ty_dec128_abs(ty_dec128_t a) {
  return {a.value < 0 ? -a.value : a.value, a.scale};
}

static inline int ty_dec128_sign(ty_dec128_t a) {
  return (a.value > 0) - (a.value < 0);
}

/*
 * a <=> b of a decimal128, at any scales: -1, 0 or 1. Two values of one
 * scale compare in line; others out of line, where a value that does not
 * fit 128 bits at the larger scale is the larger in magnitude.
 */
int ty_dec128_cmp_scaled(ty_dec128_t a, ty_dec128_t b);

static inline int ty_dec128_cmp(ty_dec128_t a, ty_dec128_t b) {
  if (a.scale == b.scale) {
    return (a.value > b.value) - (a.value < b.value);
  }
  return ty_dec128_cmp_scaled(a, b);
}

/*
 * *result = a rounded, or truncated, to n places, as for a decimal64: n is
 * the scale, a negative n gives scale 0, and an n above 36 is 36. Fails
 * with TY_ERR_RANGE if the result needs more than 36 digits; *result is
 * then not set.
 */
int ty_dec128_round(const ty_dec128_t *a, int n, ty_dec128_t *result);
int ty_dec128_trunc(const ty_dec128_t *a, int n, ty_dec128_t *result);

/* The nearest whole number at or above, and at or below, a. */
ty_dec128_t ty_dec128_ceil(ty_dec128_t a);
ty_dec128_t ty_dec128_floor(ty_dec128_t a);

/*
 * a % b, gcd(a, b) and lcm(a, b) of two decimal128, at the larger of the
 * scales; the remainder has the sign of a, and an lcm is 0 if either is
 * zero. % fails with TY_ERR_DIVZERO if b is zero. gcd and lcm fail with
 * TY_ERR_RANGE if the result needs more than 36 digits, as gcd(0, x) can
 * at a scale that puts x past them, or an lcm of two large coprimes.
 * *result is then not set. The scales of both are at most 36.
 */
int ty_dec128_mod(const ty_dec128_t *a, const ty_dec128_t *b,
                  ty_dec128_t *result);
int ty_dec128_gcd(const ty_dec128_t *a, const ty_dec128_t *b,
                  ty_dec128_t *result);
int ty_dec128_lcm(const ty_dec128_t *a, const ty_dec128_t *b,
                  ty_dec128_t *result);

/*
 * *result = a / b at rscale places, rounded half away from zero, or cut at
 * rscale places if cut. The caller picks rscale (0 to 36). Fails with
 * TY_ERR_DIVZERO if b is zero, and with TY_ERR_RANGE if the quotient needs
 * more than 36 digits: *result is then not set. The scales of both are at
 * most 36. It takes no more than 128 bits: the quotient is made a place
 * at a time.
 */
int ty_dec128_div(const ty_dec128_t *a, const ty_dec128_t *b, int rscale,
                  bool cut, ty_dec128_t *result);

/*
 * *result = a / b of two decimal64, at rscale places and then fitted to a
 * decimal64: places past 17, and places that digits past 17 push out, are
 * rounded away, half away from zero; it fails with TY_ERR_RANGE only if
 * the integer part has more than 17 digits.
 */
int ty_dec64_div(const ty_dec64_t *a, const ty_dec64_t *b, int rscale,
                 ty_dec64_t *result);

/*
 * The functions of the exponent of a decimal128, at rscale places, which
 * the caller picks (0 to 36): sqrt is made a digit at a time, rounded half
 * up; the others are computed in double, some 15 digits, and rounded. Each
 * takes no more than 128 bits, and a result of more than 36 digits before
 * the point is TY_ERR_RANGE. A result that has more than 36 digits at
 * rscale places has fewer places, rounded. *result is not set on an error.
 *
 *   - sqrt fails with TY_ERR_INVALID if a is negative.
 *   - exp and the powers fail with TY_ERR_RANGE where a double overflows,
 *     and with TY_ERR_UNDERFLOW where it underflows: a result that is
 *     not zero, but is zero or below the smallest normal double.
 *   - ln and log fail with TY_ERR_INVALID if an argument is not above
 *     zero; log fails with TY_ERR_DIVZERO if the base is 1.
 *   - pow_int takes an integer power exactly while it fits 128 bits, then
 *     in double; base is not zero. pow_real is for a real exponent: the
 *     base is above zero, or below it and the exponent a whole number.
 *   - to_double is a value as a double, rounded once; from_double is a
 *     double as a value at rscale places. TY_ERR_RANGE if it is not
 *     finite or has 37 digits before the point.
 */
double ty_dec128_to_double(const ty_dec128_t *a);
int ty_dec128_from_double(double x, int rscale, ty_dec128_t *result);
int ty_dec128_sqrt(const ty_dec128_t *a, int rscale, ty_dec128_t *result);
int ty_dec128_exp(const ty_dec128_t *a, int rscale, ty_dec128_t *result);
int ty_dec128_ln(const ty_dec128_t *a, int rscale, ty_dec128_t *result);
int ty_dec128_log(const ty_dec128_t *base, const ty_dec128_t *a, int rscale,
                  ty_dec128_t *result);
int ty_dec128_pow_int(const ty_dec128_t *base, int64_t n, int rscale,
                      ty_dec128_t *result);
int ty_dec128_pow_real(const ty_dec128_t *base, const ty_dec128_t *exp,
                       int rscale, ty_dec128_t *result);

/*
 * The scale PostgreSQL's numeric gives a result that is not exact, from 0 to
 * 36: at least 16 significant digits, and not below the arguments' scales.
 * A caller passes it as the rscale of the functions above. They cannot fail.
 *
 *   - sqrt, ln and log are of arguments above zero; the scale of any other
 *     is a scale, so a caller may look for the error after.
 *   - var_scale is of a variance: num, at scale, over den.
 */
int ty_dec128_div_scale(const ty_dec128_t *a, const ty_dec128_t *b);
static inline int ty_dec64_div_scale(const ty_dec64_t *a, const ty_dec64_t *b) {
  ty_dec128_t x = {a->value, a->scale}, y = {b->value, b->scale};

  return ty_dec128_div_scale(&x, &y);
}
int ty_dec128_var_scale(double num, int scale, ty_int128_t den);
int ty_dec128_sqrt_scale(const ty_dec128_t *a);
int ty_dec128_exp_scale(const ty_dec128_t *a);
int ty_dec128_ln_scale(const ty_dec128_t *a);
int ty_dec128_log_scale(const ty_dec128_t *base, const ty_dec128_t *a);

/*
 * *result = base ^ exp, at PostgreSQL's scale for it. TY_ERR_INVALID, with
 * PostgreSQL's message, for zero to a negative power, and for a negative
 * number to a power that is not whole; TY_ERR_RANGE and TY_ERR_UNDERFLOW as
 * pow_int and pow_real.
 */
int ty_dec128_power(const ty_dec128_t *base, const ty_dec128_t *exp,
                    ty_dec128_t *result);

/*
 * a rounded to a whole number, half away from zero, in *out: TY_ERR_RANGE if
 * it is below lo or above hi.
 */
static inline int ty_dec128_to_int64(const ty_dec128_t *a, int64_t lo,
                                     int64_t hi, int64_t *out) {
  ty_dec128_t r = *a;

  if (a->scale != 0 && ty_dec128_round(a, 0, &r) != 0) {
    return -1;
  }
  if (TY_UNLIKELY(r.value < lo || r.value > hi)) {
    return ty_err_range();
  }
  *out = (int64_t)r.value;
  return 0;
}

/* The number of decimal digits of v; 0 has one. */
int ty_dec128_digits(ty_int128_t v);

/*
 * *v, of scale from, put to scale to, which is not below it: TY_ERR_RANGE if
 * it then has more than 36 digits.
 */
int ty_dec128_scale_up(ty_int128_t *v, int from, int to);

/*
 * *a rounded to scale, half away from zero, and checked against precision,
 * as an assignment to numeric(precision, scale) does: TY_ERR_RANGE, and *a
 * as it was, if it has more than precision - scale digits before the point.
 */
int ty_dec128_typmod(ty_dec128_t *a, int precision, int scale);

/*
 * n! as a decimal128, scale 0: TY_ERR_RANGE past 36 digits (n above 32),
 * TY_ERR_INVALID if n is negative.
 */
int ty_dec128_factorial(int64_t n, ty_dec128_t *result);

/*
 * The bucket of operand among count equal buckets between b1 and b2, in
 * either order: 0 before them, count + 1 after, else
 * floor((operand - low) * count / (high - low)) + 1, in 128 bits.
 * TY_ERR_INVALID if count is not above 0 or the bounds are equal;
 * TY_ERR_RANGE if the differences do not fit 36 digits.
 */
int ty_dec128_width_bucket(const ty_dec128_t *operand, const ty_dec128_t *b1,
                           const ty_dec128_t *b2, int32_t count,
                           int64_t *result);

/*
 * *result = a, a decimal128, fitted to a decimal64: places past 17, and
 * places that digits past 17 push out, are rounded away, half away from
 * zero. Fails with TY_ERR_RANGE only if the integer part has more than 17
 * digits. The scale of a is at most 36.
 */
int ty_dec64_from_dec128(const ty_dec128_t *a, ty_dec64_t *result);

/*
 * A hash of a value, 64 bits, by a seed: equal values hash alike whatever
 * their scales, so 1.5 and 1.50 do, and a decimal64 hashes as the
 * decimal128 of its value does. The low 32 bits with seed 0 are the
 * standard 32-bit hash; with another seed, another hash. It cannot
 * fail.
 */
uint64_t ty_dec128_hash(const ty_dec128_t *a, uint64_t seed);
uint64_t ty_dec64_hash(const ty_dec64_t *a, uint64_t seed);

/* The most digits, and the most places, a decimal64 holds. */
#define TY_DEC64_MAX_DIGITS 17
#define TY_DEC64_MAX_SCALE  17

/*
 * *result = a + b, at the larger of their scales. Fails with
 * TY_ERR_RANGE if the sum needs more than 17 digits: *result is then
 * not set. To add without that limit, the caller makes one operand a
 * decimal128 and adds a decimal128 to a decimal64.
 */
int ty_dec64_add_scaled(const ty_dec64_t *a, const ty_dec64_t *b,
                        ty_dec64_t *result);

static inline int ty_dec64_add(const ty_dec64_t *a, const ty_dec64_t *b,
                               ty_dec64_t *result) {
  int64_t r;

  if (a->scale != b->scale) {
    return ty_dec64_add_scaled(a, b, result);
  }
  if (TY_UNLIKELY(__builtin_add_overflow(a->value, b->value, &r) ||
                  r > TY_DEC64_MAX_VALUE || r < -TY_DEC64_MAX_VALUE)) {
    return ty_err_range();
  }
  result->value = r;
  result->scale = a->scale;
  return 0;
}

/* *result = a - b, as ty_dec64_add() adds. */
int ty_dec64_sub_scaled(const ty_dec64_t *a, const ty_dec64_t *b,
                        ty_dec64_t *result);

static inline int ty_dec64_sub(const ty_dec64_t *a, const ty_dec64_t *b,
                               ty_dec64_t *result) {
  int64_t r;

  if (a->scale != b->scale) {
    return ty_dec64_sub_scaled(a, b, result);
  }
  if (TY_UNLIKELY(__builtin_sub_overflow(a->value, b->value, &r) ||
                  r > TY_DEC64_MAX_VALUE || r < -TY_DEC64_MAX_VALUE)) {
    return ty_err_range();
  }
  result->value = r;
  result->scale = a->scale;
  return 0;
}

/*
 * *result = a % b, the remainder of the division of a by b, with the
 * sign of a, at the larger of their scales. Fails with TY_ERR_DIVZERO
 * if b is zero. The scales of both are at most 17.
 */
int ty_dec64_mod(const ty_dec64_t *a, const ty_dec64_t *b, ty_dec64_t *result);

/*
 * *result = gcd(a, b): the largest value that both are whole multiples
 * of, at the larger of their scales. It is below 10^17 unless one operand
 * is zero and the other's value, put to the larger scale, is not: it then
 * fails with TY_ERR_RANGE. The scales of both are at most 17.
 */
int ty_dec64_gcd(const ty_dec64_t *a, const ty_dec64_t *b, ty_dec64_t *result);

/*
 * *result = lcm(a, b): the smallest value that is a whole multiple of
 * both, at the larger of their scales; 0 if either is zero. Fails with
 * TY_ERR_RANGE if it needs more than 17 digits. The scales of both are
 * at most 17.
 */
int ty_dec64_lcm(const ty_dec64_t *a, const ty_dec64_t *b, ty_dec64_t *result);

/*
 * *result = a rounded to n places, half away from zero: n is the result's
 * scale, as round(numeric, int) says. A larger n adds zeros. A negative n
 * rounds to tens, hundreds and so on, and gives scale 0. Fails with
 * TY_ERR_RANGE if the result needs more than 17 digits or n is above 17:
 * *result is then not set. The scale of a is at most 17.
 */
int ty_dec64_round(const ty_dec64_t *a, int n, ty_dec64_t *result);

/* *result = a truncated to n places, as ty_dec64_round() rounds. */
int ty_dec64_trunc(const ty_dec64_t *a, int n, ty_dec64_t *result);

/*
 * The nearest whole number at or above a, and at or below a, at scale 0.
 * They cannot fail: a whole number has no more digits than a. The scale
 * of a is at most 17.
 */
ty_dec64_t ty_dec64_ceil(ty_dec64_t a);
ty_dec64_t ty_dec64_floor(ty_dec64_t a);

/*
 * *result = a * b, at the sum of their scales. Fails with TY_ERR_RANGE
 * if the product needs more than 17 digits, or more than 17 places:
 * *result is then not set. It is exact or an error, as add and sub are.
 */
static inline int ty_dec64_mul(const ty_dec64_t *a, const ty_dec64_t *b,
                               ty_dec64_t *result) {
  ty_int128_t p = (ty_int128_t)a->value * b->value;
  int s = a->scale + b->scale;

  if (TY_UNLIKELY(s > TY_DEC64_MAX_SCALE || p > TY_DEC64_MAX_VALUE ||
                  p < -TY_DEC64_MAX_VALUE)) {
    return ty_err_range();
  }
  result->value = (int64_t)p;
  result->scale = s;
  return 0;
}

/*
 * The text of a decimal128.
 *
 *   - format writes a's text as PostgreSQL prints a numeric, every place
 *     of its scale and no exponent, NUL-ended into TY_DEC_TEXT_SIZE bytes;
 *     it returns the length.
 *   - parse reads the NUL-ended text s: a decimal number, which may have
 *     an exponent and single underscores between digits, or an integer in
 *     base 16, 8 or 2 after 0x, 0o or 0b, with a sign and spaces around.
 *     precision and scale are a typmod's, precision -1 for none: the value
 *     is rounded to scale, half away from zero, and may have at most
 *     precision - scale digits before the point. Without one, places past
 *     36 are rounded away and zeros past 36 digits dropped; more digits
 *     are refused, not rounded. 0, or -1 with *why one of TY_DEC_IN_* and
 *     TY_ERR_INVALID (syntax, NaN) or TY_ERR_RANGE (the rest).
 *   - min_scale is the fewest places that hold a's value; trim_scale is a
 *     with no zeros at the end of its places.
 *   - to_float64 and to_float32 are the nearest float to a's text;
 *     from_float is x by its first digits significant digits, as
 *     PostgreSQL takes a float, "%.*g" read by parse: 15 for a float64, 6
 *     for a float32. NaN and the infinities are TY_DEC_IN_NAN.
 */
#define TY_DEC_TEXT_SIZE 48

enum {
  TY_DEC_IN_SYNTAX = 1, /* not a number */
  TY_DEC_IN_NAN,        /* NaN or an infinity, which a decimal has not */
  TY_DEC_IN_RANGE,      /* an exponent, or digits, past what is read */
  TY_DEC_IN_DIGITS,     /* more than 36 significant digits */
  TY_DEC_IN_FIELD       /* more digits before the point than a typmod's */
};

int ty_dec128_format(const ty_dec128_t *a, char *out);
int ty_dec128_parse(const char *s, int precision, int scale, ty_dec128_t *out,
                    int *why);
double ty_dec128_to_float64(const ty_dec128_t *a);
float ty_dec128_to_float32(const ty_dec128_t *a);
int ty_dec128_from_float(double x, int digits, ty_dec128_t *out, int *why);

static inline int ty_dec128_min_scale(const ty_dec128_t *a) {
  ty_int128_t v = a->value;
  int s = a->scale;

  while (s > 0 && 0 == v % 10) {
    v /= 10;
    s--;
  }
  return s;
}

static inline ty_dec128_t ty_dec128_trim_scale(ty_dec128_t a) {
  while (a.scale > 0 && 0 == a.value % 10) {
    a.value /= 10;
    a.scale--;
  }
  return a;
}

/*
 * A number as its decimal digits, before it is a value: what a binary form
 * is read into. No leading zeros, nd digits in dig (0 for zero), scale of
 * them places, and up to TY_DEC_DIGITS_MAX, so that a typmod can round the
 * places of a long input away.
 *
 *   - drop takes the last drop places off, rounded half away from zero by
 *     the first of them, or cut if cut is 1.
 *   - pad adds zeros after the point up to scale; -1 if too long.
 *   - typmod rounds d to a typmod's scale and refuses more than precision -
 *     scale digits before the point, with TY_ERR_RANGE.
 *   - pack makes the value: places past 36 rounded away, then digits past
 *     36 pushed out of the places; TY_ERR_RANGE if the integer part has
 *     more than 36 digits.
 */
#define TY_DEC_DIGITS_MAX 154

typedef struct ty_dec_digits_t {
  int nd;
  int scale;
  char dig[TY_DEC_DIGITS_MAX + 2];
} ty_dec_digits_t;

void ty_dec_digits_drop(ty_dec_digits_t *d, int drop, int cut);
int ty_dec_digits_pad(ty_dec_digits_t *d, int scale);
int ty_dec_digits_typmod(ty_dec_digits_t *d, int precision, int scale);
int ty_dec_digits_pack(ty_dec_digits_t *d, int neg, ty_dec128_t *out);

#endif /* TY_DECIMAL_H */

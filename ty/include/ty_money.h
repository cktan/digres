/*
 * Copyright (c) 2026 CK Tan
 * SPDX-License-Identifier: MIT
 */

/*
 * ty_money.h -- the operations of money: an int64 count of the smallest
 * unit (cents). The text of a money value depends on the locale
 * (lc_monetary), which the host gives as a ty_money_fmt_t, a copy of the
 * fields of C's struct lconv that PostgreSQL reads. The cases and the errors
 * follow PostgreSQL's cash.c.
 *
 * A call that can fail returns 0, or -1 with the thread's error set.
 */
#ifndef TY_MONEY_H
#define TY_MONEY_H

#include <math.h>
#include <stddef.h>
#include <stdint.h>

#include "ty.h"

/*
 * The fields of lconv that money text reads. A string is NUL-ended. Each
 * integer is lconv's: frac_digits and the *_precedes, *_sep_by_space and
 * *_sign_posn are small, or CHAR_MAX (127) for "not given"; mon_grouping is
 * the first byte of the grouping string.
 */
typedef struct ty_money_fmt_t {
    int frac_digits;
    const char* mon_decimal_point;
    const char* mon_thousands_sep;
    const char* currency_symbol;
    const char* positive_sign;
    const char* negative_sign;
    int mon_grouping;
    int p_cs_precedes, n_cs_precedes;
    int p_sep_by_space, n_sep_by_space;
    int p_sign_posn, n_sign_posn;
} ty_money_fmt_t;

/* The fields of the C locale (what a locale with no money settings gives). */
extern const ty_money_fmt_t ty_money_fmt_c;

/* The digits after the point: frac_digits, or 2 if it is not 0 to 10. */
static inline int ty_money_frac_digits(const ty_money_fmt_t* f) {
    return (f->frac_digits < 0 || f->frac_digits > 10) ? 2 : f->frac_digits;
}

/* 10 to the power of ty_money_frac_digits. */
int64_t ty_money_scale(const ty_money_fmt_t* f);

/*
 * Read money text: a currency symbol, a sign (a minus, a plus, or
 * parentheses) in the places a locale puts them, digits with thousands
 * separators, a decimal point and fraction digits (more than the locale has
 * are rounded on the first dropped digit). A bad text is TY_ERR_INVALID;
 * a value past int64 is TY_ERR_RANGE.
 */
int ty_money_parse(const char* s, const ty_money_fmt_t* f, int64_t* out);

/*
 * Write money text, as snprintf does: the length the text has (without the
 * NUL) is returned, and the text is written if it fits in cap bytes.
 */
size_t ty_money_format(int64_t v, const ty_money_fmt_t* f, char* out,
                       size_t cap);

/* a + b and a - b: TY_ERR_RANGE ("money out of range") on overflow. */
static inline int ty_money_add(int64_t a, int64_t b, int64_t* r) {
    if (__builtin_add_overflow(a, b, r)) {
        return ty_err_range();
    }
    return 0;
}
static inline int ty_money_sub(int64_t a, int64_t b, int64_t* r) {
    if (__builtin_sub_overflow(a, b, r)) {
        return ty_err_range();
    }
    return 0;
}

/* money times and over an integer; a division by zero is TY_ERR_DIVZERO. */
static inline int ty_money_mul_int64(int64_t c, int64_t i, int64_t* r) {
    if (__builtin_mul_overflow(c, i, r)) {
        return ty_err_range();
    }
    return 0;
}
int ty_money_div_int64(int64_t c, int64_t i, int64_t* r);

/*
 * money times and over a float: the float arithmetic of ty_float.h, rounded
 * to an integer; TY_ERR_RANGE if the result is not a number or does not fit.
 */
int ty_money_mul_float64(int64_t c, double f, int64_t* r);
int ty_money_div_float64(int64_t c, double f, int64_t* r);

/* money over money: a float; TY_ERR_DIVZERO if the divisor is 0. */
int ty_money_div_money(int64_t a, int64_t b, double* r);

/*
 * The value in English words, as cash_words: "Twelve dollars and
 * thirty-four cents". out must have 256 bytes; the length is returned.
 */
size_t ty_money_words(int64_t v, char* out);

#endif

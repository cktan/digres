/*
 * Copyright (c) 2026 CK Tan
 * SPDX-License-Identifier: MIT
 */

/*
 * ty_int.h -- the operations of int16, int32 and int64 (SQL's smallint,
 * integer and bigint). They are written for ty and take no code from
 * PostgreSQL; the cases and the errors are those of PostgreSQL's.
 *
 * The rule is the one of ty_decimal.h: an operation on two values of one
 * width gives that width, exact, or an error. int32 + int32 is an int32,
 * and TY_ERR_RANGE if it does not fit; a caller who wants an int64
 * widens an operand first. Nothing here rounds, and nothing wraps.
 *
 * A function that can fail returns 0, or -1 with the thread's error set,
 * and hands its value back through its last parameter, which it sets
 * only on success. One that cannot fail returns its result. The small
 * ones are in line, so a host's wrapper costs no call.
 *
 * What each function does is written once, below, for the name "N" as
 * 16, 32 or 64, and the type "T" as int16_t, int32_t or int64_t.
 *
 *   ty_intN_add(a, b, &r)     r = a + b.        RANGE on overflow.
 *   ty_intN_sub(a, b, &r)     r = a - b.        RANGE on overflow.
 *   ty_intN_mul(a, b, &r)     r = a * b.        RANGE on overflow.
 *   ty_intN_div(a, b, &r)     r = a / b, to zero. DIVZERO if b is 0;
 *                             RANGE for MIN / -1.
 *   ty_intN_mod(a, b, &r)     r = a % b, the sign of a. DIVZERO if b is
 *                             0; 0 for any a % -1.
 *   ty_intN_neg(a, &r)        r = -a.           RANGE for MIN.
 *   ty_intN_abs(a, &r)        r = |a|.          RANGE for MIN.
 *   ty_intN_inc(a, &r)        r = a + 1.        RANGE on overflow.
 *   ty_intN_dec(a, &r)        r = a - 1.        RANGE on overflow.
 *   ty_intN_gcd(a, b, &r)     r = the greatest common divisor, never
 *                             negative; gcd(0, 0) is 0. RANGE if it
 *                             would be -MIN.
 *   ty_intN_lcm(a, b, &r)     r = the least common multiple, never
 *                             negative; 0 if a or b is 0. RANGE if it
 *                             does not fit.
 *   ty_intN_larger(a, b)      the larger of a and b.     Cannot fail.
 *   ty_intN_smaller(a, b)     the smaller of a and b.    Cannot fail.
 *   ty_intN_and(a, b)         a & b, and ty_intN_or, ty_intN_xor.
 *   ty_intN_not(a)            ~a.                        Cannot fail.
 *   ty_intN_shl(a, n, &r)     r = a << n, the bits that leave are lost.
 *   ty_intN_shr(a, n, &r)     r = a >> n, the sign copied in. For both,
 *                             RANGE if n < 0 or n >= N: C leaves that
 *                             undefined, and PostgreSQL gives what the
 *                             machine gives.
 *
 * And between the widths:
 *
 *   ty_int32_to_int16(a, &r)  r = a.            RANGE if it does not
 *   ty_int64_to_int32(a, &r)                    fit; the same for
 *   ty_int64_to_int16(a, &r)                    each narrowing.
 *   ty_int64_to_double(a)     a as a double.             Cannot fail.
 *   ty_int64_from_double(d, &r)  r = d rounded to the nearest, a tie to
 *                             the even; RANGE if d is not a number or
 *                             does not fit.
 *
 * To widen, a caller assigns: int32_t to int64_t cannot fail.
 */
#ifndef TY_INT_H
#define TY_INT_H

#include <stdint.h>

#include "ty.h"

/* The out-of-line parts: gcd and lcm. */
int ty_int16_gcd(int16_t a, int16_t b, int16_t* r);
int ty_int32_gcd(int32_t a, int32_t b, int32_t* r);
int ty_int64_gcd(int64_t a, int64_t b, int64_t* r);
int ty_int16_lcm(int16_t a, int16_t b, int16_t* r);
int ty_int32_lcm(int32_t a, int32_t b, int32_t* r);
int ty_int64_lcm(int64_t a, int64_t b, int64_t* r);
int ty_int64_from_double(double d, int64_t* r);

/*
 * The in-line operations of one width: N is the number of bits, T the
 * type, MIN its smallest value, and U its unsigned type, which does the
 * shift left without leaving C's defined cases.
 */
#define TY_INT_OPS(N, T, MIN, U)                                             \
    static inline int ty_int##N##_add(T a, T b, T* r) {                      \
        T v;                                                                 \
        if (TY_UNLIKELY(__builtin_add_overflow(a, b, &v))) {                 \
            return ty_err_range();                                           \
        }                                                                    \
        *r = v;                                                              \
        return 0;                                                            \
    }                                                                        \
    static inline int ty_int##N##_sub(T a, T b, T* r) {                      \
        T v;                                                                 \
        if (TY_UNLIKELY(__builtin_sub_overflow(a, b, &v))) {                 \
            return ty_err_range();                                           \
        }                                                                    \
        *r = v;                                                              \
        return 0;                                                            \
    }                                                                        \
    static inline int ty_int##N##_mul(T a, T b, T* r) {                      \
        T v;                                                                 \
        if (TY_UNLIKELY(__builtin_mul_overflow(a, b, &v))) {                 \
            return ty_err_range();                                           \
        }                                                                    \
        *r = v;                                                              \
        return 0;                                                            \
    }                                                                        \
    static inline int ty_int##N##_div(T a, T b, T* r) {                      \
        if (TY_UNLIKELY(b == 0)) {                                           \
            return ty_err_divzero();                                         \
        }                                                                    \
        if (TY_UNLIKELY(b == -1)) {                                          \
            /* MIN / -1 does not fit, and traps on some machines. */         \
            if (a == MIN) {                                                  \
                return ty_err_range();                                       \
            }                                                                \
            *r = (T)-a;                                                      \
            return 0;                                                        \
        }                                                                    \
        *r = (T)(a / b);                                                     \
        return 0;                                                            \
    }                                                                        \
    static inline int ty_int##N##_mod(T a, T b, T* r) {                      \
        if (TY_UNLIKELY(b == 0)) {                                           \
            return ty_err_divzero();                                         \
        }                                                                    \
        /* MIN % -1 is 0, and traps on some machines. */                     \
        *r = (b == -1) ? (T)0 : (T)(a % b);                                  \
        return 0;                                                            \
    }                                                                        \
    static inline int ty_int##N##_neg(T a, T* r) {                           \
        if (TY_UNLIKELY(a == MIN)) {                                         \
            return ty_err_range();                                           \
        }                                                                    \
        *r = (T)-a;                                                          \
        return 0;                                                            \
    }                                                                        \
    static inline int ty_int##N##_abs(T a, T* r) {                           \
        if (TY_UNLIKELY(a == MIN)) {                                         \
            return ty_err_range();                                           \
        }                                                                    \
        *r = (T)(a < 0 ? -a : a);                                            \
        return 0;                                                            \
    }                                                                        \
    static inline int ty_int##N##_inc(T a, T* r) {                           \
        return ty_int##N##_add(a, 1, r);                                     \
    }                                                                        \
    static inline int ty_int##N##_dec(T a, T* r) {                           \
        return ty_int##N##_sub(a, 1, r);                                     \
    }                                                                        \
    static inline T ty_int##N##_larger(T a, T b) { return a > b ? a : b; }   \
    static inline T ty_int##N##_smaller(T a, T b) { return a < b ? a : b; }  \
    static inline T ty_int##N##_and(T a, T b) { return (T)(a & b); }         \
    static inline T ty_int##N##_or(T a, T b) { return (T)(a | b); }          \
    static inline T ty_int##N##_xor(T a, T b) { return (T)(a ^ b); }         \
    static inline T ty_int##N##_not(T a) { return (T)~a; }                   \
    static inline int ty_int##N##_shl(T a, int n, T* r) {                    \
        if (TY_UNLIKELY(n < 0 || n >= N)) {                                  \
            return ty_err_range();                                           \
        }                                                                    \
        *r = (T)(U)((U)a << n);                                              \
        return 0;                                                            \
    }                                                                        \
    static inline int ty_int##N##_shr(T a, int n, T* r) {                    \
        if (TY_UNLIKELY(n < 0 || n >= N)) {                                  \
            return ty_err_range();                                           \
        }                                                                    \
        *r = (T)(a >> n);                                                    \
        return 0;                                                            \
    }

TY_INT_OPS(16, int16_t, INT16_MIN, uint16_t)
TY_INT_OPS(32, int32_t, INT32_MIN, uint32_t)
TY_INT_OPS(64, int64_t, INT64_MIN, uint64_t)

#undef TY_INT_OPS

/* Narrow a value: RANGE if it does not fit the smaller type. */
static inline int ty_int32_to_int16(int32_t a, int16_t* r) {
    if (TY_UNLIKELY(a < INT16_MIN || a > INT16_MAX)) {
        return ty_err_range();
    }
    *r = (int16_t)a;
    return 0;
}

static inline int ty_int64_to_int32(int64_t a, int32_t* r) {
    if (TY_UNLIKELY(a < INT32_MIN || a > INT32_MAX)) {
        return ty_err_range();
    }
    *r = (int32_t)a;
    return 0;
}

static inline int ty_int64_to_int16(int64_t a, int16_t* r) {
    if (TY_UNLIKELY(a < INT16_MIN || a > INT16_MAX)) {
        return ty_err_range();
    }
    *r = (int16_t)a;
    return 0;
}

/* An int64 as a double: the nearest, as C converts it. */
static inline double ty_int64_to_double(int64_t a) { return (double)a; }

#endif /* TY_INT_H */

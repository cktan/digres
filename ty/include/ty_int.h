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
 *   ty_intN_cmp(a, b)         -1, 0 or 1 as a is below, equal to or above
 *                             b.                          Cannot fail.
 *   ty_intN_eq, _ne, _lt, _le, _gt, _ge (a, b)   1 or 0.  Cannot fail.
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
 * To widen, a caller assigns: int32_t to int64_t cannot fail. So a compare
 * of an int16 and an int64 is ty_int64_lt(a, b), as C does it.
 *
 * And for the window frame of RANGE offsets:
 *
 *   ty_int_in_range(val, base, offset, sub, less, &r)
 *                             r = 1 if val is within the offset of base:
 *                             val <= base + offset if less and not sub,
 *                             val >= base + offset if not less, and
 *                             base - offset in place of base + offset if
 *                             sub. TY_ERR_INVALID if offset is negative.
 *                             The sum cannot overflow: when it would, r is
 *                             what the true sum gives. Done in int64, which
 *                             any width fits.
 *
 * And the text of an integer:
 *
 *   ty_intN_parse(s, len, &r) r = the integer in the len bytes at s, which
 *                             need not end in NUL. Space may stand at both
 *                             ends; a sign, '-' or '+'; then decimal digits,
 *                             or "0x", "0o" or "0b" and digits of base 16,
 *                             8 or 2. A single '_' may stand between
 *                             digits. TY_ERR_RANGE if the value does not fit
 *                             (reported as soon as the digits pass what the
 *                             type holds, before a later fault), and
 *                             TY_ERR_INVALID for any other text.
 *   ty_intN_format(a, out)    writes a in decimal and a NUL to out, which
 *                             has room for TY_INTN_TEXT_SIZE; returns the
 *                             length, without the NUL.
 *   ty_uint32_format_n, ty_uint64_format_n (v, out)
 *                             the digits of an unsigned value, with no NUL;
 *                             returns how many (at most 10, and 20).
 *
 * And the hash:
 *
 *   ty_intN_hash(a, seed)     a 64-bit hash of a by a seed. Equal values
 *                             hash alike whatever the width, so an int16,
 *                             an int32 and an int64 of one value are one
 *                             hash. The low 32 bits with seed 0 are the
 *                             standard 32-bit hash. It cannot fail.
 */
#ifndef TY_INT_H
#define TY_INT_H

#include <stdint.h>

#include "ty.h"

/* The size of the buffer ty_intN_format needs: the sign, digits and NUL. */
#define TY_INT16_TEXT_SIZE 7
#define TY_INT32_TEXT_SIZE 12
#define TY_INT64_TEXT_SIZE 21

int ty_int16_parse(const char *s, size_t len, int16_t *r);
int ty_int32_parse(const char *s, size_t len, int32_t *r);
int ty_int64_parse(const char *s, size_t len, int64_t *r);

/* The same for a string that ends at its NUL, which saves the host a strlen. */
int ty_int16_parse_z(const char *s, int16_t *r);
int ty_int32_parse_z(const char *s, int32_t *r);
int ty_int64_parse_z(const char *s, int64_t *r);
size_t ty_int16_format(int16_t a, char *out);
size_t ty_int32_format(int32_t a, char *out);
size_t ty_int64_format(int64_t a, char *out);
size_t ty_uint32_format_n(uint32_t v, char *out);
size_t ty_uint64_format_n(uint64_t v, char *out);

/* The hash of an int64, and so of every width: out of line. */
uint64_t ty_int64_hash(int64_t a, uint64_t seed);

/* The out-of-line parts: gcd and lcm. */
int ty_int16_gcd(int16_t a, int16_t b, int16_t *r);
int ty_int32_gcd(int32_t a, int32_t b, int32_t *r);
int ty_int64_gcd(int64_t a, int64_t b, int64_t *r);
int ty_int16_lcm(int16_t a, int16_t b, int16_t *r);
int ty_int32_lcm(int32_t a, int32_t b, int32_t *r);
int ty_int64_lcm(int64_t a, int64_t b, int64_t *r);
int ty_int64_from_double(double d, int64_t *r);

/*
 * The in-line operations of one width: N is the number of bits, T the
 * type, MIN its smallest value, and U its unsigned type, which does the
 * shift left without leaving C's defined cases.
 */
/* clang-format 18 moves the comments in this macro body, though
 * SkipMacroDefinitionBody is set: keep it out of the formatter. */
/* clang-format off */
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
    static inline int ty_int##N##_cmp(T a, T b) { return (a > b) - (a < b); } \
    static inline int ty_int##N##_eq(T a, T b) { return a == b; }            \
    static inline int ty_int##N##_ne(T a, T b) { return a != b; }            \
    static inline int ty_int##N##_lt(T a, T b) { return a < b; }             \
    static inline int ty_int##N##_le(T a, T b) { return a <= b; }            \
    static inline int ty_int##N##_gt(T a, T b) { return a > b; }             \
    static inline int ty_int##N##_ge(T a, T b) { return a >= b; }            \
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
/* clang-format on */

TY_INT_OPS(16, int16_t, INT16_MIN, uint16_t)
TY_INT_OPS(32, int32_t, INT32_MIN, uint32_t)
TY_INT_OPS(64, int64_t, INT64_MIN, uint64_t)

#undef TY_INT_OPS

/* Narrow a value: RANGE if it does not fit the smaller type. */
static inline int ty_int32_to_int16(int32_t a, int16_t *r) {
  if (TY_UNLIKELY(a < INT16_MIN || a > INT16_MAX)) {
    return ty_err_range();
  }
  *r = (int16_t)a;
  return 0;
}

static inline int ty_int64_to_int32(int64_t a, int32_t *r) {
  if (TY_UNLIKELY(a < INT32_MIN || a > INT32_MAX)) {
    return ty_err_range();
  }
  *r = (int32_t)a;
  return 0;
}

static inline int ty_int64_to_int16(int64_t a, int16_t *r) {
  if (TY_UNLIKELY(a < INT16_MIN || a > INT16_MAX)) {
    return ty_err_range();
  }
  *r = (int16_t)a;
  return 0;
}

/*
 * Whether val is within offset of base, for a window frame of RANGE offsets;
 * see above.
 */
static inline int ty_int_in_range(int64_t val, int64_t base, int64_t offset,
                                  int sub, int less, int *r) {
  int64_t sum;

  if (TY_UNLIKELY(offset < 0)) {
    return ty_err_invalid("invalid preceding or following size");
  }
  if (sub) {
    offset = -offset; /* cannot overflow: offset is not negative */
  }
  if (TY_UNLIKELY(__builtin_add_overflow(base, offset, &sum))) {
    /*
     * Without sub the true sum is more than any val, with it less, so
     * the answer is that of a far bound.
     */
    *r = sub ? !less : less;
    return 0;
  }
  *r = less ? val <= sum : val >= sum;
  return 0;
}

/* The hash of an int16 or an int32: that of the int64 of its value. */
static inline uint64_t ty_int16_hash(int16_t a, uint64_t seed) {
  return ty_int64_hash(a, seed);
}

static inline uint64_t ty_int32_hash(int32_t a, uint64_t seed) {
  return ty_int64_hash(a, seed);
}

/* An int64 as a double: the nearest, as C converts it. */
static inline double ty_int64_to_double(int64_t a) { return (double)a; }

/*
 * The state of avg and sum over int2 and int4, and of their moving frames: the
 * number of values, and the sum of them in an int64. The sum of values of 32
 * bits needs 2^32 values to overflow an int64, and PostgreSQL does not check:
 * the sum wraps, here without the undefined behavior of a signed overflow. A
 * caller keeps the state, zeroed, and passes only values that are not NULL.
 */
typedef struct ty_int_avg_t {
  int64_t count;
  int64_t sum;
} ty_int_avg_t;

/* A value enters the state, and leaves it (the moving frame). */
static inline void ty_int_avg_accum(ty_int_avg_t *st, int64_t v) {
  st->count++;
  st->sum = (int64_t)((uint64_t)st->sum + (uint64_t)v);
}
static inline void ty_int_avg_accum_inv(ty_int_avg_t *st, int64_t v) {
  st->count--;
  st->sum = (int64_t)((uint64_t)st->sum - (uint64_t)v);
}

/* Add the state b into the state a (the parts of a parallel aggregate). */
static inline void ty_int_avg_combine(ty_int_avg_t *a, const ty_int_avg_t *b) {
  a->count += b->count;
  a->sum = (int64_t)((uint64_t)a->sum + (uint64_t)b->sum);
}

/* The state has no value: avg and sum are NULL. */
static inline int ty_int_avg_empty(const ty_int_avg_t *st) {
  return st->count == 0;
}

/*
 * The running sum of sum(int2) and sum(int4) in plain mode: the sum so far
 * and a new value, with the same wrap.
 */
static inline int64_t ty_int_sum_add(int64_t sum, int64_t v) {
  return (int64_t)((uint64_t)sum + (uint64_t)v);
}

#endif /* TY_INT_H */

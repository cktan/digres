/*
 * Copyright (c) 2026 CK Tan
 * SPDX-License-Identifier: MIT
 */

/*
 * float.cc -- the out-of-line operations of float32 and float64.
 */
#include "ty_float.h"

#include <errno.h>
#include <float.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "error.h"
#include "ty_hash.h"
#include "ryu/ty_rename.h"
#include "ryu/common/shortest_dec.h"
#include "ty_int.h"

namespace {

/* Record the invalid argument whose text is msg; returns -1. */
int invalid(const char *msg) {
  return ty_internal::set_error(TY_ERR_INVALID, msg);
}

/* True if the double a has no integer value, or is infinite or NaN. */
bool is_whole(double a) { return floor(a) == a; }

} // namespace

/* A float64 as an int16: rounded, and checked against the range. */
int ty_float64_to_int16(double a, int16_t *r) {
  double n = rint(a);

  if (isnan(n) || n < -32768.0 || n >= 32768.0) {
    return ty_err_range();
  }
  *r = (int16_t)n;
  return 0;
}

/* A float64 as an int32. */
int ty_float64_to_int32(double a, int32_t *r) {
  double n = rint(a);

  if (isnan(n) || n < -2147483648.0 || n >= 2147483648.0) {
    return ty_err_range();
  }
  *r = (int32_t)n;
  return 0;
}

/* The square root; a negative number has none. */
int ty_float64_sqrt_slow(double a, double *r) {
  if (a < 0) {
    return invalid("cannot take square root of a negative number");
  }
  double v = sqrt(a);

  if (isinf(v) && !isinf(a)) {
    return ty_err_range();
  }
  if (v == 0.0 && a != 0.0) {
    return ty_err_underflow();
  }
  *r = v;
  return 0;
}

/* The cube root. */
int ty_float64_cbrt_slow(double a, double *r) {
  double v = cbrt(a);

  if (isinf(v) && !isinf(a)) {
    return ty_err_range();
  }
  if (v == 0.0 && a != 0.0) {
    return ty_err_underflow();
  }
  *r = v;
  return 0;
}

/*
 * e to the power a.
 * 1. NaN gives NaN, and infinity gives itself, or 0 for -infinity.
 * 2. Else call exp, and report a result of infinity or of zero as an
 *    overflow or an underflow, as the libm's ERANGE says it.
 */
int ty_float64_exp_slow(double a, double *r) {
  if (isnan(a)) {
    *r = a;
    return 0;
  }
  if (isinf(a)) {
    *r = a > 0.0 ? a : 0.0;
    return 0;
  }
  errno = 0;
  double v = exp(a);

  if (errno == ERANGE) {
    return v != 0.0 ? ty_err_range() : ty_err_underflow();
  }
  if (isinf(v)) {
    return ty_err_range();
  }
  if (v == 0.0) {
    return ty_err_underflow();
  }
  *r = v;
  return 0;
}

/* The natural logarithm. */
int ty_float64_ln_slow(double a, double *r) {
  if (a == 0.0) {
    return invalid("cannot take logarithm of zero");
  }
  if (a < 0) {
    return invalid("cannot take logarithm of a negative number");
  }
  double v = log(a);

  if (isinf(v) && !isinf(a)) {
    return ty_err_range();
  }
  if (v == 0.0 && a != 1.0) {
    return ty_err_underflow();
  }
  *r = v;
  return 0;
}

/* The base 10 logarithm. */
int ty_float64_log10_slow(double a, double *r) {
  if (a == 0.0) {
    return invalid("cannot take logarithm of zero");
  }
  if (a < 0) {
    return invalid("cannot take logarithm of a negative number");
  }
  double v = log10(a);

  if (isinf(v) && !isinf(a)) {
    return ty_err_range();
  }
  if (v == 0.0 && a != 1.0) {
    return ty_err_underflow();
  }
  *r = v;
  return 0;
}

/*
 * a to the power b, with the cases that POSIX leaves to the platform decided
 * here, so that every platform gives one answer.
 * 1. NaN: NaN ^ 0 and 1 ^ NaN are 1; any other NaN gives NaN.
 * 2. 0 ^ negative and negative ^ non-integer are invalid.
 * 3. An infinite b, then an infinite a, by the rules of POSIX; the sign of
 *    (-inf) ^ b is that of b's oddness.
 * 4. Else call pow, and report ERANGE, an infinite result, or a zero result
 *    from a nonzero a as an overflow or an underflow.
 */
int ty_float64_pow_slow(double a, double b, double *r) {
  double v;

  if (isnan(a)) {
    *r = (isnan(b) || b != 0.0) ? __builtin_nan("") : 1.0;
    return 0;
  }
  if (isnan(b)) {
    *r = a != 1.0 ? __builtin_nan("") : 1.0;
    return 0;
  }
  if (a == 0 && b < 0) {
    return invalid("zero raised to a negative power is undefined");
  }
  if (a < 0 && !is_whole(b)) {
    return invalid("a negative number raised to a non-integer power yields a "
                   "complex result");
  }
  if (isinf(b)) {
    double absx = fabs(a);

    if (absx == 1.0) {
      v = 1.0;
    } else if (b > 0.0) {
      v = absx > 1.0 ? b : 0.0;
    } else {
      v = absx > 1.0 ? 0.0 : -b;
    }
  } else if (isinf(a)) {
    if (b == 0.0) {
      v = 1.0;
    } else if (a > 0.0) {
      v = b > 0.0 ? a : 0.0;
    } else {
      double half = b / 2;
      bool odd = floor(half) != half;

      if (b > 0.0) {
        v = odd ? a : -a;
      } else {
        v = odd ? -0.0 : 0.0;
      }
    }
  } else {
    errno = 0;
    v = pow(a, b);
    if (errno == EDOM || isnan(v)) {
      /* the cases above are the domain errors; this is a libm's fault */
      if (a == 0.0) {
        v = 0.0;
      } else {
        double absx = fabs(a);

        if (absx == 1.0) {
          v = 1.0;
        } else if (b >= 0.0 ? absx > 1.0 : absx < 1.0) {
          return ty_err_range();
        } else {
          return ty_err_underflow();
        }
      }
    } else if (errno == ERANGE) {
      return v != 0.0 ? ty_err_range() : ty_err_underflow();
    } else {
      if (isinf(v)) {
        return ty_err_range();
      }
      if (v == 0.0 && a != 0.0) {
        return ty_err_underflow();
      }
    }
  }
  *r = v;
  return 0;
}

/*
 * The bucket for operand between bound1 and bound2, in count parts.
 * 1. Refuse a count of 0 or less, a NaN, an infinite bound, equal bounds.
 * 2. Ascending bounds: below the first is 0, at or past the other is
 *    count + 1.
 * 3. Between: count times the fraction of the way, and one more; the
 *    fraction can round to 1, so cap it at count; if the bounds' difference
 *    overflows, halve everything first.
 * 4. Descending bounds: the same, mirrored.
 */
int ty_float64_width_bucket(double operand, double bound1, double bound2,
                            int32_t count, int32_t *r) {
  int32_t v;

  if (count <= 0) {
    return invalid("count must be greater than zero");
  }
  if (isnan(operand) || isnan(bound1) || isnan(bound2)) {
    return invalid("operand, lower bound, and upper bound cannot be NaN");
  }
  if (isinf(bound1) || isinf(bound2)) {
    return invalid("lower and upper bounds must be finite");
  }
  if (bound1 < bound2) {
    if (operand < bound1) {
      v = 0;
    } else if (operand >= bound2) {
      if (__builtin_add_overflow(count, 1, &v)) {
        return ty_err_range();
      }
    } else {
      if (!isinf(bound2 - bound1)) {
        v = (int32_t)(count * ((operand - bound1) / (bound2 - bound1)));
      } else {
        v = (int32_t)(count *
                      ((operand / 2 - bound1 / 2) / (bound2 / 2 - bound1 / 2)));
      }
      if (v >= count) {
        v = count - 1;
      }
      v++;
    }
  } else if (bound1 > bound2) {
    if (operand > bound1) {
      v = 0;
    } else if (operand <= bound2) {
      if (__builtin_add_overflow(count, 1, &v)) {
        return ty_err_range();
      }
    } else {
      if (!isinf(bound1 - bound2)) {
        v = (int32_t)(count * ((bound1 - operand) / (bound1 - bound2)));
      } else {
        v = (int32_t)(count *
                      ((bound1 / 2 - operand / 2) / (bound1 / 2 - bound2 / 2)));
      }
      if (v >= count) {
        v = count - 1;
      }
      v++;
    }
  } else {
    return invalid("lower bound cannot equal upper bound");
  }
  *r = v;
  return 0;
}

/*
 * The hash of a float64, as PostgreSQL's.
 * 1. 0 and -0 are one value: the hash is the seed.
 * 2. Every NaN is the quiet NaN of bits 0x7ff8000000000000.
 * 3. Hash the 8 bytes of the double, in the machine's order.
 */
uint64_t ty_float64_hash(double a, uint64_t seed) {
  if (a == 0.0) {
    return seed;
  }
  if (isnan(a)) {
    a = __builtin_nan("");
  }
  uint8_t key[8];

  memcpy(key, &a, 8);
  return ty_hash_bytes(key, 8, seed);
}

/* The hash of a float32: that of the float64 of its value. */
uint64_t ty_float32_hash(float a, uint64_t seed) {
  return ty_float64_hash((double)a, seed);
}

namespace {

/* Write v as "%.{ndig}g", with PostgreSQL's NaN and Infinity; ndig is 1 to 32.
 */
size_t format_g(double v, int ndig, char *out) {
  char *p = out;

  if (isnan(v)) {
    memcpy(out, "NaN", 4);
    return 3;
  }
  if (ndig < 1) {
    ndig = 1;
  } else if (ndig > 32) {
    ndig = 32;
  }
  if (signbit(v)) {
    *p++ = '-';
    v = -v;
  }
  if (isinf(v)) {
    memcpy(p, "Infinity", 9);
    return (size_t)(p - out) + 8;
  }
  int n = snprintf(p, TY_FLOAT_TEXT_SIZE - (size_t)(p - out), "%.*g", ndig, v);

  return (size_t)(p - out) + (size_t)(n < 0 ? 0 : n);
}

/* True for the bytes C's isspace() takes in the "C" locale. */
bool is_space(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' ||
         c == '\r';
}

/* True if the text at s begins with word, ignoring case; word is lowercase. */
bool begins_with(const char *s, const char *word) {
  for (; *word; word++, s++) {
    if ((*s | 0x20) != *word) {
      return false;
    }
  }
  return true;
}

/*
 * Read a float of type T; STRTO is strtod or strtof, HUGE the largest
 * magnitude.
 * 1. Skip space; an empty text is invalid.
 * 2. Call strtod. If it saw nothing, or set errno, check for the names
 *    NaN, Infinity and inf, with a sign, which not every libc takes.
 * 3. Else, for ERANGE: a result of zero or of infinity is out of range;
 *    a number too small for full precision is accepted.
 * 4. Skip space; report where we stopped, or refuse text after.
 */
template <typename T, T (*STRTO)(const char *, char **)>
int parse_float(const char *num, T *r, const char **end, size_t *len, T huge) {
  char *endptr;
  T val;

  while (*num != '\0' && is_space(*num)) {
    num++;
  }
  if (*num == '\0') {
    return ty_err_invalid("invalid float text");
  }
  errno = 0;
  val = STRTO(num, &endptr);
  if (endptr == (char *)num || errno != 0) {
    int save = errno;
    const char *sign = (*num == '+' || *num == '-') ? num + 1 : num;
    bool neg = *num == '-';

    if (begins_with(num, "nan")) {
      val = (T)__builtin_nan("");
      endptr = (char *)num + 3;
    } else if (begins_with(sign, "infinity")) {
      val = neg ? -(T)INFINITY : (T)INFINITY;
      endptr = (char *)sign + 8;
    } else if (begins_with(sign, "inf")) {
      val = neg ? -(T)INFINITY : (T)INFINITY;
      endptr = (char *)sign + 3;
    } else if (save == ERANGE) {
      if (val == 0.0 || val >= huge || val <= -huge) {
        if (len) {
          *len = (size_t)(endptr - num);
        }
        return ty_err_range();
      }
    } else {
      return ty_err_invalid("invalid float text");
    }
  }
  if (len) {
    *len = (size_t)(endptr - num);
  }
  while (*endptr != '\0' && is_space(*endptr)) {
    endptr++;
  }
  if (end) {
    *end = endptr;
  } else if (*endptr != '\0') {
    return ty_err_invalid("invalid float text");
  }
  *r = val;
  return 0;
}

} // namespace

/* The text of a float32. */
size_t ty_float32_format(float a, int extra_digits, char *out) {
  if (extra_digits > 0) {
    return (size_t)float_to_shortest_decimal_buf(a, out);
  }
  return format_g((double)a, FLT_DIG + extra_digits, out);
}

/* The text of a float64. */
size_t ty_float64_format(double a, int extra_digits, char *out) {
  if (extra_digits > 0) {
    return (size_t)double_to_shortest_decimal_buf(a, out);
  }
  return format_g(a, DBL_DIG + extra_digits, out);
}

/* Read a float32, as strtof does, so a decimal rounds once. */
int ty_float32_parse(const char *s, float *r, const char **end, size_t *len) {
  return parse_float<float, strtof>(s, r, end, len, HUGE_VALF);
}

/* Read a float64. */
int ty_float64_parse(const char *s, double *r, const char **end, size_t *len) {
  return parse_float<double, strtod>(s, r, end, len, HUGE_VAL);
}

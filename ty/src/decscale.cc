/*
 * Copyright (c) 2026 CK Tan
 * SPDX-License-Identifier: MIT
 */

/*
 * decscale.cc -- the scale of a result that is not exact, by PostgreSQL's
 * rules for its numeric; the power; and a typmod (ty_decimal.h).
 */
#include <math.h>

#include "error.h"
#include "ty_decimal.h"

namespace {

__extension__ typedef unsigned __int128 u128;

/* PostgreSQL's NUMERIC_MIN_SIG_DIGITS: the digits a result has at least. */
const int min_sig_digits = 16;

/* The largest scale, and PostgreSQL's limit on a result's weight. */
const int max_scale = 36;
const int max_result_scale = 2000;

/* log10(e), which takes a natural logarithm to a count of digits. */
const double log10_e = 0.434294481903252;

/*
 * A value in base 10,000, as PostgreSQL's numeric holds it: the weight of
 * its first digit that is not zero, that digit, the next, and whether there
 * are more after the first. PostgreSQL's rules are written in these. Zero
 * has weight 0 and digit 0.
 */
struct nbase_t {
  int weight;
  int first;
  int second;
  int ndigits; /* 1 if nothing follows the first digit, else 2 */
};

/* v without its sign. */
u128 magnitude(ty_int128_t v) { return v < 0 ? -(u128)v : (u128)v; }

/* The number of decimal digits of u; 0 has one. */
int digits_of(u128 u) {
  int n = 1;

  for (; u >= 10; u /= 10) {
    n++;
  }
  return n;
}

/* The weight in base 10,000 of the power e of ten. */
int weight_of(int e) { return e >= 0 ? e / 4 : -((-e + 3) / 4); }

/*
 * The base-10,000 form of v at scale.
 * 1. The weight, by the power of ten of the first digit.
 * 2. The value over 10000^weight, with 8 more digits: they hold the first
 *    two digits in base 10,000, and what is left says if there are more.
 */
nbase_t nbase_of(ty_int128_t v, int scale) {
  nbase_t nb = {0, 0, 0, 1};
  u128 m, head, rest = 0, p = 1;

  if (0 == v) {
    return nb;
  }

  // 1. The weight, by the power of ten of the first digit.
  m = magnitude(v);
  nb.weight = weight_of(digits_of(m) - 1 - scale);

  // 2. The value over 10000^weight, with 8 more digits.
  int shift = scale + 4 * nb.weight - 8;

  for (int i = 0; i < (shift >= 0 ? shift : -shift); i++) {
    p *= 10;
  }
  if (shift >= 0) {
    head = m / p;
    rest = m % p;
  } else {
    head = m * p;
  }

  uint64_t low = (uint64_t)(head % 100000000);

  nb.first = (int)(head / 100000000);
  nb.second = (int)(low / 10000);
  nb.ndigits = (0 != low || 0 != rest) ? 2 : 1;
  return nb;
}

/* The base-10,000 form of a double; that of zero if it is not above zero. */
nbase_t nbase_of_double(double v) {
  nbase_t nb = {0, 0, 0, 1};

  if (!(v > 0)) {
    return nb;
  }
  int e = (int)floor(log10(v));

  while (pow(10.0, e + 1) <= v) {
    e++;
  }
  while (pow(10.0, e) > v) {
    e--;
  }
  nb.weight = weight_of(e);

  double scaled = v / pow(10000.0, nb.weight);

  nb.first = (int)scaled;
  nb.second = (int)((scaled - nb.first) * 10000);
  nb.ndigits = (scaled - nb.first) != 0 ? 2 : 1;
  return nb;
}

/* scale, not below a nor b, and from 0 to 36. */
int clamp_scale(int scale, int a, int b) {
  scale = scale < a ? a : scale;
  scale = scale < b ? b : scale;
  scale = scale < 0 ? 0 : scale;
  return scale > max_scale ? max_scale : scale;
}

/* PostgreSQL's select_div_scale() of two base-10,000 forms. */
int div_scale(const nbase_t &na, int sa, const nbase_t &nb, int sb) {
  int qweight = na.weight - nb.weight;

  if (na.first <= nb.first) {
    qweight--;
  }
  return clamp_scale(min_sig_digits - qweight * 4, sa, sb);
}

/*
 * The decimal weight of ln(w), as PostgreSQL estimates it; w is above zero.
 * From 0.9 to 1.1 ln(w) is about w - 1; else it is by the first digits.
 */
int ln_dweight(const ty_dec128_t *w) {
  static const ty_dec128_t lo = {9, 1}, hi = {11, 1}, one = {1, 0};

  if (w->value <= 0) {
    return 0;
  }
  if (ty_dec128_cmp(*w, lo) >= 0 && ty_dec128_cmp(*w, hi) <= 0) {
    ty_dec128_t x = {0, 0};

    (void)ty_dec128_sub(w, &one, &x);
    return 0 == x.value ? 0 : digits_of(magnitude(x.value)) - 1 - x.scale;
  }

  nbase_t nb = nbase_of(w->value, w->scale);
  int digits = nb.first;
  int dweight = nb.weight * 4;

  if (nb.ndigits > 1) {
    digits = digits * 10000 + nb.second;
    dweight -= 4;
  }
  return (int)log10(fabs(log((double)digits) + dweight * 2.302585092994046));
}

/* The errors of a power, with PostgreSQL's messages. */
int zero_power_error(void) {
  return ty_err_invalid("zero raised to a negative power is undefined");
}

/*
 * base ^ exp for a whole exp, of scale exp_scale as it was written.
 * 1. The decimal weight of the result, estimated by the first digits.
 * 2. The result's scale, by its weight.
 * 3. Zero's powers; any other is ty_dec128_pow_int's.
 */
int power_int(const ty_dec128_t *base, int64_t exp, int exp_scale,
              ty_dec128_t *result) {
  nbase_t nb = nbase_of(base->value, base->scale);
  double f = 0;

  // 1. The decimal weight of the result.
  if (0 != base->value) {
    double d = nb.first;
    int p = nb.weight * 4;

    if (nb.ndigits > 1) {
      d = d * 10000 + nb.second;
      p -= 4;
    }
    f = (double)exp * (log10(d) + p);
  }
  if (f > max_scale) {
    return ty_err_range();
  }

  // 2. The result's scale.
  int rscale = clamp_scale(min_sig_digits - (int)(f > -1000.0 ? f : -1000.0),
                           base->scale, exp_scale);

  // 3. Zero's powers; any other is ty_dec128_pow_int's.
  if (0 == base->value) {
    if (exp < 0) {
      return zero_power_error();
    }
    if (0 != exp) {
      *result = {0, rscale};
      return 0;
    }
    ty_int128_t one = 1;

    if (0 != ty_dec128_scale_up(&one, 0, rscale)) {
      return -1;
    }
    *result = {one, rscale};
    return 0;
  }
  return ty_dec128_pow_int(base, exp, rscale, result);
}

} // namespace

/* The number of decimal digits of v; 0 has one. */
int ty_dec128_digits(ty_int128_t v) { return digits_of(magnitude(v)); }

/* *v of scale from, put to scale to. */
int ty_dec128_scale_up(ty_int128_t *v, int from, int to) {
  for (; from < to; from++) {
    if (__builtin_mul_overflow(*v, (ty_int128_t)10, v)) {
      return ty_err_range();
    }
  }
  if (*v > TY_DEC128_MAX_VALUE || *v < -TY_DEC128_MAX_VALUE) {
    return ty_err_range();
  }
  return 0;
}

/* The scale of a / b. */
int ty_dec128_div_scale(const ty_dec128_t *a, const ty_dec128_t *b) {
  return div_scale(nbase_of(a->value, a->scale), a->scale,
                   nbase_of(b->value, b->scale), b->scale);
}

/* The scale of a variance, num at scale over den. */
int ty_dec128_var_scale(double num, int scale, ty_int128_t den) {
  return div_scale(nbase_of_double(num), scale, nbase_of(den, 0), 0);
}

/* The scale of a square root: 16 digits, and not below the argument's. */
int ty_dec128_sqrt_scale(const ty_dec128_t *a) {
  int sweight = nbase_of(a->value, a->scale).weight * 4 / 2 + 1;

  return clamp_scale(min_sig_digits - sweight, a->scale, 0);
}

/* The scale of exp(a), by a's count of digits in base ten. */
int ty_dec128_exp_scale(const ty_dec128_t *a) {
  double val = ty_dec128_to_double(a) * log10_e;

  val = val < -max_result_scale ? -max_result_scale : val;
  val = val > max_result_scale ? max_result_scale : val;
  return clamp_scale(min_sig_digits - (int)val, a->scale, 0);
}

/* The scale of ln(a). */
int ty_dec128_ln_scale(const ty_dec128_t *a) {
  return clamp_scale(min_sig_digits - ln_dweight(a), a->scale, 0);
}

/* The scale of log(base, a). */
int ty_dec128_log_scale(const ty_dec128_t *base, const ty_dec128_t *a) {
  return clamp_scale(min_sig_digits - (ln_dweight(a) - ln_dweight(base)),
                     base->scale, a->scale);
}

/*
 * base ^ exp.
 * 1. The errors of the arguments.
 * 2. A whole exponent of at most 9 digits is power_int's.
 * 3. Else the weight of the result is by its logarithm, and the result is
 *    ty_dec128_pow_real's, which keeps the sign of a negative base: its
 *    exponent is whole.
 */
int ty_dec128_power(const ty_dec128_t *base, const ty_dec128_t *exp,
                    ty_dec128_t *result) {
  ty_dec128_t whole = {0, 0};
  bool is_whole;

  // 1. The errors of the arguments.
  (void)ty_dec128_trunc(exp, 0, &whole);
  is_whole = 0 == ty_dec128_cmp(whole, *exp);
  if (0 == base->value && exp->value < 0) {
    return zero_power_error();
  }
  if (base->value < 0 && !is_whole) {
    return ty_err_invalid("a negative number raised to a non-integer power "
                          "yields a complex result");
  }

  // 2. A whole exponent of at most 9 digits.
  if (is_whole && ty_dec128_digits(whole.value) <= 9) {
    return power_int(base, (int64_t)whole.value, exp->scale, result);
  }
  if (0 == base->value) {
    *result = {0, min_sig_digits};
    return 0;
  }

  // 3. Any other: the weight of the result is by its logarithm.
  ty_dec128_t abs_base = ty_dec128_abs(*base);
  double ln_num =
      log(ty_dec128_to_double(&abs_base)) * ty_dec128_to_double(exp);

  if (fabs(ln_num) > max_result_scale * 3.01) {
    return ln_num > 0 ? ty_err_range() : ty_err_underflow();
  }
  return ty_dec128_pow_real(
      base, exp,
      clamp_scale(min_sig_digits - (int)(ln_num * log10_e), base->scale,
                  exp->scale),
      result);
}

/* *a rounded to scale, if its digits before the point then fit. */
int ty_dec128_typmod(ty_dec128_t *a, int precision, int scale) {
  ty_dec128_t r = {0, 0};

  /* If it cannot pass the zeros that scale needs, it has too many digits. */
  if (0 != ty_dec128_round(a, scale, &r)) {
    return -1;
  }
  if (ty_dec128_digits(r.value) - r.scale > precision - scale) {
    return ty_err_range();
  }
  *a = r;
  return 0;
}

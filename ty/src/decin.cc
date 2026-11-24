/*
 * Copyright (c) 2026 CK Tan
 * SPDX-License-Identifier: MIT
 */

/*
 * decin.cc -- the text of a decimal: numeric's input and output, the
 * digits a binary form is read into, and the float casts that go by the
 * text (ty_decimal.h).
 */
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "error.h"
#include "ty_decimal.h"

namespace {

__extension__ typedef unsigned __int128 u128;

/*
 * The digits of u, at least one, into out; returns how many.
 * 1. Divide in 128 bits while u does not fit 64.
 * 2. Then in 64, which is cheaper: a decimal64's value always fits.
 * 3. Reverse them into out.
 */
int digits_of(u128 u, char *out) {
  char tmp[48];
  int n = 0;
  uint64_t w;

  // 1. Divide in 128 bits while u does not fit 64.
  while (u > UINT64_MAX) {
    tmp[n++] = (char)('0' + (int)(u % 10));
    u /= 10;
  }

  // 2. Then in 64.
  w = (uint64_t)u;
  do {
    tmp[n++] = (char)('0' + (int)(w % 10));
    w /= 10;
  } while (0 != w);

  // 3. Reverse them into out.
  for (int i = 0; i < n; i++) {
    out[i] = tmp[n - 1 - i];
  }
  return n;
}

/* |v|, as unsigned. */
u128 magnitude(ty_int128_t v) { return v < 0 ? -(u128)v : (u128)v; }

/* The digits of u as digits at scale 0. */
void digits_of_u128(ty_dec_digits_t *d, u128 u) {
  d->scale = 0;
  d->nd = u == 0 ? 0 : digits_of(u, d->dig);
}

/*
 * Set the error of a text that is not a decimal: *why, and ty's error,
 * TY_ERR_INVALID for its syntax, TY_ERR_RANGE for its size.
 */
int not_decimal(int *why, int kind) {
  *why = kind;
  if (TY_DEC_IN_SYNTAX == kind || TY_DEC_IN_NAN == kind) {
    return ty_err_invalid("invalid input syntax for type numeric");
  }
  return ty_err_range();
}

/*
 * Parse a decimal number at cp: digits, which may be separated by single
 * underscores, a point, more digits and an exponent.
 * 1. Take the digits, without leading zeros; count the places after the
 *    point.
 * 2. Take the exponent, at most 1000 either way.
 * 3. Move the point by the exponent: into the places, then zeros.
 * -1 with *why set if it is not a number, or is too long to read.
 */
int parse_decimal(const char *cp, ty_dec_digits_t *d, const char **endp,
                  int *why) {
  int nd = 0;
  int scale = 0;
  bool have_dp = false, have_digit = false;
  long exponent = 0;

  // 1. Take the digits, without leading zeros; count the places.
  for (;;) {
    if (isdigit((unsigned char)*cp)) {
      if (nd > 0 || '0' != *cp) {
        if (nd >= TY_DEC_DIGITS_MAX) {
          return not_decimal(why, TY_DEC_IN_RANGE);
        }
        d->dig[nd++] = *cp;
      }
      if (have_dp) {
        scale++;
      }
      have_digit = true;
      cp++;
    } else if ('.' == *cp && !have_dp) {
      have_dp = true;
      cp++;
      /* A point must follow a digit or be followed by one. */
      if (!have_digit && !isdigit((unsigned char)*cp)) {
        return not_decimal(why, TY_DEC_IN_SYNTAX);
      }
    } else if ('_' == *cp && have_digit && isdigit((unsigned char)cp[-1]) &&
               isdigit((unsigned char)cp[1])) {
      cp++;
    } else {
      break;
    }
  }
  if (!have_digit) {
    return not_decimal(why, TY_DEC_IN_SYNTAX);
  }

  // 2. Take the exponent, at most 1000 either way.
  if ('e' == *cp || 'E' == *cp) {
    const char *ep = cp + 1;
    bool eneg = false;

    if ('+' == *ep || '-' == *ep) {
      eneg = '-' == *ep++;
    }
    if (!isdigit((unsigned char)*ep)) {
      return not_decimal(why, TY_DEC_IN_SYNTAX);
    }
    while (isdigit((unsigned char)*ep)) {
      exponent = exponent * 10 + (*ep - '0');
      if (exponent > 1000) {
        return not_decimal(why, TY_DEC_IN_RANGE);
      }
      ep++;
      if ('_' == *ep && isdigit((unsigned char)ep[1])) {
        ep++;
      }
    }
    if (eneg) {
      exponent = -exponent;
    }
    cp = ep;
  }

  // 3. Move the point by the exponent: into the places, then zeros.
  scale -= (int)exponent;
  if (scale < 0 && nd > 0) {
    if (nd - scale > TY_DEC_DIGITS_MAX) {
      return not_decimal(why, TY_DEC_IN_RANGE);
    }
    memset(d->dig + nd, '0', -scale);
    nd -= scale;
  }
  d->nd = nd;
  d->scale = scale < 0 ? 0 : scale;
  *endp = cp;
  return 0;
}

/*
 * Parse an integer in base 16, 8 or 2 at cp, after its prefix, with single
 * underscores between digits. It must fit 36 digits, as any value does.
 */
int parse_nondecimal(const char *cp, int base, ty_dec_digits_t *d,
                     const char **endp, int *why) {
  u128 v = 0;
  bool have_digit = false;

  for (;;) {
    int digit;

    if (isxdigit((unsigned char)*cp)) {
      digit = isdigit((unsigned char)*cp)
                  ? *cp - '0'
                  : (tolower((unsigned char)*cp) - 'a' + 10);
    } else if ('_' == *cp && have_digit && isxdigit((unsigned char)cp[1])) {
      cp++;
      continue;
    } else {
      break;
    }
    if (digit >= base) {
      break;
    }
    if (__builtin_mul_overflow(v, (u128)base, &v) ||
        __builtin_add_overflow(v, (u128)digit, &v)) {
      return not_decimal(why, TY_DEC_IN_DIGITS);
    }
    have_digit = true;
    cp++;
  }
  if (!have_digit) {
    return not_decimal(why, TY_DEC_IN_SYNTAX);
  }
  digits_of_u128(d, v);
  *endp = cp;
  return 0;
}

/* d with zeros at the end, past the point, dropped one by one. */
void strip(ty_dec_digits_t *d, int floor) {
  while (d->scale > floor && (0 == d->nd || '0' == d->dig[d->nd - 1])) {
    if (d->nd > 0) {
      d->nd--;
    }
    d->scale--;
  }
}

} // namespace

/*
 * d with its last drop places taken off: rounded half away from zero by
 * the first of them, or cut. A carry out of the top adds a digit.
 */
void ty_dec_digits_drop(ty_dec_digits_t *d, int drop, int cut) {
  bool up = false;

  if (drop <= 0) {
    return;
  }
  d->scale -= drop;
  if (0 == d->nd) {
    return;
  }
  if (drop > d->nd) {
    d->nd = 0;
    return;
  }
  up = !cut && d->dig[d->nd - drop] >= '5';
  d->nd -= drop;
  if (up) {
    int i = d->nd - 1;

    while (i >= 0 && '9' == d->dig[i]) {
      d->dig[i--] = '0';
    }
    if (i >= 0) {
      d->dig[i]++;
    } else {
      memmove(d->dig + 1, d->dig, d->nd);
      d->dig[0] = '1';
      d->nd++;
    }
  }
  if (0 == d->nd && up) {
    d->dig[0] = '1';
    d->nd = 1;
  }
}

/* d with zeros added after the point, up to scale; -1 if too long. */
int ty_dec_digits_pad(ty_dec_digits_t *d, int scale) {
  int add = scale - d->scale;

  if (add <= 0) {
    return 0;
  }
  if (d->nd > 0) {
    if (d->nd + add > TY_DEC_DIGITS_MAX) {
      return -1;
    }
    memset(d->dig + d->nd, '0', add);
    d->nd += add;
  }
  d->scale = scale;
  return 0;
}

/*
 * d rounded to a typmod's scale and checked against its precision, as an
 * assignment to numeric(precision, scale) does.
 * 1. Round the places past scale away, or pad to scale.
 * 2. Refuse more than precision - scale digits before the point.
 */
int ty_dec_digits_typmod(ty_dec_digits_t *d, int precision, int scale) {
  int padded;

  // 1. Round the places past scale away, or pad to scale.
  if (d->scale > scale) {
    ty_dec_digits_drop(d, d->scale - scale, 0);
    padded = 0;
  } else {
    padded = ty_dec_digits_pad(d, scale);
  }

  // 2. Refuse more than precision - scale digits before the point.
  if (0 != padded || d->nd - d->scale > precision - scale) {
    return ty_err_range();
  }
  return 0;
}

/*
 * d as a value.
 * 1. Round away places past 36.
 * 2. Push digits past 36 out of the places, half away from zero; a carry
 *    may add one, which goes too.
 * 3. -1 if the integer part has more than 36 digits.
 */
int ty_dec_digits_pack(ty_dec_digits_t *d, int neg, ty_dec128_t *out) {
  u128 u = 0;

  // 1. Round away places past 36.
  if (d->scale > TY_DEC128_MAX_SCALE) {
    ty_dec_digits_drop(d, d->scale - TY_DEC128_MAX_SCALE, 0);
  }

  // 2. Push digits past 36 out of the places.
  if (d->nd > TY_DEC128_MAX_DIGITS) {
    int drop = d->nd - TY_DEC128_MAX_DIGITS;

    // 3. -1 if the integer part has more than 36 digits.
    if (drop > d->scale) {
      return ty_err_range();
    }
    ty_dec_digits_drop(d, drop, 0);
    /* Rounding up may add a digit: 99.95 to 100.0. */
    if (d->nd > TY_DEC128_MAX_DIGITS) {
      if (0 == d->scale) {
        return ty_err_range();
      }
      ty_dec_digits_drop(d, 1, 0);
    }
  }
  for (int i = 0; i < d->nd; i++) {
    u = u * 10 + (u128)(d->dig[i] - '0');
  }
  out->value = neg ? -(ty_int128_t)u : (ty_int128_t)u;
  out->scale = d->scale;
  return 0;
}

/*
 * The text s as a decimal128, under a typmod or none.
 * 1. Skip spaces, take a sign; NaN and the infinities are not values.
 * 2. Read a number in base 16, 8 or 2 after its prefix, or a decimal one.
 * 3. Nothing but spaces may follow.
 * 4. Round to the typmod, and check it.
 * 5. Drop zeros at the end of the places while there are too many digits
 *    or places; then pack, refusing more than 36 digits.
 */
int ty_dec128_parse(const char *s, int precision, int scale, ty_dec128_t *out,
                    int *why) {
  const char *cp = s;
  bool neg = false;
  ty_dec_digits_t d;

  // 1. Skip spaces, take a sign; NaN and the infinities are not values.
  while (*cp && isspace((unsigned char)*cp)) {
    cp++;
  }
  if ('+' == *cp) {
    cp++;
  } else if ('-' == *cp) {
    neg = true;
    cp++;
  }
  if (!isdigit((unsigned char)*cp) && '.' != *cp) {
    if (0 == strncasecmp(cp, "NaN", 3) || 0 == strncasecmp(cp, "inf", 3)) {
      return not_decimal(why, TY_DEC_IN_NAN);
    }
    return not_decimal(why, TY_DEC_IN_SYNTAX);
  }

  // 2. Read a number in base 16, 8 or 2 after its prefix, or a decimal one.
  if ('0' == cp[0] && '\0' != cp[1] && nullptr != strchr("xXoObB", cp[1])) {
    int base = ('x' == cp[1] || 'X' == cp[1])   ? 16
               : ('o' == cp[1] || 'O' == cp[1]) ? 8
                                                : 2;

    if (0 != parse_nondecimal(cp + 2, base, &d, &cp, why)) {
      return -1;
    }
  } else if (0 != parse_decimal(cp, &d, &cp, why)) {
    return -1;
  }

  // 3. Nothing but spaces may follow.
  for (; *cp; cp++) {
    if (!isspace((unsigned char)*cp)) {
      return not_decimal(why, TY_DEC_IN_SYNTAX);
    }
  }
  neg = neg && 0 != d.nd;

  // 4. Round to the typmod, and check it.
  if (precision >= 0 && 0 != ty_dec_digits_typmod(&d, precision, scale)) {
    *why = TY_DEC_IN_FIELD;
    return -1;
  }

  // 5. Drop zeros at the end of the places while there are too many
  //    digits or places; then pack.
  while ((d.nd > TY_DEC128_MAX_DIGITS || d.scale > TY_DEC128_MAX_SCALE) &&
         d.scale > 0) {
    if (d.nd > 0 && '0' != d.dig[d.nd - 1]) {
      break; /* the last place is not a zero */
    }
    strip(&d, d.scale - 1);
  }
  if (d.nd > TY_DEC128_MAX_DIGITS || d.scale > TY_DEC128_MAX_SCALE ||
      0 != ty_dec_digits_pack(&d, neg, out)) {
    return not_decimal(why, TY_DEC_IN_DIGITS);
  }
  return 0;
}

/*
 * a's text, as PostgreSQL prints a numeric: every place of its scale, and
 * no exponent.
 * 1. Write the sign.
 * 2. With no more digits than places, "0." and the zeros the places need
 *    before the digits.
 * 3. Otherwise the integer digits, and the places after a point.
 */
int ty_dec128_format(const ty_dec128_t *a, char *out) {
  char dig[48];
  int n = digits_of(magnitude(a->value), dig), pos = 0;
  int scale = a->scale;

  // 1. Write the sign.
  if (a->value < 0) {
    out[pos++] = '-';
  }

  // 2. With no more digits than places, "0." and its zeros.
  if (n <= scale) {
    out[pos++] = '0';
    if (scale > 0) {
      out[pos++] = '.';
    }
    for (int i = n; i < scale; i++) {
      out[pos++] = '0';
    }
    for (int i = 0; i < n; i++) {
      out[pos++] = dig[i];
    }
  } else {
    // 3. The integer digits, and the places after a point.
    int ip = n - scale;

    for (int i = 0; i < ip; i++) {
      out[pos++] = dig[i];
    }
    if (scale > 0) {
      out[pos++] = '.';
      for (int i = ip; i < n; i++) {
        out[pos++] = dig[i];
      }
    }
  }
  out[pos] = '\0';
  return pos;
}

/* a as the nearest float64: its text read by strtod. */
double ty_dec128_to_float64(const ty_dec128_t *a) {
  char buf[TY_DEC_TEXT_SIZE];

  ty_dec128_format(a, buf);
  return strtod(buf, nullptr);
}

/* a as the nearest float32: its text read by strtof. */
float ty_dec128_to_float32(const ty_dec128_t *a) {
  char buf[TY_DEC_TEXT_SIZE];

  ty_dec128_format(a, buf);
  return strtof(buf, nullptr);
}

/*
 * x as a decimal128, by its first digits significant digits, as PostgreSQL
 * takes a float: its "%.*g" text, read as a numeric.
 */
int ty_dec128_from_float(double x, int digits, ty_dec128_t *out, int *why) {
  char buf[64];

  if (isnan(x) || isinf(x)) {
    *why = TY_DEC_IN_NAN;
    return ty_err_invalid(isnan(x) ? "cannot convert NaN to numeric"
                                   : "cannot convert infinity to numeric");
  }
  snprintf(buf, sizeof buf, "%.*g", digits, x);
  return ty_dec128_parse(buf, -1, 0, out, why);
}

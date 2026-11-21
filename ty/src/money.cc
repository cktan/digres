/*
 * Copyright (c) 2026 CK Tan
 * SPDX-License-Identifier: MIT
 */

/*
 * money.cc -- the operations of money.
 */
#include "ty_money.h"

#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

#include "error.h"
#include "ty_float.h"

const ty_money_fmt_t ty_money_fmt_c = {
    CHAR_MAX, "",       "",       "",       "",       "",      0,
    CHAR_MAX, CHAR_MAX, CHAR_MAX, CHAR_MAX, CHAR_MAX, CHAR_MAX};

namespace {

/* The error for a value that is too large. */
int range() {
  return ty_internal::set_error(TY_ERR_RANGE, "money out of range");
}

/* The decimal point, the thousands separator and the symbols, as PostgreSQL
 * chooses them when the locale gives none. */
struct symbols_t {
  int fpoint;
  char dsymbol;
  const char *ssymbol, *csymbol, *psymbol, *nsymbol;
  size_t slen, clen, plen, nlen; /* the lengths of the four symbols */
};

symbols_t symbols_of(const ty_money_fmt_t *f) {
  symbols_t s;

  s.fpoint = ty_money_frac_digits(f);
  s.dsymbol = (*f->mon_decimal_point != '\0' && f->mon_decimal_point[1] == '\0')
                  ? *f->mon_decimal_point
                  : '.';
  /* the separator must not equal the decimal point */
  s.ssymbol = *f->mon_thousands_sep != '\0' ? f->mon_thousands_sep
              : (s.dsymbol != ',')          ? ","
                                            : ".";
  s.csymbol = *f->currency_symbol != '\0' ? f->currency_symbol : "$";
  s.psymbol = *f->positive_sign != '\0' ? f->positive_sign : "+";
  s.nsymbol = *f->negative_sign != '\0' ? f->negative_sign : "-";
  s.slen = strlen(s.ssymbol);
  s.clen = strlen(s.csymbol);
  s.plen = strlen(s.psymbol);
  s.nlen = strlen(s.nsymbol);
  return s;
}

/* Whether s begins with the symbol, of len bytes (a symbol is not empty). */
inline bool starts(const char *s, const char *sym, size_t len) {
  return *s == *sym && strncmp(s, sym, len) == 0;
}

/* The C locale's isspace and isdigit, for the ASCII a UTF-8 text has. */
inline bool is_space(char c) { return c == ' ' || (c >= '\t' && c <= '\r'); }
inline bool is_digit(char c) { return c >= '0' && c <= '9'; }

} // namespace

/* 10 to the power of the digits after the point. */
int64_t ty_money_scale(const ty_money_fmt_t *f) {
  int64_t scale = 1;

  for (int i = 0; i < ty_money_frac_digits(f); i++) {
    scale *= 10;
  }
  return scale;
}

/*
 * Read money.
 * 1. Skip spaces; a currency symbol; spaces; a sign (a minus, a "(" or a
 *    plus); spaces; a currency symbol; spaces.
 * 2. Digits build the value as a negative number (to reach INT64_MIN); a
 *    decimal point starts the fraction; a thousands separator is skipped;
 *    anything else ends the number.
 * 3. A next digit of 5 or more rounds; the fraction is padded to the
 *    locale's digits.
 * 4. What is left must be spaces, ")" and symbols; a sign there also counts.
 */
int ty_money_parse(const char *str, const ty_money_fmt_t *f, int64_t *out) {
  int64_t value = 0, dec = 0, sgn = 1;
  bool seen_dot = false;
  const char *s = str;
  symbols_t y = symbols_of(f);

  while (is_space(*s)) {
    s++;
  }
  if (starts(s, y.csymbol, y.clen)) {
    s += y.clen;
  }
  while (is_space(*s)) {
    s++;
  }
  if (starts(s, y.nsymbol, y.nlen)) {
    sgn = -1;
    s += y.nlen;
  } else if (*s == '(') {
    sgn = -1;
    s++;
  } else if (starts(s, y.psymbol, y.plen)) {
    s += y.plen;
  }
  while (is_space(*s)) {
    s++;
  }
  if (starts(s, y.csymbol, y.clen)) {
    s += y.clen;
  }
  while (is_space(*s)) {
    s++;
  }
  for (; *s; s++) {
    if (is_digit(*s) && (!seen_dot || dec < y.fpoint)) {
      int8_t digit = (int8_t)(*s - '0');

      if (__builtin_mul_overflow(value, (int64_t)10, &value) ||
          __builtin_sub_overflow(value, (int64_t)digit, &value)) {
        return range();
      }
      if (seen_dot) {
        dec++;
      }
    } else if (*s == y.dsymbol && !seen_dot) {
      seen_dot = true;
    } else if (starts(s, y.ssymbol, y.slen)) {
      s += y.slen - 1;
    } else {
      break;
    }
  }
  /* round on the first digit that is dropped */
  if (is_digit(*s) && *s >= '5') {
    if (__builtin_sub_overflow(value, (int64_t)1, &value)) {
      return range();
    }
  }
  for (; dec < y.fpoint; dec++) {
    if (__builtin_mul_overflow(value, (int64_t)10, &value)) {
      return range();
    }
  }
  while (is_digit(*s)) {
    s++;
  }
  while (*s) {
    if (is_space(*s) || *s == ')') {
      s++;
    } else if (starts(s, y.nsymbol, y.nlen)) {
      sgn = -1;
      s += y.nlen;
    } else if (starts(s, y.psymbol, y.plen)) {
      s += y.plen;
    } else if (starts(s, y.csymbol, y.clen)) {
      s += y.clen;
    } else {
      return ty_internal::set_error(TY_ERR_INVALID, "invalid money text");
    }
  }
  if (sgn > 0) {
    if (value == INT64_MIN) {
      return range();
    }
    *out = -value;
  } else {
    *out = value;
  }
  return 0;
}

/*
 * Write money.
 * 1. The digits, the decimal point and the thousands separators are built
 *    from the right.
 * 2. The currency symbol and the sign go in the places the locale's
 *    sign_posn, cs_precedes and sep_by_space say (POSIX's rules).
 */
size_t ty_money_format(int64_t value, const ty_money_fmt_t *f, char *out,
                       size_t cap) {
  char buf[128];
  char *bufptr = buf + sizeof(buf) - 1;
  symbols_t y = symbols_of(f);
  int points = y.fpoint;
  int mon_group = f->mon_grouping;
  const char *signsymbol;
  int sign_posn, cs_precedes, sep_by_space;
  uint64_t uvalue;
  int digit_pos;

  if (mon_group <= 0 || mon_group > 6) {
    mon_group = 3;
  }
  if (value < 0) {
    signsymbol = y.nsymbol;
    sign_posn = f->n_sign_posn;
    cs_precedes = f->n_cs_precedes;
    sep_by_space = f->n_sep_by_space;
  } else {
    signsymbol = f->positive_sign;
    sign_posn = f->p_sign_posn;
    cs_precedes = f->p_cs_precedes;
    sep_by_space = f->p_sep_by_space;
  }
  uvalue = value < 0 ? (uint64_t)0 - (uint64_t)value : (uint64_t)value;
  *bufptr = '\0';
  digit_pos = points;
  do {
    if (points && digit_pos == 0) {
      *(--bufptr) = y.dsymbol;
    } else if (digit_pos < 0 && (digit_pos % mon_group) == 0) {
      bufptr -= y.slen;
      memcpy(bufptr, y.ssymbol, y.slen);
    }
    *(--bufptr) = (char)((uvalue % 10) + '0');
    uvalue = uvalue / 10;
    digit_pos--;
  } while (uvalue || digit_pos >= 0);

  /* the pieces in order; the text is written if it fits, as snprintf does */
  struct piece_t {
    const char *p;
    size_t n;
  } pieces[5];
  int np = 0;
  const char *c = y.csymbol, *g = signsymbol, *b = bufptr;
  size_t clen = y.clen, glen = strlen(g), blen = strlen(b);
  const char *sp1 = sep_by_space == 1 ? " " : "",
             *sp2 = sep_by_space == 2 ? " " : "";
  size_t l1 = sep_by_space == 1, l2 = sep_by_space == 2;
  auto add = [&](const char *p, size_t n) { pieces[np++] = {p, n}; };

  switch (sign_posn) {
  case 0:
    add("(", 1);
    if (cs_precedes) {
      add(c, clen), add(sp1, l1), add(b, blen);
    } else {
      add(b, blen), add(sp1, l1), add(c, clen);
    }
    add(")", 1);
    break;
  case 2:
    if (cs_precedes) {
      add(c, clen), add(sp1, l1), add(b, blen), add(sp2, l2), add(g, glen);
    } else {
      add(b, blen), add(sp1, l1), add(c, clen), add(sp2, l2), add(g, glen);
    }
    break;
  case 3:
    if (cs_precedes) {
      add(g, glen), add(sp2, l2), add(c, clen), add(sp1, l1), add(b, blen);
    } else {
      add(b, blen), add(sp1, l1), add(g, glen), add(sp2, l2), add(c, clen);
    }
    break;
  case 4:
    if (cs_precedes) {
      add(c, clen), add(sp2, l2), add(g, glen), add(sp1, l1), add(b, blen);
    } else {
      add(b, blen), add(sp1, l1), add(c, clen), add(sp2, l2), add(g, glen);
    }
    break;
  case 1:
  default:
    if (cs_precedes) {
      add(g, glen), add(sp2, l2), add(c, clen), add(sp1, l1), add(b, blen);
    } else {
      add(g, glen), add(sp2, l2), add(b, blen), add(sp1, l1), add(c, clen);
    }
    break;
  }
  size_t total = 0;

  for (int i = 0; i < np; i++) {
    total += pieces[i].n;
  }
  if (total < cap) {
    char *d = out;

    for (int i = 0; i < np; i++) {
      memcpy(d, pieces[i].p, pieces[i].n);
      d += pieces[i].n;
    }
    *d = '\0';
  }
  return total;
}

/* money over an integer; -1 is negation, which can overflow. */
int ty_money_div_int64(int64_t c, int64_t i, int64_t *r) {
  if (i == 0) {
    return ty_err_divzero();
  }
  if (i == -1) {
    if (c == INT64_MIN) {
      return range();
    }
    *r = -c;
    return 0;
  }
  *r = c / i;
  return 0;
}

namespace {

/* The float result as money: it must be a number that fits an int64. */
int float_to_money(double res, int64_t *r) {
  /* the range of an int64 is [-2^63, 2^63) */
  if (std::isnan(res) ||
      !(res >= -9223372036854775808.0 && res < 9223372036854775808.0)) {
    return range();
  }
  *r = (int64_t)res;
  return 0;
}

} // namespace

/* money times a float. */
int ty_money_mul_float64(int64_t c, double f, int64_t *r) {
  double res;

  if (ty_float64_mul((double)c, f, &res) != 0) {
    return -1;
  }
  return float_to_money(rint(res), r);
}

/* money over a float. */
int ty_money_div_float64(int64_t c, double f, int64_t *r) {
  double res;

  if (ty_float64_div((double)c, f, &res) != 0) {
    return -1;
  }
  return float_to_money(rint(res), r);
}

/* money over money. */
int ty_money_div_money(int64_t a, int64_t b, double *r) {
  if (b == 0) {
    return ty_err_divzero();
  }
  *r = (double)a / (double)b;
  return 0;
}

namespace {

/* Append the words of a number below 1000 (the number is positive). */
char *num_word(char *p, int64_t value) {
  static const char *const small[] = {
      "zero",     "one",      "two",      "three",   "four",    "five",
      "six",      "seven",    "eight",    "nine",    "ten",     "eleven",
      "twelve",   "thirteen", "fourteen", "fifteen", "sixteen", "seventeen",
      "eighteen", "nineteen", "twenty",   "thirty",  "forty",   "fifty",
      "sixty",    "seventy",  "eighty",   "ninety"};
  const char *const *big = small + 18;
  int tu = (int)(value % 100);

  if (value <= 20) {
    return p + sprintf(p, "%s", small[value]);
  }
  if (!tu) {
    return p + sprintf(p, "%s hundred", small[value / 100]);
  }
  if (value > 99) {
    if (value % 10 == 0 && tu > 10) {
      return p + sprintf(p, "%s hundred %s", small[value / 100], big[tu / 10]);
    }
    if (tu < 20) {
      return p + sprintf(p, "%s hundred and %s", small[value / 100], small[tu]);
    }
    return p + sprintf(p, "%s hundred %s %s", small[value / 100], big[tu / 10],
                       small[tu % 10]);
  }
  if (value % 10 == 0 && tu > 10) {
    return p + sprintf(p, "%s", big[tu / 10]);
  }
  if (tu < 20) {
    return p + sprintf(p, "%s", small[tu]);
  }
  return p + sprintf(p, "%s %s", big[tu / 10], small[tu % 10]);
}

} // namespace

/*
 * The words of a value: groups of three digits of the dollars, each with its
 * name (thousand, million, ...), then "dollars and" the cents.
 */
size_t ty_money_words(int64_t value, char *out) {
  char *p = out;
  uint64_t val;

  if (value < 0) {
    p += sprintf(p, "minus ");
    /* negate as unsigned: INT64_MIN has no positive int64 */
    val = (uint64_t)0 - (uint64_t)value;
  } else {
    val = (uint64_t)value;
  }
  int64_t dollars = (int64_t)(val / 100);
  int64_t m0 = (int64_t)(val % 100);
  int64_t m1 = (int64_t)((val / 100) % 1000);
  int64_t m2 = (int64_t)((val / 100000) % 1000);
  int64_t m3 = (int64_t)((val / 100000000) % 1000);
  int64_t m4 = (int64_t)((val / 100000000000ULL) % 1000);
  int64_t m5 = (int64_t)((val / 100000000000000ULL) % 1000);
  int64_t m6 = (int64_t)((val / 100000000000000000ULL) % 1000);

  if (m6) {
    p = num_word(p, m6);
    p += sprintf(p, " quadrillion ");
  }
  if (m5) {
    p = num_word(p, m5);
    p += sprintf(p, " trillion ");
  }
  if (m4) {
    p = num_word(p, m4);
    p += sprintf(p, " billion ");
  }
  if (m3) {
    p = num_word(p, m3);
    p += sprintf(p, " million ");
  }
  if (m2) {
    p = num_word(p, m2);
    p += sprintf(p, " thousand ");
  }
  if (m1) {
    p = num_word(p, m1);
  }
  if (dollars == 0) {
    p += sprintf(p, "zero");
  }
  p += sprintf(p, dollars == 1 ? " dollar and " : " dollars and ");
  p = num_word(p, m0);
  p += sprintf(p, m0 == 1 ? " cent" : " cents");
  out[0] = (char)toupper((unsigned char)out[0]);
  return (size_t)(p - out);
}

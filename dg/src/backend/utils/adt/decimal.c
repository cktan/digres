/*
 * Copyright (c) 2026 CK Tan
 * SPDX-License-Identifier: MIT
 */

/*-------------------------------------------------------------------------
 *
 * decimal.c
 *	  numeric, as a decimal of at most 36 digits (DECIMAL.md).
 *
 * A value is sm's decimal128, a 128-bit word holding the number times
 * 10^scale shifted past its scale (utils/numeric.h): there is no NaN and
 * no infinity, and a result past 36 digits loses places after the point,
 * or is an error.  What PostgreSQL's numeric did with arbitrary precision
 * this does exactly, in a 512-bit integer where 128 bits do not reach, and
 * rounds to 36 digits; ln, exp, log and a power to a fraction are computed
 * in double.  The scale of each result is the one PostgreSQL's numeric
 * would give it, as far as 36 digits allow.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <cfloat>
#include <cmath>
#include <cstring>
#include <string>

#include "catalog/pg_type.h"
#include "common/hashfn.h"
#include "common/int.h"
#include "common/int128.h"
#include "funcapi.h"
#include "lib/stringinfo.h"
#include "libpq/pqformat.h"
#include "miscadmin.h"
#include "nodes/nodeFuncs.h"
#include "nodes/supportnodes.h"
#include "optimizer/optimizer.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/float.h"
#include "utils/fmgrprotos.h"
#include "utils/numeric.h"
#include "utils/pg_lsn.h"
#include "utils/sortsupport.h"
#include "ty_decimal.h"

typedef __int128 i128;
typedef unsigned __int128 u128;

/* ----------------------------------------------------------------------
 * The value: a 128-bit integer and its scale (utils/numeric.h).
 * ----------------------------------------------------------------------
 */

#define DEC_MAX_DIGITS NUMERIC_MAX_PRECISION

/* The value of num, the number times 10^scale. */
static i128 dec_value(const decimal_t *num) { return decimal_value(num); }

/* The scale of num. */
static int dec_scale(const decimal_t *num) { return decimal_scale(num); }

/* A new value of v at scale, palloc'd. */
static Numeric dec_make(i128 v, int scale) {
  Numeric res = (Numeric)palloc(sizeof(decimal_t));

  decimal_set(res, v, scale);
  return res;
}

/*
 * The error of a decimal128 result past 36 digits: raised, or, if the
 * caller asks for it, recorded in *have_error.
 */
static Numeric dec_range_error(bool *have_error) {
  if (have_error == NULL)
    ereport(ERROR, (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
                    errmsg("value overflows numeric format")));
  *have_error = true;
  return NULL;
}

static bool division_by_zero(bool *have_error);

/* The ERROR for the error that ty's last function of the exponent set. */
static Numeric dec_math_error(void) {
  if (ty_errnum() == TY_ERR_UNDERFLOW)
    ereport(ERROR, (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
                    errmsg("value underflows numeric format")));
  if (ty_errnum() == TY_ERR_DIVZERO)
    division_by_zero(NULL);
  return dec_range_error(NULL);
}

/* num's value and scale, as ty takes them. */
static ty_dec128_t dec_ty(const decimal_t *num) {
  return {dec_value(num), dec_scale(num)};
}

/* 10^n as a value, for n up to 38. */
static i128 dec_pow10(int n) {
  i128 r = 1;

  while (n-- > 0)
    r *= 10;
  return r;
}

/* The number of decimal digits of u; 0 has one. */
static int u128_digits(u128 u) {
  int n = 1;

  for (; u >= 10; u /= 10)
    n++;
  return n;
}

/* The digits of u, at least one, into out; returns how many. */
static int dec_digits_text(u128 u, char *out) {
  char tmp[48];
  int n = 0;

  do {
    tmp[n++] = (char)('0' + (int)(u % 10));
    u /= 10;
  } while (u != 0);
  for (int i = 0; i < n; i++)
    out[i] = tmp[n - 1 - i];
  return n;
}

/* v at scale as PostgreSQL prints a numeric: every place, no exponent. */
static char *dec_text(i128 v, int scale) {
  ty_dec128_t a = {v, scale};
  char out[TY_DEC_TEXT_SIZE];
  int n = ty_dec128_format(&a, out);
  char *r = (char *)palloc(n + 1);

  memcpy(r, out, n + 1);
  return r;
}

/*
 * *v, at scale from, put to scale to (not below it): false if it then has
 * more than 36 digits.
 */
static bool dec_scale_up(i128 *v, int from, int to) {
  for (; from < to; from++)
    if (__builtin_mul_overflow(*v, (i128)10, v))
      return false;
  return (*v >= 0 ? *v : -*v) < dec_pow10(DEC_MAX_DIGITS);
}

/*
 * PostgreSQL's view of w in base 10,000, as its numeric held it: the
 * weight of its first nonzero base-10,000 digit, that digit, the next, and
 * whether there are more after the first.  Its scale rules are written in
 * these.  Zero has weight 0 and digit 0.
 */
typedef struct nbase_t {
  int weight;
  int first;
  int second;
  int ndigits; /* 1 if nothing follows the first digit, else 2 */
} nbase_t;

/* ----------------------------------------------------------------------
 * Text
 * ----------------------------------------------------------------------
 */

/* ----------------------------------------------------------------------
 * Typmods: PostgreSQL's ((precision << 16) | scale) + VARHDRSZ, with
 * 1 <= precision <= 36 and 0 <= scale <= precision.
 * ----------------------------------------------------------------------
 */

/* Whether typmod names a precision and scale. */
static bool is_valid_numeric_typmod(int32 typmod) {
  return typmod >= (int32)VARHDRSZ;
}

/* typmod's precision. */
static int numeric_typmod_precision(int32 typmod) {
  return ((typmod - VARHDRSZ) >> 16) & 0xffff;
}

/* typmod's scale. */
static int numeric_typmod_scale(int32 typmod) {
  return (((typmod - VARHDRSZ) & 0x7ff) ^ 1024) - 1024;
}

/*
 * *v rounded to typmod's scale and checked against its precision, as an
 * assignment to numeric(p, s) does: ty rounds it; false with the error saved
 * in escontext if it has too many digits before the point.
 */
static bool dec_apply_typmod(ty_dec128_t *v, int32 typmod,
                             Node *escontext) {
  int precision, scale, maxdigits;
  ty_dec128_t r;

  if (!is_valid_numeric_typmod(typmod))
    return true;
  precision = numeric_typmod_precision(typmod);
  scale = numeric_typmod_scale(typmod);
  maxdigits = precision - scale;
  /* If it cannot pass the zeros that scale needs, it has too many digits. */
  if (ty_dec128_round(v, scale, &r) == 0) {
    *v = r;
    /* Digits before the point, of the rounded value. */
    if (u128_digits(r.value < 0 ? -(u128)r.value : (u128)r.value) -
            r.scale <=
        maxdigits)
      return true;
  }
  ereturn(escontext, false,
          (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
           errmsg("numeric field overflow"),
           errdetail("A field with precision %d, scale %d must round to an "
                     "absolute value less than %s%d.",
                     precision, scale,
                     /* Display 10^0 as 1 */
                     maxdigits ? "10^" : "", maxdigits ? maxdigits : 1)));
}

/* ----------------------------------------------------------------------
 * Input and output
 * ----------------------------------------------------------------------
 */

/*
 * The error of a text that is not a numeric, saved in escontext, by why,
 * ty_dec128_parse's reason: PostgreSQL's code, message and detail.
 */
static void dec_in_error(int why, const char *str, int32 typmod,
                         Node *escontext) {
  int precision, scale, maxdigits;

  switch (why) {
  case TY_DEC_IN_NAN:
    errsave(escontext,
            (errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
             errmsg("invalid input syntax for type %s: \"%s\"", "numeric", str),
             errdetail("numeric has no NaN and no infinity.")));
    return;
  case TY_DEC_IN_RANGE:
    errsave(escontext, (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
                        errmsg("value overflows numeric format")));
    return;
  case TY_DEC_IN_DIGITS:
    errsave(escontext,
            (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
             errmsg("value overflows numeric format"),
             errdetail("A numeric value has at most %d digits.", DEC_MAX_DIGITS)));
    return;
  case TY_DEC_IN_FIELD:
    precision = numeric_typmod_precision(typmod);
    scale = numeric_typmod_scale(typmod);
    maxdigits = precision - scale;
    errsave(escontext,
            (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
             errmsg("numeric field overflow"),
             errdetail("A field with precision %d, scale %d must round to an "
                       "absolute value less than %s%d.",
                       precision, scale,
                       /* Display 10^0 as 1 */
                       maxdigits ? "10^" : "", maxdigits ? maxdigits : 1)));
    return;
  default:
    errsave(escontext,
            (errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
             errmsg("invalid input syntax for type %s: \"%s\"", "numeric", str)));
  }
}

/*
 * numeric_in() -
 *
 *	Input function for numeric: a decimal number, or an integer in base 16,
 *	8 or 2 after 0x, 0o or 0b.  NaN and infinity are not values.  A value of
 *	more than 36 significant digits is refused, not rounded, unless a typmod
 *	rounds it.
 */
Datum numeric_in(PG_FUNCTION_ARGS) {
  char *str = PG_GETARG_CSTRING(0);
  int32 typmod = PG_GETARG_INT32(2);
  Node *escontext = fcinfo->context;
  ty_dec128_t v;
  int why;

  /* The text is ty's to read; a typmod is a precision and a scale. */
  if (0 != ty_dec128_parse(str,
                           is_valid_numeric_typmod(typmod)
                               ? numeric_typmod_precision(typmod)
                               : -1,
                           is_valid_numeric_typmod(typmod)
                               ? numeric_typmod_scale(typmod)
                               : 0,
                           &v, &why)) {
    dec_in_error(why, str, typmod, escontext);
    PG_RETURN_NULL();
  }
  PG_RETURN_NUMERIC(dec_make(v.value, v.scale));
}

/* Output function for numeric: every place of its scale, no exponent. */
Datum numeric_out(PG_FUNCTION_ARGS) {
  Numeric n = PG_GETARG_NUMERIC(0);

  PG_RETURN_CSTRING(dec_text(dec_value(n), dec_scale(n)));
}

/*
 * num in scientific notation, its significand rounded to scale places:
 * 1234 at scale 1 is 1.2e+03.  Zero's exponent is 0.
 */
char *numeric_out_sci(Numeric num, int scale) {
  i128 v = dec_value(num);
  int s = dec_scale(num);
  char dig[48];
  int exponent =
      v == 0 ? 0 : dec_digits_text(v < 0 ? -(u128)v : (u128)v, dig) - 1 - s;
  ty_dec128_t sig = {v, s + exponent}, r;
  char *txt;
  char *str;
  size_t len;

  if (scale < 0)
    scale = 0;
  /* The significand is v / 10^exponent: the point moves, the digits do not. */
  if (scale < sig.scale) {
    ty_dec128_round(&sig, scale, &r);
    txt = dec_text(r.value, r.scale);
  } else {
    char *t = dec_text(sig.value, sig.scale);
    size_t tl = strlen(t);

    txt = (char *)palloc(tl + 2 + (scale - sig.scale));
    memcpy(txt, t, tl);
    if (sig.scale == 0 && scale > 0)
      txt[tl++] = '.';
    memset(txt + tl, '0', scale - sig.scale);
    txt[tl + (scale - sig.scale)] = '\0';
    pfree(t);
  }
  len = strlen(txt) + 13;
  str = (char *)palloc(len);
  snprintf(str, len, "%se%+03d", txt, exponent);
  pfree(txt);
  return str;
}

/* num's text with no zeros after the point, nor the point if bare. */
char *numeric_normalize(Numeric num) {
  ty_dec128_t a = ty_dec128_trim_scale(dec_ty(num));

  return dec_text(a.value, a.scale);
}

/* The largest number of digits a base-10,000 value of 36 digits has. */
#define DEC_MAX_NBASE_DIGITS 21

/*
 * numeric_recv - converts external binary format to numeric: PostgreSQL's,
 * base-10,000 digits with a weight, a sign and a display scale.
 */
Datum numeric_recv(PG_FUNCTION_ARGS) {
  StringInfo buf = (StringInfo)PG_GETARG_POINTER(0);
  int32 typmod = PG_GETARG_INT32(2);
  int ndigits, weight, sign, dscale;
  ty_dec_digits_t d;
  char all[4 * 38 + 4];
  int len = 0, lead = 0;
  ty_dec128_t v;

  ndigits = (uint16)pq_getmsgint(buf, sizeof(uint16));
  weight = (int16)pq_getmsgint(buf, sizeof(int16));
  sign = (uint16)pq_getmsgint(buf, sizeof(uint16));
  if (sign != 0x0000 && sign != 0x4000) {
    if (sign == 0xC000 || sign == 0xD000 || sign == 0xF000)
      ereport(ERROR, (errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
                      errmsg("numeric has no NaN and no infinity")));
    ereport(ERROR, (errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
                    errmsg("invalid sign in external \"numeric\" value")));
  }
  dscale = (uint16)pq_getmsgint(buf, sizeof(uint16));
  if ((dscale & 0x3FFF) != dscale)
    ereport(ERROR, (errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
                    errmsg("invalid scale in external \"numeric\" value")));
  for (int i = 0; i < ndigits; i++) {
    int g = (int16)pq_getmsgint(buf, sizeof(int16));

    if (g < 0 || g >= 10000)
      ereport(ERROR, (errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
                      errmsg("invalid digit in external \"numeric\" value")));
    if (i >= 38)
      ereport(ERROR, (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
                      errmsg("value overflows numeric format")));
    all[len++] = (char)('0' + g / 1000);
    all[len++] = (char)('0' + g / 100 % 10);
    all[len++] = (char)('0' + g / 10 % 10);
    all[len++] = (char)('0' + g % 10);
  }
  while (lead < len && all[lead] == '0')
    lead++;
  d.nd = len - lead;
  memcpy(d.dig, all + lead, d.nd);
  /* The digits' value is that integer times 10000^(weight - ndigits + 1). */
  d.scale = 4 * (ndigits - 1 - weight);
  if (d.scale < 0) {
    if (d.nd > 0) {
      if (d.nd - d.scale > TY_DEC_DIGITS_MAX)
        ereport(ERROR, (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
                        errmsg("value overflows numeric format")));
      memset(d.dig + d.nd, '0', -d.scale);
      d.nd -= d.scale;
    }
    d.scale = 0;
  }
  /* A display scale hiding digits truncates them, as PostgreSQL's does. */
  ty_dec_digits_drop(&d, d.scale - Min(dscale, NUMERIC_MAX_SCALE + 1), 1);
  if (d.scale < dscale && dscale <= NUMERIC_MAX_SCALE &&
      0 != ty_dec_digits_pad(&d, dscale))
    ereport(ERROR, (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
                    errmsg("value overflows numeric format")));
  if (is_valid_numeric_typmod(typmod) &&
      0 != ty_dec_digits_typmod(&d, numeric_typmod_precision(typmod),
                                numeric_typmod_scale(typmod)))
    dec_in_error(TY_DEC_IN_FIELD, NULL, typmod, NULL);
  if (0 != ty_dec_digits_pack(&d, sign == 0x4000 && d.nd != 0, &v))
    dec_range_error(NULL);
  PG_RETURN_NUMERIC(dec_make(v.value, v.scale));
}

/* numeric_send - converts numeric to PostgreSQL's binary format. */
Datum numeric_send(PG_FUNCTION_ARGS) {
  Numeric num = PG_GETARG_NUMERIC(0);
  i128 v = dec_value(num);
  int scale = dec_scale(num);
  StringInfoData buf;
  int digits[DEC_MAX_NBASE_DIGITS + 2];
  char full[64];
  int n = 0, first, last, weight, len, nd;
  int s4 = (scale + 3) / 4 * 4;

  /* The digits of |v|, then zeros to scale s4: base-10,000 digits of it. */
  nd = v == 0 ? 0 : dec_digits_text(v < 0 ? -(u128)v : (u128)v, full);
  memset(full + nd, '0', s4 - scale);
  len = nd + (s4 - scale);
  /* Least significant first. */
  for (int end = len; nd > 0 && end > 0; end -= 4) {
    int g = 0;

    for (int k = Max(0, end - 4); k < end; k++)
      g = g * 10 + (full[k] - '0');
    digits[n++] = g;
  }
  /* The most significant is digits[n - 1], of weight n - 1 - s4 / 4. */
  weight = n - 1 - s4 / 4;
  last = 0;
  while (last < n && digits[last] == 0)
    last++;
  first = n - 1;
  pq_begintypsend(&buf);
  if (n == 0) {
    pq_sendint16(&buf, 0);
    pq_sendint16(&buf, 0);
  } else {
    pq_sendint16(&buf, first - last + 1);
    pq_sendint16(&buf, weight);
  }
  pq_sendint16(&buf, v < 0 ? 0x4000 : 0x0000);
  pq_sendint16(&buf, scale);
  for (int i = first; n > 0 && i >= last; i--)
    pq_sendint16(&buf, digits[i]);
  PG_RETURN_BYTEA_P(pq_endtypsend(&buf));
}

/*
 * numeric_support() -
 *
 * Planner support function for the numeric() length coercion function.
 *
 * Flatten calls that solely represent increases in allowable precision.
 * Scale changes mutate every datum, so they are unoptimizable.
 */
Datum numeric_support(PG_FUNCTION_ARGS) {
  Node *rawreq = (Node *)PG_GETARG_POINTER(0);
  Node *ret = NULL;

  if (IsA(rawreq, SupportRequestSimplify)) {
    SupportRequestSimplify *req = (SupportRequestSimplify *)rawreq;
    FuncExpr *expr = req->fcall;
    Node *typmod;

    Assert(list_length(expr->args) >= 2);
    typmod = (Node *)lsecond(expr->args);
    if (IsA(typmod, Const) && !((Const *)typmod)->constisnull) {
      Node *source = (Node *)linitial(expr->args);
      int32 old_typmod = exprTypmod(source);
      int32 new_typmod = DatumGetInt32(((Const *)typmod)->constvalue);
      int32 old_scale = numeric_typmod_scale(old_typmod);
      int32 new_scale = numeric_typmod_scale(new_typmod);
      int32 old_precision = numeric_typmod_precision(old_typmod);
      int32 new_precision = numeric_typmod_precision(new_typmod);

      if (!is_valid_numeric_typmod(new_typmod) ||
          (is_valid_numeric_typmod(old_typmod) && new_scale == old_scale &&
           new_precision >= old_precision))
        ret = relabel_to_typmod(source, new_typmod);
    }
  }
  PG_RETURN_POINTER(ret);
}

/*
 * numeric() -
 *
 *	The length coercion of numeric: num rounded to typmod's scale, and
 *	checked against its precision.
 */
Datum numeric(PG_FUNCTION_ARGS) {
  ty_dec128_t a = dec_ty(PG_GETARG_NUMERIC(0));

  (void)dec_apply_typmod(&a, PG_GETARG_INT32(1), NULL);
  PG_RETURN_NUMERIC(dec_make(a.value, a.scale));
}

/*
 * Validate a numeric precision and scale and pack them into a typmod, with
 * soft error handling: 1 to 36 digits, a scale from 0 to the precision.
 */
int32 make_numeric_typmod_safe(int32 precision, int32 scale, Node *escontext) {
  if (precision < 1 || precision > NUMERIC_MAX_PRECISION)
    ereturn(escontext, -1,
            (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
             errmsg("NUMERIC precision %d must be between 1 and %d", precision,
                    NUMERIC_MAX_PRECISION)));
  if (scale < 0 || scale > precision)
    ereturn(escontext, -1,
            (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
             errmsg("NUMERIC scale %d must be between %d and precision %d",
                    scale, 0, precision)));
  return ((precision << 16) | (scale & 0x7ff)) + VARHDRSZ;
}

/* The typmod of numeric(p) or numeric(p, s). */
Datum numerictypmodin(PG_FUNCTION_ARGS) {
  ArrayType *ta = PG_GETARG_ARRAYTYPE_P(0);
  int32 *tl;
  int n;
  int32 typmod;

  tl = ArrayGetIntegerTypmods(ta, &n);
  if (n == 2)
    typmod = make_numeric_typmod_safe(tl[0], tl[1], NULL);
  else if (n == 1)
    typmod = make_numeric_typmod_safe(tl[0], 0, NULL); /* scale defaults to 0 */
  else {
    ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                    errmsg("invalid NUMERIC type modifier")));
    typmod = 0; /* keep compiler quiet */
  }
  PG_RETURN_INT32(typmod);
}

/* A numeric typmod's text: "(p,s)". */
Datum numerictypmodout(PG_FUNCTION_ARGS) {
  int32 typmod = PG_GETARG_INT32(0);
  char *res = (char *)palloc(64);

  if (is_valid_numeric_typmod(typmod))
    snprintf(res, 64, "(%d,%d)", numeric_typmod_precision(typmod),
             numeric_typmod_scale(typmod));
  else
    *res = '\0';
  PG_RETURN_CSTRING(res);
}

/* The largest size a value of typmod takes: every value is a decimal_t. */
int32 numeric_maximum_size(int32 typmod) { return sizeof(decimal_t); }

/* Whether num is NaN: no value is. */
bool numeric_is_nan(Numeric num) { return false; }

/* Whether num is an infinity: no value is. */
bool numeric_is_inf(Numeric num) { return false; }

/* ----------------------------------------------------------------------
 * Sign, rounding and the like
 * ----------------------------------------------------------------------
 */

/* |num|, at its scale. */
Datum numeric_abs(PG_FUNCTION_ARGS) {
  Numeric n = PG_GETARG_NUMERIC(0);
  ty_dec128_t r = ty_dec128_abs({dec_value(n), dec_scale(n)});

  PG_RETURN_NUMERIC(dec_make(r.value, r.scale));
}

/* -num, at its scale. */
Datum numeric_uminus(PG_FUNCTION_ARGS) {
  Numeric n = PG_GETARG_NUMERIC(0);
  ty_dec128_t r = ty_dec128_neg({dec_value(n), dec_scale(n)});

  PG_RETURN_NUMERIC(dec_make(r.value, r.scale));
}

/* +num: a copy. */
Datum numeric_uplus(PG_FUNCTION_ARGS) {
  PG_RETURN_NUMERIC(DatumGetNumericCopy(PG_GETARG_DATUM(0)));
}

/* -1, 0 or 1 as num is negative, zero or positive. */
Datum numeric_sign(PG_FUNCTION_ARGS) {
  Numeric n = PG_GETARG_NUMERIC(0);

  PG_RETURN_NUMERIC(
      dec_make(ty_dec128_sign({dec_value(n), dec_scale(n)}), 0));
}


/*
 * num rounded, or truncated if trunc, to n places: ty's, exact, or the
 * error of a result past 36 digits.
 */
static Datum dec_round_or_trunc(Numeric num, int n, bool trunc) {
  ty_dec128_t a = {dec_value(num), dec_scale(num)};
  ty_dec128_t r;

  if ((trunc ? ty_dec128_trunc(&a, n, &r) : ty_dec128_round(&a, n, &r)) !=
      0)
    dec_range_error(NULL);
  PG_RETURN_NUMERIC(dec_make(r.value, r.scale));
}

/*
 * num rounded to scale places, half away from zero; a negative scale
 * rounds to tens, hundreds and so on, and gives scale 0.
 */
Datum numeric_round(PG_FUNCTION_ARGS) {
  return dec_round_or_trunc(PG_GETARG_NUMERIC(0), PG_GETARG_INT32(1), false);
}

/* num truncated to scale places, as numeric_round(). */
Datum numeric_trunc(PG_FUNCTION_ARGS) {
  return dec_round_or_trunc(PG_GETARG_NUMERIC(0), PG_GETARG_INT32(1), true);
}

/* The smallest integer not below num, at scale 0. */
Datum numeric_ceil(PG_FUNCTION_ARGS) {
  Numeric n = PG_GETARG_NUMERIC(0);
  ty_dec128_t r = ty_dec128_ceil({dec_value(n), dec_scale(n)});

  PG_RETURN_NUMERIC(dec_make(r.value, r.scale));
}

/* The largest integer not above num, at scale 0. */
Datum numeric_floor(PG_FUNCTION_ARGS) {
  Numeric n = PG_GETARG_NUMERIC(0);
  ty_dec128_t r = ty_dec128_floor({dec_value(n), dec_scale(n)});

  PG_RETURN_NUMERIC(dec_make(r.value, r.scale));
}

/* num's scale. */
Datum numeric_scale(PG_FUNCTION_ARGS) {
  PG_RETURN_INT32(dec_scale(PG_GETARG_NUMERIC(0)));
}

/* The fewest places num needs: its scale with no zeros at the end. */
Datum numeric_min_scale(PG_FUNCTION_ARGS) {
  ty_dec128_t a = dec_ty(PG_GETARG_NUMERIC(0));

  PG_RETURN_INT32(ty_dec128_min_scale(&a));
}

/* num at the fewest places it needs. */
Datum numeric_trim_scale(PG_FUNCTION_ARGS) {
  ty_dec128_t a = ty_dec128_trim_scale(dec_ty(PG_GETARG_NUMERIC(0)));

  PG_RETURN_NUMERIC(dec_make(a.value, a.scale));
}

/* ----------------------------------------------------------------------
 * generate_series and width_bucket
 * ----------------------------------------------------------------------
 */

/* Where a numeric generate_series stands. */
typedef struct series_fctx_t {
  ty_dec128_t current;
  ty_dec128_t stop;
  ty_dec128_t step;
  bool done; /* the next value passed 36 digits, so it is past stop */
} series_fctx_t;

/* generate_series(start, stop): by 1. */
Datum generate_series_numeric(PG_FUNCTION_ARGS) {
  return generate_series_step_numeric(fcinfo);
}

/* generate_series(start, stop, step), step not zero. */
Datum generate_series_step_numeric(PG_FUNCTION_ARGS) {
  series_fctx_t *fctx;
  FuncCallContext *funcctx;

  if (SRF_IS_FIRSTCALL()) {
    MemoryContext oldcontext;
    ty_dec128_t step = {1, 0};

    if (PG_NARGS() == 3) {
      step = dec_ty(PG_GETARG_NUMERIC(2));
      if (step.value == 0)
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("step size cannot equal zero")));
    }
    funcctx = SRF_FIRSTCALL_INIT();
    oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);
    fctx = (series_fctx_t *)palloc(sizeof(series_fctx_t));
    fctx->current = dec_ty(PG_GETARG_NUMERIC(0));
    fctx->stop = dec_ty(PG_GETARG_NUMERIC(1));
    fctx->step = step;
    fctx->done = false;
    funcctx->user_fctx = fctx;
    MemoryContextSwitchTo(oldcontext);
  }
  funcctx = SRF_PERCALL_SETUP();
  fctx = (series_fctx_t *)funcctx->user_fctx;
  if (!fctx->done &&
      ((fctx->step.value > 0 &&
        ty_dec128_cmp(fctx->current, fctx->stop) <= 0) ||
       (fctx->step.value < 0 &&
        ty_dec128_cmp(fctx->current, fctx->stop) >= 0))) {
    Numeric result = dec_make(fctx->current.value, fctx->current.scale);
    ty_dec128_t next;

    /* A next value of more than 36 digits is past stop: the last. */
    if (ty_dec128_add(&fctx->current, &fctx->step, &next) == 0)
      fctx->current = next;
    else
      fctx->done = true;
    SRF_RETURN_NEXT(funcctx, NumericGetDatum(result));
  }
  SRF_RETURN_DONE(funcctx);
}

/* Planner support for generate_series of numerics: its rows. */
Datum generate_series_numeric_support(PG_FUNCTION_ARGS) {
  Node *rawreq = (Node *)PG_GETARG_POINTER(0);
  Node *ret = NULL;

  if (IsA(rawreq, SupportRequestRows)) {
    SupportRequestRows *req = (SupportRequestRows *)rawreq;

    if (is_funcclause(req->node)) /* be paranoid */
    {
      List *args = ((FuncExpr *)req->node)->args;
      Node *arg1, *arg2, *arg3;

      arg1 = estimate_expression_value(req->root, (Node *)linitial(args));
      arg2 = estimate_expression_value(req->root, (Node *)lsecond(args));
      arg3 = list_length(args) >= 3
                 ? estimate_expression_value(req->root, (Node *)lthird(args))
                 : NULL;
      if ((IsA(arg1, Const) && ((Const *)arg1)->constisnull) ||
          (IsA(arg2, Const) && ((Const *)arg2)->constisnull) ||
          (arg3 != NULL && IsA(arg3, Const) && ((Const *)arg3)->constisnull)) {
        req->rows = 0;
        ret = (Node *)req;
      } else if (IsA(arg1, Const) && IsA(arg2, Const) &&
                 (arg3 == NULL || IsA(arg3, Const))) {
        ty_dec128_t start =
            dec_ty(DatumGetNumeric(((Const *)arg1)->constvalue));
        ty_dec128_t stop =
            dec_ty(DatumGetNumeric(((Const *)arg2)->constvalue));
        ty_dec128_t step =
            arg3 ? dec_ty(DatumGetNumeric(((Const *)arg3)->constvalue))
                 : ty_dec128_t{1, 0};
        ty_dec128_t res, q;

        /* An estimate: if a difference passes 36 digits, there is none. */
        if (step.value != 0 && ty_dec128_sub(&stop, &start, &res) == 0) {
          if (res.value != 0 && (step.value < 0) != (res.value < 0)) {
            req->rows = 0;
            ret = (Node *)req;
          } else if (ty_dec128_div(&res, &step, 0, true, &q) == 0) {
            req->rows = (double)q.value + 1;
            ret = (Node *)req;
          }
        }
      }
    }
  }
  PG_RETURN_POINTER(ret);
}

/*
 * width_bucket(operand, bound1, bound2, count): which of count equal
 * buckets between the bounds operand falls in; 0 before them, count + 1
 * after.
 */
Datum width_bucket_numeric(PG_FUNCTION_ARGS) {
  ty_dec128_t operand = dec_ty(PG_GETARG_NUMERIC(0));
  ty_dec128_t bound1 = dec_ty(PG_GETARG_NUMERIC(1));
  ty_dec128_t bound2 = dec_ty(PG_GETARG_NUMERIC(2));
  int32 count = PG_GETARG_INT32(3);
  int64 result;

  if (count <= 0)
    ereport(ERROR, (errcode(ERRCODE_INVALID_ARGUMENT_FOR_WIDTH_BUCKET_FUNCTION),
                    errmsg("count must be greater than zero")));
  if (ty_dec128_cmp(bound1, bound2) == 0)
    ereport(ERROR, (errcode(ERRCODE_INVALID_ARGUMENT_FOR_WIDTH_BUCKET_FUNCTION),
                    errmsg("lower bound cannot equal upper bound")));
  /* The bucket is ty's, made in 128 bits. */
  if (ty_dec128_width_bucket(&operand, &bound1, &bound2, count, &result) !=
      0)
    dec_range_error(NULL);
  if (result > PG_INT32_MAX)
    ereport(ERROR, (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
                    errmsg("integer out of range")));
  PG_RETURN_INT32((int32)result);
}

/* ----------------------------------------------------------------------
 * Comparison and hashing
 * ----------------------------------------------------------------------
 */

/* -1, 0 or 1 as a is below, equal to or above b, at any scales. */
static int dec_cmp(const decimal_t *a, const decimal_t *b) {
  i128 va = dec_value(a), vb = dec_value(b);

  if (dec_scale(a) == dec_scale(b))
    return va < vb ? -1 : (va > vb ? 1 : 0);
  return ty_dec128_cmp_scaled({va, dec_scale(a)}, {vb, dec_scale(b)});
}

/* The comparator of a sort of numerics. */
static int numeric_fast_cmp(Datum x, Datum y, SortSupport ssup) {
  return dec_cmp(DatumGetNumeric(x), DatumGetNumeric(y));
}

/* Sort support: a plain comparator, no abbreviation. */
Datum numeric_sortsupport(PG_FUNCTION_ARGS) {
  SortSupport ssup = (SortSupport)PG_GETARG_POINTER(0);

  ssup->comparator = numeric_fast_cmp;
  PG_RETURN_VOID();
}

/* btree comparison: -1, 0 or 1. */
Datum numeric_cmp(PG_FUNCTION_ARGS) {
  PG_RETURN_INT32(dec_cmp(PG_GETARG_NUMERIC(0), PG_GETARG_NUMERIC(1)));
}

/* a = b, at any scales: 1.5 = 1.50. */
Datum numeric_eq(PG_FUNCTION_ARGS) {
  PG_RETURN_BOOL(dec_cmp(PG_GETARG_NUMERIC(0), PG_GETARG_NUMERIC(1)) == 0);
}

/* a <> b. */
Datum numeric_ne(PG_FUNCTION_ARGS) {
  PG_RETURN_BOOL(dec_cmp(PG_GETARG_NUMERIC(0), PG_GETARG_NUMERIC(1)) != 0);
}

/* a > b. */
Datum numeric_gt(PG_FUNCTION_ARGS) {
  PG_RETURN_BOOL(dec_cmp(PG_GETARG_NUMERIC(0), PG_GETARG_NUMERIC(1)) > 0);
}

/* a >= b. */
Datum numeric_ge(PG_FUNCTION_ARGS) {
  PG_RETURN_BOOL(dec_cmp(PG_GETARG_NUMERIC(0), PG_GETARG_NUMERIC(1)) >= 0);
}

/* a < b. */
Datum numeric_lt(PG_FUNCTION_ARGS) {
  PG_RETURN_BOOL(dec_cmp(PG_GETARG_NUMERIC(0), PG_GETARG_NUMERIC(1)) < 0);
}

/* a <= b. */
Datum numeric_le(PG_FUNCTION_ARGS) {
  PG_RETURN_BOOL(dec_cmp(PG_GETARG_NUMERIC(0), PG_GETARG_NUMERIC(1)) <= 0);
}

/*
 * in_range support for a window's RANGE frame: whether val is within
 * offset before (sub) or after base, as less says.
 */
Datum in_range_numeric_numeric(PG_FUNCTION_ARGS) {
  ty_dec128_t val = dec_ty(PG_GETARG_NUMERIC(0));
  ty_dec128_t base = dec_ty(PG_GETARG_NUMERIC(1));
  ty_dec128_t offset = dec_ty(PG_GETARG_NUMERIC(2));
  bool sub = PG_GETARG_BOOL(3);
  bool less = PG_GETARG_BOOL(4);
  ty_dec128_t sum;
  int c;

  if (offset.value < 0)
    ereport(ERROR,
            (errcode(ERRCODE_INVALID_PRECEDING_OR_FOLLOWING_SIZE),
             errmsg("invalid preceding or following size in window function")));
  if ((sub ? ty_dec128_sub(&base, &offset, &sum)
           : ty_dec128_add(&base, &offset, &sum)) == 0) {
    c = ty_dec128_cmp(val, sum);
  } else {
    /* The sum passes 36 digits, so val is on one side of it, by its sign. */
    double lb = ty_dec128_to_double(&base);
    double lo = ty_dec128_to_double(&offset);

    c = (sub ? lb - lo : lb + lo) > 0 ? -1 : 1;
  }
  PG_RETURN_BOOL(less ? c <= 0 : c >= 0);
}

/* hash_numeric: ty's hash; equal values, whatever their scales, hash alike. */
Datum hash_numeric(PG_FUNCTION_ARGS) {
  ty_dec128_t a = dec_ty(PG_GETARG_NUMERIC(0));

  PG_RETURN_UINT32((uint32)ty_dec128_hash(&a, 0));
}

/* hash_numeric_extended: as hash_numeric, with a seed. */
Datum hash_numeric_extended(PG_FUNCTION_ARGS) {
  ty_dec128_t a = dec_ty(PG_GETARG_NUMERIC(0));

  PG_RETURN_UINT64(ty_dec128_hash(&a, (uint64)PG_GETARG_INT64(1)));
}

/* ----------------------------------------------------------------------
 * Arithmetic
 * ----------------------------------------------------------------------
 */

/* The base-10000 shape of a value v at scale, as dec_nbase() gives it. */
static nbase_t dec_nbase128(i128 v, int scale) {
  nbase_t nb = {0, 0, 0, 1};
  u128 m, head, rest = 0, p = 1;
  int e, shift;

  if (v == 0)
    return nb;
  m = v < 0 ? -(u128)v : (u128)v;
  /* e is the decimal exponent of the leading digit. */
  e = u128_digits(m) - 1 - scale;
  nb.weight = e >= 0 ? e / 4 : -((-e + 3) / 4);
  /* head is the value / 10000^weight, scaled to keep 8 more digits. */
  shift = scale + 4 * nb.weight - 8;
  for (int i = 0; i < (shift >= 0 ? shift : -shift); i++)
    p *= 10;
  if (shift >= 0) {
    head = m / p;
    rest = m % p;
  } else {
    head = m * p;
  }
  {
    uint64 r1 = (uint64)(head % 100000000), r2 = r1 % 10000;

    nb.first = (int)(head / 100000000);
    nb.second = (int)(r1 / 10000);
    nb.ndigits = (r1 != 0 || rest != 0 || r2 != 0) ? 2 : 1;
  }
  return nb;
}

/*
 * The base-10000 shape of a positive v, a double: its weight and its
 * first digit, as dec_nbase128() gives them.
 */
static nbase_t dec_nbase_d(double v) {
  nbase_t nb = {0, 0, 0, 1};
  int e;
  double scaled;

  if (!(v > 0))
    return nb;
  e = (int)floor(log10(v));
  while (pow(10.0, e + 1) <= v)
    e++;
  while (pow(10.0, e) > v)
    e--;
  nb.weight = e >= 0 ? e / 4 : -((-e + 3) / 4);
  scaled = v / pow(10000.0, nb.weight);
  nb.first = (int)scaled;
  nb.second = (int)((scaled - nb.first) * 10000);
  nb.ndigits = (scaled - nb.first) != 0 ? 2 : 1;
  return nb;
}

/* PostgreSQL's scale for a quotient of two shapes: select_div_scale(). */
static int select_div_scale_nb(const nbase_t &na, int sa, const nbase_t &nb,
                               int sb) {
  int qweight = na.weight - nb.weight;
  int rscale;

  if (na.first <= nb.first)
    qweight--;
  rscale = NUMERIC_MIN_SIG_DIGITS - qweight * 4;
  rscale = Max(rscale, sa);
  rscale = Max(rscale, sb);
  rscale = Max(rscale, NUMERIC_MIN_DISPLAY_SCALE);
  rscale = Min(rscale, NUMERIC_MAX_DISPLAY_SCALE);
  return rscale;
}

/* PostgreSQL's scale for a quotient of two values: select_div_scale(). */
static int select_div_scale128(i128 va, int sa, i128 vb, int sb) {
  nbase_t na = dec_nbase128(va, sa), nb = dec_nbase128(vb, sb);
  int qweight = na.weight - nb.weight;
  int rscale;

  if (na.first <= nb.first)
    qweight--;
  rscale = NUMERIC_MIN_SIG_DIGITS - qweight * 4;
  rscale = Max(rscale, sa);
  rscale = Max(rscale, sb);
  rscale = Max(rscale, NUMERIC_MIN_DISPLAY_SCALE);
  rscale = Min(rscale, NUMERIC_MAX_DISPLAY_SCALE);
  return rscale;
}

/* The error of a division by zero, or false with *have_error set. */
static bool division_by_zero(bool *have_error) {
  if (have_error == NULL)
    ereport(ERROR,
            (errcode(ERRCODE_DIVISION_BY_ZERO), errmsg("division by zero")));
  *have_error = true;
  return false;
}

/*
 * num1 + num2, a decimal128 as an int64 + an int64 is an int64: ty adds
 * them exactly, or NULL with *have_error set past 36 digits.
 */
Numeric numeric_add_opt_error(Numeric num1, Numeric num2, bool *have_error) {
  ty_dec128_t a = {dec_value(num1), dec_scale(num1)};
  ty_dec128_t b = {dec_value(num2), dec_scale(num2)};
  ty_dec128_t r;

  if (ty_dec128_add(&a, &b, &r) != 0)
    return dec_range_error(have_error);
  return dec_make(r.value, r.scale);
}

/* num1 - num2, as num1 + num2: exact, or NULL with *have_error set. */
Numeric numeric_sub_opt_error(Numeric num1, Numeric num2, bool *have_error) {
  ty_dec128_t a = {dec_value(num1), dec_scale(num1)};
  ty_dec128_t b = {dec_value(num2), dec_scale(num2)};
  ty_dec128_t r;

  if (ty_dec128_sub(&a, &b, &r) != 0)
    return dec_range_error(have_error);
  return dec_make(r.value, r.scale);
}

/*
 * num1 * num2 at the sum of their scales: exact, or NULL with *have_error
 * set if it has more than 36 digits or 36 places.
 */
Numeric numeric_mul_opt_error(Numeric num1, Numeric num2, bool *have_error) {
  ty_dec128_t a = {dec_value(num1), dec_scale(num1)};
  ty_dec128_t b = {dec_value(num2), dec_scale(num2)};
  ty_dec128_t r;

  if (ty_dec128_mul(&a, &b, &r) != 0)
    return dec_range_error(have_error);
  return dec_make(r.value, r.scale);
}

/* num1 / num2 at PostgreSQL's scale; NULL with *have_error on an error. */
Numeric numeric_div_opt_error(Numeric num1, Numeric num2, bool *have_error) {
  ty_dec128_t a = {dec_value(num1), dec_scale(num1)};
  ty_dec128_t b = {dec_value(num2), dec_scale(num2)};
  ty_dec128_t r;

  if (b.value == 0) {
    division_by_zero(have_error);
    return NULL;
  }
  /* The quotient is ty's, made in 128 bits, at PostgreSQL's scale. */
  if (ty_dec128_div(&a, &b,
                        select_div_scale128(a.value, a.scale, b.value, b.scale),
                        false, &r) != 0)
    return dec_range_error(have_error);
  return dec_make(r.value, r.scale);
}

/* num1 mod num2, at the larger scale, the sign of num1. */
Numeric numeric_mod_opt_error(Numeric num1, Numeric num2, bool *have_error) {
  ty_dec128_t a = {dec_value(num1), dec_scale(num1)};
  ty_dec128_t b = {dec_value(num2), dec_scale(num2)};
  ty_dec128_t r;

  /* The remainder is ty's; its one error is a zero num2. */
  if (ty_dec128_mod(&a, &b, &r) != 0) {
    division_by_zero(have_error);
    return NULL;
  }
  return dec_make(r.value, r.scale);
}

/* num1 + num2. */
Datum numeric_add(PG_FUNCTION_ARGS) {
  PG_RETURN_NUMERIC(
      numeric_add_opt_error(PG_GETARG_NUMERIC(0), PG_GETARG_NUMERIC(1), NULL));
}

/* num1 - num2. */
Datum numeric_sub(PG_FUNCTION_ARGS) {
  PG_RETURN_NUMERIC(
      numeric_sub_opt_error(PG_GETARG_NUMERIC(0), PG_GETARG_NUMERIC(1), NULL));
}

/* num1 * num2. */
Datum numeric_mul(PG_FUNCTION_ARGS) {
  PG_RETURN_NUMERIC(
      numeric_mul_opt_error(PG_GETARG_NUMERIC(0), PG_GETARG_NUMERIC(1), NULL));
}

/* num1 / num2. */
Datum numeric_div(PG_FUNCTION_ARGS) {
  PG_RETURN_NUMERIC(
      numeric_div_opt_error(PG_GETARG_NUMERIC(0), PG_GETARG_NUMERIC(1), NULL));
}

/* div(num1, num2): the quotient truncated to an integer, scale 0. */
Datum numeric_div_trunc(PG_FUNCTION_ARGS) {
  Numeric n1 = PG_GETARG_NUMERIC(0), n2 = PG_GETARG_NUMERIC(1);
  ty_dec128_t a = {dec_value(n1), dec_scale(n1)};
  ty_dec128_t b = {dec_value(n2), dec_scale(n2)};
  ty_dec128_t r;

  if (b.value == 0)
    division_by_zero(NULL);
  if (ty_dec128_div(&a, &b, 0, true, &r) != 0)
    dec_range_error(NULL);
  PG_RETURN_NUMERIC(dec_make(r.value, r.scale));
}

/* num1 % num2. */
Datum numeric_mod(PG_FUNCTION_ARGS) {
  PG_RETURN_NUMERIC(
      numeric_mod_opt_error(PG_GETARG_NUMERIC(0), PG_GETARG_NUMERIC(1), NULL));
}

/* num + 1, at its scale. */
Datum numeric_inc(PG_FUNCTION_ARGS) {
  ty_dec128_t a = dec_ty(PG_GETARG_NUMERIC(0)), one = {1, 0}, r;

  if (ty_dec128_add(&a, &one, &r) != 0)
    dec_range_error(NULL);
  PG_RETURN_NUMERIC(dec_make(r.value, r.scale));
}

/* The smaller of two values: a copy of it. */
Datum numeric_smaller(PG_FUNCTION_ARGS) {
  Numeric a = PG_GETARG_NUMERIC(0), b = PG_GETARG_NUMERIC(1);

  PG_RETURN_NUMERIC(
      DatumGetNumericCopy(NumericGetDatum(dec_cmp(a, b) <= 0 ? a : b)));
}

/* The larger of two values: a copy of it. */
Datum numeric_larger(PG_FUNCTION_ARGS) {
  Numeric a = PG_GETARG_NUMERIC(0), b = PG_GETARG_NUMERIC(1);

  PG_RETURN_NUMERIC(
      DatumGetNumericCopy(NumericGetDatum(dec_cmp(a, b) >= 0 ? a : b)));
}

/*
 * gcd(num1, num2): the largest value both are whole multiples of, at the
 * larger scale; never negative.
 */
Datum numeric_gcd(PG_FUNCTION_ARGS) {
  Numeric n1 = PG_GETARG_NUMERIC(0), n2 = PG_GETARG_NUMERIC(1);
  ty_dec128_t a = {dec_value(n1), dec_scale(n1)};
  ty_dec128_t b = {dec_value(n2), dec_scale(n2)};
  ty_dec128_t r;

  if (ty_dec128_gcd(&a, &b, &r) != 0)
    dec_range_error(NULL);
  PG_RETURN_NUMERIC(dec_make(r.value, r.scale));
}

/*
 * lcm(num1, num2): the smallest value that is a whole multiple of both, at
 * the larger scale; zero if either is.
 */
Datum numeric_lcm(PG_FUNCTION_ARGS) {
  Numeric n1 = PG_GETARG_NUMERIC(0), n2 = PG_GETARG_NUMERIC(1);
  ty_dec128_t a = {dec_value(n1), dec_scale(n1)};
  ty_dec128_t b = {dec_value(n2), dec_scale(n2)};
  ty_dec128_t r;

  if (ty_dec128_lcm(&a, &b, &r) != 0)
    dec_range_error(NULL);
  PG_RETURN_NUMERIC(dec_make(r.value, r.scale));
}

/* factorial(n), for n of at most 33, the last whose factorial fits. */
Datum numeric_fac(PG_FUNCTION_ARGS) {
  int64 num = PG_GETARG_INT64(0);
  ty_dec128_t r;

  if (num < 0)
    ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                    errmsg("factorial of a negative number is undefined")));
  if (num > 100)
    ereport(ERROR, (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
                    errmsg("value overflows numeric format")));
  if (ty_dec128_factorial(num, &r) != 0)
    dec_range_error(NULL);
  PG_RETURN_NUMERIC(dec_make(r.value, r.scale));
}

/* ----------------------------------------------------------------------
 * Square root, logarithms and powers
 * ----------------------------------------------------------------------
 */

/* The square root of num, at PostgreSQL's scale for it, rounded. */
Datum numeric_sqrt(PG_FUNCTION_ARGS) {
  ty_dec128_t arg = dec_ty(PG_GETARG_NUMERIC(0)), r;
  nbase_t nb = dec_nbase128(arg.value, arg.scale);
  int sweight, rscale;

  if (arg.value < 0)
    ereport(ERROR, (errcode(ERRCODE_INVALID_ARGUMENT_FOR_POWER_FUNCTION),
                    errmsg("cannot take square root of a negative number")));
  /* At least 16 significant digits, not below the argument's scale. */
  sweight = nb.weight * 4 / 2 + 1;
  rscale = NUMERIC_MIN_SIG_DIGITS - sweight;
  rscale = Max(rscale, arg.scale);
  rscale = Max(rscale, NUMERIC_MIN_DISPLAY_SCALE);
  rscale = Min(rscale, NUMERIC_MAX_DISPLAY_SCALE);
  /* The root is ty's, made a digit at a time in 128 bits. */
  if (ty_dec128_sqrt(&arg, rscale, &r) != 0)
    dec_range_error(NULL);
  PG_RETURN_NUMERIC(dec_make(r.value, r.scale));
}

/*
 * The decimal weight of ln(w), as PostgreSQL estimates it to choose a
 * result scale; w is positive.
 */
static int estimate_ln_dweight(const ty_dec128_t &w) {
  ty_dec128_t lo = {9, 1}, hi = {11, 1};

  if (w.value <= 0)
    return 0;
  if (ty_dec128_cmp(w, lo) >= 0 && ty_dec128_cmp(w, hi) <= 0) {
    /* 0.9 <= w <= 1.1: ln(w) is about w - 1. */
    ty_dec128_t one = {1, 0}, x;

    ty_dec128_sub(&w, &one, &x);
    if (x.value == 0)
      return 0;
    return u128_digits(x.value < 0 ? -(u128)x.value : (u128)x.value) - 1 -
           x.scale;
  } else {
    nbase_t nb = dec_nbase128(w.value, w.scale);
    int digits = nb.first;
    int dweight = nb.weight * 4;
    double ln_var;

    if (nb.ndigits > 1) {
      digits = digits * 10000 + nb.second;
      dweight -= 4;
    }
    ln_var = log((double)digits) + dweight * 2.302585092994046;
    return (int)log10(fabs(ln_var));
  }
}

/* The error of a logarithm of zero or a negative number, if w is one. */
static void check_log_argument(const ty_dec128_t &w) {
  if (w.value == 0)
    ereport(ERROR, (errcode(ERRCODE_INVALID_ARGUMENT_FOR_LOG),
                    errmsg("cannot take logarithm of zero")));
  if (w.value < 0)
    ereport(ERROR, (errcode(ERRCODE_INVALID_ARGUMENT_FOR_LOG),
                    errmsg("cannot take logarithm of a negative number")));
}

/* e raised to num, at PostgreSQL's scale for it. */
Datum numeric_exp(PG_FUNCTION_ARGS) {
  ty_dec128_t arg = dec_ty(PG_GETARG_NUMERIC(0)), r;
  double x = ty_dec128_to_double(&arg);
  double val = (double)x * 0.434294481903252; /* the result's dweight */
  int rscale;

  val = Max(val, -NUMERIC_MAX_RESULT_SCALE);
  val = Min(val, NUMERIC_MAX_RESULT_SCALE);
  rscale = NUMERIC_MIN_SIG_DIGITS - (int)val;
  rscale = Max(rscale, arg.scale);
  rscale = Max(rscale, NUMERIC_MIN_DISPLAY_SCALE);
  rscale = Min(rscale, NUMERIC_MAX_DISPLAY_SCALE);
  if (ty_dec128_exp(&arg, rscale, &r) != 0)
    dec_math_error();
  PG_RETURN_NUMERIC(dec_make(r.value, r.scale));
}

/* The natural logarithm of num, at PostgreSQL's scale for it. */
Datum numeric_ln(PG_FUNCTION_ARGS) {
  ty_dec128_t arg = dec_ty(PG_GETARG_NUMERIC(0)), r;
  int rscale;

  check_log_argument(arg);
  rscale = NUMERIC_MIN_SIG_DIGITS - estimate_ln_dweight(arg);
  rscale = Max(rscale, arg.scale);
  rscale = Max(rscale, NUMERIC_MIN_DISPLAY_SCALE);
  rscale = Min(rscale, NUMERIC_MAX_DISPLAY_SCALE);
  if (ty_dec128_ln(&arg, rscale, &r) != 0)
    dec_math_error();
  PG_RETURN_NUMERIC(dec_make(r.value, r.scale));
}

/* log(base, num): the logarithm of num to base, at PostgreSQL's scale. */
Datum numeric_log(PG_FUNCTION_ARGS) {
  ty_dec128_t base = dec_ty(PG_GETARG_NUMERIC(0));
  ty_dec128_t num = dec_ty(PG_GETARG_NUMERIC(1)), r;
  int result_dweight, rscale;

  check_log_argument(base);
  check_log_argument(num);
  result_dweight = estimate_ln_dweight(num) - estimate_ln_dweight(base);
  rscale = NUMERIC_MIN_SIG_DIGITS - result_dweight;
  rscale = Max(rscale, base.scale);
  rscale = Max(rscale, num.scale);
  rscale = Max(rscale, NUMERIC_MIN_DISPLAY_SCALE);
  rscale = Min(rscale, NUMERIC_MAX_DISPLAY_SCALE);
  if (ty_dec128_log(&base, &num, rscale, &r) != 0)
    dec_math_error();
  PG_RETURN_NUMERIC(dec_make(r.value, r.scale));
}

/*
 * base ^ exp for an integer exp, exact where its value fits 128 bits, and
 * rounded to PostgreSQL's scale for it; past that, ty takes it in long
 * double.
 */
static Numeric power_int(const ty_dec128_t &base, int64 exp,
                         int exp_scale) {
  nbase_t nb = dec_nbase128(base.value, base.scale);
  double f;
  int rscale;
  ty_dec128_t r;

  /* The approximate decimal weight of the result, PostgreSQL's way. */
  if (base.value != 0) {
    double d = nb.first;
    int p = nb.weight * 4;

    if (nb.ndigits > 1) {
      d = d * 10000 + nb.second;
      p -= 4;
    }
    f = (double)exp * (log10(d) + p);
  } else
    f = 0;
  if (f > DEC_MAX_DIGITS)
    ereport(ERROR, (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
                    errmsg("value overflows numeric format")));
  rscale = NUMERIC_MIN_SIG_DIGITS - (int)Max(f, -1000.0);
  rscale = Max(rscale, base.scale);
  rscale = Max(rscale, exp_scale);
  rscale = Max(rscale, NUMERIC_MIN_DISPLAY_SCALE);
  rscale = Min(rscale, NUMERIC_MAX_DISPLAY_SCALE);
  if (base.value == 0) {
    if (exp < 0)
      ereport(ERROR, (errcode(ERRCODE_INVALID_ARGUMENT_FOR_POWER_FUNCTION),
                      errmsg("zero raised to a negative power is undefined")));
    if (exp == 0 && rscale >= DEC_MAX_DIGITS)
      return dec_range_error(NULL);
    return dec_make(exp == 0 ? dec_pow10(rscale) : 0, rscale);
  }
  if (ty_dec128_pow_int(&base, exp, rscale, &r) != 0)
    return dec_math_error();
  return dec_make(r.value, r.scale);
}

/* num1 ^ num2, at PostgreSQL's scale for it. */
Datum numeric_power(PG_FUNCTION_ARGS) {
  ty_dec128_t base = dec_ty(PG_GETARG_NUMERIC(0));
  ty_dec128_t exp = dec_ty(PG_GETARG_NUMERIC(1)), exp_int, r;
  bool exp_is_int;
  double ln_num, val;
  int rscale;

  ty_dec128_trunc(&exp, 0, &exp_int);
  exp_is_int = ty_dec128_cmp(exp_int, exp) == 0;
  if (base.value == 0 && exp.value < 0)
    ereport(ERROR, (errcode(ERRCODE_INVALID_ARGUMENT_FOR_POWER_FUNCTION),
                    errmsg("zero raised to a negative power is undefined")));
  if (base.value < 0 && !exp_is_int)
    ereport(ERROR,
            (errcode(ERRCODE_INVALID_ARGUMENT_FOR_POWER_FUNCTION),
             errmsg("a negative number raised to a non-integer power yields "
                    "a complex result")));
  if (exp_is_int && u128_digits(exp_int.value < 0 ? -(u128)exp_int.value
                                                  : (u128)exp_int.value) <= 9)
    PG_RETURN_NUMERIC(power_int(base, (int64)exp_int.value, exp.scale));
  if (base.value == 0)
    PG_RETURN_NUMERIC(dec_make(0, NUMERIC_MIN_SIG_DIGITS));
  {
    ty_dec128_t abs_base = ty_dec128_abs(base);

    ln_num = log(ty_dec128_to_double(&abs_base)) * ty_dec128_to_double(&exp);
    if (fabs(ln_num) > NUMERIC_MAX_RESULT_SCALE * 3.01) {
      if (ln_num > 0)
        ereport(ERROR, (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
                        errmsg("value overflows numeric format")));
      ereport(ERROR, (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
                      errmsg("value underflows numeric format")));
    }
    val = ln_num * 0.434294481903252; /* the result's dweight */
    rscale = NUMERIC_MIN_SIG_DIGITS - (int)val;
    rscale = Max(rscale, base.scale);
    rscale = Max(rscale, exp.scale);
    rscale = Max(rscale, NUMERIC_MIN_DISPLAY_SCALE);
    rscale = Min(rscale, NUMERIC_MAX_DISPLAY_SCALE);
    /* A negative base has an integer exponent: ty keeps its sign. */
    if (ty_dec128_pow_real(&base, &exp, rscale, &r) != 0)
      dec_math_error();
    PG_RETURN_NUMERIC(dec_make(r.value, r.scale));
  }
}

/* ----------------------------------------------------------------------
 * Casts
 * ----------------------------------------------------------------------
 */

/* val as a numeric of scale 0. */
Numeric int64_to_numeric(int64 val) { return dec_make(val, 0); }

/*
 * val1 / 10^log10val2, exactly, at scale log10val2: for a fraction kept as
 * an integer, as extract() does.
 */
Numeric int64_div_fast_to_numeric(int64 val1, int log10val2) {
  if (log10val2 < 0) {
    i128 v = val1;

    if (!dec_scale_up(&v, 0, -log10val2))
      return dec_range_error(NULL);
    return dec_make(v, 0);
  }
  return dec_make(val1, log10val2);
}

/* int4 to numeric. */
Datum int4_numeric(PG_FUNCTION_ARGS) {
  PG_RETURN_NUMERIC(int64_to_numeric(PG_GETARG_INT32(0)));
}

/* int8 to numeric. */
Datum int8_numeric(PG_FUNCTION_ARGS) {
  PG_RETURN_NUMERIC(int64_to_numeric(PG_GETARG_INT64(0)));
}

/* int2 to numeric. */
Datum int2_numeric(PG_FUNCTION_ARGS) {
  PG_RETURN_NUMERIC(int64_to_numeric(PG_GETARG_INT16(0)));
}

/*
 * num rounded to an integer, half away from zero, if it lies within lo and
 * hi; else false.
 */
static bool dec_to_int(const decimal_t *num, i128 lo, i128 hi, i128 *out) {
  ty_dec128_t a = dec_ty(num), r;

  ty_dec128_round(&a, 0, &r);
  if (r.value < lo || r.value > hi)
    return false;
  *out = r.value;
  return true;
}

/* num as an int4, or false with *have_error set out of its range. */
int32 numeric_int4_opt_error(Numeric num, bool *have_error) {
  i128 v;

  if (!dec_to_int(num, PG_INT32_MIN, PG_INT32_MAX, &v)) {
    if (have_error) {
      *have_error = true;
      return 0;
    }
    ereport(ERROR, (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
                    errmsg("integer out of range")));
  }
  return (int32)v;
}

/* num as an int8, or false with *have_error set out of its range. */
int64 numeric_int8_opt_error(Numeric num, bool *have_error) {
  i128 v;

  if (!dec_to_int(num, PG_INT64_MIN, PG_INT64_MAX, &v)) {
    if (have_error) {
      *have_error = true;
      return 0;
    }
    ereport(ERROR, (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
                    errmsg("bigint out of range")));
  }
  return (int64)v;
}

/* numeric to int4, rounded. */
Datum numeric_int4(PG_FUNCTION_ARGS) {
  PG_RETURN_INT32(numeric_int4_opt_error(PG_GETARG_NUMERIC(0), NULL));
}

/* numeric to int8, rounded. */
Datum numeric_int8(PG_FUNCTION_ARGS) {
  PG_RETURN_INT64(numeric_int8_opt_error(PG_GETARG_NUMERIC(0), NULL));
}

/* numeric to int2, rounded. */
Datum numeric_int2(PG_FUNCTION_ARGS) {
  i128 v;

  if (!dec_to_int(PG_GETARG_NUMERIC(0), PG_INT16_MIN, PG_INT16_MAX, &v))
    ereport(ERROR, (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
                    errmsg("smallint out of range")));
  PG_RETURN_INT16((int16)v);
}

/* The error of a float with no numeric value: NaN or an infinity. */
static void float_not_numeric(double val) {
  ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                  errmsg("cannot convert %s to numeric",
                         std::isnan(val) ? "NaN" : "infinity")));
}

/* val, by its first digits significant digits, as a numeric: ty's. */
static Numeric float_numeric(double val, int digits) {
  ty_dec128_t a;
  int why;

  if (std::isnan(val) || std::isinf(val))
    float_not_numeric(val);
  if (0 != ty_dec128_from_float(val, digits, &a, &why)) {
    char buf[64];

    snprintf(buf, sizeof(buf), "%.*g", digits, val);
    dec_in_error(why, buf, -1, NULL);
  }
  return dec_make(a.value, a.scale);
}

/* float8 to numeric: its 15 significant digits, as PostgreSQL takes them. */
Datum float8_numeric(PG_FUNCTION_ARGS) {
  PG_RETURN_NUMERIC(float_numeric(PG_GETARG_FLOAT8(0), DBL_DIG));
}

/* float4 to numeric: its 6 significant digits. */
Datum float4_numeric(PG_FUNCTION_ARGS) {
  PG_RETURN_NUMERIC(float_numeric((double)PG_GETARG_FLOAT4(0), FLT_DIG));
}

/* numeric to float8, the nearest to its text. */
Datum numeric_float8(PG_FUNCTION_ARGS) {
  ty_dec128_t a = dec_ty(PG_GETARG_NUMERIC(0));

  PG_RETURN_FLOAT8(ty_dec128_to_float64(&a));
}

/* numeric to float8, for the planner: never an error. */
Datum numeric_float8_no_overflow(PG_FUNCTION_ARGS) {
  return numeric_float8(fcinfo);
}

/* numeric to float4, the nearest to its text. */
Datum numeric_float4(PG_FUNCTION_ARGS) {
  ty_dec128_t a = dec_ty(PG_GETARG_NUMERIC(0));

  PG_RETURN_FLOAT4(ty_dec128_to_float32(&a));
}

/* numeric to pg_lsn: rounded to an integer, from 0 to 2^64 - 1. */
Datum numeric_pg_lsn(PG_FUNCTION_ARGS) {
  ty_dec128_t a = dec_ty(PG_GETARG_NUMERIC(0)), r;

  ty_dec128_round(&a, 0, &r);
  if (r.value < 0 || r.value > (i128)UINT64_MAX)
    ereport(ERROR, (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
                    errmsg("pg_lsn out of range")));
  PG_RETURN_LSN((XLogRecPtr)(uint64)r.value);
}

/* ----------------------------------------------------------------------
 * random(min, max)
 * ----------------------------------------------------------------------
 */

/*
 * A random value from rmin to rmax, both included, uniformly, at the
 * larger of their scales.
 */
Numeric random_numeric(pg_prng_state *state, Numeric rmin, Numeric rmax) {
  i128 lo = dec_value(rmin), hi = dec_value(rmax);
  int s = Max(dec_scale(rmin), dec_scale(rmax));
  u128 range, r;
  int bits = 0;

  if (!dec_scale_up(&lo, dec_scale(rmin), s) ||
      !dec_scale_up(&hi, dec_scale(rmax), s))
    return dec_range_error(NULL);
  if (lo > hi)
    ereport(ERROR,
            (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
             errmsg("lower bound must be less than or equal to upper bound")));
  range = (u128)hi - (u128)lo;
  for (u128 t = range; t != 0; t >>= 1)
    bits++;
  /* Draw bits as wide as the range, again while the draw is past it. */
  do {
    r = 0;
    for (int i = 0; i * 64 < bits; i++)
      r |= (u128)pg_prng_uint64(state) << (64 * i);
    if (bits < 128)
      r &= (((u128)1) << bits) - 1;
  } while (r > range);
  return dec_make(lo + (i128)r, s);
}

/* ----------------------------------------------------------------------
 * Aggregates: sum, avg, variance and standard deviation, of numeric and of
 * the integers, accumulated exactly.
 * ----------------------------------------------------------------------
 */

typedef struct dec_agg_t {
  bool calc_sumX2; /* whether squares are summed, for variance */
  MemoryContext agg_context;
  int64 N; /* values counted */
  /*
   * Their sum, exact to 36 digits, as 128 bits in two halves at sx_scale;
   * and the sum of their squares in double, in bytes (a state is only
   * 8-byte aligned), as a sum of the squares of their words, in units of
   * 10^-(2 * sx2_scale): a row of a lower scale is multiplied up, and the
   * sum when a row has a higher one.
   */
  uint64 sx_lo;
  int64 sx_hi;
  int sx_scale;
  int sx2_scale;
  unsigned char sx2[8];
  int max_scale;         /* the largest scale among them */
  int64 max_scale_count; /* how many have it */
  /*
   * A decimal64 sum not yet in the sum (agg_flush()): 128 bits in two
   * halves, at fast_scale, if fast.
   */
  bool fast;
  int fast_scale;
  uint64 fast_lo;
  int64 fast_hi;
} dec_agg_t;

/* state's sum. */
static ty_dec128_t agg_sumX(const dec_agg_t *state) {
  return {(i128)(((u128)(uint64)state->sx_hi << 64) | state->sx_lo),
          state->sx_scale};
}

/* Set state's sum to v. */
static void agg_set_sumX(dec_agg_t *state, const ty_dec128_t &v) {
  state->sx_lo = (uint64)(u128)v.value;
  state->sx_hi = (int64)((u128)v.value >> 64);
  state->sx_scale = v.scale;
}

/* 10^n as a double. */
static double agg_pow10(int n) {
  double p = 1;

  while (n-- > 0)
    p *= 10;
  return p;
}

/* state's sum of squares of words, in units of 10^-(2 * its scale). */
static double agg_sumX2_raw(const dec_agg_t *state) {
  double x;

  memcpy(&x, state->sx2, sizeof(x));
  return x;
}

/* Set state's sum of squares to x, in units of 10^-(2 * scale). */
static void agg_set_sumX2_raw(dec_agg_t *state, double x, int scale) {
  if (!std::isfinite(x))
    dec_range_error(NULL);
  memcpy(state->sx2, &x, sizeof(x));
  state->sx2_scale = scale;
}

/* state's sum of squares in units of 10^-(2 * scale), not below its scale. */
static double agg_sumX2_at(const dec_agg_t *state, int scale) {
  double x = agg_sumX2_raw(state);

  return scale == state->sx2_scale
             ? x
             : x * agg_pow10(2 * (scale - state->sx2_scale));
}

/* state's sum of squares, as a value. */
static double agg_sumX2(const dec_agg_t *state) {
  return agg_sumX2_raw(state) / agg_pow10(2 * state->sx2_scale);
}

/* Add the square of the word v at scale to state's sum of squares. */
static void agg_add_square(dec_agg_t *state, i128 v, int scale, bool sub) {
  int s = Max(scale, state->sx2_scale);
  double x = agg_sumX2_at(state, s);
  double w = (double)v;

  w *= w;
  if (scale < s)
    w *= agg_pow10(2 * (s - scale));
  agg_set_sumX2_raw(state, sub ? x - w : x + w, s);
}

/* state's sum plus x, or less x if sub; past 36 digits is an error. */
static void agg_add_sumX(dec_agg_t *state, const ty_dec128_t &x, bool sub) {
  ty_dec128_t a = agg_sumX(state), r;

  if ((sub ? ty_dec128_sub(&a, &x, &r) : ty_dec128_add(&a, &x, &r)) !=
      0)
    dec_range_error(NULL);
  agg_set_sumX(state, r);
}

/* A new state in the aggregate's memory context. */
static dec_agg_t *make_agg_state(FunctionCallInfo fcinfo, bool calc_sumX2) {
  MemoryContext agg_context;
  dec_agg_t *state;

  if (!AggCheckCallContext(fcinfo, &agg_context))
    elog(ERROR, "aggregate function called in non-aggregate context");
  state = (dec_agg_t *)MemoryContextAllocZero(agg_context, sizeof(dec_agg_t));
  state->calc_sumX2 = calc_sumX2;
  state->agg_context = agg_context;
  return state;
}

/* Count a value of scale into state, but for its sum. */
static void agg_count(dec_agg_t *state, int scale) {
  state->N++;
  if (scale > state->max_scale) {
    state->max_scale = scale;
    state->max_scale_count = 1;
  } else if (scale == state->max_scale)
    state->max_scale_count++;
}

/* state's decimal64 sum not yet in the sum. */
static i128 agg_fast_sum(const dec_agg_t *state) {
  return (i128)(((u128)(uint64)state->fast_hi << 64) | state->fast_lo);
}

/* Set state's decimal64 sum not yet in the sum to v at scale. */
static void agg_set_fast(dec_agg_t *state, i128 v, int scale) {
  state->fast = true;
  state->fast_scale = scale;
  state->fast_lo = (uint64)(u128)v;
  state->fast_hi = (int64)((u128)v >> 64);
}

/* Add state's decimal64 sum into the sum, before the sum is read. */
static void agg_flush(dec_agg_t *state) {
  if (!state->fast)
    return;
  /* The sum is ty's: exact, or an error past 36 digits. */
  agg_add_sumX(state, {agg_fast_sum(state), state->fast_scale}, false);
  state->fast = false;
}

/*
 * Count v at scale s into state: into the 128-bit sum while the scale
 * stays; the sum takes it when the scale changes, the sum would overflow,
 * or the sum is read (agg_flush()).
 */
static void agg_fast_accum(dec_agg_t *state, int64 v, int s) {
  i128 sum;

  agg_count(state, s);
  if (!state->fast || state->fast_scale != s) {
    agg_flush(state);
    agg_set_fast(state, 0, s);
  }
  if (__builtin_add_overflow(agg_fast_sum(state), (i128)v, &sum)) {
    agg_flush(state);
    sum = v;
  }
  agg_set_fast(state, sum, s);
}

/* Count x into state. */
static void agg_accum(dec_agg_t *state, const ty_dec128_t &x) {
  agg_count(state, x.scale);
  agg_add_sumX(state, x, false);
  if (state->calc_sumX2)
    agg_add_square(state, x.value, x.scale, false);
}

/*
 * Take x out of state, for a moving aggregate; false if the state can no
 * longer say its sum's scale, the last value of the largest scale gone,
 * and the aggregate must be computed again.
 */
static bool agg_discard(dec_agg_t *state, const ty_dec128_t &x) {
  agg_flush(state);
  if (x.scale == state->max_scale) {
    if (state->max_scale_count > 1 || state->max_scale == 0)
      state->max_scale_count--;
    else if (state->N == 1) {
      state->max_scale = 0;
      state->max_scale_count = 0;
    } else
      return false;
  }
  state->N--;
  agg_add_sumX(state, x, true);
  if (state->calc_sumX2)
    agg_add_square(state, x.value, x.scale, true);
  if (state->N == 0) {
    agg_set_sumX(state, {0, 0});
    agg_set_sumX2_raw(state, 0, 0);
  }
  return true;
}

/* The state argument 0, or a new one if it is null. */
static dec_agg_t *agg_state_arg(FunctionCallInfo fcinfo, bool calc_sumX2) {
  return PG_ARGISNULL(0) ? make_agg_state(fcinfo, calc_sumX2)
                         : (dec_agg_t *)PG_GETARG_POINTER(0);
}

/* Accumulate a numeric, summing squares too: variance and stddev. */
Datum numeric_accum(PG_FUNCTION_ARGS) {
  dec_agg_t *state = agg_state_arg(fcinfo, true);

  if (!PG_ARGISNULL(1))
    agg_accum(state, dec_ty(PG_GETARG_NUMERIC(1)));
  PG_RETURN_POINTER(state);
}

/* Accumulate a numeric for sum and avg. */
Datum numeric_avg_accum(PG_FUNCTION_ARGS) {
  dec_agg_t *state = agg_state_arg(fcinfo, false);

  if (!PG_ARGISNULL(1))
    agg_accum(state, dec_ty(PG_GETARG_NUMERIC(1)));
  PG_RETURN_POINTER(state);
}

/* Take a numeric out of a moving aggregate; null to compute it again. */
Datum numeric_accum_inv(PG_FUNCTION_ARGS) {
  dec_agg_t *state;

  if (PG_ARGISNULL(0))
    elog(ERROR, "numeric_accum_inv called with NULL state");
  state = (dec_agg_t *)PG_GETARG_POINTER(0);
  if (!PG_ARGISNULL(1) && !agg_discard(state, dec_ty(PG_GETARG_NUMERIC(1))))
    PG_RETURN_NULL();
  PG_RETURN_POINTER(state);
}

/* Accumulate an integer, argument 1, of the call's type. */
static Datum int_accum(FunctionCallInfo fcinfo, int64 value, bool calc_sumX2) {
  dec_agg_t *state = agg_state_arg(fcinfo, calc_sumX2);

  if (!PG_ARGISNULL(1))
    agg_accum(state, ty_dec128_t{value, 0});
  PG_RETURN_POINTER(state);
}

/* Accumulate an int2 for variance and stddev. */
Datum int2_accum(PG_FUNCTION_ARGS) {
  return int_accum(fcinfo, PG_ARGISNULL(1) ? 0 : PG_GETARG_INT16(1), true);
}

/* Accumulate an int4 for variance and stddev. */
Datum int4_accum(PG_FUNCTION_ARGS) {
  return int_accum(fcinfo, PG_ARGISNULL(1) ? 0 : PG_GETARG_INT32(1), true);
}

/* Accumulate an int8 for variance and stddev. */
Datum int8_accum(PG_FUNCTION_ARGS) {
  return int_accum(fcinfo, PG_ARGISNULL(1) ? 0 : PG_GETARG_INT64(1), true);
}

/* Accumulate an int8 for sum and avg, as a decimal64 of scale 0. */
Datum int8_avg_accum(PG_FUNCTION_ARGS) {
  dec_agg_t *state = agg_state_arg(fcinfo, false);

  if (!PG_ARGISNULL(1))
    agg_fast_accum(state, PG_GETARG_INT64(1), 0);
  PG_RETURN_POINTER(state);
}

/* Take an integer out of a moving aggregate. */
static Datum int_accum_inv(FunctionCallInfo fcinfo, int64 value) {
  dec_agg_t *state;

  if (PG_ARGISNULL(0))
    elog(ERROR, "integer accumulator inverse called with NULL state");
  state = (dec_agg_t *)PG_GETARG_POINTER(0);
  if (!PG_ARGISNULL(1))
    (void)agg_discard(state, ty_dec128_t{value, 0});
  PG_RETURN_POINTER(state);
}

/* Take an int2 out of a moving variance. */
Datum int2_accum_inv(PG_FUNCTION_ARGS) {
  return int_accum_inv(fcinfo, PG_ARGISNULL(1) ? 0 : PG_GETARG_INT16(1));
}

/* Take an int4 out of a moving variance. */
Datum int4_accum_inv(PG_FUNCTION_ARGS) {
  return int_accum_inv(fcinfo, PG_ARGISNULL(1) ? 0 : PG_GETARG_INT32(1));
}

/* Take an int8 out of a moving variance. */
Datum int8_accum_inv(PG_FUNCTION_ARGS) {
  return int_accum_inv(fcinfo, PG_ARGISNULL(1) ? 0 : PG_GETARG_INT64(1));
}

/* Take an int8 out of a moving sum or avg. */
Datum int8_avg_accum_inv(PG_FUNCTION_ARGS) {
  return int_accum_inv(fcinfo, PG_ARGISNULL(1) ? 0 : PG_GETARG_INT64(1));
}

/* Combine two states, for a partial aggregate. */
static Datum agg_combine(FunctionCallInfo fcinfo) {
  dec_agg_t *state1, *state2;
  MemoryContext agg_context, old;

  if (!AggCheckCallContext(fcinfo, &agg_context))
    elog(ERROR, "aggregate function called in non-aggregate context");
  state1 = PG_ARGISNULL(0) ? NULL : (dec_agg_t *)PG_GETARG_POINTER(0);
  state2 = PG_ARGISNULL(1) ? NULL : (dec_agg_t *)PG_GETARG_POINTER(1);
  if (state1 != NULL)
    agg_flush(state1);
  if (state2 != NULL)
    agg_flush(state2);
  if (state2 == NULL)
    PG_RETURN_POINTER(state1);
  if (state1 == NULL) {
    old = MemoryContextSwitchTo(agg_context);
    state1 = (dec_agg_t *)palloc(sizeof(dec_agg_t));
    *state1 = *state2;
    state1->agg_context = agg_context;
    MemoryContextSwitchTo(old);
    PG_RETURN_POINTER(state1);
  }
  state1->N += state2->N;
  agg_add_sumX(state1, agg_sumX(state2), false);
  if (state1->calc_sumX2) {
    int sc = Max(state1->sx2_scale, state2->sx2_scale);

    agg_set_sumX2_raw(state1,
                      agg_sumX2_at(state1, sc) + agg_sumX2_at(state2, sc), sc);
  }
  if (state2->max_scale > state1->max_scale) {
    state1->max_scale = state2->max_scale;
    state1->max_scale_count = state2->max_scale_count;
  } else if (state2->max_scale == state1->max_scale)
    state1->max_scale_count += state2->max_scale_count;
  PG_RETURN_POINTER(state1);
}

/* Combine numeric variance states. */
Datum numeric_combine(PG_FUNCTION_ARGS) { return agg_combine(fcinfo); }

/* Combine numeric sum and avg states. */
Datum numeric_avg_combine(PG_FUNCTION_ARGS) { return agg_combine(fcinfo); }

/* Combine integer variance states. */
Datum numeric_poly_combine(PG_FUNCTION_ARGS) { return agg_combine(fcinfo); }

/* Combine int8 sum and avg states. */
Datum int8_avg_combine(PG_FUNCTION_ARGS) { return agg_combine(fcinfo); }

/* A state as bytes, for a partial aggregate's worker. */
static Datum agg_serialize(FunctionCallInfo fcinfo) {
  dec_agg_t *state;
  StringInfoData buf;

  if (!AggCheckCallContext(fcinfo, NULL))
    elog(ERROR, "aggregate function called in non-aggregate context");
  state = (dec_agg_t *)PG_GETARG_POINTER(0);
  agg_flush(state);
  pq_begintypsend(&buf);
  pq_sendbyte(&buf, state->calc_sumX2 ? 1 : 0);
  pq_sendint64(&buf, state->N);
  pq_sendint32(&buf, state->max_scale);
  pq_sendint64(&buf, state->max_scale_count);
  pq_sendint64(&buf, (int64)state->sx_lo);
  pq_sendint64(&buf, state->sx_hi);
  pq_sendint32(&buf, state->sx_scale);
  pq_sendint32(&buf, state->sx2_scale);
  pq_sendbytes(&buf, state->sx2, sizeof(state->sx2));
  PG_RETURN_BYTEA_P(pq_endtypsend(&buf));
}

/* A state from its bytes. */
static Datum agg_deserialize(FunctionCallInfo fcinfo) {
  bytea *sstate;
  StringInfoData buf;
  dec_agg_t *state;

  if (!AggCheckCallContext(fcinfo, NULL))
    elog(ERROR, "aggregate function called in non-aggregate context");
  sstate = PG_GETARG_BYTEA_PP(0);
  initReadOnlyStringInfo(&buf, VARDATA_ANY(sstate), VARSIZE_ANY_EXHDR(sstate));
  state = (dec_agg_t *)palloc0(sizeof(dec_agg_t));
  state->calc_sumX2 = pq_getmsgbyte(&buf) != 0;
  state->N = pq_getmsgint64(&buf);
  state->max_scale = pq_getmsgint(&buf, 4);
  state->max_scale_count = pq_getmsgint64(&buf);
  state->sx_lo = (uint64)pq_getmsgint64(&buf);
  state->sx_hi = pq_getmsgint64(&buf);
  state->sx_scale = pq_getmsgint(&buf, 4);
  state->sx2_scale = pq_getmsgint(&buf, 4);
  memcpy(state->sx2, pq_getmsgbytes(&buf, sizeof(state->sx2)),
         sizeof(state->sx2));
  pq_getmsgend(&buf);
  PG_RETURN_POINTER(state);
}

/* Serialize a numeric variance state. */
Datum numeric_serialize(PG_FUNCTION_ARGS) { return agg_serialize(fcinfo); }

/* Deserialize a numeric variance state. */
Datum numeric_deserialize(PG_FUNCTION_ARGS) { return agg_deserialize(fcinfo); }

/* Serialize a numeric sum and avg state. */
Datum numeric_avg_serialize(PG_FUNCTION_ARGS) { return agg_serialize(fcinfo); }

/* Deserialize a numeric sum and avg state. */
Datum numeric_avg_deserialize(PG_FUNCTION_ARGS) {
  return agg_deserialize(fcinfo);
}

/* Serialize an integer variance state. */
Datum numeric_poly_serialize(PG_FUNCTION_ARGS) { return agg_serialize(fcinfo); }

/* Deserialize an integer variance state. */
Datum numeric_poly_deserialize(PG_FUNCTION_ARGS) {
  return agg_deserialize(fcinfo);
}

/* Serialize an int8 sum and avg state. */
Datum int8_avg_serialize(PG_FUNCTION_ARGS) { return agg_serialize(fcinfo); }

/* Deserialize an int8 sum and avg state. */
Datum int8_avg_deserialize(PG_FUNCTION_ARGS) { return agg_deserialize(fcinfo); }

/* The state of a final function, argument 0, or NULL. */
static dec_agg_t *final_state(FunctionCallInfo fcinfo) {
  dec_agg_t *state =
      PG_ARGISNULL(0) ? NULL : (dec_agg_t *)PG_GETARG_POINTER(0);

  if (state != NULL)
    agg_flush(state);
  return state;
}

/* sum: the sum, or null of no values; past 36 digits is an error. */
static Datum agg_sum(FunctionCallInfo fcinfo) {
  dec_agg_t *state = final_state(fcinfo);

  if (state == NULL || state->N == 0)
    PG_RETURN_NULL();
  ty_dec128_t sx = agg_sumX(state);

  PG_RETURN_NUMERIC(dec_make(sx.value, sx.scale));
}

/* avg: the sum over the count, at division's scale, or null. */
static Datum agg_avg(FunctionCallInfo fcinfo) {
  dec_agg_t *state = final_state(fcinfo);
  ty_dec128_t sx, n, r;

  if (state == NULL || state->N == 0)
    PG_RETURN_NULL();
  sx = agg_sumX(state);
  n = {state->N, 0};
  /* The quotient is ty's, at PostgreSQL's scale for it. */
  if (ty_dec128_div(&sx, &n,
                        select_div_scale128(sx.value, sx.scale, n.value, 0),
                        false, &r) != 0)
    dec_range_error(NULL);
  PG_RETURN_NUMERIC(dec_make(r.value, r.scale));
}

/*
 * The variance, or with !variance the standard deviation, of the sample if
 * sample, else of the population:
 * (N * sumX2 - sumX^2) / (N * (N - 1)), or over N * N, in double, as
 * DuckDB's are, so that a sum of squares does not pass 36 digits. A result that is no more
 * than the noise of that difference is zero. Null with too few values.
 */
static Datum agg_stddev(FunctionCallInfo fcinfo, bool variance, bool sample) {
  dec_agg_t *state = final_state(fcinfo);
  ty_dec128_t sx, var, r;
  double n, sum, n_sumX2, num;
  i128 den;
  int rscale;

  if (state == NULL || state->N == 0 || (sample && state->N <= 1))
    PG_RETURN_NULL();
  sx = agg_sumX(state);
  n = (double)state->N;
  sum = ty_dec128_to_double(&sx);
  n_sumX2 = n * agg_sumX2(state);
  num = n_sumX2 - sum * sum;
  if (!std::isfinite(n_sumX2) || !std::isfinite(num))
    dec_range_error(NULL);
  if (!(num > n_sumX2 * 1e-14))
    PG_RETURN_NUMERIC(dec_make(0, 0));
  den = (i128)state->N * (sample ? state->N - 1 : state->N);
  /* num is at twice the sum's scale. */
  rscale = select_div_scale_nb(dec_nbase_d(num), 2 * sx.scale,
                               dec_nbase128(den, 0), 0);
  if (ty_dec128_from_double(num / (double)den, rscale, &var) != 0)
    dec_range_error(NULL);
  if (!variance) {
    /* The root of the rounded variance, rounded at the same scale. */
    if (ty_dec128_sqrt(&var, rscale, &r) != 0)
      dec_range_error(NULL);
    var = r;
  }
  PG_RETURN_NUMERIC(dec_make(var.value, var.scale));
}

/* avg(numeric). */
Datum numeric_avg(PG_FUNCTION_ARGS) { return agg_avg(fcinfo); }

/* sum(numeric). */
Datum numeric_sum(PG_FUNCTION_ARGS) { return agg_sum(fcinfo); }

/* var_samp of numeric or int8. */
Datum numeric_var_samp(PG_FUNCTION_ARGS) {
  return agg_stddev(fcinfo, true, true);
}

/* stddev_samp of numeric or int8. */
Datum numeric_stddev_samp(PG_FUNCTION_ARGS) {
  return agg_stddev(fcinfo, false, true);
}

/* var_pop of numeric or int8. */
Datum numeric_var_pop(PG_FUNCTION_ARGS) {
  return agg_stddev(fcinfo, true, false);
}

/* stddev_pop of numeric or int8. */
Datum numeric_stddev_pop(PG_FUNCTION_ARGS) {
  return agg_stddev(fcinfo, false, false);
}

/* sum(int8). */
Datum numeric_poly_sum(PG_FUNCTION_ARGS) { return agg_sum(fcinfo); }

/* avg(int8). */
Datum numeric_poly_avg(PG_FUNCTION_ARGS) { return agg_avg(fcinfo); }

/* var_samp of int2 or int4. */
Datum numeric_poly_var_samp(PG_FUNCTION_ARGS) {
  return agg_stddev(fcinfo, true, true);
}

/* stddev_samp of int2 or int4. */
Datum numeric_poly_stddev_samp(PG_FUNCTION_ARGS) {
  return agg_stddev(fcinfo, false, true);
}

/* var_pop of int2 or int4. */
Datum numeric_poly_var_pop(PG_FUNCTION_ARGS) {
  return agg_stddev(fcinfo, true, false);
}

/* stddev_pop of int2 or int4. */
Datum numeric_poly_stddev_pop(PG_FUNCTION_ARGS) {
  return agg_stddev(fcinfo, false, false);
}

/* ----------------------------------------------------------------------
 * Sums and averages of int2 and int4, in int8: PostgreSQL's own.
 * ----------------------------------------------------------------------
 */

/*
 * SUM transition functions for integer datatypes.
 *
 * To avoid overflow, we use accumulators wider than the input datatype.
 * A Numeric accumulator is needed for int8 input; for int4 and int2
 * inputs, we use int8 accumulators which should be sufficient for practical
 * purposes.  (The latter two therefore don't really belong in this file,
 * but we keep them here anyway.)
 *
 * Because SQL defines the SUM() of no values to be NULL, not zero,
 * the initial condition of the transition data value needs to be NULL. This
 * means we can't rely on ExecAgg to automatically insert the first non-null
 * data value into the transition data: it doesn't know how to do the type
 * conversion.  The upshot is that these routines have to be marked non-strict
 * and handle substitution of the first non-null input themselves.
 *
 * Note: these functions are used only in plain aggregation mode.
 * In moving-aggregate mode, we use intX_avg_accum and intX_avg_accum_inv.
 */

Datum int2_sum(PG_FUNCTION_ARGS) {
  int64 newval;

  if (PG_ARGISNULL(0)) {
    /* No non-null input seen so far... */
    if (PG_ARGISNULL(1))
      PG_RETURN_NULL(); /* still no non-null */
    /* This is the first non-null input. */
    newval = (int64)PG_GETARG_INT16(1);
    PG_RETURN_INT64(newval);
  }

  /*
   * If we're invoked as an aggregate, we can cheat and modify our first
   * parameter in-place to avoid palloc overhead. If not, we need to return
   * the new value of the transition variable. (If int8 is pass-by-value,
   * then of course this is useless as well as incorrect, so just ifdef it
   * out.)
   */
#ifndef USE_FLOAT8_BYVAL /* controls int8 too */
  if (AggCheckCallContext(fcinfo, NULL)) {
    int64 *oldsum = (int64 *)PG_GETARG_POINTER(0);

    /* Leave the running sum unchanged in the new input is null */
    if (!PG_ARGISNULL(1))
      *oldsum = ty_int_sum_add(*oldsum, PG_GETARG_INT16(1));

    PG_RETURN_POINTER(oldsum);
  } else
#endif
  {
    int64 oldsum = PG_GETARG_INT64(0);

    /* Leave sum unchanged if new input is null. */
    if (PG_ARGISNULL(1))
      PG_RETURN_INT64(oldsum);

    /* OK to do the addition. */
    newval = ty_int_sum_add(oldsum, PG_GETARG_INT16(1));

    PG_RETURN_INT64(newval);
  }
}

/* sum(int4): an int8 sum, null of no values. */
Datum int4_sum(PG_FUNCTION_ARGS) {
  int64 newval;

  if (PG_ARGISNULL(0)) {
    /* No non-null input seen so far... */
    if (PG_ARGISNULL(1))
      PG_RETURN_NULL(); /* still no non-null */
    /* This is the first non-null input. */
    newval = (int64)PG_GETARG_INT32(1);
    PG_RETURN_INT64(newval);
  }

  /*
   * If we're invoked as an aggregate, we can cheat and modify our first
   * parameter in-place to avoid palloc overhead. If not, we need to return
   * the new value of the transition variable. (If int8 is pass-by-value,
   * then of course this is useless as well as incorrect, so just ifdef it
   * out.)
   */
#ifndef USE_FLOAT8_BYVAL /* controls int8 too */
  if (AggCheckCallContext(fcinfo, NULL)) {
    int64 *oldsum = (int64 *)PG_GETARG_POINTER(0);

    /* Leave the running sum unchanged in the new input is null */
    if (!PG_ARGISNULL(1))
      *oldsum = ty_int_sum_add(*oldsum, PG_GETARG_INT32(1));

    PG_RETURN_POINTER(oldsum);
  } else
#endif
  {
    int64 oldsum = PG_GETARG_INT64(0);

    /* Leave sum unchanged if new input is null. */
    if (PG_ARGISNULL(1))
      PG_RETURN_INT64(oldsum);

    /* OK to do the addition. */
    newval = ty_int_sum_add(oldsum, PG_GETARG_INT32(1));

    PG_RETURN_INT64(newval);
  }
}

/*
 * Note: this function is obsolete, it's no longer used for SUM(int8).
 */
Datum int8_sum(PG_FUNCTION_ARGS) {
  Numeric oldsum;

  if (PG_ARGISNULL(0)) {
    /* No non-null input seen so far... */
    if (PG_ARGISNULL(1))
      PG_RETURN_NULL(); /* still no non-null */
    /* This is the first non-null input. */
    PG_RETURN_NUMERIC(int64_to_numeric(PG_GETARG_INT64(1)));
  }

  /*
   * Note that we cannot special-case the aggregate case here, as we do for
   * int2_sum and int4_sum: numeric is of variable size, so we cannot modify
   * our first parameter in-place.
   */

  oldsum = PG_GETARG_NUMERIC(0);

  /* Leave sum unchanged if new input is null. */
  if (PG_ARGISNULL(1))
    PG_RETURN_NUMERIC(oldsum);

  /* OK to do the addition. */
  PG_RETURN_DATUM(DirectFunctionCall2(
      numeric_add, NumericGetDatum(oldsum),
      NumericGetDatum(int64_to_numeric(PG_GETARG_INT64(1)))));
}

/*
 * Routines for avg(int2) and avg(int4).  The transition datatype
 * is a two-element int8 array, holding count and sum.
 *
 * These functions are also used for sum(int2) and sum(int4) when
 * operating in moving-aggregate mode, since for correct inverse transitions
 * we need to count the inputs.
 */

/* the state is ty's: the count of the values, and their sum in an int64 */
typedef ty_int_avg_t Int8TransTypeData;

/* Accumulate an int2 into an avg state: an int8 array of count and sum. */
Datum int2_avg_accum(PG_FUNCTION_ARGS) {
  ArrayType *transarray;
  int16 newval = PG_GETARG_INT16(1);
  Int8TransTypeData *transdata;

  /*
   * If we're invoked as an aggregate, we can cheat and modify our first
   * parameter in-place to reduce palloc overhead. Otherwise we need to make
   * a copy of it before scribbling on it.
   */
  if (AggCheckCallContext(fcinfo, NULL))
    transarray = PG_GETARG_ARRAYTYPE_P(0);
  else
    transarray = PG_GETARG_ARRAYTYPE_P_COPY(0);

  if (ARR_HASNULL(transarray) ||
      ARR_SIZE(transarray) !=
          ARR_OVERHEAD_NONULLS(1) + sizeof(Int8TransTypeData))
    elog(ERROR, "expected 2-element int8 array");

  transdata = (Int8TransTypeData *)ARR_DATA_PTR(transarray);
  ty_int_avg_accum(transdata, newval);

  PG_RETURN_ARRAYTYPE_P(transarray);
}

/* Accumulate an int4 into an avg state: an int8 array of count and sum. */
Datum int4_avg_accum(PG_FUNCTION_ARGS) {
  ArrayType *transarray;
  int32 newval = PG_GETARG_INT32(1);
  Int8TransTypeData *transdata;

  /*
   * If we're invoked as an aggregate, we can cheat and modify our first
   * parameter in-place to reduce palloc overhead. Otherwise we need to make
   * a copy of it before scribbling on it.
   */
  if (AggCheckCallContext(fcinfo, NULL))
    transarray = PG_GETARG_ARRAYTYPE_P(0);
  else
    transarray = PG_GETARG_ARRAYTYPE_P_COPY(0);

  if (ARR_HASNULL(transarray) ||
      ARR_SIZE(transarray) !=
          ARR_OVERHEAD_NONULLS(1) + sizeof(Int8TransTypeData))
    elog(ERROR, "expected 2-element int8 array");

  transdata = (Int8TransTypeData *)ARR_DATA_PTR(transarray);
  ty_int_avg_accum(transdata, newval);

  PG_RETURN_ARRAYTYPE_P(transarray);
}

/* Combine two int2 or int4 avg states. */
Datum int4_avg_combine(PG_FUNCTION_ARGS) {
  ArrayType *transarray1;
  ArrayType *transarray2;
  Int8TransTypeData *state1;
  Int8TransTypeData *state2;

  if (!AggCheckCallContext(fcinfo, NULL))
    elog(ERROR, "aggregate function called in non-aggregate context");

  transarray1 = PG_GETARG_ARRAYTYPE_P(0);
  transarray2 = PG_GETARG_ARRAYTYPE_P(1);

  if (ARR_HASNULL(transarray1) ||
      ARR_SIZE(transarray1) !=
          ARR_OVERHEAD_NONULLS(1) + sizeof(Int8TransTypeData))
    elog(ERROR, "expected 2-element int8 array");

  if (ARR_HASNULL(transarray2) ||
      ARR_SIZE(transarray2) !=
          ARR_OVERHEAD_NONULLS(1) + sizeof(Int8TransTypeData))
    elog(ERROR, "expected 2-element int8 array");

  state1 = (Int8TransTypeData *)ARR_DATA_PTR(transarray1);
  state2 = (Int8TransTypeData *)ARR_DATA_PTR(transarray2);

  ty_int_avg_combine(state1, state2);

  PG_RETURN_ARRAYTYPE_P(transarray1);
}

/* Take an int2 out of a moving avg state. */
Datum int2_avg_accum_inv(PG_FUNCTION_ARGS) {
  ArrayType *transarray;
  int16 newval = PG_GETARG_INT16(1);
  Int8TransTypeData *transdata;

  /*
   * If we're invoked as an aggregate, we can cheat and modify our first
   * parameter in-place to reduce palloc overhead. Otherwise we need to make
   * a copy of it before scribbling on it.
   */
  if (AggCheckCallContext(fcinfo, NULL))
    transarray = PG_GETARG_ARRAYTYPE_P(0);
  else
    transarray = PG_GETARG_ARRAYTYPE_P_COPY(0);

  if (ARR_HASNULL(transarray) ||
      ARR_SIZE(transarray) !=
          ARR_OVERHEAD_NONULLS(1) + sizeof(Int8TransTypeData))
    elog(ERROR, "expected 2-element int8 array");

  transdata = (Int8TransTypeData *)ARR_DATA_PTR(transarray);
  ty_int_avg_accum_inv(transdata, newval);

  PG_RETURN_ARRAYTYPE_P(transarray);
}

/* Take an int4 out of a moving avg state. */
Datum int4_avg_accum_inv(PG_FUNCTION_ARGS) {
  ArrayType *transarray;
  int32 newval = PG_GETARG_INT32(1);
  Int8TransTypeData *transdata;

  /*
   * If we're invoked as an aggregate, we can cheat and modify our first
   * parameter in-place to reduce palloc overhead. Otherwise we need to make
   * a copy of it before scribbling on it.
   */
  if (AggCheckCallContext(fcinfo, NULL))
    transarray = PG_GETARG_ARRAYTYPE_P(0);
  else
    transarray = PG_GETARG_ARRAYTYPE_P_COPY(0);

  if (ARR_HASNULL(transarray) ||
      ARR_SIZE(transarray) !=
          ARR_OVERHEAD_NONULLS(1) + sizeof(Int8TransTypeData))
    elog(ERROR, "expected 2-element int8 array");

  transdata = (Int8TransTypeData *)ARR_DATA_PTR(transarray);
  ty_int_avg_accum_inv(transdata, newval);

  PG_RETURN_ARRAYTYPE_P(transarray);
}

/* avg of int2 or int4: the sum over the count, as numeric, or null. */
Datum int8_avg(PG_FUNCTION_ARGS) {
  ArrayType *transarray = PG_GETARG_ARRAYTYPE_P(0);
  Int8TransTypeData *transdata;
  Datum countd, sumd;

  if (ARR_HASNULL(transarray) ||
      ARR_SIZE(transarray) !=
          ARR_OVERHEAD_NONULLS(1) + sizeof(Int8TransTypeData))
    elog(ERROR, "expected 2-element int8 array");
  transdata = (Int8TransTypeData *)ARR_DATA_PTR(transarray);

  /* SQL defines AVG of no values to be NULL */
  if (ty_int_avg_empty(transdata))
    PG_RETURN_NULL();

  countd = NumericGetDatum(int64_to_numeric(transdata->count));
  sumd = NumericGetDatum(int64_to_numeric(transdata->sum));

  PG_RETURN_DATUM(DirectFunctionCall2(numeric_div, sumd, countd));
}

/*
 * SUM(int2) and SUM(int4) both return int8, so we can use this
 * final function for both.
 */
Datum int2int4_sum(PG_FUNCTION_ARGS) {
  ArrayType *transarray = PG_GETARG_ARRAYTYPE_P(0);
  Int8TransTypeData *transdata;

  if (ARR_HASNULL(transarray) ||
      ARR_SIZE(transarray) !=
          ARR_OVERHEAD_NONULLS(1) + sizeof(Int8TransTypeData))
    elog(ERROR, "expected 2-element int8 array");
  transdata = (Int8TransTypeData *)ARR_DATA_PTR(transarray);

  /* SQL defines SUM of no values to be NULL */
  if (ty_int_avg_empty(transdata))
    PG_RETURN_NULL();

  PG_RETURN_DATUM(Int64GetDatumFast(transdata->sum));
}

/* ----------------------------------------------------------------------
 * decimal64: sm's decimal64 by value, the word (value << 5) | scale, of at
 * most 17 digits (DECIMAL.md section 2.8).  It compares, hashes and sorts on
 * its own; what else is done to it is done to it as a decimal128.
 * ----------------------------------------------------------------------
 */

#define D64_SCALE_BITS 5
#define D64_MAX_DIGITS 17

/* The value of the decimal64 d, the number times 10^scale. */
static int64 d64_value(Datum d) { return DatumGetInt64(d) >> D64_SCALE_BITS; }

/* The scale of the decimal64 d. */
static int d64_scale(Datum d) {
  return (int)(DatumGetInt64(d) & ((1 << D64_SCALE_BITS) - 1));
}

/* The decimal64 of v at scale, |v| below 10^17. */
static Datum d64_make(int64 v, int scale) {
  return Int64GetDatum((int64)((uint64)v << D64_SCALE_BITS) | scale);
}

/* The decimal128 of the decimal64 d, palloc'd. */
static Numeric d64_numeric(Datum d) {
  return dec_make(d64_value(d), d64_scale(d));
}

/*
 * a as a decimal64 in *out: places past the point rounded away, half away
 * from zero, until it has at most 17 digits and places (ty fits it); false,
 * with the error saved in escontext, if its integer part has more than 17
 * digits.
 */
static bool d64_of_dec(const ty_dec128_t &a, Datum *out, Node *escontext) {
  ty_dec64_t r;

  if (ty_dec64_from_dec128(&a, &r) != 0)
    ereturn(escontext, false,
            (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
             errmsg("value overflows numeric format"),
             errdetail("A numeric of precision %d or less has at most %d "
                       "digits.",
                       D64_MAX_DIGITS, D64_MAX_DIGITS)));
  *out = d64_make(r.value, r.scale);
  return true;
}

/* a as a decimal64; more than 17 digits before the point is an error. */
static Datum d64_from_dec(const ty_dec128_t &a) {
  Datum d = 0;

  d64_of_dec(a, &d, NULL);
  return d;
}

/*
 * num as a decimal64 in *out, exactly: false if it has more than 17 digits
 * or places.  What a literal is typed by (parse_node.c).
 */
bool numeric_to_decimal64(Numeric num, Datum *out) {
  ty_dec128_t a = dec_ty(num);

  if (a.scale > D64_MAX_DIGITS ||
      u128_digits(a.value < 0 ? -(u128)a.value : (u128)a.value) >
          D64_MAX_DIGITS)
    return false;
  *out = d64_make((int64)a.value, a.scale);
  return true;
}

/* Input: numeric's text, under typmod, as a decimal64. */
Datum decimal64_in(PG_FUNCTION_ARGS) {
  Datum num, res;

  if (!DirectInputFunctionCallSafe(numeric_in, PG_GETARG_CSTRING(0),
                                   InvalidOid, PG_GETARG_INT32(2),
                                   fcinfo->context, &num) ||
      !d64_of_dec(dec_ty(DatumGetNumeric(num)), &res, fcinfo->context))
    PG_RETURN_NULL();
  return res;
}

/* Output: every place of its scale, as numeric's. */
Datum decimal64_out(PG_FUNCTION_ARGS) {
  Datum d = PG_GETARG_DATUM(0);

  PG_RETURN_CSTRING(dec_text(d64_value(d), d64_scale(d)));
}

/* Binary input: numeric's format, as a decimal64. */
Datum decimal64_recv(PG_FUNCTION_ARGS) {
  Datum num = DirectFunctionCall3(numeric_recv, PG_GETARG_DATUM(0),
                                  PG_GETARG_DATUM(1), PG_GETARG_DATUM(2));

  return d64_from_dec(dec_ty(DatumGetNumeric(num)));
}

/* Binary output: numeric's format. */
Datum decimal64_send(PG_FUNCTION_ARGS) {
  return DirectFunctionCall1(numeric_send,
                             NumericGetDatum(d64_numeric(PG_GETARG_DATUM(0))));
}

/* The typmod of decimal64(p) or decimal64(p, s): p at most 17. */
Datum decimal64typmodin(PG_FUNCTION_ARGS) {
  int32 typmod = DatumGetInt32(numerictypmodin(fcinfo));

  if (numeric_typmod_precision(typmod) > D64_MAX_DIGITS)
    ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                    errmsg("NUMERIC precision %d must be between 1 and %d",
                           numeric_typmod_precision(typmod), D64_MAX_DIGITS)));
  PG_RETURN_INT32(typmod);
}

/* The length coercion: d rounded to typmod's scale, checked against it. */
Datum decimal64(PG_FUNCTION_ARGS) {
  Datum d = PG_GETARG_DATUM(0);
  ty_dec128_t a = {d64_value(d), d64_scale(d)};

  (void)dec_apply_typmod(&a, PG_GETARG_INT32(1), NULL);
  return d64_from_dec(a);
}

/* decimal64 to decimal128. */
Datum decimal64_decimal128(PG_FUNCTION_ARGS) {
  PG_RETURN_NUMERIC(d64_numeric(PG_GETARG_DATUM(0)));
}

/* decimal128 to decimal64: places rounded away to fit 17 digits. */
Datum decimal128_decimal64(PG_FUNCTION_ARGS) {
  return d64_from_dec(dec_ty(PG_GETARG_NUMERIC(0)));
}

/* int2 to decimal64. */
Datum int2_decimal64(PG_FUNCTION_ARGS) {
  return d64_make(PG_GETARG_INT16(0), 0);
}

/* int4 to decimal64. */
Datum int4_decimal64(PG_FUNCTION_ARGS) {
  return d64_make(PG_GETARG_INT32(0), 0);
}

/* int8 to decimal64: past 17 digits is an error. */
Datum int8_decimal64(PG_FUNCTION_ARGS) {
  return d64_from_dec(ty_dec128_t{PG_GETARG_INT64(0), 0});
}

/* float4 to decimal64, as to numeric. */
Datum float4_decimal64(PG_FUNCTION_ARGS) {
  Datum num = DirectFunctionCall1(float4_numeric, PG_GETARG_DATUM(0));

  return d64_from_dec(dec_ty(DatumGetNumeric(num)));
}

/* float8 to decimal64, as to numeric. */
Datum float8_decimal64(PG_FUNCTION_ARGS) {
  Datum num = DirectFunctionCall1(float8_numeric, PG_GETARG_DATUM(0));

  return d64_from_dec(dec_ty(DatumGetNumeric(num)));
}

/* decimal64 to int2, as numeric's. */
Datum decimal64_int2(PG_FUNCTION_ARGS) {
  return DirectFunctionCall1(numeric_int2,
                             NumericGetDatum(d64_numeric(PG_GETARG_DATUM(0))));
}

/* decimal64 to int4, as numeric's. */
Datum decimal64_int4(PG_FUNCTION_ARGS) {
  return DirectFunctionCall1(numeric_int4,
                             NumericGetDatum(d64_numeric(PG_GETARG_DATUM(0))));
}

/* decimal64 to int8, as numeric's. */
Datum decimal64_int8(PG_FUNCTION_ARGS) {
  return DirectFunctionCall1(numeric_int8,
                             NumericGetDatum(d64_numeric(PG_GETARG_DATUM(0))));
}

/* decimal64 to float4, the nearest to its text. */
Datum decimal64_float4(PG_FUNCTION_ARGS) {
  Datum d = PG_GETARG_DATUM(0);
  ty_dec128_t a = {d64_value(d), d64_scale(d)};

  PG_RETURN_FLOAT4(ty_dec128_to_float32(&a));
}

/* decimal64 to float8, the nearest to its text. */
Datum decimal64_float8(PG_FUNCTION_ARGS) {
  Datum d = PG_GETARG_DATUM(0);
  ty_dec128_t a = {d64_value(d), d64_scale(d)};

  PG_RETURN_FLOAT8(ty_dec128_to_float64(&a));
}

/*
 * -1, 0 or 1 as a is below, equal to or above b. At one scale the order of
 * the words is the order of the values, as the scale is in their low bits:
 * that is digres's packing, so it stays here, and a sort pays no unpacking
 * (ty_dec64_cmp in line cost a sort 3.8% more instructions). At two
 * scales ty puts both to one scale in 128 bits.
 */
static int d64_cmp(Datum a, Datum b) {
  int sa = d64_scale(a), sb = d64_scale(b);

  if (sa == sb) {
    int64 wa = DatumGetInt64(a), wb = DatumGetInt64(b);

    return wa < wb ? -1 : (wa > wb ? 1 : 0);
  }
  return ty_dec64_cmp_scaled({d64_value(a), sa}, {d64_value(b), sb});
}

/* The comparator of a sort of decimal64s. */
static int decimal64_fast_cmp(Datum x, Datum y, SortSupport ssup) {
  return d64_cmp(x, y);
}

/* Sort support: a comparator of words. */
Datum decimal64_sortsupport(PG_FUNCTION_ARGS) {
  SortSupport ssup = (SortSupport)PG_GETARG_POINTER(0);

  ssup->comparator = decimal64_fast_cmp;
  PG_RETURN_VOID();
}

/* btree comparison: -1, 0 or 1. */
Datum decimal64_cmp(PG_FUNCTION_ARGS) {
  PG_RETURN_INT32(d64_cmp(PG_GETARG_DATUM(0), PG_GETARG_DATUM(1)));
}

/* a = b, at any scales. */
Datum decimal64_eq(PG_FUNCTION_ARGS) {
  PG_RETURN_BOOL(d64_cmp(PG_GETARG_DATUM(0), PG_GETARG_DATUM(1)) == 0);
}

/* a <> b. */
Datum decimal64_ne(PG_FUNCTION_ARGS) {
  PG_RETURN_BOOL(d64_cmp(PG_GETARG_DATUM(0), PG_GETARG_DATUM(1)) != 0);
}

/* a < b. */
Datum decimal64_lt(PG_FUNCTION_ARGS) {
  PG_RETURN_BOOL(d64_cmp(PG_GETARG_DATUM(0), PG_GETARG_DATUM(1)) < 0);
}

/* a <= b. */
Datum decimal64_le(PG_FUNCTION_ARGS) {
  PG_RETURN_BOOL(d64_cmp(PG_GETARG_DATUM(0), PG_GETARG_DATUM(1)) <= 0);
}

/* a > b. */
Datum decimal64_gt(PG_FUNCTION_ARGS) {
  PG_RETURN_BOOL(d64_cmp(PG_GETARG_DATUM(0), PG_GETARG_DATUM(1)) > 0);
}

/* a >= b. */
Datum decimal64_ge(PG_FUNCTION_ARGS) {
  PG_RETURN_BOOL(d64_cmp(PG_GETARG_DATUM(0), PG_GETARG_DATUM(1)) >= 0);
}

/* hash_decimal64: ty's hash, as hash_numeric of the same value. */
Datum hash_decimal64(PG_FUNCTION_ARGS) {
  Datum d = PG_GETARG_DATUM(0);
  ty_dec64_t a = {d64_value(d), d64_scale(d)};

  PG_RETURN_UINT32((uint32)ty_dec64_hash(&a, 0));
}

/* hash_decimal64_extended: as hash_numeric_extended of the same value. */
Datum hash_decimal64_extended(PG_FUNCTION_ARGS) {
  Datum d = PG_GETARG_DATUM(0);
  ty_dec64_t a = {d64_value(d), d64_scale(d)};

  PG_RETURN_UINT64(ty_dec64_hash(&a, (uint64)PG_GETARG_INT64(1)));
}

/* The larger of two decimal64s. */
Datum decimal64_larger(PG_FUNCTION_ARGS) {
  Datum a = PG_GETARG_DATUM(0), b = PG_GETARG_DATUM(1);

  return d64_cmp(a, b) >= 0 ? a : b;
}

/* The smaller of two decimal64s. */
Datum decimal64_smaller(PG_FUNCTION_ARGS) {
  Datum a = PG_GETARG_DATUM(0), b = PG_GETARG_DATUM(1);

  return d64_cmp(a, b) <= 0 ? a : b;
}

/* jsonb to decimal64: its number, as to numeric, fitted to 17 digits. */
Datum jsonb_decimal64(PG_FUNCTION_ARGS) {
  Datum num = DirectFunctionCall1(jsonb_numeric, PG_GETARG_DATUM(0));

  return d64_from_dec(dec_ty(DatumGetNumeric(num)));
}

/* decimal64 to money, as numeric's. */
Datum decimal64_cash(PG_FUNCTION_ARGS) {
  return DirectFunctionCall1(numeric_cash,
                             NumericGetDatum(d64_numeric(PG_GETARG_DATUM(0))));
}

/* Raise PostgreSQL's ERROR for a rounded value that has too many digits. */
static void d64_format_overflow(void) {
  ereport(ERROR,
          (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
           errmsg("value overflows numeric format"),
           errdetail("A numeric of precision %d or less has at most %d "
                     "digits.",
                     D64_MAX_DIGITS, D64_MAX_DIGITS)));
}

/* The ERROR for the error that the last call of ty set, in ty_errnum(). */
static void ty_error_raise(void) {
  if (ty_errnum() == TY_ERR_DIVZERO)
    division_by_zero(NULL);
  ereport(ERROR, (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
                  errmsg("numeric field overflow")));
}

/*
 * a + b, a decimal64, by value: past 17 digits it is an error, and a
 * caller who wants more adds a decimal128 to a decimal64.
 */
Datum decimal64_add(PG_FUNCTION_ARGS) {
  Datum da = PG_GETARG_DATUM(0), db = PG_GETARG_DATUM(1);
  ty_dec64_t a = {d64_value(da), d64_scale(da)};
  ty_dec64_t b = {d64_value(db), d64_scale(db)};
  ty_dec64_t r;

  /* The addition is ty's: it takes the two parts of each decimal64. */
  if (ty_dec64_add(&a, &b, &r) != 0)
    ty_error_raise();
  return d64_make(r.value, r.scale);
}

/* a - b, a decimal64: past 17 digits it is an error, as a + b's is. */
Datum decimal64_sub(PG_FUNCTION_ARGS) {
  Datum da = PG_GETARG_DATUM(0), db = PG_GETARG_DATUM(1);
  ty_dec64_t a = {d64_value(da), d64_scale(da)};
  ty_dec64_t b = {d64_value(db), d64_scale(db)};
  ty_dec64_t r;

  /* The subtraction is ty's: it takes the two parts of each decimal64. */
  if (ty_dec64_sub(&a, &b, &r) != 0)
    ty_error_raise();
  return d64_make(r.value, r.scale);
}

/*
 * a * b at the sum of their scales, a decimal64: exact, or an error past 17
 * digits or 17 places, as a + b is.
 */
Datum decimal64_mul(PG_FUNCTION_ARGS) {
  Datum da = PG_GETARG_DATUM(0), db = PG_GETARG_DATUM(1);
  ty_dec64_t a = {d64_value(da), d64_scale(da)};
  ty_dec64_t b = {d64_value(db), d64_scale(db)};
  ty_dec64_t r;

  /* The multiplication is ty's: it takes the two parts of each decimal64. */
  if (ty_dec64_mul(&a, &b, &r) != 0)
    ty_error_raise();
  return d64_make(r.value, r.scale);
}

/*
 * a / b at numeric's scale, a decimal64: a quotient is not exact, so its
 * places past 17 digits are rounded away, as a cast to decimal64 does; it
 * is an error only if the integer part has more than 17 digits.
 */
Datum decimal64_div(PG_FUNCTION_ARGS) {
  Datum da = PG_GETARG_DATUM(0), db = PG_GETARG_DATUM(1);
  ty_dec64_t a = {d64_value(da), d64_scale(da)};
  ty_dec64_t b = {d64_value(db), d64_scale(db)};
  ty_dec64_t r;

  if (b.value == 0)
    division_by_zero(NULL);
  /* ty divides in 128 bits at PostgreSQL's scale, and fits 17 digits. */
  if (ty_dec64_div(&a, &b,
                       select_div_scale128(a.value, a.scale, b.value, b.scale),
                       &r) != 0)
    d64_format_overflow();
  return d64_make(r.value, r.scale);
}

/* -a. */
Datum decimal64_uminus(PG_FUNCTION_ARGS) {
  Datum da = PG_GETARG_DATUM(0);
  ty_dec64_t r = ty_dec64_neg({d64_value(da), d64_scale(da)});

  return d64_make(r.value, r.scale);
}

/* |a|. */
Datum decimal64_abs(PG_FUNCTION_ARGS) {
  Datum da = PG_GETARG_DATUM(0);
  ty_dec64_t r = ty_dec64_abs({d64_value(da), d64_scale(da)});

  return d64_make(r.value, r.scale);
}

/* Accumulate a decimal64 for sum and avg (agg_fast_accum()). */
Datum decimal64_avg_accum(PG_FUNCTION_ARGS) {
  dec_agg_t *state = agg_state_arg(fcinfo, false);

  if (!PG_ARGISNULL(1))
    agg_fast_accum(state, d64_value(PG_GETARG_DATUM(1)),
                   d64_scale(PG_GETARG_DATUM(1)));
  PG_RETURN_POINTER(state);
}

/* Take a decimal64 out of a moving aggregate; null to compute it again. */
Datum decimal64_accum_inv(PG_FUNCTION_ARGS) {
  dec_agg_t *state;

  if (PG_ARGISNULL(0))
    elog(ERROR, "decimal64_accum_inv called with NULL state");
  state = (dec_agg_t *)PG_GETARG_POINTER(0);
  if (!PG_ARGISNULL(1) && !agg_discard(state, ty_dec128_t{d64_value(PG_GETARG_DATUM(1)), d64_scale(PG_GETARG_DATUM(1))}))
    PG_RETURN_NULL();
  PG_RETURN_POINTER(state);
}

/*
 * d rounded, or truncated if trunc, to n places: ty's, exact or an error
 * past 17 digits or places, as PostgreSQL's message says it.
 */
static Datum d64_round_or_trunc(Datum d, int n, bool trunc) {
  ty_dec64_t a = {d64_value(d), d64_scale(d)};
  ty_dec64_t r;

  if ((trunc ? ty_dec64_trunc(&a, n, &r) : ty_dec64_round(&a, n, &r)) !=
      0)
    d64_format_overflow();
  return d64_make(r.value, r.scale);
}

/* round(decimal64, int), a decimal64, as numeric's; past 17 digits an error. */
Datum decimal64_round(PG_FUNCTION_ARGS) {
  return d64_round_or_trunc(PG_GETARG_DATUM(0), PG_GETARG_INT32(1), false);
}

/* round(decimal64): round(x, 0). */
Datum decimal64_round0(PG_FUNCTION_ARGS) {
  return d64_round_or_trunc(PG_GETARG_DATUM(0), 0, false);
}

/* trunc(decimal64, int), a decimal64, as numeric's; past 17 places an error. */
Datum decimal64_trunc(PG_FUNCTION_ARGS) {
  return d64_round_or_trunc(PG_GETARG_DATUM(0), PG_GETARG_INT32(1), true);
}

/* trunc(decimal64): trunc(x, 0). */
Datum decimal64_trunc0(PG_FUNCTION_ARGS) {
  return d64_round_or_trunc(PG_GETARG_DATUM(0), 0, true);
}

/*
 * to_char(decimal64, text), as numeric's: its value as a decimal128 on the
 * stack. The call keeps this call's FmgrInfo, where numeric_to_char keeps
 * its compiled format.
 */
Datum decimal64_to_char(PG_FUNCTION_ARGS) {
  Datum d = PG_GETARG_DATUM(0);
  decimal_t num;
  LOCAL_FCINFO(call, 2);

  decimal_set(&num, d64_value(d), d64_scale(d));
  InitFunctionCallInfoData(*call, fcinfo->flinfo, 2, PG_GET_COLLATION(), NULL,
                           NULL);
  call->args[0].value = PointerGetDatum(&num);
  call->args[0].isnull = false;
  call->args[1].value = PG_GETARG_DATUM(1);
  call->args[1].isnull = false;
  return numeric_to_char(call);
}

/* ceil(decimal64) and ceiling(decimal64): the nearest integer >= it. */
Datum decimal64_ceil(PG_FUNCTION_ARGS) {
  Datum d = PG_GETARG_DATUM(0);
  ty_dec64_t r = ty_dec64_ceil({d64_value(d), d64_scale(d)});

  return d64_make(r.value, r.scale);
}

/* floor(decimal64): the nearest integer <= it. */
Datum decimal64_floor(PG_FUNCTION_ARGS) {
  Datum d = PG_GETARG_DATUM(0);
  ty_dec64_t r = ty_dec64_floor({d64_value(d), d64_scale(d)});

  return d64_make(r.value, r.scale);
}

/*
 * a mod b at the larger of their scales, the sign of a, as numeric's: in
 * 128 bits, and a decimal64, as a remainder has no more digits before the
 * point than the smaller of a and b, nor more places than the larger.
 */
Datum decimal64_mod(PG_FUNCTION_ARGS) {
  Datum da = PG_GETARG_DATUM(0), db = PG_GETARG_DATUM(1);
  ty_dec64_t a = {d64_value(da), d64_scale(da)};
  ty_dec64_t b = {d64_value(db), d64_scale(db)};
  ty_dec64_t r;

  /* The remainder is ty's; a zero b is its TY_ERR_DIVZERO. */
  if (ty_dec64_mod(&a, &b, &r) != 0)
    ty_error_raise();
  return d64_make(r.value, r.scale);
}

/* sign(decimal64): -1, 0 or 1, at scale 0, as numeric's. */
Datum decimal64_sign(PG_FUNCTION_ARGS) {
  Datum da = PG_GETARG_DATUM(0);

  return d64_make(ty_dec64_sign({d64_value(da), d64_scale(da)}), 0);
}

/*
 * gcd(decimal64, decimal64) at the larger scale, as numeric's: ty takes it
 * in 128 bits. It is a decimal64, as it is no larger than either operand,
 * except for gcd(0, x) at a scale that makes x past 17 digits: an error.
 */
Datum decimal64_gcd(PG_FUNCTION_ARGS) {
  Datum da = PG_GETARG_DATUM(0), db = PG_GETARG_DATUM(1);
  ty_dec64_t a = {d64_value(da), d64_scale(da)};
  ty_dec64_t b = {d64_value(db), d64_scale(db)};
  ty_dec64_t r;

  /* The gcd is ty's; past 17 digits is its TY_ERR_RANGE. */
  if (ty_dec64_gcd(&a, &b, &r) != 0)
    ty_error_raise();
  return d64_make(r.value, r.scale);
}

/*
 * lcm(decimal64, decimal64) at the larger scale, a decimal64: ty takes it in
 * 128 bits, and past 17 digits is an error, as a * b's is.
 */
Datum decimal64_lcm(PG_FUNCTION_ARGS) {
  Datum da = PG_GETARG_DATUM(0), db = PG_GETARG_DATUM(1);
  ty_dec64_t a = {d64_value(da), d64_scale(da)};
  ty_dec64_t b = {d64_value(db), d64_scale(db)};
  ty_dec64_t r;

  if (ty_dec64_lcm(&a, &b, &r) != 0)
    ty_error_raise();
  return d64_make(r.value, r.scale);
}

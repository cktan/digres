/*
 * Portions Copyright (c) 2026, CK Tan: the changes for digres,
 * under the MIT License (https://github.com/cktan/digres/blob/main/LICENSE)
 */

/*
 * cash.c
 * Written by D'Arcy J.M. Cain
 * darcy@druid.net
 * http://www.druid.net/darcy/
 *
 * Functions to allow input and output of money normally but store
 * and handle it as 64 bit ints
 *
 * A slightly modified version of this file and a discussion of the
 * workings can be found in the book "Software Solutions in C" by
 * Dale Schumacher, Academic Press, ISBN: 0-12-632360-7 except that
 * this version handles 64 bit numbers and so can hold values up to
 * $92,233,720,368,547,758.07.
 *
 * src/backend/utils/adt/cash.c
 */

#include "postgres.h"

#include <limits.h>
#include <ctype.h>
#include <math.h>

#include "common/int.h"
#include "libpq/pqformat.h"
#include "utils/builtins.h"
#include "utils/cash.h"
#include "utils/float.h"
#include "utils/numeric.h"
#include "utils/pg_locale.h"
#include "ty_money.h"

/*************************************************************************
 * Private routines
 ************************************************************************/

/* The money fields of the locale, as ty reads them. */
static ty_money_fmt_t money_fmt_of(const struct lconv *l) {
  ty_money_fmt_t f;

  f.frac_digits = l->frac_digits;
  f.mon_decimal_point = l->mon_decimal_point;
  f.mon_thousands_sep = l->mon_thousands_sep;
  f.currency_symbol = l->currency_symbol;
  f.positive_sign = l->positive_sign;
  f.negative_sign = l->negative_sign;
  f.mon_grouping = *l->mon_grouping;
  f.p_cs_precedes = l->p_cs_precedes;
  f.n_cs_precedes = l->n_cs_precedes;
  f.p_sep_by_space = l->p_sep_by_space;
  f.n_sep_by_space = l->n_sep_by_space;
  f.p_sign_posn = l->p_sign_posn;
  f.n_sign_posn = l->n_sign_posn;
  return f;
}

/* Raise the error of a failed ty money call. */
static pg_noreturn void money_raise(void) {
  switch (ty_errnum()) {
  case TY_ERR_DIVZERO:
    ereport(ERROR,
            (errcode(ERRCODE_DIVISION_BY_ZERO), errmsg("division by zero")));
  default:
    ereport(ERROR, (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
                    errmsg("money out of range")));
  }
}


static inline Cash cash_pl_cash(Cash c1, Cash c2) {
  Cash res;

  if (unlikely(ty_money_add(c1, c2, &res)))
    money_raise();
  return res;
}

static inline Cash cash_mi_cash(Cash c1, Cash c2) {
  Cash res;

  if (unlikely(ty_money_sub(c1, c2, &res)))
    money_raise();
  return res;
}

static inline Cash cash_mul_float8(Cash c, float8 f) {
  Cash res;

  if (unlikely(ty_money_mul_float64(c, f, &res))) {
    /* an overflow or underflow of the float multiply is its own error */
    if (strcmp(ty_errmsg(), "money out of range") != 0)
      float_raise_ty();
    money_raise();
  }
  return res;
}

static inline Cash cash_div_float8(Cash c, float8 f) {
  Cash res;

  if (unlikely(ty_money_div_float64(c, f, &res))) {
    if (strcmp(ty_errmsg(), "money out of range") != 0)
      float_raise_ty();
    money_raise();
  }
  return res;
}

static inline Cash cash_mul_int64(Cash c, int64 i) {
  Cash res;

  if (unlikely(ty_money_mul_int64(c, i, &res)))
    money_raise();
  return res;
}

static inline Cash cash_div_int64(Cash c, int64 i) {
  Cash res;

  if (unlikely(ty_money_div_int64(c, i, &res)))
    money_raise();
  return res;
}

/* cash_in()
 * Convert a string to a cash data type.
 * Format is [$]###[,]###[.##]
 * Examples: 123.45 $123.45 $123,456.78
 *
 */
Datum cash_in(PG_FUNCTION_ARGS) {
  char *str = PG_GETARG_CSTRING(0);
  Node *escontext = fcinfo->context;
  Cash result;
  ty_money_fmt_t f = money_fmt_of(PGLC_localeconv());

  /* ty reads the text, by the locale's money settings */
  if (ty_money_parse(str, &f, &result)) {
    if (ty_errnum() == TY_ERR_RANGE)
      ereturn(
          escontext, (Datum)0,
          (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
           errmsg("value \"%s\" is out of range for type %s", str, "money")));
    ereturn(
        escontext, (Datum)0,
        (errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
         errmsg("invalid input syntax for type %s: \"%s\"", "money", str)));
  }

  PG_RETURN_CASH(result);
}

/* cash_out()
 * Function to convert cash to a dollars and cents representation, using
 * the lc_monetary locale's formatting.
 */
Datum cash_out(PG_FUNCTION_ARGS) {
  Cash value = PG_GETARG_CASH(0);
  ty_money_fmt_t f = money_fmt_of(PGLC_localeconv());
  char *result = (char *)palloc(64);
  size_t n = ty_money_format(value, &f, result, 64);

  if (n >= 64) {
    /* a long currency symbol: ask again, with room */
    result = (char *)repalloc(result, n + 1);
    ty_money_format(value, &f, result, n + 1);
  }

  PG_RETURN_CSTRING(result);
}

/*
 *		cash_recv			- converts external binary
 *format to cash
 */
Datum cash_recv(PG_FUNCTION_ARGS) {
  StringInfo buf = (StringInfo)PG_GETARG_POINTER(0);

  PG_RETURN_CASH((Cash)pq_getmsgint64(buf));
}

/*
 *		cash_send			- converts cash to binary format
 */
Datum cash_send(PG_FUNCTION_ARGS) {
  Cash arg1 = PG_GETARG_CASH(0);
  StringInfoData buf;

  pq_begintypsend(&buf);
  pq_sendint64(&buf, arg1);
  PG_RETURN_BYTEA_P(pq_endtypsend(&buf));
}

/*
 * Comparison functions
 */

Datum cash_eq(PG_FUNCTION_ARGS) {
  Cash c1 = PG_GETARG_CASH(0);
  Cash c2 = PG_GETARG_CASH(1);

  PG_RETURN_BOOL(c1 == c2);
}

Datum cash_ne(PG_FUNCTION_ARGS) {
  Cash c1 = PG_GETARG_CASH(0);
  Cash c2 = PG_GETARG_CASH(1);

  PG_RETURN_BOOL(c1 != c2);
}

Datum cash_lt(PG_FUNCTION_ARGS) {
  Cash c1 = PG_GETARG_CASH(0);
  Cash c2 = PG_GETARG_CASH(1);

  PG_RETURN_BOOL(c1 < c2);
}

Datum cash_le(PG_FUNCTION_ARGS) {
  Cash c1 = PG_GETARG_CASH(0);
  Cash c2 = PG_GETARG_CASH(1);

  PG_RETURN_BOOL(c1 <= c2);
}

Datum cash_gt(PG_FUNCTION_ARGS) {
  Cash c1 = PG_GETARG_CASH(0);
  Cash c2 = PG_GETARG_CASH(1);

  PG_RETURN_BOOL(c1 > c2);
}

Datum cash_ge(PG_FUNCTION_ARGS) {
  Cash c1 = PG_GETARG_CASH(0);
  Cash c2 = PG_GETARG_CASH(1);

  PG_RETURN_BOOL(c1 >= c2);
}

Datum cash_cmp(PG_FUNCTION_ARGS) {
  Cash c1 = PG_GETARG_CASH(0);
  Cash c2 = PG_GETARG_CASH(1);

  if (c1 > c2)
    PG_RETURN_INT32(1);
  else if (c1 == c2)
    PG_RETURN_INT32(0);
  else
    PG_RETURN_INT32(-1);
}

/* cash_pl()
 * Add two cash values.
 */
Datum cash_pl(PG_FUNCTION_ARGS) {
  Cash c1 = PG_GETARG_CASH(0);
  Cash c2 = PG_GETARG_CASH(1);

  PG_RETURN_CASH(cash_pl_cash(c1, c2));
}

/* cash_mi()
 * Subtract two cash values.
 */
Datum cash_mi(PG_FUNCTION_ARGS) {
  Cash c1 = PG_GETARG_CASH(0);
  Cash c2 = PG_GETARG_CASH(1);

  PG_RETURN_CASH(cash_mi_cash(c1, c2));
}

/* cash_div_cash()
 * Divide cash by cash, returning float8.
 */
Datum cash_div_cash(PG_FUNCTION_ARGS) {
  Cash dividend = PG_GETARG_CASH(0);
  Cash divisor = PG_GETARG_CASH(1);
  float8 quotient;

  if (ty_money_div_money(dividend, divisor, &quotient))
    money_raise();

  PG_RETURN_FLOAT8(quotient);
}

/* cash_mul_flt8()
 * Multiply cash by float8.
 */
Datum cash_mul_flt8(PG_FUNCTION_ARGS) {
  Cash c = PG_GETARG_CASH(0);
  float8 f = PG_GETARG_FLOAT8(1);

  PG_RETURN_CASH(cash_mul_float8(c, f));
}

/* flt8_mul_cash()
 * Multiply float8 by cash.
 */
Datum flt8_mul_cash(PG_FUNCTION_ARGS) {
  float8 f = PG_GETARG_FLOAT8(0);
  Cash c = PG_GETARG_CASH(1);

  PG_RETURN_CASH(cash_mul_float8(c, f));
}

/* cash_div_flt8()
 * Divide cash by float8.
 */
Datum cash_div_flt8(PG_FUNCTION_ARGS) {
  Cash c = PG_GETARG_CASH(0);
  float8 f = PG_GETARG_FLOAT8(1);

  PG_RETURN_CASH(cash_div_float8(c, f));
}

/* cash_mul_flt4()
 * Multiply cash by float4.
 */
Datum cash_mul_flt4(PG_FUNCTION_ARGS) {
  Cash c = PG_GETARG_CASH(0);
  float4 f = PG_GETARG_FLOAT4(1);

  PG_RETURN_CASH(cash_mul_float8(c, (float8)f));
}

/* flt4_mul_cash()
 * Multiply float4 by cash.
 */
Datum flt4_mul_cash(PG_FUNCTION_ARGS) {
  float4 f = PG_GETARG_FLOAT4(0);
  Cash c = PG_GETARG_CASH(1);

  PG_RETURN_CASH(cash_mul_float8(c, (float8)f));
}

/* cash_div_flt4()
 * Divide cash by float4.
 *
 */
Datum cash_div_flt4(PG_FUNCTION_ARGS) {
  Cash c = PG_GETARG_CASH(0);
  float4 f = PG_GETARG_FLOAT4(1);

  PG_RETURN_CASH(cash_div_float8(c, (float8)f));
}

/* cash_mul_int8()
 * Multiply cash by int8.
 */
Datum cash_mul_int8(PG_FUNCTION_ARGS) {
  Cash c = PG_GETARG_CASH(0);
  int64 i = PG_GETARG_INT64(1);

  PG_RETURN_CASH(cash_mul_int64(c, i));
}

/* int8_mul_cash()
 * Multiply int8 by cash.
 */
Datum int8_mul_cash(PG_FUNCTION_ARGS) {
  int64 i = PG_GETARG_INT64(0);
  Cash c = PG_GETARG_CASH(1);

  PG_RETURN_CASH(cash_mul_int64(c, i));
}

/* cash_div_int8()
 * Divide cash by 8-byte integer.
 */
Datum cash_div_int8(PG_FUNCTION_ARGS) {
  Cash c = PG_GETARG_CASH(0);
  int64 i = PG_GETARG_INT64(1);

  PG_RETURN_CASH(cash_div_int64(c, i));
}

/* cash_mul_int4()
 * Multiply cash by int4.
 */
Datum cash_mul_int4(PG_FUNCTION_ARGS) {
  Cash c = PG_GETARG_CASH(0);
  int32 i = PG_GETARG_INT32(1);

  PG_RETURN_CASH(cash_mul_int64(c, (int64)i));
}

/* int4_mul_cash()
 * Multiply int4 by cash.
 */
Datum int4_mul_cash(PG_FUNCTION_ARGS) {
  int32 i = PG_GETARG_INT32(0);
  Cash c = PG_GETARG_CASH(1);

  PG_RETURN_CASH(cash_mul_int64(c, (int64)i));
}

/* cash_div_int4()
 * Divide cash by 4-byte integer.
 *
 */
Datum cash_div_int4(PG_FUNCTION_ARGS) {
  Cash c = PG_GETARG_CASH(0);
  int32 i = PG_GETARG_INT32(1);

  PG_RETURN_CASH(cash_div_int64(c, (int64)i));
}

/* cash_mul_int2()
 * Multiply cash by int2.
 */
Datum cash_mul_int2(PG_FUNCTION_ARGS) {
  Cash c = PG_GETARG_CASH(0);
  int16 s = PG_GETARG_INT16(1);

  PG_RETURN_CASH(cash_mul_int64(c, (int64)s));
}

/* int2_mul_cash()
 * Multiply int2 by cash.
 */
Datum int2_mul_cash(PG_FUNCTION_ARGS) {
  int16 s = PG_GETARG_INT16(0);
  Cash c = PG_GETARG_CASH(1);

  PG_RETURN_CASH(cash_mul_int64(c, (int64)s));
}

/* cash_div_int2()
 * Divide cash by int2.
 *
 */
Datum cash_div_int2(PG_FUNCTION_ARGS) {
  Cash c = PG_GETARG_CASH(0);
  int16 s = PG_GETARG_INT16(1);

  PG_RETURN_CASH(cash_div_int64(c, (int64)s));
}

/* cashlarger()
 * Return larger of two cash values.
 */
Datum cashlarger(PG_FUNCTION_ARGS) {
  Cash c1 = PG_GETARG_CASH(0);
  Cash c2 = PG_GETARG_CASH(1);
  Cash result;

  result = (c1 > c2) ? c1 : c2;

  PG_RETURN_CASH(result);
}

/* cashsmaller()
 * Return smaller of two cash values.
 */
Datum cashsmaller(PG_FUNCTION_ARGS) {
  Cash c1 = PG_GETARG_CASH(0);
  Cash c2 = PG_GETARG_CASH(1);
  Cash result;

  result = (c1 < c2) ? c1 : c2;

  PG_RETURN_CASH(result);
}

/* cash_words()
 * This converts an int4 as well but to a representation using words
 * Obviously way North American centric - sorry
 */
Datum cash_words(PG_FUNCTION_ARGS) {
  Cash value = PG_GETARG_CASH(0);
  char buf[256];
  size_t n = ty_money_words(value, buf);

  PG_RETURN_TEXT_P(cstring_to_text_with_len(buf, n));
}

/* cash_numeric()
 * Convert cash to numeric.
 */
Datum cash_numeric(PG_FUNCTION_ARGS) {
  Cash money = PG_GETARG_CASH(0);
  Datum result;
  int fpoint;
  struct lconv *lconvert = PGLC_localeconv();

  /* see comments about frac_digits in cash_in() */
  fpoint = lconvert->frac_digits;
  if (fpoint < 0 || fpoint > 10)
    fpoint = 2;

  /* convert the integral money value to numeric */
  result = NumericGetDatum(int64_to_numeric(money));

  /* scale appropriately, if needed */
  if (fpoint > 0) {
    int64 scale;
    int i;
    Datum numeric_scale;
    Datum quotient;

    /* compute required scale factor */
    scale = 1;
    for (i = 0; i < fpoint; i++)
      scale *= 10;
    numeric_scale = NumericGetDatum(int64_to_numeric(scale));

    /*
     * Given integral inputs approaching INT64_MAX, select_div_scale()
     * might choose a result scale of zero, causing loss of fractional
     * digits in the quotient.  We can ensure an exact result by setting
     * the dscale of either input to be at least as large as the desired
     * result scale.  numeric_round() will do that for us.
     */
    numeric_scale = DirectFunctionCall2(numeric_round, numeric_scale,
                                        Int32GetDatum(fpoint));

    /* Now we can safely divide ... */
    quotient = DirectFunctionCall2(numeric_div, result, numeric_scale);

    /* ... and forcibly round to exactly the intended number of digits */
    result =
        DirectFunctionCall2(numeric_round, quotient, Int32GetDatum(fpoint));
  }

  PG_RETURN_DATUM(result);
}

/* numeric_cash()
 * Convert numeric to cash.
 */
Datum numeric_cash(PG_FUNCTION_ARGS) {
  Datum amount = PG_GETARG_DATUM(0);
  Cash result;
  int fpoint;
  int64 scale;
  int i;
  Datum numeric_scale;
  struct lconv *lconvert = PGLC_localeconv();

  /* see comments about frac_digits in cash_in() */
  fpoint = lconvert->frac_digits;
  if (fpoint < 0 || fpoint > 10)
    fpoint = 2;

  /* compute required scale factor */
  scale = 1;
  for (i = 0; i < fpoint; i++)
    scale *= 10;

  /* multiply the input amount by scale factor */
  numeric_scale = NumericGetDatum(int64_to_numeric(scale));
  amount = DirectFunctionCall2(numeric_mul, amount, numeric_scale);

  /* note that numeric_int8 will round to nearest integer for us */
  result = DatumGetInt64(DirectFunctionCall1(numeric_int8, amount));

  PG_RETURN_CASH(result);
}

/* int4_cash()
 * Convert int4 (int) to cash
 */
Datum int4_cash(PG_FUNCTION_ARGS) {
  int32 amount = PG_GETARG_INT32(0);
  Cash result;
  int fpoint;
  int64 scale;
  int i;
  struct lconv *lconvert = PGLC_localeconv();

  /* see comments about frac_digits in cash_in() */
  fpoint = lconvert->frac_digits;
  if (fpoint < 0 || fpoint > 10)
    fpoint = 2;

  /* compute required scale factor */
  scale = 1;
  for (i = 0; i < fpoint; i++)
    scale *= 10;

  /* compute amount * scale, checking for overflow */
  result = DatumGetInt64(DirectFunctionCall2(int8mul, Int64GetDatum(amount),
                                             Int64GetDatum(scale)));

  PG_RETURN_CASH(result);
}

/* int8_cash()
 * Convert int8 (bigint) to cash
 */
Datum int8_cash(PG_FUNCTION_ARGS) {
  int64 amount = PG_GETARG_INT64(0);
  Cash result;
  int fpoint;
  int64 scale;
  int i;
  struct lconv *lconvert = PGLC_localeconv();

  /* see comments about frac_digits in cash_in() */
  fpoint = lconvert->frac_digits;
  if (fpoint < 0 || fpoint > 10)
    fpoint = 2;

  /* compute required scale factor */
  scale = 1;
  for (i = 0; i < fpoint; i++)
    scale *= 10;

  /* compute amount * scale, checking for overflow */
  result = DatumGetInt64(DirectFunctionCall2(int8mul, Int64GetDatum(amount),
                                             Int64GetDatum(scale)));

  PG_RETURN_CASH(result);
}

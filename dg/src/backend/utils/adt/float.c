/*-------------------------------------------------------------------------
 *
 * float.c
 *	  Functions for the built-in floating-point types.
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 * Portions Copyright (c) 2026, CK Tan: the changes for digres,
 * under the MIT License (https://github.com/cktan/digres/blob/main/LICENSE)
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/float.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <ctype.h>
#include <float.h>
#include <math.h>
#include <limits.h>

#include "catalog/pg_type.h"
#include "common/int.h"
#include "common/shortest_dec.h"
#include "libpq/pqformat.h"
#include "utils/array.h"
#include "utils/float.h"
#include "utils/fmgrprotos.h"
#include "utils/sortsupport.h"
#include "utils/control_block.h"

/*
 * Configurable GUC parameter
 *
 * If >0, use shortest-decimal format for output; this is both the default and
 * allows for compatibility with clients that explicitly set a value here to
 * get round-trip-accurate results. If 0 or less, then use the old, slow,
 * decimal rounding method.
 */

/*
 * We use these out-of-line ereport() calls to report float overflow,
 * underflow, and zero-divide, because following our usual practice of
 * repeating them at each call site would lead to a lot of code bloat.
 *
 * This does mean that you don't get a useful error location indicator.
 */
pg_noinline void float_overflow_error(void) {
  ereport(ERROR, (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
                  errmsg("value out of range: overflow")));
}

pg_noinline void float_underflow_error(void) {
  ereport(ERROR, (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
                  errmsg("value out of range: underflow")));
}

pg_noinline void float_zero_divide_error(void) {
  ereport(ERROR,
          (errcode(ERRCODE_DIVISION_BY_ZERO), errmsg("division by zero")));
}

/*
 * Returns -1 if 'val' represents negative infinity, 1 if 'val'
 * represents (positive) infinity, and 0 otherwise. On some platforms,
 * this is equivalent to the isinf() macro, but not everywhere: C99
 * does not specify that isinf() needs to distinguish between positive
 * and negative infinity.
 */
int is_infinite(double val) {
  int inf = isinf(val);

  if (inf == 0)
    return 0;
  else if (val > 0)
    return 1;
  else
    return -1;
}

/* ========== USER I/O ROUTINES ========== */

/*
 *		float4in		- converts "num" to float4
 *
 * Note that this code now uses strtof(), where it used to use strtod().
 *
 * The motivation for using strtof() is to avoid a double-rounding problem:
 * for certain decimal inputs, if you round the input correctly to a double,
 * and then round the double to a float, the result is incorrect in that it
 * does not match the result of rounding the decimal value to float directly.
 *
 * One of the best examples is 7.038531e-26:
 *
 * 0xAE43FDp-107 = 7.03853069185120912085...e-26
 *      midpoint   7.03853100000000022281...e-26
 * 0xAE43FEp-107 = 7.03853130814879132477...e-26
 *
 * making 0xAE43FDp-107 the correct float result, but if you do the conversion
 * via a double, you get
 *
 * 0xAE43FD.7FFFFFF8p-107 = 7.03853099999999907487...e-26
 *               midpoint   7.03853099999999964884...e-26
 * 0xAE43FD.80000000p-107 = 7.03853100000000022281...e-26
 * 0xAE43FD.80000008p-107 = 7.03853100000000137076...e-26
 *
 * so the value rounds to the double exactly on the midpoint between the two
 * nearest floats, and then rounding again to a float gives the incorrect
 * result of 0xAE43FEp-107.
 *
 */
Datum float4in(PG_FUNCTION_ARGS) {
  char *num = PG_GETARG_CSTRING(0);

  PG_RETURN_FLOAT4(float4in_internal(num, NULL, "real", num, fcinfo->context));
}

/*
 * float4in_internal - guts of float4in()
 *
 * This is exposed for use by functions that want a reasonably
 * platform-independent way of inputting floats. The behavior is
 * essentially like strtof + ereturn on error.
 *
 * Uses the same API as float8in_internal below, so most of its
 * comments also apply here, except regarding use in geometric types.
 */
float4 float4in_internal(char *num, char **endptr_p, const char *type_name,
                         const char *orig_string, struct Node *escontext) {
  float4 val;
  const char *end;
  size_t len;
  const char *first = num;

  /* ty reads the number, the names NaN and Infinity, and the space around */
  if (ty_float32_parse(num, &val, endptr_p ? &end : NULL, &len) == 0) {
    if (endptr_p)
      *endptr_p = (char *)end;
    return val;
  }

  if (ty_errnum() == TY_ERR_RANGE) {
    char *errnumber;

    /* the message shows the number, not the space before it or text after */
    while (*first != '\0' && isspace((unsigned char)*first))
      first++;
    errnumber = pstrdup(first);
    errnumber[len] = '\0';

    ereturn(escontext, 0,
            (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
             errmsg("\"%s\" is out of range for type real", errnumber)));
  }

  ereturn(escontext, 0,
          (errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
           errmsg("invalid input syntax for type %s: \"%s\"", type_name,
                  orig_string)));
}

/*
 *		float4out		- converts a float4 number to a string
 *						  using a standard output format
 */
Datum float4out(PG_FUNCTION_ARGS) {
  char *ascii = (char *)palloc(TY_FLOAT_TEXT_SIZE);

  /* ty prints the shortest text if extra_float_digits is above 0, else %g */
  ty_float32_format(PG_GETARG_FLOAT4(0), tcb->guc.extra_float_digits, ascii);
  PG_RETURN_CSTRING(ascii);
}

/*
 *		float4recv			- converts external binary
 *format to float4
 */
Datum float4recv(PG_FUNCTION_ARGS) {
  StringInfo buf = (StringInfo)PG_GETARG_POINTER(0);

  PG_RETURN_FLOAT4(pq_getmsgfloat4(buf));
}

/*
 *		float4send			- converts float4 to binary
 *format
 */
Datum float4send(PG_FUNCTION_ARGS) {
  float4 num = PG_GETARG_FLOAT4(0);
  StringInfoData buf;

  pq_begintypsend(&buf);
  pq_sendfloat4(&buf, num);
  PG_RETURN_BYTEA_P(pq_endtypsend(&buf));
}

/*
 *		float8in		- converts "num" to float8
 */
Datum float8in(PG_FUNCTION_ARGS) {
  char *num = PG_GETARG_CSTRING(0);

  PG_RETURN_FLOAT8(
      float8in_internal(num, NULL, "double precision", num, fcinfo->context));
}

/*
 * float8in_internal - guts of float8in()
 *
 * This is exposed for use by functions that want a reasonably
 * platform-independent way of inputting doubles.  The behavior is
 * essentially like strtod + ereturn on error, but note the following
 * differences:
 * 1. Both leading and trailing whitespace are skipped.
 * 2. If endptr_p is NULL, we report error if there's trailing junk.
 * Otherwise, it's up to the caller to complain about trailing junk.
 * 3. In event of a syntax error, the report mentions the given type_name
 * and prints orig_string as the input; this is meant to support use of
 * this function with types such as "box" and "point", where what we are
 * parsing here is just a substring of orig_string.
 *
 * If escontext points to an ErrorSaveContext node, that is filled instead
 * of throwing an error; the caller must check SOFT_ERROR_OCCURRED()
 * to detect errors.
 *
 * "num" could validly be declared "const char *", but that results in an
 * unreasonable amount of extra casting both here and in callers, so we don't.
 */
float8 float8in_internal(char *num, char **endptr_p, const char *type_name,
                         const char *orig_string, struct Node *escontext) {
  float8 val;
  const char *end;
  size_t len;
  const char *first = num;

  /* ty reads the number, the names NaN and Infinity, and the space around */
  if (ty_float64_parse(num, &val, endptr_p ? &end : NULL, &len) == 0) {
    if (endptr_p)
      *endptr_p = (char *)end;
    return val;
  }

  if (ty_errnum() == TY_ERR_RANGE) {
    char *errnumber;

    /* the message shows the number, not the space before it or text after */
    while (*first != '\0' && isspace((unsigned char)*first))
      first++;
    errnumber = pstrdup(first);
    errnumber[len] = '\0';

    ereturn(escontext, 0,
            (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
             errmsg("\"%s\" is out of range for type double precision", errnumber)));
  }

  ereturn(escontext, 0,
          (errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
           errmsg("invalid input syntax for type %s: \"%s\"", type_name,
                  orig_string)));
}

/*
 *		float8out		- converts float8 number to a string
 *						  using a standard output format
 */
Datum float8out(PG_FUNCTION_ARGS) {
  float8 num = PG_GETARG_FLOAT8(0);

  PG_RETURN_CSTRING(float8out_internal(num));
}

/*
 * float8out_internal - guts of float8out()
 *
 * This is exposed for use by functions that want a reasonably
 * platform-independent way of outputting doubles.
 * The result is always palloc'd.
 */
char *float8out_internal(double num) {
  char *ascii = (char *)palloc(TY_FLOAT_TEXT_SIZE);

  ty_float64_format(num, tcb->guc.extra_float_digits, ascii);
  return ascii;
}

/*
 *		float8recv			- converts external binary
 *format to float8
 */
Datum float8recv(PG_FUNCTION_ARGS) {
  StringInfo buf = (StringInfo)PG_GETARG_POINTER(0);

  PG_RETURN_FLOAT8(pq_getmsgfloat8(buf));
}

/*
 *		float8send			- converts float8 to binary
 *format
 */
Datum float8send(PG_FUNCTION_ARGS) {
  float8 num = PG_GETARG_FLOAT8(0);
  StringInfoData buf;

  pq_begintypsend(&buf);
  pq_sendfloat8(&buf, num);
  PG_RETURN_BYTEA_P(pq_endtypsend(&buf));
}

/*
 * Raise PostgreSQL's error for a ty float function that failed: a domain
 * error (TY_ERR_INVALID) with the given SQLSTATE and ty's message, which is
 * PostgreSQL's text; any other as float.h says.
 */
static pg_noreturn pg_noinline void float_ty_error(int sqlstate) {
  if (ty_errnum() == TY_ERR_INVALID)
    ereport(ERROR, (errcode(sqlstate), errmsg("%s", ty_errmsg())));
  float_raise_ty();
}


/* ========== PUBLIC ROUTINES ========== */

/*
 *		======================
 *		FLOAT4 BASE OPERATIONS
 *		======================
 */

/*
 *		float4abs		- returns |arg1| (absolute value)
 */
Datum float4abs(PG_FUNCTION_ARGS) {
  PG_RETURN_FLOAT4(ty_float32_abs(PG_GETARG_FLOAT4(0)));
}

/*
 *		float4um		- returns -arg1 (unary minus)
 */
Datum float4um(PG_FUNCTION_ARGS) {
  PG_RETURN_FLOAT4(ty_float32_neg(PG_GETARG_FLOAT4(0)));
}

Datum float4up(PG_FUNCTION_ARGS) {
  float4 arg = PG_GETARG_FLOAT4(0);

  PG_RETURN_FLOAT4(arg);
}

Datum float4larger(PG_FUNCTION_ARGS) {
  PG_RETURN_FLOAT4(ty_float32_larger(PG_GETARG_FLOAT4(0), PG_GETARG_FLOAT4(1)));
}

Datum float4smaller(PG_FUNCTION_ARGS) {
  PG_RETURN_FLOAT4(ty_float32_smaller(PG_GETARG_FLOAT4(0), PG_GETARG_FLOAT4(1)));
}

/*
 *		======================
 *		FLOAT8 BASE OPERATIONS
 *		======================
 */

/*
 *		float8abs		- returns |arg1| (absolute value)
 */
Datum float8abs(PG_FUNCTION_ARGS) {
  PG_RETURN_FLOAT8(ty_float64_abs(PG_GETARG_FLOAT8(0)));
}

/*
 *		float8um		- returns -arg1 (unary minus)
 */
Datum float8um(PG_FUNCTION_ARGS) {
  PG_RETURN_FLOAT8(ty_float64_neg(PG_GETARG_FLOAT8(0)));
}

Datum float8up(PG_FUNCTION_ARGS) {
  float8 arg = PG_GETARG_FLOAT8(0);

  PG_RETURN_FLOAT8(arg);
}

Datum float8larger(PG_FUNCTION_ARGS) {
  PG_RETURN_FLOAT8(ty_float64_larger(PG_GETARG_FLOAT8(0), PG_GETARG_FLOAT8(1)));
}

Datum float8smaller(PG_FUNCTION_ARGS) {
  PG_RETURN_FLOAT8(ty_float64_smaller(PG_GETARG_FLOAT8(0), PG_GETARG_FLOAT8(1)));
}

/*
 *		====================
 *		ARITHMETIC OPERATORS
 *		====================
 */

/*
 *		float4pl		- returns arg1 + arg2
 *		float4mi		- returns arg1 - arg2
 *		float4mul		- returns arg1 * arg2
 *		float4div		- returns arg1 / arg2
 */
Datum float4pl(PG_FUNCTION_ARGS) {
  float4 arg1 = PG_GETARG_FLOAT4(0);
  float4 arg2 = PG_GETARG_FLOAT4(1);

  PG_RETURN_FLOAT4(float4_pl(arg1, arg2));
}

Datum float4mi(PG_FUNCTION_ARGS) {
  float4 arg1 = PG_GETARG_FLOAT4(0);
  float4 arg2 = PG_GETARG_FLOAT4(1);

  PG_RETURN_FLOAT4(float4_mi(arg1, arg2));
}

Datum float4mul(PG_FUNCTION_ARGS) {
  float4 arg1 = PG_GETARG_FLOAT4(0);
  float4 arg2 = PG_GETARG_FLOAT4(1);

  PG_RETURN_FLOAT4(float4_mul(arg1, arg2));
}

Datum float4div(PG_FUNCTION_ARGS) {
  float4 arg1 = PG_GETARG_FLOAT4(0);
  float4 arg2 = PG_GETARG_FLOAT4(1);

  PG_RETURN_FLOAT4(float4_div(arg1, arg2));
}

/*
 *		float8pl		- returns arg1 + arg2
 *		float8mi		- returns arg1 - arg2
 *		float8mul		- returns arg1 * arg2
 *		float8div		- returns arg1 / arg2
 */
Datum float8pl(PG_FUNCTION_ARGS) {
  float8 arg1 = PG_GETARG_FLOAT8(0);
  float8 arg2 = PG_GETARG_FLOAT8(1);

  PG_RETURN_FLOAT8(float8_pl(arg1, arg2));
}

Datum float8mi(PG_FUNCTION_ARGS) {
  float8 arg1 = PG_GETARG_FLOAT8(0);
  float8 arg2 = PG_GETARG_FLOAT8(1);

  PG_RETURN_FLOAT8(float8_mi(arg1, arg2));
}

Datum float8mul(PG_FUNCTION_ARGS) {
  float8 arg1 = PG_GETARG_FLOAT8(0);
  float8 arg2 = PG_GETARG_FLOAT8(1);

  PG_RETURN_FLOAT8(float8_mul(arg1, arg2));
}

Datum float8div(PG_FUNCTION_ARGS) {
  float8 arg1 = PG_GETARG_FLOAT8(0);
  float8 arg2 = PG_GETARG_FLOAT8(1);

  PG_RETURN_FLOAT8(float8_div(arg1, arg2));
}

/*
 *		====================
 *		COMPARISON OPERATORS
 *		====================
 */

/*
 *		float4{eq,ne,lt,le,gt,ge}		- float4/float4
 *comparison operations
 */
int float4_cmp_internal(float4 a, float4 b) {
  return ty_float32_cmp(a, b);
}

Datum float4eq(PG_FUNCTION_ARGS) {
  float4 arg1 = PG_GETARG_FLOAT4(0);
  float4 arg2 = PG_GETARG_FLOAT4(1);

  PG_RETURN_BOOL(float4_eq(arg1, arg2));
}

Datum float4ne(PG_FUNCTION_ARGS) {
  float4 arg1 = PG_GETARG_FLOAT4(0);
  float4 arg2 = PG_GETARG_FLOAT4(1);

  PG_RETURN_BOOL(float4_ne(arg1, arg2));
}

Datum float4lt(PG_FUNCTION_ARGS) {
  float4 arg1 = PG_GETARG_FLOAT4(0);
  float4 arg2 = PG_GETARG_FLOAT4(1);

  PG_RETURN_BOOL(float4_lt(arg1, arg2));
}

Datum float4le(PG_FUNCTION_ARGS) {
  float4 arg1 = PG_GETARG_FLOAT4(0);
  float4 arg2 = PG_GETARG_FLOAT4(1);

  PG_RETURN_BOOL(float4_le(arg1, arg2));
}

Datum float4gt(PG_FUNCTION_ARGS) {
  float4 arg1 = PG_GETARG_FLOAT4(0);
  float4 arg2 = PG_GETARG_FLOAT4(1);

  PG_RETURN_BOOL(float4_gt(arg1, arg2));
}

Datum float4ge(PG_FUNCTION_ARGS) {
  float4 arg1 = PG_GETARG_FLOAT4(0);
  float4 arg2 = PG_GETARG_FLOAT4(1);

  PG_RETURN_BOOL(float4_ge(arg1, arg2));
}

Datum btfloat4cmp(PG_FUNCTION_ARGS) {
  float4 arg1 = PG_GETARG_FLOAT4(0);
  float4 arg2 = PG_GETARG_FLOAT4(1);

  PG_RETURN_INT32(float4_cmp_internal(arg1, arg2));
}

static int btfloat4fastcmp(Datum x, Datum y, SortSupport ssup) {
  float4 arg1 = DatumGetFloat4(x);
  float4 arg2 = DatumGetFloat4(y);

  return float4_cmp_internal(arg1, arg2);
}

Datum btfloat4sortsupport(PG_FUNCTION_ARGS) {
  SortSupport ssup = (SortSupport)PG_GETARG_POINTER(0);

  ssup->comparator = btfloat4fastcmp;
  PG_RETURN_VOID();
}

/*
 *		float8{eq,ne,lt,le,gt,ge}		- float8/float8
 *comparison operations
 */
int float8_cmp_internal(float8 a, float8 b) {
  return ty_float64_cmp(a, b);
}

Datum float8eq(PG_FUNCTION_ARGS) {
  float8 arg1 = PG_GETARG_FLOAT8(0);
  float8 arg2 = PG_GETARG_FLOAT8(1);

  PG_RETURN_BOOL(float8_eq(arg1, arg2));
}

Datum float8ne(PG_FUNCTION_ARGS) {
  float8 arg1 = PG_GETARG_FLOAT8(0);
  float8 arg2 = PG_GETARG_FLOAT8(1);

  PG_RETURN_BOOL(float8_ne(arg1, arg2));
}

Datum float8lt(PG_FUNCTION_ARGS) {
  float8 arg1 = PG_GETARG_FLOAT8(0);
  float8 arg2 = PG_GETARG_FLOAT8(1);

  PG_RETURN_BOOL(float8_lt(arg1, arg2));
}

Datum float8le(PG_FUNCTION_ARGS) {
  float8 arg1 = PG_GETARG_FLOAT8(0);
  float8 arg2 = PG_GETARG_FLOAT8(1);

  PG_RETURN_BOOL(float8_le(arg1, arg2));
}

Datum float8gt(PG_FUNCTION_ARGS) {
  float8 arg1 = PG_GETARG_FLOAT8(0);
  float8 arg2 = PG_GETARG_FLOAT8(1);

  PG_RETURN_BOOL(float8_gt(arg1, arg2));
}

Datum float8ge(PG_FUNCTION_ARGS) {
  float8 arg1 = PG_GETARG_FLOAT8(0);
  float8 arg2 = PG_GETARG_FLOAT8(1);

  PG_RETURN_BOOL(float8_ge(arg1, arg2));
}

Datum btfloat8cmp(PG_FUNCTION_ARGS) {
  float8 arg1 = PG_GETARG_FLOAT8(0);
  float8 arg2 = PG_GETARG_FLOAT8(1);

  PG_RETURN_INT32(float8_cmp_internal(arg1, arg2));
}

static int btfloat8fastcmp(Datum x, Datum y, SortSupport ssup) {
  float8 arg1 = DatumGetFloat8(x);
  float8 arg2 = DatumGetFloat8(y);

  return float8_cmp_internal(arg1, arg2);
}

Datum btfloat8sortsupport(PG_FUNCTION_ARGS) {
  SortSupport ssup = (SortSupport)PG_GETARG_POINTER(0);

  ssup->comparator = btfloat8fastcmp;
  PG_RETURN_VOID();
}

Datum btfloat48cmp(PG_FUNCTION_ARGS) {
  float4 arg1 = PG_GETARG_FLOAT4(0);
  float8 arg2 = PG_GETARG_FLOAT8(1);

  /* widen float4 to float8 and then compare */
  PG_RETURN_INT32(float8_cmp_internal(arg1, arg2));
}

Datum btfloat84cmp(PG_FUNCTION_ARGS) {
  float8 arg1 = PG_GETARG_FLOAT8(0);
  float4 arg2 = PG_GETARG_FLOAT4(1);

  /* widen float4 to float8 and then compare */
  PG_RETURN_INT32(float8_cmp_internal(arg1, arg2));
}

/*
 * in_range support function for float8.
 *
 * Note: we needn't supply a float8_float4 variant, as implicit coercion
 * of the offset value takes care of that scenario just as well.
 */
Datum in_range_float8_float8(PG_FUNCTION_ARGS) {
  float8 val = PG_GETARG_FLOAT8(0);
  float8 base = PG_GETARG_FLOAT8(1);
  float8 offset = PG_GETARG_FLOAT8(2);
  bool sub = PG_GETARG_BOOL(3);
  bool less = PG_GETARG_BOOL(4);
  float8 sum;

  /*
   * Reject negative or NaN offset.  Negative is per spec, and NaN is
   * because appropriate semantics for that seem non-obvious.
   */
  if (isnan(offset) || offset < 0)
    ereport(ERROR,
            (errcode(ERRCODE_INVALID_PRECEDING_OR_FOLLOWING_SIZE),
             errmsg("invalid preceding or following size in window function")));

  /*
   * Deal with cases where val and/or base is NaN, following the rule that
   * NaN sorts after non-NaN (cf float8_cmp_internal).  The offset cannot
   * affect the conclusion.
   */
  if (isnan(val)) {
    if (isnan(base))
      PG_RETURN_BOOL(true); /* NAN = NAN */
    else
      PG_RETURN_BOOL(!less); /* NAN > non-NAN */
  } else if (isnan(base)) {
    PG_RETURN_BOOL(less); /* non-NAN < NAN */
  }

  /*
   * Deal with cases where both base and offset are infinite, and computing
   * base +/- offset would produce NaN.  This corresponds to a window frame
   * whose boundary infinitely precedes +inf or infinitely follows -inf,
   * which is not well-defined.  For consistency with other cases involving
   * infinities, such as the fact that +inf infinitely follows +inf, we
   * choose to assume that +inf infinitely precedes +inf and -inf infinitely
   * follows -inf, and therefore that all finite and infinite values are in
   * such a window frame.
   *
   * offset is known positive, so we need only check the sign of base in
   * this test.
   */
  if (isinf(offset) && isinf(base) && (sub ? base > 0 : base < 0))
    PG_RETURN_BOOL(true);

  /*
   * Otherwise it should be safe to compute base +/- offset.  We trust the
   * FPU to cope if an input is +/-inf or the true sum would overflow, and
   * produce a suitably signed infinity, which will compare properly against
   * val whether or not that's infinity.
   */
  if (sub)
    sum = base - offset;
  else
    sum = base + offset;

  if (less)
    PG_RETURN_BOOL(val <= sum);
  else
    PG_RETURN_BOOL(val >= sum);
}

/*
 * in_range support function for float4.
 *
 * We would need a float4_float8 variant in any case, so we supply that and
 * let implicit coercion take care of the float4_float4 case.
 */
Datum in_range_float4_float8(PG_FUNCTION_ARGS) {
  float4 val = PG_GETARG_FLOAT4(0);
  float4 base = PG_GETARG_FLOAT4(1);
  float8 offset = PG_GETARG_FLOAT8(2);
  bool sub = PG_GETARG_BOOL(3);
  bool less = PG_GETARG_BOOL(4);
  float8 sum;

  /*
   * Reject negative or NaN offset.  Negative is per spec, and NaN is
   * because appropriate semantics for that seem non-obvious.
   */
  if (isnan(offset) || offset < 0)
    ereport(ERROR,
            (errcode(ERRCODE_INVALID_PRECEDING_OR_FOLLOWING_SIZE),
             errmsg("invalid preceding or following size in window function")));

  /*
   * Deal with cases where val and/or base is NaN, following the rule that
   * NaN sorts after non-NaN (cf float8_cmp_internal).  The offset cannot
   * affect the conclusion.
   */
  if (isnan(val)) {
    if (isnan(base))
      PG_RETURN_BOOL(true); /* NAN = NAN */
    else
      PG_RETURN_BOOL(!less); /* NAN > non-NAN */
  } else if (isnan(base)) {
    PG_RETURN_BOOL(less); /* non-NAN < NAN */
  }

  /*
   * Deal with cases where both base and offset are infinite, and computing
   * base +/- offset would produce NaN.  This corresponds to a window frame
   * whose boundary infinitely precedes +inf or infinitely follows -inf,
   * which is not well-defined.  For consistency with other cases involving
   * infinities, such as the fact that +inf infinitely follows +inf, we
   * choose to assume that +inf infinitely precedes +inf and -inf infinitely
   * follows -inf, and therefore that all finite and infinite values are in
   * such a window frame.
   *
   * offset is known positive, so we need only check the sign of base in
   * this test.
   */
  if (isinf(offset) && isinf(base) && (sub ? base > 0 : base < 0))
    PG_RETURN_BOOL(true);

  /*
   * Otherwise it should be safe to compute base +/- offset.  We trust the
   * FPU to cope if an input is +/-inf or the true sum would overflow, and
   * produce a suitably signed infinity, which will compare properly against
   * val whether or not that's infinity.
   */
  if (sub)
    sum = base - offset;
  else
    sum = base + offset;

  if (less)
    PG_RETURN_BOOL(val <= sum);
  else
    PG_RETURN_BOOL(val >= sum);
}

/*
 *		===================
 *		CONVERSION ROUTINES
 *		===================
 */

/*
 *		ftod			- converts a float4 number to a float8
 *number
 */
Datum ftod(PG_FUNCTION_ARGS) {
  PG_RETURN_FLOAT8(ty_float32_to_float64(PG_GETARG_FLOAT4(0)));
}

/*
 *		dtof			- converts a float8 number to a float4
 *number
 */
Datum dtof(PG_FUNCTION_ARGS) {
  float4 result;

  if (ty_float64_to_float32(PG_GETARG_FLOAT8(0), &result))
    float_raise_ty();

  PG_RETURN_FLOAT4(result);
}

/*
 *		dtoi4			- converts a float8 number to an int4
 *number
 */
Datum dtoi4(PG_FUNCTION_ARGS) {
  int32 result;

  /* ty rounds to the nearest and checks the range */
  if (ty_float64_to_int32(PG_GETARG_FLOAT8(0), &result))
    ereport(ERROR, (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
                    errmsg("integer out of range")));

  PG_RETURN_INT32(result);
}

/*
 *		dtoi2			- converts a float8 number to an int2
 *number
 */
Datum dtoi2(PG_FUNCTION_ARGS) {
  int16 result;

  /* ty rounds to the nearest and checks the range */
  if (ty_float64_to_int16(PG_GETARG_FLOAT8(0), &result))
    ereport(ERROR, (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
                    errmsg("smallint out of range")));

  PG_RETURN_INT16(result);
}

/*
 *		i4tod			- converts an int4 number to a float8
 *number
 */
Datum i4tod(PG_FUNCTION_ARGS) {
  PG_RETURN_FLOAT8(ty_int32_to_float64(PG_GETARG_INT32(0)));
}

/*
 *		i2tod			- converts an int2 number to a float8
 *number
 */
Datum i2tod(PG_FUNCTION_ARGS) {
  PG_RETURN_FLOAT8(ty_int16_to_float64(PG_GETARG_INT16(0)));
}

/*
 *		ftoi4			- converts a float4 number to an int4
 *number
 */
Datum ftoi4(PG_FUNCTION_ARGS) {
  int32 result;

  /* ty rounds to the nearest and checks the range */
  if (ty_float32_to_int32(PG_GETARG_FLOAT4(0), &result))
    ereport(ERROR, (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
                    errmsg("integer out of range")));

  PG_RETURN_INT32(result);
}

/*
 *		ftoi2			- converts a float4 number to an int2
 *number
 */
Datum ftoi2(PG_FUNCTION_ARGS) {
  int16 result;

  /* ty rounds to the nearest and checks the range */
  if (ty_float32_to_int16(PG_GETARG_FLOAT4(0), &result))
    ereport(ERROR, (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
                    errmsg("smallint out of range")));

  PG_RETURN_INT16(result);
}

/*
 *		i4tof			- converts an int4 number to a float4
 *number
 */
Datum i4tof(PG_FUNCTION_ARGS) {
  PG_RETURN_FLOAT4(ty_int32_to_float32(PG_GETARG_INT32(0)));
}

/*
 *		i2tof			- converts an int2 number to a float4
 *number
 */
Datum i2tof(PG_FUNCTION_ARGS) {
  PG_RETURN_FLOAT4(ty_int16_to_float32(PG_GETARG_INT16(0)));
}

/*
 *		=======================
 *		RANDOM FLOAT8 OPERATORS
 *		=======================
 */

/*
 *		dround			- returns	ROUND(arg1)
 */
Datum dround(PG_FUNCTION_ARGS) {
  PG_RETURN_FLOAT8(ty_float64_round(PG_GETARG_FLOAT8(0)));
}

/*
 *		dceil			- returns the smallest integer greater
 *than or equal to the specified float
 */
Datum dceil(PG_FUNCTION_ARGS) {
  PG_RETURN_FLOAT8(ty_float64_ceil(PG_GETARG_FLOAT8(0)));
}

/*
 *		dfloor			- returns the largest integer lesser
 *than or equal to the specified float
 */
Datum dfloor(PG_FUNCTION_ARGS) {
  PG_RETURN_FLOAT8(ty_float64_floor(PG_GETARG_FLOAT8(0)));
}

/*
 *		dsign			- returns -1 if the argument is less
 *than 0, 0 if the argument is equal to 0, and 1 if the argument is greater than
 *zero.
 */
Datum dsign(PG_FUNCTION_ARGS) {
  PG_RETURN_FLOAT8(ty_float64_sign(PG_GETARG_FLOAT8(0)));
}

/*
 *		dtrunc			- returns truncation-towards-zero of
 *arg1, arg1 >= 0 ... the greatest integer less than or equal to arg1 arg1 < 0
 *... the least integer greater than or equal to arg1
 */
Datum dtrunc(PG_FUNCTION_ARGS) {
  PG_RETURN_FLOAT8(ty_float64_trunc(PG_GETARG_FLOAT8(0)));
}

/*
 *		dsqrt			- returns square root of arg1
 */
Datum dsqrt(PG_FUNCTION_ARGS) {
  float8 result;

  if (ty_float64_sqrt(PG_GETARG_FLOAT8(0), &result))
    float_ty_error(ERRCODE_INVALID_ARGUMENT_FOR_POWER_FUNCTION);

  PG_RETURN_FLOAT8(result);
}

/*
 *		dcbrt			- returns cube root of arg1
 */
Datum dcbrt(PG_FUNCTION_ARGS) {
  float8 result;

  if (ty_float64_cbrt(PG_GETARG_FLOAT8(0), &result))
    float_raise_ty();

  PG_RETURN_FLOAT8(result);
}

/*
 *		dpow			- returns pow(arg1,arg2)
 */
Datum dpow(PG_FUNCTION_ARGS) {
  float8 result;

  /* ty decides the NaN, the infinities and the domain errors as POSIX says */
  if (ty_float64_pow(PG_GETARG_FLOAT8(0), PG_GETARG_FLOAT8(1), &result))
    float_ty_error(ERRCODE_INVALID_ARGUMENT_FOR_POWER_FUNCTION);

  PG_RETURN_FLOAT8(result);
}

/*
 *		dexp			- returns the exponential function of
 *arg1
 */
Datum dexp(PG_FUNCTION_ARGS) {
  float8 result;

  if (ty_float64_exp(PG_GETARG_FLOAT8(0), &result))
    float_raise_ty();

  PG_RETURN_FLOAT8(result);
}

/*
 *		dlog1			- returns the natural logarithm of arg1
 */
Datum dlog1(PG_FUNCTION_ARGS) {
  float8 result;

  /* the SQL standard asks for particular SQLSTATE codes for ln() */
  if (ty_float64_ln(PG_GETARG_FLOAT8(0), &result))
    float_ty_error(ERRCODE_INVALID_ARGUMENT_FOR_LOG);

  PG_RETURN_FLOAT8(result);
}

/*
 *		dlog10			- returns the base 10 logarithm of arg1
 */
Datum dlog10(PG_FUNCTION_ARGS) {
  float8 result;

  if (ty_float64_log10(PG_GETARG_FLOAT8(0), &result))
    float_ty_error(ERRCODE_INVALID_ARGUMENT_FOR_LOG);

  PG_RETURN_FLOAT8(result);
}

/*
 *		dacos			- returns the arccos of arg1 (radians)
 */
Datum dacos(PG_FUNCTION_ARGS) {
  float8 result;

  if (ty_float64_acos(PG_GETARG_FLOAT8(0), &result))
    float_ty_error(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE);

  PG_RETURN_FLOAT8(result);
}

/*
 *		dasin			- returns the arcsin of arg1 (radians)
 */
Datum dasin(PG_FUNCTION_ARGS) {
  float8 result;

  if (ty_float64_asin(PG_GETARG_FLOAT8(0), &result))
    float_ty_error(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE);

  PG_RETURN_FLOAT8(result);
}

/*
 *		datan			- returns the arctan of arg1 (radians)
 */
Datum datan(PG_FUNCTION_ARGS) {
  float8 result;

  if (ty_float64_atan(PG_GETARG_FLOAT8(0), &result))
    float_ty_error(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE);

  PG_RETURN_FLOAT8(result);
}

/*
 *		atan2			- returns the arctan of arg1/arg2
 *(radians)
 */
Datum datan2(PG_FUNCTION_ARGS) {
  float8 result;

  if (ty_float64_atan2(PG_GETARG_FLOAT8(0), PG_GETARG_FLOAT8(1), &result))
    float_ty_error(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE);

  PG_RETURN_FLOAT8(result);
}

/*
 *		dcos			- returns the cosine of arg1 (radians)
 */
Datum dcos(PG_FUNCTION_ARGS) {
  float8 result;

  if (ty_float64_cos(PG_GETARG_FLOAT8(0), &result))
    float_ty_error(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE);

  PG_RETURN_FLOAT8(result);
}

/*
 *		dcot			- returns the cotangent of arg1
 *(radians)
 */
Datum dcot(PG_FUNCTION_ARGS) {
  float8 result;

  if (ty_float64_cot(PG_GETARG_FLOAT8(0), &result))
    float_ty_error(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE);

  PG_RETURN_FLOAT8(result);
}

/*
 *		dsin			- returns the sine of arg1 (radians)
 */
Datum dsin(PG_FUNCTION_ARGS) {
  float8 result;

  if (ty_float64_sin(PG_GETARG_FLOAT8(0), &result))
    float_ty_error(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE);

  PG_RETURN_FLOAT8(result);
}

/*
 *		dtan			- returns the tangent of arg1 (radians)
 */
Datum dtan(PG_FUNCTION_ARGS) {
  float8 result;

  if (ty_float64_tan(PG_GETARG_FLOAT8(0), &result))
    float_ty_error(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE);

  PG_RETURN_FLOAT8(result);
}

/* ========== DEGREE-BASED TRIGONOMETRIC FUNCTIONS ========== */

/*
 *		dacosd			- returns the arccos of arg1 (degrees)
 */
Datum dacosd(PG_FUNCTION_ARGS) {
  float8 result;

  if (ty_float64_acosd(PG_GETARG_FLOAT8(0), &result))
    float_ty_error(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE);

  PG_RETURN_FLOAT8(result);
}

/*
 *		dasind			- returns the arcsin of arg1 (degrees)
 */
Datum dasind(PG_FUNCTION_ARGS) {
  float8 result;

  if (ty_float64_asind(PG_GETARG_FLOAT8(0), &result))
    float_ty_error(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE);

  PG_RETURN_FLOAT8(result);
}

/*
 *		datand			- returns the arctan of arg1 (degrees)
 */
Datum datand(PG_FUNCTION_ARGS) {
  float8 result;

  if (ty_float64_atand(PG_GETARG_FLOAT8(0), &result))
    float_ty_error(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE);

  PG_RETURN_FLOAT8(result);
}

/*
 *		atan2d			- returns the arctan of arg1/arg2
 *(degrees)
 */
Datum datan2d(PG_FUNCTION_ARGS) {
  float8 result;

  if (ty_float64_atan2d(PG_GETARG_FLOAT8(0), PG_GETARG_FLOAT8(1), &result))
    float_ty_error(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE);

  PG_RETURN_FLOAT8(result);
}

/*
 *		dcosd			- returns the cosine of arg1 (degrees)
 */
Datum dcosd(PG_FUNCTION_ARGS) {
  float8 result;

  if (ty_float64_cosd(PG_GETARG_FLOAT8(0), &result))
    float_ty_error(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE);

  PG_RETURN_FLOAT8(result);
}

/*
 *		dcotd			- returns the cotangent of arg1
 *(degrees)
 */
Datum dcotd(PG_FUNCTION_ARGS) {
  float8 result;

  if (ty_float64_cotd(PG_GETARG_FLOAT8(0), &result))
    float_ty_error(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE);

  PG_RETURN_FLOAT8(result);
}

/*
 *		dsind			- returns the sine of arg1 (degrees)
 */
Datum dsind(PG_FUNCTION_ARGS) {
  float8 result;

  if (ty_float64_sind(PG_GETARG_FLOAT8(0), &result))
    float_ty_error(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE);

  PG_RETURN_FLOAT8(result);
}

/*
 *		dtand			- returns the tangent of arg1 (degrees)
 */
Datum dtand(PG_FUNCTION_ARGS) {
  float8 result;

  if (ty_float64_tand(PG_GETARG_FLOAT8(0), &result))
    float_ty_error(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE);

  PG_RETURN_FLOAT8(result);
}

/*
 *		degrees		- returns degrees converted from radians
 */
Datum degrees(PG_FUNCTION_ARGS) {
  float8 result;

  if (ty_float64_degrees(PG_GETARG_FLOAT8(0), &result))
    float_raise_ty();

  PG_RETURN_FLOAT8(result);
}

/*
 *		dpi				- returns the constant PI
 */
Datum dpi(PG_FUNCTION_ARGS) { PG_RETURN_FLOAT8(ty_float64_pi()); }

/*
 *		radians			- converts degrees to radians
 */
Datum radians(PG_FUNCTION_ARGS) {
  float8 result;

  if (ty_float64_radians(PG_GETARG_FLOAT8(0), &result))
    float_raise_ty();

  PG_RETURN_FLOAT8(result);
}

/* ========== HYPERBOLIC FUNCTIONS ========== */

/*
 *		dsinh			- returns the hyperbolic sine of arg1
 */
Datum dsinh(PG_FUNCTION_ARGS) {
  float8 result;

  if (ty_float64_sinh(PG_GETARG_FLOAT8(0), &result))
    float_ty_error(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE);

  PG_RETURN_FLOAT8(result);
}

/*
 *		dcosh			- returns the hyperbolic cosine of arg1
 */
Datum dcosh(PG_FUNCTION_ARGS) {
  float8 result;

  if (ty_float64_cosh(PG_GETARG_FLOAT8(0), &result))
    float_ty_error(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE);

  PG_RETURN_FLOAT8(result);
}

/*
 *		dtanh			- returns the hyperbolic tangent of arg1
 */
Datum dtanh(PG_FUNCTION_ARGS) {
  float8 result;

  if (ty_float64_tanh(PG_GETARG_FLOAT8(0), &result))
    float_ty_error(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE);

  PG_RETURN_FLOAT8(result);
}

/*
 *		dasinh			- returns the inverse hyperbolic sine of
 *arg1
 */
Datum dasinh(PG_FUNCTION_ARGS) {
  float8 result;

  if (ty_float64_asinh(PG_GETARG_FLOAT8(0), &result))
    float_ty_error(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE);

  PG_RETURN_FLOAT8(result);
}

/*
 *		dacosh			- returns the inverse hyperbolic cosine
 *of arg1
 */
Datum dacosh(PG_FUNCTION_ARGS) {
  float8 result;

  if (ty_float64_acosh(PG_GETARG_FLOAT8(0), &result))
    float_ty_error(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE);

  PG_RETURN_FLOAT8(result);
}

/*
 *		datanh			- returns the inverse hyperbolic tangent
 *of arg1
 */
Datum datanh(PG_FUNCTION_ARGS) {
  float8 result;

  if (ty_float64_atanh(PG_GETARG_FLOAT8(0), &result))
    float_ty_error(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE);

  PG_RETURN_FLOAT8(result);
}

/* ========== ERROR FUNCTIONS ========== */

/*
 *		derf			- returns the error function: erf(arg1)
 */
Datum derf(PG_FUNCTION_ARGS) {
  float8 result;

  if (ty_float64_erf(PG_GETARG_FLOAT8(0), &result))
    float_ty_error(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE);

  PG_RETURN_FLOAT8(result);
}

/*
 *		derfc			- returns the complementary error
 *function: 1 - erf(arg1)
 */
Datum derfc(PG_FUNCTION_ARGS) {
  float8 result;

  if (ty_float64_erfc(PG_GETARG_FLOAT8(0), &result))
    float_ty_error(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE);

  PG_RETURN_FLOAT8(result);
}

/* ========== GAMMA FUNCTIONS ========== */

/*
 *		dgamma			- returns the gamma function of arg1
 */
Datum dgamma(PG_FUNCTION_ARGS) {
  float8 result;

  if (ty_float64_gamma(PG_GETARG_FLOAT8(0), &result))
    float_ty_error(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE);

  PG_RETURN_FLOAT8(result);
}

/*
 *		dlgamma			- natural logarithm of absolute value of
 *gamma of arg1
 */
Datum dlgamma(PG_FUNCTION_ARGS) {
  float8 result;

  if (ty_float64_lgamma(PG_GETARG_FLOAT8(0), &result))
    float_ty_error(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE);

  PG_RETURN_FLOAT8(result);
}

/*
 *		=========================
 *		FLOAT AGGREGATE OPERATORS
 *		=========================
 *
 *		float8_accum		- accumulate for AVG(), variance
 *aggregates, etc. float4_accum		- same, but input data is float4
 *		float8_avg			- produce final result for float
 *AVG() float8_var_samp		- produce final result for float VAR_SAMP()
 *		float8_var_pop		- produce final result for float
 *VAR_POP() float8_stddev_samp	- produce final result for float STDDEV_SAMP()
 *		float8_stddev_pop	- produce final result for float
 *STDDEV_POP()
 *
 * The naive schoolbook implementation of these aggregates works by
 * accumulating sum(X) and sum(X^2).  However, this approach suffers from
 * large rounding errors in the final computation of quantities like the
 * population variance (N*sum(X^2) - sum(X)^2) / N^2, since each of the
 * intermediate terms is potentially very large, while the difference is often
 * quite small.
 *
 * Instead we use the Youngs-Cramer algorithm [1] which works by accumulating
 * Sx=sum(X) and Sxx=sum((X-Sx/N)^2), using a numerically stable algorithm to
 * incrementally update those quantities.  The final computations of each of
 * the aggregate values is then trivial and gives more accurate results (for
 * example, the population variance is just Sxx/N).  This algorithm is also
 * fairly easy to generalize to allow parallel execution without loss of
 * precision (see, for example, [2]).  For more details, and a comparison of
 * this with other algorithms, see [3].
 *
 * The transition datatype for all these aggregates is a 3-element array
 * of float8, holding the values N, Sx, Sxx in that order.
 *
 * Note that we represent N as a float to avoid having to build a special
 * datatype.  Given a reasonable floating-point implementation, there should
 * be no accuracy loss unless N exceeds 2 ^ 52 or so (by which time the
 * user will have doubtless lost interest anyway...)
 *
 * [1] Some Results Relevant to Choice of Sum and Sum-of-Product Algorithms,
 * E. A. Youngs and E. M. Cramer, Technometrics Vol 13, No 3, August 1971.
 *
 * [2] Updating Formulae and a Pairwise Algorithm for Computing Sample
 * Variances, T. F. Chan, G. H. Golub & R. J. LeVeque, COMPSTAT 1982.
 *
 * [3] Numerically Stable Parallel Computation of (Co-)Variance, Erich
 * Schubert and Michael Gertz, Proceedings of the 30th International
 * Conference on Scientific and Statistical Database Management, 2018.
 */

static float8 *check_float8_array(ArrayType *transarray, const char *caller,
                                  int n) {
  /*
   * We expect the input to be an N-element float array; verify that. We
   * don't need to use deconstruct_array() since the array data is just
   * going to look like a C array of N float8 values.
   */
  if (ARR_NDIM(transarray) != 1 || ARR_DIMS(transarray)[0] != n ||
      ARR_HASNULL(transarray) || ARR_ELEMTYPE(transarray) != FLOAT8OID)
    elog(ERROR, "%s: expected %d-element float8 array", caller, n);
  return (float8 *)ARR_DATA_PTR(transarray);
}

/*
 * Put a variance state back: into the transition array in place, if we are
 * an aggregate's step and so may change it, else into a new array.
 */
static Datum float_var_store(FunctionCallInfo fcinfo, ArrayType *transarray,
                             float8 *tv, const ty_float_var_t *st) {
  if (AggCheckCallContext(fcinfo, NULL)) {
    tv[0] = st->n;
    tv[1] = st->sx;
    tv[2] = st->sxx;

    PG_RETURN_ARRAYTYPE_P(transarray);
  } else {
    Datum transdatums[3];
    ArrayType *result;

    transdatums[0] = Float8GetDatumFast(st->n);
    transdatums[1] = Float8GetDatumFast(st->sx);
    transdatums[2] = Float8GetDatumFast(st->sxx);

    result = construct_array_builtin(transdatums, 3, FLOAT8OID);

    PG_RETURN_ARRAYTYPE_P(result);
  }
}

/* The same for a regression state of six values. */
static Datum float_regr_store(FunctionCallInfo fcinfo, ArrayType *transarray,
                              float8 *tv, const ty_float_regr_t *st) {
  if (AggCheckCallContext(fcinfo, NULL)) {
    tv[0] = st->n;
    tv[1] = st->sx;
    tv[2] = st->sxx;
    tv[3] = st->sy;
    tv[4] = st->syy;
    tv[5] = st->sxy;

    PG_RETURN_ARRAYTYPE_P(transarray);
  } else {
    Datum transdatums[6];
    ArrayType *result;

    transdatums[0] = Float8GetDatumFast(st->n);
    transdatums[1] = Float8GetDatumFast(st->sx);
    transdatums[2] = Float8GetDatumFast(st->sxx);
    transdatums[3] = Float8GetDatumFast(st->sy);
    transdatums[4] = Float8GetDatumFast(st->syy);
    transdatums[5] = Float8GetDatumFast(st->sxy);

    result = construct_array_builtin(transdatums, 6, FLOAT8OID);

    PG_RETURN_ARRAYTYPE_P(result);
  }
}


/*
 * float8_combine
 *
 * An aggregate combine function used to combine two 3 fields
 * aggregate transition data into a single transition data.
 * This function is used only in two stage aggregation and
 * shouldn't be called outside aggregate context.
 */
Datum float8_combine(PG_FUNCTION_ARGS) {
  ArrayType *transarray1 = PG_GETARG_ARRAYTYPE_P(0);
  ArrayType *transarray2 = PG_GETARG_ARRAYTYPE_P(1);
  float8 *transvalues1 = check_float8_array(transarray1, "float8_combine", 3);
  float8 *transvalues2 = check_float8_array(transarray2, "float8_combine", 3);
  ty_float_var_t a = {transvalues1[0], transvalues1[1], transvalues1[2]};
  ty_float_var_t b = {transvalues2[0], transvalues2[1], transvalues2[2]};

  if (ty_float_var_combine(&a, &b))
    float_raise_ty();

  return float_var_store(fcinfo, transarray1, transvalues1, &a);
}

Datum float8_accum(PG_FUNCTION_ARGS) {
  ArrayType *transarray = PG_GETARG_ARRAYTYPE_P(0);
  float8 *transvalues = check_float8_array(transarray, "float8_accum", 3);
  ty_float_var_t st = {transvalues[0], transvalues[1], transvalues[2]};

  /* ty adds the value by the Youngs-Cramer method */
  if (ty_float_var_accum(&st, PG_GETARG_FLOAT8(1)))
    float_raise_ty();

  return float_var_store(fcinfo, transarray, transvalues, &st);
}

Datum float4_accum(PG_FUNCTION_ARGS) {
  ArrayType *transarray = PG_GETARG_ARRAYTYPE_P(0);
  float8 *transvalues = check_float8_array(transarray, "float4_accum", 3);
  ty_float_var_t st = {transvalues[0], transvalues[1], transvalues[2]};

  /* ty adds the value by the Youngs-Cramer method */
  if (ty_float_var_accum(&st, PG_GETARG_FLOAT4(1)))
    float_raise_ty();

  return float_var_store(fcinfo, transarray, transvalues, &st);
}

Datum float8_avg(PG_FUNCTION_ARGS) {
  ArrayType *transarray = PG_GETARG_ARRAYTYPE_P(0);
  float8 *transvalues = check_float8_array(transarray, "float8_avg", 3);
  ty_float_var_t st = {transvalues[0], transvalues[1], transvalues[2]};
  float8 result;

  /* ty returns 1 where SQL's result is NULL */
  if (ty_float_var_avg(&st, &result))
    PG_RETURN_NULL();

  PG_RETURN_FLOAT8(result);
}

Datum float8_var_pop(PG_FUNCTION_ARGS) {
  ArrayType *transarray = PG_GETARG_ARRAYTYPE_P(0);
  float8 *transvalues = check_float8_array(transarray, "float8_var_pop", 3);
  ty_float_var_t st = {transvalues[0], transvalues[1], transvalues[2]};
  float8 result;

  /* ty returns 1 where SQL's result is NULL */
  if (ty_float_var_pop(&st, &result))
    PG_RETURN_NULL();

  PG_RETURN_FLOAT8(result);
}

Datum float8_var_samp(PG_FUNCTION_ARGS) {
  ArrayType *transarray = PG_GETARG_ARRAYTYPE_P(0);
  float8 *transvalues = check_float8_array(transarray, "float8_var_samp", 3);
  ty_float_var_t st = {transvalues[0], transvalues[1], transvalues[2]};
  float8 result;

  /* ty returns 1 where SQL's result is NULL */
  if (ty_float_var_samp(&st, &result))
    PG_RETURN_NULL();

  PG_RETURN_FLOAT8(result);
}

Datum float8_stddev_pop(PG_FUNCTION_ARGS) {
  ArrayType *transarray = PG_GETARG_ARRAYTYPE_P(0);
  float8 *transvalues = check_float8_array(transarray, "float8_stddev_pop", 3);
  ty_float_var_t st = {transvalues[0], transvalues[1], transvalues[2]};
  float8 result;

  /* ty returns 1 where SQL's result is NULL */
  if (ty_float_stddev_pop(&st, &result))
    PG_RETURN_NULL();

  PG_RETURN_FLOAT8(result);
}

Datum float8_stddev_samp(PG_FUNCTION_ARGS) {
  ArrayType *transarray = PG_GETARG_ARRAYTYPE_P(0);
  float8 *transvalues = check_float8_array(transarray, "float8_stddev_samp", 3);
  ty_float_var_t st = {transvalues[0], transvalues[1], transvalues[2]};
  float8 result;

  /* ty returns 1 where SQL's result is NULL */
  if (ty_float_stddev_samp(&st, &result))
    PG_RETURN_NULL();

  PG_RETURN_FLOAT8(result);
}

/*
 *		=========================
 *		SQL2003 BINARY AGGREGATES
 *		=========================
 *
 * As with the preceding aggregates, we use the Youngs-Cramer algorithm to
 * reduce rounding errors in the aggregate final functions.
 *
 * The transition datatype for all these aggregates is a 6-element array of
 * float8, holding the values N, Sx=sum(X), Sxx=sum((X-Sx/N)^2), Sy=sum(Y),
 * Syy=sum((Y-Sy/N)^2), Sxy=sum((X-Sx/N)*(Y-Sy/N)) in that order.
 *
 * Note that Y is the first argument to all these aggregates!
 *
 * It might seem attractive to optimize this by having multiple accumulator
 * functions that only calculate the sums actually needed.  But on most
 * modern machines, a couple of extra floating-point multiplies will be
 * insignificant compared to the other per-tuple overhead, so I've chosen
 * to minimize code space instead.
 */

Datum float8_regr_accum(PG_FUNCTION_ARGS) {
  ArrayType *transarray = PG_GETARG_ARRAYTYPE_P(0);
  float8 *tv = check_float8_array(transarray, "float8_regr_accum", 6);
  ty_float_regr_t st = {tv[0], tv[1], tv[2], tv[3], tv[4], tv[5]};

  if (ty_float_regr_accum(&st, PG_GETARG_FLOAT8(1), PG_GETARG_FLOAT8(2)))
    float_raise_ty();

  return float_regr_store(fcinfo, transarray, tv, &st);
}

/*
 * float8_regr_combine
 *
 * An aggregate combine function used to combine two 6 fields
 * aggregate transition data into a single transition data.
 * This function is used only in two stage aggregation and
 * shouldn't be called outside aggregate context.
 */
Datum float8_regr_combine(PG_FUNCTION_ARGS) {
  ArrayType *transarray1 = PG_GETARG_ARRAYTYPE_P(0);
  ArrayType *transarray2 = PG_GETARG_ARRAYTYPE_P(1);
  float8 *t1 = check_float8_array(transarray1, "float8_regr_combine", 6);
  float8 *t2 = check_float8_array(transarray2, "float8_regr_combine", 6);
  ty_float_regr_t a = {t1[0], t1[1], t1[2], t1[3], t1[4], t1[5]};
  ty_float_regr_t b = {t2[0], t2[1], t2[2], t2[3], t2[4], t2[5]};

  if (ty_float_regr_combine(&a, &b))
    float_raise_ty();

  return float_regr_store(fcinfo, transarray1, t1, &a);
}

Datum float8_regr_sxx(PG_FUNCTION_ARGS) {
  ArrayType *transarray = PG_GETARG_ARRAYTYPE_P(0);
  float8 *tv = check_float8_array(transarray, "float8_regr_sxx", 6);
  ty_float_regr_t st = {tv[0], tv[1], tv[2], tv[3], tv[4], tv[5]};
  float8 result;

  /* ty returns 1 where SQL's result is NULL */
  if (ty_float_regr_sxx(&st, &result))
    PG_RETURN_NULL();

  PG_RETURN_FLOAT8(result);
}

Datum float8_regr_syy(PG_FUNCTION_ARGS) {
  ArrayType *transarray = PG_GETARG_ARRAYTYPE_P(0);
  float8 *tv = check_float8_array(transarray, "float8_regr_syy", 6);
  ty_float_regr_t st = {tv[0], tv[1], tv[2], tv[3], tv[4], tv[5]};
  float8 result;

  /* ty returns 1 where SQL's result is NULL */
  if (ty_float_regr_syy(&st, &result))
    PG_RETURN_NULL();

  PG_RETURN_FLOAT8(result);
}

Datum float8_regr_sxy(PG_FUNCTION_ARGS) {
  ArrayType *transarray = PG_GETARG_ARRAYTYPE_P(0);
  float8 *tv = check_float8_array(transarray, "float8_regr_sxy", 6);
  ty_float_regr_t st = {tv[0], tv[1], tv[2], tv[3], tv[4], tv[5]};
  float8 result;

  /* ty returns 1 where SQL's result is NULL */
  if (ty_float_regr_sxy(&st, &result))
    PG_RETURN_NULL();

  PG_RETURN_FLOAT8(result);
}

Datum float8_regr_avgx(PG_FUNCTION_ARGS) {
  ArrayType *transarray = PG_GETARG_ARRAYTYPE_P(0);
  float8 *tv = check_float8_array(transarray, "float8_regr_avgx", 6);
  ty_float_regr_t st = {tv[0], tv[1], tv[2], tv[3], tv[4], tv[5]};
  float8 result;

  /* ty returns 1 where SQL's result is NULL */
  if (ty_float_regr_avgx(&st, &result))
    PG_RETURN_NULL();

  PG_RETURN_FLOAT8(result);
}

Datum float8_regr_avgy(PG_FUNCTION_ARGS) {
  ArrayType *transarray = PG_GETARG_ARRAYTYPE_P(0);
  float8 *tv = check_float8_array(transarray, "float8_regr_avgy", 6);
  ty_float_regr_t st = {tv[0], tv[1], tv[2], tv[3], tv[4], tv[5]};
  float8 result;

  /* ty returns 1 where SQL's result is NULL */
  if (ty_float_regr_avgy(&st, &result))
    PG_RETURN_NULL();

  PG_RETURN_FLOAT8(result);
}

Datum float8_covar_pop(PG_FUNCTION_ARGS) {
  ArrayType *transarray = PG_GETARG_ARRAYTYPE_P(0);
  float8 *tv = check_float8_array(transarray, "float8_covar_pop", 6);
  ty_float_regr_t st = {tv[0], tv[1], tv[2], tv[3], tv[4], tv[5]};
  float8 result;

  /* ty returns 1 where SQL's result is NULL */
  if (ty_float_covar_pop(&st, &result))
    PG_RETURN_NULL();

  PG_RETURN_FLOAT8(result);
}

Datum float8_covar_samp(PG_FUNCTION_ARGS) {
  ArrayType *transarray = PG_GETARG_ARRAYTYPE_P(0);
  float8 *tv = check_float8_array(transarray, "float8_covar_samp", 6);
  ty_float_regr_t st = {tv[0], tv[1], tv[2], tv[3], tv[4], tv[5]};
  float8 result;

  /* ty returns 1 where SQL's result is NULL */
  if (ty_float_covar_samp(&st, &result))
    PG_RETURN_NULL();

  PG_RETURN_FLOAT8(result);
}

Datum float8_corr(PG_FUNCTION_ARGS) {
  ArrayType *transarray = PG_GETARG_ARRAYTYPE_P(0);
  float8 *tv = check_float8_array(transarray, "float8_corr", 6);
  ty_float_regr_t st = {tv[0], tv[1], tv[2], tv[3], tv[4], tv[5]};
  float8 result;

  /* ty returns 1 where SQL's result is NULL */
  if (ty_float_corr(&st, &result))
    PG_RETURN_NULL();

  PG_RETURN_FLOAT8(result);
}

Datum float8_regr_r2(PG_FUNCTION_ARGS) {
  ArrayType *transarray = PG_GETARG_ARRAYTYPE_P(0);
  float8 *tv = check_float8_array(transarray, "float8_regr_r2", 6);
  ty_float_regr_t st = {tv[0], tv[1], tv[2], tv[3], tv[4], tv[5]};
  float8 result;

  /* ty returns 1 where SQL's result is NULL */
  if (ty_float_regr_r2(&st, &result))
    PG_RETURN_NULL();

  PG_RETURN_FLOAT8(result);
}

Datum float8_regr_slope(PG_FUNCTION_ARGS) {
  ArrayType *transarray = PG_GETARG_ARRAYTYPE_P(0);
  float8 *tv = check_float8_array(transarray, "float8_regr_slope", 6);
  ty_float_regr_t st = {tv[0], tv[1], tv[2], tv[3], tv[4], tv[5]};
  float8 result;

  /* ty returns 1 where SQL's result is NULL */
  if (ty_float_regr_slope(&st, &result))
    PG_RETURN_NULL();

  PG_RETURN_FLOAT8(result);
}

Datum float8_regr_intercept(PG_FUNCTION_ARGS) {
  ArrayType *transarray = PG_GETARG_ARRAYTYPE_P(0);
  float8 *tv = check_float8_array(transarray, "float8_regr_intercept", 6);
  ty_float_regr_t st = {tv[0], tv[1], tv[2], tv[3], tv[4], tv[5]};
  float8 result;

  /* ty returns 1 where SQL's result is NULL */
  if (ty_float_regr_intercept(&st, &result))
    PG_RETURN_NULL();

  PG_RETURN_FLOAT8(result);
}

/*
 *		====================================
 *		MIXED-PRECISION ARITHMETIC OPERATORS
 *		====================================
 */

/*
 *		float48pl		- returns arg1 + arg2
 *		float48mi		- returns arg1 - arg2
 *		float48mul		- returns arg1 * arg2
 *		float48div		- returns arg1 / arg2
 */
Datum float48pl(PG_FUNCTION_ARGS) {
  float4 arg1 = PG_GETARG_FLOAT4(0);
  float8 arg2 = PG_GETARG_FLOAT8(1);

  PG_RETURN_FLOAT8(float8_pl((float8)arg1, arg2));
}

Datum float48mi(PG_FUNCTION_ARGS) {
  float4 arg1 = PG_GETARG_FLOAT4(0);
  float8 arg2 = PG_GETARG_FLOAT8(1);

  PG_RETURN_FLOAT8(float8_mi((float8)arg1, arg2));
}

Datum float48mul(PG_FUNCTION_ARGS) {
  float4 arg1 = PG_GETARG_FLOAT4(0);
  float8 arg2 = PG_GETARG_FLOAT8(1);

  PG_RETURN_FLOAT8(float8_mul((float8)arg1, arg2));
}

Datum float48div(PG_FUNCTION_ARGS) {
  float4 arg1 = PG_GETARG_FLOAT4(0);
  float8 arg2 = PG_GETARG_FLOAT8(1);

  PG_RETURN_FLOAT8(float8_div((float8)arg1, arg2));
}

/*
 *		float84pl		- returns arg1 + arg2
 *		float84mi		- returns arg1 - arg2
 *		float84mul		- returns arg1 * arg2
 *		float84div		- returns arg1 / arg2
 */
Datum float84pl(PG_FUNCTION_ARGS) {
  float8 arg1 = PG_GETARG_FLOAT8(0);
  float4 arg2 = PG_GETARG_FLOAT4(1);

  PG_RETURN_FLOAT8(float8_pl(arg1, (float8)arg2));
}

Datum float84mi(PG_FUNCTION_ARGS) {
  float8 arg1 = PG_GETARG_FLOAT8(0);
  float4 arg2 = PG_GETARG_FLOAT4(1);

  PG_RETURN_FLOAT8(float8_mi(arg1, (float8)arg2));
}

Datum float84mul(PG_FUNCTION_ARGS) {
  float8 arg1 = PG_GETARG_FLOAT8(0);
  float4 arg2 = PG_GETARG_FLOAT4(1);

  PG_RETURN_FLOAT8(float8_mul(arg1, (float8)arg2));
}

Datum float84div(PG_FUNCTION_ARGS) {
  float8 arg1 = PG_GETARG_FLOAT8(0);
  float4 arg2 = PG_GETARG_FLOAT4(1);

  PG_RETURN_FLOAT8(float8_div(arg1, (float8)arg2));
}

/*
 *		====================
 *		COMPARISON OPERATORS
 *		====================
 */

/*
 *		float48{eq,ne,lt,le,gt,ge}		- float4/float8
 *comparison operations
 */
Datum float48eq(PG_FUNCTION_ARGS) {
  float4 arg1 = PG_GETARG_FLOAT4(0);
  float8 arg2 = PG_GETARG_FLOAT8(1);

  PG_RETURN_BOOL(float8_eq((float8)arg1, arg2));
}

Datum float48ne(PG_FUNCTION_ARGS) {
  float4 arg1 = PG_GETARG_FLOAT4(0);
  float8 arg2 = PG_GETARG_FLOAT8(1);

  PG_RETURN_BOOL(float8_ne((float8)arg1, arg2));
}

Datum float48lt(PG_FUNCTION_ARGS) {
  float4 arg1 = PG_GETARG_FLOAT4(0);
  float8 arg2 = PG_GETARG_FLOAT8(1);

  PG_RETURN_BOOL(float8_lt((float8)arg1, arg2));
}

Datum float48le(PG_FUNCTION_ARGS) {
  float4 arg1 = PG_GETARG_FLOAT4(0);
  float8 arg2 = PG_GETARG_FLOAT8(1);

  PG_RETURN_BOOL(float8_le((float8)arg1, arg2));
}

Datum float48gt(PG_FUNCTION_ARGS) {
  float4 arg1 = PG_GETARG_FLOAT4(0);
  float8 arg2 = PG_GETARG_FLOAT8(1);

  PG_RETURN_BOOL(float8_gt((float8)arg1, arg2));
}

Datum float48ge(PG_FUNCTION_ARGS) {
  float4 arg1 = PG_GETARG_FLOAT4(0);
  float8 arg2 = PG_GETARG_FLOAT8(1);

  PG_RETURN_BOOL(float8_ge((float8)arg1, arg2));
}

/*
 *		float84{eq,ne,lt,le,gt,ge}		- float8/float4
 *comparison operations
 */
Datum float84eq(PG_FUNCTION_ARGS) {
  float8 arg1 = PG_GETARG_FLOAT8(0);
  float4 arg2 = PG_GETARG_FLOAT4(1);

  PG_RETURN_BOOL(float8_eq(arg1, (float8)arg2));
}

Datum float84ne(PG_FUNCTION_ARGS) {
  float8 arg1 = PG_GETARG_FLOAT8(0);
  float4 arg2 = PG_GETARG_FLOAT4(1);

  PG_RETURN_BOOL(float8_ne(arg1, (float8)arg2));
}

Datum float84lt(PG_FUNCTION_ARGS) {
  float8 arg1 = PG_GETARG_FLOAT8(0);
  float4 arg2 = PG_GETARG_FLOAT4(1);

  PG_RETURN_BOOL(float8_lt(arg1, (float8)arg2));
}

Datum float84le(PG_FUNCTION_ARGS) {
  float8 arg1 = PG_GETARG_FLOAT8(0);
  float4 arg2 = PG_GETARG_FLOAT4(1);

  PG_RETURN_BOOL(float8_le(arg1, (float8)arg2));
}

Datum float84gt(PG_FUNCTION_ARGS) {
  float8 arg1 = PG_GETARG_FLOAT8(0);
  float4 arg2 = PG_GETARG_FLOAT4(1);

  PG_RETURN_BOOL(float8_gt(arg1, (float8)arg2));
}

Datum float84ge(PG_FUNCTION_ARGS) {
  float8 arg1 = PG_GETARG_FLOAT8(0);
  float4 arg2 = PG_GETARG_FLOAT4(1);

  PG_RETURN_BOOL(float8_ge(arg1, (float8)arg2));
}

/*
 * Implements the float8 version of the width_bucket() function
 * defined by SQL2003. See also width_bucket_numeric().
 *
 * 'bound1' and 'bound2' are the lower and upper bounds of the
 * histogram's range, respectively. 'count' is the number of buckets
 * in the histogram. width_bucket() returns an integer indicating the
 * bucket number that 'operand' belongs to in an equiwidth histogram
 * with the specified characteristics. An operand smaller than the
 * lower bound is assigned to bucket 0. An operand greater than or equal
 * to the upper bound is assigned to an additional bucket (with number
 * count+1). We don't allow "NaN" for any of the float8 inputs, and we
 * don't allow either of the histogram bounds to be +/- infinity.
 */
Datum width_bucket_float8(PG_FUNCTION_ARGS) {
  int32 result;

  if (ty_float64_width_bucket(PG_GETARG_FLOAT8(0), PG_GETARG_FLOAT8(1),
                              PG_GETARG_FLOAT8(2), PG_GETARG_INT32(3),
                              &result)) {
    if (ty_errnum() == TY_ERR_INVALID)
      ereport(ERROR,
              (errcode(ERRCODE_INVALID_ARGUMENT_FOR_WIDTH_BUCKET_FUNCTION),
               errmsg("%s", ty_errmsg())));
    ereport(ERROR, (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
                    errmsg("integer out of range")));
  }

  PG_RETURN_INT32(result);
}

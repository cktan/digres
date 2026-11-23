/*-------------------------------------------------------------------------
 *
 * date.c
 *	  implements DATE and TIME data types specified in SQL standard
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994-5, Regents of the University of California
 * Portions Copyright (c) 2026, CK Tan: the changes for digres,
 * under the MIT License (https://github.com/cktan/digres/blob/main/LICENSE)
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/date.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <ctype.h>
#include <limits.h>
#include <float.h>
#include <math.h>
#include <time.h>

#include "access/xact.h"
#include "catalog/pg_type.h"
#include "common/hashfn.h"
#include "common/int.h"
#include "libpq/pqformat.h"
#include "miscadmin.h"
#include "nodes/supportnodes.h"
#include "parser/scansup.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/date.h"
#include "ty_datetime.h"
#include "ty_timezone.h"
#include "utils/datetime.h"
#include "utils/numeric.h"
#include "utils/skipsupport.h"
#include "utils/sortsupport.h"

/*
 * gcc's -ffast-math switch breaks routines that expect exact results from
 * expressions like timeval / SECS_PER_HOUR, where timeval is double.
 */
#ifdef __FAST_MATH__
#error -ffast-math is known to break this code
#endif

/* common code for timetypmodin and timetztypmodin */
static int32 anytime_typmodin(bool istz, ArrayType *ta) {
  int32 *tl;
  int n;

  tl = ArrayGetIntegerTypmods(ta, &n);

  /*
   * we're not too tense about good error message here because grammar
   * shouldn't allow wrong number of modifiers for TIME
   */
  if (n != 1)
    ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                    errmsg("invalid type modifier")));

  return anytime_typmod_check(istz, tl[0]);
}

/* exported so parse_expr.c can use it */
int32 anytime_typmod_check(bool istz, int32 typmod) {
  if (typmod < 0)
    ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                    errmsg("TIME(%d)%s precision must not be negative", typmod,
                           (istz ? " WITH TIME ZONE" : ""))));
  if (typmod > MAX_TIME_PRECISION) {
    ereport(
        WARNING,
        (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
         errmsg("TIME(%d)%s precision reduced to maximum allowed, %d", typmod,
                (istz ? " WITH TIME ZONE" : ""), MAX_TIME_PRECISION)));
    typmod = MAX_TIME_PRECISION;
  }

  return typmod;
}

/* common code for timetypmodout and timetztypmodout */
static char *anytime_typmodout(bool istz, int32 typmod) {
  const char *tz = istz ? " with time zone" : " without time zone";

  if (typmod >= 0)
    return psprintf("(%d)%s", (int)typmod, tz);
  else
    return pstrdup(tz);
}

/*****************************************************************************
 *	 Date ADT
 *****************************************************************************/

/* date_in()
 * Given date text string, convert to internal date format.
 */
Datum date_in(PG_FUNCTION_ARGS) {
  char *str = PG_GETARG_CSTRING(0);
  Node *escontext = fcinfo->context;
  DateADT date;
  int dterr;
  ty_dtin_extra_t extra;

  /* ty reads the text; the zone words and the clock are the hooks' */
  if (ty_date_parse(str, DateOrder, &ty_dtzone_hooks, &date, &dterr, &extra)) {
    if (dterr != 0) {
      TyDateTimeParseError(dterr, &extra, str, "date", escontext);
      PG_RETURN_NULL();
    }
    ereturn(escontext, (Datum)0,
            (errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
             errmsg("date out of range: \"%s\"", str)));
  }

  PG_RETURN_DATEADT(date);
}

/* date_out()
 * Given internal format date, convert to text string.
 */
Datum date_out(PG_FUNCTION_ARGS) {
  DateADT date = PG_GETARG_DATEADT(0);
  char *result;
  struct pg_tm tt, *tm = &tt;
  char buf[MAXDATELEN + 1];

  if (DATE_NOT_FINITE(date))
    EncodeSpecialDate(date, buf);
  else {
    j2date(date + POSTGRES_EPOCH_JDATE, &(tm->tm_year), &(tm->tm_mon),
           &(tm->tm_mday));
    EncodeDateOnly(tm, DateStyle, buf);
  }

  result = pstrdup(buf);
  PG_RETURN_CSTRING(result);
}

/*
 *		date_recv			- converts external binary
 *format to date
 */
Datum date_recv(PG_FUNCTION_ARGS) {
  StringInfo buf = (StringInfo)PG_GETARG_POINTER(0);
  DateADT result;

  result = (DateADT)pq_getmsgint(buf, sizeof(DateADT));

  /* Limit to the same range that date_in() accepts. */
  if (DATE_NOT_FINITE(result))
    /* ok */;
  else if (!IS_VALID_DATE(result))
    ereport(ERROR, (errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
                    errmsg("date out of range")));

  PG_RETURN_DATEADT(result);
}

/*
 *		date_send			- converts date to binary format
 */
Datum date_send(PG_FUNCTION_ARGS) {
  DateADT date = PG_GETARG_DATEADT(0);
  StringInfoData buf;

  pq_begintypsend(&buf);
  pq_sendint32(&buf, date);
  PG_RETURN_BYTEA_P(pq_endtypsend(&buf));
}

/*
 * Raise the error of a failed ty date or time call: its message is PostgreSQL's,
 * and the code says which SQLSTATE.
 */
static pg_noreturn void datetime_ty_error(void) {
  ereport(ERROR,
          (errcode(ty_errnum() == TY_ERR_INVALID
                       ? ERRCODE_DATETIME_FIELD_OVERFLOW
                       : ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
           errmsg("%s", ty_errmsg())));
}


/*
 *		make_date			- date constructor
 */
Datum make_date(PG_FUNCTION_ARGS) {
  DateADT date;

  /* ty checks the fields and the range, with PostgreSQL's messages */
  if (ty_date_make(PG_GETARG_INT32(0), PG_GETARG_INT32(1), PG_GETARG_INT32(2),
                   &date))
    datetime_ty_error();

  PG_RETURN_DATEADT(date);
}

/*
 * Convert reserved date values to string.
 */
void EncodeSpecialDate(DateADT dt, char *str) {
  if (DATE_IS_NOBEGIN(dt))
    strcpy(str, EARLY);
  else if (DATE_IS_NOEND(dt))
    strcpy(str, LATE);
  else /* shouldn't happen */
    elog(ERROR, "invalid argument for EncodeSpecialDate");
}

/*
 * GetSQLCurrentDate -- implements CURRENT_DATE
 */
DateADT GetSQLCurrentDate(void) {
  int32 date;

  /* ty has the time of the transaction in the session's zone, kept per row */
  if (ty_current_date(&date))
    datetime_ty_error();
  return date;
}

/*
 * GetSQLCurrentTime -- implements CURRENT_TIME, CURRENT_TIME(n)
 */
TimeTzADT *GetSQLCurrentTime(int32 typmod) {
  TimeTzADT *result = (TimeTzADT *)palloc(sizeof(TimeTzADT));

  if (ty_current_time(typmod, (ty_timetz_t *)result))
    datetime_ty_error();
  return result;
}

/*
 * GetSQLLocalTime -- implements LOCALTIME, LOCALTIME(n)
 */
TimeADT GetSQLLocalTime(int32 typmod) {
  int64 result;

  if (ty_local_time(typmod, &result))
    datetime_ty_error();
  return result;
}

/*
 * Comparison functions for dates
 */

Datum date_eq(PG_FUNCTION_ARGS) {
  DateADT dateVal1 = PG_GETARG_DATEADT(0);
  DateADT dateVal2 = PG_GETARG_DATEADT(1);

  PG_RETURN_BOOL(dateVal1 == dateVal2);
}

Datum date_ne(PG_FUNCTION_ARGS) {
  DateADT dateVal1 = PG_GETARG_DATEADT(0);
  DateADT dateVal2 = PG_GETARG_DATEADT(1);

  PG_RETURN_BOOL(dateVal1 != dateVal2);
}

Datum date_lt(PG_FUNCTION_ARGS) {
  DateADT dateVal1 = PG_GETARG_DATEADT(0);
  DateADT dateVal2 = PG_GETARG_DATEADT(1);

  PG_RETURN_BOOL(dateVal1 < dateVal2);
}

Datum date_le(PG_FUNCTION_ARGS) {
  DateADT dateVal1 = PG_GETARG_DATEADT(0);
  DateADT dateVal2 = PG_GETARG_DATEADT(1);

  PG_RETURN_BOOL(dateVal1 <= dateVal2);
}

Datum date_gt(PG_FUNCTION_ARGS) {
  DateADT dateVal1 = PG_GETARG_DATEADT(0);
  DateADT dateVal2 = PG_GETARG_DATEADT(1);

  PG_RETURN_BOOL(dateVal1 > dateVal2);
}

Datum date_ge(PG_FUNCTION_ARGS) {
  DateADT dateVal1 = PG_GETARG_DATEADT(0);
  DateADT dateVal2 = PG_GETARG_DATEADT(1);

  PG_RETURN_BOOL(dateVal1 >= dateVal2);
}

Datum date_cmp(PG_FUNCTION_ARGS) {
  DateADT dateVal1 = PG_GETARG_DATEADT(0);
  DateADT dateVal2 = PG_GETARG_DATEADT(1);

  if (dateVal1 < dateVal2)
    PG_RETURN_INT32(-1);
  else if (dateVal1 > dateVal2)
    PG_RETURN_INT32(1);
  PG_RETURN_INT32(0);
}

Datum date_sortsupport(PG_FUNCTION_ARGS) {
  SortSupport ssup = (SortSupport)PG_GETARG_POINTER(0);

  ssup->comparator = ssup_datum_int32_cmp;
  PG_RETURN_VOID();
}

static Datum date_decrement(Relation rel, Datum existing, bool *underflow) {
  DateADT dexisting = DatumGetDateADT(existing);

  if (dexisting == DATEVAL_NOBEGIN) {
    /* return value is undefined */
    *underflow = true;
    return (Datum)0;
  }

  *underflow = false;
  return DateADTGetDatum(dexisting - 1);
}

static Datum date_increment(Relation rel, Datum existing, bool *overflow) {
  DateADT dexisting = DatumGetDateADT(existing);

  if (dexisting == DATEVAL_NOEND) {
    /* return value is undefined */
    *overflow = true;
    return (Datum)0;
  }

  *overflow = false;
  return DateADTGetDatum(dexisting + 1);
}

Datum date_skipsupport(PG_FUNCTION_ARGS) {
  SkipSupport sksup = (SkipSupport)PG_GETARG_POINTER(0);

  sksup->decrement = date_decrement;
  sksup->increment = date_increment;
  sksup->low_elem = DateADTGetDatum(DATEVAL_NOBEGIN);
  sksup->high_elem = DateADTGetDatum(DATEVAL_NOEND);

  PG_RETURN_VOID();
}

Datum hashdate(PG_FUNCTION_ARGS) { return hash_uint32(PG_GETARG_DATEADT(0)); }

Datum hashdateextended(PG_FUNCTION_ARGS) {
  return hash_uint32_extended(PG_GETARG_DATEADT(0), PG_GETARG_INT64(1));
}

Datum date_finite(PG_FUNCTION_ARGS) {
  DateADT date = PG_GETARG_DATEADT(0);

  PG_RETURN_BOOL(!DATE_NOT_FINITE(date));
}

Datum date_larger(PG_FUNCTION_ARGS) {
  DateADT dateVal1 = PG_GETARG_DATEADT(0);
  DateADT dateVal2 = PG_GETARG_DATEADT(1);

  PG_RETURN_DATEADT((dateVal1 > dateVal2) ? dateVal1 : dateVal2);
}

Datum date_smaller(PG_FUNCTION_ARGS) {
  DateADT dateVal1 = PG_GETARG_DATEADT(0);
  DateADT dateVal2 = PG_GETARG_DATEADT(1);

  PG_RETURN_DATEADT((dateVal1 < dateVal2) ? dateVal1 : dateVal2);
}

/* Compute difference between two dates in days.
 */
Datum date_mi(PG_FUNCTION_ARGS) {
  DateADT dateVal1 = PG_GETARG_DATEADT(0);
  DateADT dateVal2 = PG_GETARG_DATEADT(1);
  int32 result;

  if (ty_date_diff(dateVal1, dateVal2, &result))
    datetime_ty_error();

  PG_RETURN_INT32(result);
}

/* Add a number of days to a date, giving a new date.
 * Must handle both positive and negative numbers of days.
 */
Datum date_pli(PG_FUNCTION_ARGS) {
  DateADT dateVal = PG_GETARG_DATEADT(0);
  int32 days = PG_GETARG_INT32(1);
  DateADT result;

  if (ty_date_add_days(dateVal, days, &result))
    datetime_ty_error();

  PG_RETURN_DATEADT(result);
}

/* Subtract a number of days from a date, giving a new date.
 */
Datum date_mii(PG_FUNCTION_ARGS) {
  DateADT dateVal = PG_GETARG_DATEADT(0);
  int32 days = PG_GETARG_INT32(1);
  DateADT result;

  if (ty_date_sub_days(dateVal, days, &result))
    datetime_ty_error();

  PG_RETURN_DATEADT(result);
}

/*
 * Promote date to timestamp.
 *
 * On successful conversion, *overflow is set to zero if it's not NULL.
 *
 * If the date is finite but out of the valid range for timestamp, then:
 * if overflow is NULL, we throw an out-of-range error.
 * if overflow is not NULL, we store +1 or -1 there to indicate the sign
 * of the overflow, and return the appropriate timestamp infinity.
 *
 * Note: *overflow = -1 is actually not possible currently, since both
 * datatypes have the same lower bound, Julian day zero.
 */
Timestamp date2timestamp_opt_overflow(DateADT dateVal, int *overflow) {
  Timestamp result;

  /* ty makes midnight; a date past the range is an error, or the overflow */
  if (ty_date_to_timestamp(dateVal, &result, overflow))
    ereport(ERROR, (errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
                    errmsg("date out of range for timestamp")));

  return result;
}

/*
 * Promote date to timestamp, throwing error for overflow.
 */
static TimestampTz date2timestamp(DateADT dateVal) {
  return date2timestamp_opt_overflow(dateVal, NULL);
}

/*
 * Promote date to timestamp with time zone.
 *
 * On successful conversion, *overflow is set to zero if it's not NULL.
 *
 * If the date is finite but out of the valid range for timestamptz, then:
 * if overflow is NULL, we throw an out-of-range error.
 * if overflow is not NULL, we store +1 or -1 there to indicate the sign
 * of the overflow, and return the appropriate timestamptz infinity.
 */
TimestampTz date2timestamptz_opt_overflow(DateADT dateVal, int *overflow) {
  TimestampTz result;

  /* ty gives midnight in the session's zone, or the overflow */
  if (ty_date_to_timestamptz(session_timezone, dateVal, &result, overflow))
    ereport(ERROR, (errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
                    errmsg("%s", ty_errmsg())));
  return result;
}

/*
 * Promote date to timestamptz, throwing error for overflow.
 */
static TimestampTz date2timestamptz(DateADT dateVal) {
  return date2timestamptz_opt_overflow(dateVal, NULL);
}

/*
 * date2timestamp_no_overflow
 *
 * This is chartered to produce a double value that is numerically
 * equivalent to the corresponding Timestamp value, if the date is in the
 * valid range of Timestamps, but in any case not throw an overflow error.
 * We can do this since the numerical range of double is greater than
 * that of non-erroneous timestamps.  The results are currently only
 * used for statistical estimation purposes.
 */
double date2timestamp_no_overflow(DateADT dateVal) {
  double result;

  if (DATE_IS_NOBEGIN(dateVal))
    result = -DBL_MAX;
  else if (DATE_IS_NOEND(dateVal))
    result = DBL_MAX;
  else {
    /* date is days since 2000, timestamp is microseconds since same... */
    result = dateVal * (double)USECS_PER_DAY;
  }

  return result;
}

/*
 * Crosstype comparison functions for dates
 */

int32 date_cmp_timestamp_internal(DateADT dateVal, Timestamp dt2) {
  Timestamp dt1;
  int overflow;

  dt1 = date2timestamp_opt_overflow(dateVal, &overflow);
  if (overflow > 0) {
    /* dt1 is larger than any finite timestamp, but less than infinity */
    return TIMESTAMP_IS_NOEND(dt2) ? -1 : +1;
  }
  Assert(overflow == 0); /* -1 case cannot occur */

  return timestamp_cmp_internal(dt1, dt2);
}

Datum date_eq_timestamp(PG_FUNCTION_ARGS) {
  DateADT dateVal = PG_GETARG_DATEADT(0);
  Timestamp dt2 = PG_GETARG_TIMESTAMP(1);

  PG_RETURN_BOOL(date_cmp_timestamp_internal(dateVal, dt2) == 0);
}

Datum date_ne_timestamp(PG_FUNCTION_ARGS) {
  DateADT dateVal = PG_GETARG_DATEADT(0);
  Timestamp dt2 = PG_GETARG_TIMESTAMP(1);

  PG_RETURN_BOOL(date_cmp_timestamp_internal(dateVal, dt2) != 0);
}

Datum date_lt_timestamp(PG_FUNCTION_ARGS) {
  DateADT dateVal = PG_GETARG_DATEADT(0);
  Timestamp dt2 = PG_GETARG_TIMESTAMP(1);

  PG_RETURN_BOOL(date_cmp_timestamp_internal(dateVal, dt2) < 0);
}

Datum date_gt_timestamp(PG_FUNCTION_ARGS) {
  DateADT dateVal = PG_GETARG_DATEADT(0);
  Timestamp dt2 = PG_GETARG_TIMESTAMP(1);

  PG_RETURN_BOOL(date_cmp_timestamp_internal(dateVal, dt2) > 0);
}

Datum date_le_timestamp(PG_FUNCTION_ARGS) {
  DateADT dateVal = PG_GETARG_DATEADT(0);
  Timestamp dt2 = PG_GETARG_TIMESTAMP(1);

  PG_RETURN_BOOL(date_cmp_timestamp_internal(dateVal, dt2) <= 0);
}

Datum date_ge_timestamp(PG_FUNCTION_ARGS) {
  DateADT dateVal = PG_GETARG_DATEADT(0);
  Timestamp dt2 = PG_GETARG_TIMESTAMP(1);

  PG_RETURN_BOOL(date_cmp_timestamp_internal(dateVal, dt2) >= 0);
}

Datum date_cmp_timestamp(PG_FUNCTION_ARGS) {
  DateADT dateVal = PG_GETARG_DATEADT(0);
  Timestamp dt2 = PG_GETARG_TIMESTAMP(1);

  PG_RETURN_INT32(date_cmp_timestamp_internal(dateVal, dt2));
}

int32 date_cmp_timestamptz_internal(DateADT dateVal, TimestampTz dt2) {
  TimestampTz dt1;
  int overflow;

  dt1 = date2timestamptz_opt_overflow(dateVal, &overflow);
  if (overflow > 0) {
    /* dt1 is larger than any finite timestamp, but less than infinity */
    return TIMESTAMP_IS_NOEND(dt2) ? -1 : +1;
  }
  if (overflow < 0) {
    /* dt1 is less than any finite timestamp, but more than -infinity */
    return TIMESTAMP_IS_NOBEGIN(dt2) ? +1 : -1;
  }

  return timestamptz_cmp_internal(dt1, dt2);
}

Datum date_eq_timestamptz(PG_FUNCTION_ARGS) {
  DateADT dateVal = PG_GETARG_DATEADT(0);
  TimestampTz dt2 = PG_GETARG_TIMESTAMPTZ(1);

  PG_RETURN_BOOL(date_cmp_timestamptz_internal(dateVal, dt2) == 0);
}

Datum date_ne_timestamptz(PG_FUNCTION_ARGS) {
  DateADT dateVal = PG_GETARG_DATEADT(0);
  TimestampTz dt2 = PG_GETARG_TIMESTAMPTZ(1);

  PG_RETURN_BOOL(date_cmp_timestamptz_internal(dateVal, dt2) != 0);
}

Datum date_lt_timestamptz(PG_FUNCTION_ARGS) {
  DateADT dateVal = PG_GETARG_DATEADT(0);
  TimestampTz dt2 = PG_GETARG_TIMESTAMPTZ(1);

  PG_RETURN_BOOL(date_cmp_timestamptz_internal(dateVal, dt2) < 0);
}

Datum date_gt_timestamptz(PG_FUNCTION_ARGS) {
  DateADT dateVal = PG_GETARG_DATEADT(0);
  TimestampTz dt2 = PG_GETARG_TIMESTAMPTZ(1);

  PG_RETURN_BOOL(date_cmp_timestamptz_internal(dateVal, dt2) > 0);
}

Datum date_le_timestamptz(PG_FUNCTION_ARGS) {
  DateADT dateVal = PG_GETARG_DATEADT(0);
  TimestampTz dt2 = PG_GETARG_TIMESTAMPTZ(1);

  PG_RETURN_BOOL(date_cmp_timestamptz_internal(dateVal, dt2) <= 0);
}

Datum date_ge_timestamptz(PG_FUNCTION_ARGS) {
  DateADT dateVal = PG_GETARG_DATEADT(0);
  TimestampTz dt2 = PG_GETARG_TIMESTAMPTZ(1);

  PG_RETURN_BOOL(date_cmp_timestamptz_internal(dateVal, dt2) >= 0);
}

Datum date_cmp_timestamptz(PG_FUNCTION_ARGS) {
  DateADT dateVal = PG_GETARG_DATEADT(0);
  TimestampTz dt2 = PG_GETARG_TIMESTAMPTZ(1);

  PG_RETURN_INT32(date_cmp_timestamptz_internal(dateVal, dt2));
}

Datum timestamp_eq_date(PG_FUNCTION_ARGS) {
  Timestamp dt1 = PG_GETARG_TIMESTAMP(0);
  DateADT dateVal = PG_GETARG_DATEADT(1);

  PG_RETURN_BOOL(date_cmp_timestamp_internal(dateVal, dt1) == 0);
}

Datum timestamp_ne_date(PG_FUNCTION_ARGS) {
  Timestamp dt1 = PG_GETARG_TIMESTAMP(0);
  DateADT dateVal = PG_GETARG_DATEADT(1);

  PG_RETURN_BOOL(date_cmp_timestamp_internal(dateVal, dt1) != 0);
}

Datum timestamp_lt_date(PG_FUNCTION_ARGS) {
  Timestamp dt1 = PG_GETARG_TIMESTAMP(0);
  DateADT dateVal = PG_GETARG_DATEADT(1);

  PG_RETURN_BOOL(date_cmp_timestamp_internal(dateVal, dt1) > 0);
}

Datum timestamp_gt_date(PG_FUNCTION_ARGS) {
  Timestamp dt1 = PG_GETARG_TIMESTAMP(0);
  DateADT dateVal = PG_GETARG_DATEADT(1);

  PG_RETURN_BOOL(date_cmp_timestamp_internal(dateVal, dt1) < 0);
}

Datum timestamp_le_date(PG_FUNCTION_ARGS) {
  Timestamp dt1 = PG_GETARG_TIMESTAMP(0);
  DateADT dateVal = PG_GETARG_DATEADT(1);

  PG_RETURN_BOOL(date_cmp_timestamp_internal(dateVal, dt1) >= 0);
}

Datum timestamp_ge_date(PG_FUNCTION_ARGS) {
  Timestamp dt1 = PG_GETARG_TIMESTAMP(0);
  DateADT dateVal = PG_GETARG_DATEADT(1);

  PG_RETURN_BOOL(date_cmp_timestamp_internal(dateVal, dt1) <= 0);
}

Datum timestamp_cmp_date(PG_FUNCTION_ARGS) {
  Timestamp dt1 = PG_GETARG_TIMESTAMP(0);
  DateADT dateVal = PG_GETARG_DATEADT(1);

  PG_RETURN_INT32(-date_cmp_timestamp_internal(dateVal, dt1));
}

Datum timestamptz_eq_date(PG_FUNCTION_ARGS) {
  TimestampTz dt1 = PG_GETARG_TIMESTAMPTZ(0);
  DateADT dateVal = PG_GETARG_DATEADT(1);

  PG_RETURN_BOOL(date_cmp_timestamptz_internal(dateVal, dt1) == 0);
}

Datum timestamptz_ne_date(PG_FUNCTION_ARGS) {
  TimestampTz dt1 = PG_GETARG_TIMESTAMPTZ(0);
  DateADT dateVal = PG_GETARG_DATEADT(1);

  PG_RETURN_BOOL(date_cmp_timestamptz_internal(dateVal, dt1) != 0);
}

Datum timestamptz_lt_date(PG_FUNCTION_ARGS) {
  TimestampTz dt1 = PG_GETARG_TIMESTAMPTZ(0);
  DateADT dateVal = PG_GETARG_DATEADT(1);

  PG_RETURN_BOOL(date_cmp_timestamptz_internal(dateVal, dt1) > 0);
}

Datum timestamptz_gt_date(PG_FUNCTION_ARGS) {
  TimestampTz dt1 = PG_GETARG_TIMESTAMPTZ(0);
  DateADT dateVal = PG_GETARG_DATEADT(1);

  PG_RETURN_BOOL(date_cmp_timestamptz_internal(dateVal, dt1) < 0);
}

Datum timestamptz_le_date(PG_FUNCTION_ARGS) {
  TimestampTz dt1 = PG_GETARG_TIMESTAMPTZ(0);
  DateADT dateVal = PG_GETARG_DATEADT(1);

  PG_RETURN_BOOL(date_cmp_timestamptz_internal(dateVal, dt1) >= 0);
}

Datum timestamptz_ge_date(PG_FUNCTION_ARGS) {
  TimestampTz dt1 = PG_GETARG_TIMESTAMPTZ(0);
  DateADT dateVal = PG_GETARG_DATEADT(1);

  PG_RETURN_BOOL(date_cmp_timestamptz_internal(dateVal, dt1) <= 0);
}

Datum timestamptz_cmp_date(PG_FUNCTION_ARGS) {
  TimestampTz dt1 = PG_GETARG_TIMESTAMPTZ(0);
  DateADT dateVal = PG_GETARG_DATEADT(1);

  PG_RETURN_INT32(-date_cmp_timestamptz_internal(dateVal, dt1));
}

/*
 * in_range support function for date.
 *
 * We implement this by promoting the dates to timestamp (without time zone)
 * and then using the timestamp-and-interval in_range function.
 */
Datum in_range_date_interval(PG_FUNCTION_ARGS) {
  DateADT val = PG_GETARG_DATEADT(0);
  DateADT base = PG_GETARG_DATEADT(1);
  Interval *offset = PG_GETARG_INTERVAL_P(2);
  bool sub = PG_GETARG_BOOL(3);
  bool less = PG_GETARG_BOOL(4);
  Timestamp valStamp;
  Timestamp baseStamp;

  /* XXX we could support out-of-range cases here, perhaps */
  valStamp = date2timestamp(val);
  baseStamp = date2timestamp(base);

  return DirectFunctionCall5(
      in_range_timestamp_interval, TimestampGetDatum(valStamp),
      TimestampGetDatum(baseStamp), IntervalPGetDatum(offset),
      BoolGetDatum(sub), BoolGetDatum(less));
}

/* extract_date()
 * Extract specified field from date type.
 */
Datum extract_date(PG_FUNCTION_ARGS) {
  text *units = PG_GETARG_TEXT_PP(0);
  DateADT date = PG_GETARG_DATEADT(1);
  int type, val;
  char *lowunits;
  ty_part_t part;

  lowunits = downcase_truncate_identifier(VARDATA_ANY(units),
                                          VARSIZE_ANY_EXHDR(units), false);

  type = ty_datetime_part_unit(lowunits, 0, &val);

  if (DATE_NOT_FINITE(date) && type != 0) {
    /*
     * Infinite dates: the parts of the year and above are infinite, the
     * smaller ones are null.
     */
    switch (val) {
    case DTK_DAY:
    case DTK_MONTH:
    case DTK_QUARTER:
    case DTK_WEEK:
    case DTK_DOW:
    case DTK_ISODOW:
    case DTK_DOY:
      PG_RETURN_NULL();
      break;

    case DTK_YEAR:
    case DTK_DECADE:
    case DTK_CENTURY:
    case DTK_MILLENNIUM:
    case DTK_JULIAN:
    case DTK_ISOYEAR:
    case DTK_EPOCH:
      if (DATE_IS_NOBEGIN(date))
        PG_RETURN_NUMERIC(DatumGetNumeric(DirectFunctionCall3(
            numeric_in, CStringGetDatum("-Infinity"),
            ObjectIdGetDatum(InvalidOid), Int32GetDatum(-1))));
      else
        PG_RETURN_NUMERIC(DatumGetNumeric(DirectFunctionCall3(
            numeric_in, CStringGetDatum("Infinity"),
            ObjectIdGetDatum(InvalidOid), Int32GetDatum(-1))));
    default:
      ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                      errmsg("unit \"%s\" not supported for type %s", lowunits,
                             format_type_be(DATEOID))));
    }
  }

  if (type == 0)
    ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                    errmsg("unit \"%s\" not recognized for type %s", lowunits,
                           format_type_be(DATEOID))));

  /* ty takes the date apart and picks the unit */
  if (ty_date_part(type == 2, val, date, &part))
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                    errmsg("unit \"%s\" not supported for type %s", lowunits,
                           format_type_be(DATEOID))));

  PG_RETURN_DATUM(ty_part_datum(&part, true));
}

/* Add an interval to a date, giving a new date.
 * Must handle both positive and negative intervals.
 *
 * We implement this by promoting the date to timestamp (without time zone)
 * and then using the timestamp plus interval function.
 */
Datum date_pl_interval(PG_FUNCTION_ARGS) {
  DateADT dateVal = PG_GETARG_DATEADT(0);
  Interval *span = PG_GETARG_INTERVAL_P(1);
  Timestamp dateStamp;

  dateStamp = date2timestamp(dateVal);

  return DirectFunctionCall2(timestamp_pl_interval,
                             TimestampGetDatum(dateStamp),
                             PointerGetDatum(span));
}

/* Subtract an interval from a date, giving a new date.
 * Must handle both positive and negative intervals.
 *
 * We implement this by promoting the date to timestamp (without time zone)
 * and then using the timestamp minus interval function.
 */
Datum date_mi_interval(PG_FUNCTION_ARGS) {
  DateADT dateVal = PG_GETARG_DATEADT(0);
  Interval *span = PG_GETARG_INTERVAL_P(1);
  Timestamp dateStamp;

  dateStamp = date2timestamp(dateVal);

  return DirectFunctionCall2(timestamp_mi_interval,
                             TimestampGetDatum(dateStamp),
                             PointerGetDatum(span));
}

/* date_timestamp()
 * Convert date to timestamp data type.
 */
Datum date_timestamp(PG_FUNCTION_ARGS) {
  DateADT dateVal = PG_GETARG_DATEADT(0);
  Timestamp result;

  result = date2timestamp(dateVal);

  PG_RETURN_TIMESTAMP(result);
}

/* timestamp_date()
 * Convert timestamp to date data type.
 */
Datum timestamp_date(PG_FUNCTION_ARGS) {
  Timestamp timestamp = PG_GETARG_TIMESTAMP(0);
  DateADT result;

  if (ty_timestamp_to_date(timestamp, &result))
    ereport(ERROR, (errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
                    errmsg("timestamp out of range")));

  PG_RETURN_DATEADT(result);
}

/* date_timestamptz()
 * Convert date to timestamp with time zone data type.
 */
Datum date_timestamptz(PG_FUNCTION_ARGS) {
  DateADT dateVal = PG_GETARG_DATEADT(0);
  TimestampTz result;

  result = date2timestamptz(dateVal);

  PG_RETURN_TIMESTAMP(result);
}

/* timestamptz_date()
 * Convert timestamp with time zone to date data type.
 */
Datum timestamptz_date(PG_FUNCTION_ARGS) {
  TimestampTz timestamp = PG_GETARG_TIMESTAMP(0);
  DateADT result;

  /* ty gives the date in the session's zone; the infinities pass */
  if (ty_timestamptz_to_date(session_timezone, timestamp, &result))
    ereport(ERROR, (errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
                    errmsg("%s", ty_errmsg())));

  PG_RETURN_DATEADT(result);
}

/*****************************************************************************
 *	 Time ADT
 *****************************************************************************/

Datum time_in(PG_FUNCTION_ARGS) {
  char *str = PG_GETARG_CSTRING(0);
#ifdef NOT_USED
  Oid typelem = PG_GETARG_OID(1);
#endif
  int32 typmod = PG_GETARG_INT32(2);
  Node *escontext = fcinfo->context;
  TimeADT result;
  int dterr;
  ty_dtin_extra_t extra;

  /* ty reads the text; the zone words and the clock are the hooks' */
  if (ty_time_parse(str, DateOrder, &ty_dtzone_hooks, &result, &dterr,
                    &extra)) {
    TyDateTimeParseError(dterr, &extra, str, "time", escontext);
    PG_RETURN_NULL();
  }
  AdjustTimeForTypmod(&result, typmod);

  PG_RETURN_TIMEADT(result);
}

/* tm2time()
 * Convert a tm structure to a time data type.
 */
int tm2time(struct pg_tm *tm, fsec_t fsec, TimeADT *result) {
  *result = ((((tm->tm_hour * MINS_PER_HOUR + tm->tm_min) * SECS_PER_MINUTE) +
              tm->tm_sec) *
             USECS_PER_SEC) +
            fsec;
  return 0;
}

/* time_overflows()
 * Check to see if a broken-down time-of-day is out of range.
 */
bool time_overflows(int hour, int min, int sec, fsec_t fsec) {
  /* Range-check the fields individually. */
  if (hour < 0 || hour > HOURS_PER_DAY || min < 0 || min >= MINS_PER_HOUR ||
      sec < 0 || sec > SECS_PER_MINUTE || fsec < 0 || fsec > USECS_PER_SEC)
    return true;

  /*
   * Because we allow, eg, hour = 24 or sec = 60, we must check separately
   * that the total time value doesn't exceed 24:00:00.
   */
  if ((((((hour * MINS_PER_HOUR + min) * SECS_PER_MINUTE) + sec) *
        USECS_PER_SEC) +
       fsec) > USECS_PER_DAY)
    return true;

  return false;
}

/* float_time_overflows()
 * Same, when we have seconds + fractional seconds as one "double" value.
 */
bool float_time_overflows(int hour, int min, double sec) {
  /* Range-check the fields individually. */
  if (hour < 0 || hour > HOURS_PER_DAY || min < 0 || min >= MINS_PER_HOUR)
    return true;

  /*
   * "sec", being double, requires extra care.  Cope with NaN, and round off
   * before applying the range check to avoid unexpected errors due to
   * imprecise input.  (We assume rint() behaves sanely with infinities.)
   */
  if (isnan(sec))
    return true;
  sec = rint(sec * USECS_PER_SEC);
  if (sec < 0 || sec > SECS_PER_MINUTE * USECS_PER_SEC)
    return true;

  /*
   * Because we allow, eg, hour = 24 or sec = 60, we must check separately
   * that the total time value doesn't exceed 24:00:00.  This must match the
   * way that callers will convert the fields to a time.
   */
  if (((((hour * MINS_PER_HOUR + min) * SECS_PER_MINUTE) * USECS_PER_SEC) +
       (int64)sec) > USECS_PER_DAY)
    return true;

  return false;
}

/* time2tm()
 * Convert time data type to POSIX time structure.
 *
 * Note that only the hour/min/sec/fractional-sec fields are filled in.
 */
int time2tm(TimeADT time, struct pg_tm *tm, fsec_t *fsec) {
  tm->tm_hour = time / USECS_PER_HOUR;
  time -= tm->tm_hour * USECS_PER_HOUR;
  tm->tm_min = time / USECS_PER_MINUTE;
  time -= tm->tm_min * USECS_PER_MINUTE;
  tm->tm_sec = time / USECS_PER_SEC;
  time -= tm->tm_sec * USECS_PER_SEC;
  *fsec = time;
  return 0;
}

Datum time_out(PG_FUNCTION_ARGS) {
  TimeADT time = PG_GETARG_TIMEADT(0);
  char *result;
  struct pg_tm tt, *tm = &tt;
  fsec_t fsec;
  char buf[MAXDATELEN + 1];

  time2tm(time, tm, &fsec);
  EncodeTimeOnly(tm, fsec, false, 0, DateStyle, buf);

  result = pstrdup(buf);
  PG_RETURN_CSTRING(result);
}

/*
 *		time_recv			- converts external binary
 *format to time
 */
Datum time_recv(PG_FUNCTION_ARGS) {
  StringInfo buf = (StringInfo)PG_GETARG_POINTER(0);

#ifdef NOT_USED
  Oid typelem = PG_GETARG_OID(1);
#endif
  int32 typmod = PG_GETARG_INT32(2);
  TimeADT result;

  result = pq_getmsgint64(buf);

  if (result < INT64CONST(0) || result > USECS_PER_DAY)
    ereport(ERROR, (errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
                    errmsg("time out of range")));

  AdjustTimeForTypmod(&result, typmod);

  PG_RETURN_TIMEADT(result);
}

/*
 *		time_send			- converts time to binary format
 */
Datum time_send(PG_FUNCTION_ARGS) {
  TimeADT time = PG_GETARG_TIMEADT(0);
  StringInfoData buf;

  pq_begintypsend(&buf);
  pq_sendint64(&buf, time);
  PG_RETURN_BYTEA_P(pq_endtypsend(&buf));
}

Datum timetypmodin(PG_FUNCTION_ARGS) {
  ArrayType *ta = PG_GETARG_ARRAYTYPE_P(0);

  PG_RETURN_INT32(anytime_typmodin(false, ta));
}

Datum timetypmodout(PG_FUNCTION_ARGS) {
  int32 typmod = PG_GETARG_INT32(0);

  PG_RETURN_CSTRING(anytime_typmodout(false, typmod));
}

/*
 *		make_time			- time constructor
 */
Datum make_time(PG_FUNCTION_ARGS) {
  TimeADT time;

  /* ty checks the fields; the seconds are a float */
  if (ty_time_make(PG_GETARG_INT32(0), PG_GETARG_INT32(1), PG_GETARG_FLOAT8(2),
                   &time))
    datetime_ty_error();

  PG_RETURN_TIMEADT(time);
}

/* time_support()
 *
 * Planner support function for the time_scale() and timetz_scale()
 * length coercion functions (we need not distinguish them here).
 */
Datum time_support(PG_FUNCTION_ARGS) {
  Node *rawreq = (Node *)PG_GETARG_POINTER(0);
  Node *ret = NULL;

  if (IsA(rawreq, SupportRequestSimplify)) {
    SupportRequestSimplify *req = (SupportRequestSimplify *)rawreq;

    ret = TemporalSimplify(MAX_TIME_PRECISION, (Node *)req->fcall);
  }

  PG_RETURN_POINTER(ret);
}

/* time_scale()
 * Adjust time type for specified scale factor.
 * Used by PostgreSQL type system to stuff columns.
 */
Datum time_scale(PG_FUNCTION_ARGS) {
  TimeADT time = PG_GETARG_TIMEADT(0);
  int32 typmod = PG_GETARG_INT32(1);
  TimeADT result;

  result = time;
  AdjustTimeForTypmod(&result, typmod);

  PG_RETURN_TIMEADT(result);
}

/* AdjustTimeForTypmod()
 * Force the precision of the time value to a specified value.
 * Uses *exactly* the same code as in AdjustTimestampForTypmod()
 * but we make a separate copy because those types do not
 * have a fundamental tie together but rather a coincidence of
 * implementation. - thomas
 */
void AdjustTimeForTypmod(TimeADT *time, int32 typmod) {
  ty_time_adjust_typmod(time, typmod);
}

Datum time_eq(PG_FUNCTION_ARGS) {
  TimeADT time1 = PG_GETARG_TIMEADT(0);
  TimeADT time2 = PG_GETARG_TIMEADT(1);

  PG_RETURN_BOOL(time1 == time2);
}

Datum time_ne(PG_FUNCTION_ARGS) {
  TimeADT time1 = PG_GETARG_TIMEADT(0);
  TimeADT time2 = PG_GETARG_TIMEADT(1);

  PG_RETURN_BOOL(time1 != time2);
}

Datum time_lt(PG_FUNCTION_ARGS) {
  TimeADT time1 = PG_GETARG_TIMEADT(0);
  TimeADT time2 = PG_GETARG_TIMEADT(1);

  PG_RETURN_BOOL(time1 < time2);
}

Datum time_le(PG_FUNCTION_ARGS) {
  TimeADT time1 = PG_GETARG_TIMEADT(0);
  TimeADT time2 = PG_GETARG_TIMEADT(1);

  PG_RETURN_BOOL(time1 <= time2);
}

Datum time_gt(PG_FUNCTION_ARGS) {
  TimeADT time1 = PG_GETARG_TIMEADT(0);
  TimeADT time2 = PG_GETARG_TIMEADT(1);

  PG_RETURN_BOOL(time1 > time2);
}

Datum time_ge(PG_FUNCTION_ARGS) {
  TimeADT time1 = PG_GETARG_TIMEADT(0);
  TimeADT time2 = PG_GETARG_TIMEADT(1);

  PG_RETURN_BOOL(time1 >= time2);
}

Datum time_cmp(PG_FUNCTION_ARGS) {
  TimeADT time1 = PG_GETARG_TIMEADT(0);
  TimeADT time2 = PG_GETARG_TIMEADT(1);

  if (time1 < time2)
    PG_RETURN_INT32(-1);
  if (time1 > time2)
    PG_RETURN_INT32(1);
  PG_RETURN_INT32(0);
}

Datum time_hash(PG_FUNCTION_ARGS) { return hashint8(fcinfo); }

Datum time_hash_extended(PG_FUNCTION_ARGS) { return hashint8extended(fcinfo); }

Datum time_larger(PG_FUNCTION_ARGS) {
  TimeADT time1 = PG_GETARG_TIMEADT(0);
  TimeADT time2 = PG_GETARG_TIMEADT(1);

  PG_RETURN_TIMEADT((time1 > time2) ? time1 : time2);
}

Datum time_smaller(PG_FUNCTION_ARGS) {
  TimeADT time1 = PG_GETARG_TIMEADT(0);
  TimeADT time2 = PG_GETARG_TIMEADT(1);

  PG_RETURN_TIMEADT((time1 < time2) ? time1 : time2);
}

/* overlaps_time() --- implements the SQL OVERLAPS operator.
 *
 * Algorithm is per SQL spec.  This is much harder than you'd think
 * because the spec requires us to deliver a non-null answer in some cases
 * where some of the inputs are null.
 */
Datum overlaps_time(PG_FUNCTION_ARGS) {
  int64 v[4];
  const int64 *p[4];
  int r;

  /* a null end is a null pointer to ty, which applies the SQL standard */
  for (int i = 0; i < 4; i++) {
    v[i] = PG_ARGISNULL(i) ? 0 : DatumGetTimeADT(PG_GETARG_DATUM(i));
    p[i] = PG_ARGISNULL(i) ? NULL : &v[i];
  }
  r = ty_overlaps_i64(p[0], p[1], p[2], p[3]);
  if (r == 2)
    PG_RETURN_NULL();
  PG_RETURN_BOOL(r == 1);
}

/* timestamp_time()
 * Convert timestamp to time data type.
 */
Datum timestamp_time(PG_FUNCTION_ARGS) {
  Timestamp timestamp = PG_GETARG_TIMESTAMP(0);
  TimeADT result;

  if (TIMESTAMP_NOT_FINITE(timestamp))
    PG_RETURN_NULL();

  if (ty_timestamp_to_time(timestamp, &result))
    ereport(ERROR, (errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
                    errmsg("timestamp out of range")));

  PG_RETURN_TIMEADT(result);
}

/* timestamptz_time()
 * Convert timestamptz to time data type.
 */
Datum timestamptz_time(PG_FUNCTION_ARGS) {
  TimestampTz timestamp = PG_GETARG_TIMESTAMP(0);
  TimeADT result;
  struct pg_tm tt, *tm = &tt;
  int tz;
  fsec_t fsec;

  if (TIMESTAMP_NOT_FINITE(timestamp))
    PG_RETURN_NULL();

  if (timestamp2tm(timestamp, &tz, tm, &fsec, NULL, NULL) != 0)
    ereport(ERROR, (errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
                    errmsg("timestamp out of range")));

  /*
   * Could also do this with time = (timestamp / USECS_PER_DAY *
   * USECS_PER_DAY) - timestamp;
   */
  result = ((((tm->tm_hour * MINS_PER_HOUR + tm->tm_min) * SECS_PER_MINUTE) +
             tm->tm_sec) *
            USECS_PER_SEC) +
           fsec;

  PG_RETURN_TIMEADT(result);
}

/* datetime_timestamp()
 * Convert date and time to timestamp data type.
 */
Datum datetime_timestamp(PG_FUNCTION_ARGS) {
  DateADT date = PG_GETARG_DATEADT(0);
  TimeADT time = PG_GETARG_TIMEADT(1);
  Timestamp result;

  if (ty_datetime_to_timestamp(date, time, &result))
    ereport(ERROR, (errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
                    errmsg("%s", ty_errmsg())));

  PG_RETURN_TIMESTAMP(result);
}

/* time_interval()
 * Convert time to interval data type.
 */
Datum time_interval(PG_FUNCTION_ARGS) {
  TimeADT time = PG_GETARG_TIMEADT(0);
  Interval *result;

  result = (Interval *)palloc(sizeof(Interval));

  result->time = time;
  result->day = 0;
  result->month = 0;

  PG_RETURN_INTERVAL_P(result);
}

/* interval_time()
 * Convert interval to time data type.
 *
 * This is defined as producing the fractional-day portion of the interval.
 * Therefore, we can just ignore the months field.  It is not real clear
 * what to do with negative intervals, but we choose to subtract the floor,
 * so that, say, '-2 hours' becomes '22:00:00'.
 */
Datum interval_time(PG_FUNCTION_ARGS) {
  Interval *span = PG_GETARG_INTERVAL_P(0);
  TimeADT result;

  if (ty_interval_to_time((const ty_interval_t *)span, &result))
    ereport(ERROR, (errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
                    errmsg("cannot convert infinite interval to time")));

  PG_RETURN_TIMEADT(result);
}

/* time_mi_time()
 * Subtract two times to produce an interval.
 */
Datum time_mi_time(PG_FUNCTION_ARGS) {
  TimeADT time1 = PG_GETARG_TIMEADT(0);
  TimeADT time2 = PG_GETARG_TIMEADT(1);
  Interval *result;

  result = (Interval *)palloc(sizeof(Interval));
  ty_time_diff(time1, time2, (ty_interval_t *)result);

  PG_RETURN_INTERVAL_P(result);
}

/* time_pl_interval()
 * Add interval to time.
 */
Datum time_pl_interval(PG_FUNCTION_ARGS) {
  TimeADT time = PG_GETARG_TIMEADT(0);
  Interval *span = PG_GETARG_INTERVAL_P(1);
  TimeADT result;

  if (ty_time_add_interval(time, (const ty_interval_t *)span, &result))
    datetime_ty_error();

  PG_RETURN_TIMEADT(result);
}

/* time_mi_interval()
 * Subtract interval from time.
 */
Datum time_mi_interval(PG_FUNCTION_ARGS) {
  TimeADT time = PG_GETARG_TIMEADT(0);
  Interval *span = PG_GETARG_INTERVAL_P(1);
  TimeADT result;

  if (ty_time_sub_interval(time, (const ty_interval_t *)span, &result))
    datetime_ty_error();

  PG_RETURN_TIMEADT(result);
}

/*
 * in_range support function for time.
 */
Datum in_range_time_interval(PG_FUNCTION_ARGS) {
  int r;

  if (ty_time_in_range(PG_GETARG_TIMEADT(0), PG_GETARG_TIMEADT(1),
                       (const ty_interval_t *)PG_GETARG_INTERVAL_P(2),
                       PG_GETARG_BOOL(3), PG_GETARG_BOOL(4), &r))
    ereport(ERROR,
            (errcode(ERRCODE_INVALID_PRECEDING_OR_FOLLOWING_SIZE),
             errmsg("invalid preceding or following size in window function")));
  PG_RETURN_BOOL(r);
}

/* time_part() and extract_time()
 * Extract specified field from time type.
 */
static Datum time_part_common(PG_FUNCTION_ARGS, bool retnumeric) {
  text *units = PG_GETARG_TEXT_PP(0);
  TimeADT time = PG_GETARG_TIMEADT(1);
  int type, val;
  char *lowunits;
  ty_part_t part;

  lowunits = downcase_truncate_identifier(VARDATA_ANY(units),
                                          VARSIZE_ANY_EXHDR(units), false);

  /* only epoch is a reserved word a time has a part for */
  type = ty_datetime_part_unit(lowunits, 1, &val);

  if (type == 0)
    ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                    errmsg("unit \"%s\" not recognized for type %s", lowunits,
                           format_type_be(TIMEOID))));

  if (ty_time_part(type == 2, val, time, &part))
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                    errmsg("unit \"%s\" not supported for type %s", lowunits,
                           format_type_be(TIMEOID))));

  return ty_part_datum(&part, retnumeric);
}

Datum time_part(PG_FUNCTION_ARGS) { return time_part_common(fcinfo, false); }

Datum extract_time(PG_FUNCTION_ARGS) { return time_part_common(fcinfo, true); }

/*****************************************************************************
 *	 Time With Time Zone ADT
 *****************************************************************************/

/* tm2timetz()
 * Convert a tm structure to a time data type.
 */
int tm2timetz(struct pg_tm *tm, fsec_t fsec, int tz, TimeTzADT *result) {
  result->time =
      ((((tm->tm_hour * MINS_PER_HOUR + tm->tm_min) * SECS_PER_MINUTE) +
        tm->tm_sec) *
       USECS_PER_SEC) +
      fsec;
  result->zone = tz;

  return 0;
}

Datum timetz_in(PG_FUNCTION_ARGS) {
  char *str = PG_GETARG_CSTRING(0);
#ifdef NOT_USED
  Oid typelem = PG_GETARG_OID(1);
#endif
  int32 typmod = PG_GETARG_INT32(2);
  Node *escontext = fcinfo->context;
  TimeTzADT *result;
  int dterr;
  ty_dtin_extra_t extra;

  result = (TimeTzADT *)palloc(sizeof(TimeTzADT));
  /* ty reads the text; the zones, with their offsets, are the hooks' */
  if (ty_timetz_parse(str, DateOrder, &ty_dtzone_hooks_tz,
                      (ty_timetz_t *)result, &dterr, &extra)) {
    TyDateTimeParseError(dterr, &extra, str, "time with time zone", escontext);
    PG_RETURN_NULL();
  }
  AdjustTimeForTypmod(&(result->time), typmod);

  PG_RETURN_TIMETZADT_P(result);
}

Datum timetz_out(PG_FUNCTION_ARGS) {
  TimeTzADT *time = PG_GETARG_TIMETZADT_P(0);
  char *result;
  struct pg_tm tt, *tm = &tt;
  fsec_t fsec;
  int tz;
  char buf[MAXDATELEN + 1];

  timetz2tm(time, tm, &fsec, &tz);
  EncodeTimeOnly(tm, fsec, true, tz, DateStyle, buf);

  result = pstrdup(buf);
  PG_RETURN_CSTRING(result);
}

/*
 *		timetz_recv			- converts external binary
 *format to timetz
 */
Datum timetz_recv(PG_FUNCTION_ARGS) {
  StringInfo buf = (StringInfo)PG_GETARG_POINTER(0);

#ifdef NOT_USED
  Oid typelem = PG_GETARG_OID(1);
#endif
  int32 typmod = PG_GETARG_INT32(2);
  TimeTzADT *result;

  result = (TimeTzADT *)palloc(sizeof(TimeTzADT));

  result->time = pq_getmsgint64(buf);

  if (result->time < INT64CONST(0) || result->time > USECS_PER_DAY)
    ereport(ERROR, (errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
                    errmsg("time out of range")));

  result->zone = pq_getmsgint(buf, sizeof(result->zone));

  /* Check for sane GMT displacement; see notes in datatype/timestamp.h */
  if (result->zone <= -TZDISP_LIMIT || result->zone >= TZDISP_LIMIT)
    ereport(ERROR, (errcode(ERRCODE_INVALID_TIME_ZONE_DISPLACEMENT_VALUE),
                    errmsg("time zone displacement out of range")));

  AdjustTimeForTypmod(&(result->time), typmod);

  PG_RETURN_TIMETZADT_P(result);
}

/*
 *		timetz_send			- converts timetz to binary
 *format
 */
Datum timetz_send(PG_FUNCTION_ARGS) {
  TimeTzADT *time = PG_GETARG_TIMETZADT_P(0);
  StringInfoData buf;

  pq_begintypsend(&buf);
  pq_sendint64(&buf, time->time);
  pq_sendint32(&buf, time->zone);
  PG_RETURN_BYTEA_P(pq_endtypsend(&buf));
}

Datum timetztypmodin(PG_FUNCTION_ARGS) {
  ArrayType *ta = PG_GETARG_ARRAYTYPE_P(0);

  PG_RETURN_INT32(anytime_typmodin(true, ta));
}

Datum timetztypmodout(PG_FUNCTION_ARGS) {
  int32 typmod = PG_GETARG_INT32(0);

  PG_RETURN_CSTRING(anytime_typmodout(true, typmod));
}

/* timetz2tm()
 * Convert TIME WITH TIME ZONE data type to POSIX time structure.
 */
int timetz2tm(TimeTzADT *time, struct pg_tm *tm, fsec_t *fsec, int *tzp) {
  TimeOffset trem = time->time;

  tm->tm_hour = trem / USECS_PER_HOUR;
  trem -= tm->tm_hour * USECS_PER_HOUR;
  tm->tm_min = trem / USECS_PER_MINUTE;
  trem -= tm->tm_min * USECS_PER_MINUTE;
  tm->tm_sec = trem / USECS_PER_SEC;
  *fsec = trem - tm->tm_sec * USECS_PER_SEC;

  if (tzp != NULL)
    *tzp = time->zone;

  return 0;
}

/* timetz_scale()
 * Adjust time type for specified scale factor.
 * Used by PostgreSQL type system to stuff columns.
 */
Datum timetz_scale(PG_FUNCTION_ARGS) {
  TimeTzADT *time = PG_GETARG_TIMETZADT_P(0);
  int32 typmod = PG_GETARG_INT32(1);
  TimeTzADT *result;

  result = (TimeTzADT *)palloc(sizeof(TimeTzADT));

  result->time = time->time;
  result->zone = time->zone;

  AdjustTimeForTypmod(&(result->time), typmod);

  PG_RETURN_TIMETZADT_P(result);
}

static int timetz_cmp_internal(TimeTzADT *time1, TimeTzADT *time2) {
  return ty_timetz_cmp((const ty_timetz_t *)time1, (const ty_timetz_t *)time2);
}

Datum timetz_eq(PG_FUNCTION_ARGS) {
  TimeTzADT *time1 = PG_GETARG_TIMETZADT_P(0);
  TimeTzADT *time2 = PG_GETARG_TIMETZADT_P(1);

  PG_RETURN_BOOL(timetz_cmp_internal(time1, time2) == 0);
}

Datum timetz_ne(PG_FUNCTION_ARGS) {
  TimeTzADT *time1 = PG_GETARG_TIMETZADT_P(0);
  TimeTzADT *time2 = PG_GETARG_TIMETZADT_P(1);

  PG_RETURN_BOOL(timetz_cmp_internal(time1, time2) != 0);
}

Datum timetz_lt(PG_FUNCTION_ARGS) {
  TimeTzADT *time1 = PG_GETARG_TIMETZADT_P(0);
  TimeTzADT *time2 = PG_GETARG_TIMETZADT_P(1);

  PG_RETURN_BOOL(timetz_cmp_internal(time1, time2) < 0);
}

Datum timetz_le(PG_FUNCTION_ARGS) {
  TimeTzADT *time1 = PG_GETARG_TIMETZADT_P(0);
  TimeTzADT *time2 = PG_GETARG_TIMETZADT_P(1);

  PG_RETURN_BOOL(timetz_cmp_internal(time1, time2) <= 0);
}

Datum timetz_gt(PG_FUNCTION_ARGS) {
  TimeTzADT *time1 = PG_GETARG_TIMETZADT_P(0);
  TimeTzADT *time2 = PG_GETARG_TIMETZADT_P(1);

  PG_RETURN_BOOL(timetz_cmp_internal(time1, time2) > 0);
}

Datum timetz_ge(PG_FUNCTION_ARGS) {
  TimeTzADT *time1 = PG_GETARG_TIMETZADT_P(0);
  TimeTzADT *time2 = PG_GETARG_TIMETZADT_P(1);

  PG_RETURN_BOOL(timetz_cmp_internal(time1, time2) >= 0);
}

Datum timetz_cmp(PG_FUNCTION_ARGS) {
  TimeTzADT *time1 = PG_GETARG_TIMETZADT_P(0);
  TimeTzADT *time2 = PG_GETARG_TIMETZADT_P(1);

  PG_RETURN_INT32(timetz_cmp_internal(time1, time2));
}

Datum timetz_hash(PG_FUNCTION_ARGS) {
  TimeTzADT *key = PG_GETARG_TIMETZADT_P(0);

  /* the low 32 bits of the hash with seed 0 are the standard hash */
  PG_RETURN_UINT32((uint32)ty_timetz_hash((const ty_timetz_t *)key, 0));
}

Datum timetz_hash_extended(PG_FUNCTION_ARGS) {
  TimeTzADT *key = PG_GETARG_TIMETZADT_P(0);

  PG_RETURN_UINT64(
      ty_timetz_hash((const ty_timetz_t *)key, (uint64)PG_GETARG_INT64(1)));
}

Datum timetz_larger(PG_FUNCTION_ARGS) {
  TimeTzADT *time1 = PG_GETARG_TIMETZADT_P(0);
  TimeTzADT *time2 = PG_GETARG_TIMETZADT_P(1);
  TimeTzADT *result;

  if (timetz_cmp_internal(time1, time2) > 0)
    result = time1;
  else
    result = time2;
  PG_RETURN_TIMETZADT_P(result);
}

Datum timetz_smaller(PG_FUNCTION_ARGS) {
  TimeTzADT *time1 = PG_GETARG_TIMETZADT_P(0);
  TimeTzADT *time2 = PG_GETARG_TIMETZADT_P(1);
  TimeTzADT *result;

  if (timetz_cmp_internal(time1, time2) < 0)
    result = time1;
  else
    result = time2;
  PG_RETURN_TIMETZADT_P(result);
}

/* timetz_pl_interval()
 * Add interval to timetz.
 */
Datum timetz_pl_interval(PG_FUNCTION_ARGS) {
  TimeTzADT *time = PG_GETARG_TIMETZADT_P(0);
  Interval *span = PG_GETARG_INTERVAL_P(1);
  TimeTzADT *result = (TimeTzADT *)palloc(sizeof(TimeTzADT));

  /* the time of day moves as a time does; the zone stays */
  if (ty_time_add_interval(time->time, (const ty_interval_t *)span,
                           &result->time))
    datetime_ty_error();
  result->zone = time->zone;

  PG_RETURN_TIMETZADT_P(result);
}

/* timetz_mi_interval()
 * Subtract interval from timetz.
 */
Datum timetz_mi_interval(PG_FUNCTION_ARGS) {
  TimeTzADT *time = PG_GETARG_TIMETZADT_P(0);
  Interval *span = PG_GETARG_INTERVAL_P(1);
  TimeTzADT *result = (TimeTzADT *)palloc(sizeof(TimeTzADT));

  if (ty_time_sub_interval(time->time, (const ty_interval_t *)span,
                           &result->time))
    datetime_ty_error();
  result->zone = time->zone;

  PG_RETURN_TIMETZADT_P(result);
}

/*
 * in_range support function for timetz.
 */
Datum in_range_timetz_interval(PG_FUNCTION_ARGS) {
  int r;

  if (ty_timetz_in_range(
          (const ty_timetz_t *)PG_GETARG_TIMETZADT_P(0),
          (const ty_timetz_t *)PG_GETARG_TIMETZADT_P(1),
          (const ty_interval_t *)PG_GETARG_INTERVAL_P(2), PG_GETARG_BOOL(3),
          PG_GETARG_BOOL(4), &r))
    ereport(ERROR,
            (errcode(ERRCODE_INVALID_PRECEDING_OR_FOLLOWING_SIZE),
             errmsg("invalid preceding or following size in window function")));
  PG_RETURN_BOOL(r);
}

/* overlaps_timetz() --- implements the SQL OVERLAPS operator.
 *
 * Algorithm is per SQL spec.  This is much harder than you'd think
 * because the spec requires us to deliver a non-null answer in some cases
 * where some of the inputs are null.
 */
Datum overlaps_timetz(PG_FUNCTION_ARGS) {
  TimeTzADT *v[4];
  int r;

  /* a null end is a null pointer to ty, which applies the SQL standard */
  for (int i = 0; i < 4; i++)
    v[i] = PG_ARGISNULL(i) ? NULL : DatumGetTimeTzADTP(PG_GETARG_DATUM(i));
  r = ty_overlaps_timetz((const ty_timetz_t *)v[0], (const ty_timetz_t *)v[1],
                         (const ty_timetz_t *)v[2], (const ty_timetz_t *)v[3]);
  if (r == 2)
    PG_RETURN_NULL();
  PG_RETURN_BOOL(r == 1);
}

Datum timetz_time(PG_FUNCTION_ARGS) {
  TimeTzADT *timetz = PG_GETARG_TIMETZADT_P(0);
  TimeADT result;

  /* swallow the time zone and just return the time */
  result = timetz->time;

  PG_RETURN_TIMEADT(result);
}

Datum time_timetz(PG_FUNCTION_ARGS) {
  TimeADT time = PG_GETARG_TIMEADT(0);
  TimeTzADT *result;
  struct pg_tm tt, *tm = &tt;
  fsec_t fsec;
  int tz;

  GetCurrentDateTime(tm);
  time2tm(time, tm, &fsec);
  tz = DetermineTimeZoneOffset(tm, session_timezone);

  result = (TimeTzADT *)palloc(sizeof(TimeTzADT));

  result->time = time;
  result->zone = tz;

  PG_RETURN_TIMETZADT_P(result);
}

/* timestamptz_timetz()
 * Convert timestamp to timetz data type.
 */
Datum timestamptz_timetz(PG_FUNCTION_ARGS) {
  TimestampTz timestamp = PG_GETARG_TIMESTAMP(0);
  TimeTzADT *result;
  struct pg_tm tt, *tm = &tt;
  int tz;
  fsec_t fsec;

  if (TIMESTAMP_NOT_FINITE(timestamp))
    PG_RETURN_NULL();

  if (timestamp2tm(timestamp, &tz, tm, &fsec, NULL, NULL) != 0)
    ereport(ERROR, (errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
                    errmsg("timestamp out of range")));

  result = (TimeTzADT *)palloc(sizeof(TimeTzADT));

  tm2timetz(tm, fsec, tz, result);

  PG_RETURN_TIMETZADT_P(result);
}

/* datetimetz_timestamptz()
 * Convert date and timetz to timestamp with time zone data type.
 * Timestamp is stored in GMT, so add the time zone
 * stored with the timetz to the result.
 * - thomas 2000-03-10
 */
Datum datetimetz_timestamptz(PG_FUNCTION_ARGS) {
  DateADT date = PG_GETARG_DATEADT(0);
  TimeTzADT *time = PG_GETARG_TIMETZADT_P(1);
  TimestampTz result;

  if (DATE_IS_NOBEGIN(date))
    TIMESTAMP_NOBEGIN(result);
  else if (DATE_IS_NOEND(date))
    TIMESTAMP_NOEND(result);
  else {
    /*
     * Date's range is wider than timestamp's, so check for boundaries.
     * Since dates have the same minimum values as timestamps, only upper
     * boundary need be checked for overflow.
     */
    if (date >= (TIMESTAMP_END_JULIAN - POSTGRES_EPOCH_JDATE))
      ereport(ERROR, (errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
                      errmsg("date out of range for timestamp")));
    result = date * USECS_PER_DAY + time->time + time->zone * USECS_PER_SEC;

    /*
     * Since it is possible to go beyond allowed timestamptz range because
     * of time zone, check for allowed timestamp range after adding tz.
     */
    if (!IS_VALID_TIMESTAMP(result))
      ereport(ERROR, (errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
                      errmsg("date out of range for timestamp")));
  }

  PG_RETURN_TIMESTAMP(result);
}

/* timetz_part() and extract_timetz()
 * Extract specified field from time type.
 */
static Datum timetz_part_common(PG_FUNCTION_ARGS, bool retnumeric) {
  text *units = PG_GETARG_TEXT_PP(0);
  TimeTzADT *time = PG_GETARG_TIMETZADT_P(1);
  int64 intresult;
  int type, val;
  char *lowunits;

  lowunits = downcase_truncate_identifier(VARDATA_ANY(units),
                                          VARSIZE_ANY_EXHDR(units), false);

  type = DecodeUnits(0, lowunits, &val);
  if (type == UNKNOWN_FIELD)
    type = DecodeSpecial(0, lowunits, &val);

  if (type == UNITS) {
    int tz;
    fsec_t fsec;
    struct pg_tm tt, *tm = &tt;

    timetz2tm(time, tm, &fsec, &tz);

    switch (val) {
    case DTK_TZ:
      intresult = -tz;
      break;

    case DTK_TZ_MINUTE:
      intresult = (-tz / SECS_PER_MINUTE) % MINS_PER_HOUR;
      break;

    case DTK_TZ_HOUR:
      intresult = -tz / SECS_PER_HOUR;
      break;

    case DTK_MICROSEC:
      intresult = tm->tm_sec * INT64CONST(1000000) + fsec;
      break;

    case DTK_MILLISEC:
      if (retnumeric)
        /*---
         * tm->tm_sec * 1000 + fsec / 1000
         * = (tm->tm_sec * 1'000'000 + fsec) / 1000
         */
        PG_RETURN_NUMERIC(int64_div_fast_to_numeric(
            tm->tm_sec * INT64CONST(1000000) + fsec, 3));
      else
        PG_RETURN_FLOAT8(tm->tm_sec * 1000.0 + fsec / 1000.0);
      break;

    case DTK_SECOND:
      if (retnumeric)
        /*---
         * tm->tm_sec + fsec / 1'000'000
         * = (tm->tm_sec * 1'000'000 + fsec) / 1'000'000
         */
        PG_RETURN_NUMERIC(int64_div_fast_to_numeric(
            tm->tm_sec * INT64CONST(1000000) + fsec, 6));
      else
        PG_RETURN_FLOAT8(tm->tm_sec + fsec / 1000000.0);
      break;

    case DTK_MINUTE:
      intresult = tm->tm_min;
      break;

    case DTK_HOUR:
      intresult = tm->tm_hour;
      break;

    case DTK_DAY:
    case DTK_MONTH:
    case DTK_QUARTER:
    case DTK_YEAR:
    case DTK_DECADE:
    case DTK_CENTURY:
    case DTK_MILLENNIUM:
    default:
      ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                      errmsg("unit \"%s\" not supported for type %s", lowunits,
                             format_type_be(TIMETZOID))));
      intresult = 0;
    }
  } else if (type == RESERV && val == DTK_EPOCH) {
    if (retnumeric)
      /*---
       * time->time / 1'000'000 + time->zone
       * = (time->time + time->zone * 1'000'000) / 1'000'000
       */
      PG_RETURN_NUMERIC(int64_div_fast_to_numeric(
          time->time + time->zone * INT64CONST(1000000), 6));
    else
      PG_RETURN_FLOAT8(time->time / 1000000.0 + time->zone);
  } else {
    ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                    errmsg("unit \"%s\" not recognized for type %s", lowunits,
                           format_type_be(TIMETZOID))));
    intresult = 0;
  }

  if (retnumeric)
    PG_RETURN_NUMERIC(int64_to_numeric(intresult));
  else
    PG_RETURN_FLOAT8(intresult);
}

Datum timetz_part(PG_FUNCTION_ARGS) {
  return timetz_part_common(fcinfo, false);
}

Datum extract_timetz(PG_FUNCTION_ARGS) {
  return timetz_part_common(fcinfo, true);
}

/* timetz_zone()
 * Encode time with time zone type with specified time zone.
 * Applies DST rules as of the transaction start time.
 */
Datum timetz_zone(PG_FUNCTION_ARGS) {
  text *zone = PG_GETARG_TEXT_PP(0);
  TimeTzADT *t = PG_GETARG_TIMETZADT_P(1);
  TimeTzADT *result;
  int tz;
  char tzname[TZ_STRLEN_MAX + 1];
  int type, val;
  pg_tz *tzp;

  /*
   * Look up the requested timezone.
   */
  text_to_cstring_buffer(zone, tzname, sizeof(tzname));

  type = DecodeTimezoneName(tzname, &val, &tzp);

  if (type == TZNAME_FIXED_OFFSET) {
    /* fixed-offset abbreviation */
    tz = -val;
  } else if (type == TZNAME_DYNTZ) {
    /* dynamic-offset abbreviation, resolve using transaction start time */
    TimestampTz now = GetCurrentTransactionStartTimestamp();
    int isdst;

    tz = DetermineTimeZoneAbbrevOffsetTS(now, tzname, tzp, &isdst);
  } else {
    /* Get the offset-from-GMT that is valid now for the zone name */
    TimestampTz now = GetCurrentTransactionStartTimestamp();
    struct pg_tm tm;
    fsec_t fsec;

    if (timestamp2tm(now, &tz, &tm, &fsec, NULL, tzp) != 0)
      ereport(ERROR, (errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
                      errmsg("timestamp out of range")));
  }

  result = (TimeTzADT *)palloc(sizeof(TimeTzADT));

  result->time = t->time + (t->zone - tz) * USECS_PER_SEC;
  /* C99 modulo has the wrong sign convention for negative input */
  while (result->time < INT64CONST(0))
    result->time += USECS_PER_DAY;
  if (result->time >= USECS_PER_DAY)
    result->time %= USECS_PER_DAY;

  result->zone = tz;

  PG_RETURN_TIMETZADT_P(result);
}

/* timetz_izone()
 * Encode time with time zone type with specified time interval as time zone.
 */
Datum timetz_izone(PG_FUNCTION_ARGS) {
  Interval *zone = PG_GETARG_INTERVAL_P(0);
  TimeTzADT *time = PG_GETARG_TIMETZADT_P(1);
  TimeTzADT *result;
  int tz;

  if (INTERVAL_NOT_FINITE(zone))
    ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                    errmsg("interval time zone \"%s\" must be finite",
                           DatumGetCString(DirectFunctionCall1(
                               interval_out, PointerGetDatum(zone))))));

  if (zone->month != 0 || zone->day != 0)
    ereport(ERROR,
            (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
             errmsg("interval time zone \"%s\" must not include months or days",
                    DatumGetCString(DirectFunctionCall1(
                        interval_out, PointerGetDatum(zone))))));

  tz = -(zone->time / USECS_PER_SEC);

  result = (TimeTzADT *)palloc(sizeof(TimeTzADT));

  result->time = time->time + (time->zone - tz) * USECS_PER_SEC;
  /* C99 modulo has the wrong sign convention for negative input */
  while (result->time < INT64CONST(0))
    result->time += USECS_PER_DAY;
  if (result->time >= USECS_PER_DAY)
    result->time %= USECS_PER_DAY;

  result->zone = tz;

  PG_RETURN_TIMETZADT_P(result);
}

/* timetz_at_local()
 *
 * Unlike for timestamp[tz]_at_local, the type for timetz does not flip between
 * time with/without time zone, so we cannot just call the conversion function.
 */
Datum timetz_at_local(PG_FUNCTION_ARGS) {
  Datum time = PG_GETARG_DATUM(0);
  const char *tzn = pg_get_timezone_name(session_timezone);
  Datum zone = PointerGetDatum(cstring_to_text(tzn));

  return DirectFunctionCall2(timetz_zone, zone, time);
}

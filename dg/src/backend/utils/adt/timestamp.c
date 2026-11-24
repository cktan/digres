/*-------------------------------------------------------------------------
 *
 * timestamp.c
 *	  Functions for the built-in SQL types "timestamp" and "interval".
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 * Portions Copyright (c) 2026, CK Tan: the changes for digres,
 * under the MIT License (https://github.com/cktan/digres/blob/main/LICENSE)
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/timestamp.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <ctype.h>
#include <math.h>
#include <limits.h>
#include <sys/time.h>

#include "access/xact.h"
#include "catalog/pg_type.h"
#include "common/int.h"
#include "common/int128.h"
#include "funcapi.h"
#include "libpq/pqformat.h"
#include "miscadmin.h"
#include "nodes/nodeFuncs.h"
#include "nodes/supportnodes.h"
#include "optimizer/optimizer.h"
#include "parser/scansup.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/control_block.h"
#include "utils/date.h"
#include "utils/datetime.h"
#include "utils/float.h"
#include "utils/numeric.h"
#include "utils/skipsupport.h"
#include "utils/sortsupport.h"
#include "ty_interval.h"
#include "ty_datetime.h"
#include "ty_timezone.h"

/*
 * gcc's -ffast-math switch breaks routines that expect exact results from
 * expressions like timeval / SECS_PER_HOUR, where timeval is double.
 */
#ifdef __FAST_MATH__
#error -ffast-math is known to break this code
#endif

#define SAMESIGN(a, b)	(((a) < 0) == ((b) < 0))

/* Set at postmaster start: pcb->PgStartTime */

/* Set at configuration reload: pcb->PgReloadTime */

typedef struct {
  Timestamp current;
  Timestamp finish;
  Interval step;
  int step_sign;
} generate_series_timestamp_fctx;

typedef struct {
  TimestampTz current;
  TimestampTz finish;
  Interval step;
  int step_sign;
  pg_tz *attimezone;
} generate_series_timestamptz_fctx;

/*
 * The transition datatype for interval aggregates is declared as internal.
 * It's a pointer to an IntervalAggState allocated in the aggregate context.
 */
typedef struct IntervalAggState {
  int64 N;       /* count of finite intervals processed */
  Interval sumX; /* sum of finite intervals processed */
  /* These counts are *not* included in N!  Use IA_TOTAL_COUNT() as needed */
  int64 pInfcount; /* count of +infinity intervals */
  int64 nInfcount; /* count of -infinity intervals */
} IntervalAggState;

#define IA_TOTAL_COUNT(ia) \
	((ia)->N + (ia)->pInfcount + (ia)->nInfcount)

static TimeOffset time2t(const int hour, const int min, const int sec,
                         const fsec_t fsec);
static Timestamp dt2local(Timestamp dt, int timezone);
static bool AdjustIntervalForTypmod(Interval *interval, int32 typmod,
                                    Node *escontext);
static TimestampTz timestamp2timestamptz(Timestamp timestamp);
static Timestamp timestamptz2timestamp(TimestampTz timestamp);

static void EncodeSpecialInterval(const Interval *interval, char *str);
static void interval_um_internal(const Interval *interval, Interval *result);

/* common code for timestamptypmodin and timestamptztypmodin */
static int32 anytimestamp_typmodin(bool istz, ArrayType *ta) {
  int32 *tl;
  int n;

  tl = ArrayGetIntegerTypmods(ta, &n);

  /*
   * we're not too tense about good error message here because grammar
   * shouldn't allow wrong number of modifiers for TIMESTAMP
   */
  if (n != 1)
    ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                    errmsg("invalid type modifier")));

  return anytimestamp_typmod_check(istz, tl[0]);
}

/* exported so parse_expr.c can use it */
int32 anytimestamp_typmod_check(bool istz, int32 typmod) {
  if (typmod < 0)
    ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                    errmsg("TIMESTAMP(%d)%s precision must not be negative",
                           typmod, (istz ? " WITH TIME ZONE" : ""))));
  if (typmod > MAX_TIMESTAMP_PRECISION) {
    ereport(WARNING,
            (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
             errmsg("TIMESTAMP(%d)%s precision reduced to maximum allowed, %d",
                    typmod, (istz ? " WITH TIME ZONE" : ""),
                    MAX_TIMESTAMP_PRECISION)));
    typmod = MAX_TIMESTAMP_PRECISION;
  }

  return typmod;
}

/* common code for timestamptypmodout and timestamptztypmodout */
static char *anytimestamp_typmodout(bool istz, int32 typmod) {
  const char *tz = istz ? " with time zone" : " without time zone";

  if (typmod >= 0)
    return psprintf("(%d)%s", (int)typmod, tz);
  else
    return pstrdup(tz);
}

/*****************************************************************************
 *	 USER I/O ROUTINES
 **
 *****************************************************************************/

/* timestamp_in()
 * Convert a string to internal form.
 */
Datum timestamp_in(PG_FUNCTION_ARGS) {
  char *str = PG_GETARG_CSTRING(0);
#ifdef NOT_USED
  Oid typelem = PG_GETARG_OID(1);
#endif
  int32 typmod = PG_GETARG_INT32(2);
  Node *escontext = fcinfo->context;
  Timestamp result;
  int dterr;
  ty_dtin_extra_t extra;

  /* ty reads the text; the zone words and the clock are the hooks' */
  if (ty_timestamp_parse(str, DateOrder, &ty_dtzone_hooks, &result, &dterr,
                         &extra)) {
    if (dterr != 0) {
      TyDateTimeParseError(dterr, &extra, str, "timestamp", escontext);
      PG_RETURN_NULL();
    }
    ereturn(escontext, (Datum)0,
            (errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
             errmsg("timestamp out of range: \"%s\"", str)));
  }

  AdjustTimestampForTypmod(&result, typmod, escontext);

  PG_RETURN_TIMESTAMP(result);
}

/* timestamp_out()
 * Convert a timestamp to external form.
 */
Datum timestamp_out(PG_FUNCTION_ARGS) {
  Timestamp timestamp = PG_GETARG_TIMESTAMP(0);
  char *result;
  struct pg_tm tt, *tm = &tt;
  fsec_t fsec;
  char buf[MAXDATELEN + 1];

  if (TIMESTAMP_NOT_FINITE(timestamp))
    EncodeSpecialTimestamp(timestamp, buf);
  else if (timestamp2tm(timestamp, NULL, tm, &fsec, NULL, NULL) == 0)
    EncodeDateTime(tm, fsec, false, 0, NULL, DateStyle, buf);
  else
    ereport(ERROR, (errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
                    errmsg("timestamp out of range")));

  result = pstrdup(buf);
  PG_RETURN_CSTRING(result);
}

/*
 *		timestamp_recv			- converts external binary
 *format to timestamp
 */
Datum timestamp_recv(PG_FUNCTION_ARGS) {
  StringInfo buf = (StringInfo)PG_GETARG_POINTER(0);

#ifdef NOT_USED
  Oid typelem = PG_GETARG_OID(1);
#endif
  int32 typmod = PG_GETARG_INT32(2);
  Timestamp timestamp;
  struct pg_tm tt, *tm = &tt;
  fsec_t fsec;

  timestamp = (Timestamp)pq_getmsgint64(buf);

  /* range check: see if timestamp_out would like it */
  if (TIMESTAMP_NOT_FINITE(timestamp))
    /* ok */;
  else if (timestamp2tm(timestamp, NULL, tm, &fsec, NULL, NULL) != 0 ||
           !IS_VALID_TIMESTAMP(timestamp))
    ereport(ERROR, (errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
                    errmsg("timestamp out of range")));

  AdjustTimestampForTypmod(&timestamp, typmod, NULL);

  PG_RETURN_TIMESTAMP(timestamp);
}

/*
 *		timestamp_send			- converts timestamp to binary
 *format
 */
Datum timestamp_send(PG_FUNCTION_ARGS) {
  Timestamp timestamp = PG_GETARG_TIMESTAMP(0);
  StringInfoData buf;

  pq_begintypsend(&buf);
  pq_sendint64(&buf, timestamp);
  PG_RETURN_BYTEA_P(pq_endtypsend(&buf));
}

Datum timestamptypmodin(PG_FUNCTION_ARGS) {
  ArrayType *ta = PG_GETARG_ARRAYTYPE_P(0);

  PG_RETURN_INT32(anytimestamp_typmodin(false, ta));
}

Datum timestamptypmodout(PG_FUNCTION_ARGS) {
  int32 typmod = PG_GETARG_INT32(0);

  PG_RETURN_CSTRING(anytimestamp_typmodout(false, typmod));
}

/*
 * timestamp_support()
 *
 * Planner support function for the timestamp_scale() and timestamptz_scale()
 * length coercion functions (we need not distinguish them here).
 */
Datum timestamp_support(PG_FUNCTION_ARGS) {
  Node *rawreq = (Node *)PG_GETARG_POINTER(0);
  Node *ret = NULL;

  if (IsA(rawreq, SupportRequestSimplify)) {
    SupportRequestSimplify *req = (SupportRequestSimplify *)rawreq;

    ret = TemporalSimplify(MAX_TIMESTAMP_PRECISION, (Node *)req->fcall);
  }

  PG_RETURN_POINTER(ret);
}

/* timestamp_scale()
 * Adjust time type for specified scale factor.
 * Used by PostgreSQL type system to stuff columns.
 */
Datum timestamp_scale(PG_FUNCTION_ARGS) {
  Timestamp timestamp = PG_GETARG_TIMESTAMP(0);
  int32 typmod = PG_GETARG_INT32(1);
  Timestamp result;

  result = timestamp;

  AdjustTimestampForTypmod(&result, typmod, NULL);

  PG_RETURN_TIMESTAMP(result);
}

/*
 * AdjustTimestampForTypmod --- round off a timestamp to suit given typmod
 * Works for either timestamp or timestamptz.
 *
 * Returns true on success, false on failure (if escontext points to an
 * ErrorSaveContext; otherwise errors are thrown).
 */
bool AdjustTimestampForTypmod(Timestamp *time, int32 typmod, Node *escontext) {
  if (ty_timestamp_adjust_typmod(time, typmod))
    ereturn(escontext, false,
            (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
             errmsg("%s", ty_errmsg())));
  return true;
}

/* timestamptz_in()
 * Convert a string to internal form.
 */
Datum timestamptz_in(PG_FUNCTION_ARGS) {
  char *str = PG_GETARG_CSTRING(0);
#ifdef NOT_USED
  Oid typelem = PG_GETARG_OID(1);
#endif
  int32 typmod = PG_GETARG_INT32(2);
  Node *escontext = fcinfo->context;
  TimestampTz result;
  int dterr;
  ty_dtin_extra_t extra;

  /* ty reads the text; the zones, with their offsets, are the hooks' */
  if (ty_timestamptz_parse(str, DateOrder, &ty_dtzone_hooks_tz, &result,
                           &dterr, &extra)) {
    if (dterr != 0) {
      TyDateTimeParseError(dterr, &extra, str, "timestamp with time zone",
                           escontext);
      PG_RETURN_NULL();
    }
    ereturn(escontext, (Datum)0,
            (errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
             errmsg("timestamp out of range: \"%s\"", str)));
  }

  AdjustTimestampForTypmod(&result, typmod, escontext);

  PG_RETURN_TIMESTAMPTZ(result);
}

/*
 * Try to parse a timezone specification, and return its timezone offset value
 * if it's acceptable.  Otherwise, an error is thrown.
 *
 * Note: some code paths update tm->tm_isdst, and some don't; current callers
 * don't care, so we don't bother being consistent.
 */
static int parse_sane_timezone(struct pg_tm *tm, text *zone) {
  char tzname[TZ_STRLEN_MAX + 1];
  int dterr;
  int tz;

  text_to_cstring_buffer(zone, tzname, sizeof(tzname));

  /*
   * Look up the requested timezone.  First we try to interpret it as a
   * numeric timezone specification; if DecodeTimezone decides it doesn't
   * like the format, we try timezone abbreviations and names.
   *
   * Note pg_tzset happily parses numeric input that DecodeTimezone would
   * reject.  To avoid having it accept input that would otherwise be seen
   * as invalid, it's enough to disallow having a digit in the first
   * position of our input string.
   */
  if (isdigit((unsigned char)*tzname))
    ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                    errmsg("invalid input syntax for type %s: \"%s\"",
                           "numeric time zone", tzname),
                    errhint("Numeric time zones must have \"-\" or \"+\" as "
                            "first character.")));

  dterr = DecodeTimezone(tzname, &tz);
  if (dterr != 0) {
    int type, val;
    pg_tz *tzp;

    if (dterr == DTERR_TZDISP_OVERFLOW)
      ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                      errmsg("numeric time zone \"%s\" out of range", tzname)));
    else if (dterr != DTERR_BAD_FORMAT)
      ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                      errmsg("time zone \"%s\" not recognized", tzname)));

    type = DecodeTimezoneName(tzname, &val, &tzp);

    if (type == TZNAME_FIXED_OFFSET) {
      /* fixed-offset abbreviation */
      tz = -val;
    } else if (type == TZNAME_DYNTZ) {
      /* dynamic-offset abbreviation, resolve using specified time */
      tz = DetermineTimeZoneAbbrevOffset(tm, tzname, tzp);
    } else {
      /* full zone name */
      tz = DetermineTimeZoneOffset(tm, tzp);
    }
  }

  return tz;
}

/*
 * Look up the requested timezone, returning a pg_tz struct.
 *
 * This is the same as DecodeTimezoneNameToTz, but starting with a text Datum.
 */
static pg_tz *lookup_timezone(text *zone) {
  char tzname[TZ_STRLEN_MAX + 1];

  text_to_cstring_buffer(zone, tzname, sizeof(tzname));

  return DecodeTimezoneNameToTz(tzname);
}

/*
 * make_timestamp_internal
 *		workhorse for make_timestamp and make_timestamptz
 */
static Timestamp make_timestamp_internal(int year, int month, int day, int hour,
                                         int min, double sec) {
  int64 result;

  /* ty checks the fields and the range, with PostgreSQL's messages */
  if (ty_timestamp_make(year, month, day, hour, min, sec, &result))
    ereport(ERROR, (errcode(ty_errnum() == TY_ERR_INVALID
                                ? ERRCODE_DATETIME_FIELD_OVERFLOW
                                : ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
                    errmsg("%s", ty_errmsg())));
  return result;
}

/*
 * make_timestamp() - timestamp constructor
 */
Datum make_timestamp(PG_FUNCTION_ARGS) {
  int32 year = PG_GETARG_INT32(0);
  int32 month = PG_GETARG_INT32(1);
  int32 mday = PG_GETARG_INT32(2);
  int32 hour = PG_GETARG_INT32(3);
  int32 min = PG_GETARG_INT32(4);
  float8 sec = PG_GETARG_FLOAT8(5);
  Timestamp result;

  result = make_timestamp_internal(year, month, mday, hour, min, sec);

  PG_RETURN_TIMESTAMP(result);
}

/*
 * make_timestamptz() - timestamp with time zone constructor
 */
Datum make_timestamptz(PG_FUNCTION_ARGS) {
  int32 year = PG_GETARG_INT32(0);
  int32 month = PG_GETARG_INT32(1);
  int32 mday = PG_GETARG_INT32(2);
  int32 hour = PG_GETARG_INT32(3);
  int32 min = PG_GETARG_INT32(4);
  float8 sec = PG_GETARG_FLOAT8(5);
  Timestamp result;

  result = make_timestamp_internal(year, month, mday, hour, min, sec);

  PG_RETURN_TIMESTAMPTZ(timestamp2timestamptz(result));
}

/*
 * Construct a timestamp with time zone.
 *		As above, but the time zone is specified as seventh argument.
 */
Datum make_timestamptz_at_timezone(PG_FUNCTION_ARGS) {
  int32 year = PG_GETARG_INT32(0);
  int32 month = PG_GETARG_INT32(1);
  int32 mday = PG_GETARG_INT32(2);
  int32 hour = PG_GETARG_INT32(3);
  int32 min = PG_GETARG_INT32(4);
  float8 sec = PG_GETARG_FLOAT8(5);
  text *zone = PG_GETARG_TEXT_PP(6);
  TimestampTz result;
  Timestamp timestamp;
  struct pg_tm tt;
  int tz;
  fsec_t fsec;

  timestamp = make_timestamp_internal(year, month, mday, hour, min, sec);

  if (timestamp2tm(timestamp, NULL, &tt, &fsec, NULL, NULL) != 0)
    ereport(ERROR, (errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
                    errmsg("timestamp out of range")));

  tz = parse_sane_timezone(&tt, zone);

  result = dt2local(timestamp, -tz);

  if (!IS_VALID_TIMESTAMP(result))
    ereport(ERROR, (errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
                    errmsg("timestamp out of range")));

  PG_RETURN_TIMESTAMPTZ(result);
}

/*
 * to_timestamp(double precision)
 * Convert UNIX epoch to timestamptz.
 */
Datum float8_timestamptz(PG_FUNCTION_ARGS) {
  float8 seconds = PG_GETARG_FLOAT8(0);
  TimestampTz result;

  /* Deal with NaN and infinite inputs ... */
  if (isnan(seconds))
    ereport(ERROR, (errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
                    errmsg("timestamp cannot be NaN")));

  if (isinf(seconds)) {
    if (seconds < 0)
      TIMESTAMP_NOBEGIN(result);
    else
      TIMESTAMP_NOEND(result);
  } else {
    /* Out of range? */
    if (seconds <
            (float8)SECS_PER_DAY * (DATETIME_MIN_JULIAN - UNIX_EPOCH_JDATE) ||
        seconds >=
            (float8)SECS_PER_DAY * (TIMESTAMP_END_JULIAN - UNIX_EPOCH_JDATE))
      ereport(ERROR, (errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
                      errmsg("timestamp out of range: \"%g\"", seconds)));

    /* Convert UNIX epoch to Postgres epoch */
    seconds -= ((POSTGRES_EPOCH_JDATE - UNIX_EPOCH_JDATE) * SECS_PER_DAY);

    seconds = rint(seconds * USECS_PER_SEC);
    result = (int64)seconds;

    /* Recheck in case roundoff produces something just out of range */
    if (!IS_VALID_TIMESTAMP(result))
      ereport(ERROR,
              (errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
               errmsg("timestamp out of range: \"%g\"", PG_GETARG_FLOAT8(0))));
  }

  PG_RETURN_TIMESTAMP(result);
}

/* timestamptz_out()
 * Convert a timestamp to external form.
 */
Datum timestamptz_out(PG_FUNCTION_ARGS) {
  TimestampTz dt = PG_GETARG_TIMESTAMPTZ(0);
  char *result;
  int tz;
  struct pg_tm tt, *tm = &tt;
  fsec_t fsec;
  const char *tzn;
  char buf[MAXDATELEN + 1];

  if (TIMESTAMP_NOT_FINITE(dt))
    EncodeSpecialTimestamp(dt, buf);
  else if (timestamp2tm(dt, &tz, tm, &fsec, &tzn, NULL) == 0)
    EncodeDateTime(tm, fsec, true, tz, tzn, DateStyle, buf);
  else
    ereport(ERROR, (errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
                    errmsg("timestamp out of range")));

  result = pstrdup(buf);
  PG_RETURN_CSTRING(result);
}

/*
 *		timestamptz_recv			- converts external
 *binary format to timestamptz
 */
Datum timestamptz_recv(PG_FUNCTION_ARGS) {
  StringInfo buf = (StringInfo)PG_GETARG_POINTER(0);

#ifdef NOT_USED
  Oid typelem = PG_GETARG_OID(1);
#endif
  int32 typmod = PG_GETARG_INT32(2);
  TimestampTz timestamp;
  int tz;
  struct pg_tm tt, *tm = &tt;
  fsec_t fsec;

  timestamp = (TimestampTz)pq_getmsgint64(buf);

  /* range check: see if timestamptz_out would like it */
  if (TIMESTAMP_NOT_FINITE(timestamp))
    /* ok */;
  else if (timestamp2tm(timestamp, &tz, tm, &fsec, NULL, NULL) != 0 ||
           !IS_VALID_TIMESTAMP(timestamp))
    ereport(ERROR, (errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
                    errmsg("timestamp out of range")));

  AdjustTimestampForTypmod(&timestamp, typmod, NULL);

  PG_RETURN_TIMESTAMPTZ(timestamp);
}

/*
 *		timestamptz_send			- converts timestamptz
 *to binary format
 */
Datum timestamptz_send(PG_FUNCTION_ARGS) {
  TimestampTz timestamp = PG_GETARG_TIMESTAMPTZ(0);
  StringInfoData buf;

  pq_begintypsend(&buf);
  pq_sendint64(&buf, timestamp);
  PG_RETURN_BYTEA_P(pq_endtypsend(&buf));
}

Datum timestamptztypmodin(PG_FUNCTION_ARGS) {
  ArrayType *ta = PG_GETARG_ARRAYTYPE_P(0);

  PG_RETURN_INT32(anytimestamp_typmodin(true, ta));
}

Datum timestamptztypmodout(PG_FUNCTION_ARGS) {
  int32 typmod = PG_GETARG_INT32(0);

  PG_RETURN_CSTRING(anytimestamp_typmodout(true, typmod));
}

/* timestamptz_scale()
 * Adjust time type for specified scale factor.
 * Used by PostgreSQL type system to stuff columns.
 */
Datum timestamptz_scale(PG_FUNCTION_ARGS) {
  TimestampTz timestamp = PG_GETARG_TIMESTAMPTZ(0);
  int32 typmod = PG_GETARG_INT32(1);
  TimestampTz result;

  result = timestamp;

  AdjustTimestampForTypmod(&result, typmod, NULL);

  PG_RETURN_TIMESTAMPTZ(result);
}

/* interval_in()
 * Convert a string to internal form.
 *
 * External format(s):
 *	Uses the generic date/time parsing and decoding routines.
 */
Datum interval_in(PG_FUNCTION_ARGS) {
  char *str = PG_GETARG_CSTRING(0);
#ifdef NOT_USED
  Oid typelem = PG_GETARG_OID(1);
#endif
  int32 typmod = PG_GETARG_INT32(2);
  Node *escontext = fcinfo->context;
  Interval *result;
  int range;
  int dterr;
  DateTimeErrorExtra extra;

  if (typmod >= 0)
    range = INTERVAL_RANGE(typmod);
  else
    range = INTERVAL_FULL_RANGE;

  result = (Interval *)palloc(sizeof(Interval));

  /* ty reads the text, in the postgres, sql and ISO 8601 forms */
  if (ty_interval_parse(str, range, tcb->guc.IntervalStyle,
                        (ty_interval_t *)result, &dterr)) {
    if (dterr == 1)
      ereturn(escontext, (Datum)0,
              (errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
               errmsg("interval out of range")));
    DateTimeParseError(dterr, &extra, str, "interval", escontext);
    PG_RETURN_NULL();
  }

  AdjustIntervalForTypmod(result, typmod, escontext);

  PG_RETURN_INTERVAL_P(result);
}

/* interval_out()
 * Convert a time span to external form.
 */
Datum interval_out(PG_FUNCTION_ARGS) {
  Interval *span = PG_GETARG_INTERVAL_P(0);
  char *result;
  struct pg_itm tt, *itm = &tt;
  char buf[MAXDATELEN + 1];

  if (INTERVAL_NOT_FINITE(span))
    EncodeSpecialInterval(span, buf);
  else {
    interval2itm(*span, itm);
    EncodeInterval(itm, tcb->guc.IntervalStyle, buf);
  }

  result = pstrdup(buf);
  PG_RETURN_CSTRING(result);
}

/*
 *		interval_recv			- converts external binary
 *format to interval
 */
Datum interval_recv(PG_FUNCTION_ARGS) {
  StringInfo buf = (StringInfo)PG_GETARG_POINTER(0);

#ifdef NOT_USED
  Oid typelem = PG_GETARG_OID(1);
#endif
  int32 typmod = PG_GETARG_INT32(2);
  Interval *interval;

  interval = (Interval *)palloc(sizeof(Interval));

  interval->time = pq_getmsgint64(buf);
  interval->day = pq_getmsgint(buf, sizeof(interval->day));
  interval->month = pq_getmsgint(buf, sizeof(interval->month));

  AdjustIntervalForTypmod(interval, typmod, NULL);

  PG_RETURN_INTERVAL_P(interval);
}

/*
 *		interval_send			- converts interval to binary
 *format
 */
Datum interval_send(PG_FUNCTION_ARGS) {
  Interval *interval = PG_GETARG_INTERVAL_P(0);
  StringInfoData buf;

  pq_begintypsend(&buf);
  pq_sendint64(&buf, interval->time);
  pq_sendint32(&buf, interval->day);
  pq_sendint32(&buf, interval->month);
  PG_RETURN_BYTEA_P(pq_endtypsend(&buf));
}

/*
 * The interval typmod stores a "range" in its high 16 bits and a "precision"
 * in its low 16 bits.  Both contribute to defining the resolution of the
 * type.  Range addresses resolution granules larger than one second, and
 * precision specifies resolution below one second.  This representation can
 * express all SQL standard resolutions, but we implement them all in terms of
 * truncating rightward from some position.  Range is a bitmap of permitted
 * fields, but only the temporally-smallest such field is significant to our
 * calculations.  Precision is a count of sub-second decimal places to retain.
 * Setting all bits (INTERVAL_FULL_PRECISION) gives the same truncation
 * semantics as choosing MAX_INTERVAL_PRECISION.
 */
Datum intervaltypmodin(PG_FUNCTION_ARGS) {
  ArrayType *ta = PG_GETARG_ARRAYTYPE_P(0);
  int32 *tl;
  int n;
  int32 typmod;

  tl = ArrayGetIntegerTypmods(ta, &n);

  /*
   * tl[0] - interval range (fields bitmask)	tl[1] - precision (optional)
   *
   * Note we must validate tl[0] even though it's normally guaranteed
   * correct by the grammar --- consider SELECT 'foo'::"interval"(1000).
   */
  if (n > 0) {
    switch (tl[0]) {
    case INTERVAL_MASK(YEAR):
    case INTERVAL_MASK(MONTH):
    case INTERVAL_MASK(DAY):
    case INTERVAL_MASK(HOUR):
    case INTERVAL_MASK(MINUTE):
    case INTERVAL_MASK(SECOND):
    case INTERVAL_MASK(YEAR) | INTERVAL_MASK(MONTH):
    case INTERVAL_MASK(DAY) | INTERVAL_MASK(HOUR):
    case INTERVAL_MASK(DAY) | INTERVAL_MASK(HOUR) | INTERVAL_MASK(MINUTE):
    case INTERVAL_MASK(DAY) | INTERVAL_MASK(HOUR) | INTERVAL_MASK(MINUTE) |
        INTERVAL_MASK(SECOND):
    case INTERVAL_MASK(HOUR) | INTERVAL_MASK(MINUTE):
    case INTERVAL_MASK(HOUR) | INTERVAL_MASK(MINUTE) | INTERVAL_MASK(SECOND):
    case INTERVAL_MASK(MINUTE) | INTERVAL_MASK(SECOND):
    case INTERVAL_FULL_RANGE:
      /* all OK */
      break;
    default:
      ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                      errmsg("invalid INTERVAL type modifier")));
    }
  }

  if (n == 1) {
    if (tl[0] != INTERVAL_FULL_RANGE)
      typmod = INTERVAL_TYPMOD(INTERVAL_FULL_PRECISION, tl[0]);
    else
      typmod = -1;
  } else if (n == 2) {
    if (tl[1] < 0)
      ereport(ERROR,
              (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
               errmsg("INTERVAL(%d) precision must not be negative", tl[1])));
    if (tl[1] > MAX_INTERVAL_PRECISION) {
      ereport(WARNING,
              (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
               errmsg("INTERVAL(%d) precision reduced to maximum allowed, %d",
                      tl[1], MAX_INTERVAL_PRECISION)));
      typmod = INTERVAL_TYPMOD(MAX_INTERVAL_PRECISION, tl[0]);
    } else
      typmod = INTERVAL_TYPMOD(tl[1], tl[0]);
  } else {
    ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                    errmsg("invalid INTERVAL type modifier")));
    typmod = 0; /* keep compiler quiet */
  }

  PG_RETURN_INT32(typmod);
}

Datum intervaltypmodout(PG_FUNCTION_ARGS) {
  int32 typmod = PG_GETARG_INT32(0);
  char *res = (char *)palloc(64);
  int fields;
  int precision;
  const char *fieldstr;

  if (typmod < 0) {
    *res = '\0';
    PG_RETURN_CSTRING(res);
  }

  fields = INTERVAL_RANGE(typmod);
  precision = INTERVAL_PRECISION(typmod);

  switch (fields) {
  case INTERVAL_MASK(YEAR):
    fieldstr = " year";
    break;
  case INTERVAL_MASK(MONTH):
    fieldstr = " month";
    break;
  case INTERVAL_MASK(DAY):
    fieldstr = " day";
    break;
  case INTERVAL_MASK(HOUR):
    fieldstr = " hour";
    break;
  case INTERVAL_MASK(MINUTE):
    fieldstr = " minute";
    break;
  case INTERVAL_MASK(SECOND):
    fieldstr = " second";
    break;
  case INTERVAL_MASK(YEAR) | INTERVAL_MASK(MONTH):
    fieldstr = " year to month";
    break;
  case INTERVAL_MASK(DAY) | INTERVAL_MASK(HOUR):
    fieldstr = " day to hour";
    break;
  case INTERVAL_MASK(DAY) | INTERVAL_MASK(HOUR) | INTERVAL_MASK(MINUTE):
    fieldstr = " day to minute";
    break;
  case INTERVAL_MASK(DAY) | INTERVAL_MASK(HOUR) | INTERVAL_MASK(MINUTE) |
      INTERVAL_MASK(SECOND):
    fieldstr = " day to second";
    break;
  case INTERVAL_MASK(HOUR) | INTERVAL_MASK(MINUTE):
    fieldstr = " hour to minute";
    break;
  case INTERVAL_MASK(HOUR) | INTERVAL_MASK(MINUTE) | INTERVAL_MASK(SECOND):
    fieldstr = " hour to second";
    break;
  case INTERVAL_MASK(MINUTE) | INTERVAL_MASK(SECOND):
    fieldstr = " minute to second";
    break;
  case INTERVAL_FULL_RANGE:
    fieldstr = "";
    break;
  default:
    elog(ERROR, "invalid INTERVAL typmod: 0x%x", typmod);
    fieldstr = "";
    break;
  }

  if (precision != INTERVAL_FULL_PRECISION)
    snprintf(res, 64, "%s(%d)", fieldstr, precision);
  else
    snprintf(res, 64, "%s", fieldstr);

  PG_RETURN_CSTRING(res);
}

/*
 * Given an interval typmod value, return a code for the least-significant
 * field that the typmod allows to be nonzero, for instance given
 * INTERVAL DAY TO HOUR we want to identify "hour".
 *
 * The results should be ordered by field significance, which means
 * we can't use the dt.h macros YEAR etc, because for some odd reason
 * they aren't ordered that way.  Instead, arbitrarily represent
 * SECOND = 0, MINUTE = 1, HOUR = 2, DAY = 3, MONTH = 4, YEAR = 5.
 */
static int intervaltypmodleastfield(int32 typmod) {
  if (typmod < 0)
    return 0; /* SECOND */

  switch (INTERVAL_RANGE(typmod)) {
  case INTERVAL_MASK(YEAR):
    return 5; /* YEAR */
  case INTERVAL_MASK(MONTH):
    return 4; /* MONTH */
  case INTERVAL_MASK(DAY):
    return 3; /* DAY */
  case INTERVAL_MASK(HOUR):
    return 2; /* HOUR */
  case INTERVAL_MASK(MINUTE):
    return 1; /* MINUTE */
  case INTERVAL_MASK(SECOND):
    return 0; /* SECOND */
  case INTERVAL_MASK(YEAR) | INTERVAL_MASK(MONTH):
    return 4; /* MONTH */
  case INTERVAL_MASK(DAY) | INTERVAL_MASK(HOUR):
    return 2; /* HOUR */
  case INTERVAL_MASK(DAY) | INTERVAL_MASK(HOUR) | INTERVAL_MASK(MINUTE):
    return 1; /* MINUTE */
  case INTERVAL_MASK(DAY) | INTERVAL_MASK(HOUR) | INTERVAL_MASK(MINUTE) |
      INTERVAL_MASK(SECOND):
    return 0; /* SECOND */
  case INTERVAL_MASK(HOUR) | INTERVAL_MASK(MINUTE):
    return 1; /* MINUTE */
  case INTERVAL_MASK(HOUR) | INTERVAL_MASK(MINUTE) | INTERVAL_MASK(SECOND):
    return 0; /* SECOND */
  case INTERVAL_MASK(MINUTE) | INTERVAL_MASK(SECOND):
    return 0; /* SECOND */
  case INTERVAL_FULL_RANGE:
    return 0; /* SECOND */
  default:
    elog(ERROR, "invalid INTERVAL typmod: 0x%x", typmod);
    break;
  }
  return 0; /* can't get here, but keep compiler quiet */
}

/*
 * interval_support()
 *
 * Planner support function for interval_scale().
 *
 * Flatten superfluous calls to interval_scale().  The interval typmod is
 * complex to permit accepting and regurgitating all SQL standard variations.
 * For truncation purposes, it boils down to a single, simple granularity.
 */
Datum interval_support(PG_FUNCTION_ARGS) {
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
      int32 new_typmod = DatumGetInt32(((Const *)typmod)->constvalue);
      bool noop;

      if (new_typmod < 0)
        noop = true;
      else {
        int32 old_typmod = exprTypmod(source);
        int old_least_field;
        int new_least_field;
        int old_precis;
        int new_precis;

        old_least_field = intervaltypmodleastfield(old_typmod);
        new_least_field = intervaltypmodleastfield(new_typmod);
        if (old_typmod < 0)
          old_precis = INTERVAL_FULL_PRECISION;
        else
          old_precis = INTERVAL_PRECISION(old_typmod);
        new_precis = INTERVAL_PRECISION(new_typmod);

        /*
         * Cast is a no-op if least field stays the same or decreases
         * while precision stays the same or increases.  But
         * precision, which is to say, sub-second precision, only
         * affects ranges that include SECOND.
         */
        noop =
            (new_least_field <= old_least_field) &&
            (old_least_field > 0 /* SECOND */ ||
             new_precis >= MAX_INTERVAL_PRECISION || new_precis >= old_precis);
      }
      if (noop)
        ret = relabel_to_typmod(source, new_typmod);
    }
  }

  PG_RETURN_POINTER(ret);
}

/* interval_scale()
 * Adjust interval type for specified fields.
 * Used by PostgreSQL type system to stuff columns.
 */
Datum interval_scale(PG_FUNCTION_ARGS) {
  Interval *interval = PG_GETARG_INTERVAL_P(0);
  int32 typmod = PG_GETARG_INT32(1);
  Interval *result;

  result = (Interval *)palloc(sizeof(Interval));
  *result = *interval;

  AdjustIntervalForTypmod(result, typmod, NULL);

  PG_RETURN_INTERVAL_P(result);
}

/*
 *	Adjust interval for specified precision, in both YEAR to SECOND
 *	range and sub-second precision.
 *
 * Returns true on success, false on failure (if escontext points to an
 * ErrorSaveContext; otherwise errors are thrown).
 */
static bool AdjustIntervalForTypmod(Interval *interval, int32 typmod,
                                    Node *escontext) {
  /* ty cuts the fields to the range and rounds to the precision */
  if (ty_interval_adjust_typmod((ty_interval_t *)interval, typmod)) {
    if (ty_errnum() == TY_ERR_INVALID)
      ereturn(escontext, false,
              (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
               errmsg("%s", ty_errmsg())));
    ereturn(escontext, false,
            (errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
             errmsg("interval out of range")));
  }
  return true;
}

/*
 * make_interval - numeric Interval constructor
 */
Datum make_interval(PG_FUNCTION_ARGS) {
  Interval *result = (Interval *)palloc(sizeof(Interval));

  if (ty_interval_make(PG_GETARG_INT32(0), PG_GETARG_INT32(1),
                       PG_GETARG_INT32(2), PG_GETARG_INT32(3),
                       PG_GETARG_INT32(4), PG_GETARG_INT32(5),
                       PG_GETARG_FLOAT8(6), (ty_interval_t *)result)) {
    if (ty_errnum() != TY_ERR_RANGE || strcmp(ty_errmsg(), "interval out of range"))
      float_raise_ty(); /* the seconds times a million overflowed */
    ereport(ERROR, (errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
                    errmsg("interval out of range")));
  }

  PG_RETURN_INTERVAL_P(result);
}

/* EncodeSpecialTimestamp()
 * Convert reserved timestamp data type to string.
 */
void EncodeSpecialTimestamp(Timestamp dt, char *str) {
  if (TIMESTAMP_IS_NOBEGIN(dt))
    strcpy(str, EARLY);
  else if (TIMESTAMP_IS_NOEND(dt))
    strcpy(str, LATE);
  else /* shouldn't happen */
    elog(ERROR, "invalid argument for EncodeSpecialTimestamp");
}

static void EncodeSpecialInterval(const Interval *interval, char *str) {
  if (INTERVAL_IS_NOBEGIN(interval))
    strcpy(str, EARLY);
  else if (INTERVAL_IS_NOEND(interval))
    strcpy(str, LATE);
  else /* shouldn't happen */
    elog(ERROR, "invalid argument for EncodeSpecialInterval");
}

Datum now(PG_FUNCTION_ARGS) {
  PG_RETURN_TIMESTAMPTZ(GetCurrentTransactionStartTimestamp());
}

Datum statement_timestamp(PG_FUNCTION_ARGS) {
  PG_RETURN_TIMESTAMPTZ(GetCurrentStatementStartTimestamp());
}

Datum clock_timestamp(PG_FUNCTION_ARGS) {
  PG_RETURN_TIMESTAMPTZ(GetCurrentTimestamp());
}

Datum pg_postmaster_start_time(PG_FUNCTION_ARGS) {
  PG_RETURN_TIMESTAMPTZ(pcb->PgStartTime);
}

Datum pg_conf_load_time(PG_FUNCTION_ARGS) {
  PG_RETURN_TIMESTAMPTZ(pcb->PgReloadTime.load(std::memory_order_relaxed));
}

/*
 * GetCurrentTimestamp -- get the current operating system time
 *
 * Result is in the form of a TimestampTz value, and is expressed to the
 * full precision of the gettimeofday() syscall
 */
TimestampTz GetCurrentTimestamp(void) {
  TimestampTz result;
  struct timeval tp;

  gettimeofday(&tp, NULL);

  result = (TimestampTz)tp.tv_sec -
           ((POSTGRES_EPOCH_JDATE - UNIX_EPOCH_JDATE) * SECS_PER_DAY);
  result = (result * USECS_PER_SEC) + tp.tv_usec;

  return result;
}

/*
 * GetSQLCurrentTimestamp -- implements CURRENT_TIMESTAMP, CURRENT_TIMESTAMP(n)
 */
TimestampTz GetSQLCurrentTimestamp(int32 typmod) {
  TimestampTz ts;

  ts = GetCurrentTransactionStartTimestamp();
  if (typmod >= 0)
    AdjustTimestampForTypmod(&ts, typmod, NULL);
  return ts;
}

/*
 * GetSQLLocalTimestamp -- implements LOCALTIMESTAMP, LOCALTIMESTAMP(n)
 */
Timestamp GetSQLLocalTimestamp(int32 typmod) {
  Timestamp ts;

  ts = timestamptz2timestamp(GetCurrentTransactionStartTimestamp());
  if (typmod >= 0)
    AdjustTimestampForTypmod(&ts, typmod, NULL);
  return ts;
}

/*
 * timeofday(*) -- returns the current time as a text.
 */
Datum timeofday(PG_FUNCTION_ARGS) {
  struct timeval tp;
  pg_time_t tt;
  struct pg_tm *tm;
  char part1[128];
  char part2[128];
  char buf[128 + 128 + 10];

  gettimeofday(&tp, NULL);
  tt = (pg_time_t)tp.tv_sec;
  tm = pg_localtime(&tt, session_timezone);

  pg_strftime(part1, sizeof(part1), "%a %b %d %H:%M:%S", tm);
  pg_strftime(part2, sizeof(part2), "%Y %Z", tm);
  snprintf(buf, sizeof(buf), "%s.%06d %s", part1, (int)tp.tv_usec, part2);

  PG_RETURN_TEXT_P(cstring_to_text(buf));
}

/*
 * TimestampDifference -- convert the difference between two timestamps
 *		into integer seconds and microseconds
 *
 * This is typically used to calculate a wait timeout for select(2),
 * which explains the otherwise-odd choice of output format.
 *
 * Both inputs must be ordinary finite timestamps (in current usage,
 * they'll be results from GetCurrentTimestamp()).
 *
 * We expect start_time <= stop_time.  If not, we return zeros,
 * since then we're already past the previously determined stop_time.
 */
void TimestampDifference(TimestampTz start_time, TimestampTz stop_time,
                         long *secs, int *microsecs) {
  TimestampTz diff = stop_time - start_time;

  if (diff <= 0) {
    *secs = 0;
    *microsecs = 0;
  } else {
    *secs = (long)(diff / USECS_PER_SEC);
    *microsecs = (int)(diff % USECS_PER_SEC);
  }
}

/*
 * TimestampDifferenceMilliseconds -- convert the difference between two
 * 		timestamps into integer milliseconds
 *
 * This is typically used to calculate a wait timeout for WaitLatch()
 * or a related function.  The choice of "long" as the result type
 * is to harmonize with that; furthermore, we clamp the result to at most
 * INT_MAX milliseconds, because that's all that WaitLatch() allows.
 *
 * We expect start_time <= stop_time.  If not, we return zero,
 * since then we're already past the previously determined stop_time.
 *
 * Subtracting finite and infinite timestamps works correctly, returning
 * zero or INT_MAX as appropriate.
 *
 * Note we round up any fractional millisecond, since waiting for just
 * less than the intended timeout is undesirable.
 */
long TimestampDifferenceMilliseconds(TimestampTz start_time,
                                     TimestampTz stop_time) {
  TimestampTz diff;

  /* Deal with zero or negative elapsed time quickly. */
  if (start_time >= stop_time)
    return 0;
  /* To not fail with timestamp infinities, we must detect overflow. */
  if (pg_sub_s64_overflow(stop_time, start_time, &diff))
    return (long)INT_MAX;
  if (diff >= (INT_MAX * INT64CONST(1000) - 999))
    return (long)INT_MAX;
  else
    return (long)((diff + 999) / 1000);
}

/*
 * TimestampDifferenceExceeds -- report whether the difference between two
 *		timestamps is >= a threshold (expressed in milliseconds)
 *
 * Both inputs must be ordinary finite timestamps (in current usage,
 * they'll be results from GetCurrentTimestamp()).
 */
bool TimestampDifferenceExceeds(TimestampTz start_time, TimestampTz stop_time,
                                int msec) {
  TimestampTz diff = stop_time - start_time;

  return (diff >= msec * INT64CONST(1000));
}

/*
 * Check if the difference between two timestamps is >= a given
 * threshold (expressed in seconds).
 */
bool TimestampDifferenceExceedsSeconds(TimestampTz start_time,
                                       TimestampTz stop_time,
                                       int threshold_sec) {
  long secs;
  int usecs;

  /* Calculate the difference in seconds */
  TimestampDifference(start_time, stop_time, &secs, &usecs);

  return (secs >= threshold_sec);
}

/*
 * Convert a time_t to TimestampTz.
 *
 * We do not use time_t internally in Postgres, but this is provided for use
 * by functions that need to interpret, say, a stat(2) result.
 *
 * To avoid having the function's ABI vary depending on the width of time_t,
 * we declare the argument as pg_time_t, which is cast-compatible with
 * time_t but always 64 bits wide (unless the platform has no 64-bit type).
 * This detail should be invisible to callers, at least at source code level.
 */
TimestampTz time_t_to_timestamptz(pg_time_t tm) {
  TimestampTz result;

  result = (TimestampTz)tm -
           ((POSTGRES_EPOCH_JDATE - UNIX_EPOCH_JDATE) * SECS_PER_DAY);
  result *= USECS_PER_SEC;

  return result;
}

/*
 * Convert a TimestampTz to time_t.
 *
 * This too is just marginally useful, but some places need it.
 *
 * To avoid having the function's ABI vary depending on the width of time_t,
 * we declare the result as pg_time_t, which is cast-compatible with
 * time_t but always 64 bits wide (unless the platform has no 64-bit type).
 * This detail should be invisible to callers, at least at source code level.
 */
pg_time_t timestamptz_to_time_t(TimestampTz t) {
  pg_time_t result;

  result =
      (pg_time_t)(t / USECS_PER_SEC +
                  ((POSTGRES_EPOCH_JDATE - UNIX_EPOCH_JDATE) * SECS_PER_DAY));

  return result;
}

/*
 * Produce a C-string representation of a TimestampTz.
 *
 * This is mostly for use in emitting messages.  The primary difference
 * from timestamptz_out is that we force the output format to ISO.  Note
 * also that the result is in a static buffer, not pstrdup'd.
 *
 * See also pg_strftime.
 */
const char *timestamptz_to_str(TimestampTz t) {
  static thread_local /*LOCAL*/ char buf[MAXDATELEN + 1];
  int tz;
  struct pg_tm tt, *tm = &tt;
  fsec_t fsec;
  const char *tzn;

  if (TIMESTAMP_NOT_FINITE(t))
    EncodeSpecialTimestamp(t, buf);
  else if (timestamp2tm(t, &tz, tm, &fsec, &tzn, NULL) == 0)
    EncodeDateTime(tm, fsec, true, tz, tzn, USE_ISO_DATES, buf);
  else
    strlcpy(buf, "(timestamp out of range)", sizeof(buf));

  return buf;
}

void dt2time(Timestamp jd, int *hour, int *min, int *sec, fsec_t *fsec) {
  TimeOffset time;

  time = jd;

  *hour = time / USECS_PER_HOUR;
  time -= (*hour) * USECS_PER_HOUR;
  *min = time / USECS_PER_MINUTE;
  time -= (*min) * USECS_PER_MINUTE;
  *sec = time / USECS_PER_SEC;
  *fsec = time - (*sec * USECS_PER_SEC);
} /* dt2time() */

/*
 * timestamp2tm() - Convert timestamp data type to POSIX time structure.
 *
 * Note that year is _not_ 1900-based, but is an explicit full value.
 * Also, month is one-based, _not_ zero-based.
 * Returns:
 *	 0 on success
 *	-1 on out of range
 *
 * If attimezone is NULL, the global timezone setting will be used.
 */
int timestamp2tm(Timestamp dt, int *tzp, struct pg_tm *tm, fsec_t *fsec,
                 const char **tzn, pg_tz *attimezone) {
  ty_tm_t t;
  int32_t f;

  /* Done if no TZ conversion wanted */
  if (tzp == NULL) {
    if (ty_timestamp_to_tm(dt, &t, &f) != 0)
      return -1;
    tm->tm_year = t.year;
    tm->tm_mon = t.mon;
    tm->tm_mday = t.mday;
    tm->tm_hour = t.hour;
    tm->tm_min = t.min;
    tm->tm_sec = t.sec;
    *fsec = f;
    tm->tm_isdst = -1;
    tm->tm_gmtoff = 0;
    tm->tm_zone = NULL;
    if (tzn != NULL)
      *tzn = NULL;
    return 0;
  }

  /* Use session timezone if caller asks for default */
  if (attimezone == NULL)
    attimezone = session_timezone;

  /* ty rotates the instant to the zone: the fields, the offset, the name */
  {
    int west, isdst;
    const char *abbrev;

    if (ty_timestamptz_to_local(attimezone, dt, &t, &f, &west, &isdst,
                                &abbrev) != 0)
      return -1;
    tm->tm_year = t.year;
    tm->tm_mon = t.mon;
    tm->tm_mday = t.mday;
    tm->tm_hour = t.hour;
    tm->tm_min = t.min;
    tm->tm_sec = t.sec;
    *fsec = f;
    tm->tm_isdst = isdst;
    tm->tm_gmtoff = -west;
    tm->tm_zone = abbrev;
    *tzp = west;
    if (tzn != NULL)
      *tzn = abbrev;
  }
  return 0;
}

/* tm2timestamp()
 * Convert a tm structure to a timestamp data type.
 * Note that year is _not_ 1900-based, but is an explicit full value.
 * Also, month is one-based, _not_ zero-based.
 *
 * Returns -1 on failure (value out of range).
 */
int tm2timestamp(struct pg_tm *tm, fsec_t fsec, int *tzp, Timestamp *result) {
  TimeOffset date;
  TimeOffset time;

  /* Prevent overflow in Julian-day routines */
  if (!IS_VALID_JULIAN(tm->tm_year, tm->tm_mon, tm->tm_mday)) {
    *result = 0; /* keep compiler quiet */
    return -1;
  }

  date = date2j(tm->tm_year, tm->tm_mon, tm->tm_mday) - POSTGRES_EPOCH_JDATE;
  time = time2t(tm->tm_hour, tm->tm_min, tm->tm_sec, fsec);

  if (unlikely(pg_mul_s64_overflow(date, USECS_PER_DAY, result) ||
               pg_add_s64_overflow(*result, time, result))) {
    *result = 0; /* keep compiler quiet */
    return -1;
  }
  if (tzp != NULL)
    *result = dt2local(*result, -(*tzp));

  /* final range check catches just-out-of-range timestamps */
  if (!IS_VALID_TIMESTAMP(*result)) {
    *result = 0; /* keep compiler quiet */
    return -1;
  }

  return 0;
}

/* interval2itm()
 * Convert an Interval to a pg_itm structure.
 * Note: overflow is not possible, because the pg_itm fields are
 * wide enough for all possible conversion results.
 */
void interval2itm(Interval span, struct pg_itm *itm) {
  TimeOffset time;
  TimeOffset tfrac;

  itm->tm_year = span.month / MONTHS_PER_YEAR;
  itm->tm_mon = span.month % MONTHS_PER_YEAR;
  itm->tm_mday = span.day;
  time = span.time;

  tfrac = time / USECS_PER_HOUR;
  time -= tfrac * USECS_PER_HOUR;
  itm->tm_hour = tfrac;
  tfrac = time / USECS_PER_MINUTE;
  time -= tfrac * USECS_PER_MINUTE;
  itm->tm_min = (int)tfrac;
  tfrac = time / USECS_PER_SEC;
  time -= tfrac * USECS_PER_SEC;
  itm->tm_sec = (int)tfrac;
  itm->tm_usec = (int)time;
}

/* itm2interval()
 * Convert a pg_itm structure to an Interval.
 * Returns 0 if OK, -1 on overflow.
 *
 * This is for use in computations expected to produce finite results.  Any
 * inputs that lead to infinite results are treated as overflows.
 */
int itm2interval(struct pg_itm *itm, Interval *span) {
  int64 total_months = (int64)itm->tm_year * MONTHS_PER_YEAR + itm->tm_mon;

  if (total_months > INT_MAX || total_months < INT_MIN)
    return -1;
  span->month = (int32)total_months;
  span->day = itm->tm_mday;
  if (pg_mul_s64_overflow(itm->tm_hour, USECS_PER_HOUR, &span->time))
    return -1;
  /* tm_min, tm_sec are 32 bits, so intermediate products can't overflow */
  if (pg_add_s64_overflow(span->time, itm->tm_min * USECS_PER_MINUTE,
                          &span->time))
    return -1;
  if (pg_add_s64_overflow(span->time, itm->tm_sec * USECS_PER_SEC, &span->time))
    return -1;
  if (pg_add_s64_overflow(span->time, itm->tm_usec, &span->time))
    return -1;
  if (INTERVAL_NOT_FINITE(span))
    return -1;
  return 0;
}

/* itmin2interval()
 * Convert a pg_itm_in structure to an Interval.
 * Returns 0 if OK, -1 on overflow.
 *
 * Note: if the result is infinite, it is not treated as an overflow.  This
 * avoids any dump/reload hazards from pre-17 databases that do not support
 * infinite intervals, but do allow finite intervals with all fields set to
 * INT_MIN/INT_MAX (outside the documented range).  Such intervals will be
 * silently converted to +/-infinity.  This may not be ideal, but seems
 * preferable to failure, and ought to be pretty unlikely in practice.
 */
int itmin2interval(struct pg_itm_in *itm_in, Interval *span) {
  int64 total_months =
      (int64)itm_in->tm_year * MONTHS_PER_YEAR + itm_in->tm_mon;

  if (total_months > INT_MAX || total_months < INT_MIN)
    return -1;
  span->month = (int32)total_months;
  span->day = itm_in->tm_mday;
  span->time = itm_in->tm_usec;
  return 0;
}

static TimeOffset time2t(const int hour, const int min, const int sec,
                         const fsec_t fsec) {
  return (((((hour * MINS_PER_HOUR) + min) * SECS_PER_MINUTE) + sec) *
          USECS_PER_SEC) +
         fsec;
}

static Timestamp dt2local(Timestamp dt, int timezone) {
  dt -= (timezone * USECS_PER_SEC);
  return dt;
}

/*****************************************************************************
 *	 PUBLIC ROUTINES
 **
 *****************************************************************************/

Datum timestamp_finite(PG_FUNCTION_ARGS) {
  Timestamp timestamp = PG_GETARG_TIMESTAMP(0);

  PG_RETURN_BOOL(!TIMESTAMP_NOT_FINITE(timestamp));
}

Datum interval_finite(PG_FUNCTION_ARGS) {
  Interval *interval = PG_GETARG_INTERVAL_P(0);

  PG_RETURN_BOOL(!INTERVAL_NOT_FINITE(interval));
}

/*----------------------------------------------------------
 *	Relational operators for timestamp.
 *---------------------------------------------------------*/

void GetEpochTime(struct pg_tm *tm) {
  /* The Unix epoch, 1970-01-01 00:00:00 UTC, with PostgreSQL's years. */
  tm->tm_year = 1970;
  tm->tm_mon = 1;
  tm->tm_mday = 1;
  tm->tm_hour = 0;
  tm->tm_min = 0;
  tm->tm_sec = 0;
}

Timestamp SetEpochTimestamp(void) {
  Timestamp dt;
  struct pg_tm tt, *tm = &tt;

  GetEpochTime(tm);
  /* we don't bother to test for failure ... */
  tm2timestamp(tm, 0, NULL, &dt);

  return dt;
} /* SetEpochTimestamp() */

/*
 * We are currently sharing some code between timestamp and timestamptz.
 * The comparison functions are among them. - thomas 2001-09-25
 *
 *		timestamp_relop - is timestamp1 relop timestamp2
 */
int timestamp_cmp_internal(Timestamp dt1, Timestamp dt2) {
  return ty_timestamp_cmp(dt1, dt2);
}

Datum timestamp_eq(PG_FUNCTION_ARGS) {
  Timestamp dt1 = PG_GETARG_TIMESTAMP(0);
  Timestamp dt2 = PG_GETARG_TIMESTAMP(1);

  PG_RETURN_BOOL(timestamp_cmp_internal(dt1, dt2) == 0);
}

Datum timestamp_ne(PG_FUNCTION_ARGS) {
  Timestamp dt1 = PG_GETARG_TIMESTAMP(0);
  Timestamp dt2 = PG_GETARG_TIMESTAMP(1);

  PG_RETURN_BOOL(timestamp_cmp_internal(dt1, dt2) != 0);
}

Datum timestamp_lt(PG_FUNCTION_ARGS) {
  Timestamp dt1 = PG_GETARG_TIMESTAMP(0);
  Timestamp dt2 = PG_GETARG_TIMESTAMP(1);

  PG_RETURN_BOOL(timestamp_cmp_internal(dt1, dt2) < 0);
}

Datum timestamp_gt(PG_FUNCTION_ARGS) {
  Timestamp dt1 = PG_GETARG_TIMESTAMP(0);
  Timestamp dt2 = PG_GETARG_TIMESTAMP(1);

  PG_RETURN_BOOL(timestamp_cmp_internal(dt1, dt2) > 0);
}

Datum timestamp_le(PG_FUNCTION_ARGS) {
  Timestamp dt1 = PG_GETARG_TIMESTAMP(0);
  Timestamp dt2 = PG_GETARG_TIMESTAMP(1);

  PG_RETURN_BOOL(timestamp_cmp_internal(dt1, dt2) <= 0);
}

Datum timestamp_ge(PG_FUNCTION_ARGS) {
  Timestamp dt1 = PG_GETARG_TIMESTAMP(0);
  Timestamp dt2 = PG_GETARG_TIMESTAMP(1);

  PG_RETURN_BOOL(timestamp_cmp_internal(dt1, dt2) >= 0);
}

Datum timestamp_cmp(PG_FUNCTION_ARGS) {
  Timestamp dt1 = PG_GETARG_TIMESTAMP(0);
  Timestamp dt2 = PG_GETARG_TIMESTAMP(1);

  PG_RETURN_INT32(timestamp_cmp_internal(dt1, dt2));
}

#if SIZEOF_DATUM < 8
/* note: this is used for timestamptz also */
static int timestamp_fastcmp(Datum x, Datum y, SortSupport ssup) {
  Timestamp a = DatumGetTimestamp(x);
  Timestamp b = DatumGetTimestamp(y);

  return timestamp_cmp_internal(a, b);
}
#endif

Datum timestamp_sortsupport(PG_FUNCTION_ARGS) {
  SortSupport ssup = (SortSupport)PG_GETARG_POINTER(0);

#if SIZEOF_DATUM >= 8

  /*
   * If this build has pass-by-value timestamps, then we can use a standard
   * comparator function.
   */
  ssup->comparator = ssup_datum_signed_cmp;
#else
  ssup->comparator = timestamp_fastcmp;
#endif
  PG_RETURN_VOID();
}

/* note: this is used for timestamptz also */
static Datum timestamp_decrement(Relation rel, Datum existing,
                                 bool *underflow) {
  Timestamp texisting = DatumGetTimestamp(existing);

  if (texisting == PG_INT64_MIN) {
    /* return value is undefined */
    *underflow = true;
    return (Datum)0;
  }

  *underflow = false;
  return TimestampGetDatum(texisting - 1);
}

/* note: this is used for timestamptz also */
static Datum timestamp_increment(Relation rel, Datum existing, bool *overflow) {
  Timestamp texisting = DatumGetTimestamp(existing);

  if (texisting == PG_INT64_MAX) {
    /* return value is undefined */
    *overflow = true;
    return (Datum)0;
  }

  *overflow = false;
  return TimestampGetDatum(texisting + 1);
}

Datum timestamp_skipsupport(PG_FUNCTION_ARGS) {
  SkipSupport sksup = (SkipSupport)PG_GETARG_POINTER(0);

  sksup->decrement = timestamp_decrement;
  sksup->increment = timestamp_increment;
  sksup->low_elem = TimestampGetDatum(PG_INT64_MIN);
  sksup->high_elem = TimestampGetDatum(PG_INT64_MAX);

  PG_RETURN_VOID();
}

Datum timestamp_hash(PG_FUNCTION_ARGS) { return hashint8(fcinfo); }

Datum timestamp_hash_extended(PG_FUNCTION_ARGS) {
  return hashint8extended(fcinfo);
}

Datum timestamptz_hash(PG_FUNCTION_ARGS) { return hashint8(fcinfo); }

Datum timestamptz_hash_extended(PG_FUNCTION_ARGS) {
  return hashint8extended(fcinfo);
}

/*
 * Cross-type comparison functions for timestamp vs timestamptz
 */

int32 timestamp_cmp_timestamptz_internal(Timestamp timestampVal,
                                         TimestampTz dt2) {
  TimestampTz dt1;
  int overflow;

  dt1 = timestamp2timestamptz_opt_overflow(timestampVal, &overflow);
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

Datum timestamp_eq_timestamptz(PG_FUNCTION_ARGS) {
  Timestamp timestampVal = PG_GETARG_TIMESTAMP(0);
  TimestampTz dt2 = PG_GETARG_TIMESTAMPTZ(1);

  PG_RETURN_BOOL(timestamp_cmp_timestamptz_internal(timestampVal, dt2) == 0);
}

Datum timestamp_ne_timestamptz(PG_FUNCTION_ARGS) {
  Timestamp timestampVal = PG_GETARG_TIMESTAMP(0);
  TimestampTz dt2 = PG_GETARG_TIMESTAMPTZ(1);

  PG_RETURN_BOOL(timestamp_cmp_timestamptz_internal(timestampVal, dt2) != 0);
}

Datum timestamp_lt_timestamptz(PG_FUNCTION_ARGS) {
  Timestamp timestampVal = PG_GETARG_TIMESTAMP(0);
  TimestampTz dt2 = PG_GETARG_TIMESTAMPTZ(1);

  PG_RETURN_BOOL(timestamp_cmp_timestamptz_internal(timestampVal, dt2) < 0);
}

Datum timestamp_gt_timestamptz(PG_FUNCTION_ARGS) {
  Timestamp timestampVal = PG_GETARG_TIMESTAMP(0);
  TimestampTz dt2 = PG_GETARG_TIMESTAMPTZ(1);

  PG_RETURN_BOOL(timestamp_cmp_timestamptz_internal(timestampVal, dt2) > 0);
}

Datum timestamp_le_timestamptz(PG_FUNCTION_ARGS) {
  Timestamp timestampVal = PG_GETARG_TIMESTAMP(0);
  TimestampTz dt2 = PG_GETARG_TIMESTAMPTZ(1);

  PG_RETURN_BOOL(timestamp_cmp_timestamptz_internal(timestampVal, dt2) <= 0);
}

Datum timestamp_ge_timestamptz(PG_FUNCTION_ARGS) {
  Timestamp timestampVal = PG_GETARG_TIMESTAMP(0);
  TimestampTz dt2 = PG_GETARG_TIMESTAMPTZ(1);

  PG_RETURN_BOOL(timestamp_cmp_timestamptz_internal(timestampVal, dt2) >= 0);
}

Datum timestamp_cmp_timestamptz(PG_FUNCTION_ARGS) {
  Timestamp timestampVal = PG_GETARG_TIMESTAMP(0);
  TimestampTz dt2 = PG_GETARG_TIMESTAMPTZ(1);

  PG_RETURN_INT32(timestamp_cmp_timestamptz_internal(timestampVal, dt2));
}

Datum timestamptz_eq_timestamp(PG_FUNCTION_ARGS) {
  TimestampTz dt1 = PG_GETARG_TIMESTAMPTZ(0);
  Timestamp timestampVal = PG_GETARG_TIMESTAMP(1);

  PG_RETURN_BOOL(timestamp_cmp_timestamptz_internal(timestampVal, dt1) == 0);
}

Datum timestamptz_ne_timestamp(PG_FUNCTION_ARGS) {
  TimestampTz dt1 = PG_GETARG_TIMESTAMPTZ(0);
  Timestamp timestampVal = PG_GETARG_TIMESTAMP(1);

  PG_RETURN_BOOL(timestamp_cmp_timestamptz_internal(timestampVal, dt1) != 0);
}

Datum timestamptz_lt_timestamp(PG_FUNCTION_ARGS) {
  TimestampTz dt1 = PG_GETARG_TIMESTAMPTZ(0);
  Timestamp timestampVal = PG_GETARG_TIMESTAMP(1);

  PG_RETURN_BOOL(timestamp_cmp_timestamptz_internal(timestampVal, dt1) > 0);
}

Datum timestamptz_gt_timestamp(PG_FUNCTION_ARGS) {
  TimestampTz dt1 = PG_GETARG_TIMESTAMPTZ(0);
  Timestamp timestampVal = PG_GETARG_TIMESTAMP(1);

  PG_RETURN_BOOL(timestamp_cmp_timestamptz_internal(timestampVal, dt1) < 0);
}

Datum timestamptz_le_timestamp(PG_FUNCTION_ARGS) {
  TimestampTz dt1 = PG_GETARG_TIMESTAMPTZ(0);
  Timestamp timestampVal = PG_GETARG_TIMESTAMP(1);

  PG_RETURN_BOOL(timestamp_cmp_timestamptz_internal(timestampVal, dt1) >= 0);
}

Datum timestamptz_ge_timestamp(PG_FUNCTION_ARGS) {
  TimestampTz dt1 = PG_GETARG_TIMESTAMPTZ(0);
  Timestamp timestampVal = PG_GETARG_TIMESTAMP(1);

  PG_RETURN_BOOL(timestamp_cmp_timestamptz_internal(timestampVal, dt1) <= 0);
}

Datum timestamptz_cmp_timestamp(PG_FUNCTION_ARGS) {
  TimestampTz dt1 = PG_GETARG_TIMESTAMPTZ(0);
  Timestamp timestampVal = PG_GETARG_TIMESTAMP(1);

  PG_RETURN_INT32(-timestamp_cmp_timestamptz_internal(timestampVal, dt1));
}

/*
 *		interval_relop	- is interval1 relop interval2
 *
 * Interval comparison is based on converting interval values to a linear
 * representation expressed in the units of the time field (microseconds,
 * in the case of integer timestamps) with days assumed to be always 24 hours
 * and months assumed to be always 30 days.  To avoid overflow, we need a
 * wider-than-int64 datatype for the linear representation, so use INT128.
 */

/* The ty value of an Interval: the layouts are the same. */
#define TY_IV(p) ((const ty_interval_t *)(p))

/* Raise the error of a failed ty interval call. */
static pg_noreturn void interval_raise(void) {
  if (ty_errnum() == TY_ERR_DIVZERO)
    ereport(ERROR,
            (errcode(ERRCODE_DIVISION_BY_ZERO), errmsg("division by zero")));
  ereport(ERROR, (errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
                  errmsg("interval out of range")));
}

/* The sum and the difference of two finite intervals, for the aggregates. */
static void finite_interval_pl(const Interval *span1, const Interval *span2,
                               Interval *result) {
  if (ty_interval_add(TY_IV(span1), TY_IV(span2), (ty_interval_t *)result))
    interval_raise();
}

static void finite_interval_mi(const Interval *span1, const Interval *span2,
                               Interval *result) {
  if (ty_interval_sub(TY_IV(span1), TY_IV(span2), (ty_interval_t *)result))
    interval_raise();
}


static int interval_cmp_internal(const Interval *interval1,
                                 const Interval *interval2) {
  return ty_interval_cmp(TY_IV(interval1), TY_IV(interval2));
}

static int interval_sign(const Interval *interval) {
  return ty_interval_sign(TY_IV(interval));
}

Datum interval_eq(PG_FUNCTION_ARGS) {
  Interval *interval1 = PG_GETARG_INTERVAL_P(0);
  Interval *interval2 = PG_GETARG_INTERVAL_P(1);

  PG_RETURN_BOOL(interval_cmp_internal(interval1, interval2) == 0);
}

Datum interval_ne(PG_FUNCTION_ARGS) {
  Interval *interval1 = PG_GETARG_INTERVAL_P(0);
  Interval *interval2 = PG_GETARG_INTERVAL_P(1);

  PG_RETURN_BOOL(interval_cmp_internal(interval1, interval2) != 0);
}

Datum interval_lt(PG_FUNCTION_ARGS) {
  Interval *interval1 = PG_GETARG_INTERVAL_P(0);
  Interval *interval2 = PG_GETARG_INTERVAL_P(1);

  PG_RETURN_BOOL(interval_cmp_internal(interval1, interval2) < 0);
}

Datum interval_gt(PG_FUNCTION_ARGS) {
  Interval *interval1 = PG_GETARG_INTERVAL_P(0);
  Interval *interval2 = PG_GETARG_INTERVAL_P(1);

  PG_RETURN_BOOL(interval_cmp_internal(interval1, interval2) > 0);
}

Datum interval_le(PG_FUNCTION_ARGS) {
  Interval *interval1 = PG_GETARG_INTERVAL_P(0);
  Interval *interval2 = PG_GETARG_INTERVAL_P(1);

  PG_RETURN_BOOL(interval_cmp_internal(interval1, interval2) <= 0);
}

Datum interval_ge(PG_FUNCTION_ARGS) {
  Interval *interval1 = PG_GETARG_INTERVAL_P(0);
  Interval *interval2 = PG_GETARG_INTERVAL_P(1);

  PG_RETURN_BOOL(interval_cmp_internal(interval1, interval2) >= 0);
}

Datum interval_cmp(PG_FUNCTION_ARGS) {
  Interval *interval1 = PG_GETARG_INTERVAL_P(0);
  Interval *interval2 = PG_GETARG_INTERVAL_P(1);

  PG_RETURN_INT32(interval_cmp_internal(interval1, interval2));
}

/*
 * Hashing for intervals
 *
 * We must produce equal hashvals for values that interval_cmp_internal()
 * considers equal.  So, compute the net span the same way it does,
 * and then hash that.
 */
Datum interval_hash(PG_FUNCTION_ARGS) {
  Interval *interval = PG_GETARG_INTERVAL_P(0);

  return UInt32GetDatum(ty_interval_hash(TY_IV(interval)));
}

Datum interval_hash_extended(PG_FUNCTION_ARGS) {
  Interval *interval = PG_GETARG_INTERVAL_P(0);

  return Int64GetDatum(
      ty_interval_hash_extended(TY_IV(interval), PG_GETARG_INT64(1)));
}

/* overlaps_timestamp() --- implements the SQL OVERLAPS operator.
 *
 * Algorithm is per SQL spec.  This is much harder than you'd think
 * because the spec requires us to deliver a non-null answer in some cases
 * where some of the inputs are null.
 */
Datum overlaps_timestamp(PG_FUNCTION_ARGS) {
  int64 v[4];
  const int64 *p[4];
  int r;

  /* a null end is a null pointer to ty, which applies the SQL standard */
  for (int i = 0; i < 4; i++) {
    v[i] = PG_ARGISNULL(i) ? 0 : DatumGetTimestamp(PG_GETARG_DATUM(i));
    p[i] = PG_ARGISNULL(i) ? NULL : &v[i];
  }
  r = ty_overlaps_i64(p[0], p[1], p[2], p[3]);
  if (r == 2)
    PG_RETURN_NULL();
  PG_RETURN_BOOL(r == 1);
}

/*----------------------------------------------------------
 *	"Arithmetic" operators on date/times.
 *---------------------------------------------------------*/

Datum timestamp_smaller(PG_FUNCTION_ARGS) {
  Timestamp dt1 = PG_GETARG_TIMESTAMP(0);
  Timestamp dt2 = PG_GETARG_TIMESTAMP(1);
  Timestamp result;

  /* use timestamp_cmp_internal to be sure this agrees with comparisons */
  if (timestamp_cmp_internal(dt1, dt2) < 0)
    result = dt1;
  else
    result = dt2;
  PG_RETURN_TIMESTAMP(result);
}

Datum timestamp_larger(PG_FUNCTION_ARGS) {
  Timestamp dt1 = PG_GETARG_TIMESTAMP(0);
  Timestamp dt2 = PG_GETARG_TIMESTAMP(1);
  Timestamp result;

  if (timestamp_cmp_internal(dt1, dt2) > 0)
    result = dt1;
  else
    result = dt2;
  PG_RETURN_TIMESTAMP(result);
}

Datum timestamp_mi(PG_FUNCTION_ARGS) {
  Timestamp dt1 = PG_GETARG_TIMESTAMP(0);
  Timestamp dt2 = PG_GETARG_TIMESTAMP(1);
  Interval *result = (Interval *)palloc(sizeof(Interval));

  /* ty handles the infinities, and justifies the hours */
  if (ty_timestamp_diff(dt1, dt2, (ty_interval_t *)result))
    interval_raise();

  PG_RETURN_INTERVAL_P(result);
}

/*
 *	interval_justify_interval()
 *
 *	Adjust interval so 'month', 'day', and 'time' portions are within
 *	customary bounds.  Specifically:
 *
 *		0 <= abs(time) < 24 hours
 *		0 <= abs(day)  < 30 days
 *
 *	Also, the sign bit on all three fields is made equal, so either
 *	all three fields are negative or all are positive.
 */
Datum interval_justify_interval(PG_FUNCTION_ARGS) {
  Interval *span = PG_GETARG_INTERVAL_P(0);
  Interval *result = (Interval *)palloc(sizeof(Interval));

  if (ty_interval_justify(TY_IV(span), (ty_interval_t *)result))
    interval_raise();

  PG_RETURN_INTERVAL_P(result);
}

/*
 *	interval_justify_hours()
 *
 *	Adjust interval so 'time' contains less than a whole day, adding
 *	the excess to 'day'.  This is useful for
 *	situations (such as non-TZ) where '1 day' = '24 hours' is valid,
 *	e.g. interval subtraction and division.
 */
Datum interval_justify_hours(PG_FUNCTION_ARGS) {
  Interval *span = PG_GETARG_INTERVAL_P(0);
  Interval *result = (Interval *)palloc(sizeof(Interval));

  if (ty_interval_justify_hours(TY_IV(span), (ty_interval_t *)result))
    interval_raise();

  PG_RETURN_INTERVAL_P(result);
}

/*
 *	interval_justify_days()
 *
 *	Adjust interval so 'day' contains less than 30 days, adding
 *	the excess to 'month'.
 */
Datum interval_justify_days(PG_FUNCTION_ARGS) {
  Interval *span = PG_GETARG_INTERVAL_P(0);
  Interval *result = (Interval *)palloc(sizeof(Interval));

  if (ty_interval_justify_days(TY_IV(span), (ty_interval_t *)result))
    interval_raise();

  PG_RETURN_INTERVAL_P(result);
}

/* timestamp_pl_interval()
 * Add an interval to a timestamp data type.
 * Note that interval has provisions for qualitative year/month and day
 *	units, so try to do the right thing with them.
 * To add a month, increment the month, and use the same day of month.
 * Then, if the next month has fewer days, set the day of month
 *	to the last day of month.
 * To add a day, increment the mday, and use the same time of day.
 * Lastly, add in the "quantitative time".
 */
Datum timestamp_pl_interval(PG_FUNCTION_ARGS) {
  Timestamp timestamp = PG_GETARG_TIMESTAMP(0);
  Interval *span = PG_GETARG_INTERVAL_P(1);
  Timestamp result;

  if (ty_timestamp_add_interval(timestamp, TY_IV(span), &result))
    ereport(ERROR, (errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
                    errmsg("%s", ty_errmsg())));

  PG_RETURN_TIMESTAMP(result);
}

Datum timestamp_mi_interval(PG_FUNCTION_ARGS) {
  Timestamp timestamp = PG_GETARG_TIMESTAMP(0);
  Interval *span = PG_GETARG_INTERVAL_P(1);
  Timestamp result;

  if (ty_timestamp_sub_interval(timestamp, TY_IV(span), &result))
    ereport(ERROR, (errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
                    errmsg("%s", ty_errmsg())));

  PG_RETURN_TIMESTAMP(result);
}

/* timestamptz_pl_interval_internal()
 * Add an interval to a timestamptz, in the given (or session) timezone.
 *
 * Note that interval has provisions for qualitative year/month and day
 *	units, so try to do the right thing with them.
 * To add a month, increment the month, and use the same day of month.
 * Then, if the next month has fewer days, set the day of month
 *	to the last day of month.
 * To add a day, increment the mday, and use the same time of day.
 * Lastly, add in the "quantitative time".
 */
static TimestampTz timestamptz_pl_interval_internal(TimestampTz timestamp,
                                                    Interval *span,
                                                    pg_tz *attimezone) {
  TimestampTz result;

  /* Use session timezone if caller asks for default */
  if (attimezone == NULL)
    attimezone = session_timezone;

  /* ty adds the months, the days and the microseconds, in the zone */
  if (ty_timestamptz_add_interval(attimezone, timestamp, (ty_interval_t *)span,
                                  &result))
    ereport(ERROR, (errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
                    errmsg("%s", ty_errmsg())));
  return result;
}

/* timestamptz_mi_interval_internal()
 * As above, but subtract the interval.
 */
static TimestampTz timestamptz_mi_interval_internal(TimestampTz timestamp,
                                                    Interval *span,
                                                    pg_tz *attimezone) {
  Interval tspan;

  interval_um_internal(span, &tspan);

  return timestamptz_pl_interval_internal(timestamp, &tspan, attimezone);
}

/* timestamptz_pl_interval()
 * Add an interval to a timestamptz, in the session timezone.
 */
Datum timestamptz_pl_interval(PG_FUNCTION_ARGS) {
  TimestampTz timestamp = PG_GETARG_TIMESTAMPTZ(0);
  Interval *span = PG_GETARG_INTERVAL_P(1);

  PG_RETURN_TIMESTAMP(timestamptz_pl_interval_internal(timestamp, span, NULL));
}

Datum timestamptz_mi_interval(PG_FUNCTION_ARGS) {
  TimestampTz timestamp = PG_GETARG_TIMESTAMPTZ(0);
  Interval *span = PG_GETARG_INTERVAL_P(1);

  PG_RETURN_TIMESTAMP(timestamptz_mi_interval_internal(timestamp, span, NULL));
}

/* timestamptz_pl_interval_at_zone()
 * Add an interval to a timestamptz, in the specified timezone.
 */
Datum timestamptz_pl_interval_at_zone(PG_FUNCTION_ARGS) {
  TimestampTz timestamp = PG_GETARG_TIMESTAMPTZ(0);
  Interval *span = PG_GETARG_INTERVAL_P(1);
  text *zone = PG_GETARG_TEXT_PP(2);
  pg_tz *attimezone = lookup_timezone(zone);

  PG_RETURN_TIMESTAMP(
      timestamptz_pl_interval_internal(timestamp, span, attimezone));
}

Datum timestamptz_mi_interval_at_zone(PG_FUNCTION_ARGS) {
  TimestampTz timestamp = PG_GETARG_TIMESTAMPTZ(0);
  Interval *span = PG_GETARG_INTERVAL_P(1);
  text *zone = PG_GETARG_TEXT_PP(2);
  pg_tz *attimezone = lookup_timezone(zone);

  PG_RETURN_TIMESTAMP(
      timestamptz_mi_interval_internal(timestamp, span, attimezone));
}

/* interval_um_internal()
 * Negate an interval.
 */
static void interval_um_internal(const Interval *interval, Interval *result) {
  if (ty_interval_neg(TY_IV(interval), (ty_interval_t *)result))
    interval_raise();
}

Datum interval_um(PG_FUNCTION_ARGS) {
  Interval *interval = PG_GETARG_INTERVAL_P(0);
  Interval *result;

  result = (Interval *)palloc(sizeof(Interval));
  interval_um_internal(interval, result);

  PG_RETURN_INTERVAL_P(result);
}

Datum interval_smaller(PG_FUNCTION_ARGS) {
  Interval *interval1 = PG_GETARG_INTERVAL_P(0);
  Interval *interval2 = PG_GETARG_INTERVAL_P(1);
  Interval *result;

  /* use interval_cmp_internal to be sure this agrees with comparisons */
  if (interval_cmp_internal(interval1, interval2) < 0)
    result = interval1;
  else
    result = interval2;
  PG_RETURN_INTERVAL_P(result);
}

Datum interval_larger(PG_FUNCTION_ARGS) {
  Interval *interval1 = PG_GETARG_INTERVAL_P(0);
  Interval *interval2 = PG_GETARG_INTERVAL_P(1);
  Interval *result;

  if (interval_cmp_internal(interval1, interval2) > 0)
    result = interval1;
  else
    result = interval2;
  PG_RETURN_INTERVAL_P(result);
}


Datum interval_pl(PG_FUNCTION_ARGS) {
  Interval *span1 = PG_GETARG_INTERVAL_P(0);
  Interval *span2 = PG_GETARG_INTERVAL_P(1);
  Interval *result = (Interval *)palloc(sizeof(Interval));

  if (ty_interval_add(TY_IV(span1), TY_IV(span2), (ty_interval_t *)result))
    interval_raise();

  PG_RETURN_INTERVAL_P(result);
}


Datum interval_mi(PG_FUNCTION_ARGS) {
  Interval *span1 = PG_GETARG_INTERVAL_P(0);
  Interval *span2 = PG_GETARG_INTERVAL_P(1);
  Interval *result = (Interval *)palloc(sizeof(Interval));

  if (ty_interval_sub(TY_IV(span1), TY_IV(span2), (ty_interval_t *)result))
    interval_raise();

  PG_RETURN_INTERVAL_P(result);
}

/*
 *	There is no interval_abs():  it is unclear what value to return:
 *	  http://archives.postgresql.org/pgsql-general/2009-10/msg01031.php
 *	  http://archives.postgresql.org/pgsql-general/2009-11/msg00041.php
 */

Datum interval_mul(PG_FUNCTION_ARGS) {
  Interval *span = PG_GETARG_INTERVAL_P(0);
  float8 factor = PG_GETARG_FLOAT8(1);
  Interval *result = (Interval *)palloc(sizeof(Interval));

  if (ty_interval_mul(TY_IV(span), factor, (ty_interval_t *)result))
    interval_raise();

  PG_RETURN_INTERVAL_P(result);
}

Datum mul_d_interval(PG_FUNCTION_ARGS) {
  /* Args are float8 and Interval *, but leave them as generic Datum */
  Datum factor = PG_GETARG_DATUM(0);
  Datum span = PG_GETARG_DATUM(1);

  return DirectFunctionCall2(interval_mul, span, factor);
}

Datum interval_div(PG_FUNCTION_ARGS) {
  Interval *span = PG_GETARG_INTERVAL_P(0);
  float8 factor = PG_GETARG_FLOAT8(1);
  Interval *result = (Interval *)palloc(sizeof(Interval));

  if (ty_interval_div(TY_IV(span), factor, (ty_interval_t *)result))
    interval_raise();

  PG_RETURN_INTERVAL_P(result);
}

/*
 * in_range support functions for timestamps and intervals.
 *
 * Per SQL spec, we support these with interval as the offset type.
 * The spec's restriction that the offset not be negative is a bit hard to
 * decipher for intervals, but we choose to interpret it the same as our
 * interval comparison operators would.
 */

Datum in_range_timestamptz_interval(PG_FUNCTION_ARGS) {
  TimestampTz val = PG_GETARG_TIMESTAMPTZ(0);
  TimestampTz base = PG_GETARG_TIMESTAMPTZ(1);
  Interval *offset = PG_GETARG_INTERVAL_P(2);
  bool sub = PG_GETARG_BOOL(3);
  bool less = PG_GETARG_BOOL(4);
  TimestampTz sum;

  if (interval_sign(offset) < 0)
    ereport(ERROR,
            (errcode(ERRCODE_INVALID_PRECEDING_OR_FOLLOWING_SIZE),
             errmsg("invalid preceding or following size in window function")));

  /*
   * Deal with cases where both base and offset are infinite, and computing
   * base +/- offset would cause an error.  As for float and numeric types,
   * we assume that all values infinitely precede +infinity and infinitely
   * follow -infinity.  See in_range_float8_float8() for reasoning.
   */
  if (INTERVAL_IS_NOEND(offset) &&
      (sub ? TIMESTAMP_IS_NOEND(base) : TIMESTAMP_IS_NOBEGIN(base)))
    PG_RETURN_BOOL(true);

  /* We don't currently bother to avoid overflow hazards here */
  if (sub)
    sum = timestamptz_mi_interval_internal(base, offset, NULL);
  else
    sum = timestamptz_pl_interval_internal(base, offset, NULL);

  if (less)
    PG_RETURN_BOOL(val <= sum);
  else
    PG_RETURN_BOOL(val >= sum);
}

Datum in_range_timestamp_interval(PG_FUNCTION_ARGS) {
  Timestamp val = PG_GETARG_TIMESTAMP(0);
  Timestamp base = PG_GETARG_TIMESTAMP(1);
  Interval *offset = PG_GETARG_INTERVAL_P(2);
  bool sub = PG_GETARG_BOOL(3);
  bool less = PG_GETARG_BOOL(4);
  Timestamp sum;

  if (interval_sign(offset) < 0)
    ereport(ERROR,
            (errcode(ERRCODE_INVALID_PRECEDING_OR_FOLLOWING_SIZE),
             errmsg("invalid preceding or following size in window function")));

  /*
   * Deal with cases where both base and offset are infinite, and computing
   * base +/- offset would cause an error.  As for float and numeric types,
   * we assume that all values infinitely precede +infinity and infinitely
   * follow -infinity.  See in_range_float8_float8() for reasoning.
   */
  if (INTERVAL_IS_NOEND(offset) &&
      (sub ? TIMESTAMP_IS_NOEND(base) : TIMESTAMP_IS_NOBEGIN(base)))
    PG_RETURN_BOOL(true);

  /* We don't currently bother to avoid overflow hazards here */
  if (sub)
    sum = DatumGetTimestamp(DirectFunctionCall2(timestamp_mi_interval,
                                                TimestampGetDatum(base),
                                                IntervalPGetDatum(offset)));
  else
    sum = DatumGetTimestamp(DirectFunctionCall2(timestamp_pl_interval,
                                                TimestampGetDatum(base),
                                                IntervalPGetDatum(offset)));

  if (less)
    PG_RETURN_BOOL(val <= sum);
  else
    PG_RETURN_BOOL(val >= sum);
}

Datum in_range_interval_interval(PG_FUNCTION_ARGS) {
  Interval *val = PG_GETARG_INTERVAL_P(0);
  Interval *base = PG_GETARG_INTERVAL_P(1);
  Interval *offset = PG_GETARG_INTERVAL_P(2);
  bool sub = PG_GETARG_BOOL(3);
  bool less = PG_GETARG_BOOL(4);
  Interval *sum;

  if (interval_sign(offset) < 0)
    ereport(ERROR,
            (errcode(ERRCODE_INVALID_PRECEDING_OR_FOLLOWING_SIZE),
             errmsg("invalid preceding or following size in window function")));

  /*
   * Deal with cases where both base and offset are infinite, and computing
   * base +/- offset would cause an error.  As for float and numeric types,
   * we assume that all values infinitely precede +infinity and infinitely
   * follow -infinity.  See in_range_float8_float8() for reasoning.
   */
  if (INTERVAL_IS_NOEND(offset) &&
      (sub ? INTERVAL_IS_NOEND(base) : INTERVAL_IS_NOBEGIN(base)))
    PG_RETURN_BOOL(true);

  /* We don't currently bother to avoid overflow hazards here */
  if (sub)
    sum = DatumGetIntervalP(DirectFunctionCall2(
        interval_mi, IntervalPGetDatum(base), IntervalPGetDatum(offset)));
  else
    sum = DatumGetIntervalP(DirectFunctionCall2(
        interval_pl, IntervalPGetDatum(base), IntervalPGetDatum(offset)));

  if (less)
    PG_RETURN_BOOL(interval_cmp_internal(val, sum) <= 0);
  else
    PG_RETURN_BOOL(interval_cmp_internal(val, sum) >= 0);
}

/*
 * Prepare state data for an interval aggregate function, that needs to compute
 * sum and count, in the aggregate's memory context.
 *
 * The function is used when the state data needs to be allocated in aggregate's
 * context. When the state data needs to be allocated in the current memory
 * context, we use palloc0 directly e.g. interval_avg_deserialize().
 */
static IntervalAggState *makeIntervalAggState(FunctionCallInfo fcinfo) {
  IntervalAggState *state;
  MemoryContext agg_context;
  MemoryContext old_context;

  if (!AggCheckCallContext(fcinfo, &agg_context))
    elog(ERROR, "aggregate function called in non-aggregate context");

  old_context = MemoryContextSwitchTo(agg_context);

  state = (IntervalAggState *)palloc0(sizeof(IntervalAggState));

  MemoryContextSwitchTo(old_context);

  return state;
}

/*
 * Accumulate a new input value for interval aggregate functions.
 */
static void do_interval_accum(IntervalAggState *state, Interval *newval) {
  /* Infinite inputs are counted separately, and do not affect "N" */
  if (INTERVAL_IS_NOBEGIN(newval)) {
    state->nInfcount++;
    return;
  }

  if (INTERVAL_IS_NOEND(newval)) {
    state->pInfcount++;
    return;
  }

  finite_interval_pl(&state->sumX, newval, &state->sumX);
  state->N++;
}

/*
 * Remove the given interval value from the aggregated state.
 */
static void do_interval_discard(IntervalAggState *state, Interval *newval) {
  /* Infinite inputs are counted separately, and do not affect "N" */
  if (INTERVAL_IS_NOBEGIN(newval)) {
    state->nInfcount--;
    return;
  }

  if (INTERVAL_IS_NOEND(newval)) {
    state->pInfcount--;
    return;
  }

  /* Handle the to-be-discarded finite value. */
  state->N--;
  if (state->N > 0)
    finite_interval_mi(&state->sumX, newval, &state->sumX);
  else {
    /* All values discarded, reset the state */
    Assert(state->N == 0);
    memset(&state->sumX, 0, sizeof(state->sumX));
  }
}

/*
 * Transition function for sum() and avg() interval aggregates.
 */
Datum interval_avg_accum(PG_FUNCTION_ARGS) {
  IntervalAggState *state;

  state = PG_ARGISNULL(0) ? NULL : (IntervalAggState *)PG_GETARG_POINTER(0);

  /* Create the state data on the first call */
  if (state == NULL)
    state = makeIntervalAggState(fcinfo);

  if (!PG_ARGISNULL(1))
    do_interval_accum(state, PG_GETARG_INTERVAL_P(1));

  PG_RETURN_POINTER(state);
}

/*
 * Combine function for sum() and avg() interval aggregates.
 *
 * Combine the given internal aggregate states and place the combination in
 * the first argument.
 */
Datum interval_avg_combine(PG_FUNCTION_ARGS) {
  IntervalAggState *state1;
  IntervalAggState *state2;

  state1 = PG_ARGISNULL(0) ? NULL : (IntervalAggState *)PG_GETARG_POINTER(0);
  state2 = PG_ARGISNULL(1) ? NULL : (IntervalAggState *)PG_GETARG_POINTER(1);

  if (state2 == NULL) {
    /*
     * NULL state2 is easy, just return state1, which we know is already
     * in the agg_context
     */
    if (state1 == NULL)
      PG_RETURN_NULL();
    PG_RETURN_POINTER(state1);
  }

  if (state1 == NULL) {
    /* manually copy all fields from state2 to state1 */
    state1 = makeIntervalAggState(fcinfo);

    state1->N = state2->N;
    state1->pInfcount = state2->pInfcount;
    state1->nInfcount = state2->nInfcount;

    state1->sumX.day = state2->sumX.day;
    state1->sumX.month = state2->sumX.month;
    state1->sumX.time = state2->sumX.time;

    PG_RETURN_POINTER(state1);
  }

  state1->N += state2->N;
  state1->pInfcount += state2->pInfcount;
  state1->nInfcount += state2->nInfcount;

  /* Accumulate finite interval values, if any. */
  if (state2->N > 0)
    finite_interval_pl(&state1->sumX, &state2->sumX, &state1->sumX);

  PG_RETURN_POINTER(state1);
}

/*
 * interval_avg_serialize
 *		Serialize IntervalAggState for interval aggregates.
 */
Datum interval_avg_serialize(PG_FUNCTION_ARGS) {
  IntervalAggState *state;
  StringInfoData buf;
  bytea *result;

  /* Ensure we disallow calling when not in aggregate context */
  if (!AggCheckCallContext(fcinfo, NULL))
    elog(ERROR, "aggregate function called in non-aggregate context");

  state = (IntervalAggState *)PG_GETARG_POINTER(0);

  pq_begintypsend(&buf);

  /* N */
  pq_sendint64(&buf, state->N);

  /* sumX */
  pq_sendint64(&buf, state->sumX.time);
  pq_sendint32(&buf, state->sumX.day);
  pq_sendint32(&buf, state->sumX.month);

  /* pInfcount */
  pq_sendint64(&buf, state->pInfcount);

  /* nInfcount */
  pq_sendint64(&buf, state->nInfcount);

  result = pq_endtypsend(&buf);

  PG_RETURN_BYTEA_P(result);
}

/*
 * interval_avg_deserialize
 *		Deserialize bytea into IntervalAggState for interval aggregates.
 */
Datum interval_avg_deserialize(PG_FUNCTION_ARGS) {
  bytea *sstate;
  IntervalAggState *result;
  StringInfoData buf;

  if (!AggCheckCallContext(fcinfo, NULL))
    elog(ERROR, "aggregate function called in non-aggregate context");

  sstate = PG_GETARG_BYTEA_PP(0);

  /*
   * Initialize a StringInfo so that we can "receive" it using the standard
   * recv-function infrastructure.
   */
  initReadOnlyStringInfo(&buf, VARDATA_ANY(sstate), VARSIZE_ANY_EXHDR(sstate));

  result = (IntervalAggState *)palloc0(sizeof(IntervalAggState));

  /* N */
  result->N = pq_getmsgint64(&buf);

  /* sumX */
  result->sumX.time = pq_getmsgint64(&buf);
  result->sumX.day = pq_getmsgint(&buf, 4);
  result->sumX.month = pq_getmsgint(&buf, 4);

  /* pInfcount */
  result->pInfcount = pq_getmsgint64(&buf);

  /* nInfcount */
  result->nInfcount = pq_getmsgint64(&buf);

  pq_getmsgend(&buf);

  PG_RETURN_POINTER(result);
}

/*
 * Inverse transition function for sum() and avg() interval aggregates.
 */
Datum interval_avg_accum_inv(PG_FUNCTION_ARGS) {
  IntervalAggState *state;

  state = PG_ARGISNULL(0) ? NULL : (IntervalAggState *)PG_GETARG_POINTER(0);

  /* Should not get here with no state */
  if (state == NULL)
    elog(ERROR, "interval_avg_accum_inv called with NULL state");

  if (!PG_ARGISNULL(1))
    do_interval_discard(state, PG_GETARG_INTERVAL_P(1));

  PG_RETURN_POINTER(state);
}

/* avg(interval) aggregate final function */
Datum interval_avg(PG_FUNCTION_ARGS) {
  IntervalAggState *state;

  state = PG_ARGISNULL(0) ? NULL : (IntervalAggState *)PG_GETARG_POINTER(0);

  /* If there were no non-null inputs, return NULL */
  if (state == NULL || IA_TOTAL_COUNT(state) == 0)
    PG_RETURN_NULL();

  /*
   * Aggregating infinities that all have the same sign produces infinity
   * with that sign.  Aggregating infinities with different signs results in
   * an error.
   */
  if (state->pInfcount > 0 || state->nInfcount > 0) {
    Interval *result;

    if (state->pInfcount > 0 && state->nInfcount > 0)
      ereport(ERROR, (errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
                      errmsg("interval out of range")));

    result = (Interval *)palloc(sizeof(Interval));
    if (state->pInfcount > 0)
      INTERVAL_NOEND(result);
    else
      INTERVAL_NOBEGIN(result);

    PG_RETURN_INTERVAL_P(result);
  }

  return DirectFunctionCall2(interval_div, IntervalPGetDatum(&state->sumX),
                             Float8GetDatum((double)state->N));
}

/* sum(interval) aggregate final function */
Datum interval_sum(PG_FUNCTION_ARGS) {
  IntervalAggState *state;
  Interval *result;

  state = PG_ARGISNULL(0) ? NULL : (IntervalAggState *)PG_GETARG_POINTER(0);

  /* If there were no non-null inputs, return NULL */
  if (state == NULL || IA_TOTAL_COUNT(state) == 0)
    PG_RETURN_NULL();

  /*
   * Aggregating infinities that all have the same sign produces infinity
   * with that sign.  Aggregating infinities with different signs results in
   * an error.
   */
  if (state->pInfcount > 0 && state->nInfcount > 0)
    ereport(ERROR, (errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
                    errmsg("interval out of range")));

  result = (Interval *)palloc(sizeof(Interval));

  if (state->pInfcount > 0)
    INTERVAL_NOEND(result);
  else if (state->nInfcount > 0)
    INTERVAL_NOBEGIN(result);
  else
    memcpy(result, &state->sumX, sizeof(Interval));

  PG_RETURN_INTERVAL_P(result);
}

/* timestamp_age()
 * Calculate time difference while retaining year/month fields.
 * Note that this does not result in an accurate absolute time span
 *	since year and month are out of context once the arithmetic
 *	is done.
 */
Datum timestamp_age(PG_FUNCTION_ARGS) {
  Timestamp dt1 = PG_GETARG_TIMESTAMP(0);
  Timestamp dt2 = PG_GETARG_TIMESTAMP(1);
  Interval *result = (Interval *)palloc(sizeof(Interval));

  /* ty subtracts the fields, with the borrows, and handles the infinities */
  if (ty_timestamp_age(dt1, dt2, (ty_interval_t *)result))
    ereport(ERROR, (errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
                    errmsg("%s", ty_errmsg())));

  PG_RETURN_INTERVAL_P(result);
}

/* timestamptz_age()
 * Calculate time difference while retaining year/month fields.
 * Note that this does not result in an accurate absolute time span
 *	since year and month are out of context once the arithmetic
 *	is done.
 */
Datum timestamptz_age(PG_FUNCTION_ARGS) {
  TimestampTz dt1 = PG_GETARG_TIMESTAMPTZ(0);
  TimestampTz dt2 = PG_GETARG_TIMESTAMPTZ(1);
  Interval *result = (Interval *)palloc(sizeof(Interval));

  /*
   * ty subtracts the fields of the local times, with the borrows, and
   * handles the infinities. Any difference between the offsets of the two
   * is ignored on purpose.
   */
  if (ty_timestamptz_age(session_timezone, dt1, dt2, (ty_interval_t *)result))
    ereport(ERROR, (errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
                    errmsg("%s", ty_errmsg())));

  PG_RETURN_INTERVAL_P(result);
}

/*----------------------------------------------------------
 *	Conversion operators.
 *---------------------------------------------------------*/

/* timestamp_bin()
 * Bin timestamp into specified interval.
 */
Datum timestamp_bin(PG_FUNCTION_ARGS) {
  Interval *stride = PG_GETARG_INTERVAL_P(0);
  Timestamp timestamp = PG_GETARG_TIMESTAMP(1);
  Timestamp origin = PG_GETARG_TIMESTAMP(2);
  Timestamp result;

  if (ty_timestamp_bin((ty_interval_t *)stride, timestamp, origin, &result))
    ereport(ERROR, (errcode(ty_errnum() == TY_ERR_INVALID
                                ? ERRCODE_FEATURE_NOT_SUPPORTED
                                : ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
                    errmsg("%s", ty_errmsg())));

  PG_RETURN_TIMESTAMP(result);
}

/* timestamp_trunc()
 * Truncate timestamp to specified units.
 */
Datum timestamp_trunc(PG_FUNCTION_ARGS) {
  text *units = PG_GETARG_TEXT_PP(0);
  Timestamp timestamp = PG_GETARG_TIMESTAMP(1);
  Timestamp result;
  int type, val;
  char *lowunits;

  lowunits = downcase_truncate_identifier(VARDATA_ANY(units),
                                          VARSIZE_ANY_EXHDR(units), false);

  type = ty_datetime_unit(lowunits, &val);

  if (!type)
    ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                    errmsg("unit \"%s\" not recognized for type %s", lowunits,
                           format_type_be(TIMESTAMPOID))));

  /* ty cuts the timestamp at the unit */
  if (ty_timestamp_trunc(val, timestamp, &result)) {
    if (ty_errnum() == TY_ERR_INVALID)
      ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                      errmsg("unit \"%s\" not supported for type %s",
                             lowunits, format_type_be(TIMESTAMPOID))));
    ereport(ERROR, (errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
                    errmsg("timestamp out of range")));
  }

  PG_RETURN_TIMESTAMP(result);
}

/* timestamptz_bin()
 * Bin timestamptz into specified interval using specified origin.
 */
Datum timestamptz_bin(PG_FUNCTION_ARGS) {
  Interval *stride = PG_GETARG_INTERVAL_P(0);
  TimestampTz timestamp = PG_GETARG_TIMESTAMPTZ(1);
  TimestampTz origin = PG_GETARG_TIMESTAMPTZ(2);
  TimestampTz result, stride_usecs, tm_diff, tm_modulo, tm_delta;

  if (TIMESTAMP_NOT_FINITE(timestamp))
    PG_RETURN_TIMESTAMPTZ(timestamp);

  if (TIMESTAMP_NOT_FINITE(origin))
    ereport(ERROR, (errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
                    errmsg("origin out of range")));

  if (INTERVAL_NOT_FINITE(stride))
    ereport(ERROR,
            (errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
             errmsg("timestamps cannot be binned into infinite intervals")));

  if (stride->month != 0)
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                    errmsg("timestamps cannot be binned into intervals "
                           "containing months or years")));

  if (unlikely(
          pg_mul_s64_overflow(stride->day, USECS_PER_DAY, &stride_usecs)) ||
      unlikely(pg_add_s64_overflow(stride_usecs, stride->time, &stride_usecs)))
    ereport(ERROR, (errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
                    errmsg("interval out of range")));

  if (stride_usecs <= 0)
    ereport(ERROR, (errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
                    errmsg("stride must be greater than zero")));

  if (unlikely(pg_sub_s64_overflow(timestamp, origin, &tm_diff)))
    ereport(ERROR, (errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
                    errmsg("interval out of range")));

  /* These calculations cannot overflow */
  tm_modulo = tm_diff % stride_usecs;
  tm_delta = tm_diff - tm_modulo;
  result = origin + tm_delta;

  /*
   * We want to round towards -infinity, not 0, when tm_diff is negative and
   * not a multiple of stride_usecs.  This adjustment *can* cause overflow,
   * since the result might now be out of the range origin .. timestamp.
   */
  if (tm_modulo < 0) {
    if (unlikely(pg_sub_s64_overflow(result, stride_usecs, &result)) ||
        !IS_VALID_TIMESTAMP(result))
      ereport(ERROR, (errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
                      errmsg("timestamp out of range")));
  }

  PG_RETURN_TIMESTAMPTZ(result);
}

/*
 * Common code for timestamptz_trunc() and timestamptz_trunc_zone().
 *
 * tzp identifies the zone to truncate with respect to.  We assume
 * infinite timestamps have already been rejected.
 */
static TimestampTz
timestamptz_trunc_internal(text *units, TimestampTz timestamp, pg_tz *tzp) {
  TimestampTz result;
  int type, val;
  char *lowunits;

  lowunits = downcase_truncate_identifier(VARDATA_ANY(units),
                                          VARSIZE_ANY_EXHDR(units), false);

  type = ty_datetime_unit(lowunits, &val);

  if (!type)
    ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                    errmsg("unit \"%s\" not recognized for type %s", lowunits,
                           format_type_be(TIMESTAMPTZOID))));

  /* ty cuts the local time in the zone at the unit (an infinity passes) */
  if (ty_timestamptz_trunc(tzp, val, timestamp, &result)) {
    if (ty_errnum() == TY_ERR_INVALID)
      ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                      errmsg("unit \"%s\" not supported for type %s",
                             lowunits, format_type_be(TIMESTAMPTZOID))));
    ereport(ERROR, (errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
                    errmsg("timestamp out of range")));
  }

  return result;
}

/* timestamptz_trunc()
 * Truncate timestamptz to specified units in session timezone.
 */
Datum timestamptz_trunc(PG_FUNCTION_ARGS) {
  text *units = PG_GETARG_TEXT_PP(0);
  TimestampTz timestamp = PG_GETARG_TIMESTAMPTZ(1);
  TimestampTz result;

  result = timestamptz_trunc_internal(units, timestamp, session_timezone);

  PG_RETURN_TIMESTAMPTZ(result);
}

/* timestamptz_trunc_zone()
 * Truncate timestamptz to specified units in specified timezone.
 */
Datum timestamptz_trunc_zone(PG_FUNCTION_ARGS) {
  text *units = PG_GETARG_TEXT_PP(0);
  TimestampTz timestamp = PG_GETARG_TIMESTAMPTZ(1);
  text *zone = PG_GETARG_TEXT_PP(2);
  TimestampTz result;
  pg_tz *tzp;

  /*
   * Look up the requested timezone.
   */
  tzp = lookup_timezone(zone);

  result = timestamptz_trunc_internal(units, timestamp, tzp);

  PG_RETURN_TIMESTAMPTZ(result);
}

/* interval_trunc()
 * Extract specified field from interval.
 */
Datum interval_trunc(PG_FUNCTION_ARGS) {
  text *units = PG_GETARG_TEXT_PP(0);
  Interval *interval = PG_GETARG_INTERVAL_P(1);
  Interval *result;
  int type, val;
  char *lowunits;

  result = (Interval *)palloc(sizeof(Interval));

  lowunits = downcase_truncate_identifier(VARDATA_ANY(units),
                                          VARSIZE_ANY_EXHDR(units), false);

  type = ty_datetime_unit(lowunits, &val);

  if (!type)
    ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                    errmsg("unit \"%s\" not recognized for type %s", lowunits,
                           format_type_be(INTERVALOID))));

  /* ty cuts the interval at the unit */
  if (ty_interval_trunc(val, (const ty_interval_t *)interval,
                        (ty_interval_t *)result)) {
    if (ty_errnum() == TY_ERR_INVALID)
      ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                      errmsg("unit \"%s\" not supported for type %s", lowunits,
                             format_type_be(INTERVALOID)),
                      (val == DTK_WEEK)
                          ? errdetail("Months usually have fractional weeks.")
                          : 0));
    ereport(ERROR, (errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
                    errmsg("interval out of range")));
  }

  PG_RETURN_INTERVAL_P(result);
}

/* isoweek2j()
 *
 *	Return the Julian day which corresponds to the first day (Monday) of the
 *given ISO 8601 year and week. Julian days are used to convert between ISO week
 *dates and Gregorian dates.
 *
 *	XXX: This function has integer overflow hazards, but restructuring it to
 *	work with the soft-error handling that its callers do is likely more
 *	trouble than it's worth.
 */
int isoweek2j(int year, int week) { return ty_isoweek2j(year, week); }

/* isoweek2date()
 * Convert ISO week of year number to date.
 * The year field must be specified with the ISO year!
 * karel 2000/08/07
 */
void isoweek2date(int woy, int *year, int *mon, int *mday) {
  ty_isoweek2date(woy, year, mon, mday);
}

/* isoweekdate2date()
 *
 *	Convert an ISO 8601 week date (ISO year, ISO week) into a Gregorian
 *date. Gregorian day of week sent so weekday strings can be supplied. Populates
 *year, mon, and mday with the correct Gregorian values. year must be passed in
 *as the ISO year.
 */
void isoweekdate2date(int isoweek, int wday, int *year, int *mon, int *mday) {
  int jday;

  jday = isoweek2j(*year, isoweek);
  /* convert Gregorian week start (Sunday=1) to ISO week start (Monday=1) */
  if (wday > 1)
    jday += wday - 2;
  else
    jday += 6;
  j2date(jday, year, mon, mday);
}

/* date2isoweek()
 *
 *	Returns ISO week number of year.
 */
int date2isoweek(int year, int mon, int mday) {
  return ty_date2isoweek(year, mon, mday);
}

/* date2isoyear()
 *
 *	Returns ISO 8601 year number.
 *	Note: zero or negative results follow the year-zero-exists convention.
 */
int date2isoyear(int year, int mon, int mday) {
  return ty_date2isoyear(year, mon, mday);
}

/* date2isoyearday()
 *
 *	Returns the ISO 8601 day-of-year, given a Gregorian year, month and day.
 *	Possible return values are 1 through 371 (364 in non-leap years).
 */
int date2isoyearday(int year, int mon, int mday) {
  return ty_date2isoyearday(year, mon, mday);
}

/*
 * NonFiniteTimestampTzPart
 *
 *	Used by timestamp_part and timestamptz_part when extracting from
 *infinite timestamp[tz].  Returns +/-Infinity if that is the appropriate
 *result, otherwise returns zero (which should be taken as meaning to return
 *NULL).
 *
 *	Errors thrown here for invalid units should exactly match those that
 *	would be thrown in the calling functions, else there will be unexpected
 *	discrepancies between finite- and infinite-input cases.
 */
static float8 NonFiniteTimestampTzPart(int type, int unit, char *lowunits,
                                       bool isNegative, bool isTz) {
  if ((type != UNITS) && (type != RESERV))
    ereport(ERROR,
            (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
             errmsg("unit \"%s\" not recognized for type %s", lowunits,
                    format_type_be(isTz ? TIMESTAMPTZOID : TIMESTAMPOID))));

  switch (unit) {
    /* Oscillating units */
  case DTK_MICROSEC:
  case DTK_MILLISEC:
  case DTK_SECOND:
  case DTK_MINUTE:
  case DTK_HOUR:
  case DTK_DAY:
  case DTK_MONTH:
  case DTK_QUARTER:
  case DTK_WEEK:
  case DTK_DOW:
  case DTK_ISODOW:
  case DTK_DOY:
  case DTK_TZ:
  case DTK_TZ_MINUTE:
  case DTK_TZ_HOUR:
    return 0.0;

    /* Monotonically-increasing units */
  case DTK_YEAR:
  case DTK_DECADE:
  case DTK_CENTURY:
  case DTK_MILLENNIUM:
  case DTK_JULIAN:
  case DTK_ISOYEAR:
  case DTK_EPOCH:
    if (isNegative)
      return -get_float8_infinity();
    else
      return get_float8_infinity();

  default:
    ereport(ERROR,
            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
             errmsg("unit \"%s\" not supported for type %s", lowunits,
                    format_type_be(isTz ? TIMESTAMPTZOID : TIMESTAMPOID))));
    return 0.0; /* keep compiler quiet */
  }
}

/*
 * ty_part_datum
 *	The Datum of a part that ty made: a numeric, or a float8 as date_part
 *	gives it.
 */
Datum ty_part_datum(const ty_part_t *p, bool retnumeric) {
  if (!retnumeric)
    return Float8GetDatum(p->f);
  switch (p->kind) {
  case TY_PART_INT:
    return NumericGetDatum(int64_to_numeric(p->num));
  case TY_PART_SCALED:
    return NumericGetDatum(int64_div_fast_to_numeric(p->num, p->scale));
  case TY_PART_SUM:
    return NumericGetDatum(numeric_add_opt_error(
        int64_div_fast_to_numeric(p->num, 6), int64_to_numeric(p->num2), NULL));
  default: /* TY_PART_JULIAN: the day, and the fraction of it */
    return NumericGetDatum(numeric_add_opt_error(
        int64_to_numeric(p->num2),
        numeric_div_opt_error(int64_to_numeric(p->num),
                              int64_to_numeric(SECS_PER_DAY * INT64CONST(1000000)),
                              NULL),
        NULL));
  }
}

/* timestamp_part() and extract_timestamp()
 * Extract specified field from timestamp.
 */
static Datum timestamp_part_common(PG_FUNCTION_ARGS, bool retnumeric) {
  text *units = PG_GETARG_TEXT_PP(0);
  Timestamp timestamp = PG_GETARG_TIMESTAMP(1);
  int type, val;
  char *lowunits;
  ty_part_t part;

  /* Lowercase the units string and look it up. */
  lowunits = downcase_truncate_identifier(VARDATA_ANY(units),
                                          VARSIZE_ANY_EXHDR(units), false);

  type = ty_datetime_part_unit(lowunits, 0, &val);

  if (TIMESTAMP_NOT_FINITE(timestamp)) {
    double r = NonFiniteTimestampTzPart(type == 1 ? UNITS
                                        : type == 2 ? RESERV
                                                    : UNKNOWN_FIELD,
                                        val, lowunits,
                                        TIMESTAMP_IS_NOBEGIN(timestamp), false);

    if (r != 0.0) {
      if (retnumeric) {
        if (r < 0)
          return DirectFunctionCall3(numeric_in, CStringGetDatum("-Infinity"),
                                     ObjectIdGetDatum(InvalidOid),
                                     Int32GetDatum(-1));
        else if (r > 0)
          return DirectFunctionCall3(numeric_in, CStringGetDatum("Infinity"),
                                     ObjectIdGetDatum(InvalidOid),
                                     Int32GetDatum(-1));
      } else
        PG_RETURN_FLOAT8(r);
    } else
      PG_RETURN_NULL();
  }

  if (type == 0)
    ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                    errmsg("unit \"%s\" not recognized for type %s", lowunits,
                           format_type_be(TIMESTAMPOID))));

  /* ty takes the timestamp apart and picks the unit */
  if (ty_timestamp_part(type == 2, val, timestamp, &part)) {
    if (ty_errnum() == TY_ERR_RANGE)
      ereport(ERROR, (errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
                      errmsg("timestamp out of range")));
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                    errmsg("unit \"%s\" not supported for type %s", lowunits,
                           format_type_be(TIMESTAMPOID))));
  }
  return ty_part_datum(&part, retnumeric);
}

Datum timestamp_part(PG_FUNCTION_ARGS) {
  return timestamp_part_common(fcinfo, false);
}

Datum extract_timestamp(PG_FUNCTION_ARGS) {
  return timestamp_part_common(fcinfo, true);
}

/* timestamptz_part() and extract_timestamptz()
 * Extract specified field from timestamp with time zone.
 */
static Datum timestamptz_part_common(PG_FUNCTION_ARGS, bool retnumeric) {
  text *units = PG_GETARG_TEXT_PP(0);
  TimestampTz timestamp = PG_GETARG_TIMESTAMPTZ(1);
  int type, val;
  char *lowunits;
  ty_part_t part;

  /* Lowercase the units string and look it up. */
  lowunits = downcase_truncate_identifier(VARDATA_ANY(units),
                                          VARSIZE_ANY_EXHDR(units), false);

  type = ty_datetime_part_unit(lowunits, 0, &val);

  if (TIMESTAMP_NOT_FINITE(timestamp)) {
    double r = NonFiniteTimestampTzPart(type == 1 ? UNITS
                                        : type == 2 ? RESERV
                                                    : UNKNOWN_FIELD,
                                        val, lowunits,
                                        TIMESTAMP_IS_NOBEGIN(timestamp), true);

    if (r != 0.0) {
      if (retnumeric) {
        if (r < 0)
          return DirectFunctionCall3(numeric_in, CStringGetDatum("-Infinity"),
                                     ObjectIdGetDatum(InvalidOid),
                                     Int32GetDatum(-1));
        else if (r > 0)
          return DirectFunctionCall3(numeric_in, CStringGetDatum("Infinity"),
                                     ObjectIdGetDatum(InvalidOid),
                                     Int32GetDatum(-1));
      } else
        PG_RETURN_FLOAT8(r);
    } else
      PG_RETURN_NULL();
  }

  if (type == 0)
    ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                    errmsg("unit \"%s\" not recognized for type %s", lowunits,
                           format_type_be(TIMESTAMPTZOID))));

  /* ty takes the local time in the session's zone apart and picks the unit */
  if (ty_timestamptz_part(session_timezone, type == 2, val, timestamp,
                          &part)) {
    if (ty_errnum() == TY_ERR_RANGE)
      ereport(ERROR, (errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
                      errmsg("timestamp out of range")));
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                    errmsg("unit \"%s\" not supported for type %s", lowunits,
                           format_type_be(TIMESTAMPTZOID))));
  }
  return ty_part_datum(&part, retnumeric);
}

Datum timestamptz_part(PG_FUNCTION_ARGS) {
  return timestamptz_part_common(fcinfo, false);
}

Datum extract_timestamptz(PG_FUNCTION_ARGS) {
  return timestamptz_part_common(fcinfo, true);
}

/*
 * NonFiniteIntervalPart
 *
 *	Used by interval_part when extracting from infinite interval.  Returns
 *	+/-Infinity if that is the appropriate result, otherwise returns zero
 *	(which should be taken as meaning to return NULL).
 *
 *	Errors thrown here for invalid units should exactly match those that
 *	would be thrown in the calling functions, else there will be unexpected
 *	discrepancies between finite- and infinite-input cases.
 */
static float8 NonFiniteIntervalPart(int type, int unit, char *lowunits,
                                    bool isNegative) {
  if ((type != UNITS) && (type != RESERV))
    ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                    errmsg("unit \"%s\" not recognized for type %s", lowunits,
                           format_type_be(INTERVALOID))));

  switch (unit) {
    /* Oscillating units */
  case DTK_MICROSEC:
  case DTK_MILLISEC:
  case DTK_SECOND:
  case DTK_MINUTE:
  case DTK_WEEK:
  case DTK_MONTH:
  case DTK_QUARTER:
    return 0.0;

    /* Monotonically-increasing units */
  case DTK_HOUR:
  case DTK_DAY:
  case DTK_YEAR:
  case DTK_DECADE:
  case DTK_CENTURY:
  case DTK_MILLENNIUM:
  case DTK_EPOCH:
    if (isNegative)
      return -get_float8_infinity();
    else
      return get_float8_infinity();

  default:
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                    errmsg("unit \"%s\" not supported for type %s", lowunits,
                           format_type_be(INTERVALOID))));
    return 0.0; /* keep compiler quiet */
  }
}

/* interval_part() and extract_interval()
 * Extract specified field from interval.
 */
static Datum interval_part_common(PG_FUNCTION_ARGS, bool retnumeric) {
  text *units = PG_GETARG_TEXT_PP(0);
  Interval *interval = PG_GETARG_INTERVAL_P(1);
  int type, val;
  char *lowunits;
  ty_part_t part;

  /* Lowercase the units string and look it up. */
  lowunits = downcase_truncate_identifier(VARDATA_ANY(units),
                                          VARSIZE_ANY_EXHDR(units), false);

  type = ty_datetime_part_unit(lowunits, 0, &val);

  if (INTERVAL_NOT_FINITE(interval)) {
    double r = NonFiniteIntervalPart(type == 1 ? UNITS
                                     : type == 2 ? RESERV
                                                 : UNKNOWN_FIELD,
                                     val, lowunits,
                                     INTERVAL_IS_NOBEGIN(interval));

    if (r != 0.0) {
      if (retnumeric) {
        if (r < 0)
          return DirectFunctionCall3(numeric_in, CStringGetDatum("-Infinity"),
                                     ObjectIdGetDatum(InvalidOid),
                                     Int32GetDatum(-1));
        else if (r > 0)
          return DirectFunctionCall3(numeric_in, CStringGetDatum("Infinity"),
                                     ObjectIdGetDatum(InvalidOid),
                                     Int32GetDatum(-1));
      } else
        PG_RETURN_FLOAT8(r);
    } else
      PG_RETURN_NULL();
  }

  /* only epoch is a reserved word an interval has a part for */
  if (type == 0 || (type == 2 && val != DTK_EPOCH))
    ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                    errmsg("unit \"%s\" not recognized for type %s", lowunits,
                           format_type_be(INTERVALOID))));

  if (ty_interval_part(type == 2, val, (const ty_interval_t *)interval, &part))
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                    errmsg("unit \"%s\" not supported for type %s", lowunits,
                           format_type_be(INTERVALOID))));
  return ty_part_datum(&part, retnumeric);
}

Datum interval_part(PG_FUNCTION_ARGS) {
  return interval_part_common(fcinfo, false);
}

Datum extract_interval(PG_FUNCTION_ARGS) {
  return interval_part_common(fcinfo, true);
}

/*	timestamp_zone()
 *	Encode timestamp type with specified time zone.
 *	This function is just timestamp2timestamptz() except instead of
 *	shifting to the global timezone, we shift to the specified timezone.
 *	This is different from the other AT TIME ZONE cases because instead
 *	of shifting _to_ a new time zone, it sets the time to _be_ the
 *	specified timezone.
 */
Datum timestamp_zone(PG_FUNCTION_ARGS) {
  text *zone = PG_GETARG_TEXT_PP(0);
  Timestamp timestamp = PG_GETARG_TIMESTAMP(1);
  TimestampTz result;
  int tz;
  char tzname[TZ_STRLEN_MAX + 1];
  int type, val;
  pg_tz *tzp;
  struct pg_tm tm;
  fsec_t fsec;

  if (TIMESTAMP_NOT_FINITE(timestamp))
    PG_RETURN_TIMESTAMPTZ(timestamp);

  /*
   * Look up the requested timezone.
   */
  text_to_cstring_buffer(zone, tzname, sizeof(tzname));

  type = DecodeTimezoneName(tzname, &val, &tzp);

  if (type == TZNAME_FIXED_OFFSET) {
    /* fixed-offset abbreviation */
    tz = val;
    result = dt2local(timestamp, tz);
  } else if (type == TZNAME_DYNTZ) {
    /* dynamic-offset abbreviation, resolve using specified time */
    if (timestamp2tm(timestamp, NULL, &tm, &fsec, NULL, tzp) != 0)
      ereport(ERROR, (errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
                      errmsg("timestamp out of range")));
    tz = -DetermineTimeZoneAbbrevOffset(&tm, tzname, tzp);
    result = dt2local(timestamp, tz);
  } else {
    /* full zone name, rotate to that zone: ty gives the instant */
    if (ty_timestamp_to_timestamptz(tzp, timestamp, &result, NULL))
      ereport(ERROR, (errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
                      errmsg("timestamp out of range")));
  }

  if (!IS_VALID_TIMESTAMP(result))
    ereport(ERROR, (errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
                    errmsg("timestamp out of range")));

  PG_RETURN_TIMESTAMPTZ(result);
}

/* timestamp_izone()
 * Encode timestamp type with specified time interval as time zone.
 */
Datum timestamp_izone(PG_FUNCTION_ARGS) {
  Interval *zone = PG_GETARG_INTERVAL_P(0);
  Timestamp timestamp = PG_GETARG_TIMESTAMP(1);
  TimestampTz result;
  int tz;

  if (TIMESTAMP_NOT_FINITE(timestamp))
    PG_RETURN_TIMESTAMPTZ(timestamp);

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

  tz = zone->time / USECS_PER_SEC;

  result = dt2local(timestamp, tz);

  if (!IS_VALID_TIMESTAMP(result))
    ereport(ERROR, (errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
                    errmsg("timestamp out of range")));

  PG_RETURN_TIMESTAMPTZ(result);
} /* timestamp_izone() */

/* TimestampTimestampTzRequiresRewrite()
 *
 * Returns false if the TimeZone GUC setting causes timestamp_timestamptz and
 * timestamptz_timestamp to be no-ops, where the return value has the same
 * bits as the argument.  Since project convention is to assume a GUC changes
 * no more often than STABLE functions change, the answer is valid that long.
 */
bool TimestampTimestampTzRequiresRewrite(void) {
  long offset;

  if (pg_get_timezone_offset(session_timezone, &offset) && offset == 0)
    return false;
  return true;
}

/* timestamp_timestamptz()
 * Convert local timestamp to timestamp at GMT
 */
Datum timestamp_timestamptz(PG_FUNCTION_ARGS) {
  Timestamp timestamp = PG_GETARG_TIMESTAMP(0);

  PG_RETURN_TIMESTAMPTZ(timestamp2timestamptz(timestamp));
}

/*
 * Convert timestamp to timestamp with time zone.
 *
 * On successful conversion, *overflow is set to zero if it's not NULL.
 *
 * If the timestamp is finite but out of the valid range for timestamptz, then:
 * if overflow is NULL, we throw an out-of-range error.
 * if overflow is not NULL, we store +1 or -1 there to indicate the sign
 * of the overflow, and return the appropriate timestamptz infinity.
 */
TimestampTz timestamp2timestamptz_opt_overflow(Timestamp timestamp,
                                               int *overflow) {
  TimestampTz result;

  /* ty gives the instant of the local time in the session's zone */
  if (ty_timestamp_to_timestamptz(session_timezone, timestamp, &result,
                                  overflow))
    ereport(ERROR, (errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
                    errmsg("%s", ty_errmsg())));
  return result;
}

/*
 * Promote timestamp to timestamptz, throwing error for overflow.
 */
static TimestampTz timestamp2timestamptz(Timestamp timestamp) {
  return timestamp2timestamptz_opt_overflow(timestamp, NULL);
}

/* timestamptz_timestamp()
 * Convert timestamp at GMT to local timestamp
 */
Datum timestamptz_timestamp(PG_FUNCTION_ARGS) {
  TimestampTz timestamp = PG_GETARG_TIMESTAMPTZ(0);

  PG_RETURN_TIMESTAMP(timestamptz2timestamp(timestamp));
}

static Timestamp timestamptz2timestamp(TimestampTz timestamp) {
  Timestamp result;

  /* ty gives the local time in the session's zone; the infinities pass */
  if (ty_timestamptz_to_timestamp(session_timezone, timestamp, &result))
    ereport(ERROR, (errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
                    errmsg("%s", ty_errmsg())));
  return result;
}

/* timestamptz_zone()
 * Evaluate timestamp with time zone type at the specified time zone.
 * Returns a timestamp without time zone.
 */
Datum timestamptz_zone(PG_FUNCTION_ARGS) {
  text *zone = PG_GETARG_TEXT_PP(0);
  TimestampTz timestamp = PG_GETARG_TIMESTAMPTZ(1);
  Timestamp result;
  int tz;
  char tzname[TZ_STRLEN_MAX + 1];
  int type, val;
  pg_tz *tzp;

  if (TIMESTAMP_NOT_FINITE(timestamp))
    PG_RETURN_TIMESTAMP(timestamp);

  /*
   * Look up the requested timezone.
   */
  text_to_cstring_buffer(zone, tzname, sizeof(tzname));

  type = DecodeTimezoneName(tzname, &val, &tzp);

  if (type == TZNAME_FIXED_OFFSET) {
    /* fixed-offset abbreviation */
    tz = -val;
    result = dt2local(timestamp, tz);
  } else if (type == TZNAME_DYNTZ) {
    /* dynamic-offset abbreviation, resolve using specified time */
    int isdst;

    tz = DetermineTimeZoneAbbrevOffsetTS(timestamp, tzname, tzp, &isdst);
    result = dt2local(timestamp, tz);
  } else {
    /* full zone name, rotate from that zone: ty gives the local time */
    if (ty_timestamptz_to_timestamp(tzp, timestamp, &result))
      ereport(ERROR, (errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
                      errmsg("timestamp out of range")));
  }

  if (!IS_VALID_TIMESTAMP(result))
    ereport(ERROR, (errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
                    errmsg("timestamp out of range")));

  PG_RETURN_TIMESTAMP(result);
}

/* timestamptz_izone()
 * Encode timestamp with time zone type with specified time interval as time
 * zone. Returns a timestamp without time zone.
 */
Datum timestamptz_izone(PG_FUNCTION_ARGS) {
  Interval *zone = PG_GETARG_INTERVAL_P(0);
  TimestampTz timestamp = PG_GETARG_TIMESTAMPTZ(1);
  Timestamp result;
  int tz;

  if (TIMESTAMP_NOT_FINITE(timestamp))
    PG_RETURN_TIMESTAMP(timestamp);

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

  result = dt2local(timestamp, tz);

  if (!IS_VALID_TIMESTAMP(result))
    ereport(ERROR, (errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
                    errmsg("timestamp out of range")));

  PG_RETURN_TIMESTAMP(result);
}

/* generate_series_timestamp()
 * Generate the set of timestamps from start to finish by step
 */
Datum generate_series_timestamp(PG_FUNCTION_ARGS) {
  FuncCallContext *funcctx;
  generate_series_timestamp_fctx *fctx;
  Timestamp result;

  /* stuff done only on the first call of the function */
  if (SRF_IS_FIRSTCALL()) {
    Timestamp start = PG_GETARG_TIMESTAMP(0);
    Timestamp finish = PG_GETARG_TIMESTAMP(1);
    Interval *step = PG_GETARG_INTERVAL_P(2);
    MemoryContext oldcontext;

    /* create a function context for cross-call persistence */
    funcctx = SRF_FIRSTCALL_INIT();

    /*
     * switch to memory context appropriate for multiple function calls
     */
    oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

    /* allocate memory for user context */
    fctx = (generate_series_timestamp_fctx *)palloc(
        sizeof(generate_series_timestamp_fctx));

    /*
     * Use fctx to keep state from call to call. Seed current with the
     * original start value
     */
    fctx->current = start;
    fctx->finish = finish;
    fctx->step = *step;

    /* Determine sign of the interval */
    fctx->step_sign = interval_sign(&fctx->step);

    if (fctx->step_sign == 0)
      ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                      errmsg("step size cannot equal zero")));

    if (INTERVAL_NOT_FINITE((&fctx->step)))
      ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                      errmsg("step size cannot be infinite")));

    funcctx->user_fctx = fctx;
    MemoryContextSwitchTo(oldcontext);
  }

  /* stuff done on every call of the function */
  funcctx = SRF_PERCALL_SETUP();

  /*
   * get the saved state and use current as the result for this iteration
   */
  fctx = (generate_series_timestamp_fctx *)funcctx->user_fctx;
  result = fctx->current;

  if (fctx->step_sign > 0 ? timestamp_cmp_internal(result, fctx->finish) <= 0
                          : timestamp_cmp_internal(result, fctx->finish) >= 0) {
    /* increment current in preparation for next iteration */
    fctx->current = DatumGetTimestamp(DirectFunctionCall2(
        timestamp_pl_interval, TimestampGetDatum(fctx->current),
        PointerGetDatum(&fctx->step)));

    /* do when there is more left to send */
    SRF_RETURN_NEXT(funcctx, TimestampGetDatum(result));
  } else {
    /* do when there is no more left */
    SRF_RETURN_DONE(funcctx);
  }
}

/* generate_series_timestamptz()
 * Generate the set of timestamps from start to finish by step,
 * doing arithmetic in the specified or session timezone.
 */
static Datum generate_series_timestamptz_internal(FunctionCallInfo fcinfo) {
  FuncCallContext *funcctx;
  generate_series_timestamptz_fctx *fctx;
  TimestampTz result;

  /* stuff done only on the first call of the function */
  if (SRF_IS_FIRSTCALL()) {
    TimestampTz start = PG_GETARG_TIMESTAMPTZ(0);
    TimestampTz finish = PG_GETARG_TIMESTAMPTZ(1);
    Interval *step = PG_GETARG_INTERVAL_P(2);
    text *zone = (PG_NARGS() == 4) ? PG_GETARG_TEXT_PP(3) : NULL;
    MemoryContext oldcontext;

    /* create a function context for cross-call persistence */
    funcctx = SRF_FIRSTCALL_INIT();

    /*
     * switch to memory context appropriate for multiple function calls
     */
    oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

    /* allocate memory for user context */
    fctx = (generate_series_timestamptz_fctx *)palloc(
        sizeof(generate_series_timestamptz_fctx));

    /*
     * Use fctx to keep state from call to call. Seed current with the
     * original start value
     */
    fctx->current = start;
    fctx->finish = finish;
    fctx->step = *step;
    fctx->attimezone = zone ? lookup_timezone(zone) : session_timezone;

    /* Determine sign of the interval */
    fctx->step_sign = interval_sign(&fctx->step);

    if (fctx->step_sign == 0)
      ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                      errmsg("step size cannot equal zero")));

    if (INTERVAL_NOT_FINITE((&fctx->step)))
      ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                      errmsg("step size cannot be infinite")));

    funcctx->user_fctx = fctx;
    MemoryContextSwitchTo(oldcontext);
  }

  /* stuff done on every call of the function */
  funcctx = SRF_PERCALL_SETUP();

  /*
   * get the saved state and use current as the result for this iteration
   */
  fctx = (generate_series_timestamptz_fctx *)funcctx->user_fctx;
  result = fctx->current;

  if (fctx->step_sign > 0 ? timestamp_cmp_internal(result, fctx->finish) <= 0
                          : timestamp_cmp_internal(result, fctx->finish) >= 0) {
    /* increment current in preparation for next iteration */
    fctx->current = timestamptz_pl_interval_internal(fctx->current, &fctx->step,
                                                     fctx->attimezone);

    /* do when there is more left to send */
    SRF_RETURN_NEXT(funcctx, TimestampTzGetDatum(result));
  } else {
    /* do when there is no more left */
    SRF_RETURN_DONE(funcctx);
  }
}

Datum generate_series_timestamptz(PG_FUNCTION_ARGS) {
  return generate_series_timestamptz_internal(fcinfo);
}

Datum generate_series_timestamptz_at_zone(PG_FUNCTION_ARGS) {
  return generate_series_timestamptz_internal(fcinfo);
}

/*
 * Planner support function for generate_series(timestamp, timestamp, interval)
 */
Datum generate_series_timestamp_support(PG_FUNCTION_ARGS) {
  Node *rawreq = (Node *)PG_GETARG_POINTER(0);
  Node *ret = NULL;

  if (IsA(rawreq, SupportRequestRows)) {
    /* Try to estimate the number of rows returned */
    SupportRequestRows *req = (SupportRequestRows *)rawreq;

    if (is_funcclause(req->node)) /* be paranoid */
    {
      List *args = ((FuncExpr *)req->node)->args;
      Node *arg1, *arg2, *arg3;

      /* We can use estimated argument values here */
      arg1 = estimate_expression_value(req->root, (Node *)linitial(args));
      arg2 = estimate_expression_value(req->root, (Node *)lsecond(args));
      arg3 = estimate_expression_value(req->root, (Node *)lthird(args));

      /*
       * If any argument is constant NULL, we can safely assume that
       * zero rows are returned.  Otherwise, if they're all non-NULL
       * constants, we can calculate the number of rows that will be
       * returned.
       */
      if ((IsA(arg1, Const) && ((Const *)arg1)->constisnull) ||
          (IsA(arg2, Const) && ((Const *)arg2)->constisnull) ||
          (IsA(arg3, Const) && ((Const *)arg3)->constisnull)) {
        req->rows = 0;
        ret = (Node *)req;
      } else if (IsA(arg1, Const) && IsA(arg2, Const) && IsA(arg3, Const)) {
        Timestamp start, finish;
        Interval *step;
        Datum diff;
        double dstep;
        int64 dummy;

        start = DatumGetTimestamp(((Const *)arg1)->constvalue);
        finish = DatumGetTimestamp(((Const *)arg2)->constvalue);
        step = DatumGetIntervalP(((Const *)arg3)->constvalue);

        /*
         * Perform some prechecks which could cause timestamp_mi to
         * raise an ERROR.  It's much better to just return some
         * default estimate than error out in a support function.
         */
        if (!TIMESTAMP_NOT_FINITE(start) && !TIMESTAMP_NOT_FINITE(finish) &&
            !pg_sub_s64_overflow(finish, start, &dummy)) {
          diff = DirectFunctionCall2(timestamp_mi, TimestampGetDatum(finish),
                                     TimestampGetDatum(start));

#define INTERVAL_TO_MICROSECONDS(i) ((((double) (i)->month * DAYS_PER_MONTH + (i)->day)) * USECS_PER_DAY + (i)->time)

          dstep = INTERVAL_TO_MICROSECONDS(step);

          /* This equation works for either sign of step */
          if (dstep != 0.0) {
            Interval *idiff = DatumGetIntervalP(diff);
            double ddiff = INTERVAL_TO_MICROSECONDS(idiff);

            req->rows = floor(ddiff / dstep + 1.0);
            ret = (Node *)req;
          }
#undef INTERVAL_TO_MICROSECONDS
        }
      }
    }
  }

  PG_RETURN_POINTER(ret);
}

/* timestamp_at_local()
 * timestamptz_at_local()
 *
 * The regression tests do not like two functions with the same proargs and
 * prosrc but different proname, but the grammar for AT LOCAL needs an
 * overloaded name to handle both types of timestamp, so we make simple
 * wrappers for it.
 */
Datum timestamp_at_local(PG_FUNCTION_ARGS) {
  return timestamp_timestamptz(fcinfo);
}

Datum timestamptz_at_local(PG_FUNCTION_ARGS) {
  return timestamptz_timestamp(fcinfo);
}

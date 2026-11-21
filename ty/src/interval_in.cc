/*
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * Portions Copyright (c) 2026, CK Tan: the changes for ty,
 * under the MIT License (https://github.com/cktan/digres/blob/main/LICENSE)
 */

/*
 * interval_in.cc -- the text in of interval: the tokenizer, the decoder for
 * the postgres and sql forms, and the decoder for the ISO 8601 form.
 *
 * Derived from PostgreSQL's src/backend/utils/adt/datetime.c (ParseDateTime,
 * DecodeInterval, DecodeISO8601Interval and their helpers, and the tables of
 * words), with PostgreSQL's names of the types and the error codes. The
 * function ty_interval_parse at the end is ty's.
 */
#pragma GCC diagnostic ignored "-Wunused-parameter"

#include "ty_datetime.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "error.h"

namespace {

typedef int64_t int64;
typedef int32_t int32;
typedef int fsec_t;
typedef int64_t TimeOffset;

#define unlikely(x) __builtin_expect((x) != 0, 0)
#define Assert(x) ((void)0)
#define pg_tolower(c) ((char)tolower((unsigned char)(c)))
#define pg_add_s32_overflow(a, b, r) __builtin_add_overflow(a, b, r)
#define pg_sub_s32_overflow(a, b, r) __builtin_sub_overflow(a, b, r)
#define pg_mul_s32_overflow(a, b, r) __builtin_mul_overflow(a, b, r)
#define pg_add_s64_overflow(a, b, r) __builtin_add_overflow(a, b, r)
#define pg_sub_s64_overflow(a, b, r) __builtin_sub_overflow(a, b, r)
#define pg_mul_s64_overflow(a, b, r) __builtin_mul_overflow(a, b, r)
#define INT64CONST(x) INT64_C(x)
#define PG_INT64_MAX INT64_MAX
#define PG_INT64_MIN INT64_MIN
#define PG_INT32_MAX INT32_MAX
#define PG_INT32_MIN INT32_MIN
#define SECS_PER_DAY 86400
#define USECS_PER_DAY INT64_C(86400000000)
#define USECS_PER_HOUR INT64_C(3600000000)
#define USECS_PER_MINUTE INT64_C(60000000)
#define USECS_PER_SEC INT64_C(1000000)
#define MONTHS_PER_YEAR 12
#define DAYS_PER_MONTH 30
#define DAYS_PER_YEAR 365.25
#define HOURS_PER_DAY 24
#define MINS_PER_HOUR 60
#define SECS_PER_HOUR 3600
#define SECS_PER_MINUTE 60
#define DAYS_PER_WEEK 7
#define MONTHS_PER_YEAR 12
#define TS_PREC_INV 1000000.0
#define TSROUND(j) (rint(((double)(j)) * TS_PREC_INV) / TS_PREC_INV)

/* the range of an interval (from the typmod) */
#define INTERVAL_MASK(b) (1 << (b))
#define INTERVAL_FULL_RANGE (0x7FFF)

#define DAGO "ago"
#define EARLY "-infinity"
#define LATE "infinity"
#define DMICROSEC "usecond"
#define DMILLISEC "msecond"
#define DSECOND "second"
#define DMINUTE "minute"
#define DHOUR "hour"
#define DDAY "day"
#define DWEEK "week"
#define DMONTH "month"
#define DQUARTER "quarter"
#define DYEAR "year"
#define DDECADE "decade"
#define DCENTURY "century"
#define DMILLENNIUM "millennium"
#define DA_D "ad"
#define DB_C "bc"
#define DTIMEZONE "timezone"
#define DCURRENT "current"
#define EPOCH "epoch"
#define INVALID "invalid"
#define NOW "now"
#define TODAY "today"
#define TOMORROW "tomorrow"
#define YESTERDAY "yesterday"
#define ZULU "zulu"
#define AM 0
#define PM 1
#define HR24 2
#define AD 0
#define BC 1
#define RESERV 0
#define MONTH 1
#define YEAR 2
#define DAY 3
#define JULIAN 4
#define TZ 5
#define DTZ 6
#define DYNTZ 7
#define IGNORE_DTF 8
#define AMPM 9
#define HOUR 10
#define MINUTE 11
#define SECOND 12
#define MILLISECOND 13
#define MICROSECOND 14
#define DOY 15
#define DOW 16
#define UNITS 17
#define ADBC 18
#define AGO 19
#define ABS_BEFORE 20
#define ABS_AFTER 21
#define ISODATE 22
#define ISOTIME 23
#define WEEK 24
#define DECADE 25
#define CENTURY 26
#define MILLENNIUM 27
#define DTZMOD 28
#define UNKNOWN_FIELD 31
#define DTK_NUMBER 0
#define DTK_STRING 1
#define DTK_DATE 2
#define DTK_TIME 3
#define DTK_TZ 4
#define DTK_AGO 5
#define DTK_SPECIAL 6
#define DTK_EARLY 9
#define DTK_LATE 10
#define DTK_EPOCH 11
#define DTK_NOW 12
#define DTK_YESTERDAY 13
#define DTK_TODAY 14
#define DTK_TOMORROW 15
#define DTK_ZULU 16
#define DTK_DELTA 17
#define DTK_SECOND 18
#define DTK_MINUTE 19
#define DTK_HOUR 20
#define DTK_DAY 21
#define DTK_WEEK 22
#define DTK_MONTH 23
#define DTK_QUARTER 24
#define DTK_YEAR 25
#define DTK_DECADE 26
#define DTK_CENTURY 27
#define DTK_MILLENNIUM 28
#define DTK_MILLISEC 29
#define DTK_MICROSEC 30
#define DTK_JULIAN 31
#define DTK_DOW 32
#define DTK_DOY 33
#define DTK_TZ_HOUR 34
#define DTK_TZ_MINUTE 35
#define DTK_ISOYEAR 36
#define DTK_ISODOW 37
#define DTK_M(t) (0x01 << (t))
#define DTK_ALL_SECS_M \
  (DTK_M(SECOND) | DTK_M(MILLISECOND) | DTK_M(MICROSECOND))
#define DTK_DATE_M (DTK_M(YEAR) | DTK_M(MONTH) | DTK_M(DAY))
#define DTK_TIME_M (DTK_M(HOUR) | DTK_M(MINUTE) | DTK_ALL_SECS_M)
#define MAXDATELEN 128
#define MAXDATEFIELDS 25
#define TOKMAXLEN 10

#define DTERR_BAD_FORMAT (-1)
#define DTERR_FIELD_OVERFLOW (-2)
#define DTERR_MD_FIELD_OVERFLOW (-3)
#define DTERR_INTERVAL_OVERFLOW (-4)
#define DTERR_TZDISP_OVERFLOW (-5)
#define DTERR_BAD_TIMEZONE (-6)
#define DTERR_BAD_ZONE_ABBREV (-7)

typedef struct {
  char token[TOKMAXLEN + 1]; /* always NUL-terminated */
  char type;                 /* see field type codes above */
  int32 value;               /* meaning depends on type */
} datetkn;

/* An interval as it is read: the months, days and microseconds apart. */
struct pg_itm_in {
  int64 tm_usec;
  int tm_mday;
  int tm_mon;
  int tm_year;
};

/* The broken-down time the decoder fills for a time field. */
struct pg_itm {
  int tm_usec;
  int tm_sec;
  int tm_min;
  int64 tm_hour;
  int tm_mday;
  int tm_mon;
  int tm_year;
};

/* strtol that reports an int that does not fit with ERANGE. */
static int strtoint(const char *str, char **endptr, int base) {
  long val = strtol(str, endptr, base);

  if (val != (int)val) {
    errno = ERANGE;
  }
  return (int)val;
}

static int64 strtoi64(const char *str, char **endptr, int base) {
  return (int64)strtoll(str, endptr, base);
}

/* The settings the host passes in: the interval style of the session. */
thread_local int g_intervalstyle = 0;

#define INTSTYLE_SQL_STANDARD 2

int DecodeUnits(int field, const char *lowtoken, int *val);
int DecodeSpecial(int field, const char *lowtoken, int *val);
int DecodeInterval(char **field, int *ftype, int nf, int range, int *dtype,
                   struct pg_itm_in *itm_in);
int DecodeISO8601Interval(char *str, int *dtype, struct pg_itm_in *itm_in);
int ParseDateTime(const char *timestr, char *workbuf, size_t buflen,
                  char **field, int *ftype, int maxfields, int *numfields);
static const datetkn *datebsearch(const char *key, const datetkn *base,
                                  int nel);
static int DecodeTimeForInterval(char *str, int fmask, int range, int *tmask,
                                 struct pg_itm_in *itm_in);
static int DecodeTimeCommon(char *str, int fmask, int range, int *tmask,
                            struct pg_itm *itm);
static int ParseFraction(char *cp, double *frac);
static int ParseFractionalSecond(char *cp, fsec_t *fsec);
static bool AdjustFractMicroseconds(double frac, int64 scale,
                                    struct pg_itm_in *itm_in);
static bool AdjustFractDays(double frac, int scale, struct pg_itm_in *itm_in);
static bool AdjustFractYears(double frac, int scale, struct pg_itm_in *itm_in);
static bool AdjustMicroseconds(int64 val, double fval, int64 scale,
                               struct pg_itm_in *itm_in);
static bool AdjustDays(int64 val, int scale, struct pg_itm_in *itm_in);
static bool AdjustMonths(int64 val, struct pg_itm_in *itm_in);
static bool AdjustYears(int64 val, int scale, struct pg_itm_in *itm_in);

/* The tables of words, in order, for the binary search. */
static const datetkn datetktbl[] = {
    /* token, type, value */
    {"+infinity", RESERV, DTK_LATE}, /* same as "infinity" */
    {EARLY, RESERV, DTK_EARLY},      /* "-infinity" reserved for "early time" */
    {DA_D, ADBC, AD},                /* "ad" for years > 0 */
    {"allballs", RESERV, DTK_ZULU},  /* 00:00:00 */
    {"am", AMPM, AM},
    {"apr", MONTH, 4},
    {"april", MONTH, 4},
    {"at", IGNORE_DTF, 0}, /* "at" (throwaway) */
    {"aug", MONTH, 8},
    {"august", MONTH, 8},
    {DB_C, ADBC, BC},      /* "bc" for years <= 0 */
    {"d", UNITS, DTK_DAY}, /* "day of month" for ISO input */
    {"dec", MONTH, 12},
    {"december", MONTH, 12},
    {"dow", UNITS, DTK_DOW}, /* day of week */
    {"doy", UNITS, DTK_DOY}, /* day of year */
    {"dst", DTZMOD, SECS_PER_HOUR},
    {EPOCH, RESERV, DTK_EPOCH}, /* "epoch" reserved for system epoch time */
    {"feb", MONTH, 2},
    {"february", MONTH, 2},
    {"fri", DOW, 5},
    {"friday", DOW, 5},
    {"h", UNITS, DTK_HOUR},          /* "hour" */
    {LATE, RESERV, DTK_LATE},        /* "infinity" reserved for "late time" */
    {"isodow", UNITS, DTK_ISODOW},   /* ISO day of week, Sunday == 7 */
    {"isoyear", UNITS, DTK_ISOYEAR}, /* year in terms of the ISO week date */
    {"j", UNITS, DTK_JULIAN},
    {"jan", MONTH, 1},
    {"january", MONTH, 1},
    {"jd", UNITS, DTK_JULIAN},
    {"jul", MONTH, 7},
    {"julian", UNITS, DTK_JULIAN},
    {"july", MONTH, 7},
    {"jun", MONTH, 6},
    {"june", MONTH, 6},
    {"m", UNITS, DTK_MONTH}, /* "month" for ISO input */
    {"mar", MONTH, 3},
    {"march", MONTH, 3},
    {"may", MONTH, 5},
    {"mm", UNITS, DTK_MINUTE}, /* "minute" for ISO input */
    {"mon", DOW, 1},
    {"monday", DOW, 1},
    {"nov", MONTH, 11},
    {"november", MONTH, 11},
    {NOW, RESERV, DTK_NOW}, /* current transaction time */
    {"oct", MONTH, 10},
    {"october", MONTH, 10},
    {"on", IGNORE_DTF, 0}, /* "on" (throwaway) */
    {"pm", AMPM, PM},
    {"s", UNITS, DTK_SECOND}, /* "seconds" for ISO input */
    {"sat", DOW, 6},
    {"saturday", DOW, 6},
    {"sep", MONTH, 9},
    {"sept", MONTH, 9},
    {"september", MONTH, 9},
    {"sun", DOW, 0},
    {"sunday", DOW, 0},
    {"t", ISOTIME, DTK_TIME}, /* Filler for ISO time fields */
    {"thu", DOW, 4},
    {"thur", DOW, 4},
    {"thurs", DOW, 4},
    {"thursday", DOW, 4},
    {TODAY, RESERV, DTK_TODAY},       /* midnight */
    {TOMORROW, RESERV, DTK_TOMORROW}, /* tomorrow midnight */
    {"tue", DOW, 2},
    {"tues", DOW, 2},
    {"tuesday", DOW, 2},
    {"wed", DOW, 3},
    {"wednesday", DOW, 3},
    {"weds", DOW, 3},
    {"y", UNITS, DTK_YEAR},            /* "year" for ISO input */
    {YESTERDAY, RESERV, DTK_YESTERDAY} /* yesterday midnight */
};

static const datetkn deltatktbl[] = {
    /* token, type, value */
    {"@", IGNORE_DTF, 0},         /* postgres relative prefix */
    {DAGO, AGO, 0},               /* "ago" indicates negative time offset */
    {"c", UNITS, DTK_CENTURY},    /* "century" relative */
    {"cent", UNITS, DTK_CENTURY}, /* "century" relative */
    {"centuries", UNITS, DTK_CENTURY},    /* "centuries" relative */
    {DCENTURY, UNITS, DTK_CENTURY},       /* "century" relative */
    {"d", UNITS, DTK_DAY},                /* "day" relative */
    {DDAY, UNITS, DTK_DAY},               /* "day" relative */
    {"days", UNITS, DTK_DAY},             /* "days" relative */
    {"dec", UNITS, DTK_DECADE},           /* "decade" relative */
    {DDECADE, UNITS, DTK_DECADE},         /* "decade" relative */
    {"decades", UNITS, DTK_DECADE},       /* "decades" relative */
    {"decs", UNITS, DTK_DECADE},          /* "decades" relative */
    {"h", UNITS, DTK_HOUR},               /* "hour" relative */
    {DHOUR, UNITS, DTK_HOUR},             /* "hour" relative */
    {"hours", UNITS, DTK_HOUR},           /* "hours" relative */
    {"hr", UNITS, DTK_HOUR},              /* "hour" relative */
    {"hrs", UNITS, DTK_HOUR},             /* "hours" relative */
    {"m", UNITS, DTK_MINUTE},             /* "minute" relative */
    {"microsecon", UNITS, DTK_MICROSEC},  /* "microsecond" relative */
    {"mil", UNITS, DTK_MILLENNIUM},       /* "millennium" relative */
    {"millennia", UNITS, DTK_MILLENNIUM}, /* "millennia" relative */
    {DMILLENNIUM, UNITS, DTK_MILLENNIUM}, /* "millennium" relative */
    {"millisecon", UNITS, DTK_MILLISEC},  /* relative */
    {"mils", UNITS, DTK_MILLENNIUM},      /* "millennia" relative */
    {"min", UNITS, DTK_MINUTE},           /* "minute" relative */
    {"mins", UNITS, DTK_MINUTE},          /* "minutes" relative */
    {DMINUTE, UNITS, DTK_MINUTE},         /* "minute" relative */
    {"minutes", UNITS, DTK_MINUTE},       /* "minutes" relative */
    {"mon", UNITS, DTK_MONTH},            /* "months" relative */
    {"mons", UNITS, DTK_MONTH},           /* "months" relative */
    {DMONTH, UNITS, DTK_MONTH},           /* "month" relative */
    {"months", UNITS, DTK_MONTH},
    {"ms", UNITS, DTK_MILLISEC},
    {"msec", UNITS, DTK_MILLISEC},
    {DMILLISEC, UNITS, DTK_MILLISEC},
    {"mseconds", UNITS, DTK_MILLISEC},
    {"msecs", UNITS, DTK_MILLISEC},
    {"qtr", UNITS, DTK_QUARTER},    /* "quarter" relative */
    {DQUARTER, UNITS, DTK_QUARTER}, /* "quarter" relative */
    {"s", UNITS, DTK_SECOND},
    {"sec", UNITS, DTK_SECOND},
    {DSECOND, UNITS, DTK_SECOND},
    {"seconds", UNITS, DTK_SECOND},
    {"secs", UNITS, DTK_SECOND},
    {DTIMEZONE, UNITS, DTK_TZ},           /* "timezone" time offset */
    {"timezone_h", UNITS, DTK_TZ_HOUR},   /* timezone hour units */
    {"timezone_m", UNITS, DTK_TZ_MINUTE}, /* timezone minutes units */
    {"us", UNITS, DTK_MICROSEC},          /* "microsecond" relative */
    {"usec", UNITS, DTK_MICROSEC},        /* "microsecond" relative */
    {DMICROSEC, UNITS, DTK_MICROSEC},     /* "microsecond" relative */
    {"useconds", UNITS, DTK_MICROSEC},    /* "microseconds" relative */
    {"usecs", UNITS, DTK_MICROSEC},       /* "microseconds" relative */
    {"w", UNITS, DTK_WEEK},               /* "week" relative */
    {DWEEK, UNITS, DTK_WEEK},             /* "week" relative */
    {"weeks", UNITS, DTK_WEEK},           /* "weeks" relative */
    {"y", UNITS, DTK_YEAR},               /* "year" relative */
    {DYEAR, UNITS, DTK_YEAR},             /* "year" relative */
    {"years", UNITS, DTK_YEAR},           /* "years" relative */
    {"yr", UNITS, DTK_YEAR},              /* "year" relative */
    {"yrs", UNITS, DTK_YEAR}              /* "years" relative */
};

static const int szdatetktbl = sizeof datetktbl / sizeof datetktbl[0];
static const int szdeltatktbl = sizeof deltatktbl / sizeof deltatktbl[0];
static thread_local const datetkn *datecache[MAXDATEFIELDS] = {nullptr};
static thread_local const datetkn *deltacache[MAXDATEFIELDS] = {nullptr};

/*
 * Add val * multiplier to *sum.
 * Returns true if successful, false on overflow.
 */
static bool int64_multiply_add(int64 val, int64 multiplier, int64 *sum) {
  int64 product;

  if (pg_mul_s64_overflow(val, multiplier, &product) ||
      pg_add_s64_overflow(*sum, product, sum))
    return false;
  return true;
}

/*
 * Multiply frac by scale (to produce microseconds) and add to itm_in->tm_usec.
 * Returns true if successful, false if itm_in overflows.
 */
static bool AdjustFractMicroseconds(double frac, int64 scale,
                                    struct pg_itm_in *itm_in) {
  int64 usec;

  /* Fast path for common case */
  if (frac == 0)
    return true;

  /*
   * We assume the input frac has abs value less than 1, so overflow of frac
   * or usec is not an issue for interesting values of scale.
   */
  frac *= scale;
  usec = (int64)frac;

  /* Round off any fractional microsecond */
  frac -= usec;
  if (frac > 0.5)
    usec++;
  else if (frac < -0.5)
    usec--;

  return !pg_add_s64_overflow(itm_in->tm_usec, usec, &itm_in->tm_usec);
}

/*
 * Multiply frac by scale (to produce days).  Add the integral part of the
 * result to itm_in->tm_mday, the fractional part to itm_in->tm_usec.
 * Returns true if successful, false if itm_in overflows.
 */
static bool AdjustFractDays(double frac, int scale, struct pg_itm_in *itm_in) {
  int extra_days;

  /* Fast path for common case */
  if (frac == 0)
    return true;

  /*
   * We assume the input frac has abs value less than 1, so overflow of frac
   * or extra_days is not an issue.
   */
  frac *= scale;
  extra_days = (int)frac;

  /* ... but this could overflow, if tm_mday is already nonzero */
  if (pg_add_s32_overflow(itm_in->tm_mday, extra_days, &itm_in->tm_mday))
    return false;

  /* Handle any fractional day */
  frac -= extra_days;
  return AdjustFractMicroseconds(frac, USECS_PER_DAY, itm_in);
}

/*
 * Multiply frac by scale (to produce years), then further scale up to months.
 * Add the integral part of the result to itm_in->tm_mon, discarding any
 * fractional part.
 * Returns true if successful, false if itm_in overflows.
 */
static bool AdjustFractYears(double frac, int scale, struct pg_itm_in *itm_in) {
  /*
   * As above, we assume abs(frac) < 1, so this can't overflow for any
   * interesting value of scale.
   */
  int extra_months = (int)rint(frac * scale * MONTHS_PER_YEAR);

  return !pg_add_s32_overflow(itm_in->tm_mon, extra_months, &itm_in->tm_mon);
}

/*
 * Add (val + fval) * scale to itm_in->tm_usec.
 * Returns true if successful, false if itm_in overflows.
 */
static bool AdjustMicroseconds(int64 val, double fval, int64 scale,
                               struct pg_itm_in *itm_in) {
  /* Handle the integer part */
  if (!int64_multiply_add(val, scale, &itm_in->tm_usec))
    return false;
  /* Handle the float part */
  return AdjustFractMicroseconds(fval, scale, itm_in);
}

/*
 * Multiply val by scale (to produce days) and add to itm_in->tm_mday.
 * Returns true if successful, false if itm_in overflows.
 */
static bool AdjustDays(int64 val, int scale, struct pg_itm_in *itm_in) {
  int days;

  if (val < INT_MIN || val > INT_MAX)
    return false;
  return !pg_mul_s32_overflow((int32)val, scale, &days) &&
         !pg_add_s32_overflow(itm_in->tm_mday, days, &itm_in->tm_mday);
}

/*
 * Add val to itm_in->tm_mon (no need for scale here, as val is always
 * in months already).
 * Returns true if successful, false if itm_in overflows.
 */
static bool AdjustMonths(int64 val, struct pg_itm_in *itm_in) {
  if (val < INT_MIN || val > INT_MAX)
    return false;
  return !pg_add_s32_overflow(itm_in->tm_mon, (int32)val, &itm_in->tm_mon);
}

/*
 * Multiply val by scale (to produce years) and add to itm_in->tm_year.
 * Returns true if successful, false if itm_in overflows.
 */
static bool AdjustYears(int64 val, int scale, struct pg_itm_in *itm_in) {
  int years;

  if (val < INT_MIN || val > INT_MAX)
    return false;
  return !pg_mul_s32_overflow((int32)val, scale, &years) &&
         !pg_add_s32_overflow(itm_in->tm_year, years, &itm_in->tm_year);
}

/*
 * Parse the fractional part of a number (decimal point and optional digits,
 * followed by end of string).  Returns the fractional value into *frac.
 *
 * Returns 0 if successful, DTERR code if bogus input detected.
 */
static int ParseFraction(char *cp, double *frac) {
  /* Caller should always pass the start of the fraction part */
  Assert(*cp == '.');

  /*
   * We want to allow just "." with no digits, but some versions of strtod
   * will report EINVAL for that, so special-case it.
   */
  if (cp[1] == '\0') {
    *frac = 0;
  } else {
    /*
     * On the other hand, let's reject anything that's not digits after
     * the ".".  strtod is happy with input like ".123e9", but that'd
     * break callers' expectation that the result is in 0..1.  (It's quite
     * difficult to get here with such input, but not impossible.)
     */
    if (strspn(cp + 1, "0123456789") != strlen(cp + 1))
      return DTERR_BAD_FORMAT;

    errno = 0;
    *frac = strtod(cp, &cp);
    /* check for parse failure (probably redundant given prior check) */
    if (*cp != '\0' || errno != 0)
      return DTERR_BAD_FORMAT;
  }
  return 0;
}

/*
 * Fetch a fractional-second value with suitable error checking.
 * Same as ParseFraction except we convert the result to integer microseconds.
 */
static int ParseFractionalSecond(char *cp, fsec_t *fsec) {
  double frac;
  int dterr;

  dterr = ParseFraction(cp, &frac);
  if (dterr)
    return dterr;
  *fsec = rint(frac * 1000000);
  return 0;
}

/* ParseDateTime()
 *	Break string into tokens based on a date/time context.
 *	Returns 0 if successful, DTERR code if bogus input detected.
 *
 * timestr - the input string
 * workbuf - workspace for field string storage. This must be
 *	 larger than the largest legal input for this datetime type --
 *	 some additional space will be needed to NUL terminate fields.
 * buflen - the size of workbuf
 * field[] - pointers to field strings are returned in this array
 * ftype[] - field type indicators are returned in this array
 * maxfields - dimensions of the above two arrays
 * *numfields - set to the actual number of fields detected
 *
 * The fields extracted from the input are stored as separate,
 * null-terminated strings in the workspace at workbuf. Any text is
 * converted to lower case.
 *
 * Several field types are assigned:
 *	DTK_NUMBER - digits and (possibly) a decimal point
 *	DTK_DATE - digits and two delimiters, or digits and text
 *	DTK_TIME - digits, colon delimiters, and possibly a decimal point
 *	DTK_STRING - text (no digits or punctuation)
 *	DTK_SPECIAL - leading "+" or "-" followed by text
 *	DTK_TZ - leading "+" or "-" followed by digits (also eats ':', '.', '-')
 *
 * Note that some field types can hold unexpected items:
 *	DTK_NUMBER can hold date fields (yy.ddd)
 *	DTK_STRING can hold months (January) and time zones (PST)
 *	DTK_DATE can hold time zone names (America/New_York, GMT-8)
 */
int ParseDateTime(const char *timestr, char *workbuf, size_t buflen,
                  char **field, int *ftype, int maxfields, int *numfields) {
  int nf = 0;
  const char *cp = timestr;
  char *bufp = workbuf;
  const char *bufend = workbuf + buflen;

  /*
   * Set the character pointed-to by "bufptr" to "newchar", and increment
   * "bufptr". "end" gives the end of the buffer -- we return an error if
   * there is no space left to append a character to the buffer. Note that
   * "bufptr" is evaluated twice.
   */
#define APPEND_CHAR(bufptr, end, newchar)		\
	do											\
	{											\
		if (((bufptr) + 1) >= (end))			\
			return DTERR_BAD_FORMAT;			\
		*(bufptr)++ = newchar;					\
	} while (0)

  /* outer loop through fields */
  while (*cp != '\0') {
    /* Ignore spaces between fields */
    if (isspace((unsigned char)*cp)) {
      cp++;
      continue;
    }

    /* Record start of current field */
    if (nf >= maxfields)
      return DTERR_BAD_FORMAT;
    field[nf] = bufp;

    /* leading digit? then date or time */
    if (isdigit((unsigned char)*cp)) {
      APPEND_CHAR(bufp, bufend, *cp++);
      while (isdigit((unsigned char)*cp))
        APPEND_CHAR(bufp, bufend, *cp++);

      /* time field? */
      if (*cp == ':') {
        ftype[nf] = DTK_TIME;
        APPEND_CHAR(bufp, bufend, *cp++);
        while (isdigit((unsigned char)*cp) || (*cp == ':') || (*cp == '.'))
          APPEND_CHAR(bufp, bufend, *cp++);
      }
      /* date field? allow embedded text month */
      else if (*cp == '-' || *cp == '/' || *cp == '.') {
        /* save delimiting character to use later */
        char delim = *cp;

        APPEND_CHAR(bufp, bufend, *cp++);
        /* second field is all digits? then no embedded text month */
        if (isdigit((unsigned char)*cp)) {
          ftype[nf] = ((delim == '.') ? DTK_NUMBER : DTK_DATE);
          while (isdigit((unsigned char)*cp))
            APPEND_CHAR(bufp, bufend, *cp++);

          /*
           * insist that the delimiters match to get a three-field
           * date.
           */
          if (*cp == delim) {
            ftype[nf] = DTK_DATE;
            APPEND_CHAR(bufp, bufend, *cp++);
            while (isdigit((unsigned char)*cp) || *cp == delim)
              APPEND_CHAR(bufp, bufend, *cp++);
          }
        } else {
          ftype[nf] = DTK_DATE;
          while (isalnum((unsigned char)*cp) || *cp == delim)
            APPEND_CHAR(bufp, bufend, pg_tolower((unsigned char)*cp++));
        }
      }

      /*
       * otherwise, number only and will determine year, month, day, or
       * concatenated fields later...
       */
      else
        ftype[nf] = DTK_NUMBER;
    }
    /* Leading decimal point? Then fractional seconds... */
    else if (*cp == '.') {
      APPEND_CHAR(bufp, bufend, *cp++);
      while (isdigit((unsigned char)*cp))
        APPEND_CHAR(bufp, bufend, *cp++);

      ftype[nf] = DTK_NUMBER;
    }

    /*
     * text? then date string, month, day of week, special, or timezone
     */
    else if (isalpha((unsigned char)*cp)) {
      bool is_date;

      ftype[nf] = DTK_STRING;
      APPEND_CHAR(bufp, bufend, pg_tolower((unsigned char)*cp++));
      while (isalpha((unsigned char)*cp))
        APPEND_CHAR(bufp, bufend, pg_tolower((unsigned char)*cp++));

      /*
       * Dates can have embedded '-', '/', or '.' separators.  It could
       * also be a timezone name containing embedded '/', '+', '-', '_',
       * or ':' (but '_' or ':' can't be the first punctuation). If the
       * next character is a digit or '+', we need to check whether what
       * we have so far is a recognized non-timezone keyword --- if so,
       * don't believe that this is the start of a timezone.
       */
      is_date = false;
      if (*cp == '-' || *cp == '/' || *cp == '.')
        is_date = true;
      else if (*cp == '+' || isdigit((unsigned char)*cp)) {
        *bufp = '\0'; /* null-terminate current field value */
        /* we need search only the core token table, not TZ names */
        if (datebsearch(field[nf], datetktbl, szdatetktbl) == NULL)
          is_date = true;
      }
      if (is_date) {
        ftype[nf] = DTK_DATE;
        do {
          APPEND_CHAR(bufp, bufend, pg_tolower((unsigned char)*cp++));
        } while (*cp == '+' || *cp == '-' || *cp == '/' || *cp == '_' ||
                 *cp == '.' || *cp == ':' || isalnum((unsigned char)*cp));
      }
    }
    /* sign? then special or numeric timezone */
    else if (*cp == '+' || *cp == '-') {
      APPEND_CHAR(bufp, bufend, *cp++);
      /* soak up leading whitespace */
      while (isspace((unsigned char)*cp))
        cp++;
      /* numeric timezone? */
      /* note that "DTK_TZ" could also be a signed float or yyyy-mm */
      if (isdigit((unsigned char)*cp)) {
        ftype[nf] = DTK_TZ;
        APPEND_CHAR(bufp, bufend, *cp++);
        while (isdigit((unsigned char)*cp) || *cp == ':' || *cp == '.' ||
               *cp == '-')
          APPEND_CHAR(bufp, bufend, *cp++);
      }
      /* special? */
      else if (isalpha((unsigned char)*cp)) {
        ftype[nf] = DTK_SPECIAL;
        APPEND_CHAR(bufp, bufend, pg_tolower((unsigned char)*cp++));
        while (isalpha((unsigned char)*cp))
          APPEND_CHAR(bufp, bufend, pg_tolower((unsigned char)*cp++));
      }
      /* otherwise something wrong... */
      else
        return DTERR_BAD_FORMAT;
    }
    /* ignore other punctuation but use as delimiter */
    else if (ispunct((unsigned char)*cp)) {
      cp++;
      continue;
    }
    /* otherwise, something is not right... */
    else
      return DTERR_BAD_FORMAT;

    /* force in a delimiter after each field */
    *bufp++ = '\0';
    nf++;
  }

  *numfields = nf;

  return 0;
}

/* DecodeTimeCommon()
 * Decode time string which includes delimiters.
 * Return 0 if okay, a DTERR code if not.
 * tmask and itm are output parameters.
 *
 * This code is shared between the timestamp and interval cases.
 * We return a struct pg_itm (of which only the tm_usec, tm_sec, tm_min,
 * and tm_hour fields are used) and let the wrapper functions below
 * convert and range-check as necessary.
 */
static int DecodeTimeCommon(char *str, int fmask, int range, int *tmask,
                            struct pg_itm *itm) {
  char *cp;
  int dterr;
  fsec_t fsec = 0;

  *tmask = DTK_TIME_M;

  errno = 0;
  itm->tm_hour = strtoi64(str, &cp, 10);
  if (errno == ERANGE)
    return DTERR_FIELD_OVERFLOW;
  if (*cp != ':')
    return DTERR_BAD_FORMAT;
  errno = 0;
  itm->tm_min = strtoint(cp + 1, &cp, 10);
  if (errno == ERANGE)
    return DTERR_FIELD_OVERFLOW;
  if (*cp == '\0') {
    itm->tm_sec = 0;
    /* If it's a MINUTE TO SECOND interval, take 2 fields as being mm:ss */
    if (range == (INTERVAL_MASK(MINUTE) | INTERVAL_MASK(SECOND))) {
      if (itm->tm_hour > INT_MAX || itm->tm_hour < INT_MIN)
        return DTERR_FIELD_OVERFLOW;
      itm->tm_sec = itm->tm_min;
      itm->tm_min = (int)itm->tm_hour;
      itm->tm_hour = 0;
    }
  } else if (*cp == '.') {
    /* always assume mm:ss.sss is MINUTE TO SECOND */
    dterr = ParseFractionalSecond(cp, &fsec);
    if (dterr)
      return dterr;
    if (itm->tm_hour > INT_MAX || itm->tm_hour < INT_MIN)
      return DTERR_FIELD_OVERFLOW;
    itm->tm_sec = itm->tm_min;
    itm->tm_min = (int)itm->tm_hour;
    itm->tm_hour = 0;
  } else if (*cp == ':') {
    errno = 0;
    itm->tm_sec = strtoint(cp + 1, &cp, 10);
    if (errno == ERANGE)
      return DTERR_FIELD_OVERFLOW;
    if (*cp == '.') {
      dterr = ParseFractionalSecond(cp, &fsec);
      if (dterr)
        return dterr;
    } else if (*cp != '\0')
      return DTERR_BAD_FORMAT;
  } else
    return DTERR_BAD_FORMAT;

  /* do a sanity check; but caller must check the range of tm_hour */
  if (itm->tm_hour < 0 || itm->tm_min < 0 || itm->tm_min > MINS_PER_HOUR - 1 ||
      itm->tm_sec < 0 || itm->tm_sec > SECS_PER_MINUTE || fsec < 0 ||
      fsec > USECS_PER_SEC)
    return DTERR_FIELD_OVERFLOW;

  itm->tm_usec = (int)fsec;

  return 0;
}

/* DecodeTimeForInterval()
 * Decode time string which includes delimiters.
 * Return 0 if okay, a DTERR code if not.
 *
 * This version is used for intervals.  The results are returned into
 * itm_in->tm_usec.
 */
static int DecodeTimeForInterval(char *str, int fmask, int range, int *tmask,
                                 struct pg_itm_in *itm_in) {
  struct pg_itm itm;
  int dterr;

  dterr = DecodeTimeCommon(str, fmask, range, tmask, &itm);
  if (dterr)
    return dterr;

  itm_in->tm_usec = itm.tm_usec;
  if (!int64_multiply_add(itm.tm_hour, USECS_PER_HOUR, &itm_in->tm_usec) ||
      !int64_multiply_add(itm.tm_min, USECS_PER_MINUTE, &itm_in->tm_usec) ||
      !int64_multiply_add(itm.tm_sec, USECS_PER_SEC, &itm_in->tm_usec))
    return DTERR_FIELD_OVERFLOW;

  return 0;
}

/* ClearPgItmIn
 *
 * Zero out a pg_itm_in
 */
static inline void ClearPgItmIn(struct pg_itm_in *itm_in) {
  itm_in->tm_usec = 0;
  itm_in->tm_mday = 0;
  itm_in->tm_mon = 0;
  itm_in->tm_year = 0;
}

/* DecodeInterval()
 * Interpret previously parsed fields for general time interval.
 * Returns 0 if successful, DTERR code if bogus input detected.
 * dtype and itm_in are output parameters.
 *
 * Allow "date" field DTK_DATE since this could be just
 *	an unsigned floating point number. - thomas 1997-11-16
 *
 * Allow ISO-style time span, with implicit units on number of days
 *	preceding an hh:mm:ss field. - thomas 1998-04-30
 *
 * itm_in remains undefined for infinite interval values for which dtype alone
 * suffices.
 */
int DecodeInterval(char **field, int *ftype, int nf, int range, int *dtype,
                   struct pg_itm_in *itm_in) {
  bool force_negative = false;
  bool is_before = false;
  bool parsing_unit_val = false;
  char *cp;
  int fmask = 0, tmask, type, uval;
  int i;
  int dterr;
  int64 val;
  double fval;

  *dtype = DTK_DELTA;
  type = IGNORE_DTF;
  ClearPgItmIn(itm_in);

  /*----------
   * The SQL standard defines the interval literal
   *	 '-1 1:00:00'
   * to mean "negative 1 days and negative 1 hours", while Postgres
   * traditionally treats this as meaning "negative 1 days and positive
   * 1 hours".  In SQL_STANDARD intervalstyle, we apply the leading sign
   * to all fields if there are no other explicit signs.
   *
   * We leave the signs alone if there are additional explicit signs.
   * This protects us against misinterpreting postgres-style dump output,
   * since the postgres-style output code has always put an explicit sign on
   * all fields following a negative field.  But note that SQL-spec output
   * is ambiguous and can be misinterpreted on load!	(So it's best practice
   * to dump in postgres style, not SQL style.)
   *----------
   */
  if (g_intervalstyle == INTSTYLE_SQL_STANDARD && nf > 0 && *field[0] == '-') {
    force_negative = true;
    /* Check for additional explicit signs */
    for (i = 1; i < nf; i++) {
      if (*field[i] == '-' || *field[i] == '+') {
        force_negative = false;
        break;
      }
    }
  }

  /* read through list backwards to pick up units before values */
  for (i = nf - 1; i >= 0; i--) {
    switch (ftype[i]) {
    case DTK_TIME:
      dterr = DecodeTimeForInterval(field[i], fmask, range, &tmask, itm_in);
      if (dterr)
        return dterr;
      if (force_negative && itm_in->tm_usec > 0)
        itm_in->tm_usec = -itm_in->tm_usec;
      type = DTK_DAY;
      parsing_unit_val = false;
      break;

    case DTK_TZ:

      /*
       * Timezone means a token with a leading sign character and at
       * least one digit; there could be ':', '.', '-' embedded in
       * it as well.
       */
      Assert(*field[i] == '-' || *field[i] == '+');

      /*
       * Check for signed hh:mm or hh:mm:ss.  If so, process exactly
       * like DTK_TIME case above, plus handling the sign.
       */
      if (strchr(field[i] + 1, ':') != NULL &&
          DecodeTimeForInterval(field[i] + 1, fmask, range, &tmask, itm_in) ==
              0) {
        if (*field[i] == '-') {
          /* flip the sign on time field */
          if (itm_in->tm_usec == PG_INT64_MIN)
            return DTERR_FIELD_OVERFLOW;
          itm_in->tm_usec = -itm_in->tm_usec;
        }

        if (force_negative && itm_in->tm_usec > 0)
          itm_in->tm_usec = -itm_in->tm_usec;

        /*
         * Set the next type to be a day, if units are not
         * specified. This handles the case of '1 +02:03' since we
         * are reading right to left.
         */
        type = DTK_DAY;
        parsing_unit_val = false;
        break;
      }

      /*
       * Otherwise, fall through to DTK_NUMBER case, which can
       * handle signed float numbers and signed year-month values.
       */

      /* FALLTHROUGH */

    case DTK_DATE:
    case DTK_NUMBER:
      if (type == IGNORE_DTF) {
        /* use typmod to decide what rightmost field is */
        switch (range) {
        case INTERVAL_MASK(YEAR):
          type = DTK_YEAR;
          break;
        case INTERVAL_MASK(MONTH):
        case INTERVAL_MASK(YEAR) | INTERVAL_MASK(MONTH):
          type = DTK_MONTH;
          break;
        case INTERVAL_MASK(DAY):
          type = DTK_DAY;
          break;
        case INTERVAL_MASK(HOUR):
        case INTERVAL_MASK(DAY) | INTERVAL_MASK(HOUR):
          type = DTK_HOUR;
          break;
        case INTERVAL_MASK(MINUTE):
        case INTERVAL_MASK(HOUR) | INTERVAL_MASK(MINUTE):
        case INTERVAL_MASK(DAY) | INTERVAL_MASK(HOUR) | INTERVAL_MASK(MINUTE):
          type = DTK_MINUTE;
          break;
        case INTERVAL_MASK(SECOND):
        case INTERVAL_MASK(MINUTE) | INTERVAL_MASK(SECOND):
        case INTERVAL_MASK(HOUR) | INTERVAL_MASK(MINUTE) |
            INTERVAL_MASK(SECOND):
        case INTERVAL_MASK(DAY) | INTERVAL_MASK(HOUR) | INTERVAL_MASK(MINUTE) |
            INTERVAL_MASK(SECOND):
          type = DTK_SECOND;
          break;
        default:
          type = DTK_SECOND;
          break;
        }
      }

      errno = 0;
      val = strtoi64(field[i], &cp, 10);
      if (errno == ERANGE)
        return DTERR_FIELD_OVERFLOW;

      if (*cp == '-') {
        /* SQL "years-months" syntax */
        int val2;

        val2 = strtoint(cp + 1, &cp, 10);
        if (errno == ERANGE || val2 < 0 || val2 >= MONTHS_PER_YEAR)
          return DTERR_FIELD_OVERFLOW;
        if (*cp != '\0')
          return DTERR_BAD_FORMAT;
        type = DTK_MONTH;
        if (*field[i] == '-')
          val2 = -val2;
        if (pg_mul_s64_overflow(val, MONTHS_PER_YEAR, &val))
          return DTERR_FIELD_OVERFLOW;
        if (pg_add_s64_overflow(val, val2, &val))
          return DTERR_FIELD_OVERFLOW;
        fval = 0;
      } else if (*cp == '.') {
        dterr = ParseFraction(cp, &fval);
        if (dterr)
          return dterr;
        if (*field[i] == '-')
          fval = -fval;
      } else if (*cp == '\0')
        fval = 0;
      else
        return DTERR_BAD_FORMAT;

      tmask = 0; /* DTK_M(type); */

      if (force_negative) {
        /* val and fval should be of same sign, but test anyway */
        if (val > 0)
          val = -val;
        if (fval > 0)
          fval = -fval;
      }

      switch (type) {
      case DTK_MICROSEC:
        if (!AdjustMicroseconds(val, fval, 1, itm_in))
          return DTERR_FIELD_OVERFLOW;
        tmask = DTK_M(MICROSECOND);
        break;

      case DTK_MILLISEC:
        if (!AdjustMicroseconds(val, fval, 1000, itm_in))
          return DTERR_FIELD_OVERFLOW;
        tmask = DTK_M(MILLISECOND);
        break;

      case DTK_SECOND:
        if (!AdjustMicroseconds(val, fval, USECS_PER_SEC, itm_in))
          return DTERR_FIELD_OVERFLOW;

        /*
         * If any subseconds were specified, consider this
         * microsecond and millisecond input as well.
         */
        if (fval == 0)
          tmask = DTK_M(SECOND);
        else
          tmask = DTK_ALL_SECS_M;
        break;

      case DTK_MINUTE:
        if (!AdjustMicroseconds(val, fval, USECS_PER_MINUTE, itm_in))
          return DTERR_FIELD_OVERFLOW;
        tmask = DTK_M(MINUTE);
        break;

      case DTK_HOUR:
        if (!AdjustMicroseconds(val, fval, USECS_PER_HOUR, itm_in))
          return DTERR_FIELD_OVERFLOW;
        tmask = DTK_M(HOUR);
        type = DTK_DAY; /* set for next field */
        break;

      case DTK_DAY:
        if (!AdjustDays(val, 1, itm_in) ||
            !AdjustFractMicroseconds(fval, USECS_PER_DAY, itm_in))
          return DTERR_FIELD_OVERFLOW;
        tmask = DTK_M(DAY);
        break;

      case DTK_WEEK:
        if (!AdjustDays(val, 7, itm_in) || !AdjustFractDays(fval, 7, itm_in))
          return DTERR_FIELD_OVERFLOW;
        tmask = DTK_M(WEEK);
        break;

      case DTK_MONTH:
        if (!AdjustMonths(val, itm_in) ||
            !AdjustFractDays(fval, DAYS_PER_MONTH, itm_in))
          return DTERR_FIELD_OVERFLOW;
        tmask = DTK_M(MONTH);
        break;

      case DTK_YEAR:
        if (!AdjustYears(val, 1, itm_in) || !AdjustFractYears(fval, 1, itm_in))
          return DTERR_FIELD_OVERFLOW;
        tmask = DTK_M(YEAR);
        break;

      case DTK_DECADE:
        if (!AdjustYears(val, 10, itm_in) ||
            !AdjustFractYears(fval, 10, itm_in))
          return DTERR_FIELD_OVERFLOW;
        tmask = DTK_M(DECADE);
        break;

      case DTK_CENTURY:
        if (!AdjustYears(val, 100, itm_in) ||
            !AdjustFractYears(fval, 100, itm_in))
          return DTERR_FIELD_OVERFLOW;
        tmask = DTK_M(CENTURY);
        break;

      case DTK_MILLENNIUM:
        if (!AdjustYears(val, 1000, itm_in) ||
            !AdjustFractYears(fval, 1000, itm_in))
          return DTERR_FIELD_OVERFLOW;
        tmask = DTK_M(MILLENNIUM);
        break;

      default:
        return DTERR_BAD_FORMAT;
      }
      parsing_unit_val = false;
      break;

    case DTK_STRING:
    case DTK_SPECIAL:
      /* reject consecutive unhandled units */
      if (parsing_unit_val)
        return DTERR_BAD_FORMAT;
      type = DecodeUnits(i, field[i], &uval);
      if (type == UNKNOWN_FIELD)
        type = DecodeSpecial(i, field[i], &uval);
      if (type == IGNORE_DTF)
        continue;

      tmask = 0; /* DTK_M(type); */
      switch (type) {
      case UNITS:
        type = uval;
        parsing_unit_val = true;
        break;

      case AGO:

        /*
         * "ago" is only allowed to appear at the end of the
         * interval.
         */
        if (i != nf - 1)
          return DTERR_BAD_FORMAT;
        is_before = true;
        type = uval;
        break;

      case RESERV:
        tmask = (DTK_DATE_M | DTK_TIME_M);

        /*
         * Only reserved words corresponding to infinite
         * intervals are accepted.
         */
        if (uval != DTK_LATE && uval != DTK_EARLY)
          return DTERR_BAD_FORMAT;

        /*
         * Infinity cannot be followed by anything else. We
         * could allow "ago" to reverse the sign of infinity
         * but using signed infinity is more intuitive.
         */
        if (i != nf - 1)
          return DTERR_BAD_FORMAT;

        *dtype = uval;
        break;

      default:
        return DTERR_BAD_FORMAT;
      }
      break;

    default:
      return DTERR_BAD_FORMAT;
    }

    if (tmask & fmask)
      return DTERR_BAD_FORMAT;
    fmask |= tmask;
  }

  /* ensure that at least one time field has been found */
  if (fmask == 0)
    return DTERR_BAD_FORMAT;

  /* reject if unit appeared and was never handled */
  if (parsing_unit_val)
    return DTERR_BAD_FORMAT;

  /* finally, AGO negates everything */
  if (is_before) {
    if (itm_in->tm_usec == PG_INT64_MIN || itm_in->tm_mday == INT_MIN ||
        itm_in->tm_mon == INT_MIN || itm_in->tm_year == INT_MIN)
      return DTERR_FIELD_OVERFLOW;

    itm_in->tm_usec = -itm_in->tm_usec;
    itm_in->tm_mday = -itm_in->tm_mday;
    itm_in->tm_mon = -itm_in->tm_mon;
    itm_in->tm_year = -itm_in->tm_year;
  }

  return 0;
}

/*
 * Helper functions to avoid duplicated code in DecodeISO8601Interval.
 *
 * Parse a decimal value and break it into integer and fractional parts.
 * Set *endptr to end+1 of the parsed substring.
 * Returns 0 or DTERR code.
 */
static int ParseISO8601Number(char *str, char **endptr, int64 *ipart,
                              double *fpart) {
  double val;

  /*
   * Historically this has accepted anything that strtod() would take,
   * notably including "e" notation, so continue doing that.  This is
   * slightly annoying because the precision of double is less than that of
   * int64, so we would lose accuracy for inputs larger than 2^53 or so.
   * However, historically we rejected inputs outside the int32 range,
   * making that concern moot.  What we do now is reject abs(val) above
   * 1.0e15 (a round number a bit less than 2^50), so that any accepted
   * value will have an exact integer part, and thereby a fraction part with
   * abs(*fpart) less than 1.  In the absence of field complaints it doesn't
   * seem worth working harder.
   */
  if (!(isdigit((unsigned char)*str) || *str == '-' || *str == '.'))
    return DTERR_BAD_FORMAT;
  errno = 0;
  val = strtod(str, endptr);
  /* did we not see anything that looks like a double? */
  if (*endptr == str || errno != 0)
    return DTERR_BAD_FORMAT;
  /* watch out for overflow, including infinities; reject NaN too */
  if (isnan(val) || val < -1.0e15 || val > 1.0e15)
    return DTERR_FIELD_OVERFLOW;
  /* be very sure we truncate towards zero (cf dtrunc()) */
  if (val >= 0)
    *ipart = (int64)floor(val);
  else
    *ipart = (int64)-floor(-val);
  *fpart = val - *ipart;
  /* Callers expect this to hold */
  Assert(*fpart > -1.0 && *fpart < 1.0);
  return 0;
}

/*
 * Determine number of integral digits in a valid ISO 8601 number field
 * (we should ignore sign and any fraction part)
 */
static int ISO8601IntegerWidth(char *fieldstart) {
  /* We might have had a leading '-' */
  if (*fieldstart == '-')
    fieldstart++;
  return strspn(fieldstart, "0123456789");
}

/* DecodeISO8601Interval()
 *	Decode an ISO 8601 time interval of the "format with designators"
 *	(section 4.4.3.2) or "alternative format" (section 4.4.3.3)
 *	Examples:  P1D	for 1 day
 *			   PT1H for 1 hour
 *			   P2Y6M7DT1H30M for 2 years, 6 months, 7 days 1 hour 30
 *min P0002-06-07T01:30:00 the same value in alternative format
 *
 * Returns 0 if successful, DTERR code if bogus input detected.
 * Note: error code should be DTERR_BAD_FORMAT if input doesn't look like
 * ISO8601, otherwise this could cause unexpected error messages.
 * dtype and itm_in are output parameters.
 *
 *	A couple exceptions from the spec:
 *	 - a week field ('W') may coexist with other units
 *	 - allows decimals in fields other than the least significant unit.
 */
int DecodeISO8601Interval(char *str, int *dtype, struct pg_itm_in *itm_in) {
  bool datepart = true;
  bool havefield = false;

  *dtype = DTK_DELTA;
  ClearPgItmIn(itm_in);

  if (strlen(str) < 2 || str[0] != 'P')
    return DTERR_BAD_FORMAT;

  str++;
  while (*str) {
    char *fieldstart;
    int64 val;
    double fval;
    char unit;
    int dterr;

    if (*str == 'T') /* T indicates the beginning of the time part */
    {
      datepart = false;
      havefield = false;
      str++;
      continue;
    }

    fieldstart = str;
    dterr = ParseISO8601Number(str, &str, &val, &fval);
    if (dterr)
      return dterr;

    /*
     * Note: we could step off the end of the string here.  Code below
     * *must* exit the loop if unit == '\0'.
     */
    unit = *str++;

    if (datepart) {
      switch (unit) /* before T: Y M W D */
      {
      case 'Y':
        if (!AdjustYears(val, 1, itm_in) || !AdjustFractYears(fval, 1, itm_in))
          return DTERR_FIELD_OVERFLOW;
        break;
      case 'M':
        if (!AdjustMonths(val, itm_in) ||
            !AdjustFractDays(fval, DAYS_PER_MONTH, itm_in))
          return DTERR_FIELD_OVERFLOW;
        break;
      case 'W':
        if (!AdjustDays(val, 7, itm_in) || !AdjustFractDays(fval, 7, itm_in))
          return DTERR_FIELD_OVERFLOW;
        break;
      case 'D':
        if (!AdjustDays(val, 1, itm_in) ||
            !AdjustFractMicroseconds(fval, USECS_PER_DAY, itm_in))
          return DTERR_FIELD_OVERFLOW;
        break;
      case 'T': /* ISO 8601 4.4.3.3 Alternative Format / Basic */
      case '\0':
        if (ISO8601IntegerWidth(fieldstart) == 8 && !havefield) {
          if (!AdjustYears(val / 10000, 1, itm_in) ||
              !AdjustMonths((val / 100) % 100, itm_in) ||
              !AdjustDays(val % 100, 1, itm_in) ||
              !AdjustFractMicroseconds(fval, USECS_PER_DAY, itm_in))
            return DTERR_FIELD_OVERFLOW;
          if (unit == '\0')
            return 0;
          datepart = false;
          havefield = false;
          continue;
        }
        /* Else fall through to extended alternative format */
        /* FALLTHROUGH */
      case '-': /* ISO 8601 4.4.3.3 Alternative Format,
                 * Extended */
        if (havefield)
          return DTERR_BAD_FORMAT;

        if (!AdjustYears(val, 1, itm_in) || !AdjustFractYears(fval, 1, itm_in))
          return DTERR_FIELD_OVERFLOW;
        if (unit == '\0')
          return 0;
        if (unit == 'T') {
          datepart = false;
          havefield = false;
          continue;
        }

        dterr = ParseISO8601Number(str, &str, &val, &fval);
        if (dterr)
          return dterr;
        if (!AdjustMonths(val, itm_in) ||
            !AdjustFractDays(fval, DAYS_PER_MONTH, itm_in))
          return DTERR_FIELD_OVERFLOW;
        if (*str == '\0')
          return 0;
        if (*str == 'T') {
          datepart = false;
          havefield = false;
          continue;
        }
        if (*str != '-')
          return DTERR_BAD_FORMAT;
        str++;

        dterr = ParseISO8601Number(str, &str, &val, &fval);
        if (dterr)
          return dterr;
        if (!AdjustDays(val, 1, itm_in) ||
            !AdjustFractMicroseconds(fval, USECS_PER_DAY, itm_in))
          return DTERR_FIELD_OVERFLOW;
        if (*str == '\0')
          return 0;
        if (*str == 'T') {
          datepart = false;
          havefield = false;
          continue;
        }
        return DTERR_BAD_FORMAT;
      default:
        /* not a valid date unit suffix */
        return DTERR_BAD_FORMAT;
      }
    } else {
      switch (unit) /* after T: H M S */
      {
      case 'H':
        if (!AdjustMicroseconds(val, fval, USECS_PER_HOUR, itm_in))
          return DTERR_FIELD_OVERFLOW;
        break;
      case 'M':
        if (!AdjustMicroseconds(val, fval, USECS_PER_MINUTE, itm_in))
          return DTERR_FIELD_OVERFLOW;
        break;
      case 'S':
        if (!AdjustMicroseconds(val, fval, USECS_PER_SEC, itm_in))
          return DTERR_FIELD_OVERFLOW;
        break;
      case '\0': /* ISO 8601 4.4.3.3 Alternative Format */
        if (ISO8601IntegerWidth(fieldstart) == 6 && !havefield) {
          if (!AdjustMicroseconds(val / 10000, 0, USECS_PER_HOUR, itm_in) ||
              !AdjustMicroseconds((val / 100) % 100, 0, USECS_PER_MINUTE,
                                  itm_in) ||
              !AdjustMicroseconds(val % 100, 0, USECS_PER_SEC, itm_in) ||
              !AdjustFractMicroseconds(fval, 1, itm_in))
            return DTERR_FIELD_OVERFLOW;
          return 0;
        }
        /* Else fall through to extended alternative format */
        /* FALLTHROUGH */
      case ':': /* ISO 8601 4.4.3.3 Alternative Format,
                 * Extended */
        if (havefield)
          return DTERR_BAD_FORMAT;

        if (!AdjustMicroseconds(val, fval, USECS_PER_HOUR, itm_in))
          return DTERR_FIELD_OVERFLOW;
        if (unit == '\0')
          return 0;

        dterr = ParseISO8601Number(str, &str, &val, &fval);
        if (dterr)
          return dterr;
        if (!AdjustMicroseconds(val, fval, USECS_PER_MINUTE, itm_in))
          return DTERR_FIELD_OVERFLOW;
        if (*str == '\0')
          return 0;
        if (*str != ':')
          return DTERR_BAD_FORMAT;
        str++;

        dterr = ParseISO8601Number(str, &str, &val, &fval);
        if (dterr)
          return dterr;
        if (!AdjustMicroseconds(val, fval, USECS_PER_SEC, itm_in))
          return DTERR_FIELD_OVERFLOW;
        if (*str == '\0')
          return 0;
        return DTERR_BAD_FORMAT;

      default:
        /* not a valid time unit suffix */
        return DTERR_BAD_FORMAT;
      }
    }

    havefield = true;
  }

  return 0;
}

/* DecodeUnits()
 * Decode text string using lookup table.
 *
 * This routine recognizes keywords associated with time interval units.
 *
 * Given string must be lowercased already.
 *
 * Implement a cache lookup since it is likely that dates
 *	will be related in format.
 */
int DecodeUnits(int field, const char *lowtoken, int *val) {
  int type;
  const datetkn *tp;

  tp = deltacache[field];
  /* use strncmp so that we match truncated tokens */
  if (tp == NULL || strncmp(lowtoken, tp->token, TOKMAXLEN) != 0) {
    tp = datebsearch(lowtoken, deltatktbl, szdeltatktbl);
  }
  if (tp == NULL) {
    type = UNKNOWN_FIELD;
    *val = 0;
  } else {
    deltacache[field] = tp;
    type = tp->type;
    *val = tp->value;
  }

  return type;
} /* DecodeUnits() */

/* DecodeSpecial()
 * Decode text string using lookup table.
 *
 * Recognizes the keywords listed in datetktbl.
 * Note: at one time this would also recognize timezone abbreviations,
 * but no more; use DecodeTimezoneAbbrev for that.
 *
 * Given string must be lowercased already.
 *
 * Implement a cache lookup since it is likely that dates
 *	will be related in format.
 */
int DecodeSpecial(int field, const char *lowtoken, int *val) {
  int type;
  const datetkn *tp;

  tp = datecache[field];
  /* use strncmp so that we match truncated tokens */
  if (tp == NULL || strncmp(lowtoken, tp->token, TOKMAXLEN) != 0) {
    tp = datebsearch(lowtoken, datetktbl, szdatetktbl);
  }
  if (tp == NULL) {
    type = UNKNOWN_FIELD;
    *val = 0;
  } else {
    datecache[field] = tp;
    type = tp->type;
    *val = tp->value;
  }

  return type;
}

/* datebsearch()
 * Binary search -- from Knuth (6.2.1) Algorithm B.  Special case like this
 * is WAY faster than the generic bsearch().
 */
static const datetkn *datebsearch(const char *key, const datetkn *base,
                                  int nel) {
  if (nel > 0) {
    const datetkn *last = base + nel - 1, *position;
    int result;

    while (last >= base) {
      position = base + ((last - base) >> 1);
      /* precheck the first character for a bit of extra speed */
      result = (int)key[0] - (int)position->token[0];
      if (result == 0) {
        /* use strncmp so that we match truncated tokens */
        result = strncmp(key, position->token, TOKMAXLEN);
        if (result == 0)
          return position;
      }
      if (result < 0)
        last = position - 1;
      else
        base = position + 1;
    }
  }
  return NULL;
}

} // namespace

/*
 * Read the text of an interval, as PostgreSQL's interval_in does.
 * 1. Cut it into fields with the tokenizer, and decode them by the interval
 *    style, into months, days and microseconds; or the words "infinity" and
 *    "-infinity".
 * 2. If that finds the format bad, try the ISO 8601 form, on the original
 *    text.
 * 3. A field overflow is an interval overflow.
 * 4. The months of the years must fit an int32.
 */
int ty_interval_parse(const char *str, int range, int intervalstyle,
                      ty_interval_t *out, int *dterr) {
  char workbuf[256];
  char *field[MAXDATEFIELDS];
  int ftype[MAXDATEFIELDS];
  int nf, dtype = 0, d;
  struct pg_itm_in itm_in;

  itm_in.tm_year = 0;
  itm_in.tm_mon = 0;
  itm_in.tm_mday = 0;
  itm_in.tm_usec = 0;
  g_intervalstyle = intervalstyle;
  d = ParseDateTime(str, workbuf, sizeof(workbuf), field, ftype, MAXDATEFIELDS,
                    &nf);
  if (d == 0) {
    d = DecodeInterval(field, ftype, nf, range, &dtype, &itm_in);
  }
  if (d == DTERR_BAD_FORMAT) {
    d = DecodeISO8601Interval(const_cast<char *>(str), &dtype, &itm_in);
  }
  if (d != 0) {
    if (d == DTERR_FIELD_OVERFLOW) {
      d = DTERR_INTERVAL_OVERFLOW;
    }
    *dterr = d;
    return ty_internal::set_error(d == DTERR_BAD_FORMAT ? TY_ERR_INVALID
                                                        : TY_ERR_RANGE,
                                  "invalid interval text");
  }
  switch (dtype) {
  case DTK_DELTA: {
    int64_t months = (int64_t)itm_in.tm_year * MONTHS_PER_YEAR + itm_in.tm_mon;

    if (months > INT_MAX || months < INT_MIN) {
      *dterr = 1;
      return ty_internal::set_error(TY_ERR_RANGE, "interval out of range");
    }
    out->month = (int32_t)months;
    out->day = itm_in.tm_mday;
    out->time = itm_in.tm_usec;
    break;
  }
  case DTK_LATE:
    ty_interval_set_noend(out);
    break;
  case DTK_EARLY:
    ty_interval_set_nobegin(out);
    break;
  default:
    *dterr = DTERR_BAD_FORMAT;
    return ty_internal::set_error(TY_ERR_INVALID, "invalid interval text");
  }
  *dterr = 0;
  return 0;
}

/* The code of a unit word, if it is one: see ty_datetime.h. */
int ty_datetime_unit(const char *lowtoken, int *code) {
  int val = 0;

  if (DecodeUnits(0, lowtoken, &val) != UNITS) {
    return 0;
  }
  *code = val;
  return 1;
}

/*
 * The words that extract is nearly always asked with, with what the tables of
 * words say of them (the unit tests check it). A word that is not here goes
 * to the tables.
 */
static int common_unit(const char *w, int *code) {
  static const struct {
    const char *word;
    int code, found;
  } common[] = {
      {"year", DTK_YEAR, 1},
      {"month", DTK_MONTH, 1},
      {"day", DTK_DAY, 1},
      {"hour", DTK_HOUR, 1},
      {"minute", DTK_MINUTE, 1},
      {"second", DTK_SECOND, 1},
      {"quarter", DTK_QUARTER, 1},
      {"week", DTK_WEEK, 1},
      {"decade", DTK_DECADE, 1},
      {"century", DTK_CENTURY, 1},
      {"millennium", DTK_MILLENNIUM, 1},
      {"milliseconds", DTK_MILLISEC, 1},
      {"microseconds", DTK_MICROSEC, 1},
      {"epoch", DTK_EPOCH, 2},
      {"dow", DTK_DOW, 1},
      {"doy", DTK_DOY, 1},
      {"isodow", DTK_ISODOW, 1},
      {"isoyear", DTK_ISOYEAR, 1},
      {"julian", DTK_JULIAN, 1},
  };
  /* the first letter picks the few words to compare */
  switch (w[0]) {
  case 'y':
  case 'm':
  case 'd':
  case 'h':
  case 'q':
  case 'w':
  case 'c':
  case 'e':
  case 'i':
  case 'j':
  case 's':
    for (const auto &c : common) {
      if (c.word[0] == w[0] && strcmp(c.word, w) == 0) {
        *code = c.code;
        return c.found;
      }
    }
    break;
  default:
    break;
  }
  return 0;
}

/* The kind of a word for extract: see ty_datetime.h. */
int ty_datetime_part_unit(const char *lowtoken, int epoch_only, int *code) {
  int val = 0;
  int fast = common_unit(lowtoken, &val);

  if (fast != 0) {
    *code = val;
    return fast;
  }
  int type = DecodeUnits(0, lowtoken, &val);

  if (type == UNKNOWN_FIELD) {
    type = DecodeSpecial(0, lowtoken, &val);
  }
  *code = val;
  if (type == UNITS) {
    return 1;
  }
  if (type == RESERV && !(epoch_only && val != DTK_EPOCH)) {
    return 2;
  }
  return 0;
}

/* The slow path alone, for the tests to check the common words against. */
int ty_datetime_part_unit_slow(const char *lowtoken, int epoch_only,
                               int *code) {
  int val = 0;
  int type = DecodeUnits(0, lowtoken, &val);

  if (type == UNKNOWN_FIELD) {
    type = DecodeSpecial(0, lowtoken, &val);
  }
  *code = val;
  if (type == UNITS) {
    return 1;
  }
  if (type == RESERV && !(epoch_only && val != DTK_EPOCH)) {
    return 2;
  }
  return 0;
}

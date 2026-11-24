/*
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * Portions Copyright (c) 2026, CK Tan: the changes for ty,
 * under the MIT License (https://github.com/cktan/digres/blob/main/LICENSE)
 */

/*
 * dtparse.cc -- the text in of interval: the tokenizer, the decoder for
 * the postgres and sql forms, and the decoder for the ISO 8601 form.
 *
 * Derived from PostgreSQL's src/backend/utils/adt/datetime.c (ParseDateTime,
 * DecodeInterval, DecodeISO8601Interval and their helpers, and the tables of
 * words), with PostgreSQL's names of the types and the error codes. The
 * function ty_interval_parse at the end is ty's.
 */
#pragma GCC diagnostic ignored "-Wunused-parameter"

#include "ty_datetime.h"
#include "ty_timezone.h"

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
/* ASCII only, as PostgreSQL's pg_tolower is for the characters a word has */
static inline char pg_tolower(unsigned char c) {
  return (char)(c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c);
}
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

/*
 * The text in of date, time and timestamp: the decoders of PostgreSQL's
 * datetime.c (DecodeDateTime, DecodeTimeOnly and what they call), with the
 * zone, the abbreviation and the clock left to the host's hooks.
 */

/* PostgreSQL's pg_tm: the fields the decoders use. */
struct pg_tm {
  int tm_sec;
  int tm_min;
  int tm_hour;
  int tm_mday;
  int tm_mon;
  int tm_year;
  int tm_wday;
  int tm_yday;
  int tm_isdst;
};

#define MAX_TZDISP_HOUR 15
#define JULIAN_MINYEAR (-4713)
#define JULIAN_MINMONTH 11
#define JULIAN_MAXYEAR 5874898
#define JULIAN_MAXMONTH 6
#define IS_VALID_JULIAN(y, m, d)                                              \
  (((y) > JULIAN_MINYEAR || ((y) == JULIAN_MINYEAR && ((m) >= JULIAN_MINMONTH))) && \
   ((y) < JULIAN_MAXYEAR || ((y) == JULIAN_MAXYEAR && ((m) < JULIAN_MAXMONTH))))
#define DATEORDER_YMD TY_DATEORDER_YMD
#define DATEORDER_DMY TY_DATEORDER_DMY
#define DATEORDER_MDY TY_DATEORDER_MDY

static const int day_tab[2][13] = {
    {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31, 0},
    {31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31, 0}};
#define isleap(y) (((y) % 4) == 0 && (((y) % 100) != 0 || ((y) % 400) == 0))

/* The settings of the call in progress: the date order, and the host's hooks.
 */
thread_local int g_dateorder = 0;
thread_local const ty_dtzone_t *g_zone = nullptr;

/* The fields of a time in microseconds (dt2time of timestamp.c). */
static void dt2time(int64 jd, int *hour, int *min, int *sec, fsec_t *fsec) {
  int64 time = jd;

  *hour = time / USECS_PER_HOUR;
  time -= (*hour) * USECS_PER_HOUR;
  *min = time / USECS_PER_MINUTE;
  time -= (*min) * USECS_PER_MINUTE;
  *sec = time / USECS_PER_SEC;
  *fsec = time - (*sec * USECS_PER_SEC);
}

/*
 * Look a word up as a time zone abbreviation, by the host's hook. The hook
 * gives PostgreSQL's codes (TY_ABBREV_*), and sets all three results.
 */
static inline int DecodeTimezoneAbbrev(int field, const char *lowtoken,
                                       int *ftype, int *offset, const void **tz,
                                       ty_dtin_extra_t *extra) {
  if (g_zone != nullptr && g_zone->abbrev != nullptr) {
    return g_zone->abbrev(g_zone->ctx, field, lowtoken, ftype, offset, tz,
                          extra);
  }
  *ftype = UNKNOWN_FIELD;
  *offset = 0;
  *tz = NULL;
  return 0;
}

/* The host's zone of a name (pg_tzset), or NULL. */
static const void *dt_zone(const char *name) {
  if (g_zone == nullptr || g_zone->zone == nullptr) {
    return NULL;
  }
  return g_zone->zone(g_zone->ctx, name);
}

/* Is the zone a fixed offset? If so, its offset in seconds east of UTC. */
static bool dt_zone_fixed(const void *tz, long *gmtoff) {
  return g_zone != nullptr && g_zone->zone_fixed != nullptr &&
         g_zone->zone_fixed(g_zone->ctx, tz, gmtoff);
}

/* Keep the name of a zone the host did not know, for its message. */
static void dt_set_extra_timezone(ty_dtin_extra_t *extra, const char *name) {
  size_t n = strlen(name);

  if (n >= sizeof(extra->buf)) {
    n = sizeof(extra->buf) - 1;
  }
  memcpy(extra->buf, name, n);
  extra->buf[n] = '\0';
  extra->timezone = extra->buf;
}

/*
 * The transaction's time in the session's zone: false if the thread has no
 * transaction time or no session zone (ty_xact_now).
 */
static bool dt_now(struct pg_tm *tm, fsec_t *fsec, int *tzp) {
  ty_tm_t t;
  int32_t f;
  int tz;

  if (ty_xact_now(&t, &f, &tz) != 0) {
    return false;
  }
  tm->tm_year = t.year;
  tm->tm_mon = t.mon;
  tm->tm_mday = t.mday;
  tm->tm_hour = t.hour;
  tm->tm_min = t.min;
  tm->tm_sec = t.sec;
  *fsec = f;
  if (tzp != NULL) {
    *tzp = tz;
  }
  return true;
}

/* The date of the transaction's time in the session's zone
 * (GetCurrentDateTime). */
static bool dt_now_date(struct pg_tm *tm) {
  fsec_t fsec;

  return dt_now(tm, &fsec, NULL);
}

/* A pg_tm as the broken-down time the host's hooks take. */
static inline __attribute__((always_inline)) ty_tm_t
dt_ty_tm(const struct pg_tm *tm) {
  return ty_tm_t{tm->tm_year, tm->tm_mon, tm->tm_mday,
                 tm->tm_hour, tm->tm_min, tm->tm_sec};
}

/*
 * The offset of a zone (the session's if tz is NULL) at the time tm, in
 * seconds west of Greenwich, into *tzp; nothing if the host gives no such
 * hook, as for the types that have no zone to read it.
 */
static inline __attribute__((always_inline)) void
dt_offset(const struct pg_tm *tm, const void *tz, int *tzp) {
  if (g_zone != nullptr && g_zone->offset != nullptr) {
    ty_tm_t t = dt_ty_tm(tm);

    *tzp = g_zone->offset(g_zone->ctx, &t, tz);
  }
}

/* The same for a dynamic abbreviation of the zone tz. */
static inline __attribute__((always_inline)) void
dt_abbrev_offset(const struct pg_tm *tm, const char *abbrev, const void *tz,
                 int *tzp) {
  if (g_zone != nullptr && g_zone->abbrev_offset != nullptr) {
    ty_tm_t t = dt_ty_tm(tm);

    *tzp = g_zone->abbrev_offset(g_zone->ctx, &t, abbrev, tz);
  }
}

static int DecodeDate(char *str, int fmask, int *tmask, bool *is2digits,
                      struct pg_tm *tm);
int ValidateDate(int fmask, bool isjulian, bool is2digits, bool bc,
                 struct pg_tm *tm);
static int DecodeTime(char *str, int fmask, int range, int *tmask,
                      struct pg_tm *tm, fsec_t *fsec);
static int DecodeNumber(int flen, char *str, bool haveTextMonth, int fmask,
                        int *tmask, struct pg_tm *tm, fsec_t *fsec,
                        bool *is2digits);
static int DecodeNumberField(int len, char *str, int fmask, int *tmask,
                             struct pg_tm *tm, fsec_t *fsec, bool *is2digits);
int DecodeTimezone(const char *str, int *tzp);

/* DecodeDate()
 * Decode date string which includes delimiters.
 * Return 0 if okay, a DTERR code if not.
 *
 *	str: field to be parsed
 *	fmask: bitmask for field types already seen
 *	*tmask: receives bitmask for fields found here
 *	*is2digits: set to true if we find 2-digit year
 *	*tm: field values are stored into appropriate members of this struct
 */
static int DecodeDate(char *str, int fmask, int *tmask, bool *is2digits,
                      struct pg_tm *tm) {
  fsec_t fsec;
  int nf = 0;
  int i, len;
  int dterr;
  bool haveTextMonth = false;
  int type, val, dmask = 0;
  char *field[MAXDATEFIELDS];

  *tmask = 0;

  /* parse this string... */
  while (*str != '\0' && nf < MAXDATEFIELDS) {
    /* skip field separators */
    while (*str != '\0' && !isalnum((unsigned char)*str))
      str++;

    if (*str == '\0')
      return DTERR_BAD_FORMAT; /* end of string after separator */

    field[nf] = str;
    if (isdigit((unsigned char)*str)) {
      while (isdigit((unsigned char)*str))
        str++;
    } else if (isalpha((unsigned char)*str)) {
      while (isalpha((unsigned char)*str))
        str++;
    }

    /* Just get rid of any non-digit, non-alpha characters... */
    if (*str != '\0')
      *str++ = '\0';
    nf++;
  }

  /* look first for text fields, since that will be unambiguous month */
  for (i = 0; i < nf; i++) {
    if (isalpha((unsigned char)*field[i])) {
      type = DecodeSpecial(i, field[i], &val);
      if (type == IGNORE_DTF)
        continue;

      dmask = DTK_M(type);
      switch (type) {
      case MONTH:
        tm->tm_mon = val;
        haveTextMonth = true;
        break;

      default:
        return DTERR_BAD_FORMAT;
      }
      if (fmask & dmask)
        return DTERR_BAD_FORMAT;

      fmask |= dmask;
      *tmask |= dmask;

      /* mark this field as being completed */
      field[i] = NULL;
    }
  }

  /* now pick up remaining numeric fields */
  for (i = 0; i < nf; i++) {
    if (field[i] == NULL)
      continue;

    if ((len = strlen(field[i])) <= 0)
      return DTERR_BAD_FORMAT;

    dterr = DecodeNumber(len, field[i], haveTextMonth, fmask, &dmask, tm, &fsec,
                         is2digits);
    if (dterr)
      return dterr;

    if (fmask & dmask)
      return DTERR_BAD_FORMAT;

    fmask |= dmask;
    *tmask |= dmask;
  }

  if ((fmask & ~(DTK_M(DOY) | DTK_M(TZ))) != DTK_DATE_M)
    return DTERR_BAD_FORMAT;

  /* validation of the field values must wait until ValidateDate() */

  return 0;
}

/* ValidateDate()
 * Check valid year/month/day values, handle BC and DOY cases
 * Return 0 if okay, a DTERR code if not.
 */
int ValidateDate(int fmask, bool isjulian, bool is2digits, bool bc,
                 struct pg_tm *tm) {
  if (fmask & DTK_M(YEAR)) {
    if (isjulian) {
      /* tm_year is correct and should not be touched */
    } else if (bc) {
      /* there is no year zero in AD/BC notation */
      if (tm->tm_year <= 0)
        return DTERR_FIELD_OVERFLOW;
      /* internally, we represent 1 BC as year zero, 2 BC as -1, etc */
      tm->tm_year = -(tm->tm_year - 1);
    } else if (is2digits) {
      /* process 1 or 2-digit input as 1970-2069 AD, allow '0' and '00' */
      if (tm->tm_year < 0) /* just paranoia */
        return DTERR_FIELD_OVERFLOW;
      if (tm->tm_year < 70)
        tm->tm_year += 2000;
      else if (tm->tm_year < 100)
        tm->tm_year += 1900;
    } else {
      /* there is no year zero in AD/BC notation */
      if (tm->tm_year <= 0)
        return DTERR_FIELD_OVERFLOW;
    }
  }

  /* now that we have correct year, decode DOY */
  if (fmask & DTK_M(DOY)) {
    ty_j2date(ty_date2j(tm->tm_year, 1, 1) + tm->tm_yday - 1, &tm->tm_year,
              &tm->tm_mon, &tm->tm_mday);
  }

  /* check for valid month */
  if (fmask & DTK_M(MONTH)) {
    if (tm->tm_mon < 1 || tm->tm_mon > MONTHS_PER_YEAR)
      return DTERR_MD_FIELD_OVERFLOW;
  }

  /* minimal check for valid day */
  if (fmask & DTK_M(DAY)) {
    if (tm->tm_mday < 1 || tm->tm_mday > 31)
      return DTERR_MD_FIELD_OVERFLOW;
  }

  if ((fmask & DTK_DATE_M) == DTK_DATE_M) {
    /*
     * Check for valid day of month, now that we know for sure the month
     * and year.  Note we don't use MD_FIELD_OVERFLOW here, since it seems
     * unlikely that "Feb 29" is a YMD-order error.
     */
    if (tm->tm_mday > day_tab[isleap(tm->tm_year)][tm->tm_mon - 1])
      return DTERR_FIELD_OVERFLOW;
  }

  return 0;
}

/* DecodeTime()
 * Decode time string which includes delimiters.
 * Return 0 if okay, a DTERR code if not.
 *
 * This version is used for timestamps.  The results are returned into
 * the tm_hour/tm_min/tm_sec fields of *tm, and microseconds into *fsec.
 */
static int DecodeTime(char *str, int fmask, int range, int *tmask,
                      struct pg_tm *tm, fsec_t *fsec) {
  struct pg_itm itm;
  int dterr;

  dterr = DecodeTimeCommon(str, fmask, range, tmask, &itm);
  if (dterr)
    return dterr;

  if (itm.tm_hour > INT_MAX)
    return DTERR_FIELD_OVERFLOW;
  tm->tm_hour = (int)itm.tm_hour;
  tm->tm_min = itm.tm_min;
  tm->tm_sec = itm.tm_sec;
  *fsec = itm.tm_usec;

  return 0;
}

/* DecodeNumber()
 * Interpret plain numeric field as a date value in context.
 * Return 0 if okay, a DTERR code if not.
 */
static int DecodeNumber(int flen, char *str, bool haveTextMonth, int fmask,
                        int *tmask, struct pg_tm *tm, fsec_t *fsec,
                        bool *is2digits) {
  int val;
  char *cp;
  int dterr;

  *tmask = 0;

  errno = 0;
  val = strtoint(str, &cp, 10);
  if (errno == ERANGE)
    return DTERR_FIELD_OVERFLOW;
  if (cp == str)
    return DTERR_BAD_FORMAT;

  if (*cp == '.') {
    /*
     * More than two digits before decimal point? Then could be a date or
     * a run-together time: 2001.360 20011225 040506.789
     */
    if (cp - str > 2) {
      dterr = DecodeNumberField(flen, str, (fmask | DTK_DATE_M), tmask, tm,
                                fsec, is2digits);
      if (dterr < 0)
        return dterr;
      return 0;
    }

    dterr = ParseFractionalSecond(cp, fsec);
    if (dterr)
      return dterr;
  } else if (*cp != '\0')
    return DTERR_BAD_FORMAT;

  /* Special case for day of year */
  if (flen == 3 && (fmask & DTK_DATE_M) == DTK_M(YEAR) && val >= 1 &&
      val <= 366) {
    *tmask = (DTK_M(DOY) | DTK_M(MONTH) | DTK_M(DAY));
    tm->tm_yday = val;
    /* tm_mon and tm_mday can't actually be set yet ... */
    return 0;
  }

  /* Switch based on what we have so far */
  switch (fmask & DTK_DATE_M) {
  case 0:

    /*
     * Nothing so far; make a decision about what we think the input
     * is.  There used to be lots of heuristics here, but the
     * consensus now is to be paranoid.  It *must* be either
     * YYYY-MM-DD (with a more-than-two-digit year field), or the
     * field order defined by g_dateorder.
     */
    if (flen >= 3 || g_dateorder == DATEORDER_YMD) {
      *tmask = DTK_M(YEAR);
      tm->tm_year = val;
    } else if (g_dateorder == DATEORDER_DMY) {
      *tmask = DTK_M(DAY);
      tm->tm_mday = val;
    } else {
      *tmask = DTK_M(MONTH);
      tm->tm_mon = val;
    }
    break;

  case (DTK_M(YEAR)):
    /* Must be at second field of YY-MM-DD */
    *tmask = DTK_M(MONTH);
    tm->tm_mon = val;
    break;

  case (DTK_M(MONTH)):
    if (haveTextMonth) {
      /*
       * We are at the first numeric field of a date that included a
       * textual month name.  We want to support the variants
       * MON-DD-YYYY, DD-MON-YYYY, and YYYY-MON-DD as unambiguous
       * inputs.  We will also accept MON-DD-YY or DD-MON-YY in
       * either DMY or MDY modes, as well as YY-MON-DD in YMD mode.
       */
      if (flen >= 3 || g_dateorder == DATEORDER_YMD) {
        *tmask = DTK_M(YEAR);
        tm->tm_year = val;
      } else {
        *tmask = DTK_M(DAY);
        tm->tm_mday = val;
      }
    } else {
      /* Must be at second field of MM-DD-YY */
      *tmask = DTK_M(DAY);
      tm->tm_mday = val;
    }
    break;

  case (DTK_M(YEAR) | DTK_M(MONTH)):
    if (haveTextMonth) {
      /* Need to accept DD-MON-YYYY even in YMD mode */
      if (flen >= 3 && *is2digits) {
        /* Guess that first numeric field is day was wrong */
        *tmask = DTK_M(DAY); /* YEAR is already set */
        tm->tm_mday = tm->tm_year;
        tm->tm_year = val;
        *is2digits = false;
      } else {
        *tmask = DTK_M(DAY);
        tm->tm_mday = val;
      }
    } else {
      /* Must be at third field of YY-MM-DD */
      *tmask = DTK_M(DAY);
      tm->tm_mday = val;
    }
    break;

  case (DTK_M(DAY)):
    /* Must be at second field of DD-MM-YY */
    *tmask = DTK_M(MONTH);
    tm->tm_mon = val;
    break;

  case (DTK_M(MONTH) | DTK_M(DAY)):
    /* Must be at third field of DD-MM-YY or MM-DD-YY */
    *tmask = DTK_M(YEAR);
    tm->tm_year = val;
    break;

  case (DTK_M(YEAR) | DTK_M(MONTH) | DTK_M(DAY)):
    /* we have all the date, so it must be a time field */
    dterr = DecodeNumberField(flen, str, fmask, tmask, tm, fsec, is2digits);
    if (dterr < 0)
      return dterr;
    return 0;

  default:
    /* Anything else is bogus input */
    return DTERR_BAD_FORMAT;
  }

  /*
   * When processing a year field, mark it for adjustment if it's only one
   * or two digits.
   */
  if (*tmask == DTK_M(YEAR))
    *is2digits = (flen <= 2);

  return 0;
}

/* DecodeNumberField()
 * Interpret numeric string as a concatenated date or time field.
 * Return a DTK token (>= 0) if successful, a DTERR code (< 0) if not.
 *
 * Use the context of previously decoded fields to help with
 * the interpretation.
 */
static int DecodeNumberField(int len, char *str, int fmask, int *tmask,
                             struct pg_tm *tm, fsec_t *fsec, bool *is2digits) {
  char *cp;

  /*
   * This function was originally meant to cope only with DTK_NUMBER fields,
   * but we now sometimes abuse it to parse (parts of) DTK_DATE fields,
   * which can contain letters and other punctuation.  Reject if it's not a
   * valid DTK_NUMBER, that is digits and decimal point(s).  (ParseFraction
   * will reject if there's more than one decimal point.)
   */
  if (strspn(str, "0123456789.") != (size_t)len)
    return DTERR_BAD_FORMAT;

  /*
   * Have a decimal point? Then this is a date or something with a seconds
   * field...
   */
  if ((cp = strchr(str, '.')) != NULL) {
    int dterr;

    /* Convert the fraction and store at *fsec */
    dterr = ParseFractionalSecond(cp, fsec);
    if (dterr)
      return dterr;
    /* Now truncate off the fraction for further processing */
    *cp = '\0';
    len = strlen(str);
  }
  /* No decimal point and no complete date yet? */
  else if ((fmask & DTK_DATE_M) != DTK_DATE_M) {
    if (len >= 6) {
      *tmask = DTK_DATE_M;

      /*
       * Start from end and consider first 2 as Day, next 2 as Month,
       * and the rest as Year.
       */
      tm->tm_mday = atoi(str + (len - 2));
      *(str + (len - 2)) = '\0';
      tm->tm_mon = atoi(str + (len - 4));
      *(str + (len - 4)) = '\0';
      tm->tm_year = atoi(str);
      if ((len - 4) == 2)
        *is2digits = true;

      return DTK_DATE;
    }
  }

  /* not all time fields are specified? */
  if ((fmask & DTK_TIME_M) != DTK_TIME_M) {
    /* hhmmss */
    if (len == 6) {
      *tmask = DTK_TIME_M;
      tm->tm_sec = atoi(str + 4);
      *(str + 4) = '\0';
      tm->tm_min = atoi(str + 2);
      *(str + 2) = '\0';
      tm->tm_hour = atoi(str);

      return DTK_TIME;
    }
    /* hhmm? */
    else if (len == 4) {
      *tmask = DTK_TIME_M;
      tm->tm_sec = 0;
      tm->tm_min = atoi(str + 2);
      *(str + 2) = '\0';
      tm->tm_hour = atoi(str);

      return DTK_TIME;
    }
  }

  return DTERR_BAD_FORMAT;
}

/* DecodeTimezone()
 * Interpret string as a numeric timezone.
 *
 * Return 0 if okay (and set *tzp), a DTERR code if not okay.
 */
int DecodeTimezone(const char *str, int *tzp) {
  int tz;
  int hr, min, sec = 0;
  char *cp;

  /* leading character must be "+" or "-" */
  if (*str != '+' && *str != '-')
    return DTERR_BAD_FORMAT;

  errno = 0;
  hr = strtoint(str + 1, &cp, 10);
  if (errno == ERANGE)
    return DTERR_TZDISP_OVERFLOW;

  /* explicit delimiter? */
  if (*cp == ':') {
    errno = 0;
    min = strtoint(cp + 1, &cp, 10);
    if (errno == ERANGE)
      return DTERR_TZDISP_OVERFLOW;
    if (*cp == ':') {
      errno = 0;
      sec = strtoint(cp + 1, &cp, 10);
      if (errno == ERANGE)
        return DTERR_TZDISP_OVERFLOW;
    }
  }
  /* otherwise, might have run things together... */
  else if (*cp == '\0' && strlen(str) > 3) {
    min = hr % 100;
    hr = hr / 100;
    /* we could, but don't, support a run-together hhmmss format */
  } else
    min = 0;

  /* Range-check the values; see notes in datatype/timestamp.h */
  if (hr < 0 || hr > MAX_TZDISP_HOUR)
    return DTERR_TZDISP_OVERFLOW;
  if (min < 0 || min >= MINS_PER_HOUR)
    return DTERR_TZDISP_OVERFLOW;
  if (sec < 0 || sec >= SECS_PER_MINUTE)
    return DTERR_TZDISP_OVERFLOW;

  tz = (hr * MINS_PER_HOUR + min) * SECS_PER_MINUTE + sec;
  if (*str == '-')
    tz = -tz;

  *tzp = -tz;

  if (*cp != '\0')
    return DTERR_BAD_FORMAT;

  return 0;
}

/* DecodeDateTime()
 * Interpret previously parsed fields for general date and time.
 * Return 0 if full date, 1 if only time, and negative DTERR code if problems.
 * (Currently, all callers treat 1 as an error return too.)
 *
 * Inputs are field[] and ftype[] arrays, of length nf.
 * Other arguments are outputs.
 *
 *		External format(s):
 *				"<weekday> <month>-<day>-<year>
 *<hour>:<minute>:<second>" "Fri Feb-7-1997 15:23:27" "Feb-7-1997 15:23:27"
 *				"2-7-1997 15:23:27"
 *				"1997-2-7 15:23:27"
 *				"1997.038 15:23:27"		(day of year
 *1-366) Also supports input in compact time: "970207 152327" "97038 152327"
 *				"20011225T040506.789-07"
 *
 * Use the system-provided functions to get the current time zone
 * if not specified in the input string.
 *
 * If the date is outside the range of pg_time_t (in practice that could only
 * happen if pg_time_t is just 32 bits), then assume UTC time zone - thomas
 * 1997-05-27
 */
int DecodeDateTime(char **field, int *ftype, int nf, int *dtype,
                   struct pg_tm *tm, fsec_t *fsec, int *tzp,
                   ty_dtin_extra_t *extra) {
  int fmask = 0, tmask, type;
  int ptype = 0; /* "prefix type" for ISO and Julian formats */
  int i;
  int val;
  int dterr;
  int mer = HR24;
  bool haveTextMonth = false;
  bool isjulian = false;
  bool is2digits = false;
  bool bc = false;
  const void *namedTz = NULL;
  const void *abbrevTz = NULL;
  const char *abbrev = NULL;
  const void *valtz;
  struct pg_tm cur_tm;

  /*
   * We'll insist on at least all of the date fields, but initialize the
   * remaining fields in case they are not set later...
   */
  *dtype = DTK_DATE;
  tm->tm_hour = 0;
  tm->tm_min = 0;
  tm->tm_sec = 0;
  *fsec = 0;
  /* don't know daylight savings time status apriori */
  tm->tm_isdst = -1;
  if (tzp != NULL)
    *tzp = 0;

  for (i = 0; i < nf; i++) {
    switch (ftype[i]) {
    case DTK_DATE:

      /*
       * Integral julian day with attached time zone? All other
       * forms with JD will be separated into distinct fields, so we
       * handle just this case here.
       */
      if (ptype == DTK_JULIAN) {
        char *cp;
        int jday;

        if (tzp == NULL)
          return DTERR_BAD_FORMAT;

        errno = 0;
        jday = strtoint(field[i], &cp, 10);
        if (errno == ERANGE || jday < 0)
          return DTERR_FIELD_OVERFLOW;

        ty_j2date(jday, &tm->tm_year, &tm->tm_mon, &tm->tm_mday);
        isjulian = true;

        /* Get the time zone from the end of the string */
        dterr = DecodeTimezone(cp, tzp);
        if (dterr)
          return dterr;

        tmask = DTK_DATE_M | DTK_TIME_M | DTK_M(TZ);
        ptype = 0;
        break;
      }

      /*
       * Already have a date? Then this might be a time zone name
       * with embedded punctuation (e.g. "America/New_York") or a
       * run-together time with trailing time zone (e.g. hhmmss-zz).
       * - thomas 2001-12-25
       *
       * We consider it a time zone if we already have month & day.
       * This is to allow the form "mmm dd hhmmss tz year", which
       * we've historically accepted.
       */
      else if (ptype != 0 || ((fmask & (DTK_M(MONTH) | DTK_M(DAY))) ==
                              (DTK_M(MONTH) | DTK_M(DAY)))) {
        /* No time zone accepted? Then quit... */
        if (tzp == NULL)
          return DTERR_BAD_FORMAT;

        if (isdigit((unsigned char)*field[i]) || ptype != 0) {
          char *cp;

          /*
           * Allow a preceding "t" field, but no other units.
           */
          if (ptype != 0) {
            /* Sanity check; should not fail this test */
            if (ptype != DTK_TIME)
              return DTERR_BAD_FORMAT;
            ptype = 0;
          }

          /*
           * Starts with a digit but we already have a time
           * field? Then we are in trouble with a date and time
           * already...
           */
          if ((fmask & DTK_TIME_M) == DTK_TIME_M)
            return DTERR_BAD_FORMAT;

          if ((cp = strchr(field[i], '-')) == NULL)
            return DTERR_BAD_FORMAT;

          /* Get the time zone from the end of the string */
          dterr = DecodeTimezone(cp, tzp);
          if (dterr)
            return dterr;
          *cp = '\0';

          /*
           * Then read the rest of the field as a concatenated
           * time
           */
          dterr = DecodeNumberField(strlen(field[i]), field[i], fmask, &tmask,
                                    tm, fsec, &is2digits);
          if (dterr < 0)
            return dterr;

          /*
           * modify tmask after returning from
           * DecodeNumberField()
           */
          tmask |= DTK_M(TZ);
        } else {
          namedTz = dt_zone(field[i]);
          if (!namedTz) {
            dt_set_extra_timezone(extra, field[i]);
            return DTERR_BAD_TIMEZONE;
          }
          /* we'll apply the zone setting below */
          tmask = DTK_M(TZ);
        }
      } else {
        dterr = DecodeDate(field[i], fmask, &tmask, &is2digits, tm);
        if (dterr)
          return dterr;
      }
      break;

    case DTK_TIME:

      /*
       * This might be an ISO time following a "t" field.
       */
      if (ptype != 0) {
        /* Sanity check; should not fail this test */
        if (ptype != DTK_TIME)
          return DTERR_BAD_FORMAT;
        ptype = 0;
      }
      dterr =
          DecodeTime(field[i], fmask, INTERVAL_FULL_RANGE, &tmask, tm, fsec);
      if (dterr)
        return dterr;

      /* check for time overflow */
      if (ty_time_overflows(tm->tm_hour, tm->tm_min, tm->tm_sec, *fsec))
        return DTERR_FIELD_OVERFLOW;
      break;

    case DTK_TZ: {
      int tz;

      if (tzp == NULL)
        return DTERR_BAD_FORMAT;

      dterr = DecodeTimezone(field[i], &tz);
      if (dterr)
        return dterr;
      *tzp = tz;
      tmask = DTK_M(TZ);
    } break;

    case DTK_NUMBER:

      /*
       * Deal with cases where previous field labeled this one
       */
      if (ptype != 0) {
        char *cp;
        int value;

        errno = 0;
        value = strtoint(field[i], &cp, 10);
        if (errno == ERANGE)
          return DTERR_FIELD_OVERFLOW;
        if (*cp != '.' && *cp != '\0')
          return DTERR_BAD_FORMAT;

        switch (ptype) {
        case DTK_JULIAN:
          /* previous field was a label for "julian date" */
          if (value < 0)
            return DTERR_FIELD_OVERFLOW;
          tmask = DTK_DATE_M;
          ty_j2date(value, &tm->tm_year, &tm->tm_mon, &tm->tm_mday);
          isjulian = true;

          /* fractional Julian Day? */
          if (*cp == '.') {
            double time;

            dterr = ParseFraction(cp, &time);
            if (dterr)
              return dterr;
            time *= USECS_PER_DAY;
            dt2time(time, &tm->tm_hour, &tm->tm_min, &tm->tm_sec, fsec);
            tmask |= DTK_TIME_M;
          }
          break;

        case DTK_TIME:
          /* previous field was "t" for ISO time */
          dterr = DecodeNumberField(strlen(field[i]), field[i],
                                    (fmask | DTK_DATE_M), &tmask, tm, fsec,
                                    &is2digits);
          if (dterr < 0)
            return dterr;
          if (tmask != DTK_TIME_M)
            return DTERR_BAD_FORMAT;
          break;

        default:
          return DTERR_BAD_FORMAT;
          break;
        }

        ptype = 0;
        *dtype = DTK_DATE;
      } else {
        char *cp;
        int flen;

        flen = strlen(field[i]);
        cp = strchr(field[i], '.');

        /* Embedded decimal and no date yet? */
        if (cp != NULL && !(fmask & DTK_DATE_M)) {
          dterr = DecodeDate(field[i], fmask, &tmask, &is2digits, tm);
          if (dterr)
            return dterr;
        }
        /* embedded decimal and several digits before? */
        else if (cp != NULL && flen - strlen(cp) > 2) {
          /*
           * Interpret as a concatenated date or time Set the
           * type field to allow decoding other fields later.
           * Example: 20011223 or 040506
           */
          dterr = DecodeNumberField(flen, field[i], fmask, &tmask, tm, fsec,
                                    &is2digits);
          if (dterr < 0)
            return dterr;
        }

        /*
         * Is this a YMD or HMS specification, or a year number?
         * YMD and HMS are required to be six digits or more, so
         * if it is 5 digits, it is a year.  If it is six or more
         * digits, we assume it is YMD or HMS unless no date and
         * no time values have been specified.  This forces 6+
         * digit years to be at the end of the string, or to use
         * the ISO date specification.
         */
        else if (flen >= 6 &&
                 (!(fmask & DTK_DATE_M) || !(fmask & DTK_TIME_M))) {
          dterr = DecodeNumberField(flen, field[i], fmask, &tmask, tm, fsec,
                                    &is2digits);
          if (dterr < 0)
            return dterr;
        }
        /* otherwise it is a single date/time field... */
        else {
          dterr = DecodeNumber(flen, field[i], haveTextMonth, fmask, &tmask, tm,
                               fsec, &is2digits);
          if (dterr)
            return dterr;
        }
      }
      break;

    case DTK_STRING:
    case DTK_SPECIAL:
      /* timezone abbrevs take precedence over built-in tokens */
      dterr = DecodeTimezoneAbbrev(i, field[i], &type, &val, &valtz, extra);
      if (dterr)
        return dterr;
      if (type == UNKNOWN_FIELD)
        type = DecodeSpecial(i, field[i], &val);
      if (type == IGNORE_DTF)
        continue;

      tmask = DTK_M(type);
      switch (type) {
      case RESERV:
        switch (val) {
        case DTK_NOW:
          tmask = (DTK_DATE_M | DTK_TIME_M | DTK_M(TZ));
          *dtype = DTK_DATE;
          if (!dt_now(tm, fsec, tzp))
            return DTERR_BAD_FORMAT;
          break;

        case DTK_YESTERDAY:
          tmask = DTK_DATE_M;
          *dtype = DTK_DATE;
          if (!dt_now_date(&cur_tm))
            return DTERR_BAD_FORMAT;
          ty_j2date(ty_date2j(cur_tm.tm_year, cur_tm.tm_mon, cur_tm.tm_mday) -
                        1,
                    &tm->tm_year, &tm->tm_mon, &tm->tm_mday);
          break;

        case DTK_TODAY:
          tmask = DTK_DATE_M;
          *dtype = DTK_DATE;
          if (!dt_now_date(&cur_tm))
            return DTERR_BAD_FORMAT;
          tm->tm_year = cur_tm.tm_year;
          tm->tm_mon = cur_tm.tm_mon;
          tm->tm_mday = cur_tm.tm_mday;
          break;

        case DTK_TOMORROW:
          tmask = DTK_DATE_M;
          *dtype = DTK_DATE;
          if (!dt_now_date(&cur_tm))
            return DTERR_BAD_FORMAT;
          ty_j2date(ty_date2j(cur_tm.tm_year, cur_tm.tm_mon, cur_tm.tm_mday) +
                        1,
                    &tm->tm_year, &tm->tm_mon, &tm->tm_mday);
          break;

        case DTK_ZULU:
          tmask = (DTK_TIME_M | DTK_M(TZ));
          *dtype = DTK_DATE;
          tm->tm_hour = 0;
          tm->tm_min = 0;
          tm->tm_sec = 0;
          if (tzp != NULL)
            *tzp = 0;
          break;

        case DTK_EPOCH:
        case DTK_LATE:
        case DTK_EARLY:
          tmask = (DTK_DATE_M | DTK_TIME_M | DTK_M(TZ));
          *dtype = val;
          /* caller ignores tm for these dtype codes */
          break;

        default:
          return DTERR_BAD_FORMAT; /* not in the table of words */
        }

        break;

      case MONTH:

        /*
         * already have a (numeric) month? then see if we can
         * substitute...
         */
        if ((fmask & DTK_M(MONTH)) && !haveTextMonth && !(fmask & DTK_M(DAY)) &&
            tm->tm_mon >= 1 && tm->tm_mon <= 31) {
          tm->tm_mday = tm->tm_mon;
          tmask = DTK_M(DAY);
        }
        haveTextMonth = true;
        tm->tm_mon = val;
        break;

      case DTZMOD:

        /*
         * daylight savings time modifier (solves "MET DST"
         * syntax)
         */
        tmask |= DTK_M(DTZ);
        tm->tm_isdst = 1;
        if (tzp == NULL)
          return DTERR_BAD_FORMAT;
        *tzp -= val;
        break;

      case DTZ:

        /*
         * set mask for TZ here _or_ check for DTZ later when
         * getting default timezone
         */
        tmask |= DTK_M(TZ);
        tm->tm_isdst = 1;
        if (tzp == NULL)
          return DTERR_BAD_FORMAT;
        *tzp = -val;
        break;

      case TZ:
        tm->tm_isdst = 0;
        if (tzp == NULL)
          return DTERR_BAD_FORMAT;
        *tzp = -val;
        break;

      case DYNTZ:
        tmask |= DTK_M(TZ);
        if (tzp == NULL)
          return DTERR_BAD_FORMAT;
        /* we'll determine the actual offset later */
        abbrevTz = valtz;
        abbrev = field[i];
        break;

      case AMPM:
        mer = val;
        break;

      case ADBC:
        bc = (val == BC);
        break;

      case DOW:
        tm->tm_wday = val;
        break;

      case UNITS:
        tmask = 0;
        /* reject consecutive unhandled units */
        if (ptype != 0)
          return DTERR_BAD_FORMAT;
        ptype = val;
        break;

      case ISOTIME:

        /*
         * This is a filler field "t" indicating that the next
         * field is time. Try to verify that this is sensible.
         */
        tmask = 0;

        /* No preceding date? Then quit... */
        if ((fmask & DTK_DATE_M) != DTK_DATE_M)
          return DTERR_BAD_FORMAT;

        /* reject consecutive unhandled units */
        if (ptype != 0)
          return DTERR_BAD_FORMAT;
        ptype = val;
        break;

      case UNKNOWN_FIELD:

        /*
         * Before giving up and declaring error, check to see
         * if it is an all-alpha timezone name.
         */
        namedTz = dt_zone(field[i]);
        if (!namedTz)
          return DTERR_BAD_FORMAT;
        /* we'll apply the zone setting below */
        tmask = DTK_M(TZ);
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
  } /* end loop over fields */

  /* reject if prefix type appeared and was never handled */
  if (ptype != 0)
    return DTERR_BAD_FORMAT;

  /* do additional checking for normal date specs (but not "infinity" etc) */
  if (*dtype == DTK_DATE) {
    /* do final checking/adjustment of Y/M/D fields */
    dterr = ValidateDate(fmask, isjulian, is2digits, bc, tm);
    if (dterr)
      return dterr;

    /* handle AM/PM */
    if (mer != HR24 && tm->tm_hour > HOURS_PER_DAY / 2)
      return DTERR_FIELD_OVERFLOW;
    if (mer == AM && tm->tm_hour == HOURS_PER_DAY / 2)
      tm->tm_hour = 0;
    else if (mer == PM && tm->tm_hour != HOURS_PER_DAY / 2)
      tm->tm_hour += HOURS_PER_DAY / 2;

    /* check for incomplete input */
    if ((fmask & DTK_DATE_M) != DTK_DATE_M) {
      if ((fmask & DTK_TIME_M) == DTK_TIME_M)
        return 1;
      return DTERR_BAD_FORMAT;
    }

    /*
     * If we had a full timezone spec, compute the offset (we could not do
     * it before, because we need the date to resolve DST status).
     */
    if (namedTz != NULL) {
      /* daylight savings time modifier disallowed with full TZ */
      if (fmask & DTK_M(DTZMOD))
        return DTERR_BAD_FORMAT;

      dt_offset(tm, namedTz, tzp);
    }

    /*
     * Likewise, if we had a dynamic timezone abbreviation, resolve it
     * now.
     */
    if (abbrevTz != NULL) {
      /* daylight savings time modifier disallowed with dynamic TZ */
      if (fmask & DTK_M(DTZMOD))
        return DTERR_BAD_FORMAT;

      dt_abbrev_offset(tm, abbrev, abbrevTz, tzp);
    }

    /* timezone not specified? then use session timezone */
    if (tzp != NULL && !(fmask & DTK_M(TZ))) {
      /*
       * daylight savings time modifier but no standard timezone? then
       * error
       */
      if (fmask & DTK_M(DTZMOD))
        return DTERR_BAD_FORMAT;

      dt_offset(tm, NULL, tzp);
    }
  }

  return 0;
}

/* DecodeTimeOnly()
 * Interpret parsed string as time fields only.
 * Returns 0 if successful, DTERR code if bogus input detected.
 *
 * Inputs are field[] and ftype[] arrays, of length nf.
 * Other arguments are outputs.
 *
 * Note that support for time zone is here for
 * SQL TIME WITH TIME ZONE, but it reveals
 * bogosity with SQL date/time standards, since
 * we must infer a time zone from current time.
 * - thomas 2000-03-10
 * Allow specifying date to get a better time zone,
 * if time zones are allowed. - thomas 2001-12-26
 */
int DecodeTimeOnly(char **field, int *ftype, int nf, int *dtype,
                   struct pg_tm *tm, fsec_t *fsec, int *tzp,
                   ty_dtin_extra_t *extra) {
  int fmask = 0, tmask, type;
  int ptype = 0; /* "prefix type" for ISO and Julian formats */
  int i;
  int val;
  int dterr;
  bool isjulian = false;
  bool is2digits = false;
  bool bc = false;
  int mer = HR24;
  const void *namedTz = NULL;
  const void *abbrevTz = NULL;
  const char *abbrev = NULL;
  const void *valtz;

  *dtype = DTK_TIME;
  tm->tm_hour = 0;
  tm->tm_min = 0;
  tm->tm_sec = 0;
  *fsec = 0;
  /* don't know daylight savings time status apriori */
  tm->tm_isdst = -1;

  if (tzp != NULL)
    *tzp = 0;

  for (i = 0; i < nf; i++) {
    switch (ftype[i]) {
    case DTK_DATE:

      /*
       * Time zone not allowed? Then should not accept dates or time
       * zones no matter what else!
       */
      if (tzp == NULL)
        return DTERR_BAD_FORMAT;

      /* Under limited circumstances, we will accept a date... */
      if (i == 0 && nf >= 2 &&
          (ftype[nf - 1] == DTK_DATE || ftype[1] == DTK_TIME)) {
        dterr = DecodeDate(field[i], fmask, &tmask, &is2digits, tm);
        if (dterr)
          return dterr;
      }
      /* otherwise, this is a time and/or time zone */
      else {
        if (isdigit((unsigned char)*field[i])) {
          char *cp;

          /*
           * Starts with a digit but we already have a time
           * field? Then we are in trouble with time already...
           */
          if ((fmask & DTK_TIME_M) == DTK_TIME_M)
            return DTERR_BAD_FORMAT;

          /*
           * Should not get here and fail. Sanity check only...
           */
          if ((cp = strchr(field[i], '-')) == NULL)
            return DTERR_BAD_FORMAT;

          /* Get the time zone from the end of the string */
          dterr = DecodeTimezone(cp, tzp);
          if (dterr)
            return dterr;
          *cp = '\0';

          /*
           * Then read the rest of the field as a concatenated
           * time
           */
          dterr = DecodeNumberField(strlen(field[i]), field[i],
                                    (fmask | DTK_DATE_M), &tmask, tm, fsec,
                                    &is2digits);
          if (dterr < 0)
            return dterr;
          ftype[i] = dterr;

          tmask |= DTK_M(TZ);
        } else {
          namedTz = dt_zone(field[i]);
          if (!namedTz) {
            dt_set_extra_timezone(extra, field[i]);
            return DTERR_BAD_TIMEZONE;
          }
          /* we'll apply the zone setting below */
          ftype[i] = DTK_TZ;
          tmask = DTK_M(TZ);
        }
      }
      break;

    case DTK_TIME:

      /*
       * This might be an ISO time following a "t" field.
       */
      if (ptype != 0) {
        if (ptype != DTK_TIME)
          return DTERR_BAD_FORMAT;
        ptype = 0;
      }

      dterr = DecodeTime(field[i], (fmask | DTK_DATE_M), INTERVAL_FULL_RANGE,
                         &tmask, tm, fsec);
      if (dterr)
        return dterr;
      break;

    case DTK_TZ: {
      int tz;

      if (tzp == NULL)
        return DTERR_BAD_FORMAT;

      dterr = DecodeTimezone(field[i], &tz);
      if (dterr)
        return dterr;
      *tzp = tz;
      tmask = DTK_M(TZ);
    } break;

    case DTK_NUMBER:

      /*
       * Deal with cases where previous field labeled this one
       */
      if (ptype != 0) {
        char *cp;
        int value;

        errno = 0;
        value = strtoint(field[i], &cp, 10);
        if (errno == ERANGE)
          return DTERR_FIELD_OVERFLOW;
        if (*cp != '.' && *cp != '\0')
          return DTERR_BAD_FORMAT;

        switch (ptype) {
        case DTK_JULIAN:
          /* previous field was a label for "julian date" */
          if (tzp == NULL)
            return DTERR_BAD_FORMAT;
          if (value < 0)
            return DTERR_FIELD_OVERFLOW;
          tmask = DTK_DATE_M;
          ty_j2date(value, &tm->tm_year, &tm->tm_mon, &tm->tm_mday);
          isjulian = true;

          if (*cp == '.') {
            double time;

            dterr = ParseFraction(cp, &time);
            if (dterr)
              return dterr;
            time *= USECS_PER_DAY;
            dt2time(time, &tm->tm_hour, &tm->tm_min, &tm->tm_sec, fsec);
            tmask |= DTK_TIME_M;
          }
          break;

        case DTK_TIME:
          /* previous field was "t" for ISO time */
          dterr = DecodeNumberField(strlen(field[i]), field[i],
                                    (fmask | DTK_DATE_M), &tmask, tm, fsec,
                                    &is2digits);
          if (dterr < 0)
            return dterr;
          ftype[i] = dterr;

          if (tmask != DTK_TIME_M)
            return DTERR_BAD_FORMAT;
          break;

        default:
          return DTERR_BAD_FORMAT;
          break;
        }

        ptype = 0;
        *dtype = DTK_DATE;
      } else {
        char *cp;
        int flen;

        flen = strlen(field[i]);
        cp = strchr(field[i], '.');

        /* Embedded decimal? */
        if (cp != NULL) {
          /*
           * Under limited circumstances, we will accept a
           * date...
           */
          if (i == 0 && nf >= 2 && ftype[nf - 1] == DTK_DATE) {
            dterr = DecodeDate(field[i], fmask, &tmask, &is2digits, tm);
            if (dterr)
              return dterr;
          }
          /* embedded decimal and several digits before? */
          else if (flen - strlen(cp) > 2) {
            /*
             * Interpret as a concatenated date or time Set
             * the type field to allow decoding other fields
             * later. Example: 20011223 or 040506
             */
            dterr = DecodeNumberField(flen, field[i], (fmask | DTK_DATE_M),
                                      &tmask, tm, fsec, &is2digits);
            if (dterr < 0)
              return dterr;
            ftype[i] = dterr;
          } else
            return DTERR_BAD_FORMAT;
        } else if (flen > 4) {
          dterr = DecodeNumberField(flen, field[i], (fmask | DTK_DATE_M),
                                    &tmask, tm, fsec, &is2digits);
          if (dterr < 0)
            return dterr;
          ftype[i] = dterr;
        }
        /* otherwise it is a single date/time field... */
        else {
          dterr = DecodeNumber(flen, field[i], false, (fmask | DTK_DATE_M),
                               &tmask, tm, fsec, &is2digits);
          if (dterr)
            return dterr;
        }
      }
      break;

    case DTK_STRING:
    case DTK_SPECIAL:
      /* timezone abbrevs take precedence over built-in tokens */
      dterr = DecodeTimezoneAbbrev(i, field[i], &type, &val, &valtz, extra);
      if (dterr)
        return dterr;
      if (type == UNKNOWN_FIELD)
        type = DecodeSpecial(i, field[i], &val);
      if (type == IGNORE_DTF)
        continue;

      tmask = DTK_M(type);
      switch (type) {
      case RESERV:
        switch (val) {
        case DTK_NOW:
          tmask = DTK_TIME_M;
          *dtype = DTK_TIME;
          if (!dt_now(tm, fsec, NULL))
            return DTERR_BAD_FORMAT;
          break;

        case DTK_ZULU:
          tmask = (DTK_TIME_M | DTK_M(TZ));
          *dtype = DTK_TIME;
          tm->tm_hour = 0;
          tm->tm_min = 0;
          tm->tm_sec = 0;
          tm->tm_isdst = 0;
          break;

        default:
          return DTERR_BAD_FORMAT;
        }

        break;

      case DTZMOD:

        /*
         * daylight savings time modifier (solves "MET DST"
         * syntax)
         */
        tmask |= DTK_M(DTZ);
        tm->tm_isdst = 1;
        if (tzp == NULL)
          return DTERR_BAD_FORMAT;
        *tzp -= val;
        break;

      case DTZ:

        /*
         * set mask for TZ here _or_ check for DTZ later when
         * getting default timezone
         */
        tmask |= DTK_M(TZ);
        tm->tm_isdst = 1;
        if (tzp == NULL)
          return DTERR_BAD_FORMAT;
        *tzp = -val;
        ftype[i] = DTK_TZ;
        break;

      case TZ:
        tm->tm_isdst = 0;
        if (tzp == NULL)
          return DTERR_BAD_FORMAT;
        *tzp = -val;
        ftype[i] = DTK_TZ;
        break;

      case DYNTZ:
        tmask |= DTK_M(TZ);
        if (tzp == NULL)
          return DTERR_BAD_FORMAT;
        /* we'll determine the actual offset later */
        abbrevTz = valtz;
        abbrev = field[i];
        ftype[i] = DTK_TZ;
        break;

      case AMPM:
        mer = val;
        break;

      case ADBC:
        bc = (val == BC);
        break;

      case UNITS:
        tmask = 0;
        /* reject consecutive unhandled units */
        if (ptype != 0)
          return DTERR_BAD_FORMAT;
        ptype = val;
        break;

      case ISOTIME:
        tmask = 0;
        /* reject consecutive unhandled units */
        if (ptype != 0)
          return DTERR_BAD_FORMAT;
        ptype = val;
        break;

      case UNKNOWN_FIELD:

        /*
         * Before giving up and declaring error, check to see
         * if it is an all-alpha timezone name.
         */
        namedTz = dt_zone(field[i]);
        if (!namedTz)
          return DTERR_BAD_FORMAT;
        /* we'll apply the zone setting below */
        tmask = DTK_M(TZ);
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
  } /* end loop over fields */

  /* reject if prefix type appeared and was never handled */
  if (ptype != 0)
    return DTERR_BAD_FORMAT;

  /* do final checking/adjustment of Y/M/D fields */
  dterr = ValidateDate(fmask, isjulian, is2digits, bc, tm);
  if (dterr)
    return dterr;

  /* handle AM/PM */
  if (mer != HR24 && tm->tm_hour > HOURS_PER_DAY / 2)
    return DTERR_FIELD_OVERFLOW;
  if (mer == AM && tm->tm_hour == HOURS_PER_DAY / 2)
    tm->tm_hour = 0;
  else if (mer == PM && tm->tm_hour != HOURS_PER_DAY / 2)
    tm->tm_hour += HOURS_PER_DAY / 2;

  /* check for time overflow */
  if (ty_time_overflows(tm->tm_hour, tm->tm_min, tm->tm_sec, *fsec))
    return DTERR_FIELD_OVERFLOW;

  if ((fmask & DTK_TIME_M) != DTK_TIME_M)
    return DTERR_BAD_FORMAT;

  /*
   * If we had a full timezone spec, compute the offset (we could not do it
   * before, because we may need the date to resolve DST status).
   */
  if (namedTz != NULL) {
    long int gmtoff;

    /* daylight savings time modifier disallowed with full TZ */
    if (fmask & DTK_M(DTZMOD))
      return DTERR_BAD_FORMAT;

    /* if non-DST zone, we do not need to know the date */
    if (dt_zone_fixed(namedTz, &gmtoff)) {
      *tzp = -(int)gmtoff;
    } else {
      /* a date has to be specified */
      if ((fmask & DTK_DATE_M) != DTK_DATE_M)
        return DTERR_BAD_FORMAT;
      dt_offset(tm, namedTz, tzp);
    }
  }

  /*
   * Likewise, if we had a dynamic timezone abbreviation, resolve it now.
   */
  if (abbrevTz != NULL) {
    /*
     * daylight savings time modifier but no standard timezone? then error
     */
    if (fmask & DTK_M(DTZMOD))
      return DTERR_BAD_FORMAT;

    struct pg_tm tmp = {};

    /* a date, if there is any, has to be whole; none: today's (mday 0) */
    if ((fmask & DTK_DATE_M) == 0) {
      tmp.tm_mday = 0;
    } else {
      if ((fmask & DTK_DATE_M) != DTK_DATE_M)
        return DTERR_BAD_FORMAT;
      tmp.tm_year = tm->tm_year;
      tmp.tm_mon = tm->tm_mon;
      tmp.tm_mday = tm->tm_mday;
    }
    tmp.tm_hour = tm->tm_hour;
    tmp.tm_min = tm->tm_min;
    tmp.tm_sec = tm->tm_sec;
    dt_abbrev_offset(&tmp, abbrev, abbrevTz, tzp);
  }

  /* timezone not specified? then use session timezone */
  if (tzp != NULL && !(fmask & DTK_M(TZ))) {
    /*
     * daylight savings time modifier but no standard timezone? then error
     */
    if (fmask & DTK_M(DTZMOD))
      return DTERR_BAD_FORMAT;

    struct pg_tm tmp = {};

    /* a date, if there is any, has to be whole; none: today's (mday 0) */
    if ((fmask & DTK_DATE_M) == 0) {
      tmp.tm_mday = 0;
    } else {
      if ((fmask & DTK_DATE_M) != DTK_DATE_M)
        return DTERR_BAD_FORMAT;
      tmp.tm_year = tm->tm_year;
      tmp.tm_mon = tm->tm_mon;
      tmp.tm_mday = tm->tm_mday;
    }
    tmp.tm_hour = tm->tm_hour;
    tmp.tm_min = tm->tm_min;
    tmp.tm_sec = tm->tm_sec;
    dt_offset(&tmp, NULL, tzp);
  }

  return 0;
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
      {"century", DTK_CENTURY, 1},       /* 0 */
      {"day", DTK_DAY, 1},               /* 1 */
      {"dow", DTK_DOW, 1},               /* 2 */
      {"doy", DTK_DOY, 1},               /* 3 */
      {"decade", DTK_DECADE, 1},         /* 4 */
      {"epoch", DTK_EPOCH, 2},           /* 5 */
      {"hour", DTK_HOUR, 1},             /* 6 */
      {"isodow", DTK_ISODOW, 1},         /* 7 */
      {"isoyear", DTK_ISOYEAR, 1},       /* 8 */
      {"julian", DTK_JULIAN, 1},         /* 9 */
      {"month", DTK_MONTH, 1},           /* 10 */
      {"minute", DTK_MINUTE, 1},         /* 11 */
      {"milliseconds", DTK_MILLISEC, 1}, /* 12 */
      {"microseconds", DTK_MICROSEC, 1}, /* 13 */
      {"millennium", DTK_MILLENNIUM, 1}, /* 14 */
      {"quarter", DTK_QUARTER, 1},       /* 15 */
      {"second", DTK_SECOND, 1},         /* 16 */
      {"week", DTK_WEEK, 1},             /* 17 */
      {"year", DTK_YEAR, 1},             /* 18 */
  };
  /* the words of each letter, from a: the first, and how many */
  static const signed char first[26] = {0,  0, 0, 1,  5,  0, 0,  6,  7,
                                        9,  0, 0, 10, 0,  0, 0,  15, 0,
                                        16, 0, 0, 0,  17, 0, 18, 0};
  static const signed char count[26] = {0, 0, 1, 4, 1, 0, 0, 1, 2, 1, 0, 0, 5,
                                        0, 0, 0, 1, 0, 1, 0, 0, 0, 1, 0, 1, 0};
  unsigned letter = (unsigned char)w[0] - 'a';

  if (letter < 26) {
    for (int i = first[letter], n = i + count[letter]; i < n; i++) {
      if (strcmp(common[i].word, w) == 0) {
        *code = common[i].code;
        return common[i].found;
      }
    }
  }
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

/*
 * Cut a text into fields, and decode them as a date, a time of day or a date
 * and time (what date_in, time_in and timestamp_in do first). The fields are
 * cut into a work buffer of buflen bytes, which sets the longest text.
 * 1. Set the date order and the hooks of the call.
 * 2. Cut the text into fields.
 * 3. Decode the fields: a time of day only if timeonly.
 * 4. A result that is a time of day only is a bad format.
 */
static int dt_decode(const char *str, int timeonly, size_t buflen,
                     int dateorder, const ty_dtzone_t *zone, int *dtype,
                     struct pg_tm *tm, fsec_t *fsec, ty_dtin_extra_t *extra,
                     int *tzp) {
  char workbuf[MAXDATELEN + MAXDATEFIELDS];
  char *field[MAXDATEFIELDS];
  int ftype[MAXDATEFIELDS];
  int nf;

  g_dateorder = dateorder;
  g_zone = zone;
  extra->timezone = nullptr;
  extra->abbrev = nullptr;
  int d = ParseDateTime(str, workbuf, buflen, field, ftype, MAXDATEFIELDS, &nf);

  if (d == 0) {
    d = timeonly
            ? DecodeTimeOnly(field, ftype, nf, dtype, tm, fsec, tzp, extra)
            : DecodeDateTime(field, ftype, nf, dtype, tm, fsec, tzp, extra);
  }
  return d;
}

/* The error of a text that does not read: TY_ERR_INVALID, with the code. */
static int dt_bad(int d, int *dterr) {
  *dterr = d > 0 ? DTERR_BAD_FORMAT : d;
  return ty_internal::set_error(TY_ERR_INVALID, "invalid date/time text");
}

/* date_in: see ty_datetime.h. */
int ty_date_parse(const char *str, int dateorder, const ty_dtzone_t *zone,
                  int32_t *out, int *dterr, ty_dtin_extra_t *extra) {
  struct pg_tm tt, *tm = &tt;
  fsec_t fsec;
  int dtype, tz0 = 0;
  int d = dt_decode(str, 0, MAXDATELEN + 1, dateorder, zone, &dtype, tm, &fsec,
                    extra, &tz0);

  *dterr = 0;
  if (d != 0) {
    return dt_bad(d, dterr);
  }
  switch (dtype) {
  case DTK_DATE:
    break;
  case DTK_EPOCH:
    tm->tm_year = 1970;
    tm->tm_mon = 1;
    tm->tm_mday = 1;
    break;
  case DTK_LATE:
    *out = TY_DATE_NOEND;
    return 0;
  case DTK_EARLY:
    *out = TY_DATE_NOBEGIN;
    return 0;
  default:
    return dt_bad(DTERR_BAD_FORMAT, dterr);
  }
  /* Prevent overflow in Julian-day routines */
  if (!IS_VALID_JULIAN(tm->tm_year, tm->tm_mon, tm->tm_mday)) {
    return ty_internal::set_error(TY_ERR_RANGE, "date out of range");
  }
  int32_t date =
      ty_date2j(tm->tm_year, tm->tm_mon, tm->tm_mday) - TY_POSTGRES_EPOCH_JDATE;

  /* Now check for just-out-of-range dates */
  if (!ty_date_valid(date)) {
    return ty_internal::set_error(TY_ERR_RANGE, "date out of range");
  }
  *out = date;
  return 0;
}

/* time_in, up to the typmod: see ty_datetime.h. */
int ty_time_parse(const char *str, int dateorder, const ty_dtzone_t *zone,
                  int64_t *out, int *dterr, ty_dtin_extra_t *extra) {
  struct pg_tm tt, *tm = &tt;
  fsec_t fsec;
  int dtype, tz0 = 0;
  int d = dt_decode(str, 1, MAXDATELEN + 1, dateorder, zone, &dtype, tm, &fsec,
                    extra, &tz0);

  *dterr = 0;
  if (d != 0) {
    return dt_bad(d, dterr);
  }
  *out =
      ((((int64_t)tm->tm_hour * MINS_PER_HOUR + tm->tm_min) * SECS_PER_MINUTE) +
       tm->tm_sec) *
          USECS_PER_SEC +
      fsec;
  return 0;
}

/* timestamp_in, up to the typmod: see ty_datetime.h. */
int ty_timestamp_parse(const char *str, int dateorder, const ty_dtzone_t *zone,
                       int64_t *out, int *dterr, ty_dtin_extra_t *extra) {
  struct pg_tm tt, *tm = &tt;
  fsec_t fsec;
  int dtype, tz0 = 0;
  int d = dt_decode(str, 0, MAXDATELEN + MAXDATEFIELDS, dateorder, zone, &dtype,
                    tm, &fsec, extra, &tz0);

  *dterr = 0;
  if (d != 0) {
    return dt_bad(d, dterr);
  }
  switch (dtype) {
  case DTK_DATE: {
    ty_tm_t t = {tm->tm_year, tm->tm_mon, tm->tm_mday,
                 tm->tm_hour, tm->tm_min, tm->tm_sec};

    if (ty_tm_to_timestamp(&t, fsec, out) != 0) {
      return ty_internal::set_error(TY_ERR_RANGE, "timestamp out of range");
    }
    return 0;
  }
  case DTK_EPOCH:
    /* 1970-01-01 00:00:00, as microseconds since 2000-01-01 */
    *out =
        (int64_t)(-(TY_POSTGRES_EPOCH_JDATE - 2440588)) * 86400 * USECS_PER_SEC;
    return 0;
  case DTK_LATE:
    *out = TY_TIMESTAMP_NOEND;
    return 0;
  case DTK_EARLY:
    *out = TY_TIMESTAMP_NOBEGIN;
    return 0;
  default:
    return dt_bad(DTERR_BAD_FORMAT, dterr);
  }
}

/*
 * timestamptz_in, up to the typmod: see ty_datetime.h.
 * 1. Decode the text into fields, and the zone's offset.
 * 2. Make the instant: the local time plus the offset, in range.
 */
int ty_timestamptz_parse(const char *str, int dateorder,
                         const ty_dtzone_t *zone, int64_t *out, int *dterr,
                         ty_dtin_extra_t *extra) {
  struct pg_tm tt, *tm = &tt;
  fsec_t fsec;
  int dtype, tz = 0;
  int d = dt_decode(str, 0, MAXDATELEN + MAXDATEFIELDS, dateorder, zone, &dtype,
                    tm, &fsec, extra, &tz);

  *dterr = 0;
  if (d != 0) {
    return dt_bad(d, dterr);
  }
  switch (dtype) {
  case DTK_DATE: {
    /* tm2timestamp with the zone: the local time, then the offset */
    if (!IS_VALID_JULIAN(tm->tm_year, tm->tm_mon, tm->tm_mday)) {
      return ty_internal::set_error(TY_ERR_RANGE, "timestamp out of range");
    }
    int64_t date = ty_date2j(tm->tm_year, tm->tm_mon, tm->tm_mday) -
                   TY_POSTGRES_EPOCH_JDATE;
    int64_t time =
        (((int64_t)tm->tm_hour * MINS_PER_HOUR + tm->tm_min) * SECS_PER_MINUTE +
         tm->tm_sec) *
            USECS_PER_SEC +
        fsec;
    int64_t r;

    if (__builtin_mul_overflow(date, USECS_PER_DAY, &r) ||
        __builtin_add_overflow(r, time, &r)) {
      return ty_internal::set_error(TY_ERR_RANGE, "timestamp out of range");
    }
    r += (int64_t)tz * USECS_PER_SEC;
    if (!ty_timestamp_valid(r)) {
      return ty_internal::set_error(TY_ERR_RANGE, "timestamp out of range");
    }
    *out = r;
    return 0;
  }
  case DTK_EPOCH:
    *out =
        (int64_t)(-(TY_POSTGRES_EPOCH_JDATE - 2440588)) * 86400 * USECS_PER_SEC;
    return 0;
  case DTK_LATE:
    *out = TY_TIMESTAMP_NOEND;
    return 0;
  case DTK_EARLY:
    *out = TY_TIMESTAMP_NOBEGIN;
    return 0;
  default:
    return dt_bad(DTERR_BAD_FORMAT, dterr);
  }
}

/* timetz_in, up to the typmod: see ty_datetime.h. */
int ty_timetz_parse(const char *str, int dateorder, const ty_dtzone_t *zone,
                    ty_timetz_t *out, int *dterr, ty_dtin_extra_t *extra) {
  struct pg_tm tt, *tm = &tt;
  fsec_t fsec;
  int dtype, tz = 0;
  int d = dt_decode(str, 1, MAXDATELEN + 1, dateorder, zone, &dtype, tm, &fsec,
                    extra, &tz);

  *dterr = 0;
  if (d != 0) {
    return dt_bad(d, dterr);
  }
  out->time =
      ((((int64_t)tm->tm_hour * MINS_PER_HOUR + tm->tm_min) * SECS_PER_MINUTE) +
       tm->tm_sec) *
          USECS_PER_SEC +
      fsec;
  out->zone = tz;
  return 0;
}

/* The offset of a zone written as a number: PostgreSQL's DecodeTimezone. */
int ty_zone_offset_parse(const char *str, int *tz) {
  return DecodeTimezone(str, tz);
}

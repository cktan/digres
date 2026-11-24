/*-------------------------------------------------------------------------
 *
 * datetime.c
 *	  Support functions for date/time types.
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 * Portions Copyright (c) 2026, CK Tan: the changes for digres,
 * under the MIT License (https://github.com/cktan/digres/blob/main/LICENSE)
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/datetime.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <ctype.h>
#include <limits.h>
#include <math.h>

#include "access/htup_details.h"
#include "access/xact.h"
#include "common/int.h"
#include "common/string.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "nodes/nodeFuncs.h"
#include "parser/scansup.h"
#include "utils/builtins.h"
#include "utils/date.h"
#include "utils/datetime.h"
#include "ty_datetime.h"
#include "utils/guc.h"
#include "utils/tzparser.h"
#include "utils/control_block.h"

static const datetkn *datebsearch(const char *key, const datetkn *base,
                                  int nel);
static pg_tz *FetchDynamicTimeZone(TimeZoneAbbrevTable *tbl, const datetkn *tp,
                                   DateTimeErrorExtra *extra);

const int day_tab[2][13] = {
    {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31, 0},
    {31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31, 0}};

const char *const months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul",
                              "Aug", "Sep", "Oct", "Nov", "Dec", NULL};

const char *const days[] = {"Sunday",   "Monday", "Tuesday",  "Wednesday",
                            "Thursday", "Friday", "Saturday", NULL};

/*****************************************************************************
 *	 PRIVATE ROUTINES
 **
 *****************************************************************************/

/*
 * datetktbl holds date/time keywords.
 *
 * Note that this table must be strictly alphabetically ordered to allow an
 * O(ln(N)) search algorithm to be used.
 *
 * The token field must be NUL-terminated; we truncate entries to TOKMAXLEN
 * characters to fit.
 *
 * The static table contains no TZ, DTZ, or DYNTZ entries; rather those
 * are loaded from configuration files and stored in zoneabbrevtbl, whose
 * abbrevs[] field has the same format as the static datetktbl.
 */
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

static const int szdatetktbl = sizeof datetktbl / sizeof datetktbl[0];

/*
 * deltatktbl: same format as datetktbl, but holds keywords used to represent
 * time units (eg, for intervals, and for EXTRACT).
 */
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

static const int szdeltatktbl = sizeof deltatktbl / sizeof deltatktbl[0];

static thread_local /*LOCAL*/ TimeZoneAbbrevTable *zoneabbrevtbl = NULL;

/* Caches of recent lookup results in the above tables */

static thread_local /*LOCAL*/ const datetkn *datecache[MAXDATEFIELDS] = {NULL};

static thread_local /*LOCAL*/ const datetkn *deltacache[MAXDATEFIELDS] = {NULL};

/* Cache for results of timezone abbreviation lookups */

typedef struct TzAbbrevCache {
  char abbrev[TOKMAXLEN + 1]; /* always NUL-terminated */
  char ftype;                 /* TZ, DTZ, or DYNTZ */
  int offset;                 /* GMT offset, if fixed-offset */
  pg_tz *tz;                  /* relevant zone, if variable-offset */
} TzAbbrevCache;

static thread_local /*LOCAL*/ TzAbbrevCache tzabbrevcache[MAXDATEFIELDS];

/*
 * Calendar time to Julian date conversions.
 * Julian date is commonly used in astronomical applications,
 *	since it is numerically accurate and computationally simple.
 * The algorithms here will accurately convert between Julian day
 *	and calendar date for all non-negative Julian days
 *	(i.e. from Nov 24, -4713 on).
 *
 * Rewritten to eliminate overflow problems. This now allows the
 * routines to work correctly for all Julian day counts from
 * 0 to 2147483647	(Nov 24, -4713 to Jun 3, 5874898) assuming
 * a 32-bit integer. Longer types should also work to the limits
 * of their precision.
 *
 * Actually, date2j() will work sanely, in the sense of producing
 * valid negative Julian dates, significantly before Nov 24, -4713.
 * We rely on it to do so back to Nov 1, -4713; see IS_VALID_JULIAN()
 * and associated commentary in timestamp.h.
 */

int date2j(int year, int month, int day) {
  return ty_date2j(year, month, day);
}

void j2date(int jd, int *year, int *month, int *day) {
  ty_j2date(jd, year, month, day);
}

/*
 * GetCurrentDateTime()
 *
 * Get the transaction start time ("now()") broken down as a struct pg_tm,
 * converted according to the session timezone setting.
 *
 * This is just a convenience wrapper for GetCurrentTimeUsec, to cover the
 * case where caller doesn't need either fractional seconds or tz offset.
 */
void GetCurrentDateTime(struct pg_tm *tm) {
  fsec_t fsec;

  GetCurrentTimeUsec(tm, &fsec, NULL);
}

/*
 * GetCurrentTimeUsec()
 *
 * Get the transaction start time ("now()") broken down as a struct pg_tm,
 * including fractional seconds and timezone offset.  The time is converted
 * according to the session timezone setting.
 *
 * Callers may pass tzp = NULL if they don't need the offset, but this does
 * not affect the conversion behavior (unlike timestamp2tm()).
 *
 * Internally, we cache the result, since this could be called many times
 * in a transaction, within which now() doesn't change.
 */
void GetCurrentTimeUsec(struct pg_tm *tm, fsec_t *fsec, int *tzp) {
  TimestampTz cur_ts = GetCurrentTransactionStartTimestamp();

  /*
   * The cache key must include both current time and current timezone.  By
   * representing the timezone by just a pointer, we're assuming that
   * distinct timezone settings could never have the same pointer value.
   * This is true by virtue of the hashtable used inside pg_tzset();
   * however, it might need another look if we ever allow entries in that
   * hash to be recycled.
   */
  static thread_local /*LOCAL*/ TimestampTz cache_ts = 0;
  static thread_local /*LOCAL*/ pg_tz *cache_timezone = NULL;
  static thread_local /*LOCAL*/ struct pg_tm cache_tm;
  static thread_local /*LOCAL*/ fsec_t cache_fsec;
  static thread_local /*LOCAL*/ int cache_tz;

  if (cur_ts != cache_ts || session_timezone != cache_timezone) {
    /*
     * Make sure cache is marked invalid in case of error after partial
     * update within timestamp2tm.
     */
    cache_timezone = NULL;

    /*
     * Perform the computation, storing results into cache.  We do not
     * really expect any error here, since current time surely ought to be
     * within range, but check just for sanity's sake.
     */
    if (timestamp2tm(cur_ts, &cache_tz, &cache_tm, &cache_fsec, NULL,
                     session_timezone) != 0)
      ereport(ERROR, (errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
                      errmsg("timestamp out of range")));

    /* OK, so mark the cache valid. */
    cache_ts = cur_ts;
    cache_timezone = session_timezone;
  }

  *tm = cache_tm;
  *fsec = cache_fsec;
  if (tzp != NULL)
    *tzp = cache_tz;
}

/*
 * Append seconds and fractional seconds (if any) at *cp.
 *
 * precision is the max number of fraction digits, fillzeros says to
 * pad to two integral-seconds digits.
 *
 * Returns a pointer to the new end of string.  No NUL terminator is put
 * there; callers are responsible for NUL terminating str themselves.
 *
 * Note that any sign is stripped from the input sec and fsec values.
 */

/*
 * Variant of above that's specialized to timestamp case.
 *
 * Returns a pointer to the new end of string.  No NUL terminator is put
 * there; callers are responsible for NUL terminating str themselves.
 */

/*
 * The fields of a pg_tm that a zone looks at: its local time. (The rules of
 * the zones are ty's: ty_timezone.h.)
 */
static inline ty_tm_t tz_local_of(const struct pg_tm *tm) {
  return ty_tm_t{tm->tm_year, tm->tm_mon, tm->tm_mday,
                 tm->tm_hour, tm->tm_min, tm->tm_sec};
}

/* DetermineTimeZoneOffset()
 *
 * Given a struct pg_tm in which tm_year, tm_mon, tm_mday, tm_hour, tm_min,
 * and tm_sec fields are set, and a zic-style time zone definition, determine
 * the applicable GMT offset and daylight-savings status at that time.
 * Set the struct pg_tm's tm_isdst field accordingly, and return the GMT
 * offset as the function result.
 *
 * Note: if the date is out of the range we can deal with, we return zero
 * as the GMT offset and set tm_isdst = 0.  We don't throw an error here,
 * though probably some higher-level code will.
 */
int DetermineTimeZoneOffset(struct pg_tm *tm, pg_tz *tzp) {
  ty_tm_t t = tz_local_of(tm);
  int isdst;
  int offset = ty_tz_offset_for_local(tzp, &t, &isdst, NULL);

  tm->tm_isdst = isdst;
  return offset;
}

/* DetermineTimeZoneAbbrevOffset()
 *
 * Determine the GMT offset and DST flag to be attributed to a dynamic
 * time zone abbreviation, that is one whose meaning has changed over time.
 * *tm contains the local time at which the meaning should be determined,
 * and tm->tm_isdst receives the DST flag.
 *
 * This differs from the behavior of DetermineTimeZoneOffset() in that a
 * standard-time or daylight-time abbreviation forces use of the corresponding
 * GMT offset even when the zone was then in DS or standard time respectively.
 * (However, that happens only if we can match the given abbreviation to some
 * abbreviation that appears in the IANA timezone data.  Otherwise, we fall
 * back to doing DetermineTimeZoneOffset().)
 */
int DetermineTimeZoneAbbrevOffset(struct pg_tm *tm, const char *abbr,
                                  pg_tz *tzp) {
  ty_tm_t t = tz_local_of(tm);
  int isdst;
  int offset = ty_tz_abbrev_offset(tzp, &t, abbr, &isdst);

  tm->tm_isdst = isdst;
  return offset;
}

/* DetermineTimeZoneAbbrevOffsetTS()
 *
 * As above but the probe time is specified as a TimestampTz (hence, UTC time),
 * and DST status is returned into *isdst rather than into tm->tm_isdst.
 */
int DetermineTimeZoneAbbrevOffsetTS(TimestampTz ts, const char *abbr,
                                    pg_tz *tzp, int *isdst) {
  pg_time_t t = timestamptz_to_time_t(ts);
  int zone_offset;
  int abbr_offset;
  int tz;
  struct pg_tm tm;
  fsec_t fsec;

  /*
   * If the abbrev matches anything in the zone data, this is pretty easy.
   */
  if (ty_tz_abbrev_at(tzp, t, abbr, &abbr_offset, isdst))
    return abbr_offset;

  /*
   * Else, break down the timestamp so we can use DetermineTimeZoneOffset.
   */
  if (timestamp2tm(ts, &tz, &tm, &fsec, NULL, tzp) != 0)
    ereport(ERROR, (errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
                    errmsg("timestamp out of range")));

  zone_offset = DetermineTimeZoneOffset(&tm, tzp);
  *isdst = tm.tm_isdst;
  return zone_offset;
}

/* TimeZoneAbbrevIsKnown()
 *
 * Detect whether the given string is a time zone abbreviation that's known
 * in the specified TZDB timezone, and if so whether it's fixed or varying
 * meaning.  The match is not case-sensitive.
 */
static bool TimeZoneAbbrevIsKnown(const char *abbr, pg_tz *tzp, bool *isfixed,
                                  int *offset, int *isdst) {
  int fixed;

  if (!ty_tz_abbrev_is_known(tzp, abbr, &fixed, offset, isdst))
    return false;
  *isfixed = fixed != 0;
  return true;
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

/* DecodeTimezoneAbbrev()
 * Interpret string as a timezone abbreviation, if possible.
 *
 * Sets *ftype to an abbreviation type (TZ, DTZ, or DYNTZ), or UNKNOWN_FIELD if
 * string is not any known abbreviation.  On success, set *offset and *tz to
 * represent the UTC offset (for TZ or DTZ) or underlying zone (for DYNTZ).
 * Note that full timezone names (such as America/New_York) are not handled
 * here, mostly for historical reasons.
 *
 * The function result is 0 or a DTERR code; in the latter case, *extra
 * is filled as needed.  Note that unknown-abbreviation is not considered
 * an error case.  Also note that many callers assume that the DTERR code
 * is one that DateTimeParseError does not require "str" or "datatype"
 * strings for.
 *
 * Given string must be lowercased already.
 *
 * Implement a cache lookup since it is likely that dates
 *	will be related in format.
 */
int DecodeTimezoneAbbrev(int field, const char *lowtoken, int *ftype,
                         int *offset, pg_tz **tz, DateTimeErrorExtra *extra) {
  TzAbbrevCache *tzc = &tzabbrevcache[field];
  bool isfixed;
  int isdst;
  const datetkn *tp;

  /*
   * Do we have a cached result?  Use strncmp so that we match truncated
   * names, although we shouldn't really see that happen with normal
   * abbreviations.
   */
  if (strncmp(lowtoken, tzc->abbrev, TOKMAXLEN) == 0) {
    *ftype = tzc->ftype;
    *offset = tzc->offset;
    *tz = tzc->tz;
    return 0;
  }

  /*
   * See if the current session_timezone recognizes it.  Checking this
   * before zoneabbrevtbl allows us to correctly handle abbreviations whose
   * meaning varies across zones, such as "LMT".
   */
  if (session_timezone && TimeZoneAbbrevIsKnown(lowtoken, session_timezone,
                                                &isfixed, offset, &isdst)) {
    *ftype = (isfixed ? (isdst ? DTZ : TZ) : DYNTZ);
    *tz = (isfixed ? NULL : session_timezone);
    /* flip sign to agree with the convention used in zoneabbrevtbl */
    *offset = -(*offset);
    /* cache result; use strlcpy to truncate name if necessary */
    strlcpy(tzc->abbrev, lowtoken, TOKMAXLEN + 1);
    tzc->ftype = *ftype;
    tzc->offset = *offset;
    tzc->tz = *tz;
    return 0;
  }

  /* Nope, so look in zoneabbrevtbl */
  if (zoneabbrevtbl)
    tp = datebsearch(lowtoken, zoneabbrevtbl->abbrevs,
                     zoneabbrevtbl->numabbrevs);
  else
    tp = NULL;
  if (tp == NULL) {
    *ftype = UNKNOWN_FIELD;
    *offset = 0;
    *tz = NULL;
    /* failure results are not cached */
  } else {
    *ftype = tp->type;
    if (tp->type == DYNTZ) {
      *offset = 0;
      *tz = FetchDynamicTimeZone(zoneabbrevtbl, tp, extra);
      if (*tz == NULL)
        return DTERR_BAD_ZONE_ABBREV;
    } else {
      *offset = tp->value;
      *tz = NULL;
    }

    /* cache result; use strlcpy to truncate name if necessary */
    strlcpy(tzc->abbrev, lowtoken, TOKMAXLEN + 1);
    tzc->ftype = *ftype;
    tzc->offset = *offset;
    tzc->tz = *tz;
  }

  return 0;
}

/*
 * Reset tzabbrevcache after a change in session_timezone.
 */
void ClearTimeZoneAbbrevCache(void) {
  memset(tzabbrevcache, 0, sizeof(tzabbrevcache));
}

/*
 * ty's hook for a word that may be a time zone abbreviation. The codes TZ,
 * DTZ, DYNTZ and UNKNOWN_FIELD are ty's TY_ABBREV_*, and ty's extra starts
 * as DateTimeErrorExtra does, so the lookup is called as it is.
 */
static int ty_hook_abbrev(void *ctx, int field, const char *lowtoken, int *type,
                          int *offset, const void **tz,
                          ty_dtin_extra_t *extra) {
  return DecodeTimezoneAbbrev(field, lowtoken, type, offset, (pg_tz **)tz,
                              (DateTimeErrorExtra *)extra);
}

StaticAssertDecl(TZ == TY_ABBREV_TZ && DTZ == TY_ABBREV_DTZ &&
                     DYNTZ == TY_ABBREV_DYNTZ &&
                     UNKNOWN_FIELD == TY_ABBREV_NONE,
                 "ty's codes of an abbreviation are PostgreSQL's");
StaticAssertDecl(offsetof(ty_dtin_extra_t, timezone) ==
                         offsetof(DateTimeErrorExtra, dtee_timezone) &&
                     offsetof(ty_dtin_extra_t, abbrev) ==
                         offsetof(DateTimeErrorExtra, dtee_abbrev),
                 "ty's extra starts as DateTimeErrorExtra");

/* ty's hook for the name of a time zone. */
static const void *ty_hook_zone(void *ctx, const char *name) {
  return pg_tzset(name);
}

/* ty's hook for whether a zone has one offset at all times. */
static int ty_hook_zone_fixed(void *ctx, const void *tz, long *gmtoff) {
  return pg_get_timezone_offset((const pg_tz *)tz, gmtoff);
}

const ty_dtzone_t ty_dtzone_hooks = {NULL, ty_hook_abbrev, ty_hook_zone,
                                     ty_hook_zone_fixed};

/*
 * A pg_tm of the broken-down time ty gives a hook: only what the zone code
 * reads. No date (mday 0) is today's date in the session's zone.
 */
static inline void ty_hook_tm(const ty_tm_t *t, struct pg_tm *tm) {
  if (t->mday == 0) {
    GetCurrentDateTime(tm);
  } else {
    tm->tm_year = t->year;
    tm->tm_mon = t->mon;
    tm->tm_mday = t->mday;
  }
  tm->tm_hour = t->hour;
  tm->tm_min = t->min;
  tm->tm_sec = t->sec;
}

/* ty's hook for the offset of a zone, the session's if tz is NULL. */
static int ty_hook_offset(void *ctx, const ty_tm_t *t, const void *tz) {
  struct pg_tm tm;

  ty_hook_tm(t, &tm);
  return DetermineTimeZoneOffset(&tm, tz ? (pg_tz *)tz : session_timezone);
}

/* ty's hook for the offset of a dynamic abbreviation. */
static int ty_hook_abbrev_offset(void *ctx, const ty_tm_t *t,
                                 const char *abbrev, const void *tz) {
  struct pg_tm tm;

  ty_hook_tm(t, &tm);
  return DetermineTimeZoneAbbrevOffset(&tm, abbrev, (pg_tz *)tz);
}

const ty_dtzone_t ty_dtzone_hooks_tz = {
    NULL,          ty_hook_abbrev, ty_hook_zone, ty_hook_zone_fixed,
    ty_hook_offset, ty_hook_abbrev_offset};

/* Report a code of ty's text in: the code is PostgreSQL's DTERR. */
void TyDateTimeParseError(int dterr, const ty_dtin_extra_t *x, const char *str,
                          const char *datatype, Node *escontext) {
  DateTimeErrorExtra extra = {x->timezone, x->abbrev};

  DateTimeParseError(dterr, &extra, str, datatype, escontext);
}

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

/* DecodeTimezoneName()
 * Interpret string as a timezone abbreviation or name.
 * Throw error if the name is not recognized.
 *
 * The return value indicates what kind of zone identifier it is:
 *	TZNAME_FIXED_OFFSET: fixed offset from UTC
 *	TZNAME_DYNTZ: dynamic timezone abbreviation
 *	TZNAME_ZONE: full tzdb zone name
 *
 * For TZNAME_FIXED_OFFSET, *offset receives the UTC offset (in seconds,
 * with ISO sign convention: positive is east of Greenwich).
 * For the other two cases, *tz receives the timezone struct representing
 * the zone name or the abbreviation's underlying zone.
 */
int DecodeTimezoneName(const char *tzname, int *offset, pg_tz **tz) {
  char *lowzone;
  int dterr, type;
  DateTimeErrorExtra extra;

  /*
   * First we look in the timezone abbreviation table (to handle cases like
   * "EST"), and if that fails, we look in the timezone database (to handle
   * cases like "America/New_York").  This matches the order in which
   * timestamp input checks the cases; it's important because the timezone
   * database unwisely uses a few zone names that are identical to offset
   * abbreviations.
   */

  /* DecodeTimezoneAbbrev requires lowercase input */
  lowzone = downcase_truncate_identifier(tzname, strlen(tzname), false);

  dterr = DecodeTimezoneAbbrev(0, lowzone, &type, offset, tz, &extra);
  if (dterr)
    DateTimeParseError(dterr, &extra, NULL, NULL, NULL);

  if (type == TZ || type == DTZ) {
    /* fixed-offset abbreviation, return the offset */
    return TZNAME_FIXED_OFFSET;
  } else if (type == DYNTZ) {
    /* dynamic-offset abbreviation, return its referenced timezone */
    return TZNAME_DYNTZ;
  } else {
    /* try it as a full zone name */
    *tz = pg_tzset(tzname);
    if (*tz == NULL)
      ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                      errmsg("time zone \"%s\" not recognized", tzname)));
    return TZNAME_ZONE;
  }
}

/* DecodeTimezoneNameToTz()
 * Interpret string as a timezone abbreviation or name.
 * Throw error if the name is not recognized.
 *
 * This is a simple wrapper for DecodeTimezoneName that produces a pg_tz *
 * result in all cases.
 */
pg_tz *DecodeTimezoneNameToTz(const char *tzname) {
  pg_tz *result;
  int offset;

  if (DecodeTimezoneName(tzname, &offset, &result) == TZNAME_FIXED_OFFSET) {
    /* fixed-offset abbreviation, get a pg_tz descriptor for that */
    result = pg_tzset_offset(-offset); /* flip to POSIX sign convention */
  }
  return result;
}

/* DecodeTimezoneAbbrevPrefix()
 * Interpret prefix of string as a timezone abbreviation, if possible.
 *
 * This has roughly the same functionality as DecodeTimezoneAbbrev(),
 * but the API is adapted to the needs of formatting.c.  Notably,
 * we will match the longest possible prefix of the given string
 * rather than insisting on a complete match, and downcasing is applied
 * here rather than in the caller.
 *
 * Returns the length of the timezone abbreviation, or -1 if not recognized.
 * On success, sets *offset to the GMT offset for the abbreviation if it
 * is a fixed-offset abbreviation, or sets *tz to the pg_tz struct for
 * a dynamic abbreviation.
 */
int DecodeTimezoneAbbrevPrefix(const char *str, int *offset, pg_tz **tz) {
  char lowtoken[TOKMAXLEN + 1];
  int len;

  *offset = 0; /* avoid uninitialized vars on failure */
  *tz = NULL;

  /* Downcase as much of the string as we could need */
  for (len = 0; len < TOKMAXLEN; len++) {
    if (*str == '\0' || !isalpha((unsigned char)*str))
      break;
    lowtoken[len] = pg_tolower((unsigned char)*str++);
  }
  lowtoken[len] = '\0';

  /*
   * We could avoid doing repeated binary searches if we cared to duplicate
   * datebsearch here, but it's not clear that such an optimization would be
   * worth the trouble.  In common cases there's probably not anything after
   * the zone abbrev anyway.  So just search with successively truncated
   * strings.
   */
  while (len > 0) {
    bool isfixed;
    int isdst;
    const datetkn *tp;

    /* See if the current session_timezone recognizes it. */
    if (session_timezone && TimeZoneAbbrevIsKnown(lowtoken, session_timezone,
                                                  &isfixed, offset, &isdst)) {
      if (isfixed) {
        /* flip sign to agree with the convention in zoneabbrevtbl */
        *offset = -(*offset);
      } else {
        /* Caller must resolve the abbrev's current meaning */
        *tz = session_timezone;
      }
      return len;
    }

    /* Known in zoneabbrevtbl? */
    if (zoneabbrevtbl)
      tp = datebsearch(lowtoken, zoneabbrevtbl->abbrevs,
                       zoneabbrevtbl->numabbrevs);
    else
      tp = NULL;
    if (tp != NULL) {
      if (tp->type == DYNTZ) {
        DateTimeErrorExtra extra;
        pg_tz *tzp = FetchDynamicTimeZone(zoneabbrevtbl, tp, &extra);

        if (tzp != NULL) {
          /* Caller must resolve the abbrev's current meaning */
          *tz = tzp;
          return len;
        }
      } else {
        /* Fixed-offset zone abbrev, so it's easy */
        *offset = tp->value;
        return len;
      }
    }

    /* Nope, try the next shorter string. */
    lowtoken[--len] = '\0';
  }

  /* Did not find a match */
  return -1;
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

/*
 * Report an error detected by one of the datetime input processing routines.
 *
 * dterr is the error code, and *extra contains any auxiliary info we need
 * for the error report.  extra can be NULL if not needed for the particular
 * dterr value.
 *
 * str is the original input string, and datatype is the name of the datatype
 * we were trying to accept.  (For some DTERR codes, these are not used and
 * can be NULL.)
 *
 * If escontext points to an ErrorSaveContext node, that is filled instead
 * of throwing an error.
 *
 * Note: it might seem useless to distinguish DTERR_INTERVAL_OVERFLOW and
 * DTERR_TZDISP_OVERFLOW from DTERR_FIELD_OVERFLOW, but SQL99 mandates three
 * separate SQLSTATE codes, so ...
 */
void DateTimeParseError(int dterr, DateTimeErrorExtra *extra, const char *str,
                        const char *datatype, Node *escontext) {
  switch (dterr) {
  case DTERR_FIELD_OVERFLOW:
    errsave(escontext,
            (errcode(ERRCODE_DATETIME_FIELD_OVERFLOW),
             errmsg("date/time field value out of range: \"%s\"", str)));
    break;
  case DTERR_MD_FIELD_OVERFLOW:
    /* <nanny>same as above, but add hint about DateStyle</nanny> */
    errsave(escontext,
            (errcode(ERRCODE_DATETIME_FIELD_OVERFLOW),
             errmsg("date/time field value out of range: \"%s\"", str),
             errhint("Perhaps you need a different \"DateStyle\" setting.")));
    break;
  case DTERR_INTERVAL_OVERFLOW:
    errsave(escontext,
            (errcode(ERRCODE_INTERVAL_FIELD_OVERFLOW),
             errmsg("interval field value out of range: \"%s\"", str)));
    break;
  case DTERR_TZDISP_OVERFLOW:
    errsave(escontext,
            (errcode(ERRCODE_INVALID_TIME_ZONE_DISPLACEMENT_VALUE),
             errmsg("time zone displacement out of range: \"%s\"", str)));
    break;
  case DTERR_BAD_TIMEZONE:
    errsave(escontext,
            (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
             errmsg("time zone \"%s\" not recognized", extra->dtee_timezone)));
    break;
  case DTERR_BAD_ZONE_ABBREV:
    errsave(escontext,
            (errcode(ERRCODE_CONFIG_FILE_ERROR),
             errmsg("time zone \"%s\" not recognized", extra->dtee_timezone),
             errdetail("This time zone name appears in the configuration file "
                       "for time zone abbreviation \"%s\".",
                       extra->dtee_abbrev)));
    break;
  case DTERR_BAD_FORMAT:
  default:
    errsave(escontext, (errcode(ERRCODE_INVALID_DATETIME_FORMAT),
                        errmsg("invalid input syntax for type %s: \"%s\"",
                               datatype, str)));
    break;
  }
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

/* EncodeTimezone()
 *		Copies representation of a numeric timezone offset to str.
 *
 * Returns a pointer to the new end of string.  No NUL terminator is put
 * there; callers are responsible for NUL terminating str themselves.
 */

/* EncodeDateOnly()
 * Encode date as local time.
 */
void EncodeDateOnly(struct pg_tm *tm, int style, char *str) {
  ty_tm_t t = {tm->tm_year, tm->tm_mon, tm->tm_mday, 0, 0, 0};

  /* ty writes the date, by the style and the date order */
  ty_date_format(&t, style, DateOrder, str);
}

/* EncodeTimeOnly()
 * Encode time fields only.
 *
 * tm and fsec are the value to encode, print_tz determines whether to include
 * a time zone (the difference between time and timetz types), tz is the
 * numeric time zone offset, style is the date style, str is where to write the
 * output.
 */
void EncodeTimeOnly(struct pg_tm *tm, fsec_t fsec, bool print_tz, int tz,
                    int style, char *str) {
  ty_tm_t t = {tm->tm_year, tm->tm_mon, tm->tm_mday,
               tm->tm_hour, tm->tm_min, tm->tm_sec};

  ty_time_format(&t, fsec, print_tz, tz, style, str);
}

/* EncodeDateTime()
 * Encode date and time interpreted as local time.
 *
 * tm and fsec are the value to encode, print_tz determines whether to include
 * a time zone (the difference between timestamp and timestamptz types), tz is
 * the numeric time zone offset, tzn is the textual time zone, which if
 * specified will be used instead of tz by some styles, style is the date
 * style, str is where to write the output.
 *
 * Supported date styles:
 *	Postgres - day mon hh:mm:ss yyyy tz
 *	SQL - mm/dd/yyyy hh:mm:ss.ss tz
 *	ISO - yyyy-mm-dd hh:mm:ss+/-tz
 *	German - dd.mm.yyyy hh:mm:ss tz
 *	XSD - yyyy-mm-ddThh:mm:ss.ss+/-tz
 */
void EncodeDateTime(struct pg_tm *tm, fsec_t fsec, bool print_tz, int tz,
                    const char *tzn, int style, char *str) {
  ty_tm_t t = {tm->tm_year, tm->tm_mon, tm->tm_mday,
               tm->tm_hour, tm->tm_min, tm->tm_sec};

  ty_timestamp_format(&t, fsec, print_tz, tz, tzn, tm->tm_isdst, style,
                      DateOrder, str);
}

/*
 * Helper functions to avoid duplicated code in EncodeInterval.
 */

/* Append an ISO-8601-style interval field, but only if value isn't zero */

/* Append a postgres-style interval field, but only if value isn't zero */

/* Append a verbose-style interval field, but only if value isn't zero */

/* EncodeInterval()
 * Interpret time structure as a delta time and convert to string.
 *
 * Support "traditional Postgres" and ISO-8601 styles.
 * Actually, afaik ISO does not address time interval formatting,
 *	but this looks similar to the spec for absolute date/time.
 * - thomas 1998-04-30
 *
 * Actually, afaik, ISO 8601 does specify formats for "time
 * intervals...[of the]...format with time-unit designators", which
 * are pretty ugly.  The format looks something like
 *	   P1Y1M1DT1H1M1.12345S
 * but useful for exchanging data with computers instead of humans.
 * - ron 2003-07-14
 *
 * And ISO's SQL 2008 standard specifies standards for
 * "year-month literal"s (that look like '2-3') and
 * "day-time literal"s (that look like ('4 5:6:7')
 */
void EncodeInterval(struct pg_itm *itm, int style, char *str) {
  ty_itm_t t = {itm->tm_hour, itm->tm_mday, itm->tm_mon, itm->tm_year,
                itm->tm_min,  itm->tm_sec,  itm->tm_usec};

  ty_interval_format(&t, style, str);
}

/*
 * We've been burnt by stupid errors in the ordering of the datetkn tables
 * once too often.  Arrange to check them during postmaster start.
 */
static bool CheckDateTokenTable(const char *tablename, const datetkn *base,
                                int nel) {
  bool ok = true;
  int i;

  for (i = 0; i < nel; i++) {
    /* check for token strings that don't fit */
    if (strlen(base[i].token) > TOKMAXLEN) {
      /* %.*s is safe since all our tokens are ASCII */
      elog(LOG, "token too long in %s table: \"%.*s\"", tablename,
           TOKMAXLEN + 1, base[i].token);
      ok = false;
      break; /* don't risk applying strcmp */
    }
    /* check for out of order */
    if (i > 0 && strcmp(base[i - 1].token, base[i].token) >= 0) {
      elog(LOG, "ordering error in %s table: \"%s\" >= \"%s\"", tablename,
           base[i - 1].token, base[i].token);
      ok = false;
    }
  }
  return ok;
}

bool CheckDateTokenTables(void) {
  bool ok = true;

  Assert(UNIX_EPOCH_JDATE == date2j(1970, 1, 1));
  Assert(POSTGRES_EPOCH_JDATE == date2j(2000, 1, 1));

  ok &= CheckDateTokenTable("datetktbl", datetktbl, szdatetktbl);
  ok &= CheckDateTokenTable("deltatktbl", deltatktbl, szdeltatktbl);
  return ok;
}

/*
 * Common code for temporal prosupport functions: simplify, if possible,
 * a call to a temporal type's length-coercion function.
 *
 * Types time, timetz, timestamp and timestamptz each have a range of allowed
 * precisions.  An unspecified precision is rigorously equivalent to the
 * highest specifiable precision.  We can replace the function call with a
 * no-op RelabelType if it is coercing to the same or higher precision as the
 * input is known to have.
 *
 * The input Node is always a FuncExpr, but to reduce the #include footprint
 * of datetime.h, we declare it as Node *.
 *
 * Note: timestamp_scale throws an error when the typmod is out of range, but
 * we can't get there from a cast: our typmodin will have caught it already.
 */
Node *TemporalSimplify(int32 max_precis, Node *node) {
  FuncExpr *expr = castNode(FuncExpr, node);
  Node *ret = NULL;
  Node *typmod;

  Assert(list_length(expr->args) >= 2);

  typmod = (Node *)lsecond(expr->args);

  if (IsA(typmod, Const) && !((Const *)typmod)->constisnull) {
    Node *source = (Node *)linitial(expr->args);
    int32 old_precis = exprTypmod(source);
    int32 new_precis = DatumGetInt32(((Const *)typmod)->constvalue);

    if (new_precis < 0 || new_precis == max_precis ||
        (old_precis >= 0 && new_precis >= old_precis))
      ret = relabel_to_typmod(source, new_precis);
  }

  return ret;
}

/*
 * This function gets called during timezone config file load or reload
 * to create the final array of timezone tokens.  The argument array
 * is already sorted in name order.
 *
 * The result is a TimeZoneAbbrevTable (which must be a single guc_malloc'd
 * chunk) or NULL on alloc failure.  No other error conditions are defined.
 */
TimeZoneAbbrevTable *ConvertTimeZoneAbbrevs(struct tzEntry *abbrevs, int n) {
  TimeZoneAbbrevTable *tbl;
  Size tbl_size;
  int i;

  /* Space for fixed fields and datetkn array */
  tbl_size = offsetof(TimeZoneAbbrevTable, abbrevs) + n * sizeof(datetkn);
  tbl_size = MAXALIGN(tbl_size);
  /* Count up space for dynamic abbreviations */
  for (i = 0; i < n; i++) {
    struct tzEntry *abbr = abbrevs + i;

    if (abbr->zone != NULL) {
      Size dsize;

      dsize = offsetof(DynamicZoneAbbrev, zone) + strlen(abbr->zone) + 1;
      tbl_size += MAXALIGN(dsize);
    }
  }

  /* Alloc the result ... */
  tbl = (TimeZoneAbbrevTable *)guc_malloc(LOG, tbl_size);
  if (!tbl)
    return NULL;

  /* ... and fill it in */
  tbl->tblsize = tbl_size;
  tbl->numabbrevs = n;
  /* in this loop, tbl_size reprises the space calculation above */
  tbl_size = offsetof(TimeZoneAbbrevTable, abbrevs) + n * sizeof(datetkn);
  tbl_size = MAXALIGN(tbl_size);
  for (i = 0; i < n; i++) {
    struct tzEntry *abbr = abbrevs + i;
    datetkn *dtoken = tbl->abbrevs + i;

    /* use strlcpy to truncate name if necessary */
    strlcpy(dtoken->token, abbr->abbrev, TOKMAXLEN + 1);
    if (abbr->zone != NULL) {
      /* Allocate a DynamicZoneAbbrev for this abbreviation */
      DynamicZoneAbbrev *dtza;
      Size dsize;

      dtza = (DynamicZoneAbbrev *)((char *)tbl + tbl_size);
      dtza->tz = NULL;
      strcpy(dtza->zone, abbr->zone);

      dtoken->type = DYNTZ;
      /* value is offset from table start to DynamicZoneAbbrev */
      dtoken->value = (int32)tbl_size;

      dsize = offsetof(DynamicZoneAbbrev, zone) + strlen(abbr->zone) + 1;
      tbl_size += MAXALIGN(dsize);
    } else {
      dtoken->type = abbr->is_dst ? DTZ : TZ;
      dtoken->value = abbr->offset;
    }
  }

  /* Assert the two loops above agreed on size calculations */
  Assert(tbl->tblsize == tbl_size);

  /* Check the ordering, if testing */
  Assert(CheckDateTokenTable("timezone abbreviations", tbl->abbrevs, n));

  return tbl;
}

/*
 * Install a TimeZoneAbbrevTable as the active table.
 *
 * Caller is responsible that the passed table doesn't go away while in use.
 */
void InstallTimeZoneAbbrevs(TimeZoneAbbrevTable *tbl) {
  zoneabbrevtbl = tbl;
  /* reset tzabbrevcache, which may contain results from old table */
  memset(tzabbrevcache, 0, sizeof(tzabbrevcache));
}

/*
 * Helper subroutine to locate pg_tz timezone for a dynamic abbreviation.
 *
 * On failure, returns NULL and fills *extra for a DTERR_BAD_ZONE_ABBREV error.
 */
static pg_tz *FetchDynamicTimeZone(TimeZoneAbbrevTable *tbl, const datetkn *tp,
                                   DateTimeErrorExtra *extra) {
  DynamicZoneAbbrev *dtza;

  /* Just some sanity checks to prevent indexing off into nowhere */
  Assert(tp->type == DYNTZ);
  Assert(tp->value > 0 && tp->value < tbl->tblsize);

  dtza = (DynamicZoneAbbrev *)((char *)tbl + tp->value);

  /* Look up the underlying zone if we haven't already */
  if (dtza->tz == NULL) {
    dtza->tz = pg_tzset(dtza->zone);
    if (dtza->tz == NULL) {
      /* Ooops, bogus zone name in config file entry */
      extra->dtee_timezone = dtza->zone;
      extra->dtee_abbrev = tp->token;
    }
  }
  return dtza->tz;
}

/*
 * This set-returning function reads all the time zone abbreviations
 * defined by the IANA data for the current timezone setting,
 * and returns a set of (abbrev, utc_offset, is_dst).
 */
Datum pg_timezone_abbrevs_zone(PG_FUNCTION_ARGS) {
  FuncCallContext *funcctx;
  int *pindex;
  Datum result;
  HeapTuple tuple;
  Datum values[3];
  bool nulls[3] = {0};
  TimestampTz now = GetCurrentTransactionStartTimestamp();
  pg_time_t t = timestamptz_to_time_t(now);
  const char *abbrev;
  long int gmtoff;
  int isdst;
  struct pg_itm_in itm_in;
  Interval *resInterval;

  /* stuff done only on the first call of the function */
  if (SRF_IS_FIRSTCALL()) {
    TupleDesc tupdesc;
    MemoryContext oldcontext;

    /* create a function context for cross-call persistence */
    funcctx = SRF_FIRSTCALL_INIT();

    /*
     * switch to memory context appropriate for multiple function calls
     */
    oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

    /* allocate memory for user context */
    pindex = (int *)palloc(sizeof(int));
    *pindex = 0;
    funcctx->user_fctx = pindex;

    if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
      elog(ERROR, "return type must be a row type");
    funcctx->tuple_desc = tupdesc;

    MemoryContextSwitchTo(oldcontext);
  }

  /* stuff done on every call of the function */
  funcctx = SRF_PERCALL_SETUP();
  pindex = (int *)funcctx->user_fctx;

  while ((abbrev = pg_get_next_timezone_abbrev(pindex, session_timezone)) !=
         NULL) {
    /* Ignore abbreviations that aren't all-alphabetic */
    if (strspn(abbrev, "ABCDEFGHIJKLMNOPQRSTUVWXYZ") != strlen(abbrev))
      continue;

    /* Determine the current meaning of the abbrev */
    if (!pg_interpret_timezone_abbrev(abbrev, &t, &gmtoff, &isdst,
                                      session_timezone))
      continue; /* hm, not actually used in this zone? */

    values[0] = CStringGetTextDatum(abbrev);

    /* Convert offset (in seconds) to an interval; can't overflow */
    MemSet(&itm_in, 0, sizeof(struct pg_itm_in));
    itm_in.tm_usec = (int64)gmtoff * USECS_PER_SEC;
    resInterval = (Interval *)palloc(sizeof(Interval));
    (void)itmin2interval(&itm_in, resInterval);
    values[1] = IntervalPGetDatum(resInterval);

    values[2] = BoolGetDatum(isdst);

    tuple = heap_form_tuple(funcctx->tuple_desc, values, nulls);
    result = HeapTupleGetDatum(tuple);

    SRF_RETURN_NEXT(funcctx, result);
  }

  SRF_RETURN_DONE(funcctx);
}

/*
 * This set-returning function reads all the time zone abbreviations
 * defined by the timezone_abbreviations setting,
 * and returns a set of (abbrev, utc_offset, is_dst).
 */
Datum pg_timezone_abbrevs_abbrevs(PG_FUNCTION_ARGS) {
  FuncCallContext *funcctx;
  int *pindex;
  Datum result;
  HeapTuple tuple;
  Datum values[3];
  bool nulls[3] = {0};
  const datetkn *tp;
  char buffer[TOKMAXLEN + 1];
  int gmtoffset;
  bool is_dst;
  unsigned char *p;
  struct pg_itm_in itm_in;
  Interval *resInterval;

  /* stuff done only on the first call of the function */
  if (SRF_IS_FIRSTCALL()) {
    TupleDesc tupdesc;
    MemoryContext oldcontext;

    /* create a function context for cross-call persistence */
    funcctx = SRF_FIRSTCALL_INIT();

    /*
     * switch to memory context appropriate for multiple function calls
     */
    oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

    /* allocate memory for user context */
    pindex = (int *)palloc(sizeof(int));
    *pindex = 0;
    funcctx->user_fctx = pindex;

    if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
      elog(ERROR, "return type must be a row type");
    funcctx->tuple_desc = tupdesc;

    MemoryContextSwitchTo(oldcontext);
  }

  /* stuff done on every call of the function */
  funcctx = SRF_PERCALL_SETUP();
  pindex = (int *)funcctx->user_fctx;

  if (zoneabbrevtbl == NULL || *pindex >= zoneabbrevtbl->numabbrevs)
    SRF_RETURN_DONE(funcctx);

  tp = zoneabbrevtbl->abbrevs + *pindex;

  switch (tp->type) {
  case TZ:
    gmtoffset = tp->value;
    is_dst = false;
    break;
  case DTZ:
    gmtoffset = tp->value;
    is_dst = true;
    break;
  case DYNTZ: {
    /* Determine the current meaning of the abbrev */
    pg_tz *tzp;
    DateTimeErrorExtra extra;
    TimestampTz now;
    int isdst;

    tzp = FetchDynamicTimeZone(zoneabbrevtbl, tp, &extra);
    if (tzp == NULL)
      DateTimeParseError(DTERR_BAD_ZONE_ABBREV, &extra, NULL, NULL, NULL);
    now = GetCurrentTransactionStartTimestamp();
    gmtoffset = -DetermineTimeZoneAbbrevOffsetTS(now, tp->token, tzp, &isdst);
    is_dst = (bool)isdst;
    break;
  }
  default:
    elog(ERROR, "unrecognized timezone type %d", (int)tp->type);
    gmtoffset = 0; /* keep compiler quiet */
    is_dst = false;
    break;
  }

  /*
   * Convert name to text, using upcasing conversion that is the inverse of
   * what ty's ParseDateTime() uses.
   */
  strlcpy(buffer, tp->token, sizeof(buffer));
  for (p = (unsigned char *)buffer; *p; p++)
    *p = pg_toupper(*p);

  values[0] = CStringGetTextDatum(buffer);

  /* Convert offset (in seconds) to an interval; can't overflow */
  MemSet(&itm_in, 0, sizeof(struct pg_itm_in));
  itm_in.tm_usec = (int64)gmtoffset * USECS_PER_SEC;
  resInterval = (Interval *)palloc(sizeof(Interval));
  (void)itmin2interval(&itm_in, resInterval);
  values[1] = IntervalPGetDatum(resInterval);

  values[2] = BoolGetDatum(is_dst);

  (*pindex)++;

  tuple = heap_form_tuple(funcctx->tuple_desc, values, nulls);
  result = HeapTupleGetDatum(tuple);

  SRF_RETURN_NEXT(funcctx, result);
}

/*
 * This set-returning function reads all the available full time zones
 * and returns a set of (name, abbrev, utc_offset, is_dst).
 */
Datum pg_timezone_names(PG_FUNCTION_ARGS) {
  ReturnSetInfo *rsinfo = (ReturnSetInfo *)fcinfo->resultinfo;
  pg_tzenum *tzenum;
  pg_tz *tz;
  Datum values[4];
  bool nulls[4] = {0};
  int tzoff;
  struct pg_tm tm;
  fsec_t fsec;
  const char *tzn;
  Interval *resInterval;
  struct pg_itm_in itm_in;

  InitMaterializedSRF(fcinfo, 0);

  /* initialize timezone scanning code */
  tzenum = pg_tzenumerate_start();

  /* search for another zone to display */
  for (;;) {
    tz = pg_tzenumerate_next(tzenum);
    if (!tz)
      break;

    /* Convert now() to local time in this zone */
    if (timestamp2tm(GetCurrentTransactionStartTimestamp(), &tzoff, &tm, &fsec,
                     &tzn, tz) != 0)
      continue; /* ignore if conversion fails */

    /*
     * IANA's rather silly "Factory" time zone used to emit ridiculously
     * long "abbreviations" such as "Local time zone must be set--see zic
     * manual page" or "Local time zone must be set--use tzsetup".  While
     * modern versions of tzdb emit the much saner "-00", it seems some
     * benighted packagers are hacking the IANA data so that it continues
     * to produce these strings.  To prevent producing a weirdly wide
     * abbrev column, reject ridiculously long abbreviations.
     */
    if (tzn && strlen(tzn) > 31)
      continue;

    values[0] = CStringGetTextDatum(pg_get_timezone_name(tz));
    values[1] = CStringGetTextDatum(tzn ? tzn : "");

    /* Convert tzoff to an interval; can't overflow */
    MemSet(&itm_in, 0, sizeof(struct pg_itm_in));
    itm_in.tm_usec = (int64)-tzoff * USECS_PER_SEC;
    resInterval = (Interval *)palloc(sizeof(Interval));
    (void)itmin2interval(&itm_in, resInterval);
    values[2] = IntervalPGetDatum(resInterval);

    values[3] = BoolGetDatum(tm.tm_isdst > 0);

    tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
  }

  pg_tzenumerate_end(tzenum);
  return (Datum)0;
}

/*-------------------------------------------------------------------------
 *
 * pgtime.h
 *	  PostgreSQL internal timezone library
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 2026, CK Tan: the changes for digres,
 * under the MIT License (https://github.com/cktan/digres/blob/main/LICENSE)
 *
 * IDENTIFICATION
 *	  src/include/pgtime.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef _PGTIME_H
#define _PGTIME_H

/*
 * The API of this library is generally similar to the corresponding
 * C library functions, except that we use pg_time_t which (we hope) is
 * 64 bits wide, and which is most definitely signed not unsigned.
 */

typedef int64 pg_time_t;

/*
 * Data structure representing a broken-down timestamp.
 *
 * CAUTION: the IANA timezone library (src/timezone/) follows the POSIX
 * convention that tm_mon counts from 0 and tm_year is relative to 1900.
 * However, Postgres' datetime functions generally treat tm_mon as counting
 * from 1 and tm_year as relative to 1 BC.  Be sure to make the appropriate
 * adjustments when moving from one code domain to the other.
 */
struct pg_tm {
  int tm_sec;
  int tm_min;
  int tm_hour;
  int tm_mday;
  int tm_mon;  /* see above */
  int tm_year; /* see above */
  int tm_wday;
  int tm_yday;
  int tm_isdst;
  long int tm_gmtoff;
  const char *tm_zone;
};

/*
 * The zones are ty's (ty_timezone.h): a pg_tz is a ty_tz_t, and the calls
 * below are ty's, in PostgreSQL's names and shapes. A struct pg_tzenum is
 * opaque outside this library.
 */
#include "ty_timezone.h"

typedef struct ty_tz_t pg_tz;
typedef struct pg_tzenum pg_tzenum;

/* Maximum length of a timezone name (not including trailing null) */
#define TZ_STRLEN_MAX 255

/* the layout of ty's pg_tm is this one */
static_assert(sizeof(struct pg_tm) == sizeof(ty_tz_pgtm_t),
              "ty_tz_pgtm_t is pg_tm");

/* pg_localtime: the UTC instant in a zone, in storage of the thread */
static inline struct pg_tm *pg_localtime(const pg_time_t *timep,
                                         const pg_tz *tz) {
  return (struct pg_tm *)ty_tz_localtime_pg(tz, timep);
}

/*
 * pg_interpret_timezone_abbrev: what the zone meant by an abbreviation
 * (upper case) at or before the instant *timep: its offset, east of
 * Greenwich, and DST flag.
 */
static inline bool pg_interpret_timezone_abbrev(const char *abbrev,
                                                const pg_time_t *timep,
                                                long int *gmtoff, int *isdst,
                                                const pg_tz *tz) {
  int west;

  if (!ty_tz_abbrev_at(tz, *timep, abbrev, &west, isdst))
    return false;
  *gmtoff = -west;
  return true;
}

/* pg_timezone_abbrev_is_known: whether it is the zone's, and has one meaning */
static inline bool pg_timezone_abbrev_is_known(const char *abbrev,
                                               bool *isfixed,
                                               long int *gmtoff, int *isdst,
                                               const pg_tz *tz) {
  int fixed, west;

  if (!ty_tz_abbrev_is_known(tz, abbrev, &fixed, &west, isdst))
    return false;
  *isfixed = fixed != 0;
  *gmtoff = -west;
  return true;
}

static inline const char *pg_get_next_timezone_abbrev(int *indx,
                                                      const pg_tz *tz) {
  return ty_tz_next_abbrev(tz, indx);
}

/* pg_get_timezone_offset: whether the zone has one offset, and it, east */
static inline bool pg_get_timezone_offset(const pg_tz *tz, long int *gmtoff) {
  return ty_tz_fixed_offset(tz, gmtoff) != 0;
}

static inline const char *pg_get_timezone_name(pg_tz *tz) {
  return tz ? ty_tz_name(tz) : NULL;
}

/* A zone with leap seconds is not acceptable: ty's data has none. */
static inline bool pg_tz_acceptable(pg_tz *tz) { return true; }

/* these functions are in strftime.c */
extern size_t pg_strftime(char *s, size_t maxsize, const char *format,
                          const struct pg_tm *t);

/* these functions and variables are in pgtz.c */
extern thread_local /*LOCAL*/ PGDLLIMPORT pg_tz *session_timezone;
extern void pg_timezone_initialize(void);
extern pg_tz *pg_tzset(const char *tzname);
extern pg_tz *pg_tzset_offset(long gmtoffset);
extern pg_tzenum *pg_tzenumerate_start(void);
extern pg_tz *pg_tzenumerate_next(pg_tzenum *dir);
extern void pg_tzenumerate_end(pg_tzenum *dir);

#endif /* _PGTIME_H */

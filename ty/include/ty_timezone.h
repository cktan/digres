/*
 * Copyright (c) 2026 CK Tan
 * SPDX-License-Identifier: MIT
 */

/*
 * ty_timezone.h -- time zones: the rules of the IANA database, from the data
 * that ty holds (src/tzdata.cc, ../TZDATA.md), with no file and no setting
 * of the host. The code is PostgreSQL's (src/timezone/localtime.c and
 * pgtz.c, with datetime.c's DetermineTimeZoneOffset), moved with its notice
 * into src/timezone.cc.
 *
 * A zone is found by its name, in any case ("america/new_york"), or by a
 * POSIX spec ("EST5EDT", "UTC+3"), as PostgreSQL's pg_tzset does. A zone
 * found stays good until the process ends: the host may keep the pointer.
 * The calls are safe for many threads. A lookup by name takes a lock and
 * copies the name: a host that asks often keeps the pointer.
 *
 * Offsets are in seconds. An "east" offset is the ISO sign, positive east of
 * Greenwich; a "west" offset is PostgreSQL's, as DetermineTimeZoneOffset
 * gives it. A year is the full year, as in a ty_tm_t.
 */
#ifndef TY_TIMEZONE_H
#define TY_TIMEZONE_H

#include <stddef.h>
#include <stdint.h>

#include "ty.h"
#include "ty_datetime.h"

typedef struct ty_tz_t ty_tz_t;

/* A UTC instant in a zone: the fields, the offset, and the abbreviation. */
typedef struct ty_tz_local_t {
  int32_t year, mon, mday, hour, min, sec; /* mon 1 to 12 */
  int32_t wday;                            /* 0 is Sunday */
  int32_t yday;                            /* 0 is January 1st */
  int32_t isdst;
  int32_t gmtoff;     /* seconds east of Greenwich */
  const char *abbrev; /* good as long as the zone is: always */
} ty_tz_local_t;

/* The zone named name, or NULL if there is none (pg_tzset). */
const ty_tz_t *ty_tz_find(const char *name);

/*
 * A zone with the one offset, west of Greenwich (pg_tzset_offset), or NULL
 * if the offset is out of range.
 */
const ty_tz_t *ty_tz_from_offset(long west);

/* The name of the zone, in the case of the database. */
const char *ty_tz_name(const ty_tz_t *tz);

/*
 * The UTC instant t (seconds since 1970) in the zone (pg_localtime): 0, or -1
 * with TY_ERR_RANGE ("timestamp out of range") if it does not fit.
 */
int ty_tz_localtime(const ty_tz_t *tz, int64_t t, ty_tz_local_t *out);

/*
 * The offset in the zone at the local time tm, west of Greenwich, and in
 * *isdst whether it is daylight time (DetermineTimeZoneOffset). A time that a
 * transition skips takes the offset before it, and one it repeats the offset
 * after it. A date out of range is UTC: 0, not daylight. *utc, if not NULL,
 * is the instant, in seconds since 1970 (0 for a date out of range).
 */
int ty_tz_offset_for_local(const ty_tz_t *tz, const ty_tm_t *tm, int *isdst,
                           int64_t *utc);

/*
 * The offset, west of Greenwich, to give a dynamic abbreviation at the local
 * time tm (DetermineTimeZoneAbbrevOffset): that of the abbreviation in the
 * zone, if the zone ever used it, else the zone's own at that time.
 * The abbreviation is in any case.
 */
int ty_tz_abbrev_offset(const ty_tz_t *tz, const ty_tm_t *tm, const char *abbr,
                        int *isdst);

/*
 * The offset, west of Greenwich, the zone gave the abbreviation at or before
 * the UTC instant t (or the first after it, if never before); 1 if it is
 * known in the zone, 0 if not (DetermineTimeZoneAbbrevOffsetInternal).
 */
int ty_tz_abbrev_at(const ty_tz_t *tz, int64_t t, const char *abbr,
                    int *offset_west, int *isdst);

/*
 * Whether the abbreviation is one of the zone's, in any case: 1 or 0
 * (TimeZoneAbbrevIsKnown). *fixed says whether it has the same meaning
 * wherever the zone uses it; *offset is west of Greenwich, as above.
 */
int ty_tz_abbrev_is_known(const ty_tz_t *tz, const char *abbr, int *fixed,
                          int *offset_west, int *isdst);

/*
 * Whether the zone has one offset at all times (pg_get_timezone_offset): 1
 * and *east, or 0.
 */
int ty_tz_fixed_offset(const ty_tz_t *tz, long *east);

/*
 * The data of the zones. The release of the database ("2026c"), the number
 * of names, and the name at an index (in order of the lower-case name).
 */
const char *ty_tz_data_version(void);
unsigned ty_tz_name_count(void);
const char *ty_tz_name_at(unsigned i);

/*
 * timestamptz operations that need a zone. A timestamptz is the UTC instant,
 * as a timestamp is: microseconds since 2000-01-01. Each call takes the zone
 * (the session's, or the one asked for), and a failure is -1 with
 * TY_ERR_RANGE and PostgreSQL's message. The infinities pass through.
 */

/*
 * The local fields of the instant ts in the zone (timestamp2tm with a
 * zone): the fields, the microseconds, the offset west of Greenwich, whether
 * it is daylight time, and the abbreviation (valid as long as the zone is).
 * abbrev and isdst may be NULL.
 */
int ty_timestamptz_to_local(const ty_tz_t* tz, int64_t ts, ty_tm_t* tm,
                            int32_t* fsec, int* west, int* isdst,
                            const char** abbrev);

/*
 * timestamp to timestamptz: the local time in the zone as the instant
 * (timestamp2timestamptz_opt_overflow). A result out of range is, if
 * overflow is not NULL, an infinity with *overflow -1 or 1, else an error.
 */
int ty_timestamp_to_timestamptz(const ty_tz_t* tz, int64_t ts, int64_t* out,
                                int* overflow);

/* timestamptz to timestamp: the local time in the zone (timestamptz2timestamp). */
int ty_timestamptz_to_timestamp(const ty_tz_t* tz, int64_t ts, int64_t* out);

/*
 * date to timestamptz: midnight in the zone (date2timestamptz_opt_overflow).
 * A date with no timestamp, or whose midnight in the zone is out of range,
 * is as above, with the message "date out of range for timestamp".
 */
int ty_date_to_timestamptz(const ty_tz_t* tz, int32_t d, int64_t* out,
                           int* overflow);

/* timestamptz to date: the date in the zone (timestamptz_date). */
int ty_timestamptz_to_date(const ty_tz_t* tz, int64_t ts, int32_t* out);

/*
 * PostgreSQL's own shapes, for a host whose code is PostgreSQL's (digres).
 * A ty_tz_pgtm_t is PostgreSQL's pg_tm: the month counts from 0 and the year
 * from 1900, as in the IANA library.
 */
typedef struct ty_tz_pgtm_t {
  int tm_sec, tm_min, tm_hour, tm_mday, tm_mon, tm_year, tm_wday, tm_yday,
      tm_isdst;
  long tm_gmtoff;      /* seconds east of Greenwich */
  const char *tm_zone; /* the abbreviation */
} ty_tz_pgtm_t;

/*
 * pg_localtime: the UTC instant *t in the zone, or NULL if it does not fit.
 * The result is in storage of the calling thread, good until its next call.
 */
const ty_tz_pgtm_t *ty_tz_localtime_pg(const ty_tz_t *tz, const int64_t *t);

/*
 * pg_get_next_timezone_abbrev: the abbreviations a zone uses, one at a time.
 * *indx is zero at the first call; do not touch it between calls. NULL when
 * there are no more.
 */
const char *ty_tz_next_abbrev(const ty_tz_t *tz, int *indx);

/*
 * The bytes of the zone file of a zone (in any case), as zic wrote it, for a
 * host that has its own reader of that format. canonname, if not NULL, gets
 * the name in the database's case: it must have room for 256 bytes. 1 if
 * there is such a zone, else 0. The bytes stay good as long as the data.
 */
int ty_tz_file(const char *name, char *canonname, const unsigned char **data,
               size_t *size);

/*
 * Use a pack of zones the host holds, as one it mapped from a file, in place
 * of ty's own. ty checks its magic number, its format version, its size and
 * its checksum. It must be called before the first zone is found, and the
 * memory must stay good for the life of the process. 0, or -1 with
 * TY_ERR_INVALID if it is not a good pack or a zone was found.
 */
int ty_tz_use_data(const void *pack, size_t size);

#endif

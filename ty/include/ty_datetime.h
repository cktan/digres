/*
 * Copyright (c) 2026 CK Tan
 * SPDX-License-Identifier: MIT
 */

/*
 * ty_datetime.h -- the calendar and the arithmetic of date, time and
 * timestamp (without a time zone). The cases and the errors follow
 * PostgreSQL's datetime.c, date.c and timestamp.c.
 *
 * A date is an int32 count of days since 2000-01-01; a time is an int64
 * count of microseconds since midnight; a timestamp is an int64 count of
 * microseconds since 2000-01-01 00:00. The smallest and the largest value of
 * a date and of a timestamp are minus and plus infinity.
 *
 * A year in a ty_tm_t is PostgreSQL's: 1 is 1 AD, 0 is 1 BC, -1 is 2 BC.
 * The text of these types depends on DateStyle and the time zone, and is the
 * host's.
 *
 * A call that can fail returns 0, or -1 with the thread's error set, whose
 * message is PostgreSQL's: TY_ERR_RANGE for a value out of range, and
 * TY_ERR_INVALID for a field that is out of its range.
 */
#ifndef TY_DATETIME_H
#define TY_DATETIME_H

#include <stddef.h>
#include <stdint.h>

#include "ty.h"
#include "ty_interval.h"

#define TY_POSTGRES_EPOCH_JDATE 2451545 /* the Julian day of 2000-01-01 */
#define TY_USECS_PER_HOUR INT64_C(3600000000)
#define TY_USECS_PER_MINUTE INT64_C(60000000)
#define TY_USECS_PER_SEC INT64_C(1000000)

#define TY_DATE_NOBEGIN INT32_MIN
#define TY_DATE_NOEND INT32_MAX
#define TY_TIMESTAMP_NOBEGIN INT64_MIN
#define TY_TIMESTAMP_NOEND INT64_MAX

/* The smallest and the largest value a timestamp may have, and a date. */
#define TY_MIN_TIMESTAMP INT64_C(-211813488000000000)
#define TY_END_TIMESTAMP INT64_C(9223371331200000000)
#define TY_MIN_DATE (-2451545)
#define TY_END_DATE (2147483494 - 2451545)

static inline int ty_date_finite(int32_t d) {
    return d != TY_DATE_NOBEGIN && d != TY_DATE_NOEND;
}
static inline int ty_date_valid(int32_t d) {
    return TY_MIN_DATE <= d && d < TY_END_DATE;
}
static inline int ty_timestamp_finite(int64_t t) {
    return t != TY_TIMESTAMP_NOBEGIN && t != TY_TIMESTAMP_NOEND;
}
static inline int ty_timestamp_valid(int64_t t) {
    return TY_MIN_TIMESTAMP <= t && t < TY_END_TIMESTAMP;
}

/* Broken-down time: the fields of PostgreSQL's pg_tm that these use. */
typedef struct ty_tm_t {
    int32_t year, mon, mday, hour, min, sec;
} ty_tm_t;

/* The calendar: the Julian day of a date, and back; the weekday (0 Sunday). */
int ty_date2j(int year, int mon, int mday);
void ty_j2date(int jd, int* year, int* mon, int* mday);
int ty_j2day(int jd);
static inline int ty_isleap(int y) {
    return (y % 4) == 0 && ((y % 100) != 0 || (y % 400) == 0);
}

/* The days in a month (1 to 12) of a year. */
int ty_days_in_month(int year, int mon);

/*
 * date: a day number from a year, a month and a day, as make_date does:
 * a negative year is BC, and year 0 is no year. A field out of its range
 * is TY_ERR_INVALID ("date field value out of range: Y-M-D"); a date that
 * is out of the range of dates is TY_ERR_RANGE ("date out of range: Y-M-D").
 */
int ty_date_make(int32_t year, int32_t mon, int32_t mday, int32_t* out);

/*
 * date + days and date - days; infinity stays. TY_ERR_RANGE ("date out of
 * range") otherwise. In line, as a date is used in hot loops; the errors are
 * recorded out of line.
 */
int ty_date_range_error(void);
int ty_date_infinite_error(void);

static inline int ty_date_add_days(int32_t d, int32_t days, int32_t* out) {
    int32_t r;

    if (!ty_date_finite(d)) {
        *out = d;
        return 0;
    }
    if (TY_UNLIKELY(__builtin_add_overflow(d, days, &r) || !ty_date_valid(r))) {
        return ty_date_range_error();
    }
    *out = r;
    return 0;
}

static inline int ty_date_sub_days(int32_t d, int32_t days, int32_t* out) {
    int32_t r;

    if (!ty_date_finite(d)) {
        *out = d;
        return 0;
    }
    if (TY_UNLIKELY(__builtin_sub_overflow(d, days, &r) || !ty_date_valid(r))) {
        return ty_date_range_error();
    }
    *out = r;
    return 0;
}

/* date - date, in days; an infinite date is TY_ERR_RANGE. */
static inline int ty_date_diff(int32_t a, int32_t b, int32_t* out) {
    if (TY_UNLIKELY(!ty_date_finite(a) || !ty_date_finite(b))) {
        return ty_date_infinite_error();
    }
    *out = (int32_t)((uint32_t)a - (uint32_t)b);
    return 0;
}

/*
 * time: from hours, minutes and seconds (a float, rounded to a microsecond)
 * as make_time does; a field out of range is TY_ERR_INVALID. time_make_usec
 * is the same with the seconds as microseconds.
 */
int ty_time_make(int32_t hour, int32_t min, double sec, int64_t* out);
int ty_time_overflows(int hour, int min, int sec, int32_t fsec);

/* time + interval and time - interval, wrapping at midnight. */
int ty_time_add_interval(int64_t t, const ty_interval_t* span, int64_t* out);
int ty_time_sub_interval(int64_t t, const ty_interval_t* span, int64_t* out);

/* time - time, as an interval of microseconds. */
void ty_time_diff(int64_t a, int64_t b, ty_interval_t* out);

/*
 * A timestamp as broken-down time: TY_ERR_RANGE if its day is not a
 * Julian day that fits (fsec is the microseconds of the second); and back,
 * with the range check of a timestamp.
 */
int ty_timestamp_to_tm(int64_t ts, ty_tm_t* tm, int32_t* fsec);
int ty_tm_to_timestamp(const ty_tm_t* tm, int32_t fsec, int64_t* out);

/* timestamp + interval and - interval: the months first, then the days. */
int ty_timestamp_add_interval(int64_t ts, const ty_interval_t* span,
                              int64_t* out);
int ty_timestamp_sub_interval(int64_t ts, const ty_interval_t* span,
                              int64_t* out);

/* timestamp - timestamp, as an interval with the hours justified. */
int ty_timestamp_diff(int64_t a, int64_t b, ty_interval_t* out);

/*
 * The ISO 8601 week of a date: the week number, the year the week is in, the
 * day of that year, and the Julian day of the first day of a week of a year.
 */
int ty_date2isoweek(int year, int mon, int mday);
int ty_date2isoyear(int year, int mon, int mday);
int ty_date2isoyearday(int year, int mon, int mday);
int ty_isoweek2j(int year, int week);
void ty_isoweek2date(int woy, int* year, int* mon, int* mday);

/*
 * A unit of time as a word of the host's SQL: 1 and the unit's code
 * (the DTK_ values of PostgreSQL: week 22, month 23, quarter 24, year 25,
 * decade 26, century 27, millennium 28, millisecond 29, microsecond 30,
 * second 18, minute 19, hour 20, day 21) if the lower-case word is one,
 * 0 if it is not.
 */
int ty_datetime_unit(const char* lowtoken, int* code);

/*
 * date_trunc of a timestamp: the timestamp cut at a unit (see above). An
 * infinite timestamp is returned as it is for a unit it can be cut at; a
 * code that is no unit of a timestamp (dow, epoch, ...) is TY_ERR_INVALID
 * ("unit not supported"). A result out of range is TY_ERR_RANGE.
 */
int ty_timestamp_trunc(int code, int64_t ts, int64_t* out);

/*
 * date_bin: the start of the bin of timestamp, of stride, from origin. The
 * errors are TY_ERR_RANGE with PostgreSQL's message, and for a stride with
 * months, TY_ERR_INVALID ("timestamps cannot be binned into intervals
 * containing months or years").
 */
int ty_timestamp_bin(const ty_interval_t* stride, int64_t ts, int64_t origin,
                     int64_t* out);

/*
 * age(a, b) of two timestamps: the years, months, days and time from b to a,
 * the borrow of a month taking the days of the month of the earlier one.
 */
int ty_timestamp_age(int64_t a, int64_t b, ty_interval_t* out);

/*
 * Make a timestamp fit its precision (0 to 6; -1 or 6 leaves it): rounded to
 * the microsecond digits, away from zero for a half. A precision outside 0 to
 * 6 is TY_ERR_INVALID ("timestamp(%d) precision must be between 0 and 6").
 * An infinite timestamp is not changed.
 */
int ty_timestamp_adjust_typmod(int64_t* ts, int32_t typmod);

/* Make a time fit its precision (0 to 6; anything else leaves it). */
static inline void ty_time_adjust_typmod(int64_t* t, int32_t typmod) {
    static const int64_t scales[7] = {1000000, 100000, 10000, 1000, 100, 10, 1};
    static const int64_t offsets[7] = {500000, 50000, 5000, 500, 50, 5, 0};

    if (typmod >= 0 && typmod <= 6) {
        if (*t >= 0) {
            *t = ((*t + offsets[typmod]) / scales[typmod]) * scales[typmod];
        } else {
            *t = -((((-*t) + offsets[typmod]) / scales[typmod]) * scales[typmod]);
        }
    }
}

/*
 * The text of date, time, timestamp and interval, written by the settings
 * the host passes in: a date style, a date order and an interval style, whose
 * values are PostgreSQL's. The result is written to out, which must have
 * TY_DATETIME_TEXT_MAX bytes, with a NUL; the length is returned. (The
 * special values, infinity, are the host's.) The text read is still in the
 * host.
 */
#define TY_DATETIME_TEXT_MAX 128

enum { TY_DATES_POSTGRES, TY_DATES_ISO, TY_DATES_SQL, TY_DATES_GERMAN, TY_DATES_XSD };
enum { TY_DATEORDER_YMD, TY_DATEORDER_DMY, TY_DATEORDER_MDY };
enum { TY_INTSTYLE_POSTGRES, TY_INTSTYLE_POSTGRES_VERBOSE, TY_INTSTYLE_SQL_STANDARD, TY_INTSTYLE_ISO_8601 };

/* An interval in parts, as PostgreSQL's pg_itm. */
typedef struct ty_itm_t {
    int64_t hour;
    int32_t mday, mon, year, min, sec, usec;
} ty_itm_t;

/* The parts of a finite interval. */
void ty_interval_to_itm(const ty_interval_t* span, ty_itm_t* itm);

/* A date: "2000-01-31" in ISO, and the other styles. */
size_t ty_date_format(const ty_tm_t* tm, int style, int order, char* out);

/*
 * A time of day, and a timestamp. print_tz writes the zone: tzn is its name
 * (or NULL), tz the offset in seconds west of Greenwich (the sign PostgreSQL
 * uses); isdst below 0 means the zone is not known, and nothing is written.
 */
size_t ty_time_format(const ty_tm_t* tm, int32_t fsec, int print_tz, int tz,
                      int style, char* out);
size_t ty_timestamp_format(const ty_tm_t* tm, int32_t fsec, int print_tz,
                           int tz, const char* tzn, int isdst, int style,
                           int order, char* out);

/* An interval, in an interval style. */
size_t ty_interval_format(const ty_itm_t* itm, int style, char* out);

#endif

/*
 * Copyright (c) 2026 CK Tan
 * SPDX-License-Identifier: MIT
 */

/*
 * datetime.cc -- the calendar and the arithmetic of date, time and timestamp.
 */
#include "ty_datetime.h"
#include "ty_hash.h"
#include "ty_int.h"

#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <utility>

#include "error.h"

namespace {

const int k_day_tab[2][12] = {{31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31},
                              {31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31}};

/* The error for a timestamp that does not fit. */
int ts_range() {
  return ty_internal::set_error(TY_ERR_RANGE, "timestamp out of range");
}

/* The error for an interval that does not fit. */
int iv_range() {
  return ty_internal::set_error(TY_ERR_RANGE, "interval out of range");
}

/* Whether a year, a month and a day are in the range PostgreSQL accepts. */
bool valid_julian(int y, int m) {
  return (y > -4713 || (y == -4713 && m >= 11)) &&
         (y < 5874898 || (y == 5874898 && m < 6));
}

} // namespace

/* The Julian day of a date (the arithmetic is the Fliegel-Van Flandern one). */
int ty_date2j(int year, int month, int day) {
  int julian, century;

  if (month > 2) {
    month += 1;
    year += 4800;
  } else {
    month += 13;
    year += 4799;
  }
  century = year / 100;
  julian = year * 365 - 32167;
  julian += year / 4 - century + century / 4;
  julian += 7834 * month / 256 + day;
  return julian;
}

/* The date of a Julian day. */
void ty_j2date(int jd, int *year, int *month, int *day) {
  unsigned int julian = (unsigned int)jd, quad, extra;
  int y;

  julian += 32044;
  quad = julian / 146097;
  extra = (julian - quad * 146097) * 4 + 3;
  julian += 60 + quad * 3 + extra / 146097;
  quad = julian / 1461;
  julian -= quad * 1461;
  y = (int)(julian * 4 / 1461);
  julian = ((y != 0) ? ((julian + 305) % 365) : ((julian + 306) % 366)) + 123;
  y += (int)(quad * 4);
  *year = y - 4800;
  quad = julian * 2141 / 65536;
  *day = (int)(julian - 7834 * quad / 256);
  *month = (int)((quad + 10) % 12 + 1);
}

/* The weekday of a Julian day: 0 is Sunday. */
int ty_j2day(int date) {
  date += 1;
  date %= 7;
  if (date < 0) {
    date += 7;
  }
  return date;
}

/* The days in a month. */
int ty_days_in_month(int year, int mon) {
  return k_day_tab[ty_isleap(year)][mon - 1];
}

/*
 * make_date.
 * 1. A negative year is BC: use its absolute value (the smallest int32 has
 *    none).
 * 2. A year below 1, a month not in 1..12, a day not in the month: a field out
 *    of range. The year of BC is then 1 - year, as the Julian routines want.
 * 3. A date outside the Julian range, or just outside the dates: out of range.
 */
static int date_make_impl(int32_t year, int32_t mon, int32_t mday,
                          bool as_timestamp, int32_t *out) {
  char msg[96];
  int32_t y = year;
  bool bc = false;

  if (year < 0) {
    bc = true;
    if (__builtin_sub_overflow((int32_t)0, year, &y)) {
      snprintf(msg, sizeof msg, "date field value out of range: %d-%02d-%02d",
               year, mon, mday);
      return ty_internal::set_error(TY_ERR_INVALID, msg);
    }
  }
  /* ValidateDate: no year 0 in AD and BC notation */
  int32_t ty = y;
  bool bad = false;

  if (bc) {
    if (ty <= 0) {
      bad = true;
    } else {
      ty = -(ty - 1);
    }
  } else if (ty <= 0) {
    bad = true;
  }
  if (!bad && (mon < 1 || mon > 12)) {
    bad = true;
  }
  if (!bad && (mday < 1 || mday > 31)) {
    bad = true;
  }
  if (!bad && mday > ty_days_in_month(ty, mon)) {
    bad = true;
  }
  if (bad) {
    /* make_date prints the year after the BC change, make_timestamp as given */
    snprintf(msg, sizeof msg, "date field value out of range: %d-%02d-%02d",
             as_timestamp ? year : (bc && ty != y ? ty : y), mon, mday);
    return ty_internal::set_error(TY_ERR_INVALID, msg);
  }
  if (!valid_julian(ty, mon)) {
    snprintf(msg, sizeof msg, "date out of range: %d-%02d-%02d",
             as_timestamp ? year : ty, mon, mday);
    return ty_internal::set_error(TY_ERR_RANGE, msg);
  }
  int32_t date = ty_date2j(ty, mon, mday) - TY_POSTGRES_EPOCH_JDATE;

  /* a timestamp has no other check of the date: its range is the result's */
  if (!as_timestamp && !ty_date_valid(date)) {
    snprintf(msg, sizeof msg, "date out of range: %d-%02d-%02d", ty, mon, mday);
    return ty_internal::set_error(TY_ERR_RANGE, msg);
  }
  *out = date;
  return 0;
}

/* make_date: see ty_datetime.h. */
int ty_date_make(int32_t year, int32_t mon, int32_t mday, int32_t *out) {
  return date_make_impl(year, mon, mday, false, out);
}

/* make_timestamp: see ty_datetime.h. */
int ty_timestamp_make(int32_t year, int32_t mon, int32_t mday, int32_t hour,
                      int32_t min, double sec, int64_t *out) {
  int32_t date;
  int64_t time, result;

  if (date_make_impl(year, mon, mday, true, &date) != 0) {
    return -1;
  }
  if (ty_time_make(hour, min, sec, &time) != 0) {
    return -1;
  }
  /* the result must fit, and the final range check catches just-out-of-range */
  if (__builtin_mul_overflow((int64_t)date, TY_USECS_PER_HOUR * 24, &result) ||
      __builtin_add_overflow(result, time, &result) ||
      !ty_timestamp_valid(result)) {
    char msg[128];

    snprintf(msg, sizeof msg,
             "timestamp out of range: %d-%02d-%02d %d:%02d:%02g", year, mon,
             mday, hour, min, sec);
    return ty_internal::set_error(TY_ERR_RANGE, msg);
  }
  *out = result;
  return 0;
}

/* The error of a date that does not fit. */
int ty_date_range_error(void) {
  return ty_internal::set_error(TY_ERR_RANGE, "date out of range");
}

/* The error of the difference with an infinite date. */
int ty_date_infinite_error(void) {
  return ty_internal::set_error(TY_ERR_RANGE, "cannot subtract infinite dates");
}

/* Whether the fields are a time that is not within a day. */
int ty_time_overflows(int hour, int min, int sec, int32_t fsec) {
  if (hour < 0 || hour > 24 || min < 0 || min >= 60 || sec < 0 || sec > 60 ||
      fsec < 0 || fsec > 1000000) {
    return 1;
  }
  if ((((((int64_t)hour * 60 + min) * 60) + sec) * TY_USECS_PER_SEC + fsec) >
      TY_USECS_PER_HOUR * 24) {
    return 1;
  }
  return 0;
}

/* make_time: the seconds are a float. */
int ty_time_make(int32_t hour, int32_t min, double sec, int64_t *out) {
  bool bad = hour < 0 || hour > 24 || min < 0 || min >= 60 || std::isnan(sec);

  if (!bad) {
    double s = rint(sec * 1000000.0);

    if (s < 0 || s > 60.0 * 1000000.0 ||
        ((((int64_t)hour * 60 + min) * 60) * TY_USECS_PER_SEC + (int64_t)s) >
            TY_USECS_PER_HOUR * 24) {
      bad = true;
    }
  }
  if (bad) {
    char msg[96];

    /* PostgreSQL's snprintf writes NaN and Infinity as these words */
    if (std::isnan(sec)) {
      snprintf(msg, sizeof msg, "time field value out of range: %d:%02d:NaN",
               hour, min);
    } else if (std::isinf(sec)) {
      snprintf(msg, sizeof msg,
               "time field value out of range: %d:%02d:%sInfinity", hour, min,
               sec < 0 ? "-" : "");
    } else {
      snprintf(msg, sizeof msg, "time field value out of range: %d:%02d:%02g",
               hour, min, sec);
    }
    return ty_internal::set_error(TY_ERR_INVALID, msg);
  }
  *out = ((((int64_t)hour * 60 + min) * 60) * TY_USECS_PER_SEC) +
         (int64_t)rint(sec * 1000000.0);
  return 0;
}

namespace {} // namespace

/* The error of time + or - an infinite interval. */
int ty_time_interval_error(int sub) {
  return ty_internal::set_error(
      TY_ERR_RANGE, sub ? "cannot subtract infinite interval from time"
                        : "cannot add infinite interval to time");
}

/* time - time. */
void ty_time_diff(int64_t a, int64_t b, ty_interval_t *out) {
  out->month = 0;
  out->day = 0;
  out->time = (int64_t)((uint64_t)a - (uint64_t)b);
}

/*
 * A timestamp as broken-down time.
 * 1. Split the day and the time of day, the time of day non-negative.
 * 2. The Julian day must be 0 to INT_MAX.
 */
int ty_timestamp_to_tm(int64_t dt, ty_tm_t *tm, int32_t *fsec) {
  int64_t time = dt, date;

  date = time / (TY_USECS_PER_HOUR * 24);
  if (date != 0) {
    time -= date * (TY_USECS_PER_HOUR * 24);
  }
  if (time < 0) {
    time += TY_USECS_PER_HOUR * 24;
    date -= 1;
  }
  date += TY_POSTGRES_EPOCH_JDATE;
  if (date < 0 || date > (int64_t)INT_MAX) {
    return -1;
  }
  int y, m, d;

  ty_j2date((int)date, &y, &m, &d);
  tm->year = y;
  tm->mon = m;
  tm->mday = d;
  tm->hour = (int32_t)(time / TY_USECS_PER_HOUR);
  time -= (int64_t)tm->hour * TY_USECS_PER_HOUR;
  tm->min = (int32_t)(time / TY_USECS_PER_MINUTE);
  time -= (int64_t)tm->min * TY_USECS_PER_MINUTE;
  tm->sec = (int32_t)(time / TY_USECS_PER_SEC);
  *fsec = (int32_t)(time - (int64_t)tm->sec * TY_USECS_PER_SEC);
  return 0;
}

/* Broken-down time as a timestamp. */
int ty_tm_to_timestamp(const ty_tm_t *tm, int32_t fsec, int64_t *out) {
  int64_t date, time, r;

  if (!valid_julian(tm->year, tm->mon)) {
    return -1;
  }
  date = ty_date2j(tm->year, tm->mon, tm->mday) - TY_POSTGRES_EPOCH_JDATE;
  time =
      ((((int64_t)tm->hour * 60 + tm->min) * 60) + tm->sec) * TY_USECS_PER_SEC +
      fsec;
  if (__builtin_mul_overflow(date, TY_USECS_PER_HOUR * 24, &r) ||
      __builtin_add_overflow(r, time, &r)) {
    return -1;
  }
  if (!ty_timestamp_valid(r)) {
    return -1;
  }
  *out = r;
  return 0;
}

/*
 * timestamp + interval.
 * 1. An infinite interval makes an infinite timestamp (a clash is an error);
 *    an infinite timestamp stays.
 * 2. The months move the month and the year, and the day is cut to the
 *    month's last.
 * 3. The days move the Julian day.
 * 4. The microseconds are added; the result must be a valid timestamp.
 */
int ty_timestamp_add_interval(int64_t timestamp, const ty_interval_t *span,
                              int64_t *out) {
  if (ty_interval_is_nobegin(span)) {
    if (timestamp == TY_TIMESTAMP_NOEND) {
      return ts_range();
    }
    *out = TY_TIMESTAMP_NOBEGIN;
    return 0;
  }
  if (ty_interval_is_noend(span)) {
    if (timestamp == TY_TIMESTAMP_NOBEGIN) {
      return ts_range();
    }
    *out = TY_TIMESTAMP_NOEND;
    return 0;
  }
  if (!ty_timestamp_finite(timestamp)) {
    *out = timestamp;
    return 0;
  }
  ty_tm_t tm;
  int32_t fsec;

  if (span->month != 0) {
    if (ty_timestamp_to_tm(timestamp, &tm, &fsec) != 0) {
      return ts_range();
    }
    if (__builtin_add_overflow(tm.mon, span->month, &tm.mon)) {
      return ts_range();
    }
    if (tm.mon > 12) {
      tm.year += (tm.mon - 1) / 12;
      tm.mon = ((tm.mon - 1) % 12) + 1;
    } else if (tm.mon < 1) {
      tm.year += tm.mon / 12 - 1;
      tm.mon = tm.mon % 12 + 12;
    }
    if (tm.mday > ty_days_in_month(tm.year, tm.mon)) {
      tm.mday = ty_days_in_month(tm.year, tm.mon);
    }
    if (ty_tm_to_timestamp(&tm, fsec, &timestamp) != 0) {
      return ts_range();
    }
  }
  if (span->day != 0) {
    int julian;

    if (ty_timestamp_to_tm(timestamp, &tm, &fsec) != 0) {
      return ts_range();
    }
    julian = ty_date2j(tm.year, tm.mon, tm.mday);
    if (__builtin_add_overflow(julian, span->day, &julian) || julian < 0) {
      return ts_range();
    }
    int y, m, d;

    ty_j2date(julian, &y, &m, &d);
    tm.year = y;
    tm.mon = m;
    tm.mday = d;
    if (ty_tm_to_timestamp(&tm, fsec, &timestamp) != 0) {
      return ts_range();
    }
  }
  if (__builtin_add_overflow(timestamp, span->time, &timestamp)) {
    return ts_range();
  }
  if (!ty_timestamp_valid(timestamp)) {
    return ts_range();
  }
  *out = timestamp;
  return 0;
}

/* timestamp - interval: add the negated interval. */
int ty_timestamp_sub_interval(int64_t timestamp, const ty_interval_t *span,
                              int64_t *out) {
  ty_interval_t neg;

  if (ty_interval_neg(span, &neg) != 0) {
    return iv_range();
  }
  return ty_timestamp_add_interval(timestamp, &neg, out);
}

/*
 * timestamp - timestamp: the difference as microseconds, with the hours
 * justified into days. An infinite value is an infinite interval (two of the
 * same end are an error).
 */
int ty_timestamp_diff(int64_t dt1, int64_t dt2, ty_interval_t *result) {
  if (!ty_timestamp_finite(dt1) || !ty_timestamp_finite(dt2)) {
    if (dt1 == TY_TIMESTAMP_NOBEGIN) {
      if (dt2 == TY_TIMESTAMP_NOBEGIN) {
        return iv_range();
      }
      ty_interval_set_nobegin(result);
    } else if (dt1 == TY_TIMESTAMP_NOEND) {
      if (dt2 == TY_TIMESTAMP_NOEND) {
        return iv_range();
      }
      ty_interval_set_noend(result);
    } else if (dt2 == TY_TIMESTAMP_NOBEGIN) {
      ty_interval_set_noend(result);
    } else {
      ty_interval_set_nobegin(result);
    }
    return 0;
  }
  ty_interval_t span;

  if (__builtin_sub_overflow(dt1, dt2, &span.time)) {
    return iv_range();
  }
  span.month = 0;
  span.day = 0;
  return ty_interval_justify_hours(&span, result);
}

/* The error of a timestamp precision out of range. */
int ty_timestamp_typmod_error(int32_t typmod) {
  char msg[96];

  snprintf(msg, sizeof msg, "timestamp(%d) precision must be between %d and %d",
           typmod, 0, 6);
  return ty_internal::set_error(TY_ERR_INVALID, msg);
}

/* The Julian day of the Monday of the first ISO week of a year. */
int ty_isoweek2j(int year, int week) {
  int day4 = ty_date2j(year, 1, 4);
  int day0 = ty_j2day(day4 - 1);

  return ((week - 1) * 7) + (day4 - day0);
}

/* The date of the Monday of a week of a year (year is given and set). */
void ty_isoweek2date(int woy, int *year, int *mon, int *mday) {
  ty_j2date(ty_isoweek2j(*year, woy), year, mon, mday);
}

/* The ISO week of a date. */
int ty_date2isoweek(int year, int mon, int mday) {
  double result;
  int day0, day4, dayn;

  dayn = ty_date2j(year, mon, mday);
  day4 = ty_date2j(year, 1, 4);
  day0 = ty_j2day(day4 - 1);
  /* the first week has a Thursday, or the day is in the last year's */
  if (dayn < day4 - day0) {
    day4 = ty_date2j(year - 1, 1, 4);
    day0 = ty_j2day(day4 - 1);
  }
  result = (dayn - (day4 - day0)) / 7 + 1;
  /* the last days of a year may be in the first week of the next */
  if (result >= 52) {
    day4 = ty_date2j(year + 1, 1, 4);
    day0 = ty_j2day(day4 - 1);
    if (dayn >= day4 - day0) {
      result = (dayn - (day4 - day0)) / 7 + 1;
    }
  }
  return (int)result;
}

/* The year of the ISO week of a date. */
int ty_date2isoyear(int year, int mon, int mday) {
  double result;
  int day0, day4, dayn;

  dayn = ty_date2j(year, mon, mday);
  day4 = ty_date2j(year, 1, 4);
  day0 = ty_j2day(day4 - 1);
  if (dayn < day4 - day0) {
    day4 = ty_date2j(year - 1, 1, 4);
    day0 = ty_j2day(day4 - 1);
    year--;
  }
  result = (dayn - (day4 - day0)) / 7 + 1;
  if (result >= 52) {
    day4 = ty_date2j(year + 1, 1, 4);
    day0 = ty_j2day(day4 - 1);
    if (dayn >= day4 - day0) {
      year++;
    }
  }
  return year;
}

/* The day of the ISO year of a date. */
int ty_date2isoyearday(int year, int mon, int mday) {
  return ty_date2j(year, mon, mday) -
         ty_isoweek2j(ty_date2isoyear(year, mon, mday), 1) + 1;
}

/*
 * date_trunc: break the timestamp down, cut the fields at the unit by falling
 * through the cases from the largest, and put it together again.
 */
int ty_timestamp_trunc(int code, int64_t timestamp, int64_t *out) {
  enum {
    WEEK = 22,
    MONTH = 23,
    QUARTER = 24,
    YEAR = 25,
    DECADE = 26,
    CENTURY = 27,
    MILLENNIUM = 28,
    MILLISEC = 29,
    MICROSEC = 30,
    SECOND = 18,
    MINUTE = 19,
    HOUR = 20,
    DAY = 21
  };
  ty_tm_t tm;
  int32_t fsec;

  if (!ty_timestamp_finite(timestamp)) {
    switch (code) {
    case WEEK:
    case MILLENNIUM:
    case CENTURY:
    case DECADE:
    case YEAR:
    case QUARTER:
    case MONTH:
    case DAY:
    case HOUR:
    case MINUTE:
    case SECOND:
    case MILLISEC:
    case MICROSEC:
      *out = timestamp;
      return 0;
    default:
      return ty_internal::set_error(TY_ERR_INVALID, "unit not supported");
    }
  }
  if (ty_timestamp_to_tm(timestamp, &tm, &fsec) != 0) {
    return ts_range();
  }
  if (ty_tm_trunc(code, &tm, &fsec) != 0) {
    return -1;
  }
  if (ty_tm_to_timestamp(&tm, fsec, out) != 0) {
    return ts_range();
  }
  return 0;
}

/*
 * The cut of broken-down time at a unit: what ty_timestamp_trunc does to the
 * fields. An unsupported unit is TY_ERR_INVALID.
 */
int ty_tm_trunc(int code, ty_tm_t *ptm, int32_t *pfsec) {
  enum {
    WEEK = 22,
    MONTH = 23,
    QUARTER = 24,
    YEAR = 25,
    DECADE = 26,
    CENTURY = 27,
    MILLENNIUM = 28,
    MILLISEC = 29,
    MICROSEC = 30,
    SECOND = 18,
    MINUTE = 19,
    HOUR = 20,
    DAY = 21
  };
  ty_tm_t &tm = *ptm;
  int32_t &fsec = *pfsec;

  switch (code) {
  case WEEK: {
    int woy = ty_date2isoweek(tm.year, tm.mon, tm.mday);

    /* week 52 or 53 of January is of the year before, week 1 of December of the
     * next */
    if (woy >= 52 && tm.mon == 1) {
      --tm.year;
    }
    if (woy <= 1 && tm.mon == 12) {
      ++tm.year;
    }
    int y = tm.year, m, d;

    ty_isoweek2date(woy, &y, &m, &d);
    tm.year = y;
    tm.mon = m;
    tm.mday = d;
    tm.hour = tm.min = tm.sec = 0;
    fsec = 0;
    break;
  }
  case MILLENNIUM:
  case CENTURY:
  case DECADE:
  case YEAR:
  case QUARTER:
  case MONTH:
  case DAY:
  case HOUR:
  case MINUTE:
  case SECOND:
    /* each case cuts its field, and goes on to the smaller ones */
    if (code == MILLENNIUM) {
      tm.year = tm.year > 0 ? ((tm.year + 999) / 1000) * 1000 - 999
                            : -((999 - (tm.year - 1)) / 1000) * 1000 + 1;
    }
    if (code == MILLENNIUM || code == CENTURY) {
      tm.year = tm.year > 0 ? ((tm.year + 99) / 100) * 100 - 99
                            : -((99 - (tm.year - 1)) / 100) * 100 + 1;
    }
    if (code == DECADE) {
      tm.year =
          tm.year > 0 ? (tm.year / 10) * 10 : -((8 - (tm.year - 1)) / 10) * 10;
    }
    if (code == MILLENNIUM || code == CENTURY || code == DECADE ||
        code == YEAR) {
      tm.mon = 1;
    }
    if (code == MILLENNIUM || code == CENTURY || code == DECADE ||
        code == YEAR || code == QUARTER) {
      tm.mon = (3 * ((tm.mon - 1) / 3)) + 1;
    }
    if (code == MILLENNIUM || code == CENTURY || code == DECADE ||
        code == YEAR || code == QUARTER || code == MONTH) {
      tm.mday = 1;
    }
    if (code != HOUR && code != MINUTE && code != SECOND) {
      tm.hour = 0;
    }
    if (code != MINUTE && code != SECOND) {
      tm.min = 0;
    }
    if (code != SECOND) {
      tm.sec = 0;
    }
    fsec = 0;
    break;
  case MILLISEC:
    fsec = (fsec / 1000) * 1000;
    break;
  case MICROSEC:
    break;
  default:
    return ty_internal::set_error(TY_ERR_INVALID, "unit not supported");
  }
  return 0;
}

/* date_bin: which error it is (the hot path is in the header). */
int ty_timestamp_bin_error(const ty_interval_t *stride, int64_t timestamp,
                           int64_t origin) {
  int64_t stride_usecs, tm_diff, tm_modulo, tm_delta, result;

  if (!ty_timestamp_finite(timestamp)) {
    return 0;
  }
  if (!ty_timestamp_finite(origin)) {
    return ty_internal::set_error(TY_ERR_RANGE, "origin out of range");
  }
  if (ty_interval_not_finite(stride)) {
    return ty_internal::set_error(
        TY_ERR_RANGE, "timestamps cannot be binned into infinite intervals");
  }
  if (stride->month != 0) {
    return ty_internal::set_error(TY_ERR_INVALID,
                                  "timestamps cannot be binned into intervals "
                                  "containing months or years");
  }
  if (__builtin_mul_overflow((int64_t)stride->day, TY_USECS_PER_HOUR * 24,
                             &stride_usecs) ||
      __builtin_add_overflow(stride_usecs, stride->time, &stride_usecs)) {
    return iv_range();
  }
  if (stride_usecs <= 0) {
    return ty_internal::set_error(TY_ERR_RANGE,
                                  "stride must be greater than zero");
  }
  if (__builtin_sub_overflow(timestamp, origin, &tm_diff)) {
    return iv_range();
  }
  tm_modulo = tm_diff % stride_usecs;
  tm_delta = tm_diff - tm_modulo;
  result = origin + tm_delta;
  /* round toward minus infinity: this can overflow */
  if (tm_modulo < 0) {
    if (__builtin_sub_overflow(result, stride_usecs, &result) ||
        !ty_timestamp_valid(result)) {
      return ts_range();
    }
  }
  /* the hot path would not have called: no error found */
  return 0;
}

/*
 * The interval of broken-down fields (itm2interval of PostgreSQL): the months
 * of the years must fit in 32 bits, and the time in 64.
 */
static int itm_to_interval(int64_t year, int64_t mon, int64_t mday,
                           int64_t hour, int64_t min, int64_t sec, int64_t usec,
                           ty_interval_t *result) {
  int64_t months = year * 12 + mon, time;

  if (months > INT32_MAX || months < INT32_MIN) {
    return iv_range();
  }
  if (__builtin_mul_overflow(hour, TY_USECS_PER_HOUR, &time) ||
      __builtin_add_overflow(time, min * TY_USECS_PER_MINUTE, &time) ||
      __builtin_add_overflow(time, sec * TY_USECS_PER_SEC, &time) ||
      __builtin_add_overflow(time, usec, &time)) {
    return iv_range();
  }
  result->month = (int32_t)months;
  result->day = (int32_t)mday;
  result->time = time;
  if (ty_interval_not_finite(result)) {
    return iv_range();
  }
  return 0;
}

/*
 * age: the fields of the two timestamps subtracted, then the borrows made
 * from the smallest to the largest; the larger is first, and the sign put
 * back at the end.
 */
int ty_timestamp_age(int64_t dt1, int64_t dt2, ty_interval_t *result) {
  ty_tm_t tm1, tm2;
  int32_t fsec1, fsec2;

  if (!ty_timestamp_finite(dt1) || !ty_timestamp_finite(dt2)) {
    return ty_timestamp_diff(dt1, dt2, result);
  }
  if (ty_timestamp_to_tm(dt1, &tm1, &fsec1) != 0 ||
      ty_timestamp_to_tm(dt2, &tm2, &fsec2) != 0) {
    return ts_range();
  }
  return ty_age_of_fields(&tm1, fsec1, &tm2, fsec2, dt1 < dt2, result);
}

/*
 * The symbolic difference of two broken-down times (age of two timestamps),
 * from the fields of the first and of the second; neg says that the first is
 * the earlier, which the caller knows from the instants.
 */
int ty_age_of_fields(const ty_tm_t *ptm1, int32_t fsec1, const ty_tm_t *ptm2,
                     int32_t fsec2, int neg_in, ty_interval_t *result) {
  const ty_tm_t &tm1 = *ptm1, &tm2 = *ptm2;
  int64_t usec = fsec1 - fsec2, hour = tm1.hour - tm2.hour;
  int sec = tm1.sec - tm2.sec, min = tm1.min - tm2.min;
  int mday = tm1.mday - tm2.mday, mon = tm1.mon - tm2.mon,
      year = tm1.year - tm2.year;
  bool neg = neg_in != 0;

  if (neg) {
    usec = -usec;
    sec = -sec;
    min = -min;
    hour = -hour;
    mday = -mday;
    mon = -mon;
    year = -year;
  }
  while (usec < 0) {
    usec += TY_USECS_PER_SEC;
    sec--;
  }
  while (sec < 0) {
    sec += 60;
    min--;
  }
  while (min < 0) {
    min += 60;
    hour--;
  }
  while (hour < 0) {
    hour += 24;
    mday--;
  }
  while (mday < 0) {
    if (neg) {
      mday += ty_days_in_month(tm1.year, tm1.mon);
    } else {
      mday += ty_days_in_month(tm2.year, tm2.mon);
    }
    mon--;
  }
  while (mon < 0) {
    mon += 12;
    year--;
  }
  if (neg) {
    usec = -usec;
    sec = -sec;
    min = -min;
    hour = -hour;
    mday = -mday;
    mon = -mon;
    year = -year;
  }
  return itm_to_interval(year, mon, mday, hour, min, sec, usec, result);
}

/* The hash of a timetz: the time and the zone hashed, folded by xor. */
uint64_t ty_timetz_hash(const ty_timetz_t *a, uint64_t seed) {
  return ty_int64_hash(a->time, seed) ^ ty_hash_u32((uint32_t)a->zone, seed);
}

/* RANGE offsets of a time. */
int ty_time_in_range(int64_t val, int64_t base, const ty_interval_t *offset,
                     int sub, int less, int *r) {
  return ty_int_in_range(val, base, offset->time, sub, less, r);
}

/* RANGE offsets of a timetz: the sum has the zone of base. */
int ty_timetz_in_range(const ty_timetz_t *val, const ty_timetz_t *base,
                       const ty_interval_t *offset, int sub, int less, int *r) {
  ty_timetz_t sum;

  if (offset->time < 0) {
    return ty_internal::set_error(TY_ERR_INVALID,
                                  "invalid preceding or following size");
  }
  if (sub) {
    sum.time = base->time - offset->time; /* cannot overflow */
  } else if (__builtin_add_overflow(base->time, offset->time, &sum.time)) {
    *r = less;
    return 0;
  }
  sum.zone = base->zone;
  /*
   * The order of ty_timetz_cmp, in 128 bits: the sum may be as far from a time
   * of day as an int64 allows, and then 64 bits would wrap.
   */
  __int128 t1 = (__int128)val->time + (__int128)val->zone * 1000000;
  __int128 t2 = (__int128)sum.time + (__int128)sum.zone * 1000000;
  int c = t1 != t2 ? (t1 > t2 ? 1 : -1)
                   : (val->zone > sum.zone) - (val->zone < sum.zone);

  *r = less ? c <= 0 : c >= 0;
  return 0;
}

/*
 * date_trunc of an interval: the fields cut at the unit by falling through
 * the cases from the largest. A week is no unit of an interval, since months
 * usually have fractional weeks.
 */
int ty_interval_trunc(int code, const ty_interval_t *iv, ty_interval_t *out) {
  enum {
    WEEK = 22,
    MONTH = 23,
    QUARTER = 24,
    YEAR = 25,
    DECADE = 26,
    CENTURY = 27,
    MILLENNIUM = 28,
    MILLISEC = 29,
    MICROSEC = 30,
    SECOND = 18,
    MINUTE = 19,
    HOUR = 20,
    DAY = 21
  };
  ty_itm_t tm;

  if (ty_interval_not_finite(iv)) {
    switch (code) {
    case MILLENNIUM:
    case CENTURY:
    case DECADE:
    case YEAR:
    case QUARTER:
    case MONTH:
    case DAY:
    case HOUR:
    case MINUTE:
    case SECOND:
    case MILLISEC:
    case MICROSEC:
      *out = *iv;
      return 0;
    default:
      return ty_internal::set_error(TY_ERR_INVALID, "unit not supported");
    }
  }
  ty_interval_to_itm(iv, &tm);
  int64_t year = tm.year, mon = tm.mon, mday = tm.mday, hour = tm.hour,
          min = tm.min, sec = tm.sec, usec = tm.usec;

  switch (code) {
  /* each case cuts its field, and falls through to the smaller ones */
  case MILLENNIUM:
    year = (year / 1000) * 1000;
    [[fallthrough]];
  case CENTURY:
    year = (year / 100) * 100;
    [[fallthrough]];
  case DECADE:
    year = (year / 10) * 10;
    [[fallthrough]];
  case YEAR:
    mon = 0;
    [[fallthrough]];
  case QUARTER:
    mon = 3 * (mon / 3);
    [[fallthrough]];
  case MONTH:
    mday = 0;
    [[fallthrough]];
  case DAY:
    hour = 0;
    [[fallthrough]];
  case HOUR:
    min = 0;
    [[fallthrough]];
  case MINUTE:
    sec = 0;
    [[fallthrough]];
  case SECOND:
    usec = 0;
    break;
  case MILLISEC:
    usec = (usec / 1000) * 1000;
    break;
  case MICROSEC:
    break;
  default:
    return ty_internal::set_error(TY_ERR_INVALID, "unit not supported");
  }
  return itm_to_interval(year, mon, mday, hour, min, sec, usec, out);
}

namespace {

/* The order of int64 for overlaps. */
struct i64_order_t {
  static bool gt(const int64_t *a, const int64_t *b) { return *a > *b; }
  static bool lt(const int64_t *a, const int64_t *b) { return *a < *b; }
};

/* The order of timetz for overlaps. */
struct timetz_order_t {
  static bool gt(const ty_timetz_t *a, const ty_timetz_t *b) {
    return ty_timetz_cmp(a, b) > 0;
  }
  static bool lt(const ty_timetz_t *a, const ty_timetz_t *b) {
    return ty_timetz_cmp(a, b) < 0;
  }
};

/*
 * OVERLAPS by the SQL standard, which must give a result that is not null for
 * some inputs with nulls.
 * 1. If both ends of a period are null, the result is null.
 * 2. If one end is null, take the other as the start. Else take the lesser as
 *    the start.
 * 3. Compare the starts, and then the end that decides.
 */
template <class T, class Order>
int overlaps(const T *ts1, const T *te1, const T *ts2, const T *te2) {
  if (ts1 == nullptr) {
    if (te1 == nullptr) {
      return 2;
    }
    ts1 = te1;
    te1 = nullptr;
  } else if (te1 != nullptr && Order::gt(ts1, te1)) {
    std::swap(ts1, te1);
  }
  if (ts2 == nullptr) {
    if (te2 == nullptr) {
      return 2;
    }
    ts2 = te2;
    te2 = nullptr;
  } else if (te2 != nullptr && Order::gt(ts2, te2)) {
    std::swap(ts2, te2);
  }
  if (Order::gt(ts1, ts2)) {
    /* ts1 < te2 or te1 < te2; with a null end, the nulls matter */
    if (te2 == nullptr) {
      return 2;
    }
    if (Order::lt(ts1, te2)) {
      return 1;
    }
    if (te1 == nullptr) {
      return 2;
    }
    return 0;
  }
  if (Order::lt(ts1, ts2)) {
    /* ts2 < te1 or te2 < te1 */
    if (te1 == nullptr) {
      return 2;
    }
    if (Order::lt(ts2, te1)) {
      return 1;
    }
    if (te2 == nullptr) {
      return 2;
    }
    return 0;
  }
  /* the starts are equal: they overlap, if both periods have an end */
  if (te1 == nullptr || te2 == nullptr) {
    return 2;
  }
  return 1;
}

} // namespace

/* OVERLAPS of timestamps, and of times. */
int ty_overlaps_i64(const int64_t *ts1, const int64_t *te1, const int64_t *ts2,
                    const int64_t *te2) {
  return overlaps<int64_t, i64_order_t>(ts1, te1, ts2, te2);
}

/* OVERLAPS of timetz. */
int ty_overlaps_timetz(const ty_timetz_t *ts1, const ty_timetz_t *te1,
                       const ty_timetz_t *ts2, const ty_timetz_t *te2) {
  return overlaps<ty_timetz_t, timetz_order_t>(ts1, te1, ts2, te2);
}

/* The error of a date past the range of a timestamp. */
int ty_date_to_ts_error(void) {
  return ty_internal::set_error(TY_ERR_RANGE,
                                "date out of range for timestamp");
}

/* timestamp to date. */
int ty_timestamp_to_date(int64_t ts, int32_t *out) {
  ty_tm_t tm;
  int32_t fsec;

  if (ts == TY_TIMESTAMP_NOBEGIN) {
    *out = TY_DATE_NOBEGIN;
  } else if (ts == TY_TIMESTAMP_NOEND) {
    *out = TY_DATE_NOEND;
  } else {
    if (ty_timestamp_to_tm(ts, &tm, &fsec) != 0) {
      return ts_range();
    }
    *out = ty_date2j(tm.year, tm.mon, tm.mday) - TY_POSTGRES_EPOCH_JDATE;
  }
  return 0;
}

/* timestamp to time of day: the timestamp must be finite. */
int ty_timestamp_to_time(int64_t ts, int64_t *out) {
  ty_tm_t tm;
  int32_t fsec;

  if (ty_timestamp_to_tm(ts, &tm, &fsec) != 0) {
    return ts_range();
  }
  *out = ((((int64_t)tm.hour * 60 + tm.min) * 60) + tm.sec) * TY_USECS_PER_SEC +
         fsec;
  return 0;
}

/* date and time to timestamp. */
int ty_datetime_to_timestamp(int32_t date, int64_t time, int64_t *out) {
  int64_t r;

  if (ty_date_to_timestamp(date, &r, nullptr) != 0) {
    return -1;
  }
  if (ty_timestamp_finite(r)) {
    r += time;
    if (!ty_timestamp_valid(r)) {
      return ts_range();
    }
  }
  *out = r;
  return 0;
}

/* The error of an interval to a time of day: the interval is infinite. */
int ty_interval_to_time_error(void) {
  return ty_internal::set_error(TY_ERR_RANGE,
                                "cannot convert infinite interval to time");
}

/* The error of to_timestamp: a NaN, or a number out of range. */
int ty_timestamptz_from_epoch_error(double seconds) {
  char msg[64];

  if (__builtin_isnan(seconds)) {
    return ty_err_invalid("timestamp cannot be NaN");
  }
  snprintf(msg, sizeof msg, "timestamp out of range: \"%g\"", seconds);
  return ty_internal::set_error(TY_ERR_RANGE, msg);
}

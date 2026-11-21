/*
 * Copyright (c) 2026 CK Tan
 * SPDX-License-Identifier: MIT
 */

/*
 * datetime.cc -- the calendar and the arithmetic of date, time and timestamp.
 */
#include "ty_datetime.h"

#include <limits.h>
#include <math.h>
#include <stdio.h>

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
int ty_date_make(int32_t year, int32_t mon, int32_t mday, int32_t *out) {
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
    /* PostgreSQL prints the year after the BC change */
    snprintf(msg, sizeof msg, "date field value out of range: %d-%02d-%02d",
             bc && ty != y ? ty : y, mon, mday);
    return ty_internal::set_error(TY_ERR_INVALID, msg);
  }
  if (!valid_julian(ty, mon)) {
    snprintf(msg, sizeof msg, "date out of range: %d-%02d-%02d", ty, mon, mday);
    return ty_internal::set_error(TY_ERR_RANGE, msg);
  }
  int32_t date = ty_date2j(ty, mon, mday) - TY_POSTGRES_EPOCH_JDATE;

  if (!ty_date_valid(date)) {
    snprintf(msg, sizeof msg, "date out of range: %d-%02d-%02d", ty, mon, mday);
    return ty_internal::set_error(TY_ERR_RANGE, msg);
  }
  *out = date;
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

namespace {

/* t reduced to within a day, as PostgreSQL does it. */
int64_t wrap_day(int64_t t) {
  t -= t / (TY_USECS_PER_HOUR * 24) * (TY_USECS_PER_HOUR * 24);
  if (t < 0) {
    t += TY_USECS_PER_HOUR * 24;
  }
  return t;
}

} // namespace

/* time + interval: only the microseconds count; an infinite one is an error. */
int ty_time_add_interval(int64_t t, const ty_interval_t *span, int64_t *out) {
  if (ty_interval_not_finite(span)) {
    return ty_internal::set_error(TY_ERR_RANGE,
                                  "cannot add infinite interval to time");
  }
  /* the sum wraps in two's complement, as PostgreSQL's does */
  *out = wrap_day((int64_t)((uint64_t)t + (uint64_t)span->time));
  return 0;
}

/* time - interval. */
int ty_time_sub_interval(int64_t t, const ty_interval_t *span, int64_t *out) {
  if (ty_interval_not_finite(span)) {
    return ty_internal::set_error(
        TY_ERR_RANGE, "cannot subtract infinite interval from time");
  }
  *out = wrap_day((int64_t)((uint64_t)t - (uint64_t)span->time));
  return 0;
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

/*
 * Copyright (c) 2026 CK Tan
 * SPDX-License-Identifier: MIT
 */

/*
 * interval.cc -- the arithmetic of interval.
 */
#include "ty_interval.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "ty_float.h"

#include "error.h"

namespace {

const int64_t TY_USECS_PER_HOUR_I = INT64_C(3600000000);
const double k_secs_per_day = 86400.0;
const double k_usecs_per_sec = 1000000.0;

/* The error of a result that does not fit. */
int out_of_range() {
  return ty_internal::set_error(TY_ERR_RANGE, "interval out of range");
}

/* Round to a microsecond, as PostgreSQL's TSROUND does. */
inline double tsround(double j) { return rint(j * 1000000.0) / 1000000.0; }

/* Whether a double fits an int32 or an int64, as PostgreSQL's macros say. */
inline bool fits_i32(double d) {
  return d >= -2147483648.0 && d < 2147483648.0;
}
inline bool fits_i64(double d) {
  return d >= -9223372036854775808.0 && d < 9223372036854775808.0;
}

/* t / u and the remainder, toward zero. */
inline void tmodulo(int64_t &t, int64_t &q, int64_t u) {
  q = t / u;
  if (q != 0) {
    t -= q * u;
  }
}

} // namespace

/* -a. */
int ty_interval_neg(const ty_interval_t *a, ty_interval_t *r) {
  if (ty_interval_is_nobegin(a)) {
    ty_interval_set_noend(r);
  } else if (ty_interval_is_noend(a)) {
    ty_interval_set_nobegin(r);
  } else if (__builtin_sub_overflow((int64_t)0, a->time, &r->time) ||
             __builtin_sub_overflow((int32_t)0, a->day, &r->day) ||
             __builtin_sub_overflow((int32_t)0, a->month, &r->month) ||
             ty_interval_not_finite(r)) {
    return out_of_range();
  }
  return 0;
}

/* a + b. */
int ty_interval_add(const ty_interval_t *a, const ty_interval_t *b,
                    ty_interval_t *r) {
  if (ty_interval_is_nobegin(a)) {
    if (ty_interval_is_noend(b)) {
      return out_of_range();
    }
    ty_interval_set_nobegin(r);
  } else if (ty_interval_is_noend(a)) {
    if (ty_interval_is_nobegin(b)) {
      return out_of_range();
    }
    ty_interval_set_noend(r);
  } else if (ty_interval_not_finite(b)) {
    *r = *b;
  } else if (__builtin_add_overflow(a->month, b->month, &r->month) ||
             __builtin_add_overflow(a->day, b->day, &r->day) ||
             __builtin_add_overflow(a->time, b->time, &r->time) ||
             ty_interval_not_finite(r)) {
    return out_of_range();
  }
  return 0;
}

/* a - b. */
int ty_interval_sub(const ty_interval_t *a, const ty_interval_t *b,
                    ty_interval_t *r) {
  if (ty_interval_is_nobegin(a)) {
    if (ty_interval_is_nobegin(b)) {
      return out_of_range();
    }
    ty_interval_set_nobegin(r);
  } else if (ty_interval_is_noend(a)) {
    if (ty_interval_is_noend(b)) {
      return out_of_range();
    }
    ty_interval_set_noend(r);
  } else if (ty_interval_is_nobegin(b)) {
    ty_interval_set_noend(r);
  } else if (ty_interval_is_noend(b)) {
    ty_interval_set_nobegin(r);
  } else if (__builtin_sub_overflow(a->month, b->month, &r->month) ||
             __builtin_sub_overflow(a->day, b->day, &r->day) ||
             __builtin_sub_overflow(a->time, b->time, &r->time) ||
             ty_interval_not_finite(r)) {
    return out_of_range();
  }
  return 0;
}

namespace {

/*
 * The shared part of mul and div. The month and the day are scaled and cut to
 * integers; their fractions go to days (a month is 30 days) and to seconds (a
 * day is 86400); the seconds go to the microseconds with the scaled time.
 * month_scaled(x), day_scaled(x) and time_scaled(x) are x * f or x / f.
 */
template <typename Scale>
int scale_interval(const ty_interval_t *a, Scale scale, ty_interval_t *r) {
  double month_remainder_days, sec_remainder, result_double;
  int32_t orig_month = a->month, orig_day = a->day;

  result_double = scale((double)a->month);
  if (std::isnan(result_double) || !fits_i32(result_double)) {
    return out_of_range();
  }
  r->month = (int32_t)result_double;
  result_double = scale((double)a->day);
  if (std::isnan(result_double) || !fits_i32(result_double)) {
    return out_of_range();
  }
  r->day = (int32_t)result_double;
  /* the fraction of a month is days, and of those, the fraction is seconds */
  month_remainder_days = (scale((double)orig_month) - r->month) * 30;
  month_remainder_days = tsround(month_remainder_days);
  sec_remainder = (scale((double)orig_day) - r->day + month_remainder_days -
                   (int)month_remainder_days) *
                  k_secs_per_day;
  sec_remainder = tsround(sec_remainder);
  /* more than a day of seconds goes to the days */
  if (fabs(sec_remainder) >= k_secs_per_day) {
    if (__builtin_add_overflow(r->day, (int)(sec_remainder / k_secs_per_day),
                               &r->day)) {
      return out_of_range();
    }
    sec_remainder -= (int)(sec_remainder / k_secs_per_day) * k_secs_per_day;
  }
  if (__builtin_add_overflow(r->day, (int32_t)month_remainder_days, &r->day)) {
    return out_of_range();
  }
  result_double =
      rint(scale((double)a->time) + sec_remainder * k_usecs_per_sec);
  if (std::isnan(result_double) || !fits_i64(result_double)) {
    return out_of_range();
  }
  r->time = (int64_t)result_double;
  if (ty_interval_not_finite(r)) {
    return out_of_range();
  }
  return 0;
}

} // namespace

/* a * f. */
int ty_interval_mul(const ty_interval_t *a, double f, ty_interval_t *r) {
  if (std::isnan(f)) {
    return out_of_range();
  }
  if (ty_interval_not_finite(a)) {
    if (f == 0.0) {
      return out_of_range();
    }
    if (f < 0.0) {
      return ty_interval_neg(a, r);
    }
    *r = *a;
    return 0;
  }
  if (std::isinf(f)) {
    int isign = ty_interval_sign(a);

    if (isign == 0) {
      return out_of_range();
    }
    if (f * isign < 0) {
      ty_interval_set_nobegin(r);
    } else {
      ty_interval_set_noend(r);
    }
    return 0;
  }
  return scale_interval(a, [f](double x) { return x * f; }, r);
}

/* a / f. */
int ty_interval_div(const ty_interval_t *a, double f, ty_interval_t *r) {
  if (f == 0.0) {
    return ty_err_divzero();
  }
  if (std::isnan(f)) {
    return out_of_range();
  }
  if (ty_interval_not_finite(a)) {
    if (std::isinf(f)) {
      return out_of_range();
    }
    if (f < 0.0) {
      return ty_interval_neg(a, r);
    }
    *r = *a;
    return 0;
  }
  return scale_interval(a, [f](double x) { return x / f; }, r);
}

/* justify_hours: whole days out of the time, and the signs made equal. */
int ty_interval_justify_hours(const ty_interval_t *a, ty_interval_t *r) {
  int64_t wholeday;

  *r = *a;
  if (ty_interval_not_finite(r)) {
    return 0;
  }
  tmodulo(r->time, wholeday, TY_USECS_PER_DAY);
  /* a time has at most 106,751,991 days: they fit an int32 */
  if (__builtin_add_overflow(r->day, (int32_t)wholeday, &r->day)) {
    return out_of_range();
  }
  if (r->day > 0 && r->time < 0) {
    r->time += TY_USECS_PER_DAY;
    r->day--;
  } else if (r->day < 0 && r->time > 0) {
    r->time -= TY_USECS_PER_DAY;
    r->day++;
  }
  return 0;
}

/* justify_days: whole months out of the days, and the signs made equal. */
int ty_interval_justify_days(const ty_interval_t *a, ty_interval_t *r) {
  *r = *a;
  if (ty_interval_not_finite(r)) {
    return 0;
  }
  int32_t wholemonth = r->day / TY_DAYS_PER_MONTH;

  r->day -= wholemonth * TY_DAYS_PER_MONTH;
  if (__builtin_add_overflow(r->month, wholemonth, &r->month)) {
    return out_of_range();
  }
  if (r->month > 0 && r->day < 0) {
    r->day += TY_DAYS_PER_MONTH;
    r->month--;
  } else if (r->month < 0 && r->day > 0) {
    r->day -= TY_DAYS_PER_MONTH;
    r->month++;
  }
  return 0;
}

/* justify_interval: hours to days, days to months, the signs equal. */
int ty_interval_justify(const ty_interval_t *a, ty_interval_t *r) {
  int64_t wholeday;
  int32_t wholemonth;

  *r = *a;
  if (ty_interval_not_finite(r)) {
    return 0;
  }
  /* with days and a time of one sign, take the months of the days first */
  if ((r->day > 0 && r->time > 0) || (r->day < 0 && r->time < 0)) {
    wholemonth = r->day / TY_DAYS_PER_MONTH;
    r->day -= wholemonth * TY_DAYS_PER_MONTH;
    if (__builtin_add_overflow(r->month, wholemonth, &r->month)) {
      return out_of_range();
    }
  }
  tmodulo(r->time, wholeday, TY_USECS_PER_DAY);
  /* as in PostgreSQL, the days added are cut to an int32 */
  int64_t days = (int64_t)r->day + wholeday;

  r->day = (int32_t)days;
  wholemonth = r->day / TY_DAYS_PER_MONTH;
  r->day -= wholemonth * TY_DAYS_PER_MONTH;
  if (__builtin_add_overflow(r->month, wholemonth, &r->month)) {
    return out_of_range();
  }
  if (r->month > 0 && (r->day < 0 || (r->day == 0 && r->time < 0))) {
    r->day += TY_DAYS_PER_MONTH;
    r->month--;
  } else if (r->month < 0 && (r->day > 0 || (r->day == 0 && r->time > 0))) {
    r->day -= TY_DAYS_PER_MONTH;
    r->month++;
  }
  if (r->day > 0 && r->time < 0) {
    r->time += TY_USECS_PER_DAY;
    r->day--;
  } else if (r->day < 0 && r->time > 0) {
    r->time -= TY_USECS_PER_DAY;
    r->day++;
  }
  return 0;
}

/* Make an interval fit its typmod. */
int ty_interval_adjust_typmod(ty_interval_t *a, int32_t typmod) {
  static const int64_t scales[7] = {1000000, 100000, 10000, 1000, 100, 10, 1};
  static const int64_t offsets[7] = {500000, 50000, 5000, 500, 50, 5, 0};
  if (ty_interval_not_finite(a) || typmod < 0) {
    return 0;
  }
  int range = (typmod >> 16) & 0x7FFF;
  int precision = typmod & 0xFFFF;
  /* the fields: year 2, month 1, day 3, hour 10, minute 11, second 12 */
  const int Y = 1 << 2, M = 1 << 1, D = 1 << 3, H = 1 << 10, MI = 1 << 11,
            S = 1 << 12;

  if (range == TY_INTERVAL_FULL_RANGE) {
    /* nothing to cut */
  } else if (range == Y) {
    a->month = (a->month / 12) * 12;
    a->day = 0;
    a->time = 0;
  } else if (range == M || range == (Y | M)) {
    a->day = 0;
    a->time = 0;
  } else if (range == D) {
    a->time = 0;
  } else if (range == H || range == (D | H)) {
    a->time = (a->time / TY_USECS_PER_HOUR_I) * TY_USECS_PER_HOUR_I;
  } else if (range == MI || range == (D | H | MI) || range == (H | MI)) {
    a->time = (a->time / 60000000) * 60000000;
  } else if (range == S || range == (D | H | MI | S) || range == (H | MI | S) ||
             range == (MI | S)) {
    /* the sub-second rounding is below */
  } else {
    char msg[64];

    snprintf(msg, sizeof msg, "unrecognized interval typmod: %d", typmod);
    return ty_internal::set_error(TY_ERR_INVALID, msg);
  }
  if (precision != TY_INTERVAL_FULL_PRECISION) {
    if (precision < 0 || precision > 6) {
      char msg[96];

      snprintf(msg, sizeof msg,
               "interval(%d) precision must be between %d and %d", precision, 0,
               6);
      return ty_internal::set_error(TY_ERR_INVALID, msg);
    }
    if (a->time >= 0) {
      if (__builtin_add_overflow(a->time, offsets[precision], &a->time)) {
        return out_of_range();
      }
    } else if (__builtin_sub_overflow(a->time, offsets[precision], &a->time)) {
      return out_of_range();
    }
    a->time -= a->time % scales[precision];
  }
  return 0;
}

/* make_interval. */
int ty_interval_make(int32_t years, int32_t months, int32_t weeks, int32_t days,
                     int32_t hours, int32_t mins, double secs,
                     ty_interval_t *out) {
  ty_interval_t r;

  if (std::isinf(secs) || std::isnan(secs)) {
    return out_of_range();
  }
  if (__builtin_mul_overflow(years, 12, &r.month) ||
      __builtin_add_overflow(r.month, months, &r.month)) {
    return out_of_range();
  }
  if (__builtin_mul_overflow(weeks, 7, &r.day) ||
      __builtin_add_overflow(r.day, days, &r.day)) {
    return out_of_range();
  }
  r.time = (int64_t)hours * TY_USECS_PER_HOUR_I + (int64_t)mins * 60000000;
  double us;

  if (ty_fp64_mul(secs, 1000000.0, &us) != 0) {
    return -1;
  }
  us = rint(us);
  if (!(us >= -9223372036854775808.0 && us < 9223372036854775808.0) ||
      __builtin_add_overflow(r.time, (int64_t)us, &r.time)) {
    return out_of_range();
  }
  if (ty_interval_not_finite(&r)) {
    return out_of_range();
  }
  *out = r;
  return 0;
}

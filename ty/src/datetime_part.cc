/*
 * extract and date_part of timestamp, date, time and interval: the part of a
 * value that a unit names. See ty_datetime.h.
 */
#include <stdint.h>

#include "ty_datetime.h"
#include "error.h"

namespace {

/* The Unix epoch as a timestamp, and as a Julian day. */
const int64_t k_epoch_ts = -INT64_C(946684800000000);
const int k_unix_epoch_jdate = 2440588;
const int k_postgres_epoch_jdate = 2451545;

/* A unit the type has no part for. */
int unsupported() {
  return ty_internal::set_error(TY_ERR_INVALID, "unit not supported");
}

/* part is an integer. */
int set_int(ty_part_t *o, int64_t v) {
  o->kind = TY_PART_INT;
  o->scale = 0;
  o->num = v;
  o->num2 = 0;
  o->f = (double)v;
  return 0;
}

/* part is num / 10^scale, with f as float8 computes it. */
int set_scaled(ty_part_t *o, int64_t v, int scale, double f) {
  o->kind = TY_PART_SCALED;
  o->scale = scale;
  o->num = v;
  o->num2 = 0;
  o->f = f;
  return 0;
}

/* The year, decade, century and millennium of a year of the calendar. */
int64_t year_of(int year) { return year > 0 ? year : year - 1; }
int64_t decade_of(int year) {
  return year >= 0 ? year / 10 : -((8 - (year - 1)) / 10);
}
int64_t century_of(int year) {
  return year > 0 ? (year + 99) / 100 : -((99 - (year - 1)) / 100);
}
int64_t millennium_of(int year) {
  return year > 0 ? (year + 999) / 1000 : -((999 - (year - 1)) / 1000);
}

/* The parts that a date and a timestamp share, from year, month and day. */
inline __attribute__((always_inline)) bool
date_unit(int code, int year, int mon, int mday, int64_t *v) {
  switch (code) {
  case TY_UNIT_DAY:
    *v = mday;
    return true;
  case TY_UNIT_MONTH:
    *v = mon;
    return true;
  case TY_UNIT_QUARTER:
    *v = (mon - 1) / 3 + 1;
    return true;
  case TY_UNIT_WEEK:
    *v = ty_date2isoweek(year, mon, mday);
    return true;
  case TY_UNIT_YEAR:
    *v = year_of(year);
    return true;
  case TY_UNIT_DECADE:
    *v = decade_of(year);
    return true;
  case TY_UNIT_CENTURY:
    *v = century_of(year);
    return true;
  case TY_UNIT_MILLENNIUM:
    *v = millennium_of(year);
    return true;
  case TY_UNIT_ISOYEAR:
    *v = ty_date2isoyear(year, mon, mday);
    if (*v <= 0) {
      *v -= 1; /* there is no year 0 */
    }
    return true;
  case TY_UNIT_DOW:
  case TY_UNIT_ISODOW:
    *v = ty_j2day(ty_date2j(year, mon, mday));
    if (code == TY_UNIT_ISODOW && *v == 0) {
      *v = 7;
    }
    return true;
  case TY_UNIT_DOY:
    *v = ty_date2j(year, mon, mday) - ty_date2j(year, 1, 1) + 1;
    return true;
  default:
    return false;
  }
}

} // namespace

/* The part of a timestamp. */
int ty_timestamp_part(int reserved, int code, int64_t ts, ty_part_t *out) {
  ty_tm_t tm;
  int32_t fsec;
  int64_t v;

  if (reserved) {
    if (code != TY_UNIT_EPOCH) {
      return unsupported();
    }
    if (ts < INT64_MAX + k_epoch_ts) {
      return set_scaled(out, ts - k_epoch_ts, 6, (ts - k_epoch_ts) / 1000000.0);
    }
    /* ts - epoch does not fit in 64 bits: the integer seconds and the rest */
    __int128 d = (__int128)ts - k_epoch_ts;

    out->kind = TY_PART_SUM;
    out->scale = 6;
    out->num2 = (int64_t)(d / 1000000);
    out->num = (int64_t)(d % 1000000);
    out->f = ((double)ts - k_epoch_ts) / 1000000.0;
    return 0;
  }
  if (ty_timestamp_to_tm(ts, &tm, &fsec) != 0) {
    return ty_internal::set_error(TY_ERR_RANGE, "timestamp out of range");
  }
  switch (code) {
  case TY_UNIT_MICROSEC:
    return set_int(out, tm.sec * INT64_C(1000000) + fsec);
  case TY_UNIT_MILLISEC:
    return set_scaled(out, tm.sec * INT64_C(1000000) + fsec, 3,
                      tm.sec * 1000.0 + fsec / 1000.0);
  case TY_UNIT_SECOND:
    return set_scaled(out, tm.sec * INT64_C(1000000) + fsec, 6,
                      tm.sec + fsec / 1000000.0);
  case TY_UNIT_MINUTE:
    return set_int(out, tm.min);
  case TY_UNIT_HOUR:
    return set_int(out, tm.hour);
  case TY_UNIT_JULIAN: {
    int64_t day = ty_date2j(tm.year, tm.mon, tm.mday);

    out->kind = TY_PART_JULIAN;
    out->scale = 0;
    out->num2 = day;
    out->num =
        (((tm.hour * 60 + tm.min) * 60 + tm.sec) * INT64_C(1000000)) + fsec;
    out->f =
        day + ((((tm.hour * 60) + tm.min) * 60) + tm.sec + (fsec / 1000000.0)) /
                  (double)86400;
    return 0;
  }
  default:
    if (!date_unit(code, tm.year, tm.mon, tm.mday, &v)) {
      return unsupported();
    }
    return set_int(out, v);
  }
}

/* The part of a date: the value is exact; f is the same number. */
int ty_date_part(int reserved, int code, int32_t date, ty_part_t *out) {
  int year, mon, mday;
  int64_t v;

  if (reserved) {
    if (code != TY_UNIT_EPOCH) {
      return unsupported();
    }
    return set_int(
        out,
        ((int64_t)date + k_postgres_epoch_jdate - k_unix_epoch_jdate) * 86400);
  }
  ty_j2date(date + k_postgres_epoch_jdate, &year, &mon, &mday);
  if (code == TY_UNIT_JULIAN) {
    return set_int(out, (int64_t)date + k_postgres_epoch_jdate);
  }
  if (!date_unit(code, year, mon, mday, &v)) {
    return unsupported();
  }
  return set_int(out, v);
}

/* The part of a time of day. */
int ty_time_part(int reserved, int code, int64_t t, ty_part_t *out) {
  int64_t hour = t / INT64_C(3600000000);
  int64_t rem = t - hour * INT64_C(3600000000);
  int min = (int)(rem / 60000000);
  rem -= (int64_t)min * 60000000;
  int sec = (int)(rem / 1000000);
  int32_t fsec = (int32_t)(rem - (int64_t)sec * 1000000);

  if (reserved) {
    if (code != TY_UNIT_EPOCH) {
      return unsupported();
    }
    return set_scaled(out, t, 6, t / 1000000.0);
  }
  switch (code) {
  case TY_UNIT_MICROSEC:
    return set_int(out, sec * INT64_C(1000000) + fsec);
  case TY_UNIT_MILLISEC:
    return set_scaled(out, sec * INT64_C(1000000) + fsec, 3,
                      sec * 1000.0 + fsec / 1000.0);
  case TY_UNIT_SECOND:
    return set_scaled(out, sec * INT64_C(1000000) + fsec, 6,
                      sec + fsec / 1000000.0);
  case TY_UNIT_MINUTE:
    return set_int(out, min);
  case TY_UNIT_HOUR:
    return set_int(out, hour);
  default:
    return unsupported();
  }
}

/* The part of a finite interval. */
int ty_interval_part(int reserved, int code, const ty_interval_t *iv,
                     ty_part_t *out) {
  if (reserved) {
    /* only epoch is a reserved word of an interval: the caller checked */
    int64_t secs = ((int64_t)1461 * (iv->month / 12) +
                    (int64_t)120 * (iv->month % 12) + (int64_t)4 * iv->day) *
                   (86400 / 4);
    int64_t val;
    double f = iv->time / 1000000.0;

    f += ((double)365.25 * 86400) * (iv->month / 12);
    f += ((double)30 * 86400) * (iv->month % 12);
    f += ((double)86400) * iv->day;
    if (!__builtin_mul_overflow(secs, (int64_t)1000000, &val) &&
        !__builtin_add_overflow(val, iv->time, &val)) {
      return set_scaled(out, val, 6, f);
    }
    out->kind = TY_PART_SUM;
    out->scale = 6;
    out->num = iv->time;
    out->num2 = secs;
    out->f = f;
    return 0;
  }
  /*
   * The fields of the interval, as ty_interval_to_itm cuts them, but only the
   * ones the unit needs.
   */
  switch (code) {
  case TY_UNIT_MICROSEC:
    return set_int(out, iv->time % 60000000);
  case TY_UNIT_MILLISEC:
    return set_scaled(out, iv->time % 60000000, 3,
                      ((iv->time % 60000000) / 1000000) * 1000.0 +
                          (iv->time % 1000000) / 1000.0);
  case TY_UNIT_SECOND:
    return set_scaled(out, iv->time % 60000000, 6,
                      ((iv->time % 60000000) / 1000000) +
                          (iv->time % 1000000) / 1000000.0);
  case TY_UNIT_MINUTE:
    return set_int(out, (iv->time % INT64_C(3600000000)) / 60000000);
  case TY_UNIT_HOUR:
    return set_int(out, iv->time / INT64_C(3600000000));
  case TY_UNIT_DAY:
    return set_int(out, iv->day);
  case TY_UNIT_WEEK:
    return set_int(out, iv->day / 7);
  case TY_UNIT_MONTH:
    return set_int(out, iv->month % 12);
  case TY_UNIT_QUARTER:
    /* the quarter of a negative interval counts down */
    return set_int(out, iv->month >= 0
                            ? ((iv->month % 12) / 3) + 1
                            : -(((-(int64_t)iv->month % 12) / 3) + 1));
  case TY_UNIT_YEAR:
    return set_int(out, iv->month / 12);
  case TY_UNIT_DECADE:
    return set_int(out, iv->month / 12 / 10);
  case TY_UNIT_CENTURY:
    return set_int(out, iv->month / 12 / 100);
  case TY_UNIT_MILLENNIUM:
    return set_int(out, iv->month / 12 / 1000);
  default:
    return unsupported();
  }
}

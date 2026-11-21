/*
 * Copyright (c) 2026 CK Tan
 * SPDX-License-Identifier: MIT
 */

/*
 * datetime_text.cc -- the text of date, time, timestamp and interval, by the
 * settings the host passes in. The cases follow PostgreSQL's datetime.c.
 */
#include "ty_datetime.h"

#include <stdlib.h>
#include <string.h>

namespace {

const char *const k_days[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
const char *const k_months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

const int k_max_precision = 6;
const size_t k_max_tz_len = 10;

/* Write value in at least minwidth digits, zero-padded. */
char *zeropad(char *p, uint64_t value, int minwidth) {
  /* the usual fields: two digits, or four for a year */
  if (minwidth == 2 && value < 100) {
    p[0] = (char)('0' + value / 10);
    p[1] = (char)('0' + value % 10);
    return p + 2;
  }
  if (minwidth == 4 && value < 10000) {
    p[0] = (char)('0' + value / 1000);
    p[1] = (char)('0' + value / 100 % 10);
    p[2] = (char)('0' + value / 10 % 10);
    p[3] = (char)('0' + value % 10);
    return p + 4;
  }
  char tmp[24];
  int n = 0;

  do {
    tmp[n++] = (char)('0' + value % 10);
    value /= 10;
  } while (value != 0);
  while (n < minwidth) {
    tmp[n++] = '0';
  }
  while (n > 0) {
    *p++ = tmp[--n];
  }
  return p;
}

/* Write value with no padding. */
char *plain(char *p, uint64_t value) { return zeropad(p, value, 1); }

/* Write a signed number. */
char *put_signed(char *p, int64_t v) {
  if (v < 0) {
    *p++ = '-';
  }
  return plain(p, v < 0 ? (uint64_t)0 - (uint64_t)v : (uint64_t)v);
}

/* Write a string. */
char *put_str(char *p, const char *s) {
  size_t n = strlen(s);

  memcpy(p, s, n);
  return p + n;
}

/* The absolute value of an int64, as unsigned (the smallest has one). */
uint64_t abs64(int64_t v) {
  return v < 0 ? (uint64_t)0 - (uint64_t)v : (uint64_t)v;
}

/* The year as printed: a BC year counts from 1. */
uint64_t shown_year(int32_t y) {
  return y > 0 ? (uint64_t)y : (uint64_t)(-((int64_t)y - 1));
}

/*
 * The seconds and the fraction of a second, with the trailing zeros of the
 * fraction cut. No NUL is written. fillzeros pads the seconds to 2 digits.
 */
char *append_seconds(char *cp, int sec, int32_t fsec, int precision,
                     bool fillzeros) {
  cp = fillzeros ? zeropad(cp, (uint64_t)abs(sec), 2)
                 : plain(cp, (uint64_t)abs(sec));
  if (fsec == 0) {
    return cp;
  }
  int32_t value = abs(fsec);
  char *end = &cp[precision + 1];
  bool gotnonzero = false;

  *cp++ = '.';
  /* the fraction is built from its last digit: skip zeros until a digit */
  while (precision--) {
    int32_t oldval = value;

    value /= 10;
    int32_t remainder = oldval - value * 10;

    if (remainder) {
      gotnonzero = true;
    }
    if (gotnonzero) {
      cp[precision] = (char)('0' + remainder);
    } else {
      end = &cp[precision];
    }
  }
  /* a value that did not fit the precision is written in full */
  if (value) {
    return plain(cp, (uint64_t)abs(fsec));
  }
  return end;
}

/* The zone as +hh, +hh:mm or +hh:mm:ss; the sign is that of -tz. */
char *encode_timezone(char *str, int tz, int style) {
  int sec = abs(tz);
  int min = sec / 60;

  sec -= min * 60;
  int hour = min / 60;

  min -= hour * 60;
  *str++ = (tz <= 0 ? '+' : '-');
  if (sec != 0) {
    str = zeropad(str, (uint64_t)hour, 2);
    *str++ = ':';
    str = zeropad(str, (uint64_t)min, 2);
    *str++ = ':';
    str = zeropad(str, (uint64_t)sec, 2);
  } else if (min != 0 || style == TY_DATES_XSD) {
    str = zeropad(str, (uint64_t)hour, 2);
    *str++ = ':';
    str = zeropad(str, (uint64_t)min, 2);
  } else {
    str = zeropad(str, (uint64_t)hour, 2);
  }
  return str;
}

/* A zone name cut to its longest, with a space before it. */
char *zone_name(char *str, const char *tzn) {
  *str++ = ' ';
  size_t n = strnlen(tzn, k_max_tz_len);

  memcpy(str, tzn, n);
  return str + n;
}

/* The date of a style that has numbers and one separator. */
char *numeric_date(char *str, const ty_tm_t *tm, int style, int order) {
  switch (style) {
  case TY_DATES_ISO:
  case TY_DATES_XSD:
    str = zeropad(str, shown_year(tm->year), 4);
    *str++ = '-';
    str = zeropad(str, (uint64_t)tm->mon, 2);
    *str++ = '-';
    str = zeropad(str, (uint64_t)tm->mday, 2);
    break;
  case TY_DATES_SQL:
    if (order == TY_DATEORDER_DMY) {
      str = zeropad(str, (uint64_t)tm->mday, 2);
      *str++ = '/';
      str = zeropad(str, (uint64_t)tm->mon, 2);
    } else {
      str = zeropad(str, (uint64_t)tm->mon, 2);
      *str++ = '/';
      str = zeropad(str, (uint64_t)tm->mday, 2);
    }
    *str++ = '/';
    str = zeropad(str, shown_year(tm->year), 4);
    break;
  case TY_DATES_GERMAN:
    str = zeropad(str, (uint64_t)tm->mday, 2);
    *str++ = '.';
    str = zeropad(str, (uint64_t)tm->mon, 2);
    *str++ = '.';
    str = zeropad(str, shown_year(tm->year), 4);
    break;
  default: /* postgres */
    if (order == TY_DATEORDER_DMY) {
      str = zeropad(str, (uint64_t)tm->mday, 2);
      *str++ = '-';
      str = zeropad(str, (uint64_t)tm->mon, 2);
    } else {
      str = zeropad(str, (uint64_t)tm->mon, 2);
      *str++ = '-';
      str = zeropad(str, (uint64_t)tm->mday, 2);
    }
    *str++ = '-';
    str = zeropad(str, shown_year(tm->year), 4);
    break;
  }
  return str;
}

/* " BC" after a year that is 0 or below. */
char *bc_suffix(char *str, const ty_tm_t *tm) {
  if (tm->year <= 0) {
    memcpy(str, " BC", 3);
    str += 3;
  }
  return str;
}

} // namespace

/* A date. */
size_t ty_date_format(const ty_tm_t *tm, int style, int order, char *out) {
  char *str = numeric_date(out, tm, style, order);

  str = bc_suffix(str, tm);
  *str = '\0';
  return (size_t)(str - out);
}

/* A time of day, and the zone if wanted. */
size_t ty_time_format(const ty_tm_t *tm, int32_t fsec, int print_tz, int tz,
                      int style, char *out) {
  char *str = out;

  str = zeropad(str, (uint64_t)tm->hour, 2);
  *str++ = ':';
  str = zeropad(str, (uint64_t)tm->min, 2);
  *str++ = ':';
  str = append_seconds(str, tm->sec, fsec, k_max_precision, true);
  if (print_tz) {
    str = encode_timezone(str, tz, style);
  }
  *str = '\0';
  return (size_t)(str - out);
}

/*
 * A timestamp.
 * 1. With no known zone (isdst below 0) no zone is written.
 * 2. ISO and XSD: the date and the time, with a space or a T between.
 * 3. SQL and German: the date, a space, the time, the zone name or offset.
 * 4. Postgres: the weekday, the month and day in the order of the date order,
 *    the time, the year, the zone.
 * 5. A year of 0 or below is followed by " BC".
 */
size_t ty_timestamp_format(const ty_tm_t *tm, int32_t fsec, int print_tz,
                           int tz, const char *tzn, int isdst, int style,
                           int order, char *out) {
  char *str = out;

  if (isdst < 0) {
    print_tz = 0;
  }
  switch (style) {
  case TY_DATES_ISO:
  case TY_DATES_XSD:
    str = numeric_date(str, tm, style, order);
    *str++ = (style == TY_DATES_ISO) ? ' ' : 'T';
    str = zeropad(str, (uint64_t)tm->hour, 2);
    *str++ = ':';
    str = zeropad(str, (uint64_t)tm->min, 2);
    *str++ = ':';
    str = append_seconds(str, tm->sec, fsec, k_max_precision, true);
    if (print_tz) {
      str = encode_timezone(str, tz, style);
    }
    break;
  case TY_DATES_SQL:
  case TY_DATES_GERMAN:
    str = numeric_date(str, tm, style, order);
    *str++ = ' ';
    str = zeropad(str, (uint64_t)tm->hour, 2);
    *str++ = ':';
    str = zeropad(str, (uint64_t)tm->min, 2);
    *str++ = ':';
    str = append_seconds(str, tm->sec, fsec, k_max_precision, true);
    if (print_tz) {
      str = tzn ? zone_name(str, tzn) : encode_timezone(str, tz, style);
    }
    break;
  default: { /* postgres */
    int wday = ty_j2day(ty_date2j(tm->year, tm->mon, tm->mday));

    memcpy(str, k_days[wday], 3);
    str += 3;
    *str++ = ' ';
    if (order == TY_DATEORDER_DMY) {
      str = zeropad(str, (uint64_t)tm->mday, 2);
      *str++ = ' ';
      memcpy(str, k_months[tm->mon - 1], 3);
      str += 3;
    } else {
      memcpy(str, k_months[tm->mon - 1], 3);
      str += 3;
      *str++ = ' ';
      str = zeropad(str, (uint64_t)tm->mday, 2);
    }
    *str++ = ' ';
    str = zeropad(str, (uint64_t)tm->hour, 2);
    *str++ = ':';
    str = zeropad(str, (uint64_t)tm->min, 2);
    *str++ = ':';
    str = append_seconds(str, tm->sec, fsec, k_max_precision, true);
    *str++ = ' ';
    str = zeropad(str, shown_year(tm->year), 4);
    if (print_tz) {
      if (tzn) {
        str = zone_name(str, tzn);
      } else {
        /* a zone with no name: the offset, with a space before it */
        *str++ = ' ';
        str = encode_timezone(str, tz, style);
      }
    }
    break;
  }
  }
  str = bc_suffix(str, tm);
  *str = '\0';
  return (size_t)(str - out);
}

/* An interval as its parts. */
void ty_interval_to_itm(const ty_interval_t *span, ty_itm_t *itm) {
  int64_t time = span->time, tfrac;

  itm->year = span->month / 12;
  itm->mon = span->month % 12;
  itm->mday = span->day;
  tfrac = time / TY_USECS_PER_HOUR;
  time -= tfrac * TY_USECS_PER_HOUR;
  itm->hour = tfrac;
  tfrac = time / TY_USECS_PER_MINUTE;
  time -= tfrac * TY_USECS_PER_MINUTE;
  itm->min = (int32_t)tfrac;
  tfrac = time / TY_USECS_PER_SEC;
  time -= tfrac * TY_USECS_PER_SEC;
  itm->sec = (int32_t)tfrac;
  itm->usec = (int32_t)time;
}

namespace {

/* Append "<value><units>" for ISO 8601, if value is not zero. */
char *iso_part(char *cp, int64_t value, char units) {
  if (value == 0) {
    return cp;
  }
  cp = put_signed(cp, value);
  *cp++ = units;
  return cp;
}

/* Append " <value> <units>" for the postgres style. */
char *postgres_part(char *cp, int64_t value, const char *units, bool *is_zero,
                    bool *is_before) {
  if (value == 0) {
    return cp;
  }
  if (!*is_zero) {
    *cp++ = ' ';
  }
  if (*is_before && value > 0) {
    *cp++ = '+';
  }
  cp = put_signed(cp, value);
  *cp++ = ' ';
  cp = put_str(cp, units);
  if (value != 1) {
    *cp++ = 's';
  }
  /* each nonzero field sets is_before for (only) the next one */
  *is_before = (value < 0);
  *is_zero = false;
  return cp;
}

/* Append " <value> <units>" for the postgres verbose style. */
char *verbose_part(char *cp, int64_t value, const char *units, bool *is_zero,
                   bool *is_before) {
  if (value == 0) {
    return cp;
  }
  /* the first nonzero field sets is_before */
  if (*is_zero) {
    *is_before = (value < 0);
    value = (int64_t)abs64(value);
  } else if (*is_before) {
    value = -value;
  }
  *cp++ = ' ';
  cp = put_signed(cp, value);
  *cp++ = ' ';
  cp = put_str(cp, units);
  if (value != 1) {
    *cp++ = 's';
  }
  *is_zero = false;
  return cp;
}

} // namespace

/*
 * An interval in one of the four styles: sql standard, iso 8601, postgres,
 * and postgres verbose.
 */
size_t ty_interval_format(const ty_itm_t *itm, int style, char *out) {
  char *cp = out;
  int year = itm->year, mon = itm->mon, min = itm->min, sec = itm->sec;
  int64_t mday = itm->mday, hour = itm->hour;
  int fsec = itm->usec;
  bool is_before = false, is_zero = true;

  switch (style) {
  case TY_INTSTYLE_SQL_STANDARD: {
    bool has_negative = year < 0 || mon < 0 || mday < 0 || hour < 0 ||
                        min < 0 || sec < 0 || fsec < 0;
    bool has_positive = year > 0 || mon > 0 || mday > 0 || hour > 0 ||
                        min > 0 || sec > 0 || fsec > 0;
    bool has_year_month = year != 0 || mon != 0;
    bool has_day_time =
        mday != 0 || hour != 0 || min != 0 || sec != 0 || fsec != 0;
    bool has_day = mday != 0;
    bool sql_standard_value =
        !(has_negative && has_positive) && !(has_year_month && has_day_time);

    /* one sign for the whole value, if the signs do not mix */
    if (has_negative && sql_standard_value) {
      *cp++ = '-';
      year = -year;
      mon = -mon;
      mday = -mday;
      hour = -hour;
      min = -min;
      sec = -sec;
      fsec = -fsec;
    }
    if (!has_negative && !has_positive) {
      *cp++ = '0';
    } else if (!sql_standard_value) {
      /* mixed signs: write every sign, to be sure of the meaning */
      *cp++ = (year < 0 || mon < 0) ? '-' : '+';
      cp = plain(cp, (uint64_t)abs(year));
      *cp++ = '-';
      cp = plain(cp, (uint64_t)abs(mon));
      *cp++ = ' ';
      *cp++ = (mday < 0) ? '-' : '+';
      cp = plain(cp, abs64(mday));
      *cp++ = ' ';
      *cp++ = (hour < 0 || min < 0 || sec < 0 || fsec < 0) ? '-' : '+';
      cp = plain(cp, abs64(hour));
      *cp++ = ':';
      cp = zeropad(cp, (uint64_t)abs(min), 2);
      *cp++ = ':';
      cp = append_seconds(cp, sec, fsec, k_max_precision, true);
    } else if (has_year_month) {
      cp = put_signed(cp, year);
      *cp++ = '-';
      cp = put_signed(cp, mon);
    } else if (has_day) {
      cp = put_signed(cp, mday);
      *cp++ = ' ';
      cp = put_signed(cp, hour);
      *cp++ = ':';
      cp = zeropad(cp, (uint64_t)min, 2);
      *cp++ = ':';
      cp = append_seconds(cp, sec, fsec, k_max_precision, true);
    } else {
      cp = put_signed(cp, hour);
      *cp++ = ':';
      cp = zeropad(cp, (uint64_t)min, 2);
      *cp++ = ':';
      cp = append_seconds(cp, sec, fsec, k_max_precision, true);
    }
    break;
  }
  case TY_INTSTYLE_ISO_8601:
    if (year == 0 && mon == 0 && mday == 0 && hour == 0 && min == 0 &&
        sec == 0 && fsec == 0) {
      cp = put_str(cp, "PT0S");
      break;
    }
    *cp++ = 'P';
    cp = iso_part(cp, year, 'Y');
    cp = iso_part(cp, mon, 'M');
    cp = iso_part(cp, mday, 'D');
    if (hour != 0 || min != 0 || sec != 0 || fsec != 0) {
      *cp++ = 'T';
    }
    cp = iso_part(cp, hour, 'H');
    cp = iso_part(cp, min, 'M');
    if (sec != 0 || fsec != 0) {
      if (sec < 0 || fsec < 0) {
        *cp++ = '-';
      }
      cp = append_seconds(cp, sec, fsec, k_max_precision, false);
      *cp++ = 'S';
    }
    break;
  case TY_INTSTYLE_POSTGRES:
    cp = postgres_part(cp, year, "year", &is_zero, &is_before);
    /* "mon", not "month": it has always been so */
    cp = postgres_part(cp, mon, "mon", &is_zero, &is_before);
    cp = postgres_part(cp, mday, "day", &is_zero, &is_before);
    if (is_zero || hour != 0 || min != 0 || sec != 0 || fsec != 0) {
      bool minus = (hour < 0 || min < 0 || sec < 0 || fsec < 0);

      if (!is_zero) {
        *cp++ = ' ';
      }
      if (minus) {
        *cp++ = '-';
      } else if (is_before) {
        *cp++ = '+';
      }
      cp = zeropad(cp, abs64(hour), 2);
      *cp++ = ':';
      cp = zeropad(cp, (uint64_t)abs(min), 2);
      *cp++ = ':';
      cp = append_seconds(cp, sec, fsec, k_max_precision, true);
    }
    break;
  case TY_INTSTYLE_POSTGRES_VERBOSE:
  default:
    *cp++ = '@';
    cp = verbose_part(cp, year, "year", &is_zero, &is_before);
    cp = verbose_part(cp, mon, "mon", &is_zero, &is_before);
    cp = verbose_part(cp, mday, "day", &is_zero, &is_before);
    cp = verbose_part(cp, hour, "hour", &is_zero, &is_before);
    cp = verbose_part(cp, min, "min", &is_zero, &is_before);
    if (sec != 0 || fsec != 0) {
      *cp++ = ' ';
      if (sec < 0 || (sec == 0 && fsec < 0)) {
        if (is_zero) {
          is_before = true;
        } else if (!is_before) {
          *cp++ = '-';
        }
      } else if (is_before) {
        *cp++ = '-';
      }
      cp = append_seconds(cp, sec, fsec, k_max_precision, false);
      /* "ago" says the sign, so the seconds are absolute */
      cp = put_str(cp, " sec");
      if (abs(sec) != 1 || fsec != 0) {
        *cp++ = 's';
      }
      is_zero = false;
    }
    if (is_zero) {
      cp = put_str(cp, " 0");
    }
    if (is_before) {
      cp = put_str(cp, " ago");
    }
    break;
  }
  *cp = '\0';
  return (size_t)(cp - out);
}

/*
 * Copyright (c) 2026 CK Tan
 * SPDX-License-Identifier: MIT
 */

/*
 * ty_interval.h -- the arithmetic of interval: a span of months, days and
 * microseconds, which are kept apart because a month has no fixed number of
 * days and a day no fixed number of hours. The cases and the errors follow
 * PostgreSQL's timestamp.c.
 *
 * Two values are special: minus infinity and plus infinity, which have the
 * smallest and the largest value in all three fields. A result that has all
 * three fields at an end, with no infinity as an operand, is out of range.
 *
 * A call that can fail returns 0, or -1 with the thread's error set:
 * TY_ERR_RANGE for "interval out of range", TY_ERR_DIVZERO for a division by
 * zero.
 */
#ifndef TY_INTERVAL_H
#define TY_INTERVAL_H

#include <stddef.h>
#include <stdint.h>

#include "ty.h"
#include "ty_int.h"

/* PostgreSQL's layout, so that a stored value is a ty_interval_t. */
typedef struct ty_interval_t {
  int64_t time; /* microseconds */
  int32_t day;
  int32_t month;
} ty_interval_t;

#define TY_USECS_PER_DAY INT64_C(86400000000)
#define TY_DAYS_PER_MONTH 30

static inline int ty_interval_is_nobegin(const ty_interval_t *a) {
  return a->month == INT32_MIN && a->day == INT32_MIN && a->time == INT64_MIN;
}
static inline int ty_interval_is_noend(const ty_interval_t *a) {
  return a->month == INT32_MAX && a->day == INT32_MAX && a->time == INT64_MAX;
}
static inline int ty_interval_not_finite(const ty_interval_t *a) {
  return ty_interval_is_nobegin(a) || ty_interval_is_noend(a);
}
static inline void ty_interval_set_nobegin(ty_interval_t *a) {
  a->time = INT64_MIN;
  a->day = INT32_MIN;
  a->month = INT32_MIN;
}
static inline void ty_interval_set_noend(ty_interval_t *a) {
  a->time = INT64_MAX;
  a->day = INT32_MAX;
  a->month = INT32_MAX;
}

/*
 * The span as one number of microseconds, a month being 30 days and a day
 * 24 hours: 128 bits, so that it cannot overflow. The order of two
 * intervals is the order of these.
 */
static inline __int128 ty_interval_span(const ty_interval_t *a) {
  /* the days fit an int64, so this is one 64 by 64 bit product */
  int64_t days = (int64_t)a->month * TY_DAYS_PER_MONTH + a->day;

  return (__int128)a->time + (__int128)days * TY_USECS_PER_DAY;
}

/* Negative, zero or positive. */
static inline int ty_interval_cmp(const ty_interval_t *a,
                                  const ty_interval_t *b) {
  __int128 x = ty_interval_span(a), y = ty_interval_span(b);

  return x < y ? -1 : x > y ? 1 : 0;
}

/* The sign of the span: -1, 0 or 1. */
static inline int ty_interval_sign(const ty_interval_t *a) {
  __int128 x = ty_interval_span(a);

  return x < 0 ? -1 : x > 0 ? 1 : 0;
}

/* The hash of the low 64 bits of the span, as the hash of an int64. */
static inline uint64_t ty_interval_hash_extended(const ty_interval_t *a,
                                                 uint64_t seed) {
  return ty_int64_hash((int64_t)ty_interval_span(a), seed);
}
static inline uint32_t ty_interval_hash(const ty_interval_t *a) {
  return (uint32_t)ty_int64_hash((int64_t)ty_interval_span(a), 0);
}

/* -a, a + b, a - b. Infinity with its opposite is TY_ERR_RANGE. */
int ty_interval_neg(const ty_interval_t *a, ty_interval_t *r);
int ty_interval_add(const ty_interval_t *a, const ty_interval_t *b,
                    ty_interval_t *r);
int ty_interval_sub(const ty_interval_t *a, const ty_interval_t *b,
                    ty_interval_t *r);

/*
 * a * f and a / f, with PostgreSQL's carry of the fraction of a month into
 * days and of a day into microseconds. A factor that is NaN, or a result that
 * does not fit, is TY_ERR_RANGE; a division by 0 is TY_ERR_DIVZERO.
 */
int ty_interval_mul(const ty_interval_t *a, double f, ty_interval_t *r);
int ty_interval_div(const ty_interval_t *a, double f, ty_interval_t *r);

/*
 * justify_hours: 24 hours are a day. justify_days: 30 days are a month.
 * justify_interval: both, with the signs of the parts made the same.
 * TY_ERR_RANGE if a month or a day does not fit.
 */
int ty_interval_justify_hours(const ty_interval_t *a, ty_interval_t *r);
int ty_interval_justify_days(const ty_interval_t *a, ty_interval_t *r);
int ty_interval_justify(const ty_interval_t *a, ty_interval_t *r);

/*
 * Read the text of an interval, as interval_in does, up to the typmod:
 * range is the interval typmod's range (0x7FFF for none), intervalstyle the
 * session's (TY_INTSTYLE_* of ty_datetime.h). On an error, -1, with
 * PostgreSQL's code for the error in *dterr: -1 bad format, -2 field
 * overflow, -3 field overflow with a hint, -4 interval overflow, and 1 for a
 * count of months that does not fit. The text is cut to its fields as the
 * host's date parser does it.
 */
int ty_interval_parse(const char *str, int range, int intervalstyle,
                      ty_interval_t *out, int *dterr);

/* The typmod of an interval: the range of fields, then the precision. */
#define TY_INTERVAL_FULL_RANGE 0x7FFF
#define TY_INTERVAL_FULL_PRECISION 0xFFFF
#define TY_INTERVAL_TYPMOD(precision, range) \
    ((((range) & 0x7FFF) << 16) | ((precision) & 0xFFFF))

/*
 * Make an interval fit its typmod: the fields to the right of the range are
 * zeroed, and the microseconds are rounded to the precision. A typmod of -1
 * and an infinite interval are left as they are. A precision that is not 0 to
 * 6 is TY_ERR_INVALID ("interval(%d) precision must be between 0 and 6");
 * rounding that overflows is TY_ERR_RANGE.
 */
int ty_interval_adjust_typmod(ty_interval_t *a, int32_t typmod);

/*
 * An interval from years, months, weeks, days, hours, minutes and seconds
 * (make_interval). TY_ERR_RANGE ("interval out of range") if a field
 * overflows or the seconds are not a number.
 */
int ty_interval_make(int32_t years, int32_t months, int32_t weeks, int32_t days,
                     int32_t hours, int32_t mins, double secs,
                     ty_interval_t *out);

#endif

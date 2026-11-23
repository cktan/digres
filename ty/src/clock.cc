/*
 * Copyright (c) 2026 CK Tan
 * SPDX-License-Identifier: MIT
 */

/*
 * clock.cc -- the clock of the transaction: a pointer, per thread, to the
 * start time that the host (sm) keeps (ty_set_xact_clock, ty_xact_start),
 * and the session's zone, with the time of the transaction in it.
 *
 * A thread that sets none has NULL here: so no thread_local needs a
 * constructor.
 */

#include <stdint.h>

#include "error.h"
#include "ty.h"
#include "ty_timezone.h"

/* The calling thread's pointer to the host's clock; NULL for none. */
static thread_local const int64_t *t_xact_clock = nullptr;

/* Set the calling thread's pointer to the host's clock. */
void ty_set_xact_clock(const int64_t *start) { t_xact_clock = start; }

/* The calling thread's pointer to the host's clock, or NULL. */
const int64_t *ty_xact_clock(void) { return t_xact_clock; }

/* The start time of the calling thread's transaction. */
int ty_xact_start(int64_t *out) {
  if (nullptr == t_xact_clock || INT64_MIN == *t_xact_clock) {
    return ty_internal::set_error(TY_ERR_INVALID,
                                  "the thread has no transaction start time");
  }
  *out = *t_xact_clock;
  return 0;
}

/* The calling thread's session zone; NULL for none. */
static thread_local const ty_tz_t *t_session_zone = nullptr;

/* Set the calling thread's session zone. */
void ty_set_session_zone(const ty_tz_t *tz) { t_session_zone = tz; }

/* The calling thread's session zone, or NULL. */
const ty_tz_t *ty_session_zone(void) { return t_session_zone; }

/*
 * The transaction's time in the session's zone, kept: the fields, the offset
 * and the date change only with the instant or the zone, and CURRENT_DATE
 * and the like ask for them for each row. The key is the instant and the
 * zone, which is a pointer: a zone has one address while it lasts.
 */
struct now_cache_t {
  int64_t ts = 0;
  const ty_tz_t *zone = nullptr; /* NULL: the cache is empty */
  ty_tm_t tm = {};
  int32_t fsec = 0;
  int west = 0;
  int32_t date = 0; /* the date of tm, in days since 2000-01-01 */
};
static thread_local now_cache_t t_now;

/* Fill the cache for the instant ts in the session's zone, if it is not. */
static int now_fill(int64_t ts) {
  if (t_now.zone == t_session_zone && t_now.ts == ts) {
    return 0;
  }
  now_cache_t c;
  if (ty_timestamptz_to_local(t_session_zone, ts, &c.tm, &c.fsec, &c.west,
                              nullptr, nullptr) != 0) {
    return -1;
  }
  if (ty_date_make(c.tm.year, c.tm.mon, c.tm.mday, &c.date) != 0) {
    return -1;
  }
  c.ts = ts;
  c.zone = t_session_zone;
  t_now = c;
  return 0;
}

/* The start of the transaction, with the cache filled for it. */
static int now_get() {
  int64_t ts;

  if (ty_xact_start(&ts) != 0) {
    return -1;
  }
  if (nullptr == t_session_zone) {
    return ty_internal::set_error(TY_ERR_INVALID,
                                  "the thread has no session time zone");
  }
  return now_fill(ts);
}

/* The transaction's start time as the fields of the session's zone. */
int ty_xact_now(ty_tm_t *tm, int32_t *fsec, int *west) {
  if (now_get() != 0) {
    return -1;
  }
  *tm = t_now.tm;
  *fsec = t_now.fsec;
  if (west != nullptr) {
    *west = t_now.west;
  }
  return 0;
}

/* The time of day of the cached time, in microseconds. */
static int64_t now_time_of_day() {
  return ((int64_t)t_now.tm.hour * 60 + t_now.tm.min) * 60 * TY_USECS_PER_SEC +
         (int64_t)t_now.tm.sec * TY_USECS_PER_SEC + t_now.fsec;
}

/* CURRENT_DATE: the date of the transaction in the session's zone. */
int ty_current_date(int32_t *out) {
  if (now_get() != 0) {
    return -1;
  }
  *out = t_now.date;
  return 0;
}

/* CURRENT_TIME, CURRENT_TIME(n): the time of day, with the zone's offset. */
int ty_current_time(int32_t typmod, ty_timetz_t *out) {
  if (now_get() != 0) {
    return -1;
  }
  out->time = now_time_of_day();
  out->zone = t_now.west;
  if (typmod >= 0) {
    ty_time_adjust_typmod(&out->time, typmod);
  }
  return 0;
}

/* LOCALTIME, LOCALTIME(n): the time of day in the session's zone. */
int ty_local_time(int32_t typmod, int64_t *out) {
  if (now_get() != 0) {
    return -1;
  }
  *out = now_time_of_day();
  if (typmod >= 0) {
    ty_time_adjust_typmod(out, typmod);
  }
  return 0;
}

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

/* The transaction's start time as the fields of the session's zone. */
int ty_xact_now(ty_tm_t *tm, int32_t *fsec, int *west) {
  int64_t ts;
  int w;

  if (ty_xact_start(&ts) != 0) {
    return -1;
  }
  if (nullptr == t_session_zone) {
    return ty_internal::set_error(TY_ERR_INVALID,
                                  "the thread has no session time zone");
  }
  if (ty_timestamptz_to_local(t_session_zone, ts, tm, fsec, &w, nullptr,
                              nullptr) != 0) {
    return -1;
  }
  if (west != nullptr) {
    *west = w;
  }
  return 0;
}

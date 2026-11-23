/*
 * Copyright (c) 2026 CK Tan
 * SPDX-License-Identifier: MIT
 */

/*
 * clock.cc -- the clock of the transaction: a pointer, per thread, to the
 * start time that the host (sm) keeps (ty_set_xact_clock, ty_xact_start).
 *
 * A thread that sets none has NULL here: so no thread_local needs a
 * constructor.
 */

#include <stdint.h>

#include "error.h"
#include "ty.h"

/* The calling thread's pointer to the host's clock; NULL for none. */
static thread_local const int64_t *t_xact_clock = nullptr;

/* Set the calling thread's pointer to the host's clock. */
void ty_set_xact_clock(const int64_t *start) { t_xact_clock = start; }

/* The start time of the calling thread's transaction. */
int ty_xact_start(int64_t *out) {
  if (nullptr == t_xact_clock || INT64_MIN == *t_xact_clock) {
    return ty_internal::set_error(TY_ERR_INVALID,
                                  "the thread has no transaction start time");
  }
  *out = *t_xact_clock;
  return 0;
}

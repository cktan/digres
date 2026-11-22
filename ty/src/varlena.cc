/*
 * Copyright (c) 2026 CK Tan
 * SPDX-License-Identifier: MIT
 */

/*
 * varlena.cc -- the cold part of making a varlena.
 */
#include "ty_varlena.h"

#include "error.h"

/* The error of a value that cannot be made: too long, or no memory. */
int ty_vl_new_error(size_t len) {
  if (len > TY_VARLENA_MAX_LEN) {
    return ty_internal::set_error(TY_ERR_RANGE, "requested length too large");
  }
  return ty_internal::set_error(TY_ERR_NOMEM, "out of memory");
}

/*
 * Copyright (c) 2026 CK Tan
 * SPDX-License-Identifier: MIT
 */

/*
 * error.cc -- the per-thread error state behind ty_errnum(),
 * ty_errmsg() and ty_errname().
 *
 * The message is copied into a thread-local buffer: nothing is
 * allocated, so an error can be recorded when memory has run out.
 */

#include "error.h"

#include <string.h>

namespace {

constexpr size_t errmsg_size = 256;

thread_local ty_errnum_t t_errnum = TY_OK;
thread_local char t_errmsg[errmsg_size] = "no error";

} // namespace

/* The calling thread's last error code. */
ty_errnum_t ty_errnum(void) { return t_errnum; }

/* The calling thread's last error message. */
const char *ty_errmsg(void) { return t_errmsg; }

/* The name of code, or "TY_ERR_UNKNOWN" for one not in the list. */
const char *ty_errname(ty_errnum_t code) {
  switch (code) {
  case TY_OK:
    return "TY_OK";
  case TY_ERR_INVALID:
    return "TY_ERR_INVALID";
  case TY_ERR_NOMEM:
    return "TY_ERR_NOMEM";
  case TY_ERR_EXCEPTION:
    return "TY_ERR_EXCEPTION";
  case TY_ERR_RANGE:
    return "TY_ERR_RANGE";
  case TY_ERR_DIVZERO:
    return "TY_ERR_DIVZERO";
  case TY_ERR_UNDERFLOW:
    return "TY_ERR_UNDERFLOW";
  }
  return "TY_ERR_UNKNOWN";
}

/* Record code and msg, cut to fit, as the thread's error. */
int ty_internal::set_error(ty_errnum_t code, const char *msg) {
  size_t n = strlen(msg);
  if (n > errmsg_size - 1) {
    n = errmsg_size - 1;
  }
  t_errnum = code;
  memcpy(t_errmsg, msg, n);
  t_errmsg[n] = '\0';
  return -1;
}

/* Record the range error that an inline operation found; returns -1. */
int ty_err_range(void) {
  return ty_internal::set_error(TY_ERR_RANGE, "result out of range");
}

/* Record the division by zero that an inline operation found; returns -1. */
int ty_err_divzero(void) {
  return ty_internal::set_error(TY_ERR_DIVZERO, "division by zero");
}

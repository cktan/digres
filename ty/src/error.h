/*
 * Copyright (c) 2026 CK Tan
 * SPDX-License-Identifier: MIT
 */

/*
 * error.h -- ty's own use of the per-thread error state.
 */
#ifndef TY_SRC_ERROR_H
#define TY_SRC_ERROR_H

#include "ty.h"

namespace ty_internal {

/* Record code and msg as the calling thread's error; returns -1. */
int set_error(ty_errnum_t code, const char *msg);

} // namespace ty_internal

#endif /* TY_SRC_ERROR_H */

/*
 * Copyright (c) 2026 CK Tan
 * SPDX-License-Identifier: MIT
 */

/*
 * log.h -- reporting through the caller's log hook
 * (sm1::set_log): sm0's helpers (sm0_log.h), brought into
 * namespace sm1_internal.
 */
#ifndef SM1_LOG_H
#define SM1_LOG_H

#include "sm0_log.h"

namespace sm1_internal {

using sm0::fatal;
using sm0::log_enabled;
using sm0::log_msg;

} // namespace sm1_internal

#endif /* SM1_LOG_H */

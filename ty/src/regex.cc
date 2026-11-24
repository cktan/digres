/*
 * Copyright (c) 2026 CK Tan
 * SPDX-License-Identifier: MIT
 */

/*
 * regex.cc -- the host's regular expression engine, and ~ through it
 * (ty_regex.h).
 */
#include <atomic>

#include "error.h"
#include "ty_regex.h"

namespace {

/* The process's engine, which any thread may read while one sets it. */
std::atomic<ty_regex_fn> g_engine{nullptr};

} // namespace

/* Set the engine for the whole process. */
void ty_set_regex(ty_regex_fn fn) {
  g_engine.store(fn, std::memory_order_release);
}

/* The engine set, or NULL. */
ty_regex_fn ty_regex(void) { return g_engine.load(std::memory_order_acquire); }

/*
 * Match through the engine.
 * 1. With no engine, TY_ERR_INVALID.
 * 2. Pass the bytes of both texts, the flags and the collation.
 * 3. An error of the engine is TY_ERR_INVALID, with its message.
 */
int ty_text_regex(const ty_varlena_t *t, const ty_varlena_t *pat, int flags,
                  uint32_t collation, int *matched) {
  // 1. With no engine, TY_ERR_INVALID.
  ty_regex_fn fn = ty_regex();

  if (nullptr == fn) {
    return ty_internal::set_error(TY_ERR_INVALID,
                                  "no regular expression engine is set");
  }

  // 2. Pass the bytes of both texts, the flags and the collation.
  char errmsg[256];

  errmsg[0] = '\0';
  if (0 == fn(ty_vl_data(pat), ty_vl_len(pat), ty_vl_data(t), ty_vl_len(t),
              flags, collation, matched, errmsg, sizeof errmsg)) {
    return 0;
  }

  // 3. An error of the engine is TY_ERR_INVALID, with its message.
  errmsg[sizeof errmsg - 1] = '\0';
  return ty_internal::set_error(
      TY_ERR_INVALID, errmsg[0] ? errmsg : "regular expression failed");
}

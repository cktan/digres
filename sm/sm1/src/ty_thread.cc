/*
 * Copyright (c) 2026 CK Tan
 * SPDX-License-Identifier: MIT
 */

/*
 * ty_thread.cc -- the alloc and free functions of ty on a thread of sm
 * (include/sm1_ty.h).
 */
#include "sm1_ty.h"

#include <memory>

#include "sm0_arena.h"
#include "ty.h"

namespace sm1 {
namespace {

/* The arena of this thread, made at its first call; freed at its end. */
thread_local std::unique_ptr<sm0::arena_t> t_arena;

/*
 * The alloc function of ty: size bytes of the thread's arena. A piece is
 * a multiple of 8 bytes, so the next one is aligned as ty requires; out of
 * memory aborts the process (sm0_arena.h), so it never returns NULL.
 */
void *arena_alloc(size_t size) {
  return t_arena->alloc((size + 7) & ~(size_t)7);
}

/* The free function of ty: the arena frees all its pieces at the end. */
void arena_free(void *) {}

} // namespace

/* Make the arena, and give ty's alloc and free functions to this thread. */
void ty_thread_init() {
  if (nullptr == t_arena) {
    t_arena = std::make_unique<sm0::arena_t>();
  }
  // ty keeps its functions per thread, so each thread sets its own.
  ty_set_alloc(arena_alloc, arena_free);
}

} // namespace sm1

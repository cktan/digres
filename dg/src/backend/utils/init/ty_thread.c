/*
 * Copyright (c) 2026 CK Tan
 * SPDX-License-Identifier: MIT
 */

/*-------------------------------------------------------------------------
 *
 * ty_thread.c
 *	  Give a thread of digres the alloc and free functions of ty.
 *
 * src/backend/utils/init/ty_thread.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "ty.h"
#include "utils/memutils.h"
#include "utils/ty_thread.h"

/*
 * The alloc function of ty on a thread of digres: palloc's, in the current
 * memory context. It may not throw (ty.h), so a request that palloc would
 * refuse with an error, one past MaxAllocSize, gets NULL, as does one the
 * context cannot meet, which palloc_extended() reports as NULL.
 */
static void *ty_alloc_fn_palloc(size_t size) {
  if (size > MaxAllocSize)
    return NULL;
  return palloc_extended(size, MCXT_ALLOC_NO_OOM);
}

/* The free function of ty on a thread of digres: pfree. */
static void ty_free_fn_pfree(void *ptr) { pfree(ptr); }

/*
 * Make what ty allocates on this thread come from palloc. ty keeps the
 * functions per thread, so every thread of digres calls this as it starts.
 */
void ty_thread_init(void) { ty_set_alloc(ty_alloc_fn_palloc, ty_free_fn_pfree); }

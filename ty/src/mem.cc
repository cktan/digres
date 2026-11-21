/*
 * Copyright (c) 2026 CK Tan
 * SPDX-License-Identifier: MIT
 */

/*
 * mem.cc -- the alloc and free functions of a thread, ty_set_alloc().
 *
 * A thread that sets none has NULL here, which means malloc and
 * free: so no thread_local needs a constructor.
 */

#include "mem.h"

#include <stdlib.h>

#include "error.h"
#include "ty.h"

/* A thread that sets none has NULL: malloc and free. */
thread_local ty_alloc_fn ty_t_alloc = nullptr;
thread_local ty_free_fn ty_t_free = nullptr;

/* Set the calling thread's alloc and free; both NULL for the default. */
int ty_set_alloc(ty_alloc_fn alloc, ty_free_fn free) {
  if ((nullptr == alloc) != (nullptr == free)) {
    return ty_internal::set_error(
        TY_ERR_INVALID, "alloc and free must be both set or both NULL");
  }
  ty_t_alloc = alloc;
  ty_t_free = free;
  return 0;
}

/* size bytes from the thread's alloc function, or malloc. */
void *ty_internal::mem_alloc(size_t size) { return ty_alloc(size); }

/* ptr to the thread's free function, or free. */
void ty_internal::mem_free(void *ptr) { ty_free(ptr); }

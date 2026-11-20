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

namespace {

thread_local ty_alloc_fn t_alloc = nullptr;
thread_local ty_free_fn t_free = nullptr;

} // namespace

/* Set the calling thread's alloc and free; both NULL for the default. */
int ty_set_alloc(ty_alloc_fn alloc, ty_free_fn free) {
  if ((nullptr == alloc) != (nullptr == free)) {
    return ty_internal::set_error(
        TY_ERR_INVALID, "alloc and free must be both set or both NULL");
  }
  t_alloc = alloc;
  t_free = free;
  return 0;
}

/* size bytes from the thread's alloc function, or malloc. */
void *ty_internal::mem_alloc(size_t size) {
  return t_alloc ? t_alloc(size) : malloc(size);
}

/* ptr to the thread's free function, or free. */
void ty_internal::mem_free(void *ptr) {
  if (t_free) {
    t_free(ptr);
  } else {
    free(ptr);
  }
}

/*
 * Copyright (c) 2026 CK Tan
 * SPDX-License-Identifier: MIT
 */

/*
 * mem.h -- how ty's own code gets memory: through the alloc and
 * free functions of the calling thread (ty_set_alloc).
 */
#ifndef TY_SRC_MEM_H
#define TY_SRC_MEM_H

#include <stddef.h>

namespace ty_internal {

/* size bytes from the thread's alloc function; NULL if it fails. */
void *mem_alloc(size_t size);

/* Give ptr, which mem_alloc returned, to the thread's free function. */
void mem_free(void *ptr);

} // namespace ty_internal

#endif /* TY_SRC_MEM_H */

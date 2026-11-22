/*
 * Copyright (c) 2026 CK Tan
 * SPDX-License-Identifier: MIT
 */

/*
 * ty.h -- ty's API: the operations of the SQL types, for sm and
 * for digres to call (DESIGN.md). It is C in style and compiled
 * as C++, as sm.h is. ty depends on nothing else in the
 * repository.
 *
 * Conventions that hold throughout, as in sm.h:
 *
 *   - A function returning int returns 0 on success and -1 on
 *     error; the code and message are then read with ty_errnum()
 *     and ty_errmsg(). The error state is per thread.
 *   - A call that yields a value hands it back through its last
 *     parameter, set only on success. A call that cannot fail
 *     returns its result.
 *   - A result of variable width is the caller's: ty allocates it
 *     with the thread's alloc function (ty_set_alloc), and never
 *     frees it. ty frees only what it allocated and did not
 *     return.
 *   - No C++ exception crosses this boundary, in either direction.
 *     What ty calls, the alloc and free functions, must not throw
 *     or longjmp.
 */
#ifndef TY_H
#define TY_H

#include <stddef.h>
#include <stdlib.h>

#include "ty_error.h"

/* ---- the error state ---- */

/* The calling thread's last error code, TY_OK if none. */
ty_errnum_t ty_errnum(void);

/* The calling thread's last error message; "no error" if none. */
const char *ty_errmsg(void);

/* The name of code, such as "TY_ERR_NOMEM". */
const char *ty_errname(ty_errnum_t code);

/*
 * Record TY_ERR_RANGE, or TY_ERR_DIVZERO, as the thread's error, and
 * return -1. They are out of line: an inline operation calls one on the
 * rare failure, and costs no more.
 */
int ty_err_range(void);
int ty_err_divzero(void);
int ty_err_underflow(void);

/* Record TY_ERR_INVALID with msg as the thread's error; returns -1. */
int ty_err_invalid(const char *msg);

/* A branch that is taken on the rare failure. */
#define TY_UNLIKELY(x) __builtin_expect((x) != 0, 0)

/* ---- memory ---- */

/*
 * The functions ty gets memory with, for a result of variable
 * width: a host's memory context, or an arena. They have malloc's
 * and free's signatures, and the same rules as sq's allocator.
 *
 *   - alloc returns size bytes aligned to at least 8, or NULL when
 *     it cannot. ty reports a NULL as TY_ERR_NOMEM.
 *   - free takes a pointer that alloc returned before; ty never
 *     passes NULL. ty calls it only for a piece it outgrew.
 *   - Neither may throw or longjmp.
 *   - ty calls them only on the thread that set them, so they need
 *     no thread safety of their own.
 *   - A result is the caller's, and ty has no call to free one:
 *     the caller frees it as its alloc function suggests, one
 *     piece with free, or all at once, as a memory context does.
 */
typedef void *(*ty_alloc_fn)(size_t size);
typedef void (*ty_free_fn)(void *ptr);

/*
 * Set the alloc and free functions of the calling thread; other
 * threads are not changed. A thread that sets none has malloc and
 * free. Both NULL restores that default; only one NULL fails with
 * TY_ERR_INVALID and changes nothing.
 *
 * Call it at the start of a thread, before the thread's first
 * call that returns a result. Do not change it while a result of
 * the thread is live: free would not match alloc.
 */
int ty_set_alloc(ty_alloc_fn alloc, ty_free_fn free);

/*
 * The calling thread's functions, for the operations in line in the headers:
 * NULL means malloc and free. Use ty_alloc and ty_free, not these.
 */
#ifdef __cplusplus
#define TY_THREAD_LOCAL thread_local
#else
#define TY_THREAD_LOCAL _Thread_local
#endif
extern TY_THREAD_LOCAL ty_alloc_fn ty_t_alloc;
extern TY_THREAD_LOCAL ty_free_fn ty_t_free;

/* size bytes from the thread's alloc function, or malloc; NULL if it fails. */
static inline void *ty_alloc(size_t size) {
  return ty_t_alloc ? ty_t_alloc(size) : malloc(size);
}

/* Give ptr, which ty_alloc returned, to the thread's free function. */
static inline void ty_free(void *ptr) {
  if (ty_t_free) {
    ty_t_free(ptr);
  } else {
    free(ptr);
  }
}

#endif /* TY_H */

/*
 * Copyright (c) 2026 CK Tan
 * SPDX-License-Identifier: MIT
 */

/*
 * sm1_ty.h -- the alloc and free functions of ty on a thread of sm.
 *
 * ty keeps them per thread (ty_set_alloc, ../../ty/DESIGN.md section
 * 3.2): a thread that sets none has malloc and free. Every thread that
 * sm starts, but the timer thread of sm0 (which calls no type
 * operation), calls ty_thread_init() as its first act. The values that
 * ty makes there are in an arena of the thread, which the thread's end
 * frees, so ty's free function does nothing. A thread that digres
 * adopts later (session_thread_adopt()) is given palloc's functions
 * then, which replace these.
 *
 * A thread that runs for long, and calls an operation of ty that
 * allocates, grows its arena: no such call is made yet.
 */
#ifndef SM1_TY_H
#define SM1_TY_H

namespace sm1 {

/* Give ty on this thread an arena of the thread as its alloc function. */
void ty_thread_init();

} // namespace sm1

#endif /* SM1_TY_H */

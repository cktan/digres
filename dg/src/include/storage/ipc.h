/*-------------------------------------------------------------------------
 *
 * ipc.h
 *	  POSTGRES inter-process communication definitions.
 *
 * This file is misnamed, as it no longer has much of anything directly
 * to do with IPC.  The functionality here is concerned with managing
 * exit-time cleanup for either a postmaster or a backend.
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 * Portions Copyright (c) 2026, CK Tan: the changes for digres,
 * under the MIT License (https://github.com/cktan/digres/blob/main/LICENSE)
 *
 * src/include/storage/ipc.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef IPC_H
#define IPC_H

typedef void (*pg_on_exit_callback)(int code, Datum arg);

/* ipc.c */
extern thread_local /*LOCAL*/ PGDLLIMPORT bool proc_exit_inprogress;
extern thread_local /*LOCAL*/ PGDLLIMPORT bool shmem_exit_inprogress;

/*
 * What proc_exit() throws in a session thread (IsSessionThread): the
 * session ends, and the thread with it, not the process.
 */
struct proc_exit_t {
  int code;
};

pg_noreturn extern void proc_exit(int code);
extern void shmem_exit(int code);
extern void on_proc_exit(pg_on_exit_callback function, Datum arg);
extern void on_shmem_exit(pg_on_exit_callback function, Datum arg);
extern void before_shmem_exit(pg_on_exit_callback function, Datum arg);
extern void cancel_before_shmem_exit(pg_on_exit_callback function, Datum arg);
extern void check_on_shmem_exit_lists_are_empty(void);

/* ipci.c */
extern void CreateSharedMemoryAndSemaphores(void);

#endif /* IPC_H */

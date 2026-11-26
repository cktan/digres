/*
 * Copyright (c) 2026 CK Tan
 * SPDX-License-Identifier: MIT
 */

/*
 * ty_eval.h -- the evaluation engine (../EVAL_PLAN.md): a program of an
 * expression, in Janet's syntax, compiled into bytecode for a stack machine
 * of ty's scalar and vector operations.
 *
 *   ty_prog_compile(text, len, &prog)
 *       Compile one fn form: its parameters, each a name, a colon and a
 *       type, with .v for a vector, then its body. Returns 0, or -1 with
 *       TY_ERR_INVALID and "line:col: why", or TY_ERR_NOMEM. A program is
 *       read only, and threads may share it.
 *
 *   ty_prog_free(prog)
 *       Free a program; NULL is nothing.
 *
 *   ty_prog_dump(prog, out)
 *       Append the bytecode to out, an instruction a line, for a test.
 *
 *   ty_run_new(prog, max_n, &run)
 *       A thread's state for running prog on batches of at most max_n
 *       rows: the stack's arrays, made once. 0, or -1 with TY_ERR_NOMEM.
 *
 *   ty_run(run, n, args, &result)
 *       Run on n rows. args are the parameters, in their order: a vector's
 *       n values and n flags, or a scalar's one value and one flag; a flags
 *       pointer may be NULL, for no null. Returns 0 with result, the n
 *       values and flags of a vector, or the one of a scalar, valid until
 *       the next run; or -1 with the error of the lowest row that is valid
 *       and failed (ty_errnum, ty_errmsg, ty_erridx), or TY_ERR_INVALID for
 *       an n above max_n. A run is one thread's.
 *
 *   ty_run_free(run)
 *       Free a run; NULL is nothing.
 *
 * The language, its names and its rules are in ../EVAL_PLAN.md §4 and §5.
 */
#ifndef TY_EVAL_H
#define TY_EVAL_H

#include <stddef.h>
#include <stdint.h>

#include <string>

#include "ty.h"

typedef struct ty_prog_t ty_prog_t;
typedef struct ty_run_t ty_run_t;

/* An input: a vector's values and flags, or a scalar's. */
typedef struct ty_arg_t {
  const void *values;
  const uint8_t *flags; /* NULL: no row is null */
} ty_arg_t;

/* The result: n values and flags, or a scalar's one. */
typedef struct ty_result_t {
  const void *values;
  const uint8_t *flags;
} ty_result_t;

int ty_prog_compile(const char *text, size_t len, ty_prog_t **out);
void ty_prog_free(ty_prog_t *prog);
void ty_prog_dump(const ty_prog_t *prog, std::string &out);
int ty_run_new(const ty_prog_t *prog, uint32_t max_n, ty_run_t **out);
void ty_run_free(ty_run_t *run);
int ty_run(ty_run_t *run, uint32_t n, const ty_arg_t *args,
           ty_result_t *result);

#endif /* TY_EVAL_H */

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
 * The language, its names and its rules are in ../EVAL_PLAN.md §4 and §5.
 */
#ifndef TY_EVAL_H
#define TY_EVAL_H

#include <stddef.h>

#include <string>

#include "ty.h"

typedef struct ty_prog_t ty_prog_t;

int ty_prog_compile(const char *text, size_t len, ty_prog_t **out);
void ty_prog_free(ty_prog_t *prog);
void ty_prog_dump(const ty_prog_t *prog, std::string &out);

#endif /* TY_EVAL_H */

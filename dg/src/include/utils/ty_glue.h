/*
 * Copyright (c) 2026 CK Tan
 * SPDX-License-Identifier: MIT
 */

/*-------------------------------------------------------------------------
 *
 * ty_glue.h
 *	  What digres needs to call ty: the alloc and free functions of a
 *	  thread, the type of a varlena, and the raise of an error of ty.
 *
 * src/include/utils/ty_glue.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef TY_GLUE_H
#define TY_GLUE_H

#include "ty_varlena.h"

/* A varlena that digres has (text *, bytea *) as ty's. ty reads it as it is. */
#define TYV(p) ((const ty_varlena_t *)(p))
/* The place of a varlena that ty makes: a text ** or a bytea **. */
#define TYVO(pp) ((ty_varlena_t **)(pp))

extern void ty_thread_init(void);

/*
 * Raise the error of the last call of ty that made a value and failed: out of
 * memory for TY_ERR_NOMEM, else PROGRAM_LIMIT_EXCEEDED with ty's message.
 */
extern pg_noreturn void ty_raise_alloc(void);

#endif /* TY_GLUE_H */

/*
 * postgres_fe.h -- the little of PostgreSQL's c.h that its Ryu
 * float printers need, so they compile here unchanged.
 *
 * The other files in this directory are copied verbatim from
 * PostgreSQL 18.6 (tag REL_18_6): src/common/f2s.c, d2s.c,
 * d2s_full_table.h, d2s_intrinsics.h, digit_table.h, ryu_common.h
 * and src/include/common/shortest_dec.h. They are PostgreSQL's
 * float4out and float8out, which do not always print the
 * shortest decimal, so ty prints with them to match PostgreSQL
 * exactly (sm/sm2/TYPE.md §3.3). They are built with -DFRONTEND, which
 * makes them include this file in place of postgres.h.
 *
 * Their licence is in each file: portions Copyright (c)
 * 2018-2025 PostgreSQL Global Development Group, modifying Ryu,
 * Copyright 2018 Ulf Adams, under the Boost Software License 1.0.
 */
#ifndef TY_RYU_POSTGRES_FE_H
#define TY_RYU_POSTGRES_FE_H

#include "ty_rename.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef int32_t int32;
typedef uint8_t uint8;
typedef uint32_t uint32;
typedef uint64_t uint64;

/* PostgreSQL uses 128-bit arithmetic where the compiler has it. */
#ifdef __SIZEOF_INT128__
#define HAVE_INT128 1
typedef unsigned __int128 uint128;
#endif

/*
 * The size of size_t, as PostgreSQL's configure finds it: ryu_common.h takes
 * the 32-bit code, which is slower, if it is under 8.
 */
#define SIZEOF_SIZE_T __SIZEOF_SIZE_T__

/* As in a PostgreSQL build without assertions. */
#define Assert(condition) ((void)true)
#define likely(x) __builtin_expect((x) != 0, 1)
#define unlikely(x) __builtin_expect((x) != 0, 0)

/* As PostgreSQL's configure finds it, with GCC: the code uses __builtin_clz. */
#define HAVE__BUILTIN_CLZ 1
#define UINT64CONST(x) UINT64_C(x)

/* Only the unused palloc'ing wrappers call it. */
#define palloc(size) malloc(size)

#endif /* TY_RYU_POSTGRES_FE_H */

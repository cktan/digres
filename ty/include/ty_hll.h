/*
 * Copyright (c) 2026 CK Tan
 * SPDX-License-Identifier: MIT
 */

/*
 * ty_hll.h -- HyperLogLog: an estimate of the number of distinct values in a
 * set, from a hash of each, in a fixed space (Flajolet, Fusy, Gandouet and
 * Meunier, "HyperLogLog: the analysis of a near-optimal cardinality
 * estimation algorithm", 2007). Written for ty. Its estimates are those of
 * PostgreSQL's src/backend/lib/hyperloglog.c, bit for bit, so a sort stops
 * its abbreviated keys at the row PostgreSQL's does.
 */
#ifndef TY_HLL_H
#define TY_HLL_H

#include <stddef.h>
#include <stdint.h>

#include "ty.h"

/*
 * The state: 2^width registers, each the most leading zeros, plus one, seen
 * in the hashes the register's index begins.
 */
typedef struct ty_hll_t {
  uint8_t width;   /* the bits of a hash that pick its register: 4 to 16 */
  size_t nreg;     /* the registers: 2^width */
  double alpha_mm; /* the paper's alpha times nreg squared */
  uint8_t *reg;    /* the registers, the alloc function's */
} ty_hll_t;

/*
 * Make a state of 2^width registers, all 0. A width outside 4 to 16 is
 * TY_ERR_INVALID ("bit width must be between 4 and 16 inclusive"); TY_ERR_NOMEM
 * if the alloc function returns NULL.
 */
int ty_hll_init(ty_hll_t *h, int width);

/*
 * Make a state of the fewest registers whose standard error, 1.04 over the
 * root of their number, is below error; at most 2^16.
 */
int ty_hll_init_error(ty_hll_t *h, double error);

/* Free the registers; not the state. */
void ty_hll_free(ty_hll_t *h);

/*
 * Add a value by its hash, which must spread its bits evenly. The first width
 * bits pick the register; the rest give the rank of the first 1 bit.
 */
static inline void ty_hll_add(ty_hll_t *h, uint32_t hash) {
  uint32_t index = hash >> (32 - h->width);
  uint32_t rest = hash << h->width;
  uint8_t rank =
      0 == rest ? (uint8_t)(33 - h->width) : (uint8_t)(__builtin_clz(rest) + 1);

  if (rank > h->reg[index]) {
    h->reg[index] = rank;
  }
}

/*
 * The estimate of the distinct values added: the paper's, with its
 * corrections for a small and a large number.
 */
double ty_hll_estimate(const ty_hll_t *h);

#endif /* TY_HLL_H */

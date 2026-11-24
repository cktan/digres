/*-------------------------------------------------------------------------
 *
 * hyperloglog.c
 *	  HyperLogLog cardinality estimator
 *
 * Portions Copyright (c) 2014-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 2026, CK Tan: the changes for digres,
 * under the MIT License (https://github.com/cktan/digres/blob/main/LICENSE)
 *
 * Based on Hideaki Ohno's C++ implementation.  This is probably not ideally
 * suited to estimating the cardinality of very large sets;  in particular, we
 * have not attempted to further optimize the implementation as described in
 * the Heule, Nunkesser and Hall paper "HyperLogLog in Practice: Algorithmic
 * Engineering of a State of The Art Cardinality Estimation Algorithm".
 *
 * A sparse representation of HyperLogLog state is used, with fixed space
 * overhead.
 *
 * The copyright terms of Ohno's original version (the MIT license) follow.
 *
 * IDENTIFICATION
 *	  src/backend/lib/hyperloglog.c
 *
 *-------------------------------------------------------------------------
 */

/*
 * Copyright (c) 2013 Hideaki Ohno <hide.o.j55{at}gmail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the 'Software'), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED 'AS IS', WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include "postgres.h"

#include "lib/hyperloglog.h"
#include "utils/ty_glue.h"

/*
 * The estimator is ty's (ty_hll.h), whose estimates are this file's were,
 * bit for bit. These are its PostgreSQL names, with PostgreSQL's errors.
 */

/*
 * Initialize HyperLogLog track state, by bit width: 2 to the power of bwidth
 * registers, in the current memory context. bwidth must be 4 to 16.
 */
void initHyperLogLog(hyperLogLogState *cState, uint8 bwidth) {
  if (ty_hll_init(cState, bwidth)) {
    if (TY_ERR_INVALID == ty_errnum())
      elog(ERROR, "%s", ty_errmsg());
    ty_raise_alloc();
  }
}

/*
 * Initialize HyperLogLog track state, by error rate: the fewest registers
 * whose error, 1.04 / sqrt(m), is below error.
 */
void initHyperLogLogError(hyperLogLogState *cState, double error) {
  if (ty_hll_init_error(cState, error))
    ty_raise_alloc();
}

/*
 * Free HyperLogLog track state: its registers, not the state itself (in case
 * it's not allocated by palloc).
 */
void freeHyperLogLog(hyperLogLogState *cState) {
  Assert(cState->reg != NULL);
  ty_hll_free(cState);
}

/* Estimates cardinality, based on elements added so far */
double estimateHyperLogLog(hyperLogLogState *cState) {
  return ty_hll_estimate(cState);
}

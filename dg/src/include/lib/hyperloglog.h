/*
 * hyperloglog.h
 *
 * A simple HyperLogLog cardinality estimator implementation
 *
 * Portions Copyright (c) 2014-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 2026, CK Tan: the changes for digres,
 * under the MIT License (https://github.com/cktan/digres/blob/main/LICENSE)
 *
 * Based on Hideaki Ohno's C++ implementation.  The copyright terms of Ohno's
 * original version (the MIT license) follow.
 *
 * src/include/lib/hyperloglog.h
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

#ifndef HYPERLOGLOG_H
#define HYPERLOGLOG_H

#include "ty_hll.h"

/*
 * HyperLogLog is an approximate technique for computing the number of distinct
 * entries in a set.  Importantly, it does this by using a fixed amount of
 * memory.  See the 2007 paper "HyperLogLog: the analysis of a near-optimal
 * cardinality estimation algorithm" for more.
 *
 * The state is ty's (ty_hll.h).
 */
typedef ty_hll_t hyperLogLogState;

extern void initHyperLogLog(hyperLogLogState *cState, uint8 bwidth);
extern void initHyperLogLogError(hyperLogLogState *cState, double error);
extern double estimateHyperLogLog(hyperLogLogState *cState);
extern void freeHyperLogLog(hyperLogLogState *cState);

/*
 * Adds element to the estimator, from caller-supplied hash, which must spread
 * its bits evenly (hash_any(), typically). In line: a sort calls it a row.
 */
static inline void addHyperLogLog(hyperLogLogState *cState, uint32 hash) {
  ty_hll_add(cState, hash);
}

#endif /* HYPERLOGLOG_H */

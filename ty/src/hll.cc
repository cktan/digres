/*
 * Copyright (c) 2026 CK Tan
 * SPDX-License-Identifier: MIT
 */

/*
 * hll.cc -- HyperLogLog's state and estimate (ty_hll.h).
 */
#include <math.h>
#include <string.h>

#include "error.h"
#include "ty_hll.h"

/*
 * Make the registers, and alpha times their number squared: alpha corrects
 * the bias of the raw estimate, by the paper's table for 16, 32 and 64
 * registers and its formula for more.
 */
int ty_hll_init(ty_hll_t *h, int width) {
  if (width < 4 || width > 16) {
    return ty_internal::set_error(
        TY_ERR_INVALID, "bit width must be between 4 and 16 inclusive");
  }
  size_t m = (size_t)1 << width;
  uint8_t *reg = (uint8_t *)ty_alloc(m);

  if (nullptr == reg) {
    return ty_internal::set_error(TY_ERR_NOMEM, "out of memory");
  }
  memset(reg, 0, m);
  double alpha = 16 == m   ? 0.673
                 : 32 == m ? 0.697
                 : 64 == m ? 0.709
                           : 0.7213 / (1.0 + 1.079 / m);

  h->width = (uint8_t)width;
  h->nreg = m;
  h->alpha_mm = alpha * m * m;
  h->reg = reg;
  return 0;
}

/* The fewest registers whose error 1.04 / sqrt(m) is below error. */
int ty_hll_init_error(ty_hll_t *h, double error) {
  int width = 4;

  while (width < 16 && !(1.04 / sqrt((double)((size_t)1 << width)) < error)) {
    width++;
  }
  return ty_hll_init(h, width);
}

/* Free the registers. */
void ty_hll_free(ty_hll_t *h) {
  ty_free(h->reg);
  h->reg = nullptr;
}

/*
 * Estimate the distinct values.
 * 1. The raw estimate is alpha m^2 over the sum of 2^-r of the registers,
 *    summed in their order, as PostgreSQL's is.
 * 2. Below 5/2 m, with a register still 0, count by the registers at 0:
 *    m ln(m / zeros).
 * 3. Above 2^32 / 30, correct for the collisions of 32-bit hashes:
 *    -2^32 ln(1 - E / 2^32).
 */
double ty_hll_estimate(const ty_hll_t *h) {
  const double two32 = 4294967296.0;
  double sum = 0.0;

  for (size_t i = 0; i < h->nreg; i++) {
    sum += 1.0 / ldexp(1.0, h->reg[i]);
  }
  double e = h->alpha_mm / sum;

  if (e <= 2.5 * h->nreg) {
    size_t zeros = 0;

    for (size_t i = 0; i < h->nreg; i++) {
      zeros += 0 == h->reg[i];
    }
    if (0 != zeros) {
      e = h->nreg * log((double)h->nreg / zeros);
    }
  } else if (e > (1.0 / 30.0) * two32) {
    e = -two32 * log(1.0 - e / two32);
  }
  return e;
}

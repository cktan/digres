/*
 * Copyright (c) 2026 CK Tan
 * SPDX-License-Identifier: MIT
 */

/*
 * aggfloat.cc -- the arithmetic of the statistical aggregates over float64
 * (ty_float.h), by the Youngs-Cramer method, with PostgreSQL's care for
 * overflow, infinity and NaN.
 */
#include <math.h>

#include "ty_float.h"

namespace {

const double nan_value = __builtin_nan("");

} // namespace

/* Combine two variance states: the parts of each, and of their means' gap. */
int ty_float_var_combine(ty_float_var_t *a, const ty_float_var_t *b) {
  if (a->n == 0.0) {
    *a = *b;
    return 0;
  }
  if (b->n == 0.0) {
    return 0;
  }
  double n = a->n + b->n;
  double sx;

  if (ty_fp64_add(a->sx, b->sx, &sx) != 0) {
    return -1;
  }
  double tmp = a->sx / a->n - b->sx / b->n;
  double sxx = a->sxx + b->sxx + a->n * b->n * tmp * tmp / n;

  if (isinf(sxx) && !isinf(a->sxx) && !isinf(b->sxx)) {
    return ty_err_range();
  }
  a->n = n;
  a->sx = sx;
  a->sxx = sxx;
  return 0;
}

/* The mean; NULL for no rows. */
int ty_float_var_avg(const ty_float_var_t *st, double *r) {
  if (st->n == 0.0) {
    return 1;
  }
  *r = st->sx / st->n;
  return 0;
}

/* The population variance; NULL for no rows. */
int ty_float_var_pop(const ty_float_var_t *st, double *r) {
  if (st->n == 0.0) {
    return 1;
  }
  *r = st->sxx / st->n;
  return 0;
}

/* The sample variance; NULL for one row or none. */
int ty_float_var_samp(const ty_float_var_t *st, double *r) {
  if (st->n <= 1.0) {
    return 1;
  }
  *r = st->sxx / (st->n - 1.0);
  return 0;
}

/* The population standard deviation. */
int ty_float_stddev_pop(const ty_float_var_t *st, double *r) {
  if (st->n == 0.0) {
    return 1;
  }
  *r = sqrt(st->sxx / st->n);
  return 0;
}

/* The sample standard deviation. */
int ty_float_stddev_samp(const ty_float_var_t *st, double *r) {
  if (st->n <= 1.0) {
    return 1;
  }
  *r = sqrt(st->sxx / (st->n - 1.0));
  return 0;
}

/* Combine two regression states. */
int ty_float_regr_combine(ty_float_regr_t *a, const ty_float_regr_t *b) {
  if (a->n == 0.0) {
    *a = *b;
    return 0;
  }
  if (b->n == 0.0) {
    return 0;
  }
  double n = a->n + b->n;
  double sx, sy;

  if (ty_fp64_add(a->sx, b->sx, &sx) != 0) {
    return -1;
  }
  double tmp1 = a->sx / a->n - b->sx / b->n;
  double sxx = a->sxx + b->sxx + a->n * b->n * tmp1 * tmp1 / n;

  if (isinf(sxx) && !isinf(a->sxx) && !isinf(b->sxx)) {
    return ty_err_range();
  }
  if (ty_fp64_add(a->sy, b->sy, &sy) != 0) {
    return -1;
  }
  double tmp2 = a->sy / a->n - b->sy / b->n;
  double syy = a->syy + b->syy + a->n * b->n * tmp2 * tmp2 / n;

  if (isinf(syy) && !isinf(a->syy) && !isinf(b->syy)) {
    return ty_err_range();
  }
  double sxy = a->sxy + b->sxy + a->n * b->n * tmp1 * tmp2 / n;

  if (isinf(sxy) && !isinf(a->sxy) && !isinf(b->sxy)) {
    return ty_err_range();
  }
  a->n = n;
  a->sx = sx;
  a->sxx = sxx;
  a->sy = sy;
  a->syy = syy;
  a->sxy = sxy;
  return 0;
}

/* Sum of squares of x; NULL for no rows. */
int ty_float_regr_sxx(const ty_float_regr_t *st, double *r) {
  if (st->n < 1.0) {
    return 1;
  }
  *r = st->sxx;
  return 0;
}

/* Sum of squares of y. */
int ty_float_regr_syy(const ty_float_regr_t *st, double *r) {
  if (st->n < 1.0) {
    return 1;
  }
  *r = st->syy;
  return 0;
}

/* Sum of products. */
int ty_float_regr_sxy(const ty_float_regr_t *st, double *r) {
  if (st->n < 1.0) {
    return 1;
  }
  *r = st->sxy;
  return 0;
}

/* Mean of x. */
int ty_float_regr_avgx(const ty_float_regr_t *st, double *r) {
  if (st->n < 1.0) {
    return 1;
  }
  *r = st->sx / st->n;
  return 0;
}

/* Mean of y. */
int ty_float_regr_avgy(const ty_float_regr_t *st, double *r) {
  if (st->n < 1.0) {
    return 1;
  }
  *r = st->sy / st->n;
  return 0;
}

/* Population covariance. */
int ty_float_covar_pop(const ty_float_regr_t *st, double *r) {
  if (st->n < 1.0) {
    return 1;
  }
  *r = st->sxy / st->n;
  return 0;
}

/* Sample covariance; NULL for under two rows. */
int ty_float_covar_samp(const ty_float_regr_t *st, double *r) {
  if (st->n < 2.0) {
    return 1;
  }
  *r = st->sxy / (st->n - 1.0);
  return 0;
}

/* Correlation; NULL if no rows or either variable is constant. */
int ty_float_corr(const ty_float_regr_t *st, double *r) {
  if (st->n < 1.0 || st->sxx == 0 || st->syy == 0) {
    return 1;
  }
  *r = st->sxy / sqrt(st->sxx * st->syy);
  return 0;
}

/* The coefficient of determination: 1 if y is constant, NULL if x is. */
int ty_float_regr_r2(const ty_float_regr_t *st, double *r) {
  if (st->n < 1.0 || st->sxx == 0) {
    return 1;
  }
  if (st->syy == 0) {
    *r = 1.0;
    return 0;
  }
  *r = (st->sxy * st->sxy) / (st->sxx * st->syy);
  return 0;
}

/* The slope of the least-squares line; NULL if x is constant. */
int ty_float_regr_slope(const ty_float_regr_t *st, double *r) {
  if (st->n < 1.0 || st->sxx == 0) {
    return 1;
  }
  *r = st->sxy / st->sxx;
  return 0;
}

/* The intercept of the least-squares line. */
int ty_float_regr_intercept(const ty_float_regr_t *st, double *r) {
  if (st->n < 1.0 || st->sxx == 0) {
    return 1;
  }
  *r = (st->sy - st->sx * st->sxy / st->sxx) / st->n;
  return 0;
}

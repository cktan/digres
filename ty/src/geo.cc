/*
 * Copyright (c) 2026 CK Tan
 * SPDX-License-Identifier: MIT
 */

/*
 * geo.cc -- the operations of point and box.
 */
#include "ty_geo.h"

#include <math.h>

#include "error.h"

/* The float steps return -1 from the caller on an error, as in the header. */
#define TRY(call) TY_GEO_TRY(call)

/*
 * The hypotenuse.
 * 1. An infinity gives infinity, a NaN gives NaN.
 * 2. With x the larger of the absolute values, x * sqrt(1 + (y/x)^2) cannot
 *    overflow unless the result does.
 */
int ty_hypot(double x, double y, double *r) {
  if (std::isinf(x) || std::isinf(y)) {
    *r = INFINITY;
    return 0;
  }
  if (std::isnan(x) || std::isnan(y)) {
    *r = NAN;
    return 0;
  }
  x = fabs(x);
  y = fabs(y);
  if (x < y) {
    double t = x;

    x = y;
    y = t;
  }
  if (y == 0.0) {
    *r = x;
    return 0;
  }
  double yx = y / x;
  double result = x * sqrt(1.0 + (yx * yx));

  if (std::isinf(result)) {
    return ty_err_range();
  }
  if (result == 0.0) {
    return ty_err_underflow();
  }
  *r = result;
  return 0;
}

/* The distance of two points. */
int ty_point_distance(const ty_point_t *a, const ty_point_t *b, double *r) {
  double dx, dy;

  TRY(ty_float64_sub(a->x, b->x, &dx));
  TRY(ty_float64_sub(a->y, b->y, &dy));
  return ty_hypot(dx, dy, r);
}

/* The slope of the line through two points. */
int ty_point_slope(const ty_point_t *a, const ty_point_t *b, double *r) {
  double dy, dx;

  if (ty_fp_eq(a->x, b->x)) {
    *r = INFINITY;
    return 0;
  }
  if (ty_fp_eq(a->y, b->y)) {
    *r = 0.0;
    return 0;
  }
  TRY(ty_float64_sub(a->y, b->y, &dy));
  TRY(ty_float64_sub(a->x, b->x, &dx));
  return ty_float64_div(dy, dx, r);
}

/* The slope of the perpendicular. */
int ty_point_invslope(const ty_point_t *a, const ty_point_t *b, double *r) {
  double dx, dy;

  if (ty_fp_eq(a->x, b->x)) {
    *r = 0.0;
    return 0;
  }
  if (ty_fp_eq(a->y, b->y)) {
    *r = INFINITY;
    return 0;
  }
  TRY(ty_float64_sub(a->x, b->x, &dx));
  TRY(ty_float64_sub(b->y, a->y, &dy));
  return ty_float64_div(dx, dy, r);
}

/* The box of two corners: the larger of each coordinate is high. */
void ty_box_construct(ty_box_t *r, const ty_point_t *a, const ty_point_t *b) {
  ty_box_t t;

  if (ty_float64_gt(a->x, b->x)) {
    t.high.x = a->x;
    t.low.x = b->x;
  } else {
    t.high.x = b->x;
    t.low.x = a->x;
  }
  if (ty_float64_gt(a->y, b->y)) {
    t.high.y = a->y;
    t.low.y = b->y;
  } else {
    t.high.y = b->y;
    t.low.y = a->y;
  }
  *r = t;
}

/* Swap the corners of a box that has them the wrong way. */
void ty_box_normalize(ty_box_t *r) {
  if (ty_float64_lt(r->high.x, r->low.x)) {
    double x = r->high.x;

    r->high.x = r->low.x;
    r->low.x = x;
  }
  if (ty_float64_lt(r->high.y, r->low.y)) {
    double y = r->high.y;

    r->high.y = r->low.y;
    r->low.y = y;
  }
}

/* The distance of the centers. */
int ty_box_distance(const ty_box_t *a, const ty_box_t *b, double *r) {
  ty_point_t p, q;

  TRY(ty_box_center(a, &p));
  TRY(ty_box_center(b, &q));
  return ty_point_distance(&p, &q, r);
}

/* The common part of two boxes. */
int ty_box_intersect(const ty_box_t *a, const ty_box_t *b, ty_box_t *r) {
  if (!ty_box_overlap(a, b)) {
    return 0;
  }
  r->high.x = ty_float64_smaller(a->high.x, b->high.x);
  r->low.x = ty_float64_larger(a->low.x, b->low.x);
  r->high.y = ty_float64_smaller(a->high.y, b->high.y);
  r->low.y = ty_float64_larger(a->low.y, b->low.y);
  return 1;
}

/* A box plus a point. */
int ty_box_add(const ty_box_t *a, const ty_point_t *p, ty_box_t *r) {
  ty_box_t t;

  TRY(ty_point_add(&a->high, p, &t.high));
  TRY(ty_point_add(&a->low, p, &t.low));
  *r = t;
  return 0;
}

/* A box minus a point. */
int ty_box_sub(const ty_box_t *a, const ty_point_t *p, ty_box_t *r) {
  ty_box_t t;

  TRY(ty_point_sub(&a->high, p, &t.high));
  TRY(ty_point_sub(&a->low, p, &t.low));
  *r = t;
  return 0;
}

/* A box times a point: the product of each corner, then ordered. */
int ty_box_mul(const ty_box_t *a, const ty_point_t *p, ty_box_t *r) {
  ty_point_t high, low;

  TRY(ty_point_mul(&a->high, p, &high));
  TRY(ty_point_mul(&a->low, p, &low));
  ty_box_construct(r, &high, &low);
  return 0;
}

/* A box over a point. */
int ty_box_div(const ty_box_t *a, const ty_point_t *p, ty_box_t *r) {
  ty_point_t high, low;

  TRY(ty_point_div(&a->high, p, &high));
  TRY(ty_point_div(&a->low, p, &low));
  ty_box_construct(r, &high, &low);
  return 0;
}

namespace {

/* The areas of two boxes. */
int areas(const ty_box_t *a, const ty_box_t *b, double *x, double *y) {
  TRY(ty_box_area(a, x));
  return ty_box_area(b, y);
}

} // namespace

/* a < b by area. */
int ty_box_lt(const ty_box_t *a, const ty_box_t *b, int *r) {
  double x, y;

  TRY(areas(a, b, &x, &y));
  *r = ty_fp_lt(x, y);
  return 0;
}

/* a > b by area. */
int ty_box_gt(const ty_box_t *a, const ty_box_t *b, int *r) {
  double x, y;

  TRY(areas(a, b, &x, &y));
  *r = ty_fp_gt(x, y);
  return 0;
}

/* a = b by area. */
int ty_box_eq(const ty_box_t *a, const ty_box_t *b, int *r) {
  double x, y;

  TRY(areas(a, b, &x, &y));
  *r = ty_fp_eq(x, y);
  return 0;
}

/* a <= b by area. */
int ty_box_le(const ty_box_t *a, const ty_box_t *b, int *r) {
  double x, y;

  TRY(areas(a, b, &x, &y));
  *r = ty_fp_le(x, y);
  return 0;
}

/* a >= b by area. */
int ty_box_ge(const ty_box_t *a, const ty_box_t *b, int *r) {
  double x, y;

  TRY(areas(a, b, &x, &y));
  *r = ty_fp_ge(x, y);
  return 0;
}

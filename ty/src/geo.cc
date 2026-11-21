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

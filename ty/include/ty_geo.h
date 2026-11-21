/*
 * Copyright (c) 2026 CK Tan
 * SPDX-License-Identifier: MIT
 */

/*
 * ty_geo.h -- the operations of the geometric types point and box, and the
 * fuzzy float comparison all the geometric types use. The cases follow
 * PostgreSQL's geo_ops.c. The structs have PostgreSQL's layout.
 *
 * Two floats are the same if they are within EPSILON of each other, and one is
 * below another if it is below by more than EPSILON. A call that does float
 * arithmetic returns 0, or -1 with the thread's error set, as the float
 * operations of ty_float.h do (overflow, underflow, division by zero).
 */
#ifndef TY_GEO_H
#define TY_GEO_H

#include <math.h>
#include <stddef.h>
#include <stdint.h>

#include "ty.h"
#include "ty_float.h"

#define TY_GEO_EPSILON 1.0E-06

typedef struct ty_point_t {
    double x, y;
} ty_point_t;

/* A box has its high corner first. */
typedef struct ty_box_t {
    ty_point_t high, low;
} ty_box_t;

/* The fuzzy compares. */
static inline int ty_fp_zero(double a) { return fabs(a) <= TY_GEO_EPSILON; }
static inline int ty_fp_eq(double a, double b) {
    return a == b || fabs(a - b) <= TY_GEO_EPSILON;
}
/* Not the negation of eq for NaN: PostgreSQL's FPne is false for it. */
static inline int ty_fp_ne(double a, double b) {
    return a != b && fabs(a - b) > TY_GEO_EPSILON;
}
static inline int ty_fp_lt(double a, double b) { return a + TY_GEO_EPSILON < b; }
static inline int ty_fp_le(double a, double b) { return a <= b + TY_GEO_EPSILON; }
static inline int ty_fp_gt(double a, double b) { return a > b + TY_GEO_EPSILON; }
static inline int ty_fp_ge(double a, double b) { return a + TY_GEO_EPSILON >= b; }

/*
 * point: the same (NaN is the same as NaN, as in float8 compares), and
 * left, right, above and below by the fuzzy compares of x or y.
 */
static inline int ty_point_left(const ty_point_t* a, const ty_point_t* b) {
    return ty_fp_lt(a->x, b->x);
}
static inline int ty_point_right(const ty_point_t* a, const ty_point_t* b) {
    return ty_fp_gt(a->x, b->x);
}
static inline int ty_point_above(const ty_point_t* a, const ty_point_t* b) {
    return ty_fp_gt(a->y, b->y);
}
static inline int ty_point_below(const ty_point_t* a, const ty_point_t* b) {
    return ty_fp_lt(a->y, b->y);
}
static inline int ty_point_vert(const ty_point_t* a, const ty_point_t* b) {
    return ty_fp_eq(a->x, b->x);
}
static inline int ty_point_horiz(const ty_point_t* a, const ty_point_t* b) {
    return ty_fp_eq(a->y, b->y);
}

/* The hypotenuse, without overflow unless the result overflows. */
int ty_hypot(double x, double y, double* r);

/* The distance of two points, and the slopes (infinity for vertical). */
int ty_point_distance(const ty_point_t* a, const ty_point_t* b, double* r);
int ty_point_slope(const ty_point_t* a, const ty_point_t* b, double* r);
int ty_point_invslope(const ty_point_t* a, const ty_point_t* b, double* r);

/* Sum, difference, product and quotient of two points, as complex numbers. */

/* The box with these two corners, the high one first. */
void ty_box_construct(ty_box_t* r, const ty_point_t* a, const ty_point_t* b);

/* The box with its corners in order (the text and recv of a box). */
void ty_box_normalize(ty_box_t* r);

/* The same box; overlap; a contains b. */

/* The position of a box against another, by the fuzzy compares. */
static inline int ty_box_left(const ty_box_t* a, const ty_box_t* b) {
    return ty_fp_lt(a->high.x, b->low.x);
}
static inline int ty_box_overleft(const ty_box_t* a, const ty_box_t* b) {
    return ty_fp_le(a->high.x, b->high.x);
}
static inline int ty_box_right(const ty_box_t* a, const ty_box_t* b) {
    return ty_fp_gt(a->low.x, b->high.x);
}
static inline int ty_box_overright(const ty_box_t* a, const ty_box_t* b) {
    return ty_fp_ge(a->low.x, b->low.x);
}
static inline int ty_box_below(const ty_box_t* a, const ty_box_t* b) {
    return ty_fp_lt(a->high.y, b->low.y);
}
static inline int ty_box_overbelow(const ty_box_t* a, const ty_box_t* b) {
    return ty_fp_le(a->high.y, b->high.y);
}
static inline int ty_box_above(const ty_box_t* a, const ty_box_t* b) {
    return ty_fp_gt(a->low.y, b->high.y);
}
static inline int ty_box_overabove(const ty_box_t* a, const ty_box_t* b) {
    return ty_fp_ge(a->low.y, b->low.y);
}
static inline int ty_box_below_eq(const ty_box_t* a, const ty_box_t* b) {
    return ty_fp_le(a->high.y, b->low.y);
}
static inline int ty_box_above_eq(const ty_box_t* a, const ty_box_t* b) {
    return ty_fp_ge(a->low.y, b->high.y);
}

/* The width, the height, the area and the center of a box. */

/* The distance of the centers. */
int ty_box_distance(const ty_box_t* a, const ty_box_t* b, double* r);

/*
 * The box in common, 1 if there is one (they overlap) and 0 if not: the
 * result is then not set.
 */
int ty_box_intersect(const ty_box_t* a, const ty_box_t* b, ty_box_t* r);

/* A box moved, scaled and divided by a point. */
int ty_box_add(const ty_box_t* a, const ty_point_t* p, ty_box_t* r);
int ty_box_sub(const ty_box_t* a, const ty_point_t* p, ty_box_t* r);
int ty_box_mul(const ty_box_t* a, const ty_point_t* p, ty_box_t* r);
int ty_box_div(const ty_box_t* a, const ty_point_t* p, ty_box_t* r);

/* The order of boxes is the order of their areas, by the fuzzy compares. */
int ty_box_lt(const ty_box_t* a, const ty_box_t* b, int* r);
int ty_box_gt(const ty_box_t* a, const ty_box_t* b, int* r);
int ty_box_eq(const ty_box_t* a, const ty_box_t* b, int* r);
int ty_box_le(const ty_box_t* a, const ty_box_t* b, int* r);
int ty_box_ge(const ty_box_t* a, const ty_box_t* b, int* r);

/* Run a float operation, and return -1 from the caller if it fails. */
#define TY_GEO_TRY(call)       \
    do {                       \
        if ((call) != 0) {     \
            return -1;         \
        }                      \
    } while (0)

/* The functions below are in line: they are used in loops over many values. */

/* The same point: NaN is the same as NaN. */
static inline int ty_point_eq(const ty_point_t* a, const ty_point_t* b) {
  if (isnan(a->x) || isnan(a->y) || isnan(b->x) ||
      isnan(b->y)) {
    /* the float8 equality: NaN equals NaN, and nothing else */
    return ty_float64_eq(a->x, b->x) && ty_float64_eq(a->y, b->y);
  }
  return ty_fp_eq(a->x, b->x) && ty_fp_eq(a->y, b->y);
}

/* a + b. */
static inline int ty_point_add(const ty_point_t* a, const ty_point_t* b, ty_point_t* r) {
  ty_point_t t;

  TY_GEO_TRY(ty_float64_add(a->x, b->x, &t.x));
  TY_GEO_TRY(ty_float64_add(a->y, b->y, &t.y));
  *r = t;
  return 0;
}

/* a - b. */
static inline int ty_point_sub(const ty_point_t* a, const ty_point_t* b, ty_point_t* r) {
  ty_point_t t;

  TY_GEO_TRY(ty_float64_sub(a->x, b->x, &t.x));
  TY_GEO_TRY(ty_float64_sub(a->y, b->y, &t.y));
  *r = t;
  return 0;
}

/* a * b, as complex numbers. */
static inline int ty_point_mul(const ty_point_t* a, const ty_point_t* b, ty_point_t* r) {
  double m1, m2, m3, m4;
  ty_point_t t;

  TY_GEO_TRY(ty_float64_mul(a->x, b->x, &m1));
  TY_GEO_TRY(ty_float64_mul(a->y, b->y, &m2));
  TY_GEO_TRY(ty_float64_sub(m1, m2, &t.x));
  TY_GEO_TRY(ty_float64_mul(a->x, b->y, &m3));
  TY_GEO_TRY(ty_float64_mul(a->y, b->x, &m4));
  TY_GEO_TRY(ty_float64_add(m3, m4, &t.y));
  *r = t;
  return 0;
}

/* a / b, as complex numbers. */
static inline int ty_point_div(const ty_point_t* a, const ty_point_t* b, ty_point_t* r) {
  double d1, d2, div, m1, m2, m3, m4, n1, n2;
  ty_point_t t;

  TY_GEO_TRY(ty_float64_mul(b->x, b->x, &d1));
  TY_GEO_TRY(ty_float64_mul(b->y, b->y, &d2));
  TY_GEO_TRY(ty_float64_add(d1, d2, &div));
  TY_GEO_TRY(ty_float64_mul(a->x, b->x, &m1));
  TY_GEO_TRY(ty_float64_mul(a->y, b->y, &m2));
  TY_GEO_TRY(ty_float64_add(m1, m2, &n1));
  TY_GEO_TRY(ty_float64_div(n1, div, &t.x));
  TY_GEO_TRY(ty_float64_mul(a->y, b->x, &m3));
  TY_GEO_TRY(ty_float64_mul(a->x, b->y, &m4));
  TY_GEO_TRY(ty_float64_sub(m3, m4, &n2));
  TY_GEO_TRY(ty_float64_div(n2, div, &t.y));
  *r = t;
  return 0;
}

/* The boxes have a point in common. */
static inline int ty_box_overlap(const ty_box_t* a, const ty_box_t* b) {
  return ty_fp_le(a->low.x, b->high.x) && ty_fp_le(b->low.x, a->high.x) &&
         ty_fp_le(a->low.y, b->high.y) && ty_fp_le(b->low.y, a->high.y);
}

/* a contains b. */
static inline int ty_box_contain_box(const ty_box_t* a, const ty_box_t* b) {
  return ty_fp_ge(a->high.x, b->high.x) && ty_fp_le(a->low.x, b->low.x) &&
         ty_fp_ge(a->high.y, b->high.y) && ty_fp_le(a->low.y, b->low.y);
}

/* The width. */
static inline int ty_box_width(const ty_box_t* a, double* r) {
  return ty_float64_sub(a->high.x, a->low.x, r);
}

/* The height. */
static inline int ty_box_height(const ty_box_t* a, double* r) {
  return ty_float64_sub(a->high.y, a->low.y, r);
}

/* The area. */
static inline int ty_box_area(const ty_box_t* a, double* r) {
  double w, h;

  TY_GEO_TRY(ty_box_width(a, &w));
  TY_GEO_TRY(ty_box_height(a, &h));
  return ty_float64_mul(w, h, r);
}

/* The center. */
static inline int ty_box_center(const ty_box_t* a, ty_point_t* r) {
  double sx, sy;
  ty_point_t t;

  TY_GEO_TRY(ty_float64_add(a->high.x, a->low.x, &sx));
  TY_GEO_TRY(ty_float64_div(sx, 2.0, &t.x));
  TY_GEO_TRY(ty_float64_add(a->high.y, a->low.y, &sy));
  TY_GEO_TRY(ty_float64_div(sy, 2.0, &t.y));
  *r = t;
  return 0;
}

/* The same box: both corners the same. */
static inline int ty_box_same(const ty_box_t* a, const ty_box_t* b) {
  return ty_point_eq(&a->high, &b->high) && ty_point_eq(&a->low, &b->low);
}

#endif

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

/* A translation unit that needs these small functions in line names this. */
#ifndef TY_GEO_INLINE
#define TY_GEO_INLINE
#endif

/* Run a float operation, and return -1 from the caller if it fails. */
#define TY_GEO_TRY(call)       \
    do {                       \
        if ((call) != 0) {     \
            return -1;         \
        }                      \
    } while (0)

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

/*
 * The hypotenuse, without overflow unless the result overflows. It is in
 * line: the distance of two points is in the loops of the operations of
 * lseg and line.
 * 1. An infinity gives infinity, a NaN gives NaN.
 * 2. With x the larger of the absolute values, x * sqrt(1 + (y/x)^2) cannot
 *    overflow unless the result does.
 */
static inline TY_GEO_INLINE int ty_hypot(double x, double y, double* r) {
    double yx, result;

    if (TY_UNLIKELY(isinf(x) || isinf(y))) {
        *r = INFINITY;
        return 0;
    }
    if (TY_UNLIKELY(isnan(x) || isnan(y))) {
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
    yx = y / x;
    result = x * sqrt(1.0 + (yx * yx));
    if (TY_UNLIKELY(isinf(result))) {
        return ty_err_range();
    }
    if (TY_UNLIKELY(result == 0.0)) {
        return ty_err_underflow();
    }
    *r = result;
    return 0;
}

/*
 * The distance of two points, and the slopes (infinity for vertical). They
 * are in line, as they are called in loops by the operations of lseg and line.
 */
static inline TY_GEO_INLINE int ty_point_distance(const ty_point_t* a, const ty_point_t* b, double* r) {
    double dx, dy;

    TY_GEO_TRY(ty_float64_sub(a->x, b->x, &dx));
    TY_GEO_TRY(ty_float64_sub(a->y, b->y, &dy));
    return ty_hypot(dx, dy, r);
}

static inline TY_GEO_INLINE int ty_point_slope(const ty_point_t* a, const ty_point_t* b, double* r) {
    double dy, dx;

    if (ty_fp_eq(a->x, b->x)) {
        *r = INFINITY;
        return 0;
    }
    if (ty_fp_eq(a->y, b->y)) {
        *r = 0.0;
        return 0;
    }
    TY_GEO_TRY(ty_float64_sub(a->y, b->y, &dy));
    TY_GEO_TRY(ty_float64_sub(a->x, b->x, &dx));
    return ty_float64_div(dy, dx, r);
}

static inline TY_GEO_INLINE int ty_point_invslope(const ty_point_t* a, const ty_point_t* b, double* r) {
    double dx, dy;

    if (ty_fp_eq(a->x, b->x)) {
        *r = 0.0;
        return 0;
    }
    if (ty_fp_eq(a->y, b->y)) {
        *r = INFINITY;
        return 0;
    }
    TY_GEO_TRY(ty_float64_sub(a->x, b->x, &dx));
    TY_GEO_TRY(ty_float64_sub(b->y, a->y, &dy));
    return ty_float64_div(dx, dy, r);
}

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

/*
 * line and lseg. A line is A*x + B*y + C = 0 (A and B not both zero). A
 * line segment has two end points. A call that can fail returns 0, or -1 with
 * the thread's error set. A "hit" says whether there is a result: 1 when
 * there is one, 0 when there is none (the SQL operator then gives NULL or
 * false).
 */
typedef struct ty_line_t {
    double a, b, c;
} ty_line_t;

typedef struct ty_lseg_t {
    ty_point_t p[2];
} ty_lseg_t;

/* A line with A and B both zero is not a line (the text and recv of a line). */
static inline int ty_line_valid(const ty_line_t* l) {
    return !(ty_fp_zero(l->a) && ty_fp_zero(l->b));
}

/* The line through a point with the slope m (infinity for vertical). */
static inline TY_GEO_INLINE int ty_line_construct(ty_line_t* r, const ty_point_t* pt, double m) {
    ty_line_t t;

    if (isinf(m)) {
        /* Vertical: x = C. */
        t.a = -1.0;
        t.b = 0.0;
        t.c = pt->x;
    } else if (m == 0) {
        /* Horizontal: y = C. */
        t.a = 0.0;
        t.b = -1.0;
        t.c = pt->y;
    } else {
        /* m*x - y + intercept = 0. */
        double mx;

        t.a = m;
        t.b = -1.0;
        TY_GEO_TRY(ty_float64_mul(m, pt->x, &mx));
        TY_GEO_TRY(ty_float64_sub(pt->y, mx, &t.c));
        /* The subtraction can give -0. */
        if (t.c == 0.0) {
            t.c = 0.0;
        }
    }
    *r = t;
    return 0;
}

/* The line through two points: TY_ERR_INVALID if they are the same point. */
int ty_line_from_points(const ty_point_t* a, const ty_point_t* b, ty_line_t* r);

/* The line the segment is on (the points are not tested to be distinct). */
int ty_line_from_lseg(const ty_lseg_t* s, ty_line_t* r);

/* The slope of a line, and of its perpendicular. */
int ty_line_slope(const ty_line_t* l, double* r);
int ty_line_invslope(const ty_line_t* l, double* r);

/* The lines are not parallel; are parallel; are perpendicular; are the same. */
int ty_line_intersect(const ty_line_t* a, const ty_line_t* b, int* r);
int ty_line_parallel(const ty_line_t* a, const ty_line_t* b, int* r);
int ty_line_perp(const ty_line_t* a, const ty_line_t* b, int* r);
int ty_line_eq(const ty_line_t* a, const ty_line_t* b, int* r);

static inline int ty_line_vertical(const ty_line_t* l) { return ty_fp_zero(l->b); }
static inline int ty_line_horizontal(const ty_line_t* l) { return ty_fp_zero(l->a); }

/* The distance of two lines. */
int ty_line_distance(const ty_line_t* a, const ty_line_t* b, double* r);

/* The point where two lines meet: hit is 0 if they do not. */
int ty_line_interpt(const ty_line_t* a, const ty_line_t* b, ty_point_t* r, int* hit);

/* The point is on the line. */
int ty_line_contain_point(const ty_line_t* l, const ty_point_t* p, int* r);

/* The segment from two points. */
static inline void ty_lseg_construct(ty_lseg_t* r, const ty_point_t* a, const ty_point_t* b) {
    r->p[0] = *a;
    r->p[1] = *b;
}

/* The slope of a segment, and of its perpendicular; its length; its center. */
int ty_lseg_slope(const ty_lseg_t* s, double* r);
int ty_lseg_invslope(const ty_lseg_t* s, double* r);
int ty_lseg_length(const ty_lseg_t* s, double* r);
int ty_lseg_center(const ty_lseg_t* s, ty_point_t* r);

/* The segments meet; are parallel; are perpendicular. */
int ty_lseg_intersect(const ty_lseg_t* a, const ty_lseg_t* b, int* r);
int ty_lseg_parallel(const ty_lseg_t* a, const ty_lseg_t* b, int* r);
int ty_lseg_perp(const ty_lseg_t* a, const ty_lseg_t* b, int* r);

static inline int ty_lseg_vertical(const ty_lseg_t* s) {
    return ty_fp_eq(s->p[0].x, s->p[1].x);
}
static inline int ty_lseg_horizontal(const ty_lseg_t* s) {
    return ty_fp_eq(s->p[0].y, s->p[1].y);
}
static inline int ty_lseg_eq(const ty_lseg_t* a, const ty_lseg_t* b) {
    return ty_point_eq(&a->p[0], &b->p[0]) && ty_point_eq(&a->p[1], &b->p[1]);
}

/* The order of segments is the order of their lengths, by the fuzzy compares. */
int ty_lseg_lt(const ty_lseg_t* a, const ty_lseg_t* b, int* r);
int ty_lseg_le(const ty_lseg_t* a, const ty_lseg_t* b, int* r);
int ty_lseg_gt(const ty_lseg_t* a, const ty_lseg_t* b, int* r);
int ty_lseg_ge(const ty_lseg_t* a, const ty_lseg_t* b, int* r);

/* The point where two segments meet, if they do. */
int ty_lseg_interpt(const ty_lseg_t* a, const ty_lseg_t* b, ty_point_t* r, int* hit);

/* Both end points of the segment are on the line. */
int ty_lseg_on_line(const ty_lseg_t* s, const ty_line_t* l, int* r);

/* The segment meets the line; r (may be NULL) is the point. */
int ty_lseg_interpt_line(const ty_lseg_t* s, const ty_line_t* l, ty_point_t* r, int* hit);

/* The point is on the segment. */
int ty_lseg_contain_point(const ty_lseg_t* s, const ty_point_t* p, int* r);

/* The point is in the box or on its border (a plain compare, not fuzzy). */
static inline int ty_box_contain_point(const ty_box_t* b, const ty_point_t* p) {
    return b->high.x >= p->x && b->low.x <= p->x && b->high.y >= p->y && b->low.y <= p->y;
}

/* The segment is in the box or on its border. */
static inline int ty_box_contain_lseg(const ty_box_t* b, const ty_lseg_t* s) {
    return ty_box_contain_point(b, &s->p[0]) && ty_box_contain_point(b, &s->p[1]);
}

/* The diagonal of a box, high corner first. */
static inline void ty_box_diagonal(const ty_box_t* b, ty_lseg_t* r) {
    ty_lseg_construct(r, &b->high, &b->low);
}

/* The segment meets the box; r (may be NULL) is the point nearest its center. */
int ty_box_interpt_lseg(const ty_box_t* b, const ty_lseg_t* s, ty_point_t* r, int* hit);

/* The line meets the box. */
int ty_box_interpt_line(const ty_box_t* b, const ty_line_t* l, int* hit);

/*
 * The distance between two things and, in r (may be NULL), the point of the
 * first on it that is the nearest. A distance may be NaN. The "close"
 * functions are the SQL operator "##": hit is 0 where it gives NULL.
 */
int ty_point_line_distance(const ty_point_t* p, const ty_line_t* l, ty_point_t* r, double* d);
int ty_point_lseg_distance(const ty_point_t* p, const ty_lseg_t* s, ty_point_t* r, double* d);
int ty_point_box_distance(const ty_point_t* p, const ty_box_t* b, ty_point_t* r, double* d);
int ty_lseg_lseg_distance(const ty_lseg_t* on, const ty_lseg_t* to, ty_point_t* r, double* d);
int ty_lseg_line_distance(const ty_lseg_t* s, const ty_line_t* l, ty_point_t* r, double* d);
int ty_lseg_box_distance(const ty_lseg_t* s, const ty_box_t* b, ty_point_t* r, double* d);

int ty_point_line_close(const ty_point_t* p, const ty_line_t* l, ty_point_t* r, int* hit);
int ty_point_lseg_close(const ty_point_t* p, const ty_lseg_t* s, ty_point_t* r, int* hit);
int ty_point_box_close(const ty_point_t* p, const ty_box_t* b, ty_point_t* r, int* hit);
int ty_lseg_lseg_close(const ty_lseg_t* a, const ty_lseg_t* b, ty_point_t* r, int* hit);
int ty_lseg_line_close(const ty_line_t* l, const ty_lseg_t* s, ty_point_t* r, int* hit);
int ty_lseg_box_close(const ty_lseg_t* s, const ty_box_t* b, ty_point_t* r, int* hit);

#endif

/*
 * Copyright (c) 2026 CK Tan
 * SPDX-License-Identifier: MIT
 */

/*
 * geo_lseg.cc -- the operations of line and lseg, and of them with point
 * and box. The cases follow PostgreSQL's geo_ops.c, written again in the
 * error convention of ty: a float step that fails returns -1 from the
 * caller.
 */
#include <math.h>

/*
 * The float steps and the small geometry functions are used so many times in
 * this file that the compiler stops inlining them: force it.
 */
#define TY_FLOAT_INLINE __attribute__((always_inline))
#define TY_GEO_INLINE __attribute__((always_inline))
#define INLINE inline __attribute__((always_inline))

#include "error.h"
#include "ty_geo.h"

#define TRY(call) TY_GEO_TRY(call)

namespace {

/* Set var to the 0 or 1 result of an internal function, or return its error. */
#define GET(var, call) \
  do {                 \
    var = (call);      \
    if (var < 0) {     \
      return -1;       \
    }                  \
  } while (0)

/* Set *r to a 0 or 1 result of an internal function, or return its error. */
#define SET_BOOL(r, call)  \
  do {                     \
    int v_ = (call);       \
    if (v_ < 0) {          \
      return -1;           \
    }                      \
    *(r) = v_;             \
    return 0;              \
  } while (0)

/* The distance of two points; the slopes. */
INLINE int pt_dt(const ty_point_t *a, const ty_point_t *b, double *r) {
  return ty_point_distance(a, b, r);
}

/* The slope of a line, and of its perpendicular: 0 and infinity as below. */
INLINE int line_sl(const ty_line_t *l, double *r) {
  if (ty_fp_zero(l->a)) {
    *r = 0.0;
    return 0;
  }
  if (ty_fp_zero(l->b)) {
    *r = INFINITY;
    return 0;
  }
  return ty_float64_div(l->a, -l->b, r);
}

INLINE int line_invsl(const ty_line_t *l, double *r) {
  if (ty_fp_zero(l->a)) {
    *r = INFINITY;
    return 0;
  }
  if (ty_fp_zero(l->b)) {
    *r = 0.0;
    return 0;
  }
  return ty_float64_div(l->b, l->a, r);
}

/* The slope of the line a segment is on, and of its perpendicular. */
INLINE int seg_sl(const ty_lseg_t *s, double *r) {
  return ty_point_slope(&s->p[0], &s->p[1], r);
}

INLINE int seg_invsl(const ty_lseg_t *s, double *r) {
  return ty_point_invslope(&s->p[0], &s->p[1], r);
}

/*
 * The point where two lines meet, in *result if it is not NULL: 1 if they
 * meet, 0 if they are parallel, -1 on an error. A NaN in a line gives a hit
 * with NaN coordinates. The other "interpt" and "has" functions here return
 * as this one does.
 */
int line_interpt(const ty_line_t *l1, const ty_line_t *l2, ty_point_t *result) {
  double x, y, t1, t2, n1, n2, d1, d2, q, s;

  if (!ty_fp_zero(l1->b)) {
    TRY(ty_float64_div(l2->b, l1->b, &q));
    TRY(ty_float64_mul(l1->a, q, &s));
    if (ty_fp_eq(l2->a, s)) {
      return 0;
    }
    TRY(ty_float64_mul(l1->b, l2->c, &t1));
    TRY(ty_float64_mul(l2->b, l1->c, &t2));
    TRY(ty_float64_sub(t1, t2, &n1));
    TRY(ty_float64_mul(l1->a, l2->b, &t1));
    TRY(ty_float64_mul(l2->a, l1->b, &t2));
    TRY(ty_float64_sub(t1, t2, &d1));
    TRY(ty_float64_div(n1, d1, &x));
    TRY(ty_float64_mul(l1->a, x, &t1));
    TRY(ty_float64_add(t1, l1->c, &n2));
    TRY(ty_float64_div(-n2, l1->b, &y));
  } else if (!ty_fp_zero(l2->b)) {
    TRY(ty_float64_div(l1->b, l2->b, &q));
    TRY(ty_float64_mul(l2->a, q, &s));
    if (ty_fp_eq(l1->a, s)) {
      return 0;
    }
    TRY(ty_float64_mul(l2->b, l1->c, &t1));
    TRY(ty_float64_mul(l1->b, l2->c, &t2));
    TRY(ty_float64_sub(t1, t2, &n1));
    TRY(ty_float64_mul(l2->a, l1->b, &t1));
    TRY(ty_float64_mul(l1->a, l2->b, &t2));
    TRY(ty_float64_sub(t1, t2, &d2));
    TRY(ty_float64_div(n1, d2, &x));
    TRY(ty_float64_mul(l2->a, x, &t1));
    TRY(ty_float64_add(t1, l2->c, &n2));
    TRY(ty_float64_div(-n2, l2->b, &y));
  } else {
    return 0;
  }
  /* The expressions above can give -0. */
  if (x == 0.0) {
    x = 0.0;
  }
  if (y == 0.0) {
    y = 0.0;
  }
  if (result != nullptr) {
    result->x = x;
    result->y = y;
  }
  return 1;
}

/* The point is on the line: A*x + B*y + C is zero. */
int line_has(const ty_line_t *l, const ty_point_t *p) {
  double t1, t2, t3, t4;

  TRY(ty_float64_mul(l->a, p->x, &t1));
  TRY(ty_float64_mul(l->b, p->y, &t2));
  TRY(ty_float64_add(t1, t2, &t3));
  TRY(ty_float64_add(t3, l->c, &t4));
  return ty_fp_zero(t4);
}

/*
 * The point is on the segment: by the triangle inequality, with plain
 * additions as in PostgreSQL.
 */
int seg_has(const ty_lseg_t *s, const ty_point_t *p) {
  double d0, d1, d;

  TRY(pt_dt(p, &s->p[0], &d0));
  TRY(pt_dt(p, &s->p[1], &d1));
  TRY(pt_dt(&s->p[0], &s->p[1], &d));
  return ty_fp_eq(d0 + d1, d);
}

/*
 * The distance of a point from a line, and the foot of the perpendicular in
 * *result. A NaN, and no foot, if the perpendicular does not meet the line.
 */
int line_closept_point(const ty_line_t *line, const ty_point_t *pt,
                       ty_point_t *result, double *dist) {
  ty_point_t closept;
  ty_line_t tmp;
  double m;
  int hit;

  TRY(line_invsl(line, &m));
  TRY(ty_line_construct(&tmp, pt, m));
  hit = line_interpt(&tmp, line, &closept);
  if (hit < 0) {
    return -1;
  }
  if (!hit) {
    if (result != nullptr) {
      *result = *pt;
    }
    *dist = NAN;
    return 0;
  }
  if (result != nullptr) {
    *result = closept;
  }
  return pt_dt(&closept, pt, dist);
}

/* The segment meets the line, with the end point named if it is the one. */
int seg_interpt_line(const ty_lseg_t *seg, const ty_line_t *line,
                     ty_point_t *result) {
  ty_point_t interpt;
  ty_line_t tmp;
  double m;
  int r;

  /* Make the segment a line, to meet the line. */
  TRY(seg_sl(seg, &m));
  TRY(ty_line_construct(&tmp, &seg->p[0], m));
  r = line_interpt(&tmp, line, &interpt);
  if (r <= 0) {
    return r;
  }
  /* Then see if the point is on the segment. */
  r = seg_has(seg, &interpt);
  if (r <= 0) {
    return r;
  }
  if (result != nullptr) {
    /* Name an end point, to leave out the noise of the last bit. */
    if (ty_point_eq(&seg->p[0], &interpt)) {
      *result = seg->p[0];
    } else if (ty_point_eq(&seg->p[1], &interpt)) {
      *result = seg->p[1];
    } else {
      *result = interpt;
    }
  }
  return 1;
}

/* The segments meet. */
int seg_interpt_seg(const ty_lseg_t *l1, const ty_lseg_t *l2,
                    ty_point_t *result) {
  ty_point_t interpt;
  ty_line_t tmp;
  double m;
  int r;

  TRY(seg_sl(l2, &m));
  TRY(ty_line_construct(&tmp, &l2->p[0], m));
  r = seg_interpt_line(l1, &tmp, &interpt);
  if (r <= 0) {
    return r;
  }
  /* The point of the lines must be on l2 too. */
  r = seg_has(l2, &interpt);
  if (r <= 0) {
    return r;
  }
  if (result != nullptr) {
    *result = interpt;
  }
  return 1;
}

/*
 * The point of a segment nearest to a line, and the distance. If the
 * segment meets the line, the distance is 0.
 */
int seg_closept_line(const ty_lseg_t *seg, const ty_line_t *line,
                     ty_point_t *result, double *dist) {
  double d1, d2;
  int hit;

  hit = seg_interpt_line(seg, line, result);
  if (hit < 0) {
    return -1;
  }
  if (hit) {
    *dist = 0.0;
    return 0;
  }
  TRY(line_closept_point(line, &seg->p[0], nullptr, &d1));
  TRY(line_closept_point(line, &seg->p[1], nullptr, &d2));
  if (d1 < d2) {
    if (result != nullptr) {
      *result = seg->p[0];
    }
    *dist = d1;
  } else {
    if (result != nullptr) {
      *result = seg->p[1];
    }
    *dist = d2;
  }
  return 0;
}

/* The point of a segment nearest to a point, and the distance. */
int seg_closept_point(const ty_lseg_t *seg, const ty_point_t *pt,
                      ty_point_t *result, double *dist) {
  ty_point_t closept;
  ty_line_t tmp;
  double m, ignored;

  /* A perpendicular from the point to the line of the segment. */
  TRY(seg_invsl(seg, &m));
  TRY(ty_line_construct(&tmp, pt, m));
  TRY(seg_closept_line(seg, &tmp, &closept, &ignored));
  if (result != nullptr) {
    *result = closept;
  }
  return pt_dt(&closept, pt, dist);
}

/* The point of a segment nearest to another, and the distance. */
int seg_closept_seg(const ty_lseg_t *on, const ty_lseg_t *to,
                    ty_point_t *result, double *dist) {
  ty_point_t point;
  double d;
  int hit;

  /* Segments that meet are at distance 0. */
  hit = seg_interpt_seg(on, to, result);
  if (hit < 0) {
    return -1;
  }
  if (hit) {
    *dist = 0.0;
    return 0;
  }
  /* The nearest of the end points of the second segment... */
  TRY(seg_closept_point(on, &to->p[0], result, dist));
  TRY(seg_closept_point(on, &to->p[1], &point, &d));
  if (ty_float64_lt(d, *dist)) {
    *dist = d;
    if (result != nullptr) {
      *result = point;
    }
  }
  /* ...and the end points of the first can be the nearest. */
  TRY(seg_closept_point(to, &on->p[0], nullptr, &d));
  if (ty_float64_lt(d, *dist)) {
    *dist = d;
    if (result != nullptr) {
      *result = on->p[0];
    }
  }
  TRY(seg_closept_point(to, &on->p[1], nullptr, &d));
  if (ty_float64_lt(d, *dist)) {
    *dist = d;
    if (result != nullptr) {
      *result = on->p[1];
    }
  }
  return 0;
}

/* The four sides of a box, in the order PostgreSQL tries them. */
void box_sides(const ty_box_t *box, ty_lseg_t side[4]) {
  ty_point_t corner;

  corner.x = box->low.x;
  corner.y = box->high.y;
  ty_lseg_construct(&side[0], &box->low, &corner);
  ty_lseg_construct(&side[1], &box->high, &corner);
  corner.x = box->high.x;
  corner.y = box->low.y;
  ty_lseg_construct(&side[2], &box->low, &corner);
  ty_lseg_construct(&side[3], &box->high, &corner);
}

} // namespace

/* The line through two distinct points. */
int ty_line_from_points(const ty_point_t *a, const ty_point_t *b,
                        ty_line_t *r) {
  double m;

  if (ty_point_eq(a, b)) {
    return ty_internal::set_error(
        TY_ERR_INVALID,
        "invalid line specification: must be two distinct points");
  }
  TRY(ty_point_slope(a, b, &m));
  return ty_line_construct(r, a, m);
}

/* The line a segment is on. */
int ty_line_from_lseg(const ty_lseg_t *s, ty_line_t *r) {
  double m;

  TRY(seg_sl(s, &m));
  return ty_line_construct(r, &s->p[0], m);
}

/* The slope of a line: 0 if flat, infinity if vertical. */
int ty_line_slope(const ty_line_t *l, double *r) { return line_sl(l, r); }

/* The slope of the perpendicular. */
int ty_line_invslope(const ty_line_t *l, double *r) { return line_invsl(l, r); }

/* The lines meet in one point. */
int ty_line_intersect(const ty_line_t *a, const ty_line_t *b, int *r) {
  SET_BOOL(r, line_interpt(a, b, nullptr));
}

/* The lines do not meet. */
int ty_line_parallel(const ty_line_t *a, const ty_line_t *b, int *r) {
  int hit = line_interpt(a, b, nullptr);

  if (hit < 0) {
    return -1;
  }
  *r = !hit;
  return 0;
}

/* The lines are at a right angle. */
int ty_line_perp(const ty_line_t *a, const ty_line_t *b, int *r) {
  double m1, m2, q;

  if (ty_fp_zero(a->a)) {
    *r = ty_fp_zero(b->b);
    return 0;
  }
  if (ty_fp_zero(b->a)) {
    *r = ty_fp_zero(a->b);
    return 0;
  }
  if (ty_fp_zero(a->b)) {
    *r = ty_fp_zero(b->a);
    return 0;
  }
  if (ty_fp_zero(b->b)) {
    *r = ty_fp_zero(a->a);
    return 0;
  }
  TRY(ty_float64_mul(a->a, b->a, &m1));
  TRY(ty_float64_mul(a->b, b->b, &m2));
  TRY(ty_float64_div(m1, m2, &q));
  *r = ty_fp_eq(q, -1.0);
  return 0;
}

/* The lines are the same: their constants are in proportion. */
int ty_line_eq(const ty_line_t *a, const ty_line_t *b, int *r) {
  double ratio, p1, p2, p3;

  /* With a NaN, only exact equality is the same. */
  if (std::isnan(a->a) || std::isnan(a->b) || std::isnan(a->c) ||
      std::isnan(b->a) || std::isnan(b->b) || std::isnan(b->c)) {
    *r = ty_float64_eq(a->a, b->a) && ty_float64_eq(a->b, b->b) &&
         ty_float64_eq(a->c, b->c);
    return 0;
  }
  if (!ty_fp_zero(b->a)) {
    TRY(ty_float64_div(a->a, b->a, &ratio));
  } else if (!ty_fp_zero(b->b)) {
    TRY(ty_float64_div(a->b, b->b, &ratio));
  } else if (!ty_fp_zero(b->c)) {
    TRY(ty_float64_div(a->c, b->c, &ratio));
  } else {
    ratio = 1.0;
  }
  TRY(ty_float64_mul(ratio, b->a, &p1));
  if (!ty_fp_eq(a->a, p1)) {
    /* As in PostgreSQL, the && stops at the first that is not the same. */
    *r = 0;
    return 0;
  }
  TRY(ty_float64_mul(ratio, b->b, &p2));
  if (!ty_fp_eq(a->b, p2)) {
    *r = 0;
    return 0;
  }
  TRY(ty_float64_mul(ratio, b->c, &p3));
  *r = ty_fp_eq(a->c, p3);
  return 0;
}

/* The distance of two lines. */
int ty_line_distance(const ty_line_t *a, const ty_line_t *b, double *r) {
  double ratio, rc, num, h;
  int hit = line_interpt(a, b, nullptr);

  if (hit < 0) {
    return -1;
  }
  if (hit) {
    *r = 0.0;
    return 0;
  }
  if (!ty_fp_zero(a->a) && !std::isnan(a->a) && !ty_fp_zero(b->a) &&
      !std::isnan(b->a)) {
    TRY(ty_float64_div(a->a, b->a, &ratio));
  } else if (!ty_fp_zero(a->b) && !std::isnan(a->b) && !ty_fp_zero(b->b) &&
             !std::isnan(b->b)) {
    TRY(ty_float64_div(a->b, b->b, &ratio));
  } else {
    ratio = 1.0;
  }
  TRY(ty_float64_mul(ratio, b->c, &rc));
  TRY(ty_float64_sub(a->c, rc, &num));
  TRY(ty_hypot(a->a, a->b, &h));
  return ty_float64_div(fabs(num), h, r);
}

/* The point where two lines meet. */
int ty_line_interpt(const ty_line_t *a, const ty_line_t *b, ty_point_t *r,
                    int *hit) {
  SET_BOOL(hit, line_interpt(a, b, r));
}

/* The point is on the line. */
int ty_line_contain_point(const ty_line_t *l, const ty_point_t *p, int *r) {
  SET_BOOL(r, line_has(l, p));
}

/* The slope of a segment. */
int ty_lseg_slope(const ty_lseg_t *s, double *r) { return seg_sl(s, r); }

/* The slope of the perpendicular of a segment. */
int ty_lseg_invslope(const ty_lseg_t *s, double *r) { return seg_invsl(s, r); }

/* The length of a segment. */
int ty_lseg_length(const ty_lseg_t *s, double *r) {
  return pt_dt(&s->p[0], &s->p[1], r);
}

/* The middle of a segment. */
int ty_lseg_center(const ty_lseg_t *s, ty_point_t *r) {
  double sx, sy;
  ty_point_t t;

  TRY(ty_float64_add(s->p[0].x, s->p[1].x, &sx));
  TRY(ty_float64_div(sx, 2.0, &t.x));
  TRY(ty_float64_add(s->p[0].y, s->p[1].y, &sy));
  TRY(ty_float64_div(sy, 2.0, &t.y));
  *r = t;
  return 0;
}

/* The segments meet. */
int ty_lseg_intersect(const ty_lseg_t *a, const ty_lseg_t *b, int *r) {
  SET_BOOL(r, seg_interpt_seg(a, b, nullptr));
}

/* The slopes are the same. */
int ty_lseg_parallel(const ty_lseg_t *a, const ty_lseg_t *b, int *r) {
  double m1, m2;

  TRY(seg_sl(a, &m1));
  TRY(seg_sl(b, &m2));
  *r = ty_fp_eq(m1, m2);
  return 0;
}

/* The slope of one is the slope of the perpendicular of the other. */
int ty_lseg_perp(const ty_lseg_t *a, const ty_lseg_t *b, int *r) {
  double m1, m2;

  TRY(seg_sl(a, &m1));
  TRY(seg_invsl(b, &m2));
  *r = ty_fp_eq(m1, m2);
  return 0;
}

namespace {

/* The lengths of two segments. */
int lengths(const ty_lseg_t *a, const ty_lseg_t *b, double *x, double *y) {
  TRY(pt_dt(&a->p[0], &a->p[1], x));
  return pt_dt(&b->p[0], &b->p[1], y);
}

} // namespace

/* a is shorter than b. */
int ty_lseg_lt(const ty_lseg_t *a, const ty_lseg_t *b, int *r) {
  double x, y;

  TRY(lengths(a, b, &x, &y));
  *r = ty_fp_lt(x, y);
  return 0;
}

/* a is not longer than b. */
int ty_lseg_le(const ty_lseg_t *a, const ty_lseg_t *b, int *r) {
  double x, y;

  TRY(lengths(a, b, &x, &y));
  *r = ty_fp_le(x, y);
  return 0;
}

/* a is longer than b. */
int ty_lseg_gt(const ty_lseg_t *a, const ty_lseg_t *b, int *r) {
  double x, y;

  TRY(lengths(a, b, &x, &y));
  *r = ty_fp_gt(x, y);
  return 0;
}

/* a is not shorter than b. */
int ty_lseg_ge(const ty_lseg_t *a, const ty_lseg_t *b, int *r) {
  double x, y;

  TRY(lengths(a, b, &x, &y));
  *r = ty_fp_ge(x, y);
  return 0;
}

/* The point where two segments meet. */
int ty_lseg_interpt(const ty_lseg_t *a, const ty_lseg_t *b, ty_point_t *r,
                    int *hit) {
  SET_BOOL(hit, seg_interpt_seg(a, b, r));
}

/* The segment meets the line. */
int ty_lseg_interpt_line(const ty_lseg_t *s, const ty_line_t *l, ty_point_t *r,
                         int *hit) {
  SET_BOOL(hit, seg_interpt_line(s, l, r));
}

/* The point is on the segment. */
int ty_lseg_contain_point(const ty_lseg_t *s, const ty_point_t *p, int *r) {
  SET_BOOL(r, seg_has(s, p));
}

/* Both end points of a segment are on the line; the second is tried if the
 * first is. */
int ty_lseg_on_line(const ty_lseg_t *s, const ty_line_t *l, int *r) {
  int v = line_has(l, &s->p[0]);

  if (v < 0) {
    return -1;
  }
  if (!v) {
    *r = 0;
    return 0;
  }
  SET_BOOL(r, line_has(l, &s->p[1]));
}

/*
 * The segment meets the box. Where r is wanted, it is the point of the
 * segment nearest to the center of the box.
 * 1. The boxes of the segment and the box must overlap.
 * 2. An end point in the box is a hit.
 * 3. A side of the box that meets the segment is a hit.
 */
int ty_box_interpt_lseg(const ty_box_t *box, const ty_lseg_t *seg,
                        ty_point_t *r, int *hit) {
  ty_box_t lbox;
  ty_lseg_t side[4];
  ty_point_t center;
  double ignored;
  int h;

  lbox.low.x = ty_float64_smaller(seg->p[0].x, seg->p[1].x);
  lbox.low.y = ty_float64_smaller(seg->p[0].y, seg->p[1].y);
  lbox.high.x = ty_float64_larger(seg->p[0].x, seg->p[1].x);
  lbox.high.y = ty_float64_larger(seg->p[0].y, seg->p[1].y);
  if (!ty_box_overlap(&lbox, box)) {
    *hit = 0;
    return 0;
  }
  if (r != nullptr) {
    TRY(ty_box_center(box, &center));
    TRY(seg_closept_point(seg, &center, r, &ignored));
  }
  if (ty_box_contain_point(box, &seg->p[0]) ||
      ty_box_contain_point(box, &seg->p[1])) {
    *hit = 1;
    return 0;
  }
  box_sides(box, side);
  for (int i = 0; i < 4; i++) {
    GET(h, seg_interpt_seg(&side[i], seg, nullptr));
    if (h) {
      *hit = 1;
      return 0;
    }
  }
  *hit = 0;
  return 0;
}

/* The line meets the box: some side does. */
int ty_box_interpt_line(const ty_box_t *box, const ty_line_t *line, int *hit) {
  ty_lseg_t side;
  ty_point_t p1, p2;
  int h;

  /* The sides in PostgreSQL's order: left, top, right, bottom. */
  p1.x = box->low.x;
  p1.y = box->low.y;
  p2.x = box->low.x;
  p2.y = box->high.y;
  ty_lseg_construct(&side, &p1, &p2);
  GET(h, seg_interpt_line(&side, line, nullptr));
  if (h) {
    *hit = 1;
    return 0;
  }
  p1.x = box->high.x;
  p1.y = box->high.y;
  ty_lseg_construct(&side, &p1, &p2);
  GET(h, seg_interpt_line(&side, line, nullptr));
  if (h) {
    *hit = 1;
    return 0;
  }
  p2.x = box->high.x;
  p2.y = box->low.y;
  ty_lseg_construct(&side, &p1, &p2);
  GET(h, seg_interpt_line(&side, line, nullptr));
  if (h) {
    *hit = 1;
    return 0;
  }
  p1.x = box->low.x;
  p1.y = box->low.y;
  ty_lseg_construct(&side, &p1, &p2);
  GET(h, seg_interpt_line(&side, line, nullptr));
  *hit = h;
  return 0;
}

/* The distance of a point from a line. */
int ty_point_line_distance(const ty_point_t *p, const ty_line_t *l,
                           ty_point_t *r, double *d) {
  return line_closept_point(l, p, r, d);
}

/* The distance of a point from a segment. */
int ty_point_lseg_distance(const ty_point_t *p, const ty_lseg_t *s,
                           ty_point_t *r, double *d) {
  return seg_closept_point(s, p, r, d);
}

/* The distance of a point from a box: 0 in it. */
int ty_point_box_distance(const ty_point_t *pt, const ty_box_t *box,
                          ty_point_t *r, double *d) {
  ty_lseg_t side[4];
  ty_point_t closept;
  double dd;

  if (ty_box_contain_point(box, pt)) {
    if (r != nullptr) {
      *r = *pt;
    }
    *d = 0.0;
    return 0;
  }
  box_sides(box, side);
  TRY(seg_closept_point(&side[0], pt, r, d));
  for (int i = 1; i < 4; i++) {
    TRY(seg_closept_point(&side[i], pt, &closept, &dd));
    if (ty_float64_lt(dd, *d)) {
      *d = dd;
      if (r != nullptr) {
        *r = closept;
      }
    }
  }
  return 0;
}

/* The distance of two segments. */
int ty_lseg_lseg_distance(const ty_lseg_t *on, const ty_lseg_t *to,
                          ty_point_t *r, double *d) {
  return seg_closept_seg(on, to, r, d);
}

/* The distance of a segment from a line. */
int ty_lseg_line_distance(const ty_lseg_t *s, const ty_line_t *l, ty_point_t *r,
                          double *d) {
  return seg_closept_line(s, l, r, d);
}

/* The distance of a segment from a box: 0 if they meet. */
int ty_lseg_box_distance(const ty_lseg_t *seg, const ty_box_t *box,
                         ty_point_t *r, double *d) {
  ty_lseg_t side[4];
  ty_point_t closept;
  double dd;
  int hit;

  TRY(ty_box_interpt_lseg(box, seg, r, &hit));
  if (hit) {
    *d = 0.0;
    return 0;
  }
  box_sides(box, side);
  TRY(seg_closept_seg(&side[0], seg, r, d));
  for (int i = 1; i < 4; i++) {
    TRY(seg_closept_seg(&side[i], seg, &closept, &dd));
    if (ty_float64_lt(dd, *d)) {
      *d = dd;
      if (r != nullptr) {
        *r = closept;
      }
    }
  }
  return 0;
}

/* "##" of a point and a line: the foot of the perpendicular. */
int ty_point_line_close(const ty_point_t *p, const ty_line_t *l, ty_point_t *r,
                        int *hit) {
  double d;

  TRY(line_closept_point(l, p, r, &d));
  *hit = !std::isnan(d);
  return 0;
}

/* "##" of a point and a segment. */
int ty_point_lseg_close(const ty_point_t *p, const ty_lseg_t *s, ty_point_t *r,
                        int *hit) {
  double d;

  TRY(seg_closept_point(s, p, r, &d));
  *hit = !std::isnan(d);
  return 0;
}

/* "##" of a point and a box. */
int ty_point_box_close(const ty_point_t *p, const ty_box_t *b, ty_point_t *r,
                       int *hit) {
  double d;

  TRY(ty_point_box_distance(p, b, r, &d));
  *hit = !std::isnan(d);
  return 0;
}

/* "##" of two segments: none if the slopes are the same. */
int ty_lseg_lseg_close(const ty_lseg_t *a, const ty_lseg_t *b, ty_point_t *r,
                       int *hit) {
  double m1, m2, d;

  TRY(seg_sl(a, &m1));
  TRY(seg_sl(b, &m2));
  if (m1 == m2) {
    *hit = 0;
    return 0;
  }
  TRY(seg_closept_seg(b, a, r, &d));
  *hit = !std::isnan(d);
  return 0;
}

/* "##" of a line and a segment: none if the slopes are the same. */
int ty_lseg_line_close(const ty_line_t *l, const ty_lseg_t *s, ty_point_t *r,
                       int *hit) {
  double m1, m2, d;

  TRY(seg_sl(s, &m1));
  TRY(ty_line_slope(l, &m2));
  if (m1 == m2) {
    *hit = 0;
    return 0;
  }
  TRY(seg_closept_line(s, l, r, &d));
  *hit = !std::isnan(d);
  return 0;
}

/* "##" of a segment and a box. */
int ty_lseg_box_close(const ty_lseg_t *s, const ty_box_t *b, ty_point_t *r,
                      int *hit) {
  double d;

  TRY(ty_lseg_box_distance(s, b, r, &d));
  *hit = !std::isnan(d);
  return 0;
}

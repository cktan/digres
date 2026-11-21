/*
 * Copyright (c) 2026 CK Tan
 * SPDX-License-Identifier: MIT
 */

/*
 * geo_poly.cc -- the operations of path and polygon, on arrays of points.
 * The cases follow PostgreSQL's geo_ops.c, written again in the error
 * convention of ty. The algorithm of the point in a polygon is the one of
 * PostgreSQL, which adapted it from WN, a server for HTTP.
 */
#include <limits.h>
#include <math.h>

/*
 * The float steps are used so many times in this file that the compiler stops
 * inlining them: force it, as geo_lseg.cc does.
 */
#define TY_FLOAT_INLINE __attribute__((always_inline))
#define TY_GEO_INLINE __attribute__((always_inline))
#define INLINE inline __attribute__((always_inline))

#include "error.h"
#include "geo.h"
#include "ty_geo.h"

#define TRY(call) TY_GEO_TRY(call)

namespace {

/* What lseg_crossing returns for a segment that holds the point. */
const int ON_POLYGON = INT_MAX;

/*
 * The crossing of the positive x axis by the segment from (prev_x, prev_y)
 * to (x, y), the point being at (0, 0): +-2 if it crosses, +-1 if an end is
 * on the axis, 0 if no crossing, and ON_POLYGON if the segment holds the
 * point. Summed over a polygon, these say if the point is in it.
 */
INLINE int crossing(double x, double y, double prev_x, double prev_y, int *r) {
  double z, t1, t2, m1, m2;
  int y_sign;

  if (ty_fp_zero(y)) {
    if (ty_fp_zero(x)) {
      *r = ON_POLYGON;
    } else if (ty_fp_gt(x, 0)) {
      if (ty_fp_zero(prev_y)) {
        *r = ty_fp_gt(prev_x, 0.0) ? 0 : ON_POLYGON;
      } else {
        *r = ty_fp_lt(prev_y, 0.0) ? 1 : -1;
      }
    } else {
      if (ty_fp_zero(prev_y)) {
        *r = ty_fp_lt(prev_x, 0.0) ? 0 : ON_POLYGON;
      } else {
        *r = 0;
      }
    }
    return 0;
  }
  /* y is not 0: the direction of the crossing from the previous point */
  y_sign = ty_fp_gt(y, 0.0) ? 1 : -1;
  if (ty_fp_zero(prev_y)) {
    *r = ty_fp_lt(prev_x, 0.0) ? 0 : y_sign;
    return 0;
  }
  if ((y_sign < 0 && ty_fp_lt(prev_y, 0.0)) ||
      (y_sign > 0 && ty_fp_gt(prev_y, 0.0))) {
    *r = 0; /* both above or both below the axis */
    return 0;
  }
  /* y and prev_y are on the two sides of the axis */
  if (ty_fp_ge(x, 0.0) && ty_fp_gt(prev_x, 0.0)) {
    *r = 2 * y_sign;
    return 0;
  }
  if (ty_fp_lt(x, 0.0) && ty_fp_le(prev_x, 0.0)) {
    *r = 0;
    return 0;
  }
  TRY(ty_float64_sub(x, prev_x, &t1));
  TRY(ty_float64_mul(t1, y, &m1));
  TRY(ty_float64_sub(y, prev_y, &t2));
  TRY(ty_float64_mul(t2, x, &m2));
  TRY(ty_float64_sub(m1, m2, &z));
  if (ty_fp_zero(z)) {
    *r = ON_POLYGON;
  } else if ((y_sign < 0 && ty_fp_lt(z, 0.0)) ||
             (y_sign > 0 && ty_fp_gt(z, 0.0))) {
    *r = 0;
  } else {
    *r = 2 * y_sign;
  }
  return 0;
}

/* The distance of two points. */
INLINE int pt_dt(const ty_point_t *a, const ty_point_t *b, double *r) {
  return ty_point_distance(a, b, r);
}

/* The segment of the points before and at i of a path: false for the first of
 * an open path. */
INLINE bool edge(int i, int npts, int closed, int *iprev) {
  if (i > 0) {
    *iprev = i - 1;
    return true;
  }
  if (!closed) {
    return false;
  }
  *iprev = npts - 1;
  return true;
}

/* The distance of two segments, kept if it is the smallest so far. */
int keep_min(const ty_lseg_t *a, const ty_lseg_t *b, double *min,
             bool *have_min) {
  double tmp;

  TRY(ty_internal::geo_seg_closept_seg(a, b, &tmp));
  if (!*have_min || ty_float64_lt(tmp, *min)) {
    *min = tmp;
    *have_min = true;
  }
  return 0;
}

/* The segments of two polygons are at distance: the least. */
int segments_distance(int na, const ty_point_t *pa, int ca, int nb,
                      const ty_point_t *pb, int cb, double *min,
                      bool *have_min) {
  ty_lseg_t s1, s2;

  for (int i = 0; i < na; i++) {
    int iprev;

    if (!edge(i, na, ca, &iprev)) {
      continue;
    }
    for (int j = 0; j < nb; j++) {
      int jprev;

      if (!edge(j, nb, cb, &jprev)) {
        continue;
      }
      ty_lseg_construct(&s1, &pa[iprev], &pa[i]);
      ty_lseg_construct(&s2, &pb[jprev], &pb[j]);
      TRY(keep_min(&s1, &s2, min, have_min));
    }
  }
  return 0;
}

/* The segment (a, b) is in the polygon, from its edge at start: 1 or 0. */
int inside_poly(const ty_point_t *a, const ty_point_t *b, int n,
                const ty_point_t *p, int start, ty_poll_fn poll, int *out);

/*
 * A segment (a, b) has the end a on the segment s, and b not on it. 1 if the
 * segment is in the polygon, or if it is not the same line as s: the rest
 * is checked later.
 */
int touched_inside_poly(const ty_point_t *a, const ty_point_t *b,
                        const ty_lseg_t *s, int n, const ty_point_t *p,
                        int start, ty_poll_fn poll, int *out) {
  ty_lseg_t t;
  int has;

  t.p[0] = *a;
  t.p[1] = *b;
  if (ty_point_eq(a, &s->p[0])) {
    has = ty_internal::geo_seg_has(&t, &s->p[1]);
    if (has < 0) {
      return -1;
    }
    if (has) {
      return inside_poly(b, &s->p[1], n, p, start, poll, out);
    }
  } else if (ty_point_eq(a, &s->p[1])) {
    has = ty_internal::geo_seg_has(&t, &s->p[0]);
    if (has < 0) {
      return -1;
    }
    if (has) {
      return inside_poly(b, &s->p[0], n, p, start, poll, out);
    }
  } else {
    has = ty_internal::geo_seg_has(&t, &s->p[0]);
    if (has < 0) {
      return -1;
    }
    if (has) {
      return inside_poly(b, &s->p[0], n, p, start, poll, out);
    }
    has = ty_internal::geo_seg_has(&t, &s->p[1]);
    if (has < 0) {
      return -1;
    }
    if (has) {
      return inside_poly(b, &s->p[1], n, p, start, poll, out);
    }
  }
  *out = 1;
  return 0;
}

/*
 * The segment (a, b) is in the polygon: its edges are tried from the one at
 * start.
 * 1. For each edge s: if s holds an end of the segment, the segment touches
 *    the polygon there; if the segment crosses s, the two halves are tried.
 * 2. If no edge crossed it, the middle of the segment must be in the polygon.
 */
int inside_poly(const ty_point_t *a, const ty_point_t *b, int n,
                const ty_point_t *p, int start, ty_poll_fn poll, int *out) {
  ty_lseg_t s, t;
  int res = 1, intersection = 0, has0, has1, hit;

  /* The function recurses: the host can stop it when the stack is deep. */
  if (poll != nullptr && poll()) {
    return ty_internal::set_error(TY_ERR_ABORTED, "stopped by the host");
  }
  t.p[0] = *a;
  t.p[1] = *b;
  s.p[0] = p[(start == 0) ? (n - 1) : (start - 1)];
  for (int i = start; i < n && res; i++) {
    ty_point_t interpt;

    if (poll != nullptr && poll()) {
      return ty_internal::set_error(TY_ERR_ABORTED, "stopped by the host");
    }
    s.p[1] = p[i];
    has0 = ty_internal::geo_seg_has(&s, &t.p[0]);
    if (has0 < 0) {
      return -1;
    }
    if (has0) {
      has1 = ty_internal::geo_seg_has(&s, &t.p[1]);
      if (has1 < 0) {
        return -1;
      }
      if (has1) {
        *out = 1; /* t is in s */
        return 0;
      }
      TRY(touched_inside_poly(&t.p[0], &t.p[1], &s, n, p, i + 1, poll, &res));
    } else {
      has1 = ty_internal::geo_seg_has(&s, &t.p[1]);
      if (has1 < 0) {
        return -1;
      }
      if (has1) {
        TRY(touched_inside_poly(&t.p[1], &t.p[0], &s, n, p, i + 1, poll, &res));
      } else {
        hit = ty_internal::geo_seg_interpt_seg(&t, &s, &interpt);
        if (hit < 0) {
          return -1;
        }
        if (hit) {
          /* The segments cross: each half of t must be in the polygon. */
          intersection = 1;
          TRY(inside_poly(&t.p[0], &interpt, n, p, i + 1, poll, &res));
          if (res) {
            TRY(inside_poly(&t.p[1], &interpt, n, p, i + 1, poll, &res));
          }
        }
      }
    }
    s.p[0] = s.p[1];
  }
  if (res && !intersection) {
    ty_point_t mid;
    double sx, sy;
    int in;

    TRY(ty_float64_add(t.p[0].x, t.p[1].x, &sx));
    TRY(ty_float64_div(sx, 2.0, &mid.x));
    TRY(ty_float64_add(t.p[0].y, t.p[1].y, &sy));
    TRY(ty_float64_div(sy, 2.0, &mid.y));
    in = ty_point_in_polygon(&mid, n, p);
    if (in < 0) {
      return -1;
    }
    res = in != 0;
  }
  *out = res;
  return 0;
}

} // namespace

/* The box that bounds the points. */
void ty_polygon_bound_box(int npts, const ty_point_t *p, ty_box_t *r) {
  double x1, y1, x2, y2;

  x1 = x2 = p[0].x;
  y2 = y1 = p[0].y;
  for (int i = 1; i < npts; i++) {
    if (ty_float64_lt(p[i].x, x1)) {
      x1 = p[i].x;
    }
    if (ty_float64_gt(p[i].x, x2)) {
      x2 = p[i].x;
    }
    if (ty_float64_lt(p[i].y, y1)) {
      y1 = p[i].y;
    }
    if (ty_float64_gt(p[i].y, y2)) {
      y2 = p[i].y;
    }
  }
  r->low.x = x1;
  r->high.x = x2;
  r->low.y = y1;
  r->high.y = y2;
}

/* The same points, from any of them, forward or back. */
int ty_polygon_same(int na, const ty_point_t *pa, int nb,
                    const ty_point_t *pb) {
  int i, ii, j;

  if (na != nb) {
    return 0;
  }
  /* Find a point of the second that is the first of the first. */
  for (i = 0; i < na; i++) {
    if (ty_point_eq(&pb[i], &pa[0])) {
      /* Look forward through the others. */
      for (ii = 1, j = i + 1; ii < na; ii++, j++) {
        if (j >= na) {
          j = 0;
        }
        if (!ty_point_eq(&pb[j], &pa[ii])) {
          break;
        }
      }
      if (ii == na) {
        return 1;
      }
      /* Not forward: look backward. */
      for (ii = 1, j = i - 1; ii < na; ii++, j--) {
        if (j < 0) {
          j = na - 1;
        }
        if (!ty_point_eq(&pb[j], &pa[ii])) {
          break;
        }
      }
      if (ii == na) {
        return 1;
      }
    }
  }
  return 0;
}

/* The point in a polygon. */
int ty_point_in_polygon(const ty_point_t *pt, int npts, const ty_point_t *p) {
  double x0, y0, prev_x, prev_y, x = 0, y = 0;
  int cross, total_cross = 0;

  /* The first point of the polygon, relative to the point. */
  TRY(ty_float64_sub(p[0].x, pt->x, &x0));
  TRY(ty_float64_sub(p[0].y, pt->y, &y0));
  prev_x = x0;
  prev_y = y0;
  for (int i = 1; i < npts; i++) {
    TRY(ty_float64_sub(p[i].x, pt->x, &x));
    TRY(ty_float64_sub(p[i].y, pt->y, &y));
    TRY(crossing(x, y, prev_x, prev_y, &cross));
    if (cross == ON_POLYGON) {
      return 2;
    }
    total_cross += cross;
    prev_x = x;
    prev_y = y;
  }
  /* The segment from the last point to the first. */
  TRY(crossing(x0, y0, prev_x, prev_y, &cross));
  if (cross == ON_POLYGON) {
    return 2;
  }
  total_cross += cross;
  return total_cross != 0;
}

/*
 * Two polygons overlap.
 * 1. The boxes that bound them must overlap.
 * 2. Any edge of one that meets an edge of the other is an overlap.
 * 3. Else one is in the other, which one point of each says.
 */
int ty_polygon_overlap(int na, const ty_point_t *pa, const ty_box_t *ba, int nb,
                       const ty_point_t *pb, const ty_box_t *bb, int *r) {
  int result = ty_box_overlap(ba, bb);

  if (result) {
    ty_lseg_t sa, sb;
    int hit;

    /* Each edge starts at the point before, the first at the last. */
    sa.p[0] = pa[na - 1];
    result = 0;
    for (int ia = 0; ia < na && !result; ia++) {
      sa.p[1] = pa[ia];
      sb.p[0] = pb[nb - 1];
      for (int ib = 0; ib < nb && !result; ib++) {
        sb.p[1] = pb[ib];
        hit = ty_internal::geo_seg_interpt_seg(&sa, &sb, nullptr);
        if (hit < 0) {
          return -1;
        }
        result = hit;
        sb.p[0] = sb.p[1];
      }
      sa.p[0] = sa.p[1];
    }
    if (!result) {
      int in1 = ty_point_in_polygon(pa, nb, pb);

      if (in1 < 0) {
        return -1;
      }
      result = in1 != 0;
      if (!result) {
        int in2 = ty_point_in_polygon(pb, na, pa);

        if (in2 < 0) {
          return -1;
        }
        result = in2 != 0;
      }
    }
  }
  *r = result;
  return 0;
}

/*
 * a contains b.
 * 1. The box of b must be in the box of a.
 * 2. Each edge of b must be in a.
 */
int ty_polygon_contain(int na, const ty_point_t *pa, const ty_box_t *ba, int nb,
                       const ty_point_t *pb, const ty_box_t *bb,
                       ty_poll_fn poll, int *r) {
  ty_lseg_t s;
  int in;

  if (!ty_box_contain_box(ba, bb)) {
    *r = 0;
    return 0;
  }
  s.p[0] = pb[nb - 1];
  for (int i = 0; i < nb; i++) {
    s.p[1] = pb[i];
    TRY(inside_poly(&s.p[0], &s.p[1], na, pa, 0, poll, &in));
    if (!in) {
      *r = 0;
      return 0;
    }
    s.p[0] = s.p[1];
  }
  *r = 1;
  return 0;
}

/* The distance of two polygons. */
int ty_polygon_distance(int na, const ty_point_t *pa, const ty_box_t *ba,
                        int nb, const ty_point_t *pb, const ty_box_t *bb,
                        double *r) {
  double min = 0.0;
  bool have_min = false;
  int ov;

  /* Polygons that overlap are at distance 0: a polygon in another too. */
  TRY(ty_polygon_overlap(na, pa, ba, nb, pb, bb, &ov));
  if (ov) {
    *r = 0.0;
    return 0;
  }
  /* Else the distance is that of two closed paths. */
  TRY(segments_distance(na, pa, 1, nb, pb, 1, &min, &have_min));
  *r = min;
  return 0;
}

/* The distance of a point from a polygon. */
int ty_point_polygon_distance(const ty_point_t *pt, int npts,
                              const ty_point_t *p, double *r) {
  double result, d;
  ty_lseg_t seg;
  int in = ty_point_in_polygon(pt, npts, p);

  if (in < 0) {
    return -1;
  }
  if (in != 0) {
    *r = 0.0;
    return 0;
  }
  /* The segment from the first point to the last, then the others. */
  seg.p[0] = p[0];
  seg.p[1] = p[npts - 1];
  TRY(ty_internal::geo_seg_closept_point(&seg, pt, &result));
  for (int i = 0; i < npts - 1; i++) {
    seg.p[0] = p[i];
    seg.p[1] = p[i + 1];
    TRY(ty_internal::geo_seg_closept_point(&seg, pt, &d));
    if (ty_float64_lt(d, result)) {
      result = d;
    }
  }
  *r = result;
  return 0;
}

/* The distance of a circle from a polygon. */
int ty_circle_polygon_distance(const ty_circle_t *c, int npts,
                               const ty_point_t *p, double *r) {
  double d, result;

  TRY(ty_point_polygon_distance(&c->center, npts, p, &d));
  TRY(ty_float64_sub(d, c->radius, &result));
  *r = result < 0.0 ? 0.0 : result;
  return 0;
}

/*
 * The circle of a polygon.
 * 1. The center is the mean of the points.
 * 2. The radius is the mean distance of the points from the center.
 */
int ty_polygon_to_circle(int npts, const ty_point_t *p, ty_circle_t *r) {
  ty_circle_t t;
  double d;

  t.center.x = 0;
  t.center.y = 0;
  t.radius = 0;
  for (int i = 0; i < npts; i++) {
    TRY(ty_point_add(&t.center, &p[i], &t.center));
  }
  TRY(ty_float64_div(t.center.x, npts, &t.center.x));
  TRY(ty_float64_div(t.center.y, npts, &t.center.y));
  for (int i = 0; i < npts; i++) {
    TRY(pt_dt(&p[i], &t.center, &d));
    TRY(ty_float64_add(t.radius, d, &t.radius));
  }
  TRY(ty_float64_div(t.radius, npts, &t.radius));
  *r = t;
  return 0;
}

/* The points of a polygon of npts sides around a circle. */
int ty_circle_to_polygon_points(int npts, const ty_circle_t *c,
                                ty_point_t *out) {
  double anglestep, angle, m;

  TRY(ty_float64_div(2.0 * M_PI, npts, &anglestep));
  for (int i = 0; i < npts; i++) {
    TRY(ty_float64_mul(anglestep, i, &angle));
    TRY(ty_float64_mul(c->radius, cos(angle), &m));
    TRY(ty_float64_sub(c->center.x, m, &out[i].x));
    TRY(ty_float64_mul(c->radius, sin(angle), &m));
    TRY(ty_float64_add(c->center.y, m, &out[i].y));
  }
  return 0;
}

/* The area of a closed path: half the sum of the cross products. */
int ty_path_area(int npts, const ty_point_t *p, double *r) {
  double area = 0.0, m;

  for (int i = 0; i < npts; i++) {
    int j = (i + 1) % npts;

    TRY(ty_float64_mul(p[i].x, p[j].y, &m));
    TRY(ty_float64_add(area, m, &area));
    TRY(ty_float64_mul(p[i].y, p[j].x, &m));
    TRY(ty_float64_sub(area, m, &area));
  }
  return ty_float64_div(fabs(area), 2.0, r);
}

/* The length of a path. */
int ty_path_length(int npts, const ty_point_t *p, int closed, double *r) {
  double result = 0.0, d;

  for (int i = 0; i < npts; i++) {
    int iprev;

    if (!edge(i, npts, closed, &iprev)) {
      continue;
    }
    TRY(pt_dt(&p[iprev], &p[i], &d));
    TRY(ty_float64_add(result, d, &result));
  }
  *r = result;
  return 0;
}

/*
 * Two paths meet.
 * 1. The boxes that bound them must overlap.
 * 2. Else try each segment of one with each of the other.
 */
int ty_path_intersect(int na, const ty_point_t *pa, int closeda, int nb,
                      const ty_point_t *pb, int closedb, int *r) {
  ty_box_t b1, b2;
  ty_lseg_t s1, s2;
  int hit;

  /* The larger and the smaller, in the order of PostgreSQL's code. */
  b1.high.x = b1.low.x = pa[0].x;
  b1.high.y = b1.low.y = pa[0].y;
  for (int i = 1; i < na; i++) {
    b1.high.x = ty_float64_larger(pa[i].x, b1.high.x);
    b1.high.y = ty_float64_larger(pa[i].y, b1.high.y);
    b1.low.x = ty_float64_smaller(pa[i].x, b1.low.x);
    b1.low.y = ty_float64_smaller(pa[i].y, b1.low.y);
  }
  b2.high.x = b2.low.x = pb[0].x;
  b2.high.y = b2.low.y = pb[0].y;
  for (int i = 1; i < nb; i++) {
    b2.high.x = ty_float64_larger(pb[i].x, b2.high.x);
    b2.high.y = ty_float64_larger(pb[i].y, b2.high.y);
    b2.low.x = ty_float64_smaller(pb[i].x, b2.low.x);
    b2.low.y = ty_float64_smaller(pb[i].y, b2.low.y);
  }
  if (!ty_box_overlap(&b1, &b2)) {
    *r = 0;
    return 0;
  }
  for (int i = 0; i < na; i++) {
    int iprev;

    if (!edge(i, na, closeda, &iprev)) {
      continue;
    }
    for (int j = 0; j < nb; j++) {
      int jprev;

      if (!edge(j, nb, closedb, &jprev)) {
        continue;
      }
      ty_lseg_construct(&s1, &pa[iprev], &pa[i]);
      ty_lseg_construct(&s2, &pb[jprev], &pb[j]);
      hit = ty_internal::geo_seg_interpt_seg(&s1, &s2, nullptr);
      if (hit < 0) {
        return -1;
      }
      if (hit) {
        *r = 1;
        return 0;
      }
    }
  }
  *r = 0;
  return 0;
}

/* The distance of two paths. */
int ty_path_distance(int na, const ty_point_t *pa, int closeda, int nb,
                     const ty_point_t *pb, int closedb, double *r, int *hit) {
  double min = 0.0;
  bool have_min = false;

  TRY(segments_distance(na, pa, closeda, nb, pb, closedb, &min, &have_min));
  *hit = have_min;
  *r = min;
  return 0;
}

/* The distance of a point from the nearest segment of a path. */
int ty_point_path_distance(const ty_point_t *pt, int npts, const ty_point_t *p,
                           int closed, double *r) {
  double result = 0.0, tmp;
  bool have_min = false;
  ty_lseg_t seg;

  for (int i = 0; i < npts; i++) {
    int iprev;

    if (!edge(i, npts, closed, &iprev)) {
      continue;
    }
    ty_lseg_construct(&seg, &p[iprev], &p[i]);
    TRY(ty_internal::geo_seg_closept_point(&seg, pt, &tmp));
    if (!have_min || ty_float64_lt(tmp, result)) {
      result = tmp;
      have_min = true;
    }
  }
  *r = result;
  return 0;
}

/*
 * The point is on a path. An open path: on a segment, by the triangle
 * inequality. A closed path: in it, or on it.
 */
int ty_point_on_path(const ty_point_t *pt, int npts, const ty_point_t *p,
                     int closed, int *r) {
  if (!closed) {
    double a, b, s, d;

    TRY(pt_dt(pt, &p[0], &a));
    for (int i = 0; i < npts - 1; i++) {
      TRY(pt_dt(pt, &p[i + 1], &b));
      TRY(ty_float64_add(a, b, &s));
      TRY(pt_dt(&p[i], &p[i + 1], &d));
      if (ty_fp_eq(s, d)) {
        *r = 1;
        return 0;
      }
      a = b;
    }
    *r = 0;
    return 0;
  }
  int in = ty_point_in_polygon(pt, npts, p);

  if (in < 0) {
    return -1;
  }
  *r = in != 0;
  return 0;
}

/* Each point plus the point. */
int ty_points_add(int npts, ty_point_t *p, const ty_point_t *pt) {
  for (int i = 0; i < npts; i++) {
    TRY(ty_point_add(&p[i], pt, &p[i]));
  }
  return 0;
}

/* Each point minus the point. */
int ty_points_sub(int npts, ty_point_t *p, const ty_point_t *pt) {
  for (int i = 0; i < npts; i++) {
    TRY(ty_point_sub(&p[i], pt, &p[i]));
  }
  return 0;
}

/* Each point times the point. */
int ty_points_mul(int npts, ty_point_t *p, const ty_point_t *pt) {
  for (int i = 0; i < npts; i++) {
    TRY(ty_point_mul(&p[i], pt, &p[i]));
  }
  return 0;
}

/* Each point over the point. */
int ty_points_div(int npts, ty_point_t *p, const ty_point_t *pt) {
  for (int i = 0; i < npts; i++) {
    TRY(ty_point_div(&p[i], pt, &p[i]));
  }
  return 0;
}

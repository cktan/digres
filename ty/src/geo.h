/*
 * Copyright (c) 2026 CK Tan
 * SPDX-License-Identifier: MIT
 */

/*
 * geo.h -- the functions of geo_lseg.cc that the polygon code uses. Each
 * returns -1 on an error, and 1 for a hit or 0 for none where it says so.
 */
#ifndef TY_SRC_GEO_H
#define TY_SRC_GEO_H

#include "ty_geo.h"

namespace ty_internal {

/* The segments meet: 1 or 0; the point is in result if it is not NULL. */
int geo_seg_interpt_seg(const ty_lseg_t *a, const ty_lseg_t *b,
                        ty_point_t *result);

/* The point is on the segment: 1 or 0. */
int geo_seg_has(const ty_lseg_t *s, const ty_point_t *p);

/* The distance of two segments, and of a point from a segment. */
int geo_seg_closept_seg(const ty_lseg_t *on, const ty_lseg_t *to, double *dist);
int geo_seg_closept_point(const ty_lseg_t *seg, const ty_point_t *pt,
                          double *dist);

} // namespace ty_internal

#endif /* TY_SRC_GEO_H */

/*-------------------------------------------------------------------------
 *
 * geo_ops.c
 *	  2D geometric operations
 *
 * This module implements the geometric functions and operators.  The
 * geometric types are (from simple to more complicated):
 *
 * - point
 * - line
 * - line segment
 * - box
 * - circle
 * - polygon
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 * Portions Copyright (c) 2026, CK Tan: the changes for digres,
 * under the MIT License (https://github.com/cktan/digres/blob/main/LICENSE)
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/geo_ops.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <math.h>
#include <limits.h>
#include <float.h>
#include <ctype.h>

#include "libpq/pqformat.h"
#include "miscadmin.h"
#include "nodes/miscnodes.h"
#include "utils/float.h"
#include "utils/fmgrprotos.h"
#include "utils/geo_decls.h"
#include "varatt.h"

/*
 * * Type constructors have this form:
 *   void type_construct(Type *result, ...);
 *
 * * Operators commonly have signatures such as
 *   void type1_operator_type2(Type *result, Type1 *obj1, Type2 *obj2);
 *
 * Common operators are:
 * * Intersection point:
 *   bool type1_interpt_type2(Point *result, Type1 *obj1, Type2 *obj2);
 *		Return whether the two objects intersect. If *result is not
 *NULL, it is set to the intersection point.
 *
 * * Containment:
 *   bool type1_contain_type2(Type1 *obj1, Type2 *obj2);
 *		Return whether obj1 contains obj2.
 *   bool type1_contain_type2(Type1 *contains_obj, Type1 *contained_obj);
 *		Return whether obj1 contains obj2 (used when types are the same)
 *
 * * Distance of closest point in or on obj1 to obj2:
 *   float8 type1_closept_type2(Point *result, Type1 *obj1, Type2 *obj2);
 *		Returns the shortest distance between two objects.  If *result
 *is not NULL, it is set to the closest point in or on obj1 to obj2.
 *
 * These functions may be used to implement multiple SQL-level operators.  For
 * example, determining whether two lines are parallel is done by checking
 * whether they don't intersect.
 */

/*
 * Internal routines
 */

enum path_delim { PATH_NONE, PATH_OPEN, PATH_CLOSED };

/*
 * The functions that call ty pass the address of a scalar and have no array
 * on their stack: a stack canary protects nothing in them, and costs a few
 * instructions in each call.
 */
#define GEO_NO_CANARY __attribute__((no_stack_protector))

/* The structs have the layout of ty's: a call passes them through these. */
#define TYP(p) ((ty_point_t *)(p))
#define TYL(p) ((ty_line_t *)(p))
#define TYS(p) ((ty_lseg_t *)(p))
#define TYB(p) ((ty_box_t *)(p))
#define TYC(p) ((ty_circle_t *)(p))
#define TYPS(p) ((ty_point_t *)(p))

/*
 * ty's long tests of polygons call this, to stop for a cancel request or a
 * stack that is too deep. Only an interrupt that can be served stops one.
 */
static int geo_poll(void) {
  return (INTERRUPTS_PENDING_CONDITION() && INTERRUPTS_CAN_BE_PROCESSED()) ||
         stack_is_too_deep();
}

/* Routines for points */
static inline void point_construct(Point *result, float8 x, float8 y);
static inline void point_add_point(Point *result, Point *pt1, Point *pt2);
static inline void point_sub_point(Point *result, Point *pt1, Point *pt2);
static inline void point_mul_point(Point *result, Point *pt1, Point *pt2);
static inline void point_div_point(Point *result, Point *pt1, Point *pt2);
static inline bool point_eq_point(Point *pt1, Point *pt2);
static inline float8 point_dt(Point *pt1, Point *pt2);
static inline float8 point_sl(Point *pt1, Point *pt2);
static int point_inside(Point *p, int npts, Point *plist);

/* Routines for line segments */
static inline void statlseg_construct(LSEG *lseg, Point *pt1, Point *pt2);
static bool lseg_interpt_lseg(Point *result, LSEG *l1, LSEG *l2);
static bool lseg_contain_point(LSEG *lseg, Point *pt);
static float8 lseg_closept_point(Point *result, LSEG *lseg, Point *pt);
static float8 lseg_closept_lseg(Point *result, LSEG *on_lseg, LSEG *to_lseg);

/* Routines for boxes */
static inline void box_construct(BOX *result, Point *pt1, Point *pt2);
static void box_cn(Point *center, BOX *box);
static bool box_ov(BOX *box1, BOX *box2);
static float8 box_ar(BOX *box);
static float8 box_ht(BOX *box);
static float8 box_wd(BOX *box);
static bool box_contain_box(BOX *contains_box, BOX *contained_box);

/* Routines for circles */
static float8 circle_ar(CIRCLE *circle);

/* Routines for polygons */
static void make_bound_box(POLYGON *poly);
static void poly_to_circle(CIRCLE *result, POLYGON *poly);
static bool poly_contain_poly(POLYGON *contains_poly, POLYGON *contained_poly);
static float8 dist_ppoly_internal(Point *pt, POLYGON *poly);

/* Routines for encoding and decoding */
static bool single_decode(char *num, float8 *x, char **endptr_p,
                          const char *type_name, const char *orig_string,
                          Node *escontext);
static void single_encode(float8 x, StringInfo str);
static bool pair_decode(char *str, float8 *x, float8 *y, char **endptr_p,
                        const char *type_name, const char *orig_string,
                        Node *escontext);
static void pair_encode(float8 x, float8 y, StringInfo str);
static int pair_count(char *s, char delim);
static bool path_decode(char *str, bool opentype, int npts, Point *p,
                        bool *isopen, char **endptr_p, const char *type_name,
                        const char *orig_string, Node *escontext);
static char *path_encode(enum path_delim path_delim, int npts, Point *pt);

/*
 * Delimiters for input and output strings.
 * LDELIM, RDELIM, and DELIM are left, right, and separator delimiters,
 * respectively. LDELIM_EP, RDELIM_EP are left and right delimiters for paths
 * with endpoints.
 */

#define LDELIM			'('
#define RDELIM			')'
#define DELIM			','
#define LDELIM_EP		'['
#define RDELIM_EP		']'
#define LDELIM_C		'<'
#define RDELIM_C		'>'
#define LDELIM_L		'{'
#define RDELIM_L		'}'

/*
 * Geometric data types are composed of points.
 * This code tries to support a common format throughout the data types,
 *	to allow for more predictable usage and data type conversion.
 * The fundamental unit is the point. Other units are line segments,
 *	open paths, boxes, closed paths, and polygons (which should be
 *considered non-intersecting closed paths).
 *
 * Data representation is as follows:
 *	point:				(x,y)
 *	line segment:		[(x1,y1),(x2,y2)]
 *	box:				(x1,y1),(x2,y2)
 *	open path:			[(x1,y1),...,(xn,yn)]
 *	closed path:		((x1,y1),...,(xn,yn))
 *	polygon:			((x1,y1),...,(xn,yn))
 *
 * For boxes, the points are opposite corners with the first point at the top
 *right. For closed paths and polygons, the points should be reordered to allow
 *	fast and correct equality comparisons.
 *
 * XXX perhaps points in complex shapes should be reordered internally
 *	to allow faster internal operations, but should keep track of input
 *order and restore that order for text output - tgl 97/01/16
 */

static bool single_decode(char *num, float8 *x, char **endptr_p,
                          const char *type_name, const char *orig_string,
                          Node *escontext) {
  *x = float8in_internal(num, endptr_p, type_name, orig_string, escontext);
  return (!SOFT_ERROR_OCCURRED(escontext));
} /* single_decode() */

static void single_encode(float8 x, StringInfo str) {
  char *xstr = float8out_internal(x);

  appendStringInfoString(str, xstr);
  pfree(xstr);
} /* single_encode() */

static bool pair_decode(char *str, float8 *x, float8 *y, char **endptr_p,
                        const char *type_name, const char *orig_string,
                        Node *escontext) {
  bool has_delim;

  while (isspace((unsigned char)*str))
    str++;
  if ((has_delim = (*str == LDELIM)))
    str++;

  if (!single_decode(str, x, &str, type_name, orig_string, escontext))
    return false;

  if (*str++ != DELIM)
    goto fail;

  if (!single_decode(str, y, &str, type_name, orig_string, escontext))
    return false;

  if (has_delim) {
    if (*str++ != RDELIM)
      goto fail;
    while (isspace((unsigned char)*str))
      str++;
  }

  /* report stopping point if wanted, else complain if not end of string */
  if (endptr_p)
    *endptr_p = str;
  else if (*str != '\0')
    goto fail;
  return true;

fail:
  ereturn(escontext, false,
          (errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
           errmsg("invalid input syntax for type %s: \"%s\"", type_name,
                  orig_string)));
}

static void pair_encode(float8 x, float8 y, StringInfo str) {
  char *xstr = float8out_internal(x);
  char *ystr = float8out_internal(y);

  appendStringInfo(str, "%s,%s", xstr, ystr);
  pfree(xstr);
  pfree(ystr);
}

static bool path_decode(char *str, bool opentype, int npts, Point *p,
                        bool *isopen, char **endptr_p, const char *type_name,
                        const char *orig_string, Node *escontext) {
  int depth = 0;
  char *cp;
  int i;

  while (isspace((unsigned char)*str))
    str++;
  if ((*isopen = (*str == LDELIM_EP))) {
    /* no open delimiter allowed? */
    if (!opentype)
      goto fail;
    depth++;
    str++;
  } else if (*str == LDELIM) {
    cp = (str + 1);
    while (isspace((unsigned char)*cp))
      cp++;
    if (*cp == LDELIM) {
      depth++;
      str = cp;
    } else if (strrchr(str, LDELIM) == str) {
      depth++;
      str = cp;
    }
  }

  for (i = 0; i < npts; i++) {
    if (!pair_decode(str, &(p->x), &(p->y), &str, type_name, orig_string,
                     escontext))
      return false;
    if (*str == DELIM)
      str++;
    p++;
  }

  while (depth > 0) {
    if (*str == RDELIM || (*str == RDELIM_EP && *isopen && depth == 1)) {
      depth--;
      str++;
      while (isspace((unsigned char)*str))
        str++;
    } else
      goto fail;
  }

  /* report stopping point if wanted, else complain if not end of string */
  if (endptr_p)
    *endptr_p = str;
  else if (*str != '\0')
    goto fail;
  return true;

fail:
  ereturn(escontext, false,
          (errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
           errmsg("invalid input syntax for type %s: \"%s\"", type_name,
                  orig_string)));
} /* path_decode() */

static char *path_encode(enum path_delim path_delim, int npts, Point *pt) {
  StringInfoData str;
  int i;

  initStringInfo(&str);

  switch (path_delim) {
  case PATH_CLOSED:
    appendStringInfoChar(&str, LDELIM);
    break;
  case PATH_OPEN:
    appendStringInfoChar(&str, LDELIM_EP);
    break;
  case PATH_NONE:
    break;
  }

  for (i = 0; i < npts; i++) {
    if (i > 0)
      appendStringInfoChar(&str, DELIM);
    appendStringInfoChar(&str, LDELIM);
    pair_encode(pt->x, pt->y, &str);
    appendStringInfoChar(&str, RDELIM);
    pt++;
  }

  switch (path_delim) {
  case PATH_CLOSED:
    appendStringInfoChar(&str, RDELIM);
    break;
  case PATH_OPEN:
    appendStringInfoChar(&str, RDELIM_EP);
    break;
  case PATH_NONE:
    break;
  }

  return str.data;
} /* path_encode() */

/*-------------------------------------------------------------
 * pair_count - count the number of points
 * allow the following notation:
 * '((1,2),(3,4))'
 * '(1,3,2,4)'
 * require an odd number of delim characters in the string
 *-------------------------------------------------------------*/
static int pair_count(char *s, char delim) {
  int ndelim = 0;

  while ((s = strchr(s, delim)) != NULL) {
    ndelim++;
    s++;
  }
  return (ndelim % 2) ? ((ndelim + 1) / 2) : -1;
}

/***********************************************************************
 **
 **		Routines for two-dimensional boxes.
 **
 ***********************************************************************/

/*----------------------------------------------------------
 * Formatting and conversion routines.
 *---------------------------------------------------------*/

/*		box_in	-		convert a string to internal form.
 *
 *		External format: (two corners of box)
 *				"(f8, f8), (f8, f8)"
 *				also supports the older style "(f8, f8, f8, f8)"
 */
Datum box_in(PG_FUNCTION_ARGS) {
  char *str = PG_GETARG_CSTRING(0);
  Node *escontext = fcinfo->context;
  BOX *box = (BOX *)palloc(sizeof(BOX));
  bool isopen;
  float8 x, y;

  if (!path_decode(str, false, 2, &(box->high), &isopen, NULL, "box", str,
                   escontext))
    PG_RETURN_NULL();

  /* reorder corners if necessary... */
  /* ty puts the corners in order */
  ty_box_normalize((ty_box_t *)box);

  PG_RETURN_BOX_P(box);
}

/*		box_out -		convert a box to external form.
 */
Datum box_out(PG_FUNCTION_ARGS) {
  BOX *box = PG_GETARG_BOX_P(0);

  PG_RETURN_CSTRING(path_encode(PATH_NONE, 2, &(box->high)));
}

/*
 *		box_recv			- converts external binary
 *format to box
 */
Datum box_recv(PG_FUNCTION_ARGS) {
  StringInfo buf = (StringInfo)PG_GETARG_POINTER(0);
  BOX *box;
  float8 x, y;

  box = (BOX *)palloc(sizeof(BOX));

  box->high.x = pq_getmsgfloat8(buf);
  box->high.y = pq_getmsgfloat8(buf);
  box->low.x = pq_getmsgfloat8(buf);
  box->low.y = pq_getmsgfloat8(buf);

  /* reorder corners if necessary... */
  /* ty puts the corners in order */
  ty_box_normalize((ty_box_t *)box);

  PG_RETURN_BOX_P(box);
}

/*
 *		box_send			- converts box to binary format
 */
Datum box_send(PG_FUNCTION_ARGS) {
  BOX *box = PG_GETARG_BOX_P(0);
  StringInfoData buf;

  pq_begintypsend(&buf);
  pq_sendfloat8(&buf, box->high.x);
  pq_sendfloat8(&buf, box->high.y);
  pq_sendfloat8(&buf, box->low.x);
  pq_sendfloat8(&buf, box->low.y);
  PG_RETURN_BYTEA_P(pq_endtypsend(&buf));
}

/*		box_construct	-		fill in a new box.
 */
static inline void box_construct(BOX *result, Point *pt1, Point *pt2) {
  ty_box_construct((ty_box_t *)result, (ty_point_t *)pt1, (ty_point_t *)pt2);
}

/*----------------------------------------------------------
 *	Relational operators for BOXes.
 *		<, >, <=, >=, and == are based on box area.
 *---------------------------------------------------------*/

/*		box_same		-		are two boxes identical?
 */
Datum box_same(PG_FUNCTION_ARGS) {
  BOX *box1 = PG_GETARG_BOX_P(0);
  BOX *box2 = PG_GETARG_BOX_P(1);

  PG_RETURN_BOOL(point_eq_point(&box1->high, &box2->high) &&
                 point_eq_point(&box1->low, &box2->low));
}

/*		box_overlap		-		does box1 overlap box2?
 */
Datum box_overlap(PG_FUNCTION_ARGS) {
  BOX *box1 = PG_GETARG_BOX_P(0);
  BOX *box2 = PG_GETARG_BOX_P(1);

  PG_RETURN_BOOL(box_ov(box1, box2));
}

static bool box_ov(BOX *box1, BOX *box2) {
  return ty_box_overlap((ty_box_t *)box1, (ty_box_t *)box2);
}

/*		box_left		-		is box1 strictly left of
 * box2?
 */
Datum box_left(PG_FUNCTION_ARGS) {
  BOX *box1 = PG_GETARG_BOX_P(0);
  BOX *box2 = PG_GETARG_BOX_P(1);

  PG_RETURN_BOOL(FPlt(box1->high.x, box2->low.x));
}

/*		box_overleft	-		is the right edge of box1 at or
 *left of the right edge of box2?
 *
 *		This is "less than or equal" for the end of a time range,
 *		when time ranges are stored as rectangles.
 */
Datum box_overleft(PG_FUNCTION_ARGS) {
  BOX *box1 = PG_GETARG_BOX_P(0);
  BOX *box2 = PG_GETARG_BOX_P(1);

  PG_RETURN_BOOL(FPle(box1->high.x, box2->high.x));
}

/*		box_right		-		is box1 strictly right
 * of box2?
 */
Datum box_right(PG_FUNCTION_ARGS) {
  BOX *box1 = PG_GETARG_BOX_P(0);
  BOX *box2 = PG_GETARG_BOX_P(1);

  PG_RETURN_BOOL(FPgt(box1->low.x, box2->high.x));
}

/*		box_overright	-		is the left edge of box1 at or
 *right of the left edge of box2?
 *
 *		This is "greater than or equal" for time ranges, when time
 *ranges are stored as rectangles.
 */
Datum box_overright(PG_FUNCTION_ARGS) {
  BOX *box1 = PG_GETARG_BOX_P(0);
  BOX *box2 = PG_GETARG_BOX_P(1);

  PG_RETURN_BOOL(FPge(box1->low.x, box2->low.x));
}

/*		box_below		-		is box1 strictly below
 * box2?
 */
Datum box_below(PG_FUNCTION_ARGS) {
  BOX *box1 = PG_GETARG_BOX_P(0);
  BOX *box2 = PG_GETARG_BOX_P(1);

  PG_RETURN_BOOL(FPlt(box1->high.y, box2->low.y));
}

/*		box_overbelow	-		is the upper edge of box1 at or
 *below the upper edge of box2?
 */
Datum box_overbelow(PG_FUNCTION_ARGS) {
  BOX *box1 = PG_GETARG_BOX_P(0);
  BOX *box2 = PG_GETARG_BOX_P(1);

  PG_RETURN_BOOL(FPle(box1->high.y, box2->high.y));
}

/*		box_above		-		is box1 strictly above
 * box2?
 */
Datum box_above(PG_FUNCTION_ARGS) {
  BOX *box1 = PG_GETARG_BOX_P(0);
  BOX *box2 = PG_GETARG_BOX_P(1);

  PG_RETURN_BOOL(FPgt(box1->low.y, box2->high.y));
}

/*		box_overabove	-		is the lower edge of box1 at or
 *above the lower edge of box2?
 */
Datum box_overabove(PG_FUNCTION_ARGS) {
  BOX *box1 = PG_GETARG_BOX_P(0);
  BOX *box2 = PG_GETARG_BOX_P(1);

  PG_RETURN_BOOL(FPge(box1->low.y, box2->low.y));
}

/*		box_contained	-		is box1 contained by box2?
 */
Datum box_contained(PG_FUNCTION_ARGS) {
  BOX *box1 = PG_GETARG_BOX_P(0);
  BOX *box2 = PG_GETARG_BOX_P(1);

  PG_RETURN_BOOL(box_contain_box(box2, box1));
}

/*		box_contain		-		does box1 contain box2?
 */
Datum box_contain(PG_FUNCTION_ARGS) {
  BOX *box1 = PG_GETARG_BOX_P(0);
  BOX *box2 = PG_GETARG_BOX_P(1);

  PG_RETURN_BOOL(box_contain_box(box1, box2));
}

/*
 * Check whether the second box is in the first box or on its border
 */
static bool box_contain_box(BOX *contains_box, BOX *contained_box) {
  return ty_box_contain_box((ty_box_t *)contains_box, (ty_box_t *)contained_box);
}

/*		box_positionop	-
 *				is box1 entirely {above,below} box2?
 *
 * box_below_eq and box_above_eq are obsolete versions that (probably
 * erroneously) accept the equal-boundaries case.  Since these are not
 * in sync with the box_left and box_right code, they are deprecated and
 * not supported in the PG 8.1 rtree operator class extension.
 */
Datum box_below_eq(PG_FUNCTION_ARGS) {
  BOX *box1 = PG_GETARG_BOX_P(0);
  BOX *box2 = PG_GETARG_BOX_P(1);

  PG_RETURN_BOOL(FPle(box1->high.y, box2->low.y));
}

Datum box_above_eq(PG_FUNCTION_ARGS) {
  BOX *box1 = PG_GETARG_BOX_P(0);
  BOX *box2 = PG_GETARG_BOX_P(1);

  PG_RETURN_BOOL(FPge(box1->low.y, box2->high.y));
}

/*		box_relop		-		is area(box1) relop
 *area(box2), within our accuracy constraint?
 */
Datum box_lt(PG_FUNCTION_ARGS) {
  BOX *box1 = PG_GETARG_BOX_P(0);
  BOX *box2 = PG_GETARG_BOX_P(1);

  PG_RETURN_BOOL(FPlt(box_ar(box1), box_ar(box2)));
}

Datum box_gt(PG_FUNCTION_ARGS) {
  BOX *box1 = PG_GETARG_BOX_P(0);
  BOX *box2 = PG_GETARG_BOX_P(1);

  PG_RETURN_BOOL(FPgt(box_ar(box1), box_ar(box2)));
}

Datum box_eq(PG_FUNCTION_ARGS) {
  BOX *box1 = PG_GETARG_BOX_P(0);
  BOX *box2 = PG_GETARG_BOX_P(1);

  PG_RETURN_BOOL(FPeq(box_ar(box1), box_ar(box2)));
}

Datum box_le(PG_FUNCTION_ARGS) {
  BOX *box1 = PG_GETARG_BOX_P(0);
  BOX *box2 = PG_GETARG_BOX_P(1);

  PG_RETURN_BOOL(FPle(box_ar(box1), box_ar(box2)));
}

Datum box_ge(PG_FUNCTION_ARGS) {
  BOX *box1 = PG_GETARG_BOX_P(0);
  BOX *box2 = PG_GETARG_BOX_P(1);

  PG_RETURN_BOOL(FPge(box_ar(box1), box_ar(box2)));
}

/*----------------------------------------------------------
 *	"Arithmetic" operators on boxes.
 *---------------------------------------------------------*/

/*		box_area		-		returns the area of the
 * box.
 */
Datum box_area(PG_FUNCTION_ARGS) {
  BOX *box = PG_GETARG_BOX_P(0);

  PG_RETURN_FLOAT8(box_ar(box));
}

/*		box_width		-		returns the width of the
 *box (horizontal magnitude).
 */
Datum box_width(PG_FUNCTION_ARGS) {
  BOX *box = PG_GETARG_BOX_P(0);

  PG_RETURN_FLOAT8(box_wd(box));
}

/*		box_height		-		returns the height of
 *the box (vertical magnitude).
 */
Datum box_height(PG_FUNCTION_ARGS) {
  BOX *box = PG_GETARG_BOX_P(0);

  PG_RETURN_FLOAT8(box_ht(box));
}

/*		box_distance	-		returns the distance between the
 *								  center points
 *of two boxes.
 */
Datum box_distance(PG_FUNCTION_ARGS) {
  BOX *box1 = PG_GETARG_BOX_P(0);
  BOX *box2 = PG_GETARG_BOX_P(1);
  Point a, b;

  box_cn(&a, box1);
  box_cn(&b, box2);

  PG_RETURN_FLOAT8(point_dt(&a, &b));
}

/*		box_center		-		returns the center point
 * of the box.
 */
Datum box_center(PG_FUNCTION_ARGS) {
  BOX *box = PG_GETARG_BOX_P(0);
  Point *result = (Point *)palloc(sizeof(Point));

  box_cn(result, box);

  PG_RETURN_POINT_P(result);
}

/*		box_ar	-		returns the area of the box.
 */
static float8 box_ar(BOX *box) {
  float8 r;

  if (ty_box_area((ty_box_t *)box, &r))
    float_raise_ty();
  return r;
}

/*		box_cn	-		stores the centerpoint of the box into
 * *center.
 */
static void box_cn(Point *center, BOX *box) {
  if (ty_box_center((ty_box_t *)box, (ty_point_t *)center))
    float_raise_ty();
}

/*		box_wd	-		returns the width (length) of the box
 *								  (horizontal
 *magnitude).
 */
static float8 box_wd(BOX *box) {
  float8 r;

  if (ty_box_width((ty_box_t *)box, &r))
    float_raise_ty();
  return r;
}

/*		box_ht	-		returns the height of the box
 *								  (vertical
 *magnitude).
 */
static float8 box_ht(BOX *box) {
  float8 r;

  if (ty_box_height((ty_box_t *)box, &r))
    float_raise_ty();
  return r;
}

/*----------------------------------------------------------
 *	Funky operations.
 *---------------------------------------------------------*/

/*		box_intersect	-
 *				returns the overlapping portion of two boxes,
 *				  or NULL if they do not intersect.
 */
Datum box_intersect(PG_FUNCTION_ARGS) {
  BOX *box1 = PG_GETARG_BOX_P(0);
  BOX *box2 = PG_GETARG_BOX_P(1);
  BOX *result = (BOX *)palloc(sizeof(BOX));

  /* ty finds the common part, if the boxes overlap */
  if (!ty_box_intersect((ty_box_t *)box1, (ty_box_t *)box2, (ty_box_t *)result))
    PG_RETURN_NULL();

  PG_RETURN_BOX_P(result);
}

/*		box_diagonal	-
 *				returns a line segment which happens to be the
 *				  positive-slope diagonal of "box".
 */
GEO_NO_CANARY Datum box_diagonal(PG_FUNCTION_ARGS) {
  BOX *box = PG_GETARG_BOX_P(0);
  LSEG *result = (LSEG *)palloc(sizeof(LSEG));

  ty_box_diagonal(TYB(box), TYS(result));

  PG_RETURN_LSEG_P(result);
}

/***********************************************************************
 **
 **		Routines for 2D lines.
 **
 ***********************************************************************/

static bool line_decode(char *s, const char *str, LINE *line, Node *escontext) {
  /* s was already advanced over leading '{' */
  if (!single_decode(s, &line->A, &s, "line", str, escontext))
    return false;
  if (*s++ != DELIM)
    goto fail;
  if (!single_decode(s, &line->B, &s, "line", str, escontext))
    return false;
  if (*s++ != DELIM)
    goto fail;
  if (!single_decode(s, &line->C, &s, "line", str, escontext))
    return false;
  if (*s++ != RDELIM_L)
    goto fail;
  while (isspace((unsigned char)*s))
    s++;
  if (*s != '\0')
    goto fail;
  return true;

fail:
  ereturn(escontext, false,
          (errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
           errmsg("invalid input syntax for type %s: \"%s\"", "line", str)));
}

Datum line_in(PG_FUNCTION_ARGS) {
  char *str = PG_GETARG_CSTRING(0);
  Node *escontext = fcinfo->context;
  LINE *line = (LINE *)palloc(sizeof(LINE));
  LSEG lseg;
  bool isopen;
  char *s;

  s = str;
  while (isspace((unsigned char)*s))
    s++;
  if (*s == LDELIM_L) {
    if (!line_decode(s + 1, str, line, escontext))
      PG_RETURN_NULL();
    if (!ty_line_valid(TYL(line)))
      ereturn(
          escontext, (Datum)0,
          (errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
           errmsg("invalid line specification: A and B cannot both be zero")));
  } else {
    if (!path_decode(s, true, 2, &lseg.p[0], &isopen, NULL, "line", str,
                     escontext))
      PG_RETURN_NULL();
    if (point_eq_point(&lseg.p[0], &lseg.p[1]))
      ereturn(
          escontext, (Datum)0,
          (errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
           errmsg("invalid line specification: must be two distinct points")));

    /*
     * XXX lseg_sl() and line_construct() can throw overflow/underflow
     * errors.  Eventually we should allow those to be soft, but the
     * notational pain seems to outweigh the value for now.
     */
    if (ty_line_from_lseg(TYS(&lseg), TYL(line)))
      float_raise_ty();
  }

  PG_RETURN_LINE_P(line);
}

Datum line_out(PG_FUNCTION_ARGS) {
  LINE *line = PG_GETARG_LINE_P(0);
  char *astr = float8out_internal(line->A);
  char *bstr = float8out_internal(line->B);
  char *cstr = float8out_internal(line->C);

  PG_RETURN_CSTRING(psprintf("%c%s%c%s%c%s%c", LDELIM_L, astr, DELIM, bstr,
                             DELIM, cstr, RDELIM_L));
}

/*
 *		line_recv			- converts external binary
 *format to line
 */
Datum line_recv(PG_FUNCTION_ARGS) {
  StringInfo buf = (StringInfo)PG_GETARG_POINTER(0);
  LINE *line;

  line = (LINE *)palloc(sizeof(LINE));

  line->A = pq_getmsgfloat8(buf);
  line->B = pq_getmsgfloat8(buf);
  line->C = pq_getmsgfloat8(buf);

  if (!ty_line_valid(TYL(line)))
    ereport(
        ERROR,
        (errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
         errmsg("invalid line specification: A and B cannot both be zero")));

  PG_RETURN_LINE_P(line);
}

/*
 *		line_send			- converts line to binary format
 */
Datum line_send(PG_FUNCTION_ARGS) {
  LINE *line = PG_GETARG_LINE_P(0);
  StringInfoData buf;

  pq_begintypsend(&buf);
  pq_sendfloat8(&buf, line->A);
  pq_sendfloat8(&buf, line->B);
  pq_sendfloat8(&buf, line->C);
  PG_RETURN_BYTEA_P(pq_endtypsend(&buf));
}

/*----------------------------------------------------------
 *	Conversion routines from one line formula to internal.
 *		Internal form:	Ax+By+C=0
 *---------------------------------------------------------*/


/* line_construct_pp()
 * two points
 */
GEO_NO_CANARY Datum line_construct_pp(PG_FUNCTION_ARGS) {
  Point *pt1 = PG_GETARG_POINT_P(0);
  Point *pt2 = PG_GETARG_POINT_P(1);
  LINE *result = (LINE *)palloc(sizeof(LINE));

  if (ty_line_from_points(TYP(pt1), TYP(pt2), TYL(result))) {
    if (ty_errnum() == TY_ERR_INVALID)
      ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                      errmsg("%s", ty_errmsg())));
    float_raise_ty();
  }

  PG_RETURN_LINE_P(result);
}

/*----------------------------------------------------------
 *	Relative position routines.
 *---------------------------------------------------------*/

GEO_NO_CANARY Datum line_intersect(PG_FUNCTION_ARGS) {
  LINE *a0 = PG_GETARG_LINE_P(0);
  LINE *a1 = PG_GETARG_LINE_P(1);
  int r;

  if (ty_line_intersect(TYL(a0), TYL(a1), &r))
    float_raise_ty();
  PG_RETURN_BOOL(r);
}

GEO_NO_CANARY Datum line_parallel(PG_FUNCTION_ARGS) {
  LINE *a0 = PG_GETARG_LINE_P(0);
  LINE *a1 = PG_GETARG_LINE_P(1);
  int r;

  if (ty_line_parallel(TYL(a0), TYL(a1), &r))
    float_raise_ty();
  PG_RETURN_BOOL(r);
}

GEO_NO_CANARY Datum line_perp(PG_FUNCTION_ARGS) {
  LINE *a0 = PG_GETARG_LINE_P(0);
  LINE *a1 = PG_GETARG_LINE_P(1);
  int r;

  if (ty_line_perp(TYL(a0), TYL(a1), &r))
    float_raise_ty();
  PG_RETURN_BOOL(r);
}

GEO_NO_CANARY Datum line_vertical(PG_FUNCTION_ARGS) {
  LINE *a0 = PG_GETARG_LINE_P(0);

  PG_RETURN_BOOL(ty_line_vertical(TYL(a0)));
}

GEO_NO_CANARY Datum line_horizontal(PG_FUNCTION_ARGS) {
  LINE *a0 = PG_GETARG_LINE_P(0);

  PG_RETURN_BOOL(ty_line_horizontal(TYL(a0)));
}

/*
 * Check whether the two lines are the same
 */
GEO_NO_CANARY Datum line_eq(PG_FUNCTION_ARGS) {
  LINE *a0 = PG_GETARG_LINE_P(0);
  LINE *a1 = PG_GETARG_LINE_P(1);
  int r;

  if (ty_line_eq(TYL(a0), TYL(a1), &r))
    float_raise_ty();
  PG_RETURN_BOOL(r);
}

/*----------------------------------------------------------
 *	Line arithmetic routines.
 *---------------------------------------------------------*/



/* line_distance()
 * Distance between two lines.
 */
GEO_NO_CANARY Datum line_distance(PG_FUNCTION_ARGS) {
  LINE *a0 = PG_GETARG_LINE_P(0);
  LINE *a1 = PG_GETARG_LINE_P(1);
  float8 r;

  if (ty_line_distance(TYL(a0), TYL(a1), &r))
    float_raise_ty();
  PG_RETURN_FLOAT8(r);
}

/* line_interpt()
 * Point where two lines l1, l2 intersect (if any)
 */
GEO_NO_CANARY Datum line_interpt(PG_FUNCTION_ARGS) {
  LINE *l1 = PG_GETARG_LINE_P(0);
  LINE *l2 = PG_GETARG_LINE_P(1);
  Point *result = (Point *)palloc(sizeof(Point));
  int hit;

  if (ty_line_interpt(TYL(l1), TYL(l2), TYP(result), &hit))
    float_raise_ty();
  if (!hit)
    PG_RETURN_NULL();
  PG_RETURN_POINT_P(result);
}


/***********************************************************************
 **
 **		Routines for 2D paths (sequences of line segments, also
 **				called `polylines').
 **
 **				This is not a general package for geometric
 *paths, *				which of course include polygons; the
 *emphasis here *				is on (for example) usefulness
 *in wire layout.
 **
 ***********************************************************************/

/*----------------------------------------------------------
 *	String to path / path to string conversion.
 *		External format:
 *				"((xcoord, ycoord),... )"
 *				"[(xcoord, ycoord),... ]"
 *				"(xcoord, ycoord),... "
 *				"[xcoord, ycoord,... ]"
 *		Also support older format:
 *				"(closed, npts, xcoord, ycoord,... )"
 *---------------------------------------------------------*/

Datum path_area(PG_FUNCTION_ARGS) {
  PATH *path = PG_GETARG_PATH_P(0);
  float8 area;

  if (!path->closed)
    PG_RETURN_NULL();

  if (ty_path_area(path->npts, TYPS(path->p), &area))
    float_raise_ty();

  PG_RETURN_FLOAT8(area);
}

Datum path_in(PG_FUNCTION_ARGS) {
  char *str = PG_GETARG_CSTRING(0);
  Node *escontext = fcinfo->context;
  PATH *path;
  bool isopen;
  char *s;
  int npts;
  int size;
  int base_size;
  int depth = 0;

  if ((npts = pair_count(str, ',')) <= 0)
    ereturn(escontext, (Datum)0,
            (errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
             errmsg("invalid input syntax for type %s: \"%s\"", "path", str)));

  s = str;
  while (isspace((unsigned char)*s))
    s++;

  /* skip single leading paren */
  if ((*s == LDELIM) && (strrchr(s, LDELIM) == s)) {
    s++;
    depth++;
  }

  base_size = sizeof(path->p[0]) * npts;
  size = offsetof(PATH, p) + base_size;

  /* Check for integer overflow */
  if (base_size / npts != sizeof(path->p[0]) || size <= base_size)
    ereturn(escontext, (Datum)0,
            (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
             errmsg("too many points requested")));

  path = (PATH *)palloc(size);

  SET_VARSIZE(path, size);
  path->npts = npts;

  if (!path_decode(s, true, npts, &(path->p[0]), &isopen, &s, "path", str,
                   escontext))
    PG_RETURN_NULL();

  if (depth >= 1) {
    if (*s++ != RDELIM)
      ereturn(
          escontext, (Datum)0,
          (errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
           errmsg("invalid input syntax for type %s: \"%s\"", "path", str)));
    while (isspace((unsigned char)*s))
      s++;
  }
  if (*s != '\0')
    ereturn(escontext, (Datum)0,
            (errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
             errmsg("invalid input syntax for type %s: \"%s\"", "path", str)));

  path->closed = (!isopen);
  /* prevent instability in unused pad bytes */
  path->dummy = 0;

  PG_RETURN_PATH_P(path);
}

Datum path_out(PG_FUNCTION_ARGS) {
  PATH *path = PG_GETARG_PATH_P(0);

  PG_RETURN_CSTRING(
      path_encode(path->closed ? PATH_CLOSED : PATH_OPEN, path->npts, path->p));
}

/*
 *		path_recv			- converts external binary
 *format to path
 *
 * External representation is closed flag (a boolean byte), int32 number
 * of points, and the points.
 */
Datum path_recv(PG_FUNCTION_ARGS) {
  StringInfo buf = (StringInfo)PG_GETARG_POINTER(0);
  PATH *path;
  int closed;
  int32 npts;
  int32 i;
  int size;

  closed = pq_getmsgbyte(buf);
  npts = pq_getmsgint(buf, sizeof(int32));
  if (npts <= 0 ||
      npts >= (int32)((INT_MAX - offsetof(PATH, p)) / sizeof(Point)))
    ereport(ERROR,
            (errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
             errmsg("invalid number of points in external \"path\" value")));

  size = offsetof(PATH, p) + sizeof(path->p[0]) * npts;
  path = (PATH *)palloc(size);

  SET_VARSIZE(path, size);
  path->npts = npts;
  path->closed = (closed ? 1 : 0);
  /* prevent instability in unused pad bytes */
  path->dummy = 0;

  for (i = 0; i < npts; i++) {
    path->p[i].x = pq_getmsgfloat8(buf);
    path->p[i].y = pq_getmsgfloat8(buf);
  }

  PG_RETURN_PATH_P(path);
}

/*
 *		path_send			- converts path to binary format
 */
Datum path_send(PG_FUNCTION_ARGS) {
  PATH *path = PG_GETARG_PATH_P(0);
  StringInfoData buf;
  int32 i;

  pq_begintypsend(&buf);
  pq_sendbyte(&buf, path->closed ? 1 : 0);
  pq_sendint32(&buf, path->npts);
  for (i = 0; i < path->npts; i++) {
    pq_sendfloat8(&buf, path->p[i].x);
    pq_sendfloat8(&buf, path->p[i].y);
  }
  PG_RETURN_BYTEA_P(pq_endtypsend(&buf));
}

/*----------------------------------------------------------
 *	Relational operators.
 *		These are based on the path cardinality,
 *		as stupid as that sounds.
 *
 *		Better relops and access methods coming soon.
 *---------------------------------------------------------*/

Datum path_n_lt(PG_FUNCTION_ARGS) {
  PATH *p1 = PG_GETARG_PATH_P(0);
  PATH *p2 = PG_GETARG_PATH_P(1);

  PG_RETURN_BOOL(p1->npts < p2->npts);
}

Datum path_n_gt(PG_FUNCTION_ARGS) {
  PATH *p1 = PG_GETARG_PATH_P(0);
  PATH *p2 = PG_GETARG_PATH_P(1);

  PG_RETURN_BOOL(p1->npts > p2->npts);
}

Datum path_n_eq(PG_FUNCTION_ARGS) {
  PATH *p1 = PG_GETARG_PATH_P(0);
  PATH *p2 = PG_GETARG_PATH_P(1);

  PG_RETURN_BOOL(p1->npts == p2->npts);
}

Datum path_n_le(PG_FUNCTION_ARGS) {
  PATH *p1 = PG_GETARG_PATH_P(0);
  PATH *p2 = PG_GETARG_PATH_P(1);

  PG_RETURN_BOOL(p1->npts <= p2->npts);
}

Datum path_n_ge(PG_FUNCTION_ARGS) {
  PATH *p1 = PG_GETARG_PATH_P(0);
  PATH *p2 = PG_GETARG_PATH_P(1);

  PG_RETURN_BOOL(p1->npts >= p2->npts);
}

/*----------------------------------------------------------
 * Conversion operators.
 *---------------------------------------------------------*/

Datum path_isclosed(PG_FUNCTION_ARGS) {
  PATH *path = PG_GETARG_PATH_P(0);

  PG_RETURN_BOOL(path->closed);
}

Datum path_isopen(PG_FUNCTION_ARGS) {
  PATH *path = PG_GETARG_PATH_P(0);

  PG_RETURN_BOOL(!path->closed);
}

Datum path_npoints(PG_FUNCTION_ARGS) {
  PATH *path = PG_GETARG_PATH_P(0);

  PG_RETURN_INT32(path->npts);
}

Datum path_close(PG_FUNCTION_ARGS) {
  PATH *path = PG_GETARG_PATH_P_COPY(0);

  path->closed = true;

  PG_RETURN_PATH_P(path);
}

Datum path_open(PG_FUNCTION_ARGS) {
  PATH *path = PG_GETARG_PATH_P_COPY(0);

  path->closed = false;

  PG_RETURN_PATH_P(path);
}

/* path_inter -
 *		Does p1 intersect p2 at any point?
 *		Use bounding boxes for a quick (O(n)) check, then do a
 *		O(n^2) iterative edge check.
 */
Datum path_inter(PG_FUNCTION_ARGS) {
  PATH *p1 = PG_GETARG_PATH_P(0);
  PATH *p2 = PG_GETARG_PATH_P(1);
  int r;

  Assert(p1->npts > 0 && p2->npts > 0);
  if (ty_path_intersect(p1->npts, TYPS(p1->p), p1->closed, p2->npts, TYPS(p2->p), p2->closed, &r))
    float_raise_ty();

  PG_RETURN_BOOL(r);
}

/* path_distance()
 * This essentially does a cartesian product of the lsegs in the
 *	two paths, and finds the min distance between any two lsegs
 */
Datum path_distance(PG_FUNCTION_ARGS) {
  PATH *p1 = PG_GETARG_PATH_P(0);
  PATH *p2 = PG_GETARG_PATH_P(1);
  float8 r;
  int hit;

  if (ty_path_distance(p1->npts, TYPS(p1->p), p1->closed, p2->npts, TYPS(p2->p), p2->closed, &r, &hit))
    float_raise_ty();
  if (!hit)
    PG_RETURN_NULL();
  PG_RETURN_FLOAT8(r);
}

/*----------------------------------------------------------
 *	"Arithmetic" operations.
 *---------------------------------------------------------*/

Datum path_length(PG_FUNCTION_ARGS) {
  PATH *path = PG_GETARG_PATH_P(0);
  float8 r;

  if (ty_path_length(path->npts, TYPS(path->p), path->closed, &r))
    float_raise_ty();

  PG_RETURN_FLOAT8(r);
}

/***********************************************************************
 **
 **		Routines for 2D points.
 **
 ***********************************************************************/

/*----------------------------------------------------------
 *	String to point, point to string conversion.
 *		External format:
 *				"(x,y)"
 *				"x,y"
 *---------------------------------------------------------*/

Datum point_in(PG_FUNCTION_ARGS) {
  char *str = PG_GETARG_CSTRING(0);
  Point *point = (Point *)palloc(sizeof(Point));

  /* Ignore failure from pair_decode, since our return value won't matter */
  pair_decode(str, &point->x, &point->y, NULL, "point", str, fcinfo->context);
  PG_RETURN_POINT_P(point);
}

Datum point_out(PG_FUNCTION_ARGS) {
  Point *pt = PG_GETARG_POINT_P(0);

  PG_RETURN_CSTRING(path_encode(PATH_NONE, 1, pt));
}

/*
 *		point_recv			- converts external binary
 *format to point
 */
Datum point_recv(PG_FUNCTION_ARGS) {
  StringInfo buf = (StringInfo)PG_GETARG_POINTER(0);
  Point *point;

  point = (Point *)palloc(sizeof(Point));
  point->x = pq_getmsgfloat8(buf);
  point->y = pq_getmsgfloat8(buf);
  PG_RETURN_POINT_P(point);
}

/*
 *		point_send			- converts point to binary
 *format
 */
Datum point_send(PG_FUNCTION_ARGS) {
  Point *pt = PG_GETARG_POINT_P(0);
  StringInfoData buf;

  pq_begintypsend(&buf);
  pq_sendfloat8(&buf, pt->x);
  pq_sendfloat8(&buf, pt->y);
  PG_RETURN_BYTEA_P(pq_endtypsend(&buf));
}

/*
 * Initialize a point
 */
static inline void point_construct(Point *result, float8 x, float8 y) {
  result->x = x;
  result->y = y;
}

/*----------------------------------------------------------
 *	Relational operators for Points.
 *		Since we do have a sense of coordinates being
 *		"equal" to a given accuracy (point_vert, point_horiz),
 *		the other ops must preserve that sense.  This means
 *		that results may, strictly speaking, be a lie (unless
 *		EPSILON = 0.0).
 *---------------------------------------------------------*/

Datum point_left(PG_FUNCTION_ARGS) {
  Point *pt1 = PG_GETARG_POINT_P(0);
  Point *pt2 = PG_GETARG_POINT_P(1);

  PG_RETURN_BOOL(FPlt(pt1->x, pt2->x));
}

Datum point_right(PG_FUNCTION_ARGS) {
  Point *pt1 = PG_GETARG_POINT_P(0);
  Point *pt2 = PG_GETARG_POINT_P(1);

  PG_RETURN_BOOL(FPgt(pt1->x, pt2->x));
}

Datum point_above(PG_FUNCTION_ARGS) {
  Point *pt1 = PG_GETARG_POINT_P(0);
  Point *pt2 = PG_GETARG_POINT_P(1);

  PG_RETURN_BOOL(FPgt(pt1->y, pt2->y));
}

Datum point_below(PG_FUNCTION_ARGS) {
  Point *pt1 = PG_GETARG_POINT_P(0);
  Point *pt2 = PG_GETARG_POINT_P(1);

  PG_RETURN_BOOL(FPlt(pt1->y, pt2->y));
}

Datum point_vert(PG_FUNCTION_ARGS) {
  Point *pt1 = PG_GETARG_POINT_P(0);
  Point *pt2 = PG_GETARG_POINT_P(1);

  PG_RETURN_BOOL(FPeq(pt1->x, pt2->x));
}

Datum point_horiz(PG_FUNCTION_ARGS) {
  Point *pt1 = PG_GETARG_POINT_P(0);
  Point *pt2 = PG_GETARG_POINT_P(1);

  PG_RETURN_BOOL(FPeq(pt1->y, pt2->y));
}

Datum point_eq(PG_FUNCTION_ARGS) {
  Point *pt1 = PG_GETARG_POINT_P(0);
  Point *pt2 = PG_GETARG_POINT_P(1);

  PG_RETURN_BOOL(point_eq_point(pt1, pt2));
}

Datum point_ne(PG_FUNCTION_ARGS) {
  Point *pt1 = PG_GETARG_POINT_P(0);
  Point *pt2 = PG_GETARG_POINT_P(1);

  PG_RETURN_BOOL(!point_eq_point(pt1, pt2));
}

/*
 * Check whether the two points are the same
 */
static inline bool point_eq_point(Point *pt1, Point *pt2) {
  return ty_point_eq((ty_point_t *)pt1, (ty_point_t *)pt2);
}

/*----------------------------------------------------------
 *	"Arithmetic" operators on points.
 *---------------------------------------------------------*/

Datum point_distance(PG_FUNCTION_ARGS) {
  Point *pt1 = PG_GETARG_POINT_P(0);
  Point *pt2 = PG_GETARG_POINT_P(1);

  PG_RETURN_FLOAT8(point_dt(pt1, pt2));
}

static inline float8 point_dt(Point *pt1, Point *pt2) {
  float8 r;

  if (ty_point_distance((ty_point_t *)pt1, (ty_point_t *)pt2, &r))
    float_raise_ty();
  return r;
}

Datum point_slope(PG_FUNCTION_ARGS) {
  Point *pt1 = PG_GETARG_POINT_P(0);
  Point *pt2 = PG_GETARG_POINT_P(1);

  PG_RETURN_FLOAT8(point_sl(pt1, pt2));
}

/*
 * Return slope of two points
 *
 * Note that this function returns Inf when the points are the same.
 */
static inline float8 point_sl(Point *pt1, Point *pt2) {
  float8 r;

  if (ty_point_slope((ty_point_t *)pt1, (ty_point_t *)pt2, &r))
    float_raise_ty();
  return r;
}

/*
 * Return inverse slope of two points
 *
 * Note that this function returns 0.0 when the points are the same.
 */
static inline float8 point_invsl(Point *pt1, Point *pt2) {
  float8 r;

  if (ty_point_invslope((ty_point_t *)pt1, (ty_point_t *)pt2, &r))
    float_raise_ty();
  return r;
}

/***********************************************************************
 **
 **		Routines for 2D line segments.
 **
 ***********************************************************************/

/*----------------------------------------------------------
 *	String to lseg, lseg to string conversion.
 *		External forms: "[(x1, y1), (x2, y2)]"
 *						"(x1, y1), (x2, y2)"
 *						"x1, y1, x2, y2"
 *		closed form ok	"((x1, y1), (x2, y2))"
 *		(old form)		"(x1, y1, x2, y2)"
 *---------------------------------------------------------*/

Datum lseg_in(PG_FUNCTION_ARGS) {
  char *str = PG_GETARG_CSTRING(0);
  Node *escontext = fcinfo->context;
  LSEG *lseg = (LSEG *)palloc(sizeof(LSEG));
  bool isopen;

  if (!path_decode(str, true, 2, &lseg->p[0], &isopen, NULL, "lseg", str,
                   escontext))
    PG_RETURN_NULL();

  PG_RETURN_LSEG_P(lseg);
}

Datum lseg_out(PG_FUNCTION_ARGS) {
  LSEG *ls = PG_GETARG_LSEG_P(0);

  PG_RETURN_CSTRING(path_encode(PATH_OPEN, 2, &ls->p[0]));
}

/*
 *		lseg_recv			- converts external binary
 *format to lseg
 */
Datum lseg_recv(PG_FUNCTION_ARGS) {
  StringInfo buf = (StringInfo)PG_GETARG_POINTER(0);
  LSEG *lseg;

  lseg = (LSEG *)palloc(sizeof(LSEG));

  lseg->p[0].x = pq_getmsgfloat8(buf);
  lseg->p[0].y = pq_getmsgfloat8(buf);
  lseg->p[1].x = pq_getmsgfloat8(buf);
  lseg->p[1].y = pq_getmsgfloat8(buf);

  PG_RETURN_LSEG_P(lseg);
}

/*
 *		lseg_send			- converts lseg to binary format
 */
Datum lseg_send(PG_FUNCTION_ARGS) {
  LSEG *ls = PG_GETARG_LSEG_P(0);
  StringInfoData buf;

  pq_begintypsend(&buf);
  pq_sendfloat8(&buf, ls->p[0].x);
  pq_sendfloat8(&buf, ls->p[0].y);
  pq_sendfloat8(&buf, ls->p[1].x);
  pq_sendfloat8(&buf, ls->p[1].y);
  PG_RETURN_BYTEA_P(pq_endtypsend(&buf));
}

/* lseg_construct -
 *		form a LSEG from two Points.
 */
Datum lseg_construct(PG_FUNCTION_ARGS) {
  Point *pt1 = PG_GETARG_POINT_P(0);
  Point *pt2 = PG_GETARG_POINT_P(1);
  LSEG *result = (LSEG *)palloc(sizeof(LSEG));

  statlseg_construct(result, pt1, pt2);

  PG_RETURN_LSEG_P(result);
}

/* like lseg_construct, but assume space already allocated */
static inline void statlseg_construct(LSEG *lseg, Point *pt1, Point *pt2) {
  ty_lseg_construct(TYS(lseg), TYP(pt1), TYP(pt2));
}



GEO_NO_CANARY Datum lseg_length(PG_FUNCTION_ARGS) {
  LSEG *a0 = PG_GETARG_LSEG_P(0);
  float8 r;

  if (ty_lseg_length(TYS(a0), &r))
    float_raise_ty();
  PG_RETURN_FLOAT8(r);
}

/*----------------------------------------------------------
 *	Relative position routines.
 *---------------------------------------------------------*/

/*
 **  find intersection of the two lines, and see if it falls on
 **  both segments.
 */
GEO_NO_CANARY Datum lseg_intersect(PG_FUNCTION_ARGS) {
  LSEG *a0 = PG_GETARG_LSEG_P(0);
  LSEG *a1 = PG_GETARG_LSEG_P(1);
  int r;

  if (ty_lseg_intersect(TYS(a0), TYS(a1), &r))
    float_raise_ty();
  PG_RETURN_BOOL(r);
}

GEO_NO_CANARY Datum lseg_parallel(PG_FUNCTION_ARGS) {
  LSEG *a0 = PG_GETARG_LSEG_P(0);
  LSEG *a1 = PG_GETARG_LSEG_P(1);
  int r;

  if (ty_lseg_parallel(TYS(a0), TYS(a1), &r))
    float_raise_ty();
  PG_RETURN_BOOL(r);
}

/*
 * Determine if two line segments are perpendicular.
 */
GEO_NO_CANARY Datum lseg_perp(PG_FUNCTION_ARGS) {
  LSEG *a0 = PG_GETARG_LSEG_P(0);
  LSEG *a1 = PG_GETARG_LSEG_P(1);
  int r;

  if (ty_lseg_perp(TYS(a0), TYS(a1), &r))
    float_raise_ty();
  PG_RETURN_BOOL(r);
}

GEO_NO_CANARY Datum lseg_vertical(PG_FUNCTION_ARGS) {
  LSEG *a0 = PG_GETARG_LSEG_P(0);

  PG_RETURN_BOOL(ty_lseg_vertical(TYS(a0)));
}

GEO_NO_CANARY Datum lseg_horizontal(PG_FUNCTION_ARGS) {
  LSEG *a0 = PG_GETARG_LSEG_P(0);

  PG_RETURN_BOOL(ty_lseg_horizontal(TYS(a0)));
}

GEO_NO_CANARY Datum lseg_eq(PG_FUNCTION_ARGS) {
  LSEG *a0 = PG_GETARG_LSEG_P(0);
  LSEG *a1 = PG_GETARG_LSEG_P(1);

  PG_RETURN_BOOL(ty_lseg_eq(TYS(a0), TYS(a1)));
}

GEO_NO_CANARY Datum lseg_ne(PG_FUNCTION_ARGS) {
  LSEG *a0 = PG_GETARG_LSEG_P(0);
  LSEG *a1 = PG_GETARG_LSEG_P(1);

  PG_RETURN_BOOL(!ty_lseg_eq(TYS(a0), TYS(a1)));
}

GEO_NO_CANARY Datum lseg_lt(PG_FUNCTION_ARGS) {
  LSEG *a0 = PG_GETARG_LSEG_P(0);
  LSEG *a1 = PG_GETARG_LSEG_P(1);
  int r;

  if (ty_lseg_lt(TYS(a0), TYS(a1), &r))
    float_raise_ty();
  PG_RETURN_BOOL(r);
}

GEO_NO_CANARY Datum lseg_le(PG_FUNCTION_ARGS) {
  LSEG *a0 = PG_GETARG_LSEG_P(0);
  LSEG *a1 = PG_GETARG_LSEG_P(1);
  int r;

  if (ty_lseg_le(TYS(a0), TYS(a1), &r))
    float_raise_ty();
  PG_RETURN_BOOL(r);
}

GEO_NO_CANARY Datum lseg_gt(PG_FUNCTION_ARGS) {
  LSEG *a0 = PG_GETARG_LSEG_P(0);
  LSEG *a1 = PG_GETARG_LSEG_P(1);
  int r;

  if (ty_lseg_gt(TYS(a0), TYS(a1), &r))
    float_raise_ty();
  PG_RETURN_BOOL(r);
}

GEO_NO_CANARY Datum lseg_ge(PG_FUNCTION_ARGS) {
  LSEG *a0 = PG_GETARG_LSEG_P(0);
  LSEG *a1 = PG_GETARG_LSEG_P(1);
  int r;

  if (ty_lseg_ge(TYS(a0), TYS(a1), &r))
    float_raise_ty();
  PG_RETURN_BOOL(r);
}

/*----------------------------------------------------------
 *	Line arithmetic routines.
 *---------------------------------------------------------*/

/* lseg_distance -
 *		If two segments don't intersect, then the closest
 *		point will be from one of the endpoints to the other
 *		segment.
 */
GEO_NO_CANARY Datum lseg_distance(PG_FUNCTION_ARGS) {
  LSEG *a0 = PG_GETARG_LSEG_P(0);
  LSEG *a1 = PG_GETARG_LSEG_P(1);
  float8 d;

  if (ty_lseg_lseg_distance(TYS(a0), TYS(a1), NULL, &d))
    float_raise_ty();
  PG_RETURN_FLOAT8(d);
}

GEO_NO_CANARY Datum lseg_center(PG_FUNCTION_ARGS) {
  LSEG *lseg = PG_GETARG_LSEG_P(0);
  Point *result = (Point *)palloc(sizeof(Point));

  if (ty_lseg_center(TYS(lseg), TYP(result)))
    float_raise_ty();

  PG_RETURN_POINT_P(result);
}

/*
 * Return whether the two segments intersect. If *result is not NULL,
 * it is set to the intersection point.
 *
 * This function is almost perfectly symmetric, even though it doesn't look
 * like it.  See lseg_interpt_line() for the other half of it.
 */
static bool lseg_interpt_lseg(Point *result, LSEG *l1, LSEG *l2) {
  int hit;

  if (ty_lseg_interpt(TYS(l1), TYS(l2), TYP(result), &hit))
    float_raise_ty();
  return hit;
}

GEO_NO_CANARY Datum lseg_interpt(PG_FUNCTION_ARGS) {
  LSEG *l1 = PG_GETARG_LSEG_P(0);
  LSEG *l2 = PG_GETARG_LSEG_P(1);
  Point *result = (Point *)palloc(sizeof(Point));
  int hit;

  if (ty_lseg_interpt(TYS(l1), TYS(l2), TYP(result), &hit))
    float_raise_ty();
  if (!hit)
    PG_RETURN_NULL();
  PG_RETURN_POINT_P(result);
}

/***********************************************************************
 **
 **		Routines for position comparisons of differently-typed
 **				2D objects.
 **
 ***********************************************************************/

/*---------------------------------------------------------------------
 *		dist_
 *				Minimum distance from one object to another.
 *-------------------------------------------------------------------*/

/*
 * Distance from a point to a line
 */
GEO_NO_CANARY Datum dist_pl(PG_FUNCTION_ARGS) {
  Point *a0 = PG_GETARG_POINT_P(0);
  LINE *a1 = PG_GETARG_LINE_P(1);
  float8 d;

  if (ty_point_line_distance(TYP(a0), TYL(a1), NULL, &d))
    float_raise_ty();
  PG_RETURN_FLOAT8(d);
}

/*
 * Distance from a line to a point
 */
GEO_NO_CANARY Datum dist_lp(PG_FUNCTION_ARGS) {
  LINE *a0 = PG_GETARG_LINE_P(0);
  Point *a1 = PG_GETARG_POINT_P(1);
  float8 d;

  if (ty_point_line_distance(TYP(a1), TYL(a0), NULL, &d))
    float_raise_ty();
  PG_RETURN_FLOAT8(d);
}

/*
 * Distance from a point to a lseg
 */
GEO_NO_CANARY Datum dist_ps(PG_FUNCTION_ARGS) {
  Point *a0 = PG_GETARG_POINT_P(0);
  LSEG *a1 = PG_GETARG_LSEG_P(1);
  float8 d;

  if (ty_point_lseg_distance(TYP(a0), TYS(a1), NULL, &d))
    float_raise_ty();
  PG_RETURN_FLOAT8(d);
}

/*
 * Distance from a lseg to a point
 */
GEO_NO_CANARY Datum dist_sp(PG_FUNCTION_ARGS) {
  LSEG *a0 = PG_GETARG_LSEG_P(0);
  Point *a1 = PG_GETARG_POINT_P(1);
  float8 d;

  if (ty_point_lseg_distance(TYP(a1), TYS(a0), NULL, &d))
    float_raise_ty();
  PG_RETURN_FLOAT8(d);
}

static float8 dist_ppath_internal(Point *pt, PATH *path) {
  float8 r;

  Assert(path->npts > 0);
  if (ty_point_path_distance(TYP(pt), path->npts, TYPS(path->p), path->closed, &r))
    float_raise_ty();
  return r;
}

/*
 * Distance from a point to a path
 */
Datum dist_ppath(PG_FUNCTION_ARGS) {
  Point *pt = PG_GETARG_POINT_P(0);
  PATH *path = PG_GETARG_PATH_P(1);

  PG_RETURN_FLOAT8(dist_ppath_internal(pt, path));
}

/*
 * Distance from a path to a point
 */
Datum dist_pathp(PG_FUNCTION_ARGS) {
  PATH *path = PG_GETARG_PATH_P(0);
  Point *pt = PG_GETARG_POINT_P(1);

  PG_RETURN_FLOAT8(dist_ppath_internal(pt, path));
}

/*
 * Distance from a point to a box
 */
GEO_NO_CANARY Datum dist_pb(PG_FUNCTION_ARGS) {
  Point *a0 = PG_GETARG_POINT_P(0);
  BOX *a1 = PG_GETARG_BOX_P(1);
  float8 d;

  if (ty_point_box_distance(TYP(a0), TYB(a1), NULL, &d))
    float_raise_ty();
  PG_RETURN_FLOAT8(d);
}

/*
 * Distance from a box to a point
 */
GEO_NO_CANARY Datum dist_bp(PG_FUNCTION_ARGS) {
  BOX *a0 = PG_GETARG_BOX_P(0);
  Point *a1 = PG_GETARG_POINT_P(1);
  float8 d;

  if (ty_point_box_distance(TYP(a1), TYB(a0), NULL, &d))
    float_raise_ty();
  PG_RETURN_FLOAT8(d);
}

/*
 * Distance from a lseg to a line
 */
GEO_NO_CANARY Datum dist_sl(PG_FUNCTION_ARGS) {
  LSEG *a0 = PG_GETARG_LSEG_P(0);
  LINE *a1 = PG_GETARG_LINE_P(1);
  float8 d;

  if (ty_lseg_line_distance(TYS(a0), TYL(a1), NULL, &d))
    float_raise_ty();
  PG_RETURN_FLOAT8(d);
}

/*
 * Distance from a line to a lseg
 */
GEO_NO_CANARY Datum dist_ls(PG_FUNCTION_ARGS) {
  LINE *a0 = PG_GETARG_LINE_P(0);
  LSEG *a1 = PG_GETARG_LSEG_P(1);
  float8 d;

  if (ty_lseg_line_distance(TYS(a1), TYL(a0), NULL, &d))
    float_raise_ty();
  PG_RETURN_FLOAT8(d);
}

/*
 * Distance from a lseg to a box
 */
GEO_NO_CANARY Datum dist_sb(PG_FUNCTION_ARGS) {
  LSEG *a0 = PG_GETARG_LSEG_P(0);
  BOX *a1 = PG_GETARG_BOX_P(1);
  float8 d;

  if (ty_lseg_box_distance(TYS(a0), TYB(a1), NULL, &d))
    float_raise_ty();
  PG_RETURN_FLOAT8(d);
}

/*
 * Distance from a box to a lseg
 */
GEO_NO_CANARY Datum dist_bs(PG_FUNCTION_ARGS) {
  BOX *a0 = PG_GETARG_BOX_P(0);
  LSEG *a1 = PG_GETARG_LSEG_P(1);
  float8 d;

  if (ty_lseg_box_distance(TYS(a1), TYB(a0), NULL, &d))
    float_raise_ty();
  PG_RETURN_FLOAT8(d);
}

static float8 dist_cpoly_internal(CIRCLE *circle, POLYGON *poly) {
  float8 r;

  if (ty_circle_polygon_distance(TYC(circle), poly->npts, TYPS(poly->p), &r))
    float_raise_ty();
  return r;
}

/*
 * Distance from a circle to a polygon
 */
Datum dist_cpoly(PG_FUNCTION_ARGS) {
  CIRCLE *circle = PG_GETARG_CIRCLE_P(0);
  POLYGON *poly = PG_GETARG_POLYGON_P(1);

  PG_RETURN_FLOAT8(dist_cpoly_internal(circle, poly));
}

/*
 * Distance from a polygon to a circle
 */
Datum dist_polyc(PG_FUNCTION_ARGS) {
  POLYGON *poly = PG_GETARG_POLYGON_P(0);
  CIRCLE *circle = PG_GETARG_CIRCLE_P(1);

  PG_RETURN_FLOAT8(dist_cpoly_internal(circle, poly));
}

/*
 * Distance from a point to a polygon
 */
Datum dist_ppoly(PG_FUNCTION_ARGS) {
  Point *point = PG_GETARG_POINT_P(0);
  POLYGON *poly = PG_GETARG_POLYGON_P(1);

  PG_RETURN_FLOAT8(dist_ppoly_internal(point, poly));
}

Datum dist_polyp(PG_FUNCTION_ARGS) {
  POLYGON *poly = PG_GETARG_POLYGON_P(0);
  Point *point = PG_GETARG_POINT_P(1);

  PG_RETURN_FLOAT8(dist_ppoly_internal(point, poly));
}

static float8 dist_ppoly_internal(Point *pt, POLYGON *poly) {
  float8 r;

  if (ty_point_polygon_distance(TYP(pt), poly->npts, TYPS(poly->p), &r))
    float_raise_ty();
  return r;
}

/*---------------------------------------------------------------------
 *		interpt_
 *				Intersection point of objects.
 *				We choose to ignore the "point" of intersection
 *between lines and boxes, since there are typically two.
 *-------------------------------------------------------------------*/


/*---------------------------------------------------------------------
 *		close_
 *				Point of closest proximity between objects.
 *-------------------------------------------------------------------*/


GEO_NO_CANARY Datum close_pl(PG_FUNCTION_ARGS) {
  Point *a0 = PG_GETARG_POINT_P(0);
  LINE *a1 = PG_GETARG_LINE_P(1);
  Point *result = (Point *)palloc(sizeof(Point));
  int hit;

  if (ty_point_line_close(TYP(a0), TYL(a1), TYP(result), &hit))
    float_raise_ty();
  if (!hit)
    PG_RETURN_NULL();
  PG_RETURN_POINT_P(result);
}

/*
 * Closest point on line segment to specified point.
 *
 * If *result is not NULL, set it to the closest point on the line segment
 * to the point.  Returns the distance of the two points.
 */
static float8 lseg_closept_point(Point *result, LSEG *lseg, Point *pt) {
  float8 d;

  if (ty_point_lseg_distance(TYP(pt), TYS(lseg), TYP(result), &d))
    float_raise_ty();
  return d;
}

GEO_NO_CANARY Datum close_ps(PG_FUNCTION_ARGS) {
  Point *a0 = PG_GETARG_POINT_P(0);
  LSEG *a1 = PG_GETARG_LSEG_P(1);
  Point *result = (Point *)palloc(sizeof(Point));
  int hit;

  if (ty_point_lseg_close(TYP(a0), TYS(a1), TYP(result), &hit))
    float_raise_ty();
  if (!hit)
    PG_RETURN_NULL();
  PG_RETURN_POINT_P(result);
}

/*
 * Closest point on line segment to line segment
 */
static float8 lseg_closept_lseg(Point *result, LSEG *on_lseg, LSEG *to_lseg) {
  float8 d;

  if (ty_lseg_lseg_distance(TYS(on_lseg), TYS(to_lseg), TYP(result), &d))
    float_raise_ty();
  return d;
}

GEO_NO_CANARY Datum close_lseg(PG_FUNCTION_ARGS) {
  LSEG *a0 = PG_GETARG_LSEG_P(0);
  LSEG *a1 = PG_GETARG_LSEG_P(1);
  Point *result = (Point *)palloc(sizeof(Point));
  int hit;

  if (ty_lseg_lseg_close(TYS(a0), TYS(a1), TYP(result), &hit))
    float_raise_ty();
  if (!hit)
    PG_RETURN_NULL();
  PG_RETURN_POINT_P(result);
}


GEO_NO_CANARY Datum close_pb(PG_FUNCTION_ARGS) {
  Point *a0 = PG_GETARG_POINT_P(0);
  BOX *a1 = PG_GETARG_BOX_P(1);
  Point *result = (Point *)palloc(sizeof(Point));
  int hit;

  if (ty_point_box_close(TYP(a0), TYB(a1), TYP(result), &hit))
    float_raise_ty();
  if (!hit)
    PG_RETURN_NULL();
  PG_RETURN_POINT_P(result);
}


GEO_NO_CANARY Datum close_ls(PG_FUNCTION_ARGS) {
  LINE *a0 = PG_GETARG_LINE_P(0);
  LSEG *a1 = PG_GETARG_LSEG_P(1);
  Point *result = (Point *)palloc(sizeof(Point));
  int hit;

  if (ty_lseg_line_close(TYL(a0), TYS(a1), TYP(result), &hit))
    float_raise_ty();
  if (!hit)
    PG_RETURN_NULL();
  PG_RETURN_POINT_P(result);
}


GEO_NO_CANARY Datum close_sb(PG_FUNCTION_ARGS) {
  LSEG *a0 = PG_GETARG_LSEG_P(0);
  BOX *a1 = PG_GETARG_BOX_P(1);
  Point *result = (Point *)palloc(sizeof(Point));
  int hit;

  if (ty_lseg_box_close(TYS(a0), TYB(a1), TYP(result), &hit))
    float_raise_ty();
  if (!hit)
    PG_RETURN_NULL();
  PG_RETURN_POINT_P(result);
}

/*---------------------------------------------------------------------
 *		on_
 *				Whether one object lies completely within
 *another.
 *-------------------------------------------------------------------*/


GEO_NO_CANARY Datum on_pl(PG_FUNCTION_ARGS) {
  Point *a0 = PG_GETARG_POINT_P(0);
  LINE *a1 = PG_GETARG_LINE_P(1);
  int r;

  if (ty_line_contain_point(TYL(a1), TYP(a0), &r))
    float_raise_ty();
  PG_RETURN_BOOL(r);
}

/*
 *		Determine colinearity by detecting a triangle inequality.
 * This algorithm seems to behave nicely even with lsb residues - tgl 1997-07-09
 */
static bool lseg_contain_point(LSEG *lseg, Point *pt) {
  int r;

  if (ty_lseg_contain_point(TYS(lseg), TYP(pt), &r))
    float_raise_ty();
  return r;
}

GEO_NO_CANARY Datum on_ps(PG_FUNCTION_ARGS) {
  Point *a0 = PG_GETARG_POINT_P(0);
  LSEG *a1 = PG_GETARG_LSEG_P(1);
  int r;

  if (ty_lseg_contain_point(TYS(a1), TYP(a0), &r))
    float_raise_ty();
  PG_RETURN_BOOL(r);
}


GEO_NO_CANARY Datum on_pb(PG_FUNCTION_ARGS) {
  Point *a0 = PG_GETARG_POINT_P(0);
  BOX *a1 = PG_GETARG_BOX_P(1);

  PG_RETURN_BOOL(ty_box_contain_point(TYB(a1), TYP(a0)));
}

GEO_NO_CANARY Datum box_contain_pt(PG_FUNCTION_ARGS) {
  BOX *a0 = PG_GETARG_BOX_P(0);
  Point *a1 = PG_GETARG_POINT_P(1);

  PG_RETURN_BOOL(ty_box_contain_point(TYB(a0), TYP(a1)));
}

/* on_ppath -
 *		Whether a point lies within (on) a polyline.
 *		If open, we have to (groan) check each segment.
 * (uses same algorithm as for point intersecting segment - tgl 1997-07-09)
 *		If closed, we use the old O(n) ray method for point-in-polygon.
 *				The ray is horizontal, from pt out to the right.
 *				Each segment that crosses the ray counts as an
 *				intersection; note that an endpoint or edge may
 *touch but not cross. (we can do p-in-p in lg(n), but it takes preprocessing)
 */
Datum on_ppath(PG_FUNCTION_ARGS) {
  Point *pt = PG_GETARG_POINT_P(0);
  PATH *path = PG_GETARG_PATH_P(1);
  int r;

  if (ty_point_on_path(TYP(pt), path->npts, TYPS(path->p), path->closed, &r))
    float_raise_ty();

  PG_RETURN_BOOL(r);
}

/*
 * Check whether the line segment is on the line or close enough
 *
 * It is, if both of its points are on the line or close enough.
 */
GEO_NO_CANARY Datum on_sl(PG_FUNCTION_ARGS) {
  LSEG *a0 = PG_GETARG_LSEG_P(0);
  LINE *a1 = PG_GETARG_LINE_P(1);
  int r;

  if (ty_lseg_on_line(TYS(a0), TYL(a1), &r))
    float_raise_ty();
  PG_RETURN_BOOL(r);
}


GEO_NO_CANARY Datum on_sb(PG_FUNCTION_ARGS) {
  LSEG *a0 = PG_GETARG_LSEG_P(0);
  BOX *a1 = PG_GETARG_BOX_P(1);

  PG_RETURN_BOOL(ty_box_contain_lseg(TYB(a1), TYS(a0)));
}

/*---------------------------------------------------------------------
 *		inter_
 *				Whether one object intersects another.
 *-------------------------------------------------------------------*/

GEO_NO_CANARY Datum inter_sl(PG_FUNCTION_ARGS) {
  LSEG *lseg = PG_GETARG_LSEG_P(0);
  LINE *line = PG_GETARG_LINE_P(1);
  int hit;

  if (ty_lseg_interpt_line(TYS(lseg), TYL(line), NULL, &hit))
    float_raise_ty();

  PG_RETURN_BOOL(hit);
}


GEO_NO_CANARY Datum inter_sb(PG_FUNCTION_ARGS) {
  LSEG *lseg = PG_GETARG_LSEG_P(0);
  BOX *box = PG_GETARG_BOX_P(1);
  int hit;

  if (ty_box_interpt_lseg(TYB(box), TYS(lseg), NULL, &hit))
    float_raise_ty();

  PG_RETURN_BOOL(hit);
}

/* inter_lb()
 * Do line and box intersect?
 */
GEO_NO_CANARY Datum inter_lb(PG_FUNCTION_ARGS) {
  LINE *line = PG_GETARG_LINE_P(0);
  BOX *box = PG_GETARG_BOX_P(1);
  int hit;

  if (ty_box_interpt_line(TYB(box), TYL(line), &hit))
    float_raise_ty();

  PG_RETURN_BOOL(hit);
}

/*------------------------------------------------------------------
 * The following routines define a data type and operator class for
 * POLYGONS .... Part of which (the polygon's bounding box) is built on
 * top of the BOX data type.
 *
 * make_bound_box - create the bounding box for the input polygon
 *------------------------------------------------------------------*/

static void make_bound_box(POLYGON *poly) {
  Assert(poly->npts > 0);
  ty_polygon_bound_box(poly->npts, TYPS(poly->p), TYB(&poly->boundbox));
}

/*------------------------------------------------------------------
 * poly_in - read in the polygon from a string specification
 *
 *		External format:
 *				"((x0,y0),...,(xn,yn))"
 *				"x0,y0,...,xn,yn"
 *				also supports the older style
 *"(x1,...,xn,y1,...yn)"
 *------------------------------------------------------------------*/
Datum poly_in(PG_FUNCTION_ARGS) {
  char *str = PG_GETARG_CSTRING(0);
  Node *escontext = fcinfo->context;
  POLYGON *poly;
  int npts;
  int size;
  int base_size;
  bool isopen;

  if ((npts = pair_count(str, ',')) <= 0)
    ereturn(
        escontext, (Datum)0,
        (errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
         errmsg("invalid input syntax for type %s: \"%s\"", "polygon", str)));

  base_size = sizeof(poly->p[0]) * npts;
  size = offsetof(POLYGON, p) + base_size;

  /* Check for integer overflow */
  if (base_size / npts != sizeof(poly->p[0]) || size <= base_size)
    ereturn(escontext, (Datum)0,
            (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
             errmsg("too many points requested")));

  poly = (POLYGON *)palloc0(size); /* zero any holes */

  SET_VARSIZE(poly, size);
  poly->npts = npts;

  if (!path_decode(str, false, npts, &(poly->p[0]), &isopen, NULL, "polygon",
                   str, escontext))
    PG_RETURN_NULL();

  make_bound_box(poly);

  PG_RETURN_POLYGON_P(poly);
}

/*---------------------------------------------------------------
 * poly_out - convert internal POLYGON representation to the
 *			  character string format "((f8,f8),...,(f8,f8))"
 *---------------------------------------------------------------*/
Datum poly_out(PG_FUNCTION_ARGS) {
  POLYGON *poly = PG_GETARG_POLYGON_P(0);

  PG_RETURN_CSTRING(path_encode(PATH_CLOSED, poly->npts, poly->p));
}

/*
 *		poly_recv			- converts external binary
 *format to polygon
 *
 * External representation is int32 number of points, and the points.
 * We recompute the bounding box on read, instead of trusting it to
 * be valid.  (Checking it would take just as long, so may as well
 * omit it from external representation.)
 */
Datum poly_recv(PG_FUNCTION_ARGS) {
  StringInfo buf = (StringInfo)PG_GETARG_POINTER(0);
  POLYGON *poly;
  int32 npts;
  int32 i;
  int size;

  npts = pq_getmsgint(buf, sizeof(int32));
  if (npts <= 0 ||
      npts >= (int32)((INT_MAX - offsetof(POLYGON, p)) / sizeof(Point)))
    ereport(ERROR,
            (errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
             errmsg("invalid number of points in external \"polygon\" value")));

  size = offsetof(POLYGON, p) + sizeof(poly->p[0]) * npts;
  poly = (POLYGON *)palloc0(size); /* zero any holes */

  SET_VARSIZE(poly, size);
  poly->npts = npts;

  for (i = 0; i < npts; i++) {
    poly->p[i].x = pq_getmsgfloat8(buf);
    poly->p[i].y = pq_getmsgfloat8(buf);
  }

  make_bound_box(poly);

  PG_RETURN_POLYGON_P(poly);
}

/*
 *		poly_send			- converts polygon to binary
 *format
 */
Datum poly_send(PG_FUNCTION_ARGS) {
  POLYGON *poly = PG_GETARG_POLYGON_P(0);
  StringInfoData buf;
  int32 i;

  pq_begintypsend(&buf);
  pq_sendint32(&buf, poly->npts);
  for (i = 0; i < poly->npts; i++) {
    pq_sendfloat8(&buf, poly->p[i].x);
    pq_sendfloat8(&buf, poly->p[i].y);
  }
  PG_RETURN_BYTEA_P(pq_endtypsend(&buf));
}

/*-------------------------------------------------------
 * Is polygon A strictly left of polygon B? i.e. is
 * the right most point of A left of the left most point
 * of B?
 *-------------------------------------------------------*/
Datum poly_left(PG_FUNCTION_ARGS) {
  POLYGON *polya = PG_GETARG_POLYGON_P(0);
  POLYGON *polyb = PG_GETARG_POLYGON_P(1);
  bool result;

  result = ty_polygon_left(TYB(&polya->boundbox), TYB(&polyb->boundbox));

  /* Avoid leaking memory for toasted inputs: needed for rtree indexes. */
  PG_FREE_IF_COPY(polya, 0);
  PG_FREE_IF_COPY(polyb, 1);

  PG_RETURN_BOOL(result);
}

/*-------------------------------------------------------
 * Is polygon A overlapping or left of polygon B? i.e. is
 * the right most point of A at or left of the right most point
 * of B?
 *-------------------------------------------------------*/
Datum poly_overleft(PG_FUNCTION_ARGS) {
  POLYGON *polya = PG_GETARG_POLYGON_P(0);
  POLYGON *polyb = PG_GETARG_POLYGON_P(1);
  bool result;

  result = ty_polygon_overleft(TYB(&polya->boundbox), TYB(&polyb->boundbox));

  /* Avoid leaking memory for toasted inputs: needed for rtree indexes. */
  PG_FREE_IF_COPY(polya, 0);
  PG_FREE_IF_COPY(polyb, 1);

  PG_RETURN_BOOL(result);
}

/*-------------------------------------------------------
 * Is polygon A strictly right of polygon B? i.e. is
 * the left most point of A right of the right most point
 * of B?
 *-------------------------------------------------------*/
Datum poly_right(PG_FUNCTION_ARGS) {
  POLYGON *polya = PG_GETARG_POLYGON_P(0);
  POLYGON *polyb = PG_GETARG_POLYGON_P(1);
  bool result;

  result = ty_polygon_right(TYB(&polya->boundbox), TYB(&polyb->boundbox));

  /* Avoid leaking memory for toasted inputs: needed for rtree indexes. */
  PG_FREE_IF_COPY(polya, 0);
  PG_FREE_IF_COPY(polyb, 1);

  PG_RETURN_BOOL(result);
}

/*-------------------------------------------------------
 * Is polygon A overlapping or right of polygon B? i.e. is
 * the left most point of A at or right of the left most point
 * of B?
 *-------------------------------------------------------*/
Datum poly_overright(PG_FUNCTION_ARGS) {
  POLYGON *polya = PG_GETARG_POLYGON_P(0);
  POLYGON *polyb = PG_GETARG_POLYGON_P(1);
  bool result;

  result = ty_polygon_overright(TYB(&polya->boundbox), TYB(&polyb->boundbox));

  /* Avoid leaking memory for toasted inputs: needed for rtree indexes. */
  PG_FREE_IF_COPY(polya, 0);
  PG_FREE_IF_COPY(polyb, 1);

  PG_RETURN_BOOL(result);
}

/*-------------------------------------------------------
 * Is polygon A strictly below polygon B? i.e. is
 * the upper most point of A below the lower most point
 * of B?
 *-------------------------------------------------------*/
Datum poly_below(PG_FUNCTION_ARGS) {
  POLYGON *polya = PG_GETARG_POLYGON_P(0);
  POLYGON *polyb = PG_GETARG_POLYGON_P(1);
  bool result;

  result = ty_polygon_below(TYB(&polya->boundbox), TYB(&polyb->boundbox));

  /* Avoid leaking memory for toasted inputs: needed for rtree indexes. */
  PG_FREE_IF_COPY(polya, 0);
  PG_FREE_IF_COPY(polyb, 1);

  PG_RETURN_BOOL(result);
}

/*-------------------------------------------------------
 * Is polygon A overlapping or below polygon B? i.e. is
 * the upper most point of A at or below the upper most point
 * of B?
 *-------------------------------------------------------*/
Datum poly_overbelow(PG_FUNCTION_ARGS) {
  POLYGON *polya = PG_GETARG_POLYGON_P(0);
  POLYGON *polyb = PG_GETARG_POLYGON_P(1);
  bool result;

  result = ty_polygon_overbelow(TYB(&polya->boundbox), TYB(&polyb->boundbox));

  /* Avoid leaking memory for toasted inputs: needed for rtree indexes. */
  PG_FREE_IF_COPY(polya, 0);
  PG_FREE_IF_COPY(polyb, 1);

  PG_RETURN_BOOL(result);
}

/*-------------------------------------------------------
 * Is polygon A strictly above polygon B? i.e. is
 * the lower most point of A above the upper most point
 * of B?
 *-------------------------------------------------------*/
Datum poly_above(PG_FUNCTION_ARGS) {
  POLYGON *polya = PG_GETARG_POLYGON_P(0);
  POLYGON *polyb = PG_GETARG_POLYGON_P(1);
  bool result;

  result = ty_polygon_above(TYB(&polya->boundbox), TYB(&polyb->boundbox));

  /* Avoid leaking memory for toasted inputs: needed for rtree indexes. */
  PG_FREE_IF_COPY(polya, 0);
  PG_FREE_IF_COPY(polyb, 1);

  PG_RETURN_BOOL(result);
}

/*-------------------------------------------------------
 * Is polygon A overlapping or above polygon B? i.e. is
 * the lower most point of A at or above the lower most point
 * of B?
 *-------------------------------------------------------*/
Datum poly_overabove(PG_FUNCTION_ARGS) {
  POLYGON *polya = PG_GETARG_POLYGON_P(0);
  POLYGON *polyb = PG_GETARG_POLYGON_P(1);
  bool result;

  result = ty_polygon_overabove(TYB(&polya->boundbox), TYB(&polyb->boundbox));

  /* Avoid leaking memory for toasted inputs: needed for rtree indexes. */
  PG_FREE_IF_COPY(polya, 0);
  PG_FREE_IF_COPY(polyb, 1);

  PG_RETURN_BOOL(result);
}

/*-------------------------------------------------------
 * Is polygon A the same as polygon B? i.e. are all the
 * points the same?
 * Check all points for matches in both forward and reverse
 *	direction since polygons are non-directional and are
 *	closed shapes.
 *-------------------------------------------------------*/
Datum poly_same(PG_FUNCTION_ARGS) {
  POLYGON *polya = PG_GETARG_POLYGON_P(0);
  POLYGON *polyb = PG_GETARG_POLYGON_P(1);
  bool result;

  result = ty_polygon_same(polya->npts, TYPS(polya->p), polyb->npts, TYPS(polyb->p));

  /* Avoid leaking memory for toasted inputs: needed for rtree indexes. */
  PG_FREE_IF_COPY(polya, 0);
  PG_FREE_IF_COPY(polyb, 1);

  PG_RETURN_BOOL(result);
}

static bool poly_overlap_internal(POLYGON *polya, POLYGON *polyb) {
  int r;

  Assert(polya->npts > 0 && polyb->npts > 0);
  if (ty_polygon_overlap(polya->npts, TYPS(polya->p), TYB(&polya->boundbox), polyb->npts,
                         TYPS(polyb->p), TYB(&polyb->boundbox), &r))
    float_raise_ty();
  return r;
}

Datum poly_overlap(PG_FUNCTION_ARGS) {
  POLYGON *polya = PG_GETARG_POLYGON_P(0);
  POLYGON *polyb = PG_GETARG_POLYGON_P(1);
  bool result;

  result = poly_overlap_internal(polya, polyb);

  /* Avoid leaking memory for toasted inputs: needed for rtree indexes. */
  PG_FREE_IF_COPY(polya, 0);
  PG_FREE_IF_COPY(polyb, 1);

  PG_RETURN_BOOL(result);
}

/*
 * Tests special kind of segment for in/out of polygon.
 * Special kind means:
 *	- point a should be on segment s
 *	- segment (a,b) should not be contained by s
 * Returns true if:
 *	- segment (a,b) is collinear to s and (a,b) is in polygon
 *	- segment (a,b) s not collinear to s. Note: that doesn't
 *	  mean that segment is in polygon!
 */



static bool poly_contain_poly(POLYGON *contains_poly, POLYGON *contained_poly) {
  int r;

  Assert(contains_poly->npts > 0 && contained_poly->npts > 0);
  /*
   * A call that the poll function stopped is run again, after the host has
   * raised the error if there is one: a cancel, or a stack that is too deep.
   */
  for (;;) {
    if (ty_polygon_contain(contains_poly->npts, TYPS(contains_poly->p),
                           TYB(&contains_poly->boundbox), contained_poly->npts,
                           TYPS(contained_poly->p), TYB(&contained_poly->boundbox), geo_poll, &r) == 0)
      return r;
    if (ty_errnum() != TY_ERR_ABORTED)
      float_raise_ty();
    check_stack_depth();
    CHECK_FOR_INTERRUPTS();
  }
}

Datum poly_contain(PG_FUNCTION_ARGS) {
  POLYGON *polya = PG_GETARG_POLYGON_P(0);
  POLYGON *polyb = PG_GETARG_POLYGON_P(1);
  bool result;

  result = poly_contain_poly(polya, polyb);

  /* Avoid leaking memory for toasted inputs: needed for rtree indexes. */
  PG_FREE_IF_COPY(polya, 0);
  PG_FREE_IF_COPY(polyb, 1);

  PG_RETURN_BOOL(result);
}

/*-----------------------------------------------------------------
 * Determine if polygon A is contained by polygon B
 *-----------------------------------------------------------------*/
Datum poly_contained(PG_FUNCTION_ARGS) {
  POLYGON *polya = PG_GETARG_POLYGON_P(0);
  POLYGON *polyb = PG_GETARG_POLYGON_P(1);
  bool result;

  /* Just switch the arguments and pass it off to poly_contain */
  result = poly_contain_poly(polyb, polya);

  /* Avoid leaking memory for toasted inputs: needed for rtree indexes. */
  PG_FREE_IF_COPY(polya, 0);
  PG_FREE_IF_COPY(polyb, 1);

  PG_RETURN_BOOL(result);
}

Datum poly_contain_pt(PG_FUNCTION_ARGS) {
  POLYGON *poly = PG_GETARG_POLYGON_P(0);
  Point *p = PG_GETARG_POINT_P(1);

  PG_RETURN_BOOL(point_inside(p, poly->npts, poly->p) != 0);
}

Datum pt_contained_poly(PG_FUNCTION_ARGS) {
  Point *p = PG_GETARG_POINT_P(0);
  POLYGON *poly = PG_GETARG_POLYGON_P(1);

  PG_RETURN_BOOL(point_inside(p, poly->npts, poly->p) != 0);
}

Datum poly_distance(PG_FUNCTION_ARGS) {
  POLYGON *polya = PG_GETARG_POLYGON_P(0);
  POLYGON *polyb = PG_GETARG_POLYGON_P(1);
  float8 r;

  if (ty_polygon_distance(polya->npts, TYPS(polya->p), TYB(&polya->boundbox), polyb->npts, TYPS(polyb->p),
                          TYB(&polyb->boundbox), &r))
    float_raise_ty();
  PG_RETURN_FLOAT8(r);
}

/***********************************************************************
 **
 **		Routines for 2D points.
 **
 ***********************************************************************/

Datum construct_point(PG_FUNCTION_ARGS) {
  float8 x = PG_GETARG_FLOAT8(0);
  float8 y = PG_GETARG_FLOAT8(1);
  Point *result;

  result = (Point *)palloc(sizeof(Point));

  point_construct(result, x, y);

  PG_RETURN_POINT_P(result);
}

static inline void point_add_point(Point *result, Point *pt1, Point *pt2) {
  /* ty adds, with the float overflow check */
  if (ty_point_add((ty_point_t *)pt1, (ty_point_t *)pt2, (ty_point_t *)result))
    float_raise_ty();
}

Datum point_add(PG_FUNCTION_ARGS) {
  Point *p1 = PG_GETARG_POINT_P(0);
  Point *p2 = PG_GETARG_POINT_P(1);
  Point *result;

  result = (Point *)palloc(sizeof(Point));

  point_add_point(result, p1, p2);

  PG_RETURN_POINT_P(result);
}

static inline void point_sub_point(Point *result, Point *pt1, Point *pt2) {
  if (ty_point_sub((ty_point_t *)pt1, (ty_point_t *)pt2, (ty_point_t *)result))
    float_raise_ty();
}

Datum point_sub(PG_FUNCTION_ARGS) {
  Point *p1 = PG_GETARG_POINT_P(0);
  Point *p2 = PG_GETARG_POINT_P(1);
  Point *result;

  result = (Point *)palloc(sizeof(Point));

  point_sub_point(result, p1, p2);

  PG_RETURN_POINT_P(result);
}

static inline void point_mul_point(Point *result, Point *pt1, Point *pt2) {
  if (ty_point_mul((ty_point_t *)pt1, (ty_point_t *)pt2, (ty_point_t *)result))
    float_raise_ty();
}

Datum point_mul(PG_FUNCTION_ARGS) {
  Point *p1 = PG_GETARG_POINT_P(0);
  Point *p2 = PG_GETARG_POINT_P(1);
  Point *result;

  result = (Point *)palloc(sizeof(Point));

  point_mul_point(result, p1, p2);

  PG_RETURN_POINT_P(result);
}

static inline void point_div_point(Point *result, Point *pt1, Point *pt2) {
  if (ty_point_div((ty_point_t *)pt1, (ty_point_t *)pt2, (ty_point_t *)result))
    float_raise_ty();
}

Datum point_div(PG_FUNCTION_ARGS) {
  Point *p1 = PG_GETARG_POINT_P(0);
  Point *p2 = PG_GETARG_POINT_P(1);
  Point *result;

  result = (Point *)palloc(sizeof(Point));

  point_div_point(result, p1, p2);

  PG_RETURN_POINT_P(result);
}

/***********************************************************************
 **
 **		Routines for 2D boxes.
 **
 ***********************************************************************/

Datum points_box(PG_FUNCTION_ARGS) {
  Point *p1 = PG_GETARG_POINT_P(0);
  Point *p2 = PG_GETARG_POINT_P(1);
  BOX *result;

  result = (BOX *)palloc(sizeof(BOX));

  box_construct(result, p1, p2);

  PG_RETURN_BOX_P(result);
}

Datum box_add(PG_FUNCTION_ARGS) {
  BOX *box = PG_GETARG_BOX_P(0);
  Point *p = PG_GETARG_POINT_P(1);
  BOX *result;

  result = (BOX *)palloc(sizeof(BOX));

  point_add_point(&result->high, &box->high, p);
  point_add_point(&result->low, &box->low, p);

  PG_RETURN_BOX_P(result);
}

Datum box_sub(PG_FUNCTION_ARGS) {
  BOX *box = PG_GETARG_BOX_P(0);
  Point *p = PG_GETARG_POINT_P(1);
  BOX *result;

  result = (BOX *)palloc(sizeof(BOX));

  point_sub_point(&result->high, &box->high, p);
  point_sub_point(&result->low, &box->low, p);

  PG_RETURN_BOX_P(result);
}

Datum box_mul(PG_FUNCTION_ARGS) {
  BOX *box = PG_GETARG_BOX_P(0);
  Point *p = PG_GETARG_POINT_P(1);
  BOX *result;
  Point high, low;

  result = (BOX *)palloc(sizeof(BOX));

  point_mul_point(&high, &box->high, p);
  point_mul_point(&low, &box->low, p);

  box_construct(result, &high, &low);

  PG_RETURN_BOX_P(result);
}

Datum box_div(PG_FUNCTION_ARGS) {
  BOX *box = PG_GETARG_BOX_P(0);
  Point *p = PG_GETARG_POINT_P(1);
  BOX *result;
  Point high, low;

  result = (BOX *)palloc(sizeof(BOX));

  point_div_point(&high, &box->high, p);
  point_div_point(&low, &box->low, p);

  box_construct(result, &high, &low);

  PG_RETURN_BOX_P(result);
}

/*
 * Convert point to empty box
 */
Datum point_box(PG_FUNCTION_ARGS) {
  Point *pt = PG_GETARG_POINT_P(0);
  BOX *box;

  box = (BOX *)palloc(sizeof(BOX));

  box->high.x = pt->x;
  box->low.x = pt->x;
  box->high.y = pt->y;
  box->low.y = pt->y;

  PG_RETURN_BOX_P(box);
}

/*
 * Smallest bounding box that includes both of the given boxes
 */
Datum boxes_bound_box(PG_FUNCTION_ARGS) {
  BOX *box1 = PG_GETARG_BOX_P(0), *box2 = PG_GETARG_BOX_P(1), *container;

  container = (BOX *)palloc(sizeof(BOX));

  container->high.x = float8_max(box1->high.x, box2->high.x);
  container->low.x = float8_min(box1->low.x, box2->low.x);
  container->high.y = float8_max(box1->high.y, box2->high.y);
  container->low.y = float8_min(box1->low.y, box2->low.y);

  PG_RETURN_BOX_P(container);
}

/***********************************************************************
 **
 **		Routines for 2D paths.
 **
 ***********************************************************************/

/* path_add()
 * Concatenate two paths (only if they are both open).
 */
Datum path_add(PG_FUNCTION_ARGS) {
  PATH *p1 = PG_GETARG_PATH_P(0);
  PATH *p2 = PG_GETARG_PATH_P(1);
  PATH *result;
  int size, base_size;
  int i;

  if (p1->closed || p2->closed)
    PG_RETURN_NULL();

  base_size = sizeof(p1->p[0]) * (p1->npts + p2->npts);
  size = offsetof(PATH, p) + base_size;

  /* Check for integer overflow */
  if (base_size / sizeof(p1->p[0]) != (p1->npts + p2->npts) ||
      size <= base_size)
    ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                    errmsg("too many points requested")));

  result = (PATH *)palloc(size);

  SET_VARSIZE(result, size);
  result->npts = (p1->npts + p2->npts);
  result->closed = p1->closed;
  /* prevent instability in unused pad bytes */
  result->dummy = 0;

  for (i = 0; i < p1->npts; i++) {
    result->p[i].x = p1->p[i].x;
    result->p[i].y = p1->p[i].y;
  }
  for (i = 0; i < p2->npts; i++) {
    result->p[i + p1->npts].x = p2->p[i].x;
    result->p[i + p1->npts].y = p2->p[i].y;
  }

  PG_RETURN_PATH_P(result);
}

/* path_add_pt()
 * Translation operators.
 */
Datum path_add_pt(PG_FUNCTION_ARGS) {
  PATH *path = PG_GETARG_PATH_P_COPY(0);
  Point *point = PG_GETARG_POINT_P(1);

  if (ty_points_add(path->npts, TYPS(path->p), TYP(point)))
    float_raise_ty();

  PG_RETURN_PATH_P(path);
}

Datum path_sub_pt(PG_FUNCTION_ARGS) {
  PATH *path = PG_GETARG_PATH_P_COPY(0);
  Point *point = PG_GETARG_POINT_P(1);

  if (ty_points_sub(path->npts, TYPS(path->p), TYP(point)))
    float_raise_ty();

  PG_RETURN_PATH_P(path);
}

/* path_mul_pt()
 * Rotation and scaling operators.
 */
Datum path_mul_pt(PG_FUNCTION_ARGS) {
  PATH *path = PG_GETARG_PATH_P_COPY(0);
  Point *point = PG_GETARG_POINT_P(1);

  if (ty_points_mul(path->npts, TYPS(path->p), TYP(point)))
    float_raise_ty();

  PG_RETURN_PATH_P(path);
}

Datum path_div_pt(PG_FUNCTION_ARGS) {
  PATH *path = PG_GETARG_PATH_P_COPY(0);
  Point *point = PG_GETARG_POINT_P(1);

  if (ty_points_div(path->npts, TYPS(path->p), TYP(point)))
    float_raise_ty();

  PG_RETURN_PATH_P(path);
}

Datum path_poly(PG_FUNCTION_ARGS) {
  PATH *path = PG_GETARG_PATH_P(0);
  POLYGON *poly;
  int size;
  int i;

  /* This is not very consistent --- other similar cases return NULL ... */
  if (!path->closed)
    ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                    errmsg("open path cannot be converted to polygon")));

  /*
   * Never overflows: the old size fit in MaxAllocSize, and the new size is
   * just a small constant larger.
   */
  size = offsetof(POLYGON, p) + sizeof(poly->p[0]) * path->npts;
  poly = (POLYGON *)palloc(size);

  SET_VARSIZE(poly, size);
  poly->npts = path->npts;

  for (i = 0; i < path->npts; i++) {
    poly->p[i].x = path->p[i].x;
    poly->p[i].y = path->p[i].y;
  }

  make_bound_box(poly);

  PG_RETURN_POLYGON_P(poly);
}

/***********************************************************************
 **
 **		Routines for 2D polygons.
 **
 ***********************************************************************/

Datum poly_npoints(PG_FUNCTION_ARGS) {
  POLYGON *poly = PG_GETARG_POLYGON_P(0);

  PG_RETURN_INT32(poly->npts);
}

Datum poly_center(PG_FUNCTION_ARGS) {
  POLYGON *poly = PG_GETARG_POLYGON_P(0);
  Point *result;
  CIRCLE circle;

  result = (Point *)palloc(sizeof(Point));

  poly_to_circle(&circle, poly);

  *result = circle.center;

  PG_RETURN_POINT_P(result);
}

Datum poly_box(PG_FUNCTION_ARGS) {
  POLYGON *poly = PG_GETARG_POLYGON_P(0);
  BOX *box;

  box = (BOX *)palloc(sizeof(BOX));
  *box = poly->boundbox;

  PG_RETURN_BOX_P(box);
}

/* box_poly()
 * Convert a box to a polygon.
 */
Datum box_poly(PG_FUNCTION_ARGS) {
  BOX *box = PG_GETARG_BOX_P(0);
  POLYGON *poly;
  int size;

  /* map four corners of the box to a polygon */
  size = offsetof(POLYGON, p) + sizeof(poly->p[0]) * 4;
  poly = (POLYGON *)palloc(size);

  SET_VARSIZE(poly, size);
  poly->npts = 4;

  poly->p[0].x = box->low.x;
  poly->p[0].y = box->low.y;
  poly->p[1].x = box->low.x;
  poly->p[1].y = box->high.y;
  poly->p[2].x = box->high.x;
  poly->p[2].y = box->high.y;
  poly->p[3].x = box->high.x;
  poly->p[3].y = box->low.y;

  box_construct(&poly->boundbox, &box->high, &box->low);

  PG_RETURN_POLYGON_P(poly);
}

Datum poly_path(PG_FUNCTION_ARGS) {
  POLYGON *poly = PG_GETARG_POLYGON_P(0);
  PATH *path;
  int size;
  int i;

  /*
   * Never overflows: the old size fit in MaxAllocSize, and the new size is
   * smaller by a small constant.
   */
  size = offsetof(PATH, p) + sizeof(path->p[0]) * poly->npts;
  path = (PATH *)palloc(size);

  SET_VARSIZE(path, size);
  path->npts = poly->npts;
  path->closed = true;
  /* prevent instability in unused pad bytes */
  path->dummy = 0;

  for (i = 0; i < poly->npts; i++) {
    path->p[i].x = poly->p[i].x;
    path->p[i].y = poly->p[i].y;
  }

  PG_RETURN_PATH_P(path);
}

/***********************************************************************
 **
 **		Routines for circles.
 **
 ***********************************************************************/

/*----------------------------------------------------------
 * Formatting and conversion routines.
 *---------------------------------------------------------*/

/*		circle_in		-		convert a string to
 *internal form.
 *
 *		External format: (center and radius of circle)
 *				"<(f8,f8),f8>"
 *				also supports quick entry style "f8,f8,f8"
 */
Datum circle_in(PG_FUNCTION_ARGS) {
  char *str = PG_GETARG_CSTRING(0);
  Node *escontext = fcinfo->context;
  CIRCLE *circle = (CIRCLE *)palloc(sizeof(CIRCLE));
  char *s, *cp;
  int depth = 0;

  s = str;
  while (isspace((unsigned char)*s))
    s++;
  if (*s == LDELIM_C)
    depth++, s++;
  else if (*s == LDELIM) {
    /* If there are two left parens, consume the first one */
    cp = (s + 1);
    while (isspace((unsigned char)*cp))
      cp++;
    if (*cp == LDELIM)
      depth++, s = cp;
  }

  /* pair_decode will consume parens around the pair, if any */
  if (!pair_decode(s, &circle->center.x, &circle->center.y, &s, "circle", str,
                   escontext))
    PG_RETURN_NULL();

  if (*s == DELIM)
    s++;

  if (!single_decode(s, &circle->radius, &s, "circle", str, escontext))
    PG_RETURN_NULL();

  /* We have to accept NaN. */
  if (!ty_circle_valid(TYC(circle)))
    ereturn(
        escontext, (Datum)0,
        (errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
         errmsg("invalid input syntax for type %s: \"%s\"", "circle", str)));

  while (depth > 0) {
    if ((*s == RDELIM) || ((*s == RDELIM_C) && (depth == 1))) {
      depth--;
      s++;
      while (isspace((unsigned char)*s))
        s++;
    } else
      ereturn(
          escontext, (Datum)0,
          (errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
           errmsg("invalid input syntax for type %s: \"%s\"", "circle", str)));
  }

  if (*s != '\0')
    ereturn(
        escontext, (Datum)0,
        (errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
         errmsg("invalid input syntax for type %s: \"%s\"", "circle", str)));

  PG_RETURN_CIRCLE_P(circle);
}

/*		circle_out		-		convert a circle to
 * external form.
 */
Datum circle_out(PG_FUNCTION_ARGS) {
  CIRCLE *circle = PG_GETARG_CIRCLE_P(0);
  StringInfoData str;

  initStringInfo(&str);

  appendStringInfoChar(&str, LDELIM_C);
  appendStringInfoChar(&str, LDELIM);
  pair_encode(circle->center.x, circle->center.y, &str);
  appendStringInfoChar(&str, RDELIM);
  appendStringInfoChar(&str, DELIM);
  single_encode(circle->radius, &str);
  appendStringInfoChar(&str, RDELIM_C);

  PG_RETURN_CSTRING(str.data);
}

/*
 *		circle_recv			- converts external binary
 *format to circle
 */
Datum circle_recv(PG_FUNCTION_ARGS) {
  StringInfo buf = (StringInfo)PG_GETARG_POINTER(0);
  CIRCLE *circle;

  circle = (CIRCLE *)palloc(sizeof(CIRCLE));

  circle->center.x = pq_getmsgfloat8(buf);
  circle->center.y = pq_getmsgfloat8(buf);
  circle->radius = pq_getmsgfloat8(buf);

  /* We have to accept NaN. */
  if (!ty_circle_valid(TYC(circle)))
    ereport(ERROR, (errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
                    errmsg("invalid radius in external \"circle\" value")));

  PG_RETURN_CIRCLE_P(circle);
}

/*
 *		circle_send			- converts circle to binary
 *format
 */
Datum circle_send(PG_FUNCTION_ARGS) {
  CIRCLE *circle = PG_GETARG_CIRCLE_P(0);
  StringInfoData buf;

  pq_begintypsend(&buf);
  pq_sendfloat8(&buf, circle->center.x);
  pq_sendfloat8(&buf, circle->center.y);
  pq_sendfloat8(&buf, circle->radius);
  PG_RETURN_BYTEA_P(pq_endtypsend(&buf));
}

/*----------------------------------------------------------
 *	Relational operators for CIRCLEs.
 *		<, >, <=, >=, and == are based on circle area.
 *---------------------------------------------------------*/

/*		circles identical?
 *
 * We consider NaNs values to be equal to each other to let those circles
 * to be found.
 */
GEO_NO_CANARY Datum circle_same(PG_FUNCTION_ARGS) {
  CIRCLE *a0 = PG_GETARG_CIRCLE_P(0);
  CIRCLE *a1 = PG_GETARG_CIRCLE_P(1);

  PG_RETURN_BOOL(ty_circle_same(TYC(a0), TYC(a1)));
}

/*		circle_overlap	-		does circle1 overlap circle2?
 */
GEO_NO_CANARY Datum circle_overlap(PG_FUNCTION_ARGS) {
  CIRCLE *a0 = PG_GETARG_CIRCLE_P(0);
  CIRCLE *a1 = PG_GETARG_CIRCLE_P(1);
  int r;

  if (ty_circle_overlap(TYC(a0), TYC(a1), &r))
    float_raise_ty();
  PG_RETURN_BOOL(r);
}

/*		circle_overleft -		is the right edge of circle1 at
 *or left of the right edge of circle2?
 */
GEO_NO_CANARY Datum circle_overleft(PG_FUNCTION_ARGS) {
  CIRCLE *a0 = PG_GETARG_CIRCLE_P(0);
  CIRCLE *a1 = PG_GETARG_CIRCLE_P(1);
  int r;

  if (ty_circle_overleft(TYC(a0), TYC(a1), &r))
    float_raise_ty();
  PG_RETURN_BOOL(r);
}

/*		circle_left		-		is circle1 strictly left
 * of circle2?
 */
GEO_NO_CANARY Datum circle_left(PG_FUNCTION_ARGS) {
  CIRCLE *a0 = PG_GETARG_CIRCLE_P(0);
  CIRCLE *a1 = PG_GETARG_CIRCLE_P(1);
  int r;

  if (ty_circle_left(TYC(a0), TYC(a1), &r))
    float_raise_ty();
  PG_RETURN_BOOL(r);
}

/*		circle_right	-		is circle1 strictly right of
 * circle2?
 */
GEO_NO_CANARY Datum circle_right(PG_FUNCTION_ARGS) {
  CIRCLE *a0 = PG_GETARG_CIRCLE_P(0);
  CIRCLE *a1 = PG_GETARG_CIRCLE_P(1);
  int r;

  if (ty_circle_right(TYC(a0), TYC(a1), &r))
    float_raise_ty();
  PG_RETURN_BOOL(r);
}

/*		circle_overright	-	is the left edge of circle1 at
 *or right of the left edge of circle2?
 */
GEO_NO_CANARY Datum circle_overright(PG_FUNCTION_ARGS) {
  CIRCLE *a0 = PG_GETARG_CIRCLE_P(0);
  CIRCLE *a1 = PG_GETARG_CIRCLE_P(1);
  int r;

  if (ty_circle_overright(TYC(a0), TYC(a1), &r))
    float_raise_ty();
  PG_RETURN_BOOL(r);
}

/*		circle_contained		-		is circle1
 * contained by circle2?
 */
GEO_NO_CANARY Datum circle_contained(PG_FUNCTION_ARGS) {
  CIRCLE *a0 = PG_GETARG_CIRCLE_P(0);
  CIRCLE *a1 = PG_GETARG_CIRCLE_P(1);
  int r;

  if (ty_circle_contained(TYC(a0), TYC(a1), &r))
    float_raise_ty();
  PG_RETURN_BOOL(r);
}

/*		circle_contain	-		does circle1 contain circle2?
 */
GEO_NO_CANARY Datum circle_contain(PG_FUNCTION_ARGS) {
  CIRCLE *a0 = PG_GETARG_CIRCLE_P(0);
  CIRCLE *a1 = PG_GETARG_CIRCLE_P(1);
  int r;

  if (ty_circle_contain(TYC(a0), TYC(a1), &r))
    float_raise_ty();
  PG_RETURN_BOOL(r);
}

/*		circle_below		-		is circle1 strictly
 * below circle2?
 */
GEO_NO_CANARY Datum circle_below(PG_FUNCTION_ARGS) {
  CIRCLE *a0 = PG_GETARG_CIRCLE_P(0);
  CIRCLE *a1 = PG_GETARG_CIRCLE_P(1);
  int r;

  if (ty_circle_below(TYC(a0), TYC(a1), &r))
    float_raise_ty();
  PG_RETURN_BOOL(r);
}

/*		circle_above	-		is circle1 strictly above
 * circle2?
 */
GEO_NO_CANARY Datum circle_above(PG_FUNCTION_ARGS) {
  CIRCLE *a0 = PG_GETARG_CIRCLE_P(0);
  CIRCLE *a1 = PG_GETARG_CIRCLE_P(1);
  int r;

  if (ty_circle_above(TYC(a0), TYC(a1), &r))
    float_raise_ty();
  PG_RETURN_BOOL(r);
}

/*		circle_overbelow -		is the upper edge of circle1 at
 *or below the upper edge of circle2?
 */
GEO_NO_CANARY Datum circle_overbelow(PG_FUNCTION_ARGS) {
  CIRCLE *a0 = PG_GETARG_CIRCLE_P(0);
  CIRCLE *a1 = PG_GETARG_CIRCLE_P(1);
  int r;

  if (ty_circle_overbelow(TYC(a0), TYC(a1), &r))
    float_raise_ty();
  PG_RETURN_BOOL(r);
}

/*		circle_overabove	-	is the lower edge of circle1 at
 *or above the lower edge of circle2?
 */
GEO_NO_CANARY Datum circle_overabove(PG_FUNCTION_ARGS) {
  CIRCLE *a0 = PG_GETARG_CIRCLE_P(0);
  CIRCLE *a1 = PG_GETARG_CIRCLE_P(1);
  int r;

  if (ty_circle_overabove(TYC(a0), TYC(a1), &r))
    float_raise_ty();
  PG_RETURN_BOOL(r);
}

/*		circle_relop	-		is area(circle1) relop
 *area(circle2), within our accuracy constraint?
 */
GEO_NO_CANARY Datum circle_eq(PG_FUNCTION_ARGS) {
  CIRCLE *a0 = PG_GETARG_CIRCLE_P(0);
  CIRCLE *a1 = PG_GETARG_CIRCLE_P(1);
  int r;

  if (ty_circle_eq(TYC(a0), TYC(a1), &r))
    float_raise_ty();
  PG_RETURN_BOOL(r);
}

GEO_NO_CANARY Datum circle_ne(PG_FUNCTION_ARGS) {
  CIRCLE *a0 = PG_GETARG_CIRCLE_P(0);
  CIRCLE *a1 = PG_GETARG_CIRCLE_P(1);
  int r;

  if (ty_circle_ne(TYC(a0), TYC(a1), &r))
    float_raise_ty();
  PG_RETURN_BOOL(r);
}

GEO_NO_CANARY Datum circle_lt(PG_FUNCTION_ARGS) {
  CIRCLE *a0 = PG_GETARG_CIRCLE_P(0);
  CIRCLE *a1 = PG_GETARG_CIRCLE_P(1);
  int r;

  if (ty_circle_lt(TYC(a0), TYC(a1), &r))
    float_raise_ty();
  PG_RETURN_BOOL(r);
}

GEO_NO_CANARY Datum circle_gt(PG_FUNCTION_ARGS) {
  CIRCLE *a0 = PG_GETARG_CIRCLE_P(0);
  CIRCLE *a1 = PG_GETARG_CIRCLE_P(1);
  int r;

  if (ty_circle_gt(TYC(a0), TYC(a1), &r))
    float_raise_ty();
  PG_RETURN_BOOL(r);
}

GEO_NO_CANARY Datum circle_le(PG_FUNCTION_ARGS) {
  CIRCLE *a0 = PG_GETARG_CIRCLE_P(0);
  CIRCLE *a1 = PG_GETARG_CIRCLE_P(1);
  int r;

  if (ty_circle_le(TYC(a0), TYC(a1), &r))
    float_raise_ty();
  PG_RETURN_BOOL(r);
}

GEO_NO_CANARY Datum circle_ge(PG_FUNCTION_ARGS) {
  CIRCLE *a0 = PG_GETARG_CIRCLE_P(0);
  CIRCLE *a1 = PG_GETARG_CIRCLE_P(1);
  int r;

  if (ty_circle_ge(TYC(a0), TYC(a1), &r))
    float_raise_ty();
  PG_RETURN_BOOL(r);
}

/*----------------------------------------------------------
 *	"Arithmetic" operators on circles.
 *---------------------------------------------------------*/

/* circle_add_pt()
 * Translation operator.
 */
GEO_NO_CANARY Datum circle_add_pt(PG_FUNCTION_ARGS) {
  CIRCLE *a0 = PG_GETARG_CIRCLE_P(0);
  Point *a1 = PG_GETARG_POINT_P(1);
  CIRCLE *result = (CIRCLE *)palloc(sizeof(CIRCLE));

  if (ty_circle_add(TYC(a0), TYP(a1), TYC(result)))
    float_raise_ty();

  PG_RETURN_CIRCLE_P(result);
}

GEO_NO_CANARY Datum circle_sub_pt(PG_FUNCTION_ARGS) {
  CIRCLE *a0 = PG_GETARG_CIRCLE_P(0);
  Point *a1 = PG_GETARG_POINT_P(1);
  CIRCLE *result = (CIRCLE *)palloc(sizeof(CIRCLE));

  if (ty_circle_sub(TYC(a0), TYP(a1), TYC(result)))
    float_raise_ty();

  PG_RETURN_CIRCLE_P(result);
}

/* circle_mul_pt()
 * Rotation and scaling operators.
 */
GEO_NO_CANARY Datum circle_mul_pt(PG_FUNCTION_ARGS) {
  CIRCLE *a0 = PG_GETARG_CIRCLE_P(0);
  Point *a1 = PG_GETARG_POINT_P(1);
  CIRCLE *result = (CIRCLE *)palloc(sizeof(CIRCLE));

  if (ty_circle_mul(TYC(a0), TYP(a1), TYC(result)))
    float_raise_ty();

  PG_RETURN_CIRCLE_P(result);
}

GEO_NO_CANARY Datum circle_div_pt(PG_FUNCTION_ARGS) {
  CIRCLE *a0 = PG_GETARG_CIRCLE_P(0);
  Point *a1 = PG_GETARG_POINT_P(1);
  CIRCLE *result = (CIRCLE *)palloc(sizeof(CIRCLE));

  if (ty_circle_div(TYC(a0), TYP(a1), TYC(result)))
    float_raise_ty();

  PG_RETURN_CIRCLE_P(result);
}

/*		circle_area		-		returns the area of the
 * circle.
 */
GEO_NO_CANARY Datum circle_area(PG_FUNCTION_ARGS) {
  CIRCLE *a0 = PG_GETARG_CIRCLE_P(0);
  float8 r;

  if (ty_circle_area(TYC(a0), &r))
    float_raise_ty();
  PG_RETURN_FLOAT8(r);
}

/*		circle_diameter -		returns the diameter of the
 * circle.
 */
GEO_NO_CANARY Datum circle_diameter(PG_FUNCTION_ARGS) {
  CIRCLE *a0 = PG_GETARG_CIRCLE_P(0);
  float8 r;

  if (ty_circle_diameter(TYC(a0), &r))
    float_raise_ty();
  PG_RETURN_FLOAT8(r);
}

/*		circle_radius	-		returns the radius of the
 * circle.
 */
Datum circle_radius(PG_FUNCTION_ARGS) {
  CIRCLE *circle = PG_GETARG_CIRCLE_P(0);

  PG_RETURN_FLOAT8(circle->radius);
}

/*		circle_distance -		returns the distance between
 *								  two circles.
 */
GEO_NO_CANARY Datum circle_distance(PG_FUNCTION_ARGS) {
  CIRCLE *a0 = PG_GETARG_CIRCLE_P(0);
  CIRCLE *a1 = PG_GETARG_CIRCLE_P(1);
  float8 r;

  if (ty_circle_distance(TYC(a0), TYC(a1), &r))
    float_raise_ty();
  PG_RETURN_FLOAT8(r);
}

GEO_NO_CANARY Datum circle_contain_pt(PG_FUNCTION_ARGS) {
  CIRCLE *a0 = PG_GETARG_CIRCLE_P(0);
  Point *a1 = PG_GETARG_POINT_P(1);
  int r;

  if (ty_circle_contain_point(TYC(a0), TYP(a1), &r))
    float_raise_ty();
  PG_RETURN_BOOL(r);
}

GEO_NO_CANARY Datum pt_contained_circle(PG_FUNCTION_ARGS) {
  Point *a0 = PG_GETARG_POINT_P(0);
  CIRCLE *a1 = PG_GETARG_CIRCLE_P(1);
  int r;

  if (ty_circle_contain_point(TYC(a1), TYP(a0), &r))
    float_raise_ty();
  PG_RETURN_BOOL(r);
}

/*		dist_pc -		returns the distance between
 *						  a point and a circle.
 */
GEO_NO_CANARY Datum dist_pc(PG_FUNCTION_ARGS) {
  Point *a0 = PG_GETARG_POINT_P(0);
  CIRCLE *a1 = PG_GETARG_CIRCLE_P(1);
  float8 r;

  if (ty_point_circle_distance(TYP(a0), TYC(a1), &r))
    float_raise_ty();
  PG_RETURN_FLOAT8(r);
}

/*
 * Distance from a circle to a point
 */
GEO_NO_CANARY Datum dist_cpoint(PG_FUNCTION_ARGS) {
  CIRCLE *a0 = PG_GETARG_CIRCLE_P(0);
  Point *a1 = PG_GETARG_POINT_P(1);
  float8 r;

  if (ty_point_circle_distance(TYP(a1), TYC(a0), &r))
    float_raise_ty();
  PG_RETURN_FLOAT8(r);
}

/*		circle_center	-		returns the center point of the
 * circle.
 */
Datum circle_center(PG_FUNCTION_ARGS) {
  CIRCLE *circle = PG_GETARG_CIRCLE_P(0);
  Point *result;

  result = (Point *)palloc(sizeof(Point));
  result->x = circle->center.x;
  result->y = circle->center.y;

  PG_RETURN_POINT_P(result);
}

static float8 circle_ar(CIRCLE *circle) {
  float8 r;

  if (ty_circle_area(TYC(circle), &r))
    float_raise_ty();
  return r;
}

/*----------------------------------------------------------
 *	Conversion operators.
 *---------------------------------------------------------*/

Datum cr_circle(PG_FUNCTION_ARGS) {
  Point *center = PG_GETARG_POINT_P(0);
  float8 radius = PG_GETARG_FLOAT8(1);
  CIRCLE *result;

  result = (CIRCLE *)palloc(sizeof(CIRCLE));

  result->center.x = center->x;
  result->center.y = center->y;
  result->radius = radius;

  PG_RETURN_CIRCLE_P(result);
}

GEO_NO_CANARY Datum circle_box(PG_FUNCTION_ARGS) {
  CIRCLE *circle = PG_GETARG_CIRCLE_P(0);
  BOX *box = (BOX *)palloc(sizeof(BOX));

  if (ty_circle_to_box(TYC(circle), TYB(box)))
    float_raise_ty();

  PG_RETURN_BOX_P(box);
}

/* box_circle()
 * Convert a box to a circle.
 */
GEO_NO_CANARY Datum box_circle(PG_FUNCTION_ARGS) {
  BOX *box = PG_GETARG_BOX_P(0);
  CIRCLE *circle = (CIRCLE *)palloc(sizeof(CIRCLE));

  if (ty_box_to_circle(TYB(box), TYC(circle)))
    float_raise_ty();

  PG_RETURN_CIRCLE_P(circle);
}

Datum circle_poly(PG_FUNCTION_ARGS) {
  int32 npts = PG_GETARG_INT32(0);
  CIRCLE *circle = PG_GETARG_CIRCLE_P(1);
  POLYGON *poly;
  int base_size, size;
  int i;
  float8 angle;
  float8 anglestep;

  if (FPzero(circle->radius))
    ereport(ERROR,
            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
             errmsg("cannot convert circle with radius zero to polygon")));

  if (npts < 2)
    ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                    errmsg("must request at least 2 points")));

  base_size = sizeof(poly->p[0]) * npts;
  size = offsetof(POLYGON, p) + base_size;

  /* Check for integer overflow */
  if (base_size / npts != sizeof(poly->p[0]) || size <= base_size)
    ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                    errmsg("too many points requested")));

  poly = (POLYGON *)palloc0(size); /* zero any holes */
  SET_VARSIZE(poly, size);
  poly->npts = npts;

  anglestep = float8_div(2.0 * M_PI, npts);

  for (i = 0; i < npts; i++) {
    angle = float8_mul(anglestep, i);

    poly->p[i].x =
        float8_mi(circle->center.x, float8_mul(circle->radius, cos(angle)));
    poly->p[i].y =
        float8_pl(circle->center.y, float8_mul(circle->radius, sin(angle)));
  }

  make_bound_box(poly);

  PG_RETURN_POLYGON_P(poly);
}

static void poly_to_circle(CIRCLE *result, POLYGON *poly) {
  Assert(poly->npts > 0);
  if (ty_polygon_to_circle(poly->npts, TYPS(poly->p), TYC(result)))
    float_raise_ty();
}

Datum poly_circle(PG_FUNCTION_ARGS) {
  POLYGON *poly = PG_GETARG_POLYGON_P(0);
  CIRCLE *result;

  result = (CIRCLE *)palloc(sizeof(CIRCLE));

  poly_to_circle(result, poly);

  PG_RETURN_CIRCLE_P(result);
}

/***********************************************************************
 **
 **		Private routines for multiple types.
 **
 ***********************************************************************/

/*
 *	Test to see if the point is inside the polygon, returns 1/0, or 2 if
 *	the point is on the polygon.
 *	Code adapted but not copied from integer-based routines in WN: A
 *	Server for the HTTP
 *	version 1.15.1, file wn/image.c
 *	http://hopf.math.northwestern.edu/index.html
 *	Description of algorithm:  http://www.linuxjournal.com/article/2197
 *							   http://www.linuxjournal.com/article/2029
 */


static int point_inside(Point *p, int npts, Point *plist) {
  int r;

  Assert(npts > 0);
  r = ty_point_in_polygon(TYP(p), npts, TYPS(plist));
  if (r < 0)
    float_raise_ty();
  return r;
}

/* lseg_crossing()
 * Returns +/-2 if line segment crosses the positive X-axis in a +/- direction.
 * Returns +/-1 if one point is on the positive X-axis.
 * Returns 0 if both points are on the positive X-axis, or there is no crossing.
 * Returns POINT_ON_POLYGON if the segment contains (0,0).
 * Wow, that is one confusing API, but it is used above, and when summed,
 * can tell is if a point is in a polygon.
 */



/*-------------------------------------------------------------------------
 * Determine the hypotenuse.
 *
 * If required, x and y are swapped to make x the larger number. The
 * traditional formula of x^2+y^2 is rearranged to factor x outside the
 * sqrt. This allows computation of the hypotenuse for significantly
 * larger values, and with a higher precision than when using the naive
 * formula.  In particular, this cannot overflow unless the final result
 * would be out-of-range.
 *
 * sqrt( x^2 + y^2 ) = sqrt( x^2( 1 + y^2/x^2) )
 *					 = x * sqrt( 1 + y^2/x^2 )
 *					 = x * sqrt( 1 + y/x * y/x )
 *
 * It is expected that this routine will eventually be replaced with the
 * C99 hypot() function.
 *
 * This implementation conforms to IEEE Std 1003.1 and GLIBC, in that the
 * case of hypot(inf,nan) results in INF, and not NAN.
 *-----------------------------------------------------------------------
 */
float8 pg_hypot(float8 x, float8 y) {
  float8 r;

  if (ty_hypot(x, y, &r))
    float_raise_ty();
  return r;
}

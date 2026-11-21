/*-------------------------------------------------------------------------
 *
 * tid.c
 *	  Functions for the built-in type tuple id
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 * Portions Copyright (c) 2026, CK Tan: the changes for digres,
 * under the MIT License (https://github.com/cktan/digres/blob/main/LICENSE)
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/tid.c
 *
 * NOTES
 *	  input routine largely stolen from boxin().
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <math.h>
#include <limits.h>

#include "catalog/pg_type.h"
#include "common/hashfn.h"
#include "ty_small.h"
#include "libpq/pqformat.h"
#include "utils/fmgrprotos.h"

#define LDELIM			'('
#define RDELIM			')'
#define DELIM			','
#define NTIDARGS		2

/* ----------------------------------------------------------------
 *		tidin
 * ----------------------------------------------------------------
 */
Datum tidin(PG_FUNCTION_ARGS) {
  char *str = PG_GETARG_CSTRING(0);
  Node *escontext = fcinfo->context;
  ItemPointer result;
  ty_tid_t t;

  /* ty reads the text */
  if (ty_tid_parse(str, &t) != 0)
    ereturn(escontext, (Datum)0,
            (errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
             errmsg("invalid input syntax for type %s: \"%s\"", "tid", str)));

  result = (ItemPointer)palloc(sizeof(ItemPointerData));
  ItemPointerSet(result, ty_tid_block(&t), t.offset);
  PG_RETURN_ITEMPOINTER(result);
}

/* ----------------------------------------------------------------
 *		tidout
 * ----------------------------------------------------------------
 */
Datum tidout(PG_FUNCTION_ARGS) {
  ItemPointer itemPtr = PG_GETARG_ITEMPOINTER(0);
  char buf[TY_TID_TEXT_MAX];

  /* the struct of a tid is ty's */
  ty_tid_format((const ty_tid_t *)itemPtr, buf);
  PG_RETURN_CSTRING(pstrdup(buf));
}

/*
 *		tidrecv			- converts external binary format to tid
 */
Datum tidrecv(PG_FUNCTION_ARGS) {
  StringInfo buf = (StringInfo)PG_GETARG_POINTER(0);
  ItemPointer result;
  BlockNumber blockNumber;
  OffsetNumber offsetNumber;

  blockNumber = pq_getmsgint(buf, sizeof(blockNumber));
  offsetNumber = pq_getmsgint(buf, sizeof(offsetNumber));

  result = (ItemPointer)palloc(sizeof(ItemPointerData));

  ItemPointerSet(result, blockNumber, offsetNumber);

  PG_RETURN_ITEMPOINTER(result);
}

/*
 *		tidsend			- converts tid to binary format
 */
Datum tidsend(PG_FUNCTION_ARGS) {
  ItemPointer itemPtr = PG_GETARG_ITEMPOINTER(0);
  StringInfoData buf;

  pq_begintypsend(&buf);
  pq_sendint32(&buf, ItemPointerGetBlockNumberNoCheck(itemPtr));
  pq_sendint16(&buf, ItemPointerGetOffsetNumberNoCheck(itemPtr));
  PG_RETURN_BYTEA_P(pq_endtypsend(&buf));
}

/*****************************************************************************
 *	 PUBLIC ROUTINES
 **
 *****************************************************************************/

Datum tideq(PG_FUNCTION_ARGS) {
  ItemPointer arg1 = PG_GETARG_ITEMPOINTER(0);
  ItemPointer arg2 = PG_GETARG_ITEMPOINTER(1);

  PG_RETURN_BOOL(ItemPointerCompare(arg1, arg2) == 0);
}

Datum tidne(PG_FUNCTION_ARGS) {
  ItemPointer arg1 = PG_GETARG_ITEMPOINTER(0);
  ItemPointer arg2 = PG_GETARG_ITEMPOINTER(1);

  PG_RETURN_BOOL(ItemPointerCompare(arg1, arg2) != 0);
}

Datum tidlt(PG_FUNCTION_ARGS) {
  ItemPointer arg1 = PG_GETARG_ITEMPOINTER(0);
  ItemPointer arg2 = PG_GETARG_ITEMPOINTER(1);

  PG_RETURN_BOOL(ItemPointerCompare(arg1, arg2) < 0);
}

Datum tidle(PG_FUNCTION_ARGS) {
  ItemPointer arg1 = PG_GETARG_ITEMPOINTER(0);
  ItemPointer arg2 = PG_GETARG_ITEMPOINTER(1);

  PG_RETURN_BOOL(ItemPointerCompare(arg1, arg2) <= 0);
}

Datum tidgt(PG_FUNCTION_ARGS) {
  ItemPointer arg1 = PG_GETARG_ITEMPOINTER(0);
  ItemPointer arg2 = PG_GETARG_ITEMPOINTER(1);

  PG_RETURN_BOOL(ItemPointerCompare(arg1, arg2) > 0);
}

Datum tidge(PG_FUNCTION_ARGS) {
  ItemPointer arg1 = PG_GETARG_ITEMPOINTER(0);
  ItemPointer arg2 = PG_GETARG_ITEMPOINTER(1);

  PG_RETURN_BOOL(ItemPointerCompare(arg1, arg2) >= 0);
}

Datum bttidcmp(PG_FUNCTION_ARGS) {
  ItemPointer arg1 = PG_GETARG_ITEMPOINTER(0);
  ItemPointer arg2 = PG_GETARG_ITEMPOINTER(1);

  PG_RETURN_INT32(ItemPointerCompare(arg1, arg2));
}

Datum tidlarger(PG_FUNCTION_ARGS) {
  ItemPointer arg1 = PG_GETARG_ITEMPOINTER(0);
  ItemPointer arg2 = PG_GETARG_ITEMPOINTER(1);

  PG_RETURN_ITEMPOINTER(ItemPointerCompare(arg1, arg2) >= 0 ? arg1 : arg2);
}

Datum tidsmaller(PG_FUNCTION_ARGS) {
  ItemPointer arg1 = PG_GETARG_ITEMPOINTER(0);
  ItemPointer arg2 = PG_GETARG_ITEMPOINTER(1);

  PG_RETURN_ITEMPOINTER(ItemPointerCompare(arg1, arg2) <= 0 ? arg1 : arg2);
}

Datum hashtid(PG_FUNCTION_ARGS) {
  ItemPointer key = PG_GETARG_ITEMPOINTER(0);

  return UInt32GetDatum(ty_tid_hash((const ty_tid_t *)key));
}

Datum hashtidextended(PG_FUNCTION_ARGS) {
  ItemPointer key = PG_GETARG_ITEMPOINTER(0);
  uint64 seed = PG_GETARG_INT64(1);

  return Int64GetDatum(ty_tid_hash_extended((const ty_tid_t *)key, seed));
}

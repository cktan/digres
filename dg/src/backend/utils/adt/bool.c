/*-------------------------------------------------------------------------
 *
 * bool.c
 *	  Functions for the built-in type "bool".
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 * Portions Copyright (c) 2026, CK Tan: the changes for digres,
 * under the MIT License (https://github.com/cktan/digres/blob/main/LICENSE)
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/bool.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <ctype.h>

#include "common/hashfn.h"
#include "libpq/pqformat.h"
#include "utils/builtins.h"
#include "ty_bool.h"

/*
 * Try to interpret value as boolean value.  Valid values are: true,
 * false, yes, no, on, off, 1, 0; as well as unique prefixes thereof.
 * If the string parses okay, return true, else false.
 * If okay and result is not NULL, return the value in *result.
 */
bool parse_bool(const char *value, bool *result) {
  return parse_bool_with_len(value, strlen(value), result);
}

bool parse_bool_with_len(const char *value, size_t len, bool *result) {
  bool parsed;

  /* ty does the parse; a failure is its TY_ERR_INVALID, not an error here. */
  if (ty_bool_parse(value, len, &parsed) != 0) {
    if (result)
      *result = false; /* suppress compiler warning */
    return false;
  }
  if (result)
    *result = parsed;
  return true;
}

/*****************************************************************************
 *	 USER I/O ROUTINES
 **
 *****************************************************************************/

/*
 *		boolin			- input function for type boolean
 */
Datum boolin(PG_FUNCTION_ARGS) {
  const char *in_str = PG_GETARG_CSTRING(0);
  bool result;

  /* ty skips the space at both ends, then reads the words */
  if (ty_bool_parse_text(in_str, strlen(in_str), &result) == 0)
    PG_RETURN_BOOL(result);

  ereturn(
      fcinfo->context, (Datum)0,
      (errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
       errmsg("invalid input syntax for type %s: \"%s\"", "boolean", in_str)));
}

/*
 *		boolout			- converts 1 or 0 to "t" or "f"
 */
Datum boolout(PG_FUNCTION_ARGS) {
  PG_RETURN_CSTRING(pstrdup(ty_bool_out(PG_GETARG_BOOL(0))));
}

/*
 *		boolrecv			- converts external binary
 *format to bool
 *
 * The external representation is one byte.  Any nonzero value is taken
 * as "true".
 */
Datum boolrecv(PG_FUNCTION_ARGS) {
  StringInfo buf = (StringInfo)PG_GETARG_POINTER(0);
  int ext;

  ext = pq_getmsgbyte(buf);
  PG_RETURN_BOOL(ext != 0);
}

/*
 *		boolsend			- converts bool to binary format
 */
Datum boolsend(PG_FUNCTION_ARGS) {
  bool arg1 = PG_GETARG_BOOL(0);
  StringInfoData buf;

  pq_begintypsend(&buf);
  pq_sendbyte(&buf, arg1 ? 1 : 0);
  PG_RETURN_BYTEA_P(pq_endtypsend(&buf));
}

/*
 *		booltext			- cast function for bool => text
 *
 * We need this because it's different from the behavior of boolout();
 * this function follows the SQL-spec result (except for producing lower case)
 */
Datum booltext(PG_FUNCTION_ARGS) {
  PG_RETURN_TEXT_P(cstring_to_text(ty_bool_text(PG_GETARG_BOOL(0))));
}

/*****************************************************************************
 *	 PUBLIC ROUTINES
 **
 *****************************************************************************/

Datum booleq(PG_FUNCTION_ARGS) {
  PG_RETURN_BOOL(ty_bool_eq(PG_GETARG_BOOL(0), PG_GETARG_BOOL(1)));
}

Datum boolne(PG_FUNCTION_ARGS) {
  PG_RETURN_BOOL(ty_bool_ne(PG_GETARG_BOOL(0), PG_GETARG_BOOL(1)));
}

Datum boollt(PG_FUNCTION_ARGS) {
  PG_RETURN_BOOL(ty_bool_lt(PG_GETARG_BOOL(0), PG_GETARG_BOOL(1)));
}

Datum boolgt(PG_FUNCTION_ARGS) {
  PG_RETURN_BOOL(ty_bool_gt(PG_GETARG_BOOL(0), PG_GETARG_BOOL(1)));
}

Datum boolle(PG_FUNCTION_ARGS) {
  PG_RETURN_BOOL(ty_bool_le(PG_GETARG_BOOL(0), PG_GETARG_BOOL(1)));
}

Datum boolge(PG_FUNCTION_ARGS) {
  PG_RETURN_BOOL(ty_bool_ge(PG_GETARG_BOOL(0), PG_GETARG_BOOL(1)));
}

Datum hashbool(PG_FUNCTION_ARGS) {
  PG_RETURN_UINT32((uint32)ty_bool_hash(PG_GETARG_BOOL(0), 0));
}

Datum hashboolextended(PG_FUNCTION_ARGS) {
  PG_RETURN_UINT64(
      ty_bool_hash(PG_GETARG_BOOL(0), (uint64)PG_GETARG_INT64(1)));
}

/*
 * boolean-and and boolean-or aggregates.
 */

/*
 * Function for standard EVERY aggregate conforming to SQL 2003.
 * The aggregate is also named bool_and for consistency.
 *
 * Note: this is only used in plain aggregate mode, not moving-aggregate mode.
 */
Datum booland_statefunc(PG_FUNCTION_ARGS) {
  PG_RETURN_BOOL(ty_bool_and(PG_GETARG_BOOL(0), PG_GETARG_BOOL(1)));
}

/*
 * Function for standard ANY/SOME aggregate conforming to SQL 2003.
 * The aggregate is named bool_or, because ANY/SOME have parsing conflicts.
 *
 * Note: this is only used in plain aggregate mode, not moving-aggregate mode.
 */
Datum boolor_statefunc(PG_FUNCTION_ARGS) {
  PG_RETURN_BOOL(ty_bool_or(PG_GETARG_BOOL(0), PG_GETARG_BOOL(1)));
}

typedef struct BoolAggState {
  int64 aggcount; /* number of non-null values aggregated */
  int64 aggtrue;  /* number of values aggregated that are true */
} BoolAggState;

static BoolAggState *makeBoolAggState(FunctionCallInfo fcinfo) {
  BoolAggState *state;
  MemoryContext agg_context;

  if (!AggCheckCallContext(fcinfo, &agg_context))
    elog(ERROR, "aggregate function called in non-aggregate context");

  state = (BoolAggState *)MemoryContextAlloc(agg_context, sizeof(BoolAggState));
  state->aggcount = 0;
  state->aggtrue = 0;

  return state;
}

Datum bool_accum(PG_FUNCTION_ARGS) {
  BoolAggState *state;

  state = PG_ARGISNULL(0) ? NULL : (BoolAggState *)PG_GETARG_POINTER(0);

  /* Create the state data on first call */
  if (state == NULL)
    state = makeBoolAggState(fcinfo);

  if (!PG_ARGISNULL(1)) {
    state->aggcount++;
    if (PG_GETARG_BOOL(1))
      state->aggtrue++;
  }

  PG_RETURN_POINTER(state);
}

Datum bool_accum_inv(PG_FUNCTION_ARGS) {
  BoolAggState *state;

  state = PG_ARGISNULL(0) ? NULL : (BoolAggState *)PG_GETARG_POINTER(0);

  /* bool_accum should have created the state data */
  if (state == NULL)
    elog(ERROR, "bool_accum_inv called with NULL state");

  if (!PG_ARGISNULL(1)) {
    state->aggcount--;
    if (PG_GETARG_BOOL(1))
      state->aggtrue--;
  }

  PG_RETURN_POINTER(state);
}

Datum bool_alltrue(PG_FUNCTION_ARGS) {
  BoolAggState *state;

  state = PG_ARGISNULL(0) ? NULL : (BoolAggState *)PG_GETARG_POINTER(0);

  /* if there were no non-null values, return NULL */
  if (state == NULL || state->aggcount == 0)
    PG_RETURN_NULL();

  /* true if all non-null values are true */
  PG_RETURN_BOOL(state->aggtrue == state->aggcount);
}

Datum bool_anytrue(PG_FUNCTION_ARGS) {
  BoolAggState *state;

  state = PG_ARGISNULL(0) ? NULL : (BoolAggState *)PG_GETARG_POINTER(0);

  /* if there were no non-null values, return NULL */
  if (state == NULL || state->aggcount == 0)
    PG_RETURN_NULL();

  /* true if any non-null value is true */
  PG_RETURN_BOOL(state->aggtrue > 0);
}

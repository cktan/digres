/*-------------------------------------------------------------------------
 *
 * quote.c
 *	  Functions for quoting identifiers and literals
 *
 * Portions Copyright (c) 2000-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 2026, CK Tan: the changes for digres,
 * under the MIT License (https://github.com/cktan/digres/blob/main/LICENSE)
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/quote.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "ty_text.h"
#include "utils/builtins.h"
#include "utils/ty_glue.h"
#include "varatt.h"

/*
 * quote_ident -
 *	  returns a properly quoted identifier
 */
Datum quote_ident(PG_FUNCTION_ARGS) {
  text *t = PG_GETARG_TEXT_PP(0);
  const char *qstr;
  char *str;

  str = text_to_cstring(t);
  qstr = quote_identifier(str);
  PG_RETURN_TEXT_P(cstring_to_text(qstr));
}

/*
 * quote_literal -
 *	  returns a properly quoted literal
 */
Datum quote_literal(PG_FUNCTION_ARGS) {
  text *t = PG_GETARG_TEXT_PP(0);
  text *result;

  if (ty_quote_literal(TYV(t), TYVO(&result)))
    ty_raise_alloc();

  PG_RETURN_TEXT_P(result);
}

/*
 * quote_literal_cstr -
 *	  returns a properly quoted literal
 */
char *quote_literal_cstr(const char *rawstr) {
  char *result;
  int len;
  int newlen;

  len = strlen(rawstr);
  /* We make a worst-case result area; wasting a little space is OK */
  result = (char *)palloc((len * 2) /* doubling for every character if each one
                                     * is a quote */
                          + 3 /* two outer quotes + possibly 'E' if needed */
                          + 1 /* null terminator */
  );

  newlen = ty_quote_literal_bytes(rawstr, len, result);
  result[newlen] = '\0';

  return result;
}

/*
 * quote_nullable -
 *	  Returns a properly quoted literal, with null values returned
 *	  as the text string 'NULL'.
 */
Datum quote_nullable(PG_FUNCTION_ARGS) {
  if (PG_ARGISNULL(0))
    PG_RETURN_TEXT_P(cstring_to_text("NULL"));
  else
    PG_RETURN_DATUM(DirectFunctionCall1(quote_literal, PG_GETARG_DATUM(0)));
}

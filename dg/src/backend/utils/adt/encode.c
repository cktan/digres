/*-------------------------------------------------------------------------
 *
 * encode.c
 *	  Various data encoding/decoding things.
 *
 * Copyright (c) 2001-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 2026, CK Tan: the changes for digres,
 * under the MIT License (https://github.com/cktan/digres/blob/main/LICENSE)
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/encode.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <ctype.h>

#include "mb/pg_wchar.h"
#include "utils/builtins.h"
#include "utils/memutils.h"
#include "varatt.h"
#include "ty_bytea.h"
#include "utils/ty_glue.h"

/*
 * Encoding conversion API.
 * encode_len() and decode_len() compute the amount of space needed, while
 * encode() and decode() perform the actual conversions.  It is okay for
 * the _len functions to return an overestimate, but not an underestimate.
 * (Having said that, large overestimates could cause unnecessary errors,
 * so it's better to get it right.)  The conversion routines write to the
 * buffer at *res and return the true length of their output.
 */
/*
 * Raise PostgreSQL's error for a decode that ty refused: escape text has a
 * syntax error; the others a bad parameter, with ty's message, which is
 * PostgreSQL's (and the hint for the end of base64).
 */
static pg_noreturn void binary_decode_raise(ty_bytea_enc_t enc) {
  if (enc == TY_BYTEA_ENC_ESCAPE)
    ereport(ERROR, (errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
                    errmsg("invalid input syntax for type %s", "bytea")));
  if (strcmp(ty_errmsg(), "invalid base64 end sequence") == 0)
    ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                    errmsg("invalid base64 end sequence"),
                    errhint("Input data is missing padding, is truncated, or "
                            "is otherwise corrupted.")));
  ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                  errmsg("%s", ty_errmsg())));
}

/*
 * SQL functions.
 */

Datum binary_encode(PG_FUNCTION_ARGS) {
  bytea *data = PG_GETARG_BYTEA_PP(0);
  Datum name = PG_GETARG_DATUM(1);
  text *result;
  char *namebuf;
  ty_bytea_enc_t enc;

  namebuf = TextDatumGetCString(name);

  if (ty_bytea_encoding(namebuf, &enc) != 0)
    ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                    errmsg("unrecognized encoding: \"%s\"", namebuf)));

  /* ty makes the text; a result past 1 GB is an error */
  if (ty_bytea_encode(enc, TYV(data), TYVO(&result)) != 0) {
    if (ty_errnum() == TY_ERR_RANGE)
      ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                      errmsg("result of encoding conversion is too large")));
    ty_raise_alloc();
  }

  PG_RETURN_TEXT_P(result);
}

Datum binary_decode(PG_FUNCTION_ARGS) {
  text *data = PG_GETARG_TEXT_PP(0);
  Datum name = PG_GETARG_DATUM(1);
  bytea *result;
  char *namebuf;
  ty_bytea_enc_t enc;

  namebuf = TextDatumGetCString(name);

  if (ty_bytea_encoding(namebuf, &enc) != 0)
    ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                    errmsg("unrecognized encoding: \"%s\"", namebuf)));

  /* ty makes the bytea, or says what is wrong with the text */
  if (ty_bytea_decode(enc, TYV(data), TYVO(&result)) != 0) {
    if (ty_errnum() == TY_ERR_NOMEM)
      ty_raise_alloc();
    if (ty_errnum() == TY_ERR_RANGE)
      ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                      errmsg("result of decoding conversion is too large")));
    binary_decode_raise(enc);
  }

  PG_RETURN_BYTEA_P(result);
}

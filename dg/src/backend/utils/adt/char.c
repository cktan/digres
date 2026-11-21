/*-------------------------------------------------------------------------
 *
 * char.c
 *	  Functions for the built-in type "char" (not to be confused with
 *	  bpchar, which is the SQL CHAR(n) type).
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 * Portions Copyright (c) 2026, CK Tan: the changes for digres,
 * under the MIT License (https://github.com/cktan/digres/blob/main/LICENSE)
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/char.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <limits.h>

#include "libpq/pqformat.h"
#include "utils/fmgrprotos.h"
#include "varatt.h"
#include "ty_small.h"

#define ISOCTAL(c)   (((c) >= '0') && ((c) <= '7'))
#define TOOCTAL(c)   ((c) + '0')
#define FROMOCTAL(c) ((unsigned char) (c) - '0')

/*****************************************************************************
 *	 USER I/O ROUTINES
 **
 *****************************************************************************/

/*
 *		charin			- converts "x" to 'x'
 *
 * This accepts the formats charout produces.  If we have multibyte input
 * that is not in the form '\ooo', then we take its first byte as the value
 * and silently discard the rest; this is a backwards-compatibility provision.
 */
Datum charin(PG_FUNCTION_ARGS) {
  char *ch = PG_GETARG_CSTRING(0);

  /* ty reads a byte or a backslash and three octal digits */
  PG_RETURN_CHAR(ty_char_parse(ch, strlen(ch)));
}

/*
 *		charout			- converts 'x' to "x"
 *
 * The possible output formats are:
 * 1. 0x00 is represented as an empty string.
 * 2. 0x01..0x7F are represented as a single ASCII byte.
 * 3. 0x80..0xFF are represented as \ooo (backslash and 3 octal digits).
 * Case 3 is meant to match the traditional "escape" format of bytea.
 */
Datum charout(PG_FUNCTION_ARGS) {
  char ch = PG_GETARG_CHAR(0);
  char *result = (char *)palloc(5);

  ty_char_format((int8_t)ch, result);
  PG_RETURN_CSTRING(result);
}

/*
 *		charrecv			- converts external binary
 *format to char
 *
 * The external representation is one byte, with no character set
 * conversion.  This is somewhat dubious, perhaps, but in many
 * cases people use char for a 1-byte binary type.
 */
Datum charrecv(PG_FUNCTION_ARGS) {
  StringInfo buf = (StringInfo)PG_GETARG_POINTER(0);

  PG_RETURN_CHAR(pq_getmsgbyte(buf));
}

/*
 *		charsend			- converts char to binary format
 */
Datum charsend(PG_FUNCTION_ARGS) {
  char arg1 = PG_GETARG_CHAR(0);
  StringInfoData buf;

  pq_begintypsend(&buf);
  pq_sendbyte(&buf, arg1);
  PG_RETURN_BYTEA_P(pq_endtypsend(&buf));
}

/*****************************************************************************
 *	 PUBLIC ROUTINES
 **
 *****************************************************************************/

/*
 * NOTE: comparisons are done as though char is unsigned (uint8).
 * Conversions to and from integer are done as though char is signed (int8).
 *
 * You wanted consistency?
 */

Datum chareq(PG_FUNCTION_ARGS) {
  char arg1 = PG_GETARG_CHAR(0);
  char arg2 = PG_GETARG_CHAR(1);

  PG_RETURN_BOOL(arg1 == arg2);
}

Datum charne(PG_FUNCTION_ARGS) {
  char arg1 = PG_GETARG_CHAR(0);
  char arg2 = PG_GETARG_CHAR(1);

  PG_RETURN_BOOL(arg1 != arg2);
}

Datum charlt(PG_FUNCTION_ARGS) {
  PG_RETURN_BOOL(ty_char_cmp(PG_GETARG_CHAR(0), PG_GETARG_CHAR(1)) < 0);
}

Datum charle(PG_FUNCTION_ARGS) {
  PG_RETURN_BOOL(ty_char_cmp(PG_GETARG_CHAR(0), PG_GETARG_CHAR(1)) <= 0);
}

Datum chargt(PG_FUNCTION_ARGS) {
  PG_RETURN_BOOL(ty_char_cmp(PG_GETARG_CHAR(0), PG_GETARG_CHAR(1)) > 0);
}

Datum charge(PG_FUNCTION_ARGS) {
  PG_RETURN_BOOL(ty_char_cmp(PG_GETARG_CHAR(0), PG_GETARG_CHAR(1)) >= 0);
}

Datum chartoi4(PG_FUNCTION_ARGS) {
  char arg1 = PG_GETARG_CHAR(0);

  PG_RETURN_INT32((int32)((int8)arg1));
}

Datum i4tochar(PG_FUNCTION_ARGS) {
  int32 arg1 = PG_GETARG_INT32(0);
  int8_t result;

  if (ty_char_from_int32(arg1, &result))
    ereport(ERROR, (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
                    errmsg("\"char\" out of range")));
  PG_RETURN_CHAR(result);
}

Datum text_char(PG_FUNCTION_ARGS) {
  text *arg1 = PG_GETARG_TEXT_PP(0);

  /* the same rules as charin; the empty text is 0 */
  PG_RETURN_CHAR(ty_char_parse(VARDATA_ANY(arg1), VARSIZE_ANY_EXHDR(arg1)));
}

Datum char_text(PG_FUNCTION_ARGS) {
  char arg1 = PG_GETARG_CHAR(0);
  text *result = (text *)palloc(VARHDRSZ + 4);
  char buf[5];
  size_t len = ty_char_format((int8_t)arg1, buf);

  /* the same rules as charout, with 0x00 as an empty text */
  SET_VARSIZE(result, VARHDRSZ + len);
  memcpy(VARDATA(result), buf, len);
  PG_RETURN_TEXT_P(result);
}

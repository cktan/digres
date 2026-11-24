/*-------------------------------------------------------------------------
 * oracle_compat.c
 *	Oracle compatible functions.
 *
 * Copyright (c) 1996-2025, PostgreSQL Global Development Group
 *
 *	Author: Edmund Mergl <E.Mergl@bawue.de>
 *	Multibyte enhancement: Tatsuo Ishii <ishii@postgresql.org>
 *
 *
 * IDENTIFICATION
 *	src/backend/utils/adt/oracle_compat.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "common/int.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "utils/builtins.h"
#include "utils/formatting.h"
#include "utils/memutils.h"
#include "varatt.h"
#include "ty_bytea.h"
#include "ty_text.h"
#include "utils/ty_glue.h"

static text *dotrim(text *string, text *set, bool doltrim, bool dortrim);

/* The set of btrim(s), ltrim(s) and rtrim(s) with one argument: a space. */
typedef struct {
  int32 vl_len_;
  char data[4];
} space_set_t;
#define SPACE_SET(name)                  \
  space_set_t name##_v;                  \
  text *name = (text *)&name##_v;        \
  SET_VARSIZE(&name##_v, VARHDRSZ + 1);  \
  name##_v.data[0] = ' '

static bytea *dobyteatrim(bytea *string, bytea *set, bool doltrim,
                          bool dortrim);

/********************************************************************
 *
 * lower
 *
 * Syntax:
 *
 *	 text lower(text string)
 *
 * Purpose:
 *
 *	 Returns string, with all letters forced to lowercase.
 *
 ********************************************************************/

Datum lower(PG_FUNCTION_ARGS) {
  text *in_string = PG_GETARG_TEXT_PP(0);
  char *out_string;
  text *result;

  out_string = str_tolower(VARDATA_ANY(in_string), VARSIZE_ANY_EXHDR(in_string),
                           PG_GET_COLLATION());
  result = cstring_to_text(out_string);
  pfree(out_string);

  PG_RETURN_TEXT_P(result);
}

/********************************************************************
 *
 * upper
 *
 * Syntax:
 *
 *	 text upper(text string)
 *
 * Purpose:
 *
 *	 Returns string, with all letters forced to uppercase.
 *
 ********************************************************************/

Datum upper(PG_FUNCTION_ARGS) {
  text *in_string = PG_GETARG_TEXT_PP(0);
  char *out_string;
  text *result;

  out_string = str_toupper(VARDATA_ANY(in_string), VARSIZE_ANY_EXHDR(in_string),
                           PG_GET_COLLATION());
  result = cstring_to_text(out_string);
  pfree(out_string);

  PG_RETURN_TEXT_P(result);
}

/********************************************************************
 *
 * initcap
 *
 * Syntax:
 *
 *	 text initcap(text string)
 *
 * Purpose:
 *
 *	 Returns string, with first letter of each word in uppercase, all
 *	 other letters in lowercase. A word is defined as a sequence of
 *	 alphanumeric characters, delimited by non-alphanumeric
 *	 characters.
 *
 ********************************************************************/

Datum initcap(PG_FUNCTION_ARGS) {
  text *in_string = PG_GETARG_TEXT_PP(0);
  char *out_string;
  text *result;

  out_string = str_initcap(VARDATA_ANY(in_string), VARSIZE_ANY_EXHDR(in_string),
                           PG_GET_COLLATION());
  result = cstring_to_text(out_string);
  pfree(out_string);

  PG_RETURN_TEXT_P(result);
}

Datum casefold(PG_FUNCTION_ARGS) {
  text *in_string = PG_GETARG_TEXT_PP(0);
  char *out_string;
  text *result;

  out_string = str_casefold(VARDATA_ANY(in_string),
                            VARSIZE_ANY_EXHDR(in_string), PG_GET_COLLATION());
  result = cstring_to_text(out_string);
  pfree(out_string);

  PG_RETURN_TEXT_P(result);
}

/********************************************************************
 *
 * lpad
 *
 * Syntax:
 *
 *	 text lpad(text string1, int4 len, text string2)
 *
 * Purpose:
 *
 *	 Returns string1, left-padded to length len with the sequence of
 *	 characters in string2.  If len is less than the length of string1,
 *	 instead truncate (on the right) to len.
 *
 ********************************************************************/

Datum lpad(PG_FUNCTION_ARGS) {
  text *string1 = PG_GETARG_TEXT_PP(0);
  int32 len = PG_GETARG_INT32(1);
  text *string2 = PG_GETARG_TEXT_PP(2);
  text *ret;

  if (ty_text_lpad(TYV(string1), len, TYV(string2), TYVO(&ret)))
    ty_raise_alloc();

  PG_RETURN_TEXT_P(ret);
}

/********************************************************************
 *
 * rpad
 *
 * Syntax:
 *
 *	 text rpad(text string1, int4 len, text string2)
 *
 * Purpose:
 *
 *	 Returns string1, right-padded to length len with the sequence of
 *	 characters in string2.  If len is less than the length of string1,
 *	 instead truncate (on the right) to len.
 *
 ********************************************************************/

Datum rpad(PG_FUNCTION_ARGS) {
  text *string1 = PG_GETARG_TEXT_PP(0);
  int32 len = PG_GETARG_INT32(1);
  text *string2 = PG_GETARG_TEXT_PP(2);
  text *ret;

  if (ty_text_rpad(TYV(string1), len, TYV(string2), TYVO(&ret)))
    ty_raise_alloc();

  PG_RETURN_TEXT_P(ret);
}

/********************************************************************
 *
 * btrim
 *
 * Syntax:
 *
 *	 text btrim(text string, text set)
 *
 * Purpose:
 *
 *	 Returns string with characters removed from the front and back
 *	 up to the first character not in set.
 *
 ********************************************************************/

Datum btrim(PG_FUNCTION_ARGS) {
  text *string = PG_GETARG_TEXT_PP(0);
  text *set = PG_GETARG_TEXT_PP(1);
  text *ret;

  ret = dotrim(string, set, true, true);

  PG_RETURN_TEXT_P(ret);
}

/********************************************************************
 *
 * btrim1 --- btrim with set fixed as ' '
 *
 ********************************************************************/

Datum btrim1(PG_FUNCTION_ARGS) {
  text *string = PG_GETARG_TEXT_PP(0);
  text *ret;

  SPACE_SET(set);

  ret = dotrim(string, set, true, true);

  PG_RETURN_TEXT_P(ret);
}

/*
 * Common implementation for btrim, ltrim, rtrim
 */
static text *dotrim(text *string, text *set, bool doltrim, bool dortrim) {
  text *ret;

  /* ty makes the part that is left */
  if (ty_text_trim(TYV(string), TYV(set), doltrim, dortrim, TYVO(&ret)))
    ty_raise_alloc();
  return ret;
}

/*
 * Common implementation for bytea versions of btrim, ltrim, rtrim
 */
bytea *dobyteatrim(bytea *string, bytea *set, bool doltrim, bool dortrim) {
  bytea *ret;
  size_t off, len;

  /* ty finds the part that is left; it is copied here */
  ty_bytea_trim_range(TYV(string), TYV(set), doltrim, dortrim, &off, &len);
  ret = (bytea *)palloc(VARHDRSZ + len);
  SET_VARSIZE(ret, VARHDRSZ + len);
  memcpy(VARDATA(ret), VARDATA_ANY(string) + off, len);
  return ret;
}

/********************************************************************
 *
 * byteatrim
 *
 * Syntax:
 *
 *	 bytea byteatrim(bytea string, bytea set)
 *
 * Purpose:
 *
 *	 Returns string with characters removed from the front and back
 *	 up to the first character not in set.
 *
 * Cloned from btrim and modified as required.
 ********************************************************************/

Datum byteatrim(PG_FUNCTION_ARGS) {
  bytea *string = PG_GETARG_BYTEA_PP(0);
  bytea *set = PG_GETARG_BYTEA_PP(1);
  bytea *ret;

  ret = dobyteatrim(string, set, true, true);

  PG_RETURN_BYTEA_P(ret);
}

/********************************************************************
 *
 * bytealtrim
 *
 * Syntax:
 *
 *	 bytea bytealtrim(bytea string, bytea set)
 *
 * Purpose:
 *
 *	 Returns string with initial characters removed up to the first
 *	 character not in set.
 *
 ********************************************************************/

Datum bytealtrim(PG_FUNCTION_ARGS) {
  bytea *string = PG_GETARG_BYTEA_PP(0);
  bytea *set = PG_GETARG_BYTEA_PP(1);
  bytea *ret;

  ret = dobyteatrim(string, set, true, false);

  PG_RETURN_BYTEA_P(ret);
}

/********************************************************************
 *
 * byteartrim
 *
 * Syntax:
 *
 *	 bytea byteartrim(bytea string, bytea set)
 *
 * Purpose:
 *
 *	 Returns string with final characters removed after the last
 *	 character not in set.
 *
 ********************************************************************/

Datum byteartrim(PG_FUNCTION_ARGS) {
  bytea *string = PG_GETARG_BYTEA_PP(0);
  bytea *set = PG_GETARG_BYTEA_PP(1);
  bytea *ret;

  ret = dobyteatrim(string, set, false, true);

  PG_RETURN_BYTEA_P(ret);
}

/********************************************************************
 *
 * ltrim
 *
 * Syntax:
 *
 *	 text ltrim(text string, text set)
 *
 * Purpose:
 *
 *	 Returns string with initial characters removed up to the first
 *	 character not in set.
 *
 ********************************************************************/

Datum ltrim(PG_FUNCTION_ARGS) {
  text *string = PG_GETARG_TEXT_PP(0);
  text *set = PG_GETARG_TEXT_PP(1);
  text *ret;

  ret = dotrim(string, set, true, false);

  PG_RETURN_TEXT_P(ret);
}

/********************************************************************
 *
 * ltrim1 --- ltrim with set fixed as ' '
 *
 ********************************************************************/

Datum ltrim1(PG_FUNCTION_ARGS) {
  text *string = PG_GETARG_TEXT_PP(0);
  text *ret;

  SPACE_SET(set);

  ret = dotrim(string, set, true, false);

  PG_RETURN_TEXT_P(ret);
}

/********************************************************************
 *
 * rtrim
 *
 * Syntax:
 *
 *	 text rtrim(text string, text set)
 *
 * Purpose:
 *
 *	 Returns string with final characters removed after the last
 *	 character not in set.
 *
 ********************************************************************/

Datum rtrim(PG_FUNCTION_ARGS) {
  text *string = PG_GETARG_TEXT_PP(0);
  text *set = PG_GETARG_TEXT_PP(1);
  text *ret;

  ret = dotrim(string, set, false, true);

  PG_RETURN_TEXT_P(ret);
}

/********************************************************************
 *
 * rtrim1 --- rtrim with set fixed as ' '
 *
 ********************************************************************/

Datum rtrim1(PG_FUNCTION_ARGS) {
  text *string = PG_GETARG_TEXT_PP(0);
  text *ret;

  SPACE_SET(set);

  ret = dotrim(string, set, false, true);

  PG_RETURN_TEXT_P(ret);
}

/********************************************************************
 *
 * translate
 *
 * Syntax:
 *
 *	 text translate(text string, text from, text to)
 *
 * Purpose:
 *
 *	 Returns string after replacing all occurrences of characters in from
 *	 with the corresponding character in to.  If from is longer than to,
 *	 occurrences of the extra characters in from are deleted.
 *	 Improved by Edwin Ramirez <ramirez@doc.mssm.edu>.
 *
 ********************************************************************/

Datum translate(PG_FUNCTION_ARGS) {
  text *string = PG_GETARG_TEXT_PP(0);
  text *from = PG_GETARG_TEXT_PP(1);
  text *to = PG_GETARG_TEXT_PP(2);
  text *result;

  /* ty makes the result, or gives string if it is empty */
  if (ty_text_translate(TYV(string), TYV(from), TYV(to), TYVO(&result)))
    ty_raise_alloc();

  PG_RETURN_TEXT_P(result);
}

/********************************************************************
 *
 * ascii
 *
 * Syntax:
 *
 *	 int ascii(text string)
 *
 * Purpose:
 *
 *	 Returns the decimal representation of the first character from
 *	 string.
 *	 If the string is empty we return 0.
 *	 If the database encoding is UTF8, we return the Unicode codepoint.
 *	 If the database encoding is any other multi-byte encoding, we
 *	 return the value of the first byte if it is an ASCII character
 *	 (range 1 .. 127), or raise an error.
 *	 For all other encodings we return the value of the first byte,
 *	 (range 1..255).
 *
 ********************************************************************/

Datum ascii(PG_FUNCTION_ARGS) {
  text *string = PG_GETARG_TEXT_PP(0);
  int32 result;

  if (ty_text_ascii(TYV(string), &result))
    ereport(ERROR, (errcode(ERRCODE_CHARACTER_NOT_IN_REPERTOIRE),
                    errmsg("invalid byte sequence for encoding \"%s\"",
                           GetDatabaseEncodingName())));

  PG_RETURN_INT32(result);
}

/********************************************************************
 *
 * chr
 *
 * Syntax:
 *
 *	 text chr(int val)
 *
 * Purpose:
 *
 *	Returns the character having the binary equivalent to val.
 *
 * For UTF8 we treat the argument as a Unicode code point.
 * For other multi-byte encodings we raise an error for arguments
 * outside the strict ASCII range (1..127).
 *
 * It's important that we don't ever return a value that is not valid
 * in the database encoding, so that this doesn't become a way for
 * invalid data to enter the database.
 *
 ********************************************************************/

Datum chr(PG_FUNCTION_ARGS) {
  int32 arg = PG_GETARG_INT32(0);
  text *result;

  if (ty_text_chr(arg, TYVO(&result))) {
    /* the number was negative, or no character: ty's message says which */
    if (ty_errnum() == TY_ERR_NOMEM)
      ty_raise_alloc();
    ereport(ERROR, (errcode(ty_errnum() == TY_ERR_INVALID
                                ? ERRCODE_INVALID_PARAMETER_VALUE
                                : ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                    errmsg("%s", ty_errmsg())));
  }

  PG_RETURN_TEXT_P(result);
}

/********************************************************************
 *
 * repeat
 *
 * Syntax:
 *
 *	 text repeat(text string, int val)
 *
 * Purpose:
 *
 *	Repeat string by val.
 *
 ********************************************************************/

Datum repeat(PG_FUNCTION_ARGS) {
  text *string = PG_GETARG_TEXT_PP(0);
  int32 count = PG_GETARG_INT32(1);
  text *result;

  if (ty_text_repeat(TYV(string), count, TYVO(&result)))
    ty_raise_alloc();

  PG_RETURN_TEXT_P(result);
}

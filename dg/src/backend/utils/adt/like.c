/*-------------------------------------------------------------------------
 *
 * like.c
 *	  like expression handling code.
 *
 *	 NOTES
 *		A big hack of the regexp.c code!! Contributed by
 *		Keith Parks <emkxp01@mtcc.demon.co.uk> (7/95).
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	src/backend/utils/adt/like.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <ctype.h>

#include "catalog/pg_collation.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "utils/fmgrprotos.h"
#include "utils/pg_locale.h"
#include "varatt.h"
#include "ty_text.h"
#include "utils/ty_glue.h"

#define LIKE_TRUE						1
#define LIKE_FALSE						0
#define LIKE_ABORT						(-1)

static int SB_MatchText(const char *t, int tlen, const char *p, int plen,
                        pg_locale_t locale);

static int MB_MatchText(const char *t, int tlen, const char *p, int plen,
                        pg_locale_t locale);

static int UTF8_MatchText(const char *t, int tlen, const char *p, int plen,
                          pg_locale_t locale);

static int SB_IMatchText(const char *t, int tlen, const char *p, int plen,
                         pg_locale_t locale);

static int GenericMatchText(const char *s, int slen, const char *p, int plen,
                            Oid collation);
static int Generic_Text_IC_like(text *str, text *pat, Oid collation);

/*
 * Formerly we had a routine iwchareq() here that tried to do case-insensitive
 * comparison of multibyte characters.  It did not work at all, however,
 * because it relied on tolower() which has a single-byte API ... and
 * towlower() wouldn't be much better since we have no suitably cheap way
 * of getting a single character transformed to the system's wchar_t format.
 * So now, we just downcase the strings using lower() and apply regular LIKE
 * comparison.  This should be revisited when we install better locale support.
 */

/*
 * We do handle case-insensitive matching for single-byte encodings using
 * fold-on-the-fly processing, however.
 */
static char SB_lower_char(unsigned char c, pg_locale_t locale) {
  if (locale->ctype_is_c)
    return pg_ascii_tolower(c);
  else if (locale->is_default)
    return pg_tolower(c);
  else
    return tolower_l(c, locale->info.lt);
}

#define NextByte(p, plen)	((p)++, (plen)--)

/* Set up to compile like_match.c for multibyte characters */
#define NextChar(p, plen) \
	do { int __l = pg_mblen_with_len((p), (plen)); (p) +=__l; (plen) -=__l; } while (0)

#define MatchText	MB_MatchText

#include "like_match.c"

/* Set up to compile like_match.c for single-byte characters */
#define NextChar(p, plen) NextByte((p), (plen))

#define MatchText	SB_MatchText

#include "like_match.c"

/* setup to compile like_match.c for single byte case insensitive matches */
#define MATCH_LOWER(t, locale) SB_lower_char((unsigned char) (t), locale)
#define NextChar(p, plen) NextByte((p), (plen))
#define MatchText SB_IMatchText

#include "like_match.c"

/* setup to compile like_match.c for UTF8 encoding, using fast NextChar */

#define NextChar(p, plen) \
	do { (p)++; (plen)--; } while ((plen) > 0 && (*(p) & 0xC0) == 0x80 )
#define MatchText	UTF8_MatchText

#include "like_match.c"

/* Generic for all cases not requiring inline case-folding */
/*
 * Match with ty, which does the LIKE of a deterministic collation: utf8 is 1
 * for text and 0 for bytea. Its errors are PostgreSQL's.
 */
static int like_ty(const char *s, int slen, const char *p, int plen,
                   int utf8) {
  int r = ty_text_like_bytes(s, slen, p, plen, utf8);

  if (r < 0) {
    if (ty_errnum() == TY_ERR_RANGE)
      ereport(ERROR,
              (errcode(ERRCODE_STATEMENT_TOO_COMPLEX),
               errmsg("stack depth limit exceeded")));
    ereport(ERROR,
            (errcode(ERRCODE_INVALID_ESCAPE_SEQUENCE),
             errmsg("LIKE pattern must not end with escape character")));
  }
  return r;
}

static inline int GenericMatchText(const char *s, int slen, const char *p,
                                   int plen, Oid collation) {
  pg_locale_t locale;

  if (!OidIsValid(collation)) {
    /*
     * This typically means that the parser could not resolve a conflict
     * of implicit collations, so report it that way.
     */
    ereport(
        ERROR,
        (errcode(ERRCODE_INDETERMINATE_COLLATION),
         errmsg("could not determine which collation to use for LIKE"),
         errhint("Use the COLLATE clause to set the collation explicitly.")));
  }

  locale = pg_newlocale_from_collation(collation);

  /* a deterministic collation: ty matches the bytes (the encoding is UTF8) */
  if (locale->deterministic)
    return like_ty(s, slen, p, plen, 1);

  if (pg_database_encoding_max_length() == 1)
    return SB_MatchText(s, slen, p, plen, locale);
  else if (GetDatabaseEncoding() == PG_UTF8)
    return UTF8_MatchText(s, slen, p, plen, locale);
  else
    return MB_MatchText(s, slen, p, plen, locale);
}

static inline int Generic_Text_IC_like(text *str, text *pat, Oid collation) {
  char *s, *p;
  int slen, plen;
  pg_locale_t locale;

  if (!OidIsValid(collation)) {
    /*
     * This typically means that the parser could not resolve a conflict
     * of implicit collations, so report it that way.
     */
    ereport(
        ERROR,
        (errcode(ERRCODE_INDETERMINATE_COLLATION),
         errmsg("could not determine which collation to use for ILIKE"),
         errhint("Use the COLLATE clause to set the collation explicitly.")));
  }

  locale = pg_newlocale_from_collation(collation);

  if (!locale->deterministic)
    ereport(
        ERROR,
        (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
         errmsg("nondeterministic collations are not supported for ILIKE")));

  /*
   * For efficiency reasons, in the single byte case we don't call lower()
   * on the pattern and text, but instead call SB_lower_char on each
   * character.  In the multi-byte case we don't have much choice :-(. Also,
   * ICU does not support single-character case folding, so we go the long
   * way.
   */

  if (pg_database_encoding_max_length() > 1 ||
      (locale->provider == COLLPROVIDER_ICU)) {
    pat = DatumGetTextPP(
        DirectFunctionCall1Coll(lower, collation, PointerGetDatum(pat)));
    p = VARDATA_ANY(pat);
    plen = VARSIZE_ANY_EXHDR(pat);
    str = DatumGetTextPP(
        DirectFunctionCall1Coll(lower, collation, PointerGetDatum(str)));
    s = VARDATA_ANY(str);
    slen = VARSIZE_ANY_EXHDR(str);
    if (GetDatabaseEncoding() == PG_UTF8)
      return like_ty(s, slen, p, plen, 1);
    else
      return MB_MatchText(s, slen, p, plen, 0);
  } else {
    p = VARDATA_ANY(pat);
    plen = VARSIZE_ANY_EXHDR(pat);
    s = VARDATA_ANY(str);
    slen = VARSIZE_ANY_EXHDR(str);
    return SB_IMatchText(s, slen, p, plen, locale);
  }
}

/*
 *	interface routines called by the function manager
 */

Datum namelike(PG_FUNCTION_ARGS) {
  Name str = PG_GETARG_NAME(0);
  text *pat = PG_GETARG_TEXT_PP(1);
  bool result;
  char *s, *p;
  int slen, plen;

  s = NameStr(*str);
  slen = strlen(s);
  p = VARDATA_ANY(pat);
  plen = VARSIZE_ANY_EXHDR(pat);

  result =
      (GenericMatchText(s, slen, p, plen, PG_GET_COLLATION()) == LIKE_TRUE);

  PG_RETURN_BOOL(result);
}

Datum namenlike(PG_FUNCTION_ARGS) {
  Name str = PG_GETARG_NAME(0);
  text *pat = PG_GETARG_TEXT_PP(1);
  bool result;
  char *s, *p;
  int slen, plen;

  s = NameStr(*str);
  slen = strlen(s);
  p = VARDATA_ANY(pat);
  plen = VARSIZE_ANY_EXHDR(pat);

  result =
      (GenericMatchText(s, slen, p, plen, PG_GET_COLLATION()) != LIKE_TRUE);

  PG_RETURN_BOOL(result);
}

Datum textlike(PG_FUNCTION_ARGS) {
  text *str = PG_GETARG_TEXT_PP(0);
  text *pat = PG_GETARG_TEXT_PP(1);
  bool result;
  char *s, *p;
  int slen, plen;

  s = VARDATA_ANY(str);
  slen = VARSIZE_ANY_EXHDR(str);
  p = VARDATA_ANY(pat);
  plen = VARSIZE_ANY_EXHDR(pat);

  result =
      (GenericMatchText(s, slen, p, plen, PG_GET_COLLATION()) == LIKE_TRUE);

  PG_RETURN_BOOL(result);
}

Datum textnlike(PG_FUNCTION_ARGS) {
  text *str = PG_GETARG_TEXT_PP(0);
  text *pat = PG_GETARG_TEXT_PP(1);
  bool result;
  char *s, *p;
  int slen, plen;

  s = VARDATA_ANY(str);
  slen = VARSIZE_ANY_EXHDR(str);
  p = VARDATA_ANY(pat);
  plen = VARSIZE_ANY_EXHDR(pat);

  result =
      (GenericMatchText(s, slen, p, plen, PG_GET_COLLATION()) != LIKE_TRUE);

  PG_RETURN_BOOL(result);
}

Datum bytealike(PG_FUNCTION_ARGS) {
  bytea *str = PG_GETARG_BYTEA_PP(0);
  bytea *pat = PG_GETARG_BYTEA_PP(1);
  bool result;
  char *s, *p;
  int slen, plen;

  s = VARDATA_ANY(str);
  slen = VARSIZE_ANY_EXHDR(str);
  p = VARDATA_ANY(pat);
  plen = VARSIZE_ANY_EXHDR(pat);

  result = (like_ty(s, slen, p, plen, 0) == LIKE_TRUE);

  PG_RETURN_BOOL(result);
}

Datum byteanlike(PG_FUNCTION_ARGS) {
  bytea *str = PG_GETARG_BYTEA_PP(0);
  bytea *pat = PG_GETARG_BYTEA_PP(1);
  bool result;
  char *s, *p;
  int slen, plen;

  s = VARDATA_ANY(str);
  slen = VARSIZE_ANY_EXHDR(str);
  p = VARDATA_ANY(pat);
  plen = VARSIZE_ANY_EXHDR(pat);

  result = (like_ty(s, slen, p, plen, 0) != LIKE_TRUE);

  PG_RETURN_BOOL(result);
}

/*
 * Case-insensitive versions
 */

Datum nameiclike(PG_FUNCTION_ARGS) {
  Name str = PG_GETARG_NAME(0);
  text *pat = PG_GETARG_TEXT_PP(1);
  bool result;
  text *strtext;

  strtext = DatumGetTextPP(DirectFunctionCall1(name_text, NameGetDatum(str)));
  result =
      (Generic_Text_IC_like(strtext, pat, PG_GET_COLLATION()) == LIKE_TRUE);

  PG_RETURN_BOOL(result);
}

Datum nameicnlike(PG_FUNCTION_ARGS) {
  Name str = PG_GETARG_NAME(0);
  text *pat = PG_GETARG_TEXT_PP(1);
  bool result;
  text *strtext;

  strtext = DatumGetTextPP(DirectFunctionCall1(name_text, NameGetDatum(str)));
  result =
      (Generic_Text_IC_like(strtext, pat, PG_GET_COLLATION()) != LIKE_TRUE);

  PG_RETURN_BOOL(result);
}

Datum texticlike(PG_FUNCTION_ARGS) {
  text *str = PG_GETARG_TEXT_PP(0);
  text *pat = PG_GETARG_TEXT_PP(1);
  bool result;

  result = (Generic_Text_IC_like(str, pat, PG_GET_COLLATION()) == LIKE_TRUE);

  PG_RETURN_BOOL(result);
}

Datum texticnlike(PG_FUNCTION_ARGS) {
  text *str = PG_GETARG_TEXT_PP(0);
  text *pat = PG_GETARG_TEXT_PP(1);
  bool result;

  result = (Generic_Text_IC_like(str, pat, PG_GET_COLLATION()) != LIKE_TRUE);

  PG_RETURN_BOOL(result);
}

/*
 * like_escape() --- given a pattern and an ESCAPE string,
 * convert the pattern to use Postgres' standard backslash escape convention.
 */
Datum like_escape(PG_FUNCTION_ARGS) {
  text *pat = PG_GETARG_TEXT_PP(0);
  text *esc = PG_GETARG_TEXT_PP(1);
  text *result;

  /* ty makes the pattern of the default escape */
  if (ty_text_like_escape(TYV(pat), TYV(esc), 1, TYVO(&result))) {
    if (ty_errnum() != TY_ERR_INVALID)
      ty_raise_alloc();
    ereport(ERROR, (errcode(ERRCODE_INVALID_ESCAPE_SEQUENCE),
                    errmsg("invalid escape string"),
                    errhint("Escape string must be empty or one character.")));
  }

  PG_RETURN_TEXT_P(result);
}

/*
 * like_escape_bytea() --- given a pattern and an ESCAPE string,
 * convert the pattern to use Postgres' standard backslash escape convention.
 */
Datum like_escape_bytea(PG_FUNCTION_ARGS) {
  bytea *pat = PG_GETARG_BYTEA_PP(0);
  bytea *esc = PG_GETARG_BYTEA_PP(1);
  bytea *result;

  /* ty makes the pattern of the default escape */
  if (ty_text_like_escape(TYV(pat), TYV(esc), 0, TYVO(&result))) {
    if (ty_errnum() != TY_ERR_INVALID)
      ty_raise_alloc();
    ereport(ERROR, (errcode(ERRCODE_INVALID_ESCAPE_SEQUENCE),
                    errmsg("invalid escape string"),
                    errhint("Escape string must be empty or one character.")));
  }

  PG_RETURN_BYTEA_P(result);
}

/* -----------------------------------------------------------------------
 * formatting.c
 *
 * src/backend/utils/adt/formatting.c
 *
 *
 *	 Portions Copyright (c) 1999-2025, PostgreSQL Global Development Group
 *	 Portions Copyright (c) 2026, CK Tan: the changes for digres,
 *	 under the MIT License (https://github.com/cktan/digres/blob/main/LICENSE)
 *
 *
 *	 TO_CHAR(); TO_TIMESTAMP(); TO_DATE(); TO_NUMBER();
 *
 *	 The PostgreSQL routines for a timestamp/int/float/numeric formatting,
 *	 inspired by the Oracle TO_CHAR() / TO_DATE() / TO_NUMBER() routines.
 *
 *
 *	 Cache & Memory:
 *	Routines use (itself) internal cache for format pictures.
 *
 *	The cache uses a static buffer and is persistent across transactions. If
 *	the format-picture is bigger than the cache buffer, the parser is called
 *	always.
 *
 *	 NOTE for Number version:
 *	All in this version is implemented as keywords ( => not used
 *	suffixes), because a format picture is for *one* item (number)
 *	only. It not is as a timestamp version, where each keyword (can)
 *	has suffix.
 *
 *	 NOTE for Timestamp routines:
 *	In this module the POSIX 'struct tm' type is *not* used, but rather
 *	PgSQL type, which has tm_mon based on one (*non* zero) and
 *	year *not* based on 1900, but is used full year number.
 *	Module supports AD / BC / AM / PM.
 *
 *	Supported types for to_char():
 *
 *		Timestamp, Numeric, int4, int8, float4, float8
 *
 *	Supported types for reverse conversion:
 *
 *		Timestamp	- to_timestamp()
 *		Date		- to_date()
 *		Numeric		- to_number()
 *
 *
 *	Karel Zak
 *
 * TODO
 *	- better number building (formatting) / parsing, now it isn't
 *		  ideal code
 *	- use Assert()
 *	- add support for number spelling
 *	- add support for string to string formatting (we must be better
 *	  than Oracle :-),
 *		to_char('Hello', 'X X X X X') -> 'H e l l o'
 *
 * -----------------------------------------------------------------------
 */

#ifdef DEBUG_TO_FROM_CHAR
#define DEBUG_elog_output	DEBUG3
#endif

#include "postgres.h"

#include <ctype.h>
#include <unistd.h>
#include <math.h>
#include <float.h>
#include <limits.h>
#include <wctype.h>

#ifdef USE_ICU
#include <unicode/ustring.h>
#endif

#include "catalog/pg_collation.h"
#include "catalog/pg_type.h"
#include "common/int.h"
#include "common/unicode_case.h"
#include "common/unicode_category.h"
#include "mb/pg_wchar.h"
#include "nodes/miscnodes.h"
#include "parser/scansup.h"
#include "utils/builtins.h"
#include "utils/date.h"
#include "utils/datetime.h"
#include "utils/formatting.h"
#include "utils/memutils.h"
#include "utils/numeric.h"
#include "utils/pg_locale.h"
#include "ty_text.h"
#include "ty_tochar.h"
#include "utils/ty_glue.h"
#include "varatt.h"

/* ----------
 * Maximal length of one node
 * ----------
 */
#define NUM_MAX_ITEM_SIZ		8  /* roman number (RN has 15 chars)	*/

#define MAX_L10N_DATA			80 /* max localized day or month name */

/* ----------
 * Format picture cache: the date formats ty compiled (ty_tochar.h), of up
 * to DCH_CACHE_SIZE bytes, at most DCH_CACHE_ENTRIES of them. 119 bytes is
 * what PostgreSQL's cache of FormatNode arrays held, so the same formats
 * are cached. A number format is kept in its call site's fn_extra.
 * ----------
 */
#define DCH_CACHE_SIZE		119
#define DCH_CACHE_ENTRIES	20

typedef struct {
  ty_dch_format_t *format; /* ty's, in TopMemoryContext */
  char str[DCH_CACHE_SIZE + 1];
  bool std;
  bool valid;
  int age;
} DCHCacheEntry;

/* global cache for date/time format pictures */
static thread_local /*LOCAL*/ DCHCacheEntry *DCHCache[DCH_CACHE_ENTRIES];
static thread_local /*LOCAL*/ int n_DCHCache =
    0; /* current number of entries */
static thread_local /*LOCAL*/ int DCHCounter = 0; /* aging-event counter */

/* ----------
 * Functions
 * ----------
 */
static int seq_search_localized(const char *name, char **array, size_t *len,
                                Oid collid);
static bool do_to_timestamp(text *date_txt, text *fmt, Oid collid, bool std,
                            ty_dch_parsed_t *parsed, uint32 *flags,
                            Node *escontext);
static void dch_parsed_tm(const ty_dch_parsed_t *parsed, struct pg_tm *tm);
static DCHCacheEntry *DCH_cache_getnew(const char *str, bool std);
static DCHCacheEntry *DCH_cache_search(const char *str, bool std);
static DCHCacheEntry *DCH_cache_fetch(const char *str, bool std);

/*****************************************************************************
 *			upper/lower/initcap functions
 *****************************************************************************/

/*
 * If the system provides the needed functions for wide-character manipulation
 * (which are all standardized by C99), then we implement upper/lower/initcap
 * using wide-character functions, if necessary.  Otherwise we use the
 * traditional <ctype.h> functions, which of course will not work as desired
 * in multibyte character sets.  Note that in either case we are effectively
 * assuming that the database character encoding matches the encoding implied
 * by LC_CTYPE.
 */

/*
 * collation-aware, wide-character-aware lower function
 *
 * We pass the number of bytes so we can pass varlena and char*
 * to this function.  The result is a palloc'd, null-terminated string.
 */
char *str_tolower(const char *buff, size_t nbytes, Oid collid) {
  char *result;
  pg_locale_t mylocale;

  if (!buff)
    return NULL;

  if (!OidIsValid(collid)) {
    /*
     * This typically means that the parser could not resolve a conflict
     * of implicit collations, so report it that way.
     */
    ereport(
        ERROR,
        (errcode(ERRCODE_INDETERMINATE_COLLATION),
         errmsg("could not determine which collation to use for %s function",
                "lower()"),
         errhint("Use the COLLATE clause to set the collation explicitly.")));
  }

  mylocale = pg_newlocale_from_collation(collid);

  /* C/POSIX collations use this path regardless of database encoding */
  if (mylocale->ctype_is_c) {
    result = asc_tolower(buff, nbytes);
  } else {
    const char *src = buff;
    size_t srclen = nbytes;
    size_t dstsize;
    char *dst;
    size_t needed;

    /* first try buffer of equal size plus terminating NUL */
    dstsize = srclen + 1;
    dst = (char *)palloc(dstsize);

    needed = pg_strlower(dst, dstsize, src, srclen, mylocale);
    if (needed + 1 > dstsize) {
      /* grow buffer if needed and retry */
      dstsize = needed + 1;
      dst = (char *)repalloc(dst, dstsize);
      needed = pg_strlower(dst, dstsize, src, srclen, mylocale);
      Assert(needed + 1 <= dstsize);
    }

    Assert(dst[needed] == '\0');
    result = dst;
  }

  return result;
}

/*
 * collation-aware, wide-character-aware upper function
 *
 * We pass the number of bytes so we can pass varlena and char*
 * to this function.  The result is a palloc'd, null-terminated string.
 */
char *str_toupper(const char *buff, size_t nbytes, Oid collid) {
  char *result;
  pg_locale_t mylocale;

  if (!buff)
    return NULL;

  if (!OidIsValid(collid)) {
    /*
     * This typically means that the parser could not resolve a conflict
     * of implicit collations, so report it that way.
     */
    ereport(
        ERROR,
        (errcode(ERRCODE_INDETERMINATE_COLLATION),
         errmsg("could not determine which collation to use for %s function",
                "upper()"),
         errhint("Use the COLLATE clause to set the collation explicitly.")));
  }

  mylocale = pg_newlocale_from_collation(collid);

  /* C/POSIX collations use this path regardless of database encoding */
  if (mylocale->ctype_is_c) {
    result = asc_toupper(buff, nbytes);
  } else {
    const char *src = buff;
    size_t srclen = nbytes;
    size_t dstsize;
    char *dst;
    size_t needed;

    /* first try buffer of equal size plus terminating NUL */
    dstsize = srclen + 1;
    dst = (char *)palloc(dstsize);

    needed = pg_strupper(dst, dstsize, src, srclen, mylocale);
    if (needed + 1 > dstsize) {
      /* grow buffer if needed and retry */
      dstsize = needed + 1;
      dst = (char *)repalloc(dst, dstsize);
      needed = pg_strupper(dst, dstsize, src, srclen, mylocale);
      Assert(needed + 1 <= dstsize);
    }

    Assert(dst[needed] == '\0');
    result = dst;
  }

  return result;
}

/*
 * collation-aware, wide-character-aware initcap function
 *
 * We pass the number of bytes so we can pass varlena and char*
 * to this function.  The result is a palloc'd, null-terminated string.
 */
char *str_initcap(const char *buff, size_t nbytes, Oid collid) {
  char *result;
  pg_locale_t mylocale;

  if (!buff)
    return NULL;

  if (!OidIsValid(collid)) {
    /*
     * This typically means that the parser could not resolve a conflict
     * of implicit collations, so report it that way.
     */
    ereport(
        ERROR,
        (errcode(ERRCODE_INDETERMINATE_COLLATION),
         errmsg("could not determine which collation to use for %s function",
                "initcap()"),
         errhint("Use the COLLATE clause to set the collation explicitly.")));
  }

  mylocale = pg_newlocale_from_collation(collid);

  /* C/POSIX collations use this path regardless of database encoding */
  if (mylocale->ctype_is_c) {
    result = asc_initcap(buff, nbytes);
  } else {
    const char *src = buff;
    size_t srclen = nbytes;
    size_t dstsize;
    char *dst;
    size_t needed;

    /* first try buffer of equal size plus terminating NUL */
    dstsize = srclen + 1;
    dst = (char *)palloc(dstsize);

    needed = pg_strtitle(dst, dstsize, src, srclen, mylocale);
    if (needed + 1 > dstsize) {
      /* grow buffer if needed and retry */
      dstsize = needed + 1;
      dst = (char *)repalloc(dst, dstsize);
      needed = pg_strtitle(dst, dstsize, src, srclen, mylocale);
      Assert(needed + 1 <= dstsize);
    }

    Assert(dst[needed] == '\0');
    result = dst;
  }

  return result;
}

/*
 * collation-aware, wide-character-aware case folding
 *
 * We pass the number of bytes so we can pass varlena and char*
 * to this function.  The result is a palloc'd, null-terminated string.
 */
char *str_casefold(const char *buff, size_t nbytes, Oid collid) {
  char *result;
  pg_locale_t mylocale;

  if (!buff)
    return NULL;

  if (!OidIsValid(collid)) {
    /*
     * This typically means that the parser could not resolve a conflict
     * of implicit collations, so report it that way.
     */
    ereport(
        ERROR,
        (errcode(ERRCODE_INDETERMINATE_COLLATION),
         errmsg("could not determine which collation to use for %s function",
                "lower()"),
         errhint("Use the COLLATE clause to set the collation explicitly.")));
  }

  if (GetDatabaseEncoding() != PG_UTF8)
    ereport(ERROR, (errcode(ERRCODE_SYNTAX_ERROR),
                    errmsg("Unicode case folding can only be performed if "
                           "server encoding is UTF8")));

  mylocale = pg_newlocale_from_collation(collid);

  /* C/POSIX collations use this path regardless of database encoding */
  if (mylocale->ctype_is_c) {
    result = asc_tolower(buff, nbytes);
  } else {
    const char *src = buff;
    size_t srclen = nbytes;
    size_t dstsize;
    char *dst;
    size_t needed;

    /* first try buffer of equal size plus terminating NUL */
    dstsize = srclen + 1;
    dst = (char *)palloc(dstsize);

    needed = pg_strfold(dst, dstsize, src, srclen, mylocale);
    if (needed + 1 > dstsize) {
      /* grow buffer if needed and retry */
      dstsize = needed + 1;
      dst = (char *)repalloc(dst, dstsize);
      needed = pg_strfold(dst, dstsize, src, srclen, mylocale);
      Assert(needed + 1 <= dstsize);
    }

    Assert(dst[needed] == '\0');
    result = dst;
  }

  return result;
}

/*
 * ASCII-only lower function
 *
 * We pass the number of bytes so we can pass varlena and char*
 * to this function.  The result is a palloc'd, null-terminated string.
 */
char *asc_tolower(const char *buff, size_t nbytes) {
  char *result;

  if (!buff)
    return NULL;

  result = pnstrdup(buff, nbytes);
  /* ty changes the ASCII letters, in place */
  ty_ascii_lower(result, strlen(result), result);

  return result;
}

/*
 * ASCII-only upper function
 *
 * We pass the number of bytes so we can pass varlena and char*
 * to this function.  The result is a palloc'd, null-terminated string.
 */
char *asc_toupper(const char *buff, size_t nbytes) {
  char *result;

  if (!buff)
    return NULL;

  result = pnstrdup(buff, nbytes);
  /* ty changes the ASCII letters, in place */
  ty_ascii_upper(result, strlen(result), result);

  return result;
}

/*
 * ASCII-only initcap function
 *
 * We pass the number of bytes so we can pass varlena and char*
 * to this function.  The result is a palloc'd, null-terminated string.
 */
char *asc_initcap(const char *buff, size_t nbytes) {
  char *result;

  if (!buff)
    return NULL;

  result = pnstrdup(buff, nbytes);
  /* ty changes the ASCII letters, in place */
  ty_ascii_initcap(result, strlen(result), result);

  return result;
}

/* convenience routines for when the input is null-terminated */

static char *str_tolower_z(const char *buff, Oid collid) {
  return str_tolower(buff, strlen(buff), collid);
}

static char *str_toupper_z(const char *buff, Oid collid) {
  return str_toupper(buff, strlen(buff), collid);
}

static char *str_initcap_z(const char *buff, Oid collid) {
  return str_initcap(buff, strlen(buff), collid);
}

/* asc_initcap_z is not currently needed */

/* ----------
 * Skip TM / th in FROM_CHAR
 *
 * If S_THth is on, skip two chars, assuming there are two available
 * ----------
 */


/*
 * Compare 'name' with 'element' in a case-insensitive way, by first
 * converting 'name' to upper case, then lower case. ('element' is already
 * case-folded that way.)
 *
 * A helper function for seq_search_localized().
 */
static bool casefold_str_cmp(const char *name, size_t name_len,
                             const char *element, size_t element_len,
                             pg_locale_t mylocale) {
  /*
   * 'name' is expected to fit in MAX_L10N_DATA, even with the case
   * conversions.
   */
  char upper_substr[MAX_L10N_DATA];
  size_t upper_substr_len;
  char lower_substr[MAX_L10N_DATA];
  size_t lower_substr_len;

  upper_substr_len =
      pg_strupper(upper_substr, sizeof(upper_substr), name, name_len, mylocale);
  if (upper_substr_len > sizeof(upper_substr) - 1)
    return false; /* shouldn't happen */
  lower_substr_len = pg_strlower(lower_substr, sizeof(lower_substr),
                                 upper_substr, upper_substr_len, mylocale);
  if (lower_substr_len > sizeof(lower_substr) - 1)
    return false; /* shouldn't happen */

  return strcmp(lower_substr, element) == 0;
}

/*
 * Sequentially search an array of possibly non-English words for
 * a case-insensitive match to the initial character(s) of "name".
 *
 * This has the same API as seq_search_ascii(), but we use a more general
 * case-folding transformation to achieve case-insensitivity.  Case folding
 * is done per the rules of the collation identified by "collid".
 *
 * The array is treated as const, but we don't declare it that way because
 * the arrays exported by pg_locale.c aren't const.
 */
static int seq_search_localized(const char *name, char **array, size_t *len,
                                Oid collid) {
  size_t name_len = strlen(name);
  const char *name_end = name + name_len;
  char *upper_name;
  char *lower_name;
  pg_locale_t mylocale;

  *len = 0;

  /* empty string can't match anything */
  if (!*name)
    return -1;

  /*
   * The case-folding processing done below is fairly expensive, so before
   * doing that, make a quick pass to see if there is an exact match.
   */
  for (char **a = array; *a != NULL; a++) {
    size_t element_len = strlen(*a);

    if (strncmp(name, *a, element_len) == 0) {
      *len = element_len;
      return a - array;
    }
  }

  mylocale = pg_newlocale_from_collation(collid);

  /*
   * Fold to upper case, then to lower case, so that we can match reliably
   * even in languages in which case conversions are not injective.
   */
  upper_name = str_toupper(name, name_len, collid);
  lower_name = str_tolower(upper_name, strlen(upper_name), collid);
  pfree(upper_name);

  for (char **a = array; *a != NULL; a++) {
    char upper_element[MAX_L10N_DATA];
    size_t upper_element_len;
    char lower_element[MAX_L10N_DATA];
    size_t lower_element_len;

    /* Likewise upper/lower-case array element */
    upper_element_len = pg_strupper(upper_element, sizeof(upper_element), *a,
                                    strlen(*a), mylocale);
    if (upper_element_len > sizeof(upper_element) - 1)
      continue; /* shouldn't happen */
    lower_element_len = pg_strlower(lower_element, sizeof(lower_element),
                                    upper_element, upper_element_len, mylocale);
    if (lower_element_len > sizeof(lower_element) - 1)
      continue; /* shouldn't happen */

    /* Is 'lower_element' a prefix of 'lower_name' ? */
    if (strncmp(lower_name, lower_element, lower_element_len) == 0) {
      /*
       * We have a match, but we still need to figure out how long the
       * match is.  The case conversions could have changed the lengths
       * of either string, or both.
       */
      const char *ep;
      const char *element_end;
      size_t element_nchars;
      size_t substr_len;
      size_t substr_nchars;

      /*
       * First, check the easy case that the string matches as whole.
       */
      if (strlen(lower_name) == lower_element_len) {
        *len = name_len;
        pfree(lower_name);
        return a - array;
      }

      /*
       * Another good guess is that the case conversions did not change
       * the number of characters.
       */

      /* count characters in the element */
      ep = lower_element;
      element_end = lower_element + lower_element_len;
      for (element_nchars = 0; ep < element_end; element_nchars++)
        ep += pg_mblen_range(ep, element_end);

      /*
       * count the byte length of a substring of 'name' having the same
       * character count as the element
       */
      substr_len = 0;
      for (substr_nchars = 0;
           substr_nchars < element_nchars && substr_len < name_len;
           substr_nchars++) {
        substr_len += pg_mblen_range(name + substr_len, name_end);
      }

      if (casefold_str_cmp(name, substr_len, lower_element, lower_element_len,
                           mylocale)) {
        *len = substr_len;
        pfree(lower_name);
        return a - array;
      }

      /*
       * As last resort, try the case conversion and comparison for
       * every substring from the beginning of the original string until
       * we find a match.
       */
      substr_len = 0;
      while (substr_len < name_len) {
        substr_len += pg_mblen_range(name + substr_len, name_end);

        if (casefold_str_cmp(name, substr_len, lower_element, lower_element_len,
                             mylocale)) {
          *len = substr_len;
          pfree(lower_name);
          return a - array;
        }
      }
    }
  }

  pfree(lower_name);
  return -1;
}

/*
 * The invariant for DCH cache entry management is that DCHCounter is equal
 * to the maximum age value among the existing entries, and we increment it
 * whenever an access occurs.  If we approach overflow, deal with that by
 * halving all the age values, so that we retain a fairly accurate idea of
 * which entries are oldest.
 */
static inline void DCH_prevent_counter_overflow(void) {
  if (DCHCounter >= (INT_MAX - 1)) {
    for (int i = 0; i < n_DCHCache; i++)
      DCHCache[i]->age >>= 1;
    DCHCounter >>= 1;
  }
}

/*
 * A date format compiled by ty in the context cxt, or its error: the
 * standard mode's separator, or an allocation's.
 */
static ty_dch_format_t *dch_compile_or_raise(const char *str, size_t len,
                                             bool std, MemoryContext cxt) {
  MemoryContext oldcontext = MemoryContextSwitchTo(cxt);
  ty_dch_format_t *f;
  int rc = ty_dch_compile(str, len, std, &f);

  MemoryContextSwitchTo(oldcontext);
  if (0 == rc)
    return f;
  if (TY_ERR_INVALID == ty_errnum())
    ereport(ERROR, (errcode(ERRCODE_INVALID_DATETIME_FORMAT),
                    errmsg("%s", ty_errmsg())));
  ty_raise_alloc();
}

/* select a DCHCacheEntry to hold the given format picture */
static DCHCacheEntry *DCH_cache_getnew(const char *str, bool std) {
  DCHCacheEntry *ent;

  /* Ensure we can advance DCHCounter below */
  DCH_prevent_counter_overflow();

  /*
   * If cache is full, remove oldest entry (or recycle first not-valid one)
   */
  if (n_DCHCache >= DCH_CACHE_ENTRIES) {
    DCHCacheEntry *old = DCHCache[0];

#ifdef DEBUG_TO_FROM_CHAR
    elog(DEBUG_elog_output, "cache is full (%d)", n_DCHCache);
#endif
    if (old->valid) {
      for (int i = 1; i < DCH_CACHE_ENTRIES; i++) {
        ent = DCHCache[i];
        if (!ent->valid) {
          old = ent;
          break;
        }
        if (ent->age < old->age)
          old = ent;
      }
    }
#ifdef DEBUG_TO_FROM_CHAR
    elog(DEBUG_elog_output, "OLD: '%s' AGE: %d", old->str, old->age);
#endif
    old->valid = false;
    strlcpy(old->str, str, DCH_CACHE_SIZE + 1);
    old->age = (++DCHCounter);
    /* caller is expected to fill format, then set valid */
    return old;
  } else {
#ifdef DEBUG_TO_FROM_CHAR
    elog(DEBUG_elog_output, "NEW (%d)", n_DCHCache);
#endif
    Assert(DCHCache[n_DCHCache] == NULL);
    DCHCache[n_DCHCache] = ent = (DCHCacheEntry *)MemoryContextAllocZero(
        TopMemoryContext, sizeof(DCHCacheEntry));
    ent->valid = false;
    strlcpy(ent->str, str, DCH_CACHE_SIZE + 1);
    ent->std = std;
    ent->age = (++DCHCounter);
    /* caller is expected to fill format, then set valid */
    ++n_DCHCache;
    return ent;
  }
}

/* look for an existing DCHCacheEntry matching the given format picture */
static DCHCacheEntry *DCH_cache_search(const char *str, bool std) {
  /* Ensure we can advance DCHCounter below */
  DCH_prevent_counter_overflow();

  for (int i = 0; i < n_DCHCache; i++) {
    DCHCacheEntry *ent = DCHCache[i];

    if (ent->valid && strcmp(ent->str, str) == 0 && ent->std == std) {
      ent->age = (++DCHCounter);
      return ent;
    }
  }

  return NULL;
}

/* Find or create a DCHCacheEntry for the given format picture */
static DCHCacheEntry *DCH_cache_fetch(const char *str, bool std) {
  DCHCacheEntry *ent;

  if ((ent = DCH_cache_search(str, std)) == NULL) {
    /*
     * Not in the cache, must run parser and save a new format-picture to
     * the cache.  Do not mark the cache entry valid until parsing
     * succeeds.
     */
    ent = DCH_cache_getnew(str, std);

    /* ty compiles it; the entry lives as long as the session */
    if (NULL != ent->format) {
      ty_free(ent->format);
      ent->format = NULL;
    }
    ent->format = dch_compile_or_raise(str, strlen(str), std, TopMemoryContext);
    ent->valid = true;
  }
  return ent;
}

/*
 * The format of a to_char call site, compiled by ty and kept in its
 * fn_extra, with the format's text to tell it is the same next time.
 */
typedef struct ToCharFormat {
  void *format; /* ty's ty_dch_format_t or ty_num_format_t */
  int len;
  char text[FLEXIBLE_ARRAY_MEMBER];
} ToCharFormat;

/*
 * The compiled format fmt: the call site's, if it is the same text, else
 * compiled anew by compile in the call site's memory, or NULL with ty's
 * error. A call with no FmgrInfo compiles into the current context.
 * 1. The call site's format, if its text is fmt's.
 * 2. Else free it, and forget it before anything can fail: an error the
 *    caller catches must not leave fn_extra pointing at freed memory.
 * 3. Compile fmt, and keep it only if it compiles.
 */
static void *tochar_format(FunctionCallInfo fcinfo, text *fmt,
                           int (*compile)(const char *, size_t, void **)) {
  FmgrInfo *flinfo = fcinfo->flinfo;
  ToCharFormat *cache = flinfo ? (ToCharFormat *)flinfo->fn_extra : NULL;
  int len = VARSIZE_ANY_EXHDR(fmt);
  MemoryContext oldcontext;
  void *format;

  if (NULL != cache && cache->len == len &&
      0 == memcmp(cache->text, VARDATA_ANY(fmt), len))
    return cache->format;
  if (NULL != cache) {
    flinfo->fn_extra = NULL;
    ty_free(cache->format);
    pfree(cache);
  }

  /* the compiled format lives as long as the call site */
  oldcontext =
      MemoryContextSwitchTo(flinfo ? flinfo->fn_mcxt : CurrentMemoryContext);
  if (compile(VARDATA_ANY(fmt), len, &format)) {
    MemoryContextSwitchTo(oldcontext);
    return NULL;
  }
  cache = (ToCharFormat *)palloc(offsetof(ToCharFormat, text) + len);
  cache->format = format;
  cache->len = len;
  memcpy(cache->text, VARDATA_ANY(fmt), len);
  MemoryContextSwitchTo(oldcontext);
  if (flinfo)
    flinfo->fn_extra = cache;
  return format;
}

/* ty_dch_compile, as tochar_format calls it. */
static int dch_compile(const char *fmt, size_t len, void **out) {
  return ty_dch_compile(fmt, len, 0, (ty_dch_format_t **)out);
}

/* The compiled date format of the call site; it fails only for memory. */
static const ty_dch_format_t *dch_format(FunctionCallInfo fcinfo, text *fmt) {
  void *f = tochar_format(fcinfo, fmt, dch_compile);

  if (NULL == f)
    ty_raise_alloc();
  return (const ty_dch_format_t *)f;
}

/*
 * TM's names, for ty: the localized names of lc_time, cased under the
 * collation ctx points to.
 */
static int dch_tm_name(void *ctx, int kind, int i, int c, const char **name,
                       size_t *len) {
  Oid collid = *(Oid *)ctx;
  const char *src;
  char *str;

  /* cache localized days and months */
  cache_locale_time();
  src = TY_TOCHAR_FULL_MONTH == kind     ? localized_full_months[i]
        : TY_TOCHAR_ABBREV_MONTH == kind ? localized_abbrev_months[i]
        : TY_TOCHAR_FULL_DAY == kind     ? localized_full_days[i]
                                         : localized_abbrev_days[i];
  str = TY_TOCHAR_UPPER == c     ? str_toupper_z(src, collid)
        : TY_TOCHAR_INITCAP == c ? str_initcap_z(src, collid)
                                 : str_tolower_z(src, collid);
  *name = str;
  *len = strlen(str);
  return 0;
}

/*
 * Finish a to_char by ty: NULL for no result, the text, or ty's error with
 * PostgreSQL's code and hint.
 */
static Datum dch_result(FunctionCallInfo fcinfo, int rc, text *res) {
  if (1 == rc)
    PG_RETURN_NULL();
  if (0 == rc)
    PG_RETURN_TEXT_P(res);
  if (TY_ERR_INVALID == ty_errnum())
    ereport(ERROR, (errcode(ERRCODE_INVALID_DATETIME_FORMAT),
                    errmsg("%s", ty_errmsg()),
                    errhint("Intervals are not tied to specific calendar "
                            "dates.")));
  if (TY_ERR_RANGE == ty_errnum())
    ereport(ERROR, (errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
                    errmsg("%s", ty_errmsg())));
  ty_raise_alloc();
}

/****************************************************************************
 *				Public routines
 ***************************************************************************/

/* -------------------
 * TIMESTAMP to_char(): ty writes it (ty_tochar.h)
 * -------------------
 */
Datum timestamp_to_char(PG_FUNCTION_ARGS) {
  Timestamp dt = PG_GETARG_TIMESTAMP(0);
  text *fmt = PG_GETARG_TEXT_PP(1), *res;
  Oid collid = PG_GET_COLLATION();
  ty_tochar_locale_t loc = {&collid, dch_tm_name};
  int rc;

  if (VARSIZE_ANY_EXHDR(fmt) <= 0 || TIMESTAMP_NOT_FINITE(dt))
    PG_RETURN_NULL();
  rc = ty_timestamp_to_char(dt, dch_format(fcinfo, fmt), &loc, TYVO(&res));
  return dch_result(fcinfo, rc, res);
}

/* TIMESTAMPTZ to_char(), in the session's time zone */
Datum timestamptz_to_char(PG_FUNCTION_ARGS) {
  TimestampTz dt = PG_GETARG_TIMESTAMP(0);
  text *fmt = PG_GETARG_TEXT_PP(1), *res;
  Oid collid = PG_GET_COLLATION();
  ty_tochar_locale_t loc = {&collid, dch_tm_name};
  int rc;

  if (VARSIZE_ANY_EXHDR(fmt) <= 0 || TIMESTAMP_NOT_FINITE(dt))
    PG_RETURN_NULL();
  rc = ty_timestamptz_to_char(dt, session_timezone, dch_format(fcinfo, fmt),
                              &loc, TYVO(&res));
  return dch_result(fcinfo, rc, res);
}

/* -------------------
 * INTERVAL to_char()
 * -------------------
 */
Datum interval_to_char(PG_FUNCTION_ARGS) {
  Interval *it = PG_GETARG_INTERVAL_P(0);
  text *fmt = PG_GETARG_TEXT_PP(1), *res;
  Oid collid = PG_GET_COLLATION();
  ty_tochar_locale_t loc = {&collid, dch_tm_name};
  int rc;

  if (VARSIZE_ANY_EXHDR(fmt) <= 0 || INTERVAL_NOT_FINITE(it))
    PG_RETURN_NULL();
  rc = ty_interval_to_char((const ty_interval_t *)it, dch_format(fcinfo, fmt),
                           &loc, TYVO(&res));
  return dch_result(fcinfo, rc, res);
}

/* ---------------------
 * TO_TIMESTAMP()
 *
 * Make Timestamp from date_str which is formatted at argument 'fmt'
 * ( to_timestamp is reverse to_char() )
 * ---------------------
 */
Datum to_timestamp(PG_FUNCTION_ARGS) {
  text *date_txt = PG_GETARG_TEXT_PP(0);
  text *fmt = PG_GETARG_TEXT_PP(1);
  Oid collid = PG_GET_COLLATION();
  Timestamp result;
  ty_dch_parsed_t parsed;

  do_to_timestamp(date_txt, fmt, collid, false, &parsed, NULL, NULL);

  /* ty: the text's zone, else the session's; then FFn's precision */
  if (ty_dch_to_timestamp(&parsed, session_timezone, &result))
    ereport(ERROR, (errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
                    errmsg("timestamp out of range")));

  PG_RETURN_TIMESTAMP(result);
}

/* ----------
 * TO_DATE
 *	Make Date from date_str which is formatted at argument 'fmt'
 * ----------
 */
Datum to_date(PG_FUNCTION_ARGS) {
  text *date_txt = PG_GETARG_TEXT_PP(0);
  text *fmt = PG_GETARG_TEXT_PP(1);
  Oid collid = PG_GET_COLLATION();
  int32 result;
  ty_dch_parsed_t parsed;

  do_to_timestamp(date_txt, fmt, collid, false, &parsed, NULL, NULL);

  /* ty checks the Julian range and a date's */
  if (ty_dch_to_date(&parsed, &result))
    ereport(ERROR,
            (errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
             errmsg("date out of range: \"%s\"", text_to_cstring(date_txt))));

  PG_RETURN_DATEADT(result);
}

/* The fields ty parsed, as a pg_tm. */
static void dch_parsed_tm(const ty_dch_parsed_t *parsed, struct pg_tm *tm) {
  memset(tm, 0, sizeof(*tm));
  tm->tm_year = parsed->tm.year;
  tm->tm_mon = parsed->tm.mon;
  tm->tm_mday = parsed->tm.mday;
  tm->tm_hour = parsed->tm.hour;
  tm->tm_min = parsed->tm.min;
  tm->tm_sec = parsed->tm.sec;
}

/*
 * Convert the 'date_txt' input to a datetime type using argument 'fmt'
 * as a format string.  The collation 'collid' may be used for case-folding
 * rules in some cases.  'strict' specifies standard parsing mode.
 *
 * The actual data type (returned in 'typid', 'typmod') is determined by
 * the presence of date/time/zone components in the format string.
 *
 * When a timezone component is present, the corresponding offset is
 * returned in '*tz'.
 *
 * If escontext points to an ErrorSaveContext, data errors will be reported
 * by filling that struct; the caller must test SOFT_ERROR_OCCURRED() to see
 * whether an error occurred.  Otherwise, errors are thrown.
 */
Datum parse_datetime(text *date_txt, text *fmt, Oid collid, bool strict,
                     Oid *typid, int32 *typmod, int *tz, Node *escontext) {
  struct pg_tm tm;
  ty_dch_parsed_t parsed;
  fsec_t fsec;
  int fprec;
  uint32 flags;

  if (!do_to_timestamp(date_txt, fmt, collid, strict, &parsed, &flags,
                       escontext))
    return (Datum)0;
  dch_parsed_tm(&parsed, &tm);
  fsec = parsed.fsec;
  fprec = parsed.fprec;

  *typmod = fprec ? fprec : -1; /* fractional part precision */

  if (flags & TY_DCH_DATED) {
    if (flags & TY_DCH_TIMED) {
      if (flags & TY_DCH_ZONED) {
        TimestampTz result;

        if (parsed.has_tz) {
          *tz = parsed.gmtoffset;
        } else {
          /*
           * Time zone is present in format string, but not in input
           * string.  Assuming do_to_timestamp() triggers no error
           * this should be possible only in non-strict case.
           */
          Assert(!strict);

          ereturn(
              escontext, (Datum)0,
              (errcode(ERRCODE_INVALID_DATETIME_FORMAT),
               errmsg(
                   "missing time zone in input string for type timestamptz")));
        }

        if (tm2timestamp(&tm, fsec, tz, &result) != 0)
          ereturn(escontext, (Datum)0,
                  (errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
                   errmsg("timestamptz out of range")));

        AdjustTimestampForTypmod(&result, *typmod, escontext);

        *typid = TIMESTAMPTZOID;
        return TimestampTzGetDatum(result);
      } else {
        Timestamp result;

        if (tm2timestamp(&tm, fsec, NULL, &result) != 0)
          ereturn(escontext, (Datum)0,
                  (errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
                   errmsg("timestamp out of range")));

        AdjustTimestampForTypmod(&result, *typmod, escontext);

        *typid = TIMESTAMPOID;
        return TimestampGetDatum(result);
      }
    } else {
      if (flags & TY_DCH_ZONED) {
        ereturn(escontext, (Datum)0,
                (errcode(ERRCODE_INVALID_DATETIME_FORMAT),
                 errmsg("datetime format is zoned but not timed")));
      } else {
        DateADT result;

        /* Prevent overflow in Julian-day routines */
        if (!IS_VALID_JULIAN(tm.tm_year, tm.tm_mon, tm.tm_mday))
          ereturn(
              escontext, (Datum)0,
              (errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
               errmsg("date out of range: \"%s\"", text_to_cstring(date_txt))));

        result =
            date2j(tm.tm_year, tm.tm_mon, tm.tm_mday) - POSTGRES_EPOCH_JDATE;

        /* Now check for just-out-of-range dates */
        if (!IS_VALID_DATE(result))
          ereturn(
              escontext, (Datum)0,
              (errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
               errmsg("date out of range: \"%s\"", text_to_cstring(date_txt))));

        *typid = DATEOID;
        return DateADTGetDatum(result);
      }
    }
  } else if (flags & TY_DCH_TIMED) {
    if (flags & TY_DCH_ZONED) {
      TimeTzADT *result = (TimeTzADT *)palloc(sizeof(TimeTzADT));

      if (parsed.has_tz) {
        *tz = parsed.gmtoffset;
      } else {
        /*
         * Time zone is present in format string, but not in input
         * string.  Assuming do_to_timestamp() triggers no error this
         * should be possible only in non-strict case.
         */
        Assert(!strict);

        ereturn(escontext, (Datum)0,
                (errcode(ERRCODE_INVALID_DATETIME_FORMAT),
                 errmsg("missing time zone in input string for type timetz")));
      }

      if (tm2timetz(&tm, fsec, *tz, result) != 0)
        ereturn(escontext, (Datum)0,
                (errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
                 errmsg("timetz out of range")));

      AdjustTimeForTypmod(&result->time, *typmod);

      *typid = TIMETZOID;
      return TimeTzADTPGetDatum(result);
    } else {
      TimeADT result;

      if (tm2time(&tm, fsec, &result) != 0)
        ereturn(escontext, (Datum)0,
                (errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
                 errmsg("time out of range")));

      AdjustTimeForTypmod(&result, *typmod);

      *typid = TIMEOID;
      return TimeADTGetDatum(result);
    }
  } else {
    ereturn(escontext, (Datum)0,
            (errcode(ERRCODE_INVALID_DATETIME_FORMAT),
             errmsg("datetime format is not dated and not timed")));
  }
}

/*
 * The compiled date format of fmt_str: the session's cache's, or one
 * compiled in the current context for a format too long for it, which
 * *incache false says the caller frees.
 */
static const ty_dch_format_t *dch_fetch(const char *fmt_str, int fmt_len,
                                        bool std, bool *incache) {
  if (fmt_len > DCH_CACHE_SIZE) {
    *incache = false;
    return dch_compile_or_raise(fmt_str, fmt_len, std, CurrentMemoryContext);
  }
  *incache = true;
  return DCH_cache_fetch(fmt_str, std)->format;
}

/*
 * Parses the datetime format string in 'fmt_str' and returns true if it
 * contains a timezone specifier, false if not.
 */
bool datetime_format_has_tz(const char *fmt_str) {
  bool incache;
  const ty_dch_format_t *format =
      dch_fetch(fmt_str, strlen(fmt_str), false, &incache);
  unsigned flags = ty_dch_flags(format);

  if (!incache)
    ty_free((void *)format);
  return 0 != (flags & TY_DCH_ZONED);
}

/*
 * TM's localized names, for ty: the name of lc_time that the text at s
 * begins with, folded by the collation ctx points to.
 */
static int dch_match_name(void *ctx, int kind, const char *s, int *index,
                          size_t *len) {
  Oid collid = *(Oid *)ctx;
  char **array;

  /* cache localized days and months */
  cache_locale_time();
  array = TY_TOCHAR_FULL_MONTH == kind     ? localized_full_months
          : TY_TOCHAR_ABBREV_MONTH == kind ? localized_abbrev_months
          : TY_TOCHAR_FULL_DAY == kind     ? localized_full_days
                                           : localized_abbrev_days;
  *index = seq_search_localized(s, array, len, collid);
  return *index < 0 ? -1 : 0;
}

/* TZ's abbreviations, for ty: those of the session's table. */
static int dch_tz_abbrev(void *ctx, const char *s, int *gmtoff,
                         const ty_tz_t **tz) {
  pg_tz *tzp = NULL;
  int len = DecodeTimezoneAbbrevPrefix(s, gmtoff, &tzp);

  *tz = tzp;
  return len;
}

/*
 * Raise the error of a reverse parse, or save it in escontext: ty's message,
 * detail and hint with PostgreSQL's code, or DateTimeParseError's.
 */
static void dch_parse_raise(const ty_dch_error_t *err, const char *date_str,
                            Node *escontext) {
  int code = TY_DCH_E_RANGE == err->code   ? ERRCODE_DATETIME_VALUE_OUT_OF_RANGE
             : TY_DCH_E_FIELD == err->code ? ERRCODE_DATETIME_FIELD_OVERFLOW
                                           : ERRCODE_INVALID_DATETIME_FORMAT;

  if (TY_ERR_NOMEM == ty_errnum())
    ty_raise_alloc();
  if (TY_DCH_E_DTERR == err->code) {
    DateTimeParseError(err->dterr, NULL, date_str, "timestamp", escontext);
    return;
  }
  errsave(escontext,
          (errcode(code), errmsg("%s", ty_errmsg()),
           err->detail[0] ? errdetail("%s", err->detail) : 0,
           err->hint[0] ? errhint("%s", err->hint) : 0));
}

/*
 * do_to_timestamp: shared code for to_timestamp and to_date
 *
 * Parse the 'date_txt' according to 'fmt' into ty's fields: the date and
 * time, the zone, and the fractional precision. ty reads the text field by
 * field and makes the fields a date and time (ty_dch_parse); digres gives
 * the format, the localized names and the zone abbreviations.
 *
 * 'collid' identifies the collation to use, if needed.
 * 'std' specifies standard parsing mode.
 *
 * Bit mask of date/time/zone components found in 'fmt' is returned in 'flags',
 * if that is not NULL.
 *
 * Returns true on success, false on failure (if escontext points to an
 * ErrorSaveContext; otherwise errors are thrown).  Note that currently,
 * soft-error behavior is provided for bad data but not bad format.
 */
static bool do_to_timestamp(text *date_txt, text *fmt, Oid collid, bool std,
                            ty_dch_parsed_t *parsed, uint32 *flags,
                            Node *escontext) {
  char *date_str = text_to_cstring(date_txt);
  char *fmt_str = text_to_cstring(fmt);
  int fmt_len = VARSIZE_ANY_EXHDR(fmt);
  ty_dch_parse_host_t host = {&collid, dch_match_name, dch_tz_abbrev};
  ty_dch_error_t err;
  bool incache;
  const ty_dch_format_t *format = dch_fetch(fmt_str, fmt_len, std, &incache);
  int rc = ty_dch_parse(format, date_str, std, &host, parsed, &err);

  if (flags)
    *flags = 0 == rc ? ty_dch_flags(format) : 0;
  if (!incache)
    ty_free((void *)format);
  pfree(fmt_str);
  if (rc) {
    dch_parse_raise(&err, date_str, escontext);
    pfree(date_str);
    return false;
  }
  pfree(date_str);
  return true;
}

/**********************************************************************
 *	the NUMBER version part: ty compiles, writes and reads a number format
 *	(ty_tochar.h)
 *********************************************************************/

/*
 * The compiled number format of the call site, or PostgreSQL's syntax error
 * with its detail.
 */
static int num_compile(const char *fmt, size_t len, void **out) {
  return ty_num_compile(fmt, len, (ty_num_format_t **)out);
}

/* The compiled number format of a to_char call site. */
static const ty_num_format_t *num_format(FunctionCallInfo fcinfo, text *fmt) {
  void *f = tochar_format(fcinfo, fmt, num_compile);
  const char *msg;

  if (NULL != f)
    return (const ty_num_format_t *)f;
  if (TY_ERR_INVALID != ty_errnum())
    ty_raise_alloc();
  msg = ty_errmsg();
  ereport(ERROR,
          (errcode(ERRCODE_SYNTAX_ERROR), errmsg("%s", msg),
           0 == strcmp(msg, "\"EEEE\" is incompatible with other formats")
               ? errdetail("\"EEEE\" may only be used together with digit "
                           "and decimal point patterns.")
           : 0 == strcmp(msg, "\"RN\" is incompatible with other formats")
               ? errdetail("\"RN\" may only be used together with \"FM\".")
               : 0));
}

/* The lconv symbols of the session, for ty's D, G, L and S. */
static void num_symbols(void *ctx, ty_num_symbols_t *out) {
  struct lconv *lconv = PGLC_localeconv();

  out->negative_sign = lconv->negative_sign;
  out->positive_sign = lconv->positive_sign;
  out->decimal_point = lconv->decimal_point;
  out->thousands_sep = lconv->thousands_sep;
  out->currency_symbol = lconv->currency_symbol;
}

/*
 * Whether a format gives an empty text without being read, as PostgreSQL's
 * NUM_TOCHAR_prepare: an empty one, or one too long for its result.
 */
static bool num_format_empty(text *fmt) {
  int len = VARSIZE_ANY_EXHDR(fmt);

  return len <= 0 || len >= (INT_MAX - VARHDRSZ) / NUM_MAX_ITEM_SIZ;
}

/* Finish a number's to_char by ty: the text, or its error. */
static Datum num_result(int rc, text *res) {
  if (0 == rc)
    PG_RETURN_TEXT_P(res);
  if (TY_ERR_RANGE == ty_errnum())
    ereport(ERROR, (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
                    errmsg("%s", ty_errmsg())));
  if (TY_ERR_INVALID == ty_errnum())
    ereport(ERROR, (errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
                    errmsg("%s", ty_errmsg())));
  ty_raise_alloc();
}

/* -------------------
 * NUMERIC to_number() (convert string to numeric)
 * -------------------
 */
Datum numeric_to_number(PG_FUNCTION_ARGS) {
  text *value = PG_GETARG_TEXT_PP(0);
  text *fmt = PG_GETARG_TEXT_PP(1);
  const ty_num_format_t *format;
  ty_num_desc_t num;
  ty_num_locale_t loc = {NULL, num_symbols};
  Datum result;
  char *numstr;
  int len = VARSIZE_ANY_EXHDR(fmt);
  int scale, precision;

  if (len <= 0 || len >= INT_MAX / NUM_MAX_ITEM_SIZ)
    PG_RETURN_NULL();

  format = num_format(fcinfo, fmt);
  ty_num_describe(format, &num);
  numstr = (char *)palloc((len * NUM_MAX_ITEM_SIZ) + 2);

  /* ty reads the text into the digits numeric_in reads */
  if (ty_num_from_char(format, VARDATA_ANY(value), VARSIZE_ANY_EXHDR(value),
                       &loc, numstr, &precision, &scale)) {
    if (0 == strcmp(ty_errmsg(), "\"EEEE\" not supported for input"))
      ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                      errmsg("%s", ty_errmsg())));
    ereport(ERROR, (errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
                    errmsg("%s", ty_errmsg())));
  }

  result = DirectFunctionCall3(
      numeric_in, CStringGetDatum(numstr), ObjectIdGetDatum(InvalidOid),
      Int32GetDatum(((precision << 16) | scale) + VARHDRSZ));

  /* V, even with no 9 after it: 10^0 still sets the scale */
  if (num.v) {
    Numeric x;
    Numeric a = int64_to_numeric(10);
    Numeric b = int64_to_numeric(-num.multi);

    x = DatumGetNumeric(DirectFunctionCall2(numeric_power, NumericGetDatum(a),
                                            NumericGetDatum(b)));
    result = DirectFunctionCall2(numeric_mul, result, NumericGetDatum(x));
  }

  pfree(numstr);
  return result;
}

/* ------------------
 * NUMERIC to_char()
 * ------------------
 */
Datum numeric_to_char(PG_FUNCTION_ARGS) {
  Numeric value = PG_GETARG_NUMERIC(0);
  text *fmt = PG_GETARG_TEXT_PP(1), *result;
  const ty_num_format_t *format;
  ty_num_desc_t num;
  ty_num_locale_t loc = {NULL, num_symbols};
  int out_pre_spaces = 0, sign = 0, rc;
  char *numstr, *orgnum, *p, roman[16];

  if (num_format_empty(fmt))
    PG_RETURN_TEXT_P(cstring_to_text(""));
  format = num_format(fcinfo, fmt);
  ty_num_describe(format, &num);

  /*
   * On DateType depend part (numeric): digres makes the digits of its
   * decimal, and ty writes them by the format
   */
  if (num.roman) {
    int32 intvalue;
    bool err;

    /* Round and convert to int */
    intvalue = numeric_int4_opt_error(value, &err);
    /* On overflow, just use PG_INT32_MAX; ty_num_roman will cope */
    if (err)
      intvalue = PG_INT32_MAX;
    numstr = (char *)ty_num_roman(intvalue, roman);
  } else if (num.eeee) {
    orgnum = numeric_out_sci(value, num.post);

    /*
     * numeric_out_sci() does not emit a sign for positive numbers.  We
     * need to add a space in this case so that positive and negative
     * numbers are aligned.  Also must check for NaN/infinity cases, which
     * we handle the same way as in float8_to_char.
     */
    if (strcmp(orgnum, "NaN") == 0 || strcmp(orgnum, "Infinity") == 0 ||
        strcmp(orgnum, "-Infinity") == 0) {
      /*
       * Allow 6 characters for the leading sign, the decimal point,
       * "e", the exponent's sign and two exponent digits.
       */
      numstr = (char *)palloc(num.pre + num.post + 7);
      memset(numstr, '#', num.pre + num.post + 6);
      numstr[num.pre + num.post + 6] = '\0';
      *numstr = ' ';
      *(numstr + num.pre + 1) = '.';
    } else if (*orgnum != '-') {
      numstr = (char *)palloc(strlen(orgnum) + 2);
      *numstr = ' ';
      strcpy(numstr + 1, orgnum);
    } else {
      numstr = orgnum;
    }
  } else {
    int numstr_pre_len;
    Numeric val = value;
    Numeric x;

    if (num.v) {
      Numeric a = int64_to_numeric(10);
      Numeric b = int64_to_numeric(num.multi);

      x = DatumGetNumeric(DirectFunctionCall2(numeric_power, NumericGetDatum(a),
                                              NumericGetDatum(b)));
      val = DatumGetNumeric(DirectFunctionCall2(
          numeric_mul, NumericGetDatum(value), NumericGetDatum(x)));
      num.pre += num.multi;
    }

    x = DatumGetNumeric(DirectFunctionCall2(numeric_round, NumericGetDatum(val),
                                            Int32GetDatum(num.post)));
    orgnum =
        DatumGetCString(DirectFunctionCall1(numeric_out, NumericGetDatum(x)));

    if (*orgnum == '-') {
      sign = '-';
      numstr = orgnum + 1;
    } else {
      sign = '+';
      numstr = orgnum;
    }

    if ((p = strchr(numstr, '.')))
      numstr_pre_len = p - numstr;
    else
      numstr_pre_len = strlen(numstr);

    /* needs padding? */
    if (numstr_pre_len < num.pre)
      out_pre_spaces = num.pre - numstr_pre_len;
    /* overflowed prefix digit format? */
    else if (numstr_pre_len > num.pre) {
      numstr = (char *)palloc(num.pre + num.post + 2);
      memset(numstr, '#', num.pre + num.post + 1);
      numstr[num.pre + num.post + 1] = '\0';
      *(numstr + num.pre) = '.';
    }
  }

  rc = ty_num_to_char_digits(format, numstr, out_pre_spaces, sign, num.pre,
                             num.post, &loc, TYVO(&result));
  return num_result(rc, result);
}

/* ---------------
 * INT4 to_char()
 * ---------------
 */
Datum int4_to_char(PG_FUNCTION_ARGS) {
  int32 value = PG_GETARG_INT32(0);
  text *fmt = PG_GETARG_TEXT_PP(1), *result;
  ty_num_locale_t loc = {NULL, num_symbols};
  int rc;

  if (num_format_empty(fmt))
    PG_RETURN_TEXT_P(cstring_to_text(""));
  /* ty makes the digits and writes them by the format */
  rc = ty_int32_to_char(value, num_format(fcinfo, fmt), &loc, TYVO(&result));
  return num_result(rc, result);
}

/* ---------------
 * INT8 to_char()
 * ---------------
 */
Datum int8_to_char(PG_FUNCTION_ARGS) {
  int64 value = PG_GETARG_INT64(0);
  text *fmt = PG_GETARG_TEXT_PP(1), *result;
  ty_num_locale_t loc = {NULL, num_symbols};
  int rc;

  if (num_format_empty(fmt))
    PG_RETURN_TEXT_P(cstring_to_text(""));
  /* ty makes the digits and writes them by the format */
  rc = ty_int64_to_char(value, num_format(fcinfo, fmt), &loc, TYVO(&result));
  return num_result(rc, result);
}

/* -----------------
 * FLOAT4 to_char()
 * -----------------
 */
Datum float4_to_char(PG_FUNCTION_ARGS) {
  float4 value = PG_GETARG_FLOAT4(0);
  text *fmt = PG_GETARG_TEXT_PP(1), *result;
  ty_num_locale_t loc = {NULL, num_symbols};
  int rc;

  if (num_format_empty(fmt))
    PG_RETURN_TEXT_P(cstring_to_text(""));
  /* ty makes the digits and writes them by the format */
  rc = ty_float4_to_char(value, num_format(fcinfo, fmt), &loc, TYVO(&result));
  return num_result(rc, result);
}

/* -----------------
 * FLOAT8 to_char()
 * -----------------
 */
Datum float8_to_char(PG_FUNCTION_ARGS) {
  float8 value = PG_GETARG_FLOAT8(0);
  text *fmt = PG_GETARG_TEXT_PP(1), *result;
  ty_num_locale_t loc = {NULL, num_symbols};
  int rc;

  if (num_format_empty(fmt))
    PG_RETURN_TEXT_P(cstring_to_text(""));
  /* ty makes the digits and writes them by the format */
  rc = ty_float8_to_char(value, num_format(fcinfo, fmt), &loc, TYVO(&result));
  return num_result(rc, result);
}

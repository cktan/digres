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
 * Routines flags
 * ----------
 */
#define DCH_FLAG		0x1 /* DATE-TIME flag	*/
#define NUM_FLAG		0x2 /* NUMBER flag	*/
#define STD_FLAG		0x4 /* STANDARD flag	*/

/* ----------
 * KeyWord Index (ascii from position 32 (' ') to 126 (~))
 * ----------
 */
#define KeyWord_INDEX_SIZE		('~' - ' ')
#define KeyWord_INDEX_FILTER(_c)	((_c) <= ' ' || (_c) >= '~' ? 0 : 1)

/* ----------
 * Maximal length of one node
 * ----------
 */
#define NUM_MAX_ITEM_SIZ		8  /* roman number (RN has 15 chars)	*/

#define MAX_L10N_DATA			80 /* max localized day or month name */

/* ----------
 * Format parser structs
 * ----------
 */
typedef struct {
  const char *name; /* suffix string		*/
  int len,          /* suffix length		*/
      id,           /* used in node->suffix */
      type;         /* prefix / postfix		*/
} KeySuffix;

/* ----------
 * FromCharDateMode
 * ----------
 *
 * This value is used to nominate one of several distinct (and mutually
 * exclusive) date conventions that a keyword can belong to.
 */
typedef enum {
  FROM_CHAR_DATE_NONE = 0,  /* Value does not affect date mode. */
  FROM_CHAR_DATE_GREGORIAN, /* Gregorian (day, month, year) style date */
  FROM_CHAR_DATE_ISOWEEK,   /* ISO 8601 week date */
} FromCharDateMode;

typedef struct {
  const char *name;
  int len;
  int id;
  bool is_digit;
  FromCharDateMode date_mode;
} KeyWord;

typedef struct {
  uint8 type;                                 /* NODE_TYPE_XXX, see below */
  char character[MAX_MULTIBYTE_CHAR_LEN + 1]; /* if type is CHAR */
  uint8 suffix;       /* keyword prefix/suffix code, if any */
  const KeyWord *key; /* if type is ACTION */
} FormatNode;

#define NODE_TYPE_END		1
#define NODE_TYPE_ACTION	2
#define NODE_TYPE_CHAR		3
#define NODE_TYPE_SEPARATOR	4
#define NODE_TYPE_SPACE		5

#define SUFFTYPE_PREFIX		1
#define SUFFTYPE_POSTFIX	2

/* ----------
 * Roman numerals: ty writes them (ty_num_roman); to_number reads them
 * ----------
 */
/*
 * MACRO: Check if the current and next characters form a valid subtraction
 * combination for roman numerals.
 */
#define IS_VALID_SUB_COMB(curr, next) \
	(((curr) == 'I' && ((next) == 'V' || (next) == 'X')) || \
	 ((curr) == 'X' && ((next) == 'L' || (next) == 'C')) || \
	 ((curr) == 'C' && ((next) == 'D' || (next) == 'M')))

/*
 * MACRO: Roman numeral value, or 0 if character isn't a roman numeral.
 */
#define ROMAN_VAL(r) \
	((r) == 'I' ? 1 : \
	 (r) == 'V' ? 5 : \
	 (r) == 'X' ? 10 : \
	 (r) == 'L' ? 50 : \
	 (r) == 'C' ? 100 : \
	 (r) == 'D' ? 500 : \
	 (r) == 'M' ? 1000 : 0)

/*
 * 'MMMDCCCLXXXVIII' (3888) is the longest valid roman numeral (15 characters).
 */
#define MAX_ROMAN_LEN	15

/* ----------
 * Ordinal postfixes
 * ----------
 */
static const char *const numTH[] = {"ST", "ND", "RD", "TH", NULL};
static const char *const numth[] = {"st", "nd", "rd", "th", NULL};

/* ----------
 * Flags & Options:
 * ----------
 */
#define TH_UPPER		1
#define TH_LOWER		2

/* ----------
 * Number description struct
 * ----------
 */
typedef struct {
  int pre,           /* (count) numbers before decimal */
      post,          /* (count) numbers after decimal  */
      lsign,         /* want locales sign		  */
      flag,          /* number parameters		  */
      pre_lsign_num, /* tmp value for lsign		  */
      multi,         /* multiplier for 'V'		  */
      zero_start,    /* position of first zero	  */
      zero_end,      /* position of last zero	  */
      need_locale;   /* needs it locale		  */
} NUMDesc;

/* ----------
 * Flags for NUMBER version
 * ----------
 */
#define NUM_F_DECIMAL		(1 << 1)
#define NUM_F_LDECIMAL		(1 << 2)
#define NUM_F_ZERO			(1 << 3)
#define NUM_F_BLANK			(1 << 4)
#define NUM_F_FILLMODE		(1 << 5)
#define NUM_F_LSIGN			(1 << 6)
#define NUM_F_BRACKET		(1 << 7)
#define NUM_F_MINUS			(1 << 8)
#define NUM_F_PLUS			(1 << 9)
#define NUM_F_ROMAN			(1 << 10)
#define NUM_F_MULTI			(1 << 11)
#define NUM_F_PLUS_POST		(1 << 12)
#define NUM_F_MINUS_POST	(1 << 13)
#define NUM_F_EEEE			(1 << 14)

#define NUM_LSIGN_PRE	(-1)
#define NUM_LSIGN_POST	1
#define NUM_LSIGN_NONE	0

/* ----------
 * Tests
 * ----------
 */
#define IS_DECIMAL(_f)	((_f)->flag & NUM_F_DECIMAL)
#define IS_LDECIMAL(_f) ((_f)->flag & NUM_F_LDECIMAL)
#define IS_ZERO(_f) ((_f)->flag & NUM_F_ZERO)
#define IS_BLANK(_f)	((_f)->flag & NUM_F_BLANK)
#define IS_FILLMODE(_f) ((_f)->flag & NUM_F_FILLMODE)
#define IS_BRACKET(_f)	((_f)->flag & NUM_F_BRACKET)
#define IS_MINUS(_f)	((_f)->flag & NUM_F_MINUS)
#define IS_LSIGN(_f)	((_f)->flag & NUM_F_LSIGN)
#define IS_PLUS(_f) ((_f)->flag & NUM_F_PLUS)
#define IS_ROMAN(_f)	((_f)->flag & NUM_F_ROMAN)
#define IS_MULTI(_f)	((_f)->flag & NUM_F_MULTI)
#define IS_EEEE(_f)		((_f)->flag & NUM_F_EEEE)

/* ----------
 * Format picture cache
 *
 * We will cache datetime format pictures up to DCH_CACHE_SIZE bytes long;
 * likewise number format pictures up to NUM_CACHE_SIZE bytes long.
 *
 * For simplicity, the cache entries are fixed-size, so they allow for the
 * worst case of a FormatNode for each byte in the picture string.
 *
 * The CACHE_SIZE constants are computed to make sizeof(DCHCacheEntry) and
 * sizeof(NUMCacheEntry) be powers of 2, or just less than that, so that
 * we don't waste too much space by palloc'ing them individually.  Be sure
 * to adjust those macros if you add fields to those structs.
 *
 * The max number of entries in each cache is DCH_CACHE_ENTRIES
 * resp. NUM_CACHE_ENTRIES.
 * ----------
 */
#define DCH_CACHE_OVERHEAD \
	MAXALIGN(sizeof(bool) + sizeof(int))
#define NUM_CACHE_OVERHEAD \
	MAXALIGN(sizeof(bool) + sizeof(int) + sizeof(NUMDesc))

#define DCH_CACHE_SIZE \
	((2048 - DCH_CACHE_OVERHEAD) / (sizeof(FormatNode) + sizeof(char)) - 1)
#define NUM_CACHE_SIZE \
	((1024 - NUM_CACHE_OVERHEAD) / (sizeof(FormatNode) + sizeof(char)) - 1)

#define DCH_CACHE_ENTRIES	20
#define NUM_CACHE_ENTRIES	20

typedef struct {
  ty_dch_format_t *format; /* ty's, in TopMemoryContext */
  char str[DCH_CACHE_SIZE + 1];
  bool std;
  bool valid;
  int age;
} DCHCacheEntry;

typedef struct {
  FormatNode format[NUM_CACHE_SIZE + 1];
  char str[NUM_CACHE_SIZE + 1];
  bool valid;
  int age;
  NUMDesc Num;
} NUMCacheEntry;

/* global cache for date/time format pictures */
static thread_local /*LOCAL*/ DCHCacheEntry *DCHCache[DCH_CACHE_ENTRIES];
static thread_local /*LOCAL*/ int n_DCHCache =
    0; /* current number of entries */
static thread_local /*LOCAL*/ int DCHCounter = 0; /* aging-event counter */

/* global cache for number format pictures */
static thread_local /*LOCAL*/ NUMCacheEntry *NUMCache[NUM_CACHE_ENTRIES];
static thread_local /*LOCAL*/ int n_NUMCache =
    0; /* current number of entries */
static thread_local /*LOCAL*/ int NUMCounter = 0; /* aging-event counter */

/*****************************************************************************
 *			KeyWord definitions
 *****************************************************************************/

/* ----------
 * Format-pictures (KeyWord).
 *
 * The KeyWord field; alphabetic sorted, *BUT* strings alike is sorted
 *		  complicated -to-> easy:
 *
 *	(example: "DDD","DD","Day","D" )
 *
 * (this specific sort needs the algorithm for sequential search for strings,
 * which not has exact end; -> How keyword is in "HH12blabla" ? - "HH"
 * or "HH12"? You must first try "HH12", because "HH" is in string, but
 * it is not good.
 *
 * (!)
 *	 - Position for the keyword is similar as position in the enum
 *DCH/NUM_poz.
 * (!)
 *
 * For fast search is used the 'int index[]', index is ascii table from position
 * 32 (' ') to 126 (~), in this index is DCH_ / NUM_ enums for each ASCII
 * position or -1 if char is not used in the KeyWord. Search example for
 * string "MM":
 *	1)	see in index to index['M' - 32],
 *	2)	take keywords position (enum DCH_MI) from index
 *	3)	run sequential search in keywords[] from this position
 *
 * ----------
 */

typedef enum {
  NUM_COMMA,
  NUM_DEC,
  NUM_0,
  NUM_9,
  NUM_B,
  NUM_C,
  NUM_D,
  NUM_E,
  NUM_FM,
  NUM_G,
  NUM_L,
  NUM_MI,
  NUM_PL,
  NUM_PR,
  NUM_RN,
  NUM_SG,
  NUM_SP,
  NUM_S,
  NUM_TH,
  NUM_V,
  NUM_b,
  NUM_c,
  NUM_d,
  NUM_e,
  NUM_fm,
  NUM_g,
  NUM_l,
  NUM_mi,
  NUM_pl,
  NUM_pr,
  NUM_rn,
  NUM_sg,
  NUM_sp,
  NUM_s,
  NUM_th,
  NUM_v,

  /* last */
  _NUM_last_
} NUM_poz;

/* ----------
 * KeyWords for NUMBER version
 *
 * The is_digit and date_mode fields are not relevant here.
 * ----------
 */
static const KeyWord NUM_keywords[] = {
    /*	name, len, id			is in Index */
    {",", 1, NUM_COMMA}, /* , */
    {".", 1, NUM_DEC},   /* . */
    {"0", 1, NUM_0},     /* 0 */
    {"9", 1, NUM_9},     /* 9 */
    {"B", 1, NUM_B},     /* B */
    {"C", 1, NUM_C},     /* C */
    {"D", 1, NUM_D},     /* D */
    {"EEEE", 4, NUM_E},  /* E */
    {"FM", 2, NUM_FM},   /* F */
    {"G", 1, NUM_G},     /* G */
    {"L", 1, NUM_L},     /* L */
    {"MI", 2, NUM_MI},   /* M */
    {"PL", 2, NUM_PL},   /* P */
    {"PR", 2, NUM_PR},
    {"RN", 2, NUM_RN}, /* R */
    {"SG", 2, NUM_SG}, /* S */
    {"SP", 2, NUM_SP},
    {"S", 1, NUM_S},
    {"TH", 2, NUM_TH},  /* T */
    {"V", 1, NUM_V},    /* V */
    {"b", 1, NUM_B},    /* b */
    {"c", 1, NUM_C},    /* c */
    {"d", 1, NUM_D},    /* d */
    {"eeee", 4, NUM_E}, /* e */
    {"fm", 2, NUM_FM},  /* f */
    {"g", 1, NUM_G},    /* g */
    {"l", 1, NUM_L},    /* l */
    {"mi", 2, NUM_MI},  /* m */
    {"pl", 2, NUM_PL},  /* p */
    {"pr", 2, NUM_PR},
    {"rn", 2, NUM_rn}, /* r */
    {"sg", 2, NUM_SG}, /* s */
    {"sp", 2, NUM_SP},
    {"s", 1, NUM_S},
    {"th", 2, NUM_th}, /* t */
    {"v", 1, NUM_V},   /* v */

    /* last */
    {NULL, 0, 0}};

/* ----------
 * KeyWords index for NUMBER version
 * ----------
 */
static const int NUM_index[KeyWord_INDEX_SIZE] = {
    /*
    0	1	2	3	4	5	6	7	8	9
    */
    /*---- first 0..31 chars are skipped ----*/

    -1,     -1,     -1,     -1,        -1,    -1,      -1,     -1,     -1,
    -1,     -1,     -1,     NUM_COMMA, -1,    NUM_DEC, -1,     NUM_0,  -1,
    -1,     -1,     -1,     -1,        -1,    -1,      -1,     NUM_9,  -1,
    -1,     -1,     -1,     -1,        -1,    -1,      -1,     NUM_B,  NUM_C,
    NUM_D,  NUM_E,  NUM_FM, NUM_G,     -1,    -1,      -1,     -1,     NUM_L,
    NUM_MI, -1,     -1,     NUM_PL,    -1,    NUM_RN,  NUM_SG, NUM_TH, -1,
    NUM_V,  -1,     -1,     -1,        -1,    -1,      -1,     -1,     -1,
    -1,     -1,     -1,     NUM_b,     NUM_c, NUM_d,   NUM_e,  NUM_fm, NUM_g,
    -1,     -1,     -1,     -1,        NUM_l, NUM_mi,  -1,     -1,     NUM_pl,
    -1,     NUM_rn, NUM_sg, NUM_th,    -1,    NUM_v,   -1,     -1,     -1,
    -1,     -1,     -1,     -1

    /*---- chars over 126 are skipped ----*/
};

/* ----------
 * Number processor struct
 * ----------
 */
typedef struct NUMProc {
  bool is_to_char;
  NUMDesc *Num; /* number description		*/

  int sign,           /* '-' or '+'			*/
      sign_wrote,     /* was sign write		*/
      num_count,      /* number of write digits	*/
      num_in,         /* is inside number		*/
      num_curr,       /* current position in number	*/
      out_pre_spaces, /* spaces before first digit	*/

      read_dec,  /* to_number - was read dec. point	*/
      read_post, /* to_number - number of dec. digit */
      read_pre;  /* to_number - number non-dec. digit */

  char *number,  /* string with number	*/
      *number_p, /* pointer to current number position */
      *inout,    /* in / out buffer	*/
      *inout_p;  /* pointer to current inout position */

  const char *last_relevant, /* last relevant number after decimal point */

      *L_negative_sign, /* Locale */
      *L_positive_sign, *decimal, *L_thousands_sep, *L_currency_symbol;
} NUMProc;

/*
 * These macros are used in NUM_processor() and its subsidiary routines.
 * OVERLOAD_TEST: true if we've reached end of input string
 * AMOUNT_TEST(s): true if at least s bytes remain in string
 */
#define OVERLOAD_TEST	(Np->inout_p >= Np->inout + input_len)
#define AMOUNT_TEST(s)	(Np->inout_p <= Np->inout + (input_len - (s)))

/* ----------
 * Functions
 * ----------
 */
static const KeyWord *index_seq_search(const char *str, const KeyWord *kw,
                                       const int *index);
static const KeySuffix *suff_search(const char *str, const KeySuffix *suf,
                                    int type);
static bool is_separator_char(const char *str);
static void NUMDesc_prepare(NUMDesc *num, FormatNode *n);
static void parse_format(FormatNode *node, const char *str, const KeyWord *kw,
                         const KeySuffix *suf, const int *index, uint32 flags,
                         NUMDesc *Num);

#ifdef DEBUG_TO_FROM_CHAR
static void dump_index(const KeyWord *k, const int *index);
static void dump_node(FormatNode *node, int max);
#endif

static const char *get_th(char *num, int type);
static char *str_numth(char *dest, char *num, int type);
static int seq_search_localized(const char *name, char **array, size_t *len,
                                Oid collid);
static bool do_to_timestamp(text *date_txt, text *fmt, Oid collid, bool std,
                            ty_dch_parsed_t *parsed, uint32 *flags,
                            Node *escontext);
static void dch_parsed_tm(const ty_dch_parsed_t *parsed, struct pg_tm *tm);
static FormatNode *NUM_cache(int len, NUMDesc *Num, text *pars_str,
                             bool *shouldFree);
static int roman_to_int(NUMProc *Np, int input_len);
static void NUM_prepare_locale(NUMProc *Np);
static char *get_last_relevant_decnum(char *num);
static void NUM_numpart_from_char(NUMProc *Np, int id, int input_len);
static void NUM_numpart_to_char(NUMProc *Np, int id);
static void NUM_add_locale_symbol(NUMProc *Np, const char *pattern);
static char *NUM_processor(FormatNode *node, NUMDesc *Num, char *inout,
                           char *number, int input_len,
                           int to_char_out_pre_spaces, int sign,
                           bool is_to_char, Oid collid);
static DCHCacheEntry *DCH_cache_getnew(const char *str, bool std);
static DCHCacheEntry *DCH_cache_search(const char *str, bool std);
static DCHCacheEntry *DCH_cache_fetch(const char *str, bool std);
static NUMCacheEntry *NUM_cache_getnew(const char *str);
static NUMCacheEntry *NUM_cache_search(const char *str);
static NUMCacheEntry *NUM_cache_fetch(const char *str);

/* ----------
 * Fast sequential search, use index for data selection which
 * go to seq. cycle (it is very fast for unwanted strings)
 * (can't be used binary search in format parsing)
 * ----------
 */
static const KeyWord *index_seq_search(const char *str, const KeyWord *kw,
                                       const int *index) {
  int poz;

  if (!KeyWord_INDEX_FILTER(*str))
    return NULL;

  if ((poz = *(index + (*str - ' '))) > -1) {
    const KeyWord *k = kw + poz;

    do {
      if (strncmp(str, k->name, k->len) == 0)
        return k;
      k++;
      if (!k->name)
        return NULL;
    } while (*str == *k->name);
  }
  return NULL;
}

static const KeySuffix *suff_search(const char *str, const KeySuffix *suf,
                                    int type) {
  const KeySuffix *s;

  for (s = suf; s->name != NULL; s++) {
    if (s->type != type)
      continue;

    if (strncmp(str, s->name, s->len) == 0)
      return s;
  }
  return NULL;
}

static bool is_separator_char(const char *str) {
  /* ASCII printable character, but not letter or digit */
  return (*str > 0x20 && *str < 0x7F && !(*str >= 'A' && *str <= 'Z') &&
          !(*str >= 'a' && *str <= 'z') && !(*str >= '0' && *str <= '9'));
}

/* ----------
 * Prepare NUMDesc (number description struct) via FormatNode struct
 * ----------
 */
static void NUMDesc_prepare(NUMDesc *num, FormatNode *n) {
  if (n->type != NODE_TYPE_ACTION)
    return;

  if (IS_EEEE(num) && n->key->id != NUM_E)
    ereport(ERROR, (errcode(ERRCODE_SYNTAX_ERROR),
                    errmsg("\"EEEE\" must be the last pattern used")));

  switch (n->key->id) {
  case NUM_9:
    if (IS_BRACKET(num))
      ereport(ERROR, (errcode(ERRCODE_SYNTAX_ERROR),
                      errmsg("\"9\" must be ahead of \"PR\"")));
    if (IS_MULTI(num)) {
      ++num->multi;
      break;
    }
    if (IS_DECIMAL(num))
      ++num->post;
    else
      ++num->pre;
    break;

  case NUM_0:
    if (IS_BRACKET(num))
      ereport(ERROR, (errcode(ERRCODE_SYNTAX_ERROR),
                      errmsg("\"0\" must be ahead of \"PR\"")));
    if (!IS_ZERO(num) && !IS_DECIMAL(num)) {
      num->flag |= NUM_F_ZERO;
      num->zero_start = num->pre + 1;
    }
    if (!IS_DECIMAL(num))
      ++num->pre;
    else
      ++num->post;

    num->zero_end = num->pre + num->post;
    break;

  case NUM_B:
    if (num->pre == 0 && num->post == 0 && (!IS_ZERO(num)))
      num->flag |= NUM_F_BLANK;
    break;

  case NUM_D:
    num->flag |= NUM_F_LDECIMAL;
    num->need_locale = true;
    /* FALLTHROUGH */
  case NUM_DEC:
    if (IS_DECIMAL(num))
      ereport(ERROR, (errcode(ERRCODE_SYNTAX_ERROR),
                      errmsg("multiple decimal points")));
    if (IS_MULTI(num))
      ereport(ERROR, (errcode(ERRCODE_SYNTAX_ERROR),
                      errmsg("cannot use \"V\" and decimal point together")));
    num->flag |= NUM_F_DECIMAL;
    break;

  case NUM_FM:
    num->flag |= NUM_F_FILLMODE;
    break;

  case NUM_S:
    if (IS_LSIGN(num))
      ereport(ERROR, (errcode(ERRCODE_SYNTAX_ERROR),
                      errmsg("cannot use \"S\" twice")));
    if (IS_PLUS(num) || IS_MINUS(num) || IS_BRACKET(num))
      ereport(
          ERROR,
          (errcode(ERRCODE_SYNTAX_ERROR),
           errmsg(
               "cannot use \"S\" and \"PL\"/\"MI\"/\"SG\"/\"PR\" together")));
    if (!IS_DECIMAL(num)) {
      num->lsign = NUM_LSIGN_PRE;
      num->pre_lsign_num = num->pre;
      num->need_locale = true;
      num->flag |= NUM_F_LSIGN;
    } else if (num->lsign == NUM_LSIGN_NONE) {
      num->lsign = NUM_LSIGN_POST;
      num->need_locale = true;
      num->flag |= NUM_F_LSIGN;
    }
    break;

  case NUM_MI:
    if (IS_LSIGN(num))
      ereport(ERROR, (errcode(ERRCODE_SYNTAX_ERROR),
                      errmsg("cannot use \"S\" and \"MI\" together")));
    num->flag |= NUM_F_MINUS;
    if (IS_DECIMAL(num))
      num->flag |= NUM_F_MINUS_POST;
    break;

  case NUM_PL:
    if (IS_LSIGN(num))
      ereport(ERROR, (errcode(ERRCODE_SYNTAX_ERROR),
                      errmsg("cannot use \"S\" and \"PL\" together")));
    num->flag |= NUM_F_PLUS;
    if (IS_DECIMAL(num))
      num->flag |= NUM_F_PLUS_POST;
    break;

  case NUM_SG:
    if (IS_LSIGN(num))
      ereport(ERROR, (errcode(ERRCODE_SYNTAX_ERROR),
                      errmsg("cannot use \"S\" and \"SG\" together")));
    num->flag |= NUM_F_MINUS;
    num->flag |= NUM_F_PLUS;
    break;

  case NUM_PR:
    if (IS_LSIGN(num) || IS_PLUS(num) || IS_MINUS(num))
      ereport(
          ERROR,
          (errcode(ERRCODE_SYNTAX_ERROR),
           errmsg(
               "cannot use \"PR\" and \"S\"/\"PL\"/\"MI\"/\"SG\" together")));
    num->flag |= NUM_F_BRACKET;
    break;

  case NUM_rn:
  case NUM_RN:
    if (IS_ROMAN(num))
      ereport(ERROR, (errcode(ERRCODE_SYNTAX_ERROR),
                      errmsg("cannot use \"RN\" twice")));
    num->flag |= NUM_F_ROMAN;
    break;

  case NUM_L:
  case NUM_G:
    num->need_locale = true;
    break;

  case NUM_V:
    if (IS_DECIMAL(num))
      ereport(ERROR, (errcode(ERRCODE_SYNTAX_ERROR),
                      errmsg("cannot use \"V\" and decimal point together")));
    num->flag |= NUM_F_MULTI;
    break;

  case NUM_E:
    if (IS_EEEE(num))
      ereport(ERROR, (errcode(ERRCODE_SYNTAX_ERROR),
                      errmsg("cannot use \"EEEE\" twice")));
    if (IS_BLANK(num) || IS_FILLMODE(num) || IS_LSIGN(num) || IS_BRACKET(num) ||
        IS_MINUS(num) || IS_PLUS(num) || IS_ROMAN(num) || IS_MULTI(num))
      ereport(ERROR, (errcode(ERRCODE_SYNTAX_ERROR),
                      errmsg("\"EEEE\" is incompatible with other formats"),
                      errdetail("\"EEEE\" may only be used together with digit "
                                "and decimal point patterns.")));
    num->flag |= NUM_F_EEEE;
    break;
  }

  if (IS_ROMAN(num) && (num->flag & ~(NUM_F_ROMAN | NUM_F_FILLMODE)) != 0)
    ereport(ERROR,
            (errcode(ERRCODE_SYNTAX_ERROR),
             errmsg("\"RN\" is incompatible with other formats"),
             errdetail("\"RN\" may only be used together with \"FM\".")));
}

/* ----------
 * Format parser, search small keywords and keyword's suffixes, and make
 * format-node tree.
 *
 * for DATE-TIME & NUMBER version
 * ----------
 */
static void parse_format(FormatNode *node, const char *str, const KeyWord *kw,
                         const KeySuffix *suf, const int *index, uint32 flags,
                         NUMDesc *Num) {
  FormatNode *n;

#ifdef DEBUG_TO_FROM_CHAR
  elog(DEBUG_elog_output, "to_char/number(): run parser");
#endif

  n = node;

  while (*str) {
    int suffix = 0;
    const KeySuffix *s;

    /*
     * Prefix
     */
    if ((flags & DCH_FLAG) &&
        (s = suff_search(str, suf, SUFFTYPE_PREFIX)) != NULL) {
      suffix |= s->id;
      if (s->len)
        str += s->len;
    }

    /*
     * Keyword
     */
    if (*str && (n->key = index_seq_search(str, kw, index)) != NULL) {
      n->type = NODE_TYPE_ACTION;
      n->suffix = suffix;
      if (n->key->len)
        str += n->key->len;

      /*
       * NUM version: Prepare global NUMDesc struct
       */
      if (flags & NUM_FLAG)
        NUMDesc_prepare(Num, n);

      /*
       * Postfix
       */
      if ((flags & DCH_FLAG) && *str &&
          (s = suff_search(str, suf, SUFFTYPE_POSTFIX)) != NULL) {
        n->suffix |= s->id;
        if (s->len)
          str += s->len;
      }

      n++;
    } else if (*str) {
      int chlen;

      if ((flags & STD_FLAG) && *str != '"') {
        /*
         * Standard mode, allow only following separators: "-./,':; ".
         * However, we support double quotes even in standard mode
         * (see below).  This is our extension of standard mode.
         */
        if (strchr("-./,':; ", *str) == NULL)
          ereport(ERROR, (errcode(ERRCODE_INVALID_DATETIME_FORMAT),
                          errmsg("invalid datetime format separator: \"%s\"",
                                 pnstrdup(str, pg_mblen_cstr(str)))));

        if (*str == ' ')
          n->type = NODE_TYPE_SPACE;
        else
          n->type = NODE_TYPE_SEPARATOR;

        n->character[0] = *str;
        n->character[1] = '\0';
        n->key = NULL;
        n->suffix = 0;
        n++;
        str++;
      } else if (*str == '"') {
        /*
         * Process double-quoted literal string, if any
         */
        str++;
        while (*str) {
          if (*str == '"') {
            str++;
            break;
          }
          /* backslash quotes the next character, if any */
          if (*str == '\\' && *(str + 1))
            str++;
          chlen = pg_mblen_cstr(str);
          n->type = NODE_TYPE_CHAR;
          memcpy(n->character, str, chlen);
          n->character[chlen] = '\0';
          n->key = NULL;
          n->suffix = 0;
          n++;
          str += chlen;
        }
      } else {
        /*
         * Outside double-quoted strings, backslash is only special if
         * it immediately precedes a double quote.
         */
        if (*str == '\\' && *(str + 1) == '"')
          str++;
        chlen = pg_mblen_cstr(str);

        if ((flags & DCH_FLAG) && is_separator_char(str))
          n->type = NODE_TYPE_SEPARATOR;
        else if (isspace((unsigned char)*str))
          n->type = NODE_TYPE_SPACE;
        else
          n->type = NODE_TYPE_CHAR;

        memcpy(n->character, str, chlen);
        n->character[chlen] = '\0';
        n->key = NULL;
        n->suffix = 0;
        n++;
        str += chlen;
      }
    }
  }

  n->type = NODE_TYPE_END;
  n->suffix = 0;
}

/* ----------
 * DEBUG: Dump the FormatNode Tree (debug)
 * ----------
 */
#ifdef DEBUG_TO_FROM_CHAR

#define DUMP_THth(_suf) (S_TH(_suf) ? "TH" : (S_th(_suf) ? "th" : " "))
#define DUMP_FM(_suf)	(S_FM(_suf) ? "FM" : " ")

static void dump_node(FormatNode *node, int max) {
  FormatNode *n;
  int a;

  elog(DEBUG_elog_output, "to_from-char(): DUMP FORMAT");

  for (a = 0, n = node; a <= max; n++, a++) {
    if (n->type == NODE_TYPE_ACTION)
      elog(DEBUG_elog_output, "%d:\t NODE_TYPE_ACTION '%s'\t(%s,%s)", a,
           n->key->name, DUMP_THth(n->suffix), DUMP_FM(n->suffix));
    else if (n->type == NODE_TYPE_CHAR)
      elog(DEBUG_elog_output, "%d:\t NODE_TYPE_CHAR '%s'", a, n->character);
    else if (n->type == NODE_TYPE_END) {
      elog(DEBUG_elog_output, "%d:\t NODE_TYPE_END", a);
      return;
    } else
      elog(DEBUG_elog_output, "%d:\t unknown NODE!", a);
  }
}
#endif /* DEBUG */

/*****************************************************************************
 *			Private utils
 *****************************************************************************/

/* ----------
 * Return ST/ND/RD/TH for simple (1..9) numbers
 * type --> 0 upper, 1 lower
 * ----------
 */
static const char *get_th(char *num, int type) {
  int len = strlen(num), last;

  last = *(num + (len - 1));
  if (!isdigit((unsigned char)last))
    ereport(ERROR, (errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
                    errmsg("\"%s\" is not a number", num)));

  /*
   * All "teens" (<x>1[0-9]) get 'TH/th', while <x>[02-9][123] still get
   * 'ST/st', 'ND/nd', 'RD/rd', respectively
   */
  if ((len > 1) && (num[len - 2] == '1'))
    last = 0;

  switch (last) {
  case '1':
    if (type == TH_UPPER)
      return numTH[0];
    return numth[0];
  case '2':
    if (type == TH_UPPER)
      return numTH[1];
    return numth[1];
  case '3':
    if (type == TH_UPPER)
      return numTH[2];
    return numth[2];
  default:
    if (type == TH_UPPER)
      return numTH[3];
    return numth[3];
  }
}

/* ----------
 * Convert string-number to ordinal string-number
 * type --> 0 upper, 1 lower
 * ----------
 */
static char *str_numth(char *dest, char *num, int type) {
  if (dest != num)
    strcpy(dest, num);
  strcat(dest, get_th(num, type));
  return dest;
}

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

static char *asc_tolower_z(const char *buff) {
  return asc_tolower(buff, strlen(buff));
}

static char *asc_toupper_z(const char *buff) {
  return asc_toupper(buff, strlen(buff));
}

/* asc_initcap_z is not currently needed */

/* ----------
 * Skip TM / th in FROM_CHAR
 *
 * If S_THth is on, skip two chars, assuming there are two available
 * ----------
 */

#ifdef DEBUG_TO_FROM_CHAR
/* -----------
 * DEBUG: Call for debug and for index checking; (Show ASCII char
 * and defined keyword for each used position
 * ----------
 */
static void dump_index(const KeyWord *k, const int *index) {
  int i, count = 0, free_i = 0;

  elog(DEBUG_elog_output, "TO-FROM_CHAR: Dump KeyWord Index:");

  for (i = 0; i < KeyWord_INDEX_SIZE; i++) {
    if (index[i] != -1) {
      elog(DEBUG_elog_output, "\t%c: %s, ", i + 32, k[index[i]].name);
      count++;
    } else {
      free_i++;
      elog(DEBUG_elog_output, "\t(%d) %c %d", i, i + 32, index[i]);
    }
  }
  elog(DEBUG_elog_output, "\n\t\tUsed positions: %d,\n\t\tFree positions: %d",
       count, free_i);
}
#endif /* DEBUG */

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
 *	the NUMBER version part
 *********************************************************************/

#define zeroize_NUM(_n) \
do { \
	(_n)->flag		= 0;	\
	(_n)->lsign		= 0;	\
	(_n)->pre		= 0;	\
	(_n)->post		= 0;	\
	(_n)->pre_lsign_num = 0;	\
	(_n)->need_locale	= 0;	\
	(_n)->multi		= 0;	\
	(_n)->zero_start	= 0;	\
	(_n)->zero_end		= 0;	\
} while(0)

/* This works the same as DCH_prevent_counter_overflow */
static inline void NUM_prevent_counter_overflow(void) {
  if (NUMCounter >= (INT_MAX - 1)) {
    for (int i = 0; i < n_NUMCache; i++)
      NUMCache[i]->age >>= 1;
    NUMCounter >>= 1;
  }
}

/* select a NUMCacheEntry to hold the given format picture */
static NUMCacheEntry *NUM_cache_getnew(const char *str) {
  NUMCacheEntry *ent;

  /* Ensure we can advance NUMCounter below */
  NUM_prevent_counter_overflow();

  /*
   * If cache is full, remove oldest entry (or recycle first not-valid one)
   */
  if (n_NUMCache >= NUM_CACHE_ENTRIES) {
    NUMCacheEntry *old = NUMCache[0];

#ifdef DEBUG_TO_FROM_CHAR
    elog(DEBUG_elog_output, "Cache is full (%d)", n_NUMCache);
#endif
    if (old->valid) {
      for (int i = 1; i < NUM_CACHE_ENTRIES; i++) {
        ent = NUMCache[i];
        if (!ent->valid) {
          old = ent;
          break;
        }
        if (ent->age < old->age)
          old = ent;
      }
    }
#ifdef DEBUG_TO_FROM_CHAR
    elog(DEBUG_elog_output, "OLD: \"%s\" AGE: %d", old->str, old->age);
#endif
    old->valid = false;
    strlcpy(old->str, str, NUM_CACHE_SIZE + 1);
    old->age = (++NUMCounter);
    /* caller is expected to fill format and Num, then set valid */
    return old;
  } else {
#ifdef DEBUG_TO_FROM_CHAR
    elog(DEBUG_elog_output, "NEW (%d)", n_NUMCache);
#endif
    Assert(NUMCache[n_NUMCache] == NULL);
    NUMCache[n_NUMCache] = ent = (NUMCacheEntry *)MemoryContextAllocZero(
        TopMemoryContext, sizeof(NUMCacheEntry));
    ent->valid = false;
    strlcpy(ent->str, str, NUM_CACHE_SIZE + 1);
    ent->age = (++NUMCounter);
    /* caller is expected to fill format and Num, then set valid */
    ++n_NUMCache;
    return ent;
  }
}

/* look for an existing NUMCacheEntry matching the given format picture */
static NUMCacheEntry *NUM_cache_search(const char *str) {
  /* Ensure we can advance NUMCounter below */
  NUM_prevent_counter_overflow();

  for (int i = 0; i < n_NUMCache; i++) {
    NUMCacheEntry *ent = NUMCache[i];

    if (ent->valid && strcmp(ent->str, str) == 0) {
      ent->age = (++NUMCounter);
      return ent;
    }
  }

  return NULL;
}

/* Find or create a NUMCacheEntry for the given format picture */
static NUMCacheEntry *NUM_cache_fetch(const char *str) {
  NUMCacheEntry *ent;

  if ((ent = NUM_cache_search(str)) == NULL) {
    /*
     * Not in the cache, must run parser and save a new format-picture to
     * the cache.  Do not mark the cache entry valid until parsing
     * succeeds.
     */
    ent = NUM_cache_getnew(str);

    zeroize_NUM(&ent->Num);

    parse_format(ent->format, str, NUM_keywords, NULL, NUM_index, NUM_FLAG,
                 &ent->Num);

    ent->valid = true;
  }
  return ent;
}

/* ----------
 * Cache routine for NUM to_char version
 * ----------
 */
static FormatNode *NUM_cache(int len, NUMDesc *Num, text *pars_str,
                             bool *shouldFree) {
  FormatNode *format = NULL;
  char *str;

  str = text_to_cstring(pars_str);

  if (len > NUM_CACHE_SIZE) {
    /*
     * Allocate new memory if format picture is bigger than static cache
     * and do not use cache (call parser always)
     */
    format = palloc_array(FormatNode, len + 1);

    *shouldFree = true;

    zeroize_NUM(Num);

    parse_format(format, str, NUM_keywords, NULL, NUM_index, NUM_FLAG, Num);
  } else {
    /*
     * Use cache buffers
     */
    NUMCacheEntry *ent = NUM_cache_fetch(str);

    *shouldFree = false;

    format = ent->format;

    /*
     * Copy cache to used struct
     */
    Num->flag = ent->Num.flag;
    Num->lsign = ent->Num.lsign;
    Num->pre = ent->Num.pre;
    Num->post = ent->Num.post;
    Num->pre_lsign_num = ent->Num.pre_lsign_num;
    Num->need_locale = ent->Num.need_locale;
    Num->multi = ent->Num.multi;
    Num->zero_start = ent->Num.zero_start;
    Num->zero_end = ent->Num.zero_end;
  }

#ifdef DEBUG_TO_FROM_CHAR
  /* dump_node(format, len); */
  dump_index(NUM_keywords, NUM_index);
#endif

  pfree(str);
  return format;
}

  
/*
 * Convert a roman numeral (standard form) to an integer.
 * Result is an integer between 1 and 3999.
 * Np->inout_p is advanced past the characters consumed.
 *
 * If input is invalid, return -1.
 */
static int roman_to_int(NUMProc *Np, int input_len) {
  int result = 0;
  int len;
  char romanChars[MAX_ROMAN_LEN];
  int romanValues[MAX_ROMAN_LEN];
  int repeatCount = 1;
  int vCount = 0, lCount = 0, dCount = 0;
  bool subtractionEncountered = false;
  int lastSubtractedValue = 0;

  /*
   * Skip any leading whitespace.  Perhaps we should limit the amount of
   * space skipped to MAX_ROMAN_LEN, but that seems unnecessarily picky.
   */
  while (!OVERLOAD_TEST && isspace((unsigned char)*Np->inout_p))
    Np->inout_p++;

  /*
   * Collect and decode valid roman numerals, consuming at most
   * MAX_ROMAN_LEN characters.  We do this in a separate loop to avoid
   * repeated decoding and because the main loop needs to know when it's at
   * the last numeral.
   */
  for (len = 0; len < MAX_ROMAN_LEN && !OVERLOAD_TEST; len++) {
    char currChar = pg_ascii_toupper(*Np->inout_p);
    int currValue = ROMAN_VAL(currChar);

    if (currValue == 0)
      break; /* Not a valid roman numeral. */
    romanChars[len] = currChar;
    romanValues[len] = currValue;
    Np->inout_p++;
  }

  if (len == 0)
    return -1; /* No valid roman numerals. */

  /* Check for valid combinations and compute the represented value. */
  for (int i = 0; i < len; i++) {
    char currChar = romanChars[i];
    int currValue = romanValues[i];

    /*
     * Ensure no numeral greater than or equal to the subtracted numeral
     * appears after a subtraction.
     */
    if (subtractionEncountered && currValue >= lastSubtractedValue)
      return -1;

    /*
     * V, L, and D should not appear before a larger numeral, nor should
     * they be repeated.
     */
    if ((vCount && currValue >= ROMAN_VAL('V')) ||
        (lCount && currValue >= ROMAN_VAL('L')) ||
        (dCount && currValue >= ROMAN_VAL('D')))
      return -1;
    if (currChar == 'V')
      vCount++;
    else if (currChar == 'L')
      lCount++;
    else if (currChar == 'D')
      dCount++;

    if (i < len - 1) {
      /* Compare current numeral to next numeral. */
      char nextChar = romanChars[i + 1];
      int nextValue = romanValues[i + 1];

      /*
       * If the current value is less than the next value, handle
       * subtraction. Verify valid subtractive combinations and update
       * the result accordingly.
       */
      if (currValue < nextValue) {
        if (!IS_VALID_SUB_COMB(currChar, nextChar))
          return -1;

        /*
         * Reject cases where same numeral is repeated with
         * subtraction (e.g. 'MCCM' or 'DCCCD').
         */
        if (repeatCount > 1)
          return -1;

        /*
         * We are going to skip nextChar, so first make checks needed
         * for V, L, and D.  These are the same as we'd have applied
         * if we reached nextChar without a subtraction.
         */
        if ((vCount && nextValue >= ROMAN_VAL('V')) ||
            (lCount && nextValue >= ROMAN_VAL('L')) ||
            (dCount && nextValue >= ROMAN_VAL('D')))
          return -1;
        if (nextChar == 'V')
          vCount++;
        else if (nextChar == 'L')
          lCount++;
        else if (nextChar == 'D')
          dCount++;

        /*
         * Skip the next numeral as it is part of the subtractive
         * combination.
         */
        i++;

        /* Update state. */
        repeatCount = 1;
        subtractionEncountered = true;
        lastSubtractedValue = currValue;
        result += (nextValue - currValue);
      } else {
        /* For same numerals, check for repetition. */
        if (currChar == nextChar) {
          repeatCount++;
          if (repeatCount > 3)
            return -1;
        } else
          repeatCount = 1;
        result += currValue;
      }
    } else {
      /* This is the last numeral; just add it to the result. */
      result += currValue;
    }
  }

  return result;
}

/* ----------
 * Locale
 * ----------
 */
static void NUM_prepare_locale(NUMProc *Np) {
  if (Np->Num->need_locale) {
    struct lconv *lconv;

    /*
     * Get locales
     */
    lconv = PGLC_localeconv();

    /*
     * Positive / Negative number sign
     */
    if (lconv->negative_sign && *lconv->negative_sign)
      Np->L_negative_sign = lconv->negative_sign;
    else
      Np->L_negative_sign = "-";

    if (lconv->positive_sign && *lconv->positive_sign)
      Np->L_positive_sign = lconv->positive_sign;
    else
      Np->L_positive_sign = "+";

    /*
     * Number decimal point
     */
    if (lconv->decimal_point && *lconv->decimal_point)
      Np->decimal = lconv->decimal_point;

    else
      Np->decimal = ".";

    if (!IS_LDECIMAL(Np->Num))
      Np->decimal = ".";

    /*
     * Number thousands separator
     *
     * Some locales (e.g. broken glibc pt_BR), have a comma for decimal,
     * but "" for thousands_sep, so we set the thousands_sep too.
     * http://archives.postgresql.org/pgsql-hackers/2007-11/msg00772.php
     */
    if (lconv->thousands_sep && *lconv->thousands_sep)
      Np->L_thousands_sep = lconv->thousands_sep;
    /* Make sure thousands separator doesn't match decimal point symbol. */
    else if (strcmp(Np->decimal, ",") != 0)
      Np->L_thousands_sep = ",";
    else
      Np->L_thousands_sep = ".";

    /*
     * Currency symbol
     */
    if (lconv->currency_symbol && *lconv->currency_symbol)
      Np->L_currency_symbol = lconv->currency_symbol;
    else
      Np->L_currency_symbol = " ";
  } else {
    /*
     * Default values
     */
    Np->L_negative_sign = "-";
    Np->L_positive_sign = "+";
    Np->decimal = ".";

    Np->L_thousands_sep = ",";
    Np->L_currency_symbol = " ";
  }
}

/* ----------
 * Return pointer of last relevant number after decimal point
 *	12.0500 --> last relevant is '5'
 *	12.0000 --> last relevant is '.'
 * If there is no decimal point, return NULL (which will result in same
 * behavior as if FM hadn't been specified).
 * ----------
 */
static char *get_last_relevant_decnum(char *num) {
  char *result, *p = strchr(num, '.');

#ifdef DEBUG_TO_FROM_CHAR
  elog(DEBUG_elog_output, "get_last_relevant_decnum()");
#endif

  if (!p)
    return NULL;

  result = p;

  while (*(++p)) {
    if (*p != '0')
      result = p;
  }

  return result;
}

/* ----------
 * Number extraction for TO_NUMBER()
 * ----------
 */
static void NUM_numpart_from_char(NUMProc *Np, int id, int input_len) {
  bool isread = false;

#ifdef DEBUG_TO_FROM_CHAR
  elog(DEBUG_elog_output, " --- scan start --- id=%s",
       (id == NUM_0 || id == NUM_9) ? "NUM_0/9"
       : id == NUM_DEC              ? "NUM_DEC"
                                    : "???");
#endif

  if (OVERLOAD_TEST)
    return;

  if (*Np->inout_p == ' ')
    Np->inout_p++;

  if (OVERLOAD_TEST)
    return;

  /*
   * read sign before number
   */
  if (*Np->number == ' ' && (id == NUM_0 || id == NUM_9) &&
      (Np->read_pre + Np->read_post) == 0) {
#ifdef DEBUG_TO_FROM_CHAR
    elog(DEBUG_elog_output,
         "Try read sign (%c), locale positive: %s, negative: %s", *Np->inout_p,
         Np->L_positive_sign, Np->L_negative_sign);
#endif

    /*
     * locale sign
     */
    if (IS_LSIGN(Np->Num) && Np->Num->lsign == NUM_LSIGN_PRE) {
      int x = 0;

#ifdef DEBUG_TO_FROM_CHAR
      elog(DEBUG_elog_output, "Try read locale pre-sign (%c)", *Np->inout_p);
#endif
      if ((x = strlen(Np->L_negative_sign)) && AMOUNT_TEST(x) &&
          strncmp(Np->inout_p, Np->L_negative_sign, x) == 0) {
        Np->inout_p += x;
        *Np->number = '-';
      } else if ((x = strlen(Np->L_positive_sign)) && AMOUNT_TEST(x) &&
                 strncmp(Np->inout_p, Np->L_positive_sign, x) == 0) {
        Np->inout_p += x;
        *Np->number = '+';
      }
    } else {
#ifdef DEBUG_TO_FROM_CHAR
      elog(DEBUG_elog_output, "Try read simple sign (%c)", *Np->inout_p);
#endif

      /*
       * simple + - < >
       */
      if (*Np->inout_p == '-' || (IS_BRACKET(Np->Num) && *Np->inout_p == '<')) {
        *Np->number = '-'; /* set - */
        Np->inout_p++;
      } else if (*Np->inout_p == '+') {
        *Np->number = '+'; /* set + */
        Np->inout_p++;
      }
    }
  }

  if (OVERLOAD_TEST)
    return;

#ifdef DEBUG_TO_FROM_CHAR
  elog(DEBUG_elog_output, "Scan for numbers (%c), current number: '%s'",
       *Np->inout_p, Np->number);
#endif

  /*
   * read digit or decimal point
   */
  if (isdigit((unsigned char)*Np->inout_p)) {
    if (Np->read_dec && Np->read_post == Np->Num->post)
      return;

    *Np->number_p = *Np->inout_p;
    Np->number_p++;

    if (Np->read_dec)
      Np->read_post++;
    else
      Np->read_pre++;

    isread = true;

#ifdef DEBUG_TO_FROM_CHAR
    elog(DEBUG_elog_output, "Read digit (%c)", *Np->inout_p);
#endif
  } else if (IS_DECIMAL(Np->Num) && Np->read_dec == false) {
    /*
     * We need not test IS_LDECIMAL(Np->Num) explicitly here, because
     * Np->decimal is always just "." if we don't have a D format token.
     * So we just unconditionally match to Np->decimal.
     */
    int x = strlen(Np->decimal);

#ifdef DEBUG_TO_FROM_CHAR
    elog(DEBUG_elog_output, "Try read decimal point (%c)", *Np->inout_p);
#endif
    if (x && AMOUNT_TEST(x) && strncmp(Np->inout_p, Np->decimal, x) == 0) {
      Np->inout_p += x - 1;
      *Np->number_p = '.';
      Np->number_p++;
      Np->read_dec = true;
      isread = true;
    }
  }

  if (OVERLOAD_TEST)
    return;

  /*
   * Read sign behind "last" number
   *
   * We need sign detection because determine exact position of post-sign is
   * difficult:
   *
   * FM9999.9999999S	   -> 123.001- 9.9S			   -> .5-
   * FM9.999999MI -> 5.01-
   */
  if (*Np->number == ' ' && Np->read_pre + Np->read_post > 0) {
    /*
     * locale sign (NUM_S) is always anchored behind a last number, if: -
     * locale sign expected - last read char was NUM_0/9 or NUM_DEC - and
     * next char is not digit
     */
    if (IS_LSIGN(Np->Num) && isread &&
        (Np->inout_p + 1) < Np->inout + input_len &&
        !isdigit((unsigned char)*(Np->inout_p + 1))) {
      int x;
      char *tmp = Np->inout_p++;

#ifdef DEBUG_TO_FROM_CHAR
      elog(DEBUG_elog_output, "Try read locale post-sign (%c)", *Np->inout_p);
#endif
      if ((x = strlen(Np->L_negative_sign)) && AMOUNT_TEST(x) &&
          strncmp(Np->inout_p, Np->L_negative_sign, x) == 0) {
        Np->inout_p += x - 1; /* -1 .. NUM_processor() do inout_p++ */
        *Np->number = '-';
      } else if ((x = strlen(Np->L_positive_sign)) && AMOUNT_TEST(x) &&
                 strncmp(Np->inout_p, Np->L_positive_sign, x) == 0) {
        Np->inout_p += x - 1; /* -1 .. NUM_processor() do inout_p++ */
        *Np->number = '+';
      }
      if (*Np->number == ' ')
        /* no sign read */
        Np->inout_p = tmp;
    }

    /*
     * try read non-locale sign, which happens only if format is not exact
     * and we cannot determine sign position of MI/PL/SG, an example:
     *
     * FM9.999999MI			   -> 5.01-
     *
     * if (.... && IS_LSIGN(Np->Num)==false) prevents read wrong formats
     * like to_number('1 -', '9S') where sign is not anchored to last
     * number.
     */
    else if (isread == false && IS_LSIGN(Np->Num) == false &&
             (IS_PLUS(Np->Num) || IS_MINUS(Np->Num))) {
#ifdef DEBUG_TO_FROM_CHAR
      elog(DEBUG_elog_output, "Try read simple post-sign (%c)", *Np->inout_p);
#endif

      /*
       * simple + -
       */
      if (*Np->inout_p == '-' || *Np->inout_p == '+')
        /* NUM_processor() do inout_p++ */
        *Np->number = *Np->inout_p;
    }
  }
}

#define IS_PREDEC_SPACE(_n) \
		(IS_ZERO((_n)->Num)==false && \
		 (_n)->number == (_n)->number_p && \
		 *(_n)->number == '0' && \
				 (_n)->Num->post != 0)

/* ----------
 * Add digit or sign to number-string
 * ----------
 */
static void NUM_numpart_to_char(NUMProc *Np, int id) {
  int end;

  if (IS_ROMAN(Np->Num))
    return;

    /* Note: in this elog() output not set '\0' in 'inout' */

#ifdef DEBUG_TO_FROM_CHAR

  /*
   * Np->num_curr is number of current item in format-picture, it is not
   * current position in inout!
   */
  elog(DEBUG_elog_output,
       "SIGN_WROTE: %d, CURRENT: %d, NUMBER_P: \"%s\", INOUT: \"%s\"",
       Np->sign_wrote, Np->num_curr, Np->number_p, Np->inout);
#endif
  Np->num_in = false;

  /*
   * Write sign if real number will write to output Note: IS_PREDEC_SPACE()
   * handle "9.9" --> " .1"
   */
  if (Np->sign_wrote == false &&
      (Np->num_curr >= Np->out_pre_spaces ||
       (IS_ZERO(Np->Num) && Np->Num->zero_start == Np->num_curr)) &&
      (IS_PREDEC_SPACE(Np) == false ||
       (Np->last_relevant && *Np->last_relevant == '.'))) {
    if (IS_LSIGN(Np->Num)) {
      if (Np->Num->lsign == NUM_LSIGN_PRE) {
        NUM_add_locale_symbol(Np, (Np->sign == '-') ? Np->L_negative_sign
                                                    : Np->L_positive_sign);
        Np->sign_wrote = true;
      }
    } else if (IS_BRACKET(Np->Num)) {
      *Np->inout_p = Np->sign == '+' ? ' ' : '<';
      ++Np->inout_p;
      Np->sign_wrote = true;
    } else if (Np->sign == '+') {
      if (!IS_FILLMODE(Np->Num)) {
        *Np->inout_p = ' '; /* Write + */
        ++Np->inout_p;
      }
      Np->sign_wrote = true;
    } else if (Np->sign == '-') { /* Write - */
      *Np->inout_p = '-';
      ++Np->inout_p;
      Np->sign_wrote = true;
    }
  }

  /*
   * digits / FM / Zero / Dec. point
   */
  if (id == NUM_9 || id == NUM_0 || id == NUM_D || id == NUM_DEC) {
    if (Np->num_curr < Np->out_pre_spaces &&
        (Np->Num->zero_start > Np->num_curr || !IS_ZERO(Np->Num))) {
      /*
       * Write blank space
       */
      if (!IS_FILLMODE(Np->Num)) {
        *Np->inout_p = ' '; /* Write ' ' */
        ++Np->inout_p;
      }
    } else if (IS_ZERO(Np->Num) && Np->num_curr < Np->out_pre_spaces &&
               Np->Num->zero_start <= Np->num_curr) {
      /*
       * Write ZERO
       */
      *Np->inout_p = '0'; /* Write '0' */
      ++Np->inout_p;
      Np->num_in = true;
    } else {
      /*
       * Write Decimal point
       */
      if (*Np->number_p == '.') {
        if (!Np->last_relevant || *Np->last_relevant != '.') {
          NUM_add_locale_symbol(Np, Np->decimal); /* Write DEC/D */
        }

        /*
         * Ora 'n' -- FM9.9 --> 'n.'
         */
        else if (IS_FILLMODE(Np->Num) && Np->last_relevant &&
                 *Np->last_relevant == '.') {
          NUM_add_locale_symbol(Np, Np->decimal); /* Write DEC/D */
        }
      } else {
        /*
         * Write Digits
         */
        if (Np->last_relevant && Np->number_p > Np->last_relevant &&
            id != NUM_0)
          ;

        /*
         * '0.1' -- 9.9 --> '  .1'
         */
        else if (IS_PREDEC_SPACE(Np)) {
          if (!IS_FILLMODE(Np->Num)) {
            *Np->inout_p = ' ';
            ++Np->inout_p;
          }

          /*
           * '0' -- FM9.9 --> '0.'
           */
          else if (Np->last_relevant && *Np->last_relevant == '.') {
            *Np->inout_p = '0';
            ++Np->inout_p;
          }
        } else {
          *Np->inout_p = *Np->number_p; /* Write DIGIT */
          ++Np->inout_p;
          Np->num_in = true;
        }
      }
      /* do no exceed string length */
      if (*Np->number_p)
        ++Np->number_p;
    }

    end = Np->num_count + (Np->out_pre_spaces ? 1 : 0) +
          (IS_DECIMAL(Np->Num) ? 1 : 0);

    if (Np->last_relevant && Np->last_relevant == Np->number_p)
      end = Np->num_curr;

    if (Np->num_curr + 1 == end) {
      if (Np->sign_wrote == true && IS_BRACKET(Np->Num)) {
        *Np->inout_p = Np->sign == '+' ? ' ' : '>';
        ++Np->inout_p;
      } else if (IS_LSIGN(Np->Num) && Np->Num->lsign == NUM_LSIGN_POST) {
        NUM_add_locale_symbol(Np, (Np->sign == '-') ? Np->L_negative_sign
                                                    : Np->L_positive_sign);
      }
    }
  }

  ++Np->num_curr;
}

/*
 * Append locale-specific symbol to Np->inout.
 * Note we don't null-terminate the output
 */
static void NUM_add_locale_symbol(NUMProc *Np, const char *pattern) {
  size_t pattern_len = strlen(pattern);

  /* Truncate symbol if it's potentially too long */
  if (unlikely(pattern_len > NUM_MAX_ITEM_SIZ))
    pattern_len = pg_mbcliplen(pattern, pattern_len, NUM_MAX_ITEM_SIZ);
  memcpy(Np->inout_p, pattern, pattern_len);
  Np->inout_p += pattern_len;
}

/*
 * Skip over "n" input characters, but only if they aren't numeric data
 */
static void NUM_eat_non_data_chars(NUMProc *Np, int n, int input_len) {
  const char *end = Np->inout + input_len;

  while (n-- > 0) {
    if (OVERLOAD_TEST)
      break; /* end of input */
    if (strchr("0123456789.,+-", *Np->inout_p) != NULL)
      break; /* it's a data character */
    Np->inout_p += pg_mblen_range(Np->inout_p, end);
  }
}

static char *NUM_processor(FormatNode *node, NUMDesc *Num, char *inout,
                           char *number, int input_len,
                           int to_char_out_pre_spaces, int sign,
                           bool is_to_char, Oid collid) {
  FormatNode *n;
  NUMProc _Np, *Np = &_Np;
  const char *pattern;
  int pattern_len;

  MemSet(Np, 0, sizeof(NUMProc));

  Np->Num = Num;
  Np->is_to_char = is_to_char;
  Np->number = number;
  Np->inout = inout;
  Np->last_relevant = NULL;
  Np->read_post = 0;
  Np->read_pre = 0;
  Np->read_dec = false;

  if (Np->Num->zero_start)
    --Np->Num->zero_start;

  if (IS_EEEE(Np->Num)) {
    if (!Np->is_to_char)
      ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                      errmsg("\"EEEE\" not supported for input")));
    return strcpy(inout, number);
  }

  /*
   * Sign
   */
  if (is_to_char) {
    Np->sign = sign;

    /* MI/PL/SG - write sign itself and not in number */
    if (IS_PLUS(Np->Num) || IS_MINUS(Np->Num)) {
      if (IS_PLUS(Np->Num) && IS_MINUS(Np->Num) == false)
        Np->sign_wrote = false; /* need sign */
      else
        Np->sign_wrote = true; /* needn't sign */
    } else {
      if (Np->sign != '-') {
        if (IS_FILLMODE(Np->Num))
          Np->Num->flag &= ~NUM_F_BRACKET;
      }

      if (Np->sign == '+' && IS_FILLMODE(Np->Num) && IS_LSIGN(Np->Num) == false)
        Np->sign_wrote = true; /* needn't sign */
      else
        Np->sign_wrote = false; /* need sign */

      if (Np->Num->lsign == NUM_LSIGN_PRE &&
          Np->Num->pre == Np->Num->pre_lsign_num)
        Np->Num->lsign = NUM_LSIGN_POST;
    }
  } else
    Np->sign = false;

  /*
   * Count
   */
  Np->num_count = Np->Num->post + Np->Num->pre - 1;

  if (is_to_char) {
    Np->out_pre_spaces = to_char_out_pre_spaces;

    if (IS_FILLMODE(Np->Num) && IS_DECIMAL(Np->Num)) {
      Np->last_relevant = get_last_relevant_decnum(Np->number);

      /*
       * If any '0' specifiers are present, make sure we don't strip
       * those digits.  But don't advance last_relevant beyond the last
       * character of the Np->number string, which is a hazard if the
       * number got shortened due to precision limitations.
       */
      if (Np->last_relevant && Np->Num->zero_end > Np->out_pre_spaces) {
        int last_zero_pos;
        char *last_zero;

        /* note that Np->number cannot be zero-length here */
        last_zero_pos = strlen(Np->number) - 1;
        last_zero_pos =
            Min(last_zero_pos, Np->Num->zero_end - Np->out_pre_spaces);
        last_zero = Np->number + last_zero_pos;
        if (Np->last_relevant < last_zero)
          Np->last_relevant = last_zero;
      }
    }

    if (Np->sign_wrote == false && Np->out_pre_spaces == 0)
      ++Np->num_count;
  } else {
    Np->out_pre_spaces = 0;
    *Np->number = ' '; /* sign space */
    *(Np->number + 1) = '\0';
  }

  Np->num_in = 0;
  Np->num_curr = 0;

#ifdef DEBUG_TO_FROM_CHAR
  elog(DEBUG_elog_output,
       "\n\tSIGN: '%c'\n\tNUM: '%s'\n\tPRE: %d\n\tPOST: %d\n\tNUM_COUNT: "
       "%d\n\tNUM_PRE: %d\n\tSIGN_WROTE: %s\n\tZERO: %s\n\tZERO_START: "
       "%d\n\tZERO_END: %d\n\tLAST_RELEVANT: %s\n\tBRACKET: %s\n\tPLUS: "
       "%s\n\tMINUS: %s\n\tFILLMODE: %s\n\tROMAN: %s\n\tEEEE: %s",
       Np->sign, Np->number, Np->Num->pre, Np->Num->post, Np->num_count,
       Np->out_pre_spaces, Np->sign_wrote ? "Yes" : "No",
       IS_ZERO(Np->Num) ? "Yes" : "No", Np->Num->zero_start, Np->Num->zero_end,
       Np->last_relevant ? Np->last_relevant : "<not set>",
       IS_BRACKET(Np->Num) ? "Yes" : "No", IS_PLUS(Np->Num) ? "Yes" : "No",
       IS_MINUS(Np->Num) ? "Yes" : "No", IS_FILLMODE(Np->Num) ? "Yes" : "No",
       IS_ROMAN(Np->Num) ? "Yes" : "No", IS_EEEE(Np->Num) ? "Yes" : "No");
#endif

  /*
   * Locale
   */
  NUM_prepare_locale(Np);

  /*
   * Processor direct cycle
   */
  if (Np->is_to_char)
    Np->number_p = Np->number;
  else
    Np->number_p = Np->number + 1; /* first char is space for sign */

  for (n = node, Np->inout_p = Np->inout; n->type != NODE_TYPE_END; n++) {
    if (!Np->is_to_char) {
      /*
       * Check at least one byte remains to be scanned.  (In actions
       * below, must use AMOUNT_TEST if we want to read more bytes than
       * that.)
       */
      if (OVERLOAD_TEST)
        break;
    }

    /*
     * Format pictures actions
     */
    if (n->type == NODE_TYPE_ACTION) {
      /*
       * Create/read digit/zero/blank/sign/special-case
       *
       * 'NUM_S' note: The locale sign is anchored to number and we
       * read/write it when we work with first or last number
       * (NUM_0/NUM_9).  This is why NUM_S is missing in switch().
       *
       * Notice the "Np->inout_p++" at the bottom of the loop.  This is
       * why most of the actions advance inout_p one less than you might
       * expect.  In cases where we don't want that increment to happen,
       * a switch case ends with "continue" not "break".
       */
      switch (n->key->id) {
      case NUM_9:
      case NUM_0:
      case NUM_DEC:
      case NUM_D:
        if (Np->is_to_char) {
          NUM_numpart_to_char(Np, n->key->id);
          continue; /* for() */
        } else {
          NUM_numpart_from_char(Np, n->key->id, input_len);
          break; /* switch() case: */
        }

      case NUM_COMMA:
        if (Np->is_to_char) {
          if (!Np->num_in) {
            if (IS_FILLMODE(Np->Num))
              continue;
            else
              *Np->inout_p = ' ';
          } else
            *Np->inout_p = ',';
        } else {
          if (!Np->num_in) {
            if (IS_FILLMODE(Np->Num))
              continue;
          }
          if (*Np->inout_p != ',')
            continue;
        }
        break;

      case NUM_G:
        pattern = Np->L_thousands_sep;
        pattern_len = strlen(pattern);
        if (Np->is_to_char) {
          /* Truncate symbol if it's potentially too long */
          if (unlikely(pattern_len > NUM_MAX_ITEM_SIZ))
            pattern_len = pg_mbcliplen(pattern, pattern_len, NUM_MAX_ITEM_SIZ);
          if (!Np->num_in) {
            if (IS_FILLMODE(Np->Num))
              continue;
            else {
              /* just in case there are MB chars */
              pattern_len = pg_mbstrlen_with_len(pattern, pattern_len);
              memset(Np->inout_p, ' ', pattern_len);
              Np->inout_p += pattern_len - 1;
            }
          } else {
            memcpy(Np->inout_p, pattern, pattern_len);
            Np->inout_p += pattern_len - 1;
          }
        } else {
          /* Here we do not truncate the symbol ... */
          if (!Np->num_in) {
            if (IS_FILLMODE(Np->Num))
              continue;
          }

          /*
           * Because L_thousands_sep typically contains data
           * characters (either '.' or ','), we can't use
           * NUM_eat_non_data_chars here.  Instead skip only if
           * the input matches L_thousands_sep.
           */
          if (AMOUNT_TEST(pattern_len) &&
              strncmp(Np->inout_p, pattern, pattern_len) == 0)
            Np->inout_p += pattern_len - 1;
          else
            continue;
        }
        break;

      case NUM_L:
        pattern = Np->L_currency_symbol;
        if (Np->is_to_char) {
          /* Truncate symbol if it's potentially too long */
          pattern_len = strlen(pattern);
          if (unlikely(pattern_len > NUM_MAX_ITEM_SIZ))
            pattern_len = pg_mbcliplen(pattern, pattern_len, NUM_MAX_ITEM_SIZ);

          memcpy(Np->inout_p, pattern, pattern_len);
          Np->inout_p += pattern_len - 1;
        } else {
          /* Here we do not truncate the symbol ... */
          NUM_eat_non_data_chars(Np, pg_mbstrlen(pattern), input_len);
          continue;
        }
        break;

      case NUM_RN:
      case NUM_rn:
        if (Np->is_to_char) {
          const char *number_p;

          if (n->key->id == NUM_rn)
            number_p = asc_tolower_z(Np->number_p);
          else
            number_p = Np->number_p;
          if (IS_FILLMODE(Np->Num))
            strcpy(Np->inout_p, number_p);
          else
            sprintf(Np->inout_p, "%15s", number_p);
          Np->inout_p += strlen(Np->inout_p) - 1;
        } else {
          int roman_result = roman_to_int(Np, input_len);
          int numlen;

          if (roman_result < 0)
            ereport(ERROR, (errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
                            errmsg("invalid Roman numeral")));
          numlen = sprintf(Np->number_p, "%d", roman_result);
          Np->number_p += numlen;
          Np->Num->pre = numlen;
          Np->Num->post = 0;
          continue; /* roman_to_int ate all the chars */
        }
        break;

      case NUM_th:
        if (IS_ROMAN(Np->Num) || *Np->number == '#' || Np->sign == '-' ||
            IS_DECIMAL(Np->Num))
          continue;

        if (Np->is_to_char) {
          strcpy(Np->inout_p, get_th(Np->number, TH_LOWER));
          Np->inout_p += 1;
        } else {
          /* All variants of 'th' occupy 2 characters */
          NUM_eat_non_data_chars(Np, 2, input_len);
          continue;
        }
        break;

      case NUM_TH:
        if (IS_ROMAN(Np->Num) || *Np->number == '#' || Np->sign == '-' ||
            IS_DECIMAL(Np->Num))
          continue;

        if (Np->is_to_char) {
          strcpy(Np->inout_p, get_th(Np->number, TH_UPPER));
          Np->inout_p += 1;
        } else {
          /* All variants of 'TH' occupy 2 characters */
          NUM_eat_non_data_chars(Np, 2, input_len);
          continue;
        }
        break;

      case NUM_MI:
        if (Np->is_to_char) {
          if (Np->sign == '-')
            *Np->inout_p = '-';
          else if (IS_FILLMODE(Np->Num))
            continue;
          else
            *Np->inout_p = ' ';
        } else {
          if (*Np->inout_p == '-')
            *Np->number = '-';
          else {
            NUM_eat_non_data_chars(Np, 1, input_len);
            continue;
          }
        }
        break;

      case NUM_PL:
        if (Np->is_to_char) {
          if (Np->sign == '+')
            *Np->inout_p = '+';
          else if (IS_FILLMODE(Np->Num))
            continue;
          else
            *Np->inout_p = ' ';
        } else {
          if (*Np->inout_p == '+')
            *Np->number = '+';
          else {
            NUM_eat_non_data_chars(Np, 1, input_len);
            continue;
          }
        }
        break;

      case NUM_SG:
        if (Np->is_to_char)
          *Np->inout_p = Np->sign;
        else {
          if (*Np->inout_p == '-')
            *Np->number = '-';
          else if (*Np->inout_p == '+')
            *Np->number = '+';
          else {
            NUM_eat_non_data_chars(Np, 1, input_len);
            continue;
          }
        }
        break;

      default:
        continue;
        break;
      }
    } else {
      /*
       * In TO_CHAR, non-pattern characters in the format are copied to
       * the output.  In TO_NUMBER, we skip one input character for each
       * non-pattern format character, whether or not it matches the
       * format character.
       */
      if (Np->is_to_char) {
        strcpy(Np->inout_p, n->character);
        Np->inout_p += strlen(Np->inout_p);
      } else {
        Np->inout_p += pg_mblen_range(Np->inout_p, Np->inout + input_len);
      }
      continue;
    }
    Np->inout_p++;
  }

  if (Np->is_to_char) {
    *Np->inout_p = '\0';
    return Np->inout;
  } else {
    if (*(Np->number_p - 1) == '.')
      *(Np->number_p - 1) = '\0';
    else
      *Np->number_p = '\0';

    /*
     * Correction - precision of dec. number
     */
    Np->Num->post = Np->read_post;

#ifdef DEBUG_TO_FROM_CHAR
    elog(DEBUG_elog_output, "TO_NUMBER (number): '%s'", Np->number);
#endif
    return Np->number;
  }
}

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
  NUMDesc Num;
  Datum result;
  FormatNode *format;
  char *numstr;
  bool shouldFree;
  int len = 0;
  int scale, precision;

  len = VARSIZE_ANY_EXHDR(fmt);

  if (len <= 0 || len >= INT_MAX / NUM_MAX_ITEM_SIZ)
    PG_RETURN_NULL();

  format = NUM_cache(len, &Num, fmt, &shouldFree);

  numstr = (char *)palloc((len * NUM_MAX_ITEM_SIZ) + 1);

  NUM_processor(format, &Num, VARDATA_ANY(value), numstr,
                VARSIZE_ANY_EXHDR(value), 0, 0, false, PG_GET_COLLATION());

  scale = Num.post;
  precision = Num.pre + Num.multi + scale;

  if (shouldFree)
    pfree(format);

  result = DirectFunctionCall3(
      numeric_in, CStringGetDatum(numstr), ObjectIdGetDatum(InvalidOid),
      Int32GetDatum(((precision << 16) | scale) + VARHDRSZ));

  if (IS_MULTI(&Num)) {
    Numeric x;
    Numeric a = int64_to_numeric(10);
    Numeric b = int64_to_numeric(-Num.multi);

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

    if (num.multi) {
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

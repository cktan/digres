/*
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * Portions Copyright (c) 2026, CK Tan: the changes for ty,
 * under the MIT License (https://github.com/cktan/digres/blob/main/LICENSE)
 */

/*
 * timezone.cc -- the rules of time zones, from the data of tzdata.cc.
 *
 * Derived from PostgreSQL's src/timezone/localtime.c (the IANA tz library:
 * "This file is in the public domain, so clarified as of 1996-06-05 by
 * Arthur David Olson"; leap second handling from Bradley White, POSIX-style
 * TZ variable handling from Guy Harris), from src/timezone/pgtz.c (pg_tzset,
 * pg_tzset_offset), and from DetermineTimeZoneOffset and its helpers in
 * src/backend/utils/adt/datetime.c. The changes:
 *
 * 1. A zone file is read from the pack of tzdata.h, not from a file; the
 *    name is looked up in any case, as pg_open_tzfile does.
 * 2. The zones found are kept in a table of the process, which a mutex
 *    guards; a zone found is never freed or changed.
 * 3. The public calls are those of ty_timezone.h.
 */
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"

#include "ty_timezone.h"

#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include <atomic>
#include <mutex>
#include <string>
#include <unordered_map>

#include "error.h"
#include "tzdata.h"

namespace {

typedef int32_t int32;
typedef int64_t int64;
typedef int64_t pg_time_t;
typedef uint64_t uint64;

/* from pgtime.h */
#define TZ_STRLEN_MAX 255

struct pg_tm {
  int tm_sec;
  int tm_min;
  int tm_hour;
  int tm_mday;
  int tm_mon;  /* from 0 */
  int tm_year; /* from 1900 */
  int tm_wday;
  int tm_yday;
  int tm_isdst;
  long int tm_gmtoff;
  const char *tm_zone;
};

/* from tzfile.h */
#define TZ_MAGIC "TZif"
struct tzhead {
  char tzh_magic[4];
  char tzh_version[1];
  char tzh_reserved[15];
  char tzh_ttisutcnt[4];
  char tzh_ttisstdcnt[4];
  char tzh_leapcnt[4];
  char tzh_timecnt[4];
  char tzh_typecnt[4];
  char tzh_charcnt[4];
};
#define TZ_MAX_TIMES 2000
#define TZ_MAX_TYPES 256
#define TZ_MAX_CHARS 50
#define TZ_MAX_LEAPS 50

/* from private.h */
#define is_digit(c) ((unsigned)(c) - '0' <= 9)
#define TYPE_BIT(type) (sizeof(type) * CHAR_BIT)
#define TYPE_SIGNED(type) (((type) - 1) < 0)
#define TWOS_COMPLEMENT(t) ((t) ~(t)0 < 0)
#define MAXVAL(t, b)                                                           \
  ((t)(((t)1 << ((b) - 1 - TYPE_SIGNED(t))) - 1 +                              \
       ((t)1 << ((b) - 1 - TYPE_SIGNED(t)))))
#define MINVAL(t, b)                                                           \
  ((t)(TYPE_SIGNED(t) ? -TWOS_COMPLEMENT(t) - MAXVAL(t, b) : 0))
#define TIME_T_MIN MINVAL(pg_time_t, TYPE_BIT(pg_time_t))
#define TIME_T_MAX MAXVAL(pg_time_t, TYPE_BIT(pg_time_t))
#define INT_STRLEN_MAXIMUM(type)                                               \
  ((TYPE_BIT(type) - TYPE_SIGNED(type)) * 302 / 1000 + 1 + TYPE_SIGNED(type))
#define INITIALIZE(x) ((x) = 0)
#define _(msgid) (msgid)
#define YEARSPERREPEAT 400
#define SECSPERMIN 60
#define MINSPERHOUR 60
#define HOURSPERDAY 24
#define DAYSPERWEEK 7
#define DAYSPERNYEAR 365
#define DAYSPERLYEAR 366
#define SECSPERHOUR (SECSPERMIN * MINSPERHOUR)
#define SECSPERDAY ((int32)SECSPERHOUR * HOURSPERDAY)
#define MONSPERYEAR 12
#define TM_SUNDAY 0
#define TM_MONDAY 1
#define TM_TUESDAY 2
#define TM_WEDNESDAY 3
#define TM_THURSDAY 4
#define TM_FRIDAY 5
#define TM_SATURDAY 6
#define TM_JANUARY 0
#define TM_FEBRUARY 1
#define TM_MARCH 2
#define TM_APRIL 3
#define TM_MAY 4
#define TM_JUNE 5
#define TM_JULY 6
#define TM_AUGUST 7
#define TM_SEPTEMBER 8
#define TM_OCTOBER 9
#define TM_NOVEMBER 10
#define TM_DECEMBER 11
#define TM_YEAR_BASE 1900
#define EPOCH_YEAR 1970
#define EPOCH_WDAY TM_THURSDAY
#define isleap(y) (((y) % 4) == 0 && (((y) % 100) != 0 || ((y) % 400) == 0))
#define isleap_sum(a, b) isleap((a) % 400 + (b) % 400)
#define AVGSECSPERYEAR 31556952L
#define SECSPERREPEAT ((int64)YEARSPERREPEAT * (int64)AVGSECSPERYEAR)
#define SECSPERREPEAT_BITS 34

#define SMALLEST(a, b) (((a) < (b)) ? (a) : (b))
#define BIGGEST(a, b) (((a) > (b)) ? (a) : (b))

struct ttinfo {    /* time type information */
  int32 tt_utoff;  /* UT offset in seconds */
  bool tt_isdst;   /* used to set tm_isdst */
  int tt_desigidx; /* abbreviation list index */
  bool tt_ttisstd; /* transition is std time */
  bool tt_ttisut;  /* transition is UT */
};

struct lsinfo {       /* leap second information */
  pg_time_t ls_trans; /* transition time */
  int64 ls_corr;      /* correction to apply */
};

struct state {
  int leapcnt;
  int timecnt;
  int typecnt;
  int charcnt;
  bool goback;
  bool goahead;
  pg_time_t ats[TZ_MAX_TIMES];
  unsigned char types[TZ_MAX_TIMES];
  struct ttinfo ttis[TZ_MAX_TYPES];
  char chars[BIGGEST(BIGGEST(TZ_MAX_CHARS + 1, 4 /* sizeof gmt */),
                     (2 * (TZ_STRLEN_MAX + 1)))];
  struct lsinfo lsis[TZ_MAX_LEAPS];

  /*
   * The time type to use for early times or if no transitions. It is always
   * zero for recent tzdb releases. It might be nonzero for data from tzdb
   * 2018e or earlier.
   */
  int defaulttype;
};

} // namespace

/* A zone: its name, and its rules. */
struct ty_tz_t {
  char TZname[TZ_STRLEN_MAX + 1]; /* the canonically-cased name */
  struct state state;
};

namespace {

typedef struct ty_tz_t pg_tz;

/* in the pack, below */
bool pack_find(const char *name, char *canonname, const unsigned char **data,
               size_t *size);

int tzload(const char *name, char *canonname, struct state *sp, bool doextend);
bool tzparse(const char *name, struct state *sp, bool lastditch);

#ifndef WILDABBR
/*
 * Someone might make incorrect use of a time zone abbreviation:
 *	1.	They might reference tzname[0] before calling tzset (explicitly
 *		or implicitly).
 *	2.	They might reference tzname[1] before calling tzset (explicitly
 *		or implicitly).
 *	3.	They might reference tzname[1] after setting to a time zone
 *		in which Daylight Saving Time is never observed.
 *	4.	They might reference tzname[0] after setting to a time zone
 *		in which Standard Time is never observed.
 *	5.	They might reference tm.tm_zone after calling offtime.
 * What's best to do in the above cases is open to debate;
 * for now, we just set things up so that in any of the five cases
 * WILDABBR is used. Another possibility: initialize tzname[0] to the
 * string "tzname[0] used before set", and similarly for the other cases.
 * And another: initialize tzname[0] to "ERA", with an explanation in the
 * manual page of what this "time zone abbreviation" means (doing this so
 * that tzname[0] has the "normal" length of three characters).
 */
#define WILDABBR	"   "
#endif /* !defined WILDABBR */

static const char wildabbr[] = WILDABBR;

static const char gmt[] = "GMT";

/*
 * The DST rules to use if a POSIX TZ string has no rules.
 * Default to US rules as of 2017-05-07.
 * POSIX does not specify the default DST rules;
 * for historical reasons, US rules are a common default.
 */
#define TZDEFRULESTRING ",M3.2.0,M11.1.0"

/* structs ttinfo, lsinfo, state have been moved to pgtz.h */

enum r_type {
  JULIAN_DAY,            /* Jn = Julian day */
  DAY_OF_YEAR,           /* n = day of year */
  MONTH_NTH_DAY_OF_WEEK, /* Mm.n.d = month, week, day of week */
};

struct rule {
  enum r_type r_type; /* type of rule */
  int r_day;          /* day number of rule */
  int r_week;         /* week number of rule */
  int r_mon;          /* month number of rule */
  int32 r_time;       /* transition time of rule */
};

/*
 * Prototypes for static functions.
 */

static struct pg_tm *gmtsub(pg_time_t const *timep, int32 offset,
                            struct pg_tm *tmp);
static bool increment_overflow(int *ip, int j);
static bool increment_overflow_time(pg_time_t *tp, int32 j);
static int64 leapcorr(struct state const *sp, pg_time_t t);
static struct pg_tm *timesub(pg_time_t const *timep, int32 offset,
                             struct state const *sp, struct pg_tm *tmp);
static bool typesequiv(struct state const *sp, int a, int b);

/*
 * Section 4.12.3 of X3.159-1989 requires that
 *	Except for the strftime function, these functions [asctime,
 *	ctime, gmtime, localtime] return values in one of two static
 *	objects: a broken-down time structure and an array of char.
 * Thanks to Paul Eggert for noting this.
 */

static thread_local /*LOCAL*/ struct pg_tm tm;

/* Initialize *S to a value based on UTOFF, ISDST, and DESIGIDX.  */
static void init_ttinfo(struct ttinfo *s, int32 utoff, bool isdst,
                        int desigidx) {
  s->tt_utoff = utoff;
  s->tt_isdst = isdst;
  s->tt_desigidx = desigidx;
  s->tt_ttisstd = false;
  s->tt_ttisut = false;
}

static int32 detzcode(const char *const codep) {
  int32 result;
  int i;
  int32 one = 1;
  int32 halfmaxval = one << (32 - 2);
  int32 maxval = halfmaxval - 1 + halfmaxval;
  int32 minval = -1 - maxval;

  result = codep[0] & 0x7f;
  for (i = 1; i < 4; ++i)
    result = (result << 8) | (codep[i] & 0xff);

  if (codep[0] & 0x80) {
    /*
     * Do two's-complement negation even on non-two's-complement machines.
     * If the result would be minval - 1, return minval.
     */
    result -= !TWOS_COMPLEMENT(int32) && result != 0;
    result += minval;
  }
  return result;
}

static int64 detzcode64(const char *const codep) {
  uint64 result;
  int i;
  int64 one = 1;
  int64 halfmaxval = one << (64 - 2);
  int64 maxval = halfmaxval - 1 + halfmaxval;
  int64 minval = -TWOS_COMPLEMENT(int64) - maxval;

  result = codep[0] & 0x7f;
  for (i = 1; i < 8; ++i)
    result = (result << 8) | (codep[i] & 0xff);

  if (codep[0] & 0x80) {
    /*
     * Do two's-complement negation even on non-two's-complement machines.
     * If the result would be minval - 1, return minval.
     */
    result -= !TWOS_COMPLEMENT(int64) && result != 0;
    result += minval;
  }
  return result;
}

static bool differ_by_repeat(const pg_time_t t1, const pg_time_t t0) {
  if (TYPE_BIT(pg_time_t) - TYPE_SIGNED(pg_time_t) < SECSPERREPEAT_BITS)
    return 0;
  return t1 - t0 == SECSPERREPEAT;
}

/* Input buffer for data read from a compiled tz file.  */
union input_buffer {
  /* The first part of the buffer, interpreted as a header.  */
  struct tzhead tzhead;

  /* The entire buffer.  */
  char buf[2 * sizeof(struct tzhead) + 2 * sizeof(struct state) +
           4 * TZ_MAX_TIMES];
};

/* Local storage needed for 'tzloadbody'.  */
union local_storage {
  /* The results of analyzing the file's contents after it is opened.  */
  struct file_analysis {
    /* The input buffer.  */
    union input_buffer u;

    /* A temporary state used for parsing a TZ string in the file.  */
    struct state st;
  } u;

  /* We don't need the "fullname" member */
};

/* Load tz data from the file named NAME into *SP.  Read extended
 * format if DOEXTEND.  Use *LSP for temporary storage.  Return 0 on
 * success, an errno value on failure.
 * PG: If "canonname" is not NULL, then on success the canonical spelling of
 * given name is stored there (the buffer must be > TZ_STRLEN_MAX bytes!).
 */
static int tzloadbody(char const *name, char *canonname, struct state *sp,
                      bool doextend, union local_storage *lsp) {
  int i;
  int stored;
  ssize_t nread;
  union input_buffer *up = &lsp->u.u;
  int tzheadsize = sizeof(struct tzhead);
  const unsigned char *zdata;
  size_t zsize;

  sp->goback = sp->goahead = false;

  if (name[0] == ':')
    ++name;

  /* The zone file is in the pack: no file is opened. */
  if (!pack_find(name, canonname, &zdata, &zsize))
    return ENOENT;
  nread = zsize < sizeof up->buf ? zsize : sizeof up->buf;
  memcpy(up->buf, zdata, nread);
  if (nread < tzheadsize)
    return EINVAL;
  for (stored = 4; stored <= 8; stored *= 2) {
    int32 ttisstdcnt = detzcode(up->tzhead.tzh_ttisstdcnt);
    int32 ttisutcnt = detzcode(up->tzhead.tzh_ttisutcnt);
    int64 prevtr = 0;
    int32 prevcorr = 0;
    int32 leapcnt = detzcode(up->tzhead.tzh_leapcnt);
    int32 timecnt = detzcode(up->tzhead.tzh_timecnt);
    int32 typecnt = detzcode(up->tzhead.tzh_typecnt);
    int32 charcnt = detzcode(up->tzhead.tzh_charcnt);
    char const *p = up->buf + tzheadsize;

    /*
     * Although tzfile(5) currently requires typecnt to be nonzero,
     * support future formats that may allow zero typecnt in files that
     * have a TZ string and no transitions.
     */
    if (!(0 <= leapcnt && leapcnt < TZ_MAX_LEAPS && 0 <= typecnt &&
          typecnt < TZ_MAX_TYPES && 0 <= timecnt && timecnt < TZ_MAX_TIMES &&
          0 <= charcnt && charcnt < TZ_MAX_CHARS &&
          (ttisstdcnt == typecnt || ttisstdcnt == 0) &&
          (ttisutcnt == typecnt || ttisutcnt == 0)))
      return EINVAL;
    if (nread < (tzheadsize               /* struct tzhead */
                 + timecnt * stored       /* ats */
                 + timecnt                /* types */
                 + typecnt * 6            /* ttinfos */
                 + charcnt                /* chars */
                 + leapcnt * (stored + 4) /* lsinfos */
                 + ttisstdcnt             /* ttisstds */
                 + ttisutcnt))            /* ttisuts */
      return EINVAL;
    sp->leapcnt = leapcnt;
    sp->timecnt = timecnt;
    sp->typecnt = typecnt;
    sp->charcnt = charcnt;

    /*
     * Read transitions, discarding those out of pg_time_t range. But
     * pretend the last transition before TIME_T_MIN occurred at
     * TIME_T_MIN.
     */
    timecnt = 0;
    for (i = 0; i < sp->timecnt; ++i) {
      int64 at = stored == 4 ? detzcode(p) : detzcode64(p);

      sp->types[i] = at <= TIME_T_MAX;
      if (sp->types[i]) {
        pg_time_t attime =
            ((TYPE_SIGNED(pg_time_t) ? at < TIME_T_MIN : at < 0) ? TIME_T_MIN
                                                                 : at);

        if (timecnt && attime <= sp->ats[timecnt - 1]) {
          if (attime < sp->ats[timecnt - 1])
            return EINVAL;
          sp->types[i - 1] = 0;
          timecnt--;
        }
        sp->ats[timecnt++] = attime;
      }
      p += stored;
    }

    timecnt = 0;
    for (i = 0; i < sp->timecnt; ++i) {
      unsigned char typ = *p++;

      if (sp->typecnt <= typ)
        return EINVAL;
      if (sp->types[i])
        sp->types[timecnt++] = typ;
    }
    sp->timecnt = timecnt;
    for (i = 0; i < sp->typecnt; ++i) {
      struct ttinfo *ttisp;
      unsigned char isdst, desigidx;

      ttisp = &sp->ttis[i];
      ttisp->tt_utoff = detzcode(p);
      p += 4;
      isdst = *p++;
      if (!(isdst < 2))
        return EINVAL;
      ttisp->tt_isdst = isdst;
      desigidx = *p++;
      if (!(desigidx < sp->charcnt))
        return EINVAL;
      ttisp->tt_desigidx = desigidx;
    }
    for (i = 0; i < sp->charcnt; ++i)
      sp->chars[i] = *p++;
    sp->chars[i] = '\0'; /* ensure '\0' at end */

    /* Read leap seconds, discarding those out of pg_time_t range.  */
    leapcnt = 0;
    for (i = 0; i < sp->leapcnt; ++i) {
      int64 tr = stored == 4 ? detzcode(p) : detzcode64(p);
      int32 corr = detzcode(p + stored);

      p += stored + 4;
      /* Leap seconds cannot occur before the Epoch.  */
      if (tr < 0)
        return EINVAL;
      if (tr <= TIME_T_MAX) {
        /*
         * Leap seconds cannot occur more than once per UTC month, and
         * UTC months are at least 28 days long (minus 1 second for a
         * negative leap second).  Each leap second's correction must
         * differ from the previous one's by 1 second.
         */
        if (tr - prevtr < 28 * SECSPERDAY - 1 ||
            (corr != prevcorr - 1 && corr != prevcorr + 1))
          return EINVAL;
        sp->lsis[leapcnt].ls_trans = prevtr = tr;
        sp->lsis[leapcnt].ls_corr = prevcorr = corr;
        leapcnt++;
      }
    }
    sp->leapcnt = leapcnt;

    for (i = 0; i < sp->typecnt; ++i) {
      struct ttinfo *ttisp;

      ttisp = &sp->ttis[i];
      if (ttisstdcnt == 0)
        ttisp->tt_ttisstd = false;
      else {
        if (*p != true && *p != false)
          return EINVAL;
        ttisp->tt_ttisstd = *p++;
      }
    }
    for (i = 0; i < sp->typecnt; ++i) {
      struct ttinfo *ttisp;

      ttisp = &sp->ttis[i];
      if (ttisutcnt == 0)
        ttisp->tt_ttisut = false;
      else {
        if (*p != true && *p != false)
          return EINVAL;
        ttisp->tt_ttisut = *p++;
      }
    }

    /*
     * If this is an old file, we're done.
     */
    if (up->tzhead.tzh_version[0] == '\0')
      break;
    nread -= p - up->buf;
    memmove(up->buf, p, nread);
  }
  if (doextend && nread > 2 && up->buf[0] == '\n' &&
      up->buf[nread - 1] == '\n' && sp->typecnt + 2 <= TZ_MAX_TYPES) {
    struct state *ts = &lsp->u.st;

    up->buf[nread - 1] = '\0';
    if (tzparse(&up->buf[1], ts, false)) {
      /*
       * Attempt to reuse existing abbreviations. Without this,
       * America/Anchorage would be right on the edge after 2037 when
       * TZ_MAX_CHARS is 50, as sp->charcnt equals 40 (for LMT AST AWT
       * APT AHST AHDT YST AKDT AKST) and ts->charcnt equals 10 (for
       * AKST AKDT).  Reusing means sp->charcnt can stay 40 in this
       * example.
       */
      int gotabbr = 0;
      int charcnt = sp->charcnt;

      for (i = 0; i < ts->typecnt; i++) {
        char *tsabbr = ts->chars + ts->ttis[i].tt_desigidx;
        int j;

        for (j = 0; j < charcnt; j++)
          if (strcmp(sp->chars + j, tsabbr) == 0) {
            ts->ttis[i].tt_desigidx = j;
            gotabbr++;
            break;
          }
        if (!(j < charcnt)) {
          int tsabbrlen = strlen(tsabbr);

          if (j + tsabbrlen < TZ_MAX_CHARS) {
            strcpy(sp->chars + j, tsabbr);
            charcnt = j + tsabbrlen + 1;
            ts->ttis[i].tt_desigidx = j;
            gotabbr++;
          }
        }
      }
      if (gotabbr == ts->typecnt) {
        sp->charcnt = charcnt;

        /*
         * Ignore any trailing, no-op transitions generated by zic as
         * they don't help here and can run afoul of bugs in zic 2016j
         * or earlier.
         */
        while (1 < sp->timecnt &&
               (sp->types[sp->timecnt - 1] == sp->types[sp->timecnt - 2]))
          sp->timecnt--;

        for (i = 0; i < ts->timecnt; i++)
          if (sp->timecnt == 0 || (sp->ats[sp->timecnt - 1] <
                                   ts->ats[i] + leapcorr(sp, ts->ats[i])))
            break;
        while (i < ts->timecnt && sp->timecnt < TZ_MAX_TIMES) {
          sp->ats[sp->timecnt] = ts->ats[i] + leapcorr(sp, ts->ats[i]);
          sp->types[sp->timecnt] = (sp->typecnt + ts->types[i]);
          sp->timecnt++;
          i++;
        }
        for (i = 0; i < ts->typecnt; i++)
          sp->ttis[sp->typecnt++] = ts->ttis[i];
      }
    }
  }
  if (sp->typecnt == 0)
    return EINVAL;
  if (sp->timecnt > 1) {
    for (i = 1; i < sp->timecnt; ++i)
      if (typesequiv(sp, sp->types[i], sp->types[0]) &&
          differ_by_repeat(sp->ats[i], sp->ats[0])) {
        sp->goback = true;
        break;
      }
    for (i = sp->timecnt - 2; i >= 0; --i)
      if (typesequiv(sp, sp->types[sp->timecnt - 1], sp->types[i]) &&
          differ_by_repeat(sp->ats[sp->timecnt - 1], sp->ats[i])) {
        sp->goahead = true;
        break;
      }
  }

  /*
   * Infer sp->defaulttype from the data.  Although this default type is
   * always zero for data from recent tzdb releases, things are trickier for
   * data from tzdb 2018e or earlier.
   *
   * The first set of heuristics work around bugs in 32-bit data generated
   * by tzdb 2013c or earlier.  The workaround is for zones like
   * Australia/Macquarie where timestamps before the first transition have a
   * time type that is not the earliest standard-time type.  See:
   * https://mm.icann.org/pipermail/tz/2013-May/019368.html
   */

  /*
   * If type 0 is unused in transitions, it's the type to use for early
   * times.
   */
  for (i = 0; i < sp->timecnt; ++i)
    if (sp->types[i] == 0)
      break;
  i = i < sp->timecnt ? -1 : 0;

  /*
   * Absent the above, if there are transition times and the first
   * transition is to a daylight time find the standard type less than and
   * closest to the type of the first transition.
   */
  if (i < 0 && sp->timecnt > 0 && sp->ttis[sp->types[0]].tt_isdst) {
    i = sp->types[0];
    while (--i >= 0)
      if (!sp->ttis[i].tt_isdst)
        break;
  }

  /*
   * The next heuristics are for data generated by tzdb 2018e or earlier,
   * for zones like EST5EDT where the first transition is to DST.
   */

  /*
   * If no result yet, find the first standard type. If there is none, punt
   * to type zero.
   */
  if (i < 0) {
    i = 0;
    while (sp->ttis[i].tt_isdst)
      if (++i >= sp->typecnt) {
        i = 0;
        break;
      }
  }

  /*
   * A simple 'sp->defaulttype = 0;' would suffice here if we didn't have to
   * worry about 2018e-or-earlier data.  Even simpler would be to remove the
   * defaulttype member and just use 0 in its place.
   */
  sp->defaulttype = i;

  return 0;
}

/* Load tz data from the file named NAME into *SP.  Read extended
 * format if DOEXTEND.  Return 0 on success, an errno value on failure.
 * PG: If "canonname" is not NULL, then on success the canonical spelling of
 * given name is stored there (the buffer must be > TZ_STRLEN_MAX bytes!).
 */
int tzload(const char *name, char *canonname, struct state *sp, bool doextend) {
  union local_storage *lsp = (local_storage *)malloc(sizeof *lsp);

  if (!lsp)
    return errno;
  else {
    int err = tzloadbody(name, canonname, sp, doextend, lsp);

    free(lsp);
    return err;
  }
}

static bool typesequiv(const struct state *sp, int a, int b) {
  bool result;

  if (sp == NULL || a < 0 || a >= sp->typecnt || b < 0 || b >= sp->typecnt)
    result = false;
  else {
    const struct ttinfo *ap = &sp->ttis[a];
    const struct ttinfo *bp = &sp->ttis[b];

    result =
        (ap->tt_utoff == bp->tt_utoff && ap->tt_isdst == bp->tt_isdst &&
         ap->tt_ttisstd == bp->tt_ttisstd && ap->tt_ttisut == bp->tt_ttisut &&
         (strcmp(&sp->chars[ap->tt_desigidx], &sp->chars[bp->tt_desigidx]) ==
          0));
  }
  return result;
}

static const int mon_lengths[2][MONSPERYEAR] = {
    {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31},
    {31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31}};

static const int year_lengths[2] = {DAYSPERNYEAR, DAYSPERLYEAR};

/*
 * Given a pointer into a timezone string, scan until a character that is not
 * a valid character in a time zone abbreviation is found.
 * Return a pointer to that character.
 */

static const char *getzname(const char *strp) {
  char c;

  while ((c = *strp) != '\0' && !is_digit(c) && c != ',' && c != '-' &&
         c != '+')
    ++strp;
  return strp;
}

/*
 * Given a pointer into an extended timezone string, scan until the ending
 * delimiter of the time zone abbreviation is located.
 * Return a pointer to the delimiter.
 *
 * As with getzname above, the legal character set is actually quite
 * restricted, with other characters producing undefined results.
 * We don't do any checking here; checking is done later in common-case code.
 */

static const char *getqzname(const char *strp, const int delim) {
  int c;

  while ((c = *strp) != '\0' && c != delim)
    ++strp;
  return strp;
}

/*
 * Given a pointer into a timezone string, extract a number from that string.
 * Check that the number is within a specified range; if it is not, return
 * NULL.
 * Otherwise, return a pointer to the first character not part of the number.
 */

static const char *getnum(const char *strp, int *const nump, const int min,
                          const int max) {
  char c;
  int num;

  if (strp == NULL || !is_digit(c = *strp))
    return NULL;
  num = 0;
  do {
    num = num * 10 + (c - '0');
    if (num > max)
      return NULL; /* illegal value */
    c = *++strp;
  } while (is_digit(c));
  if (num < min)
    return NULL; /* illegal value */
  *nump = num;
  return strp;
}

/*
 * Given a pointer into a timezone string, extract a number of seconds,
 * in hh[:mm[:ss]] form, from the string.
 * If any error occurs, return NULL.
 * Otherwise, return a pointer to the first character not part of the number
 * of seconds.
 */

static const char *getsecs(const char *strp, int32 *const secsp) {
  int num;

  /*
   * 'HOURSPERDAY * DAYSPERWEEK - 1' allows quasi-Posix rules like
   * "M10.4.6/26", which does not conform to Posix, but which specifies the
   * equivalent of "02:00 on the first Sunday on or after 23 Oct".
   */
  strp = getnum(strp, &num, 0, HOURSPERDAY * DAYSPERWEEK - 1);
  if (strp == NULL)
    return NULL;
  *secsp = num * (int32)SECSPERHOUR;
  if (*strp == ':') {
    ++strp;
    strp = getnum(strp, &num, 0, MINSPERHOUR - 1);
    if (strp == NULL)
      return NULL;
    *secsp += num * SECSPERMIN;
    if (*strp == ':') {
      ++strp;
      /* 'SECSPERMIN' allows for leap seconds.  */
      strp = getnum(strp, &num, 0, SECSPERMIN);
      if (strp == NULL)
        return NULL;
      *secsp += num;
    }
  }
  return strp;
}

/*
 * Given a pointer into a timezone string, extract an offset, in
 * [+-]hh[:mm[:ss]] form, from the string.
 * If any error occurs, return NULL.
 * Otherwise, return a pointer to the first character not part of the time.
 */

static const char *getoffset(const char *strp, int32 *const offsetp) {
  bool neg = false;

  if (*strp == '-') {
    neg = true;
    ++strp;
  } else if (*strp == '+')
    ++strp;
  strp = getsecs(strp, offsetp);
  if (strp == NULL)
    return NULL; /* illegal time */
  if (neg)
    *offsetp = -*offsetp;
  return strp;
}

/*
 * Given a pointer into a timezone string, extract a rule in the form
 * date[/time]. See POSIX section 8 for the format of "date" and "time".
 * If a valid rule is not found, return NULL.
 * Otherwise, return a pointer to the first character not part of the rule.
 */

static const char *getrule(const char *strp, struct rule *const rulep) {
  if (*strp == 'J') {
    /*
     * Julian day.
     */
    rulep->r_type = JULIAN_DAY;
    ++strp;
    strp = getnum(strp, &rulep->r_day, 1, DAYSPERNYEAR);
  } else if (*strp == 'M') {
    /*
     * Month, week, day.
     */
    rulep->r_type = MONTH_NTH_DAY_OF_WEEK;
    ++strp;
    strp = getnum(strp, &rulep->r_mon, 1, MONSPERYEAR);
    if (strp == NULL)
      return NULL;
    if (*strp++ != '.')
      return NULL;
    strp = getnum(strp, &rulep->r_week, 1, 5);
    if (strp == NULL)
      return NULL;
    if (*strp++ != '.')
      return NULL;
    strp = getnum(strp, &rulep->r_day, 0, DAYSPERWEEK - 1);
  } else if (is_digit(*strp)) {
    /*
     * Day of year.
     */
    rulep->r_type = DAY_OF_YEAR;
    strp = getnum(strp, &rulep->r_day, 0, DAYSPERLYEAR - 1);
  } else
    return NULL; /* invalid format */
  if (strp == NULL)
    return NULL;
  if (*strp == '/') {
    /*
     * Time specified.
     */
    ++strp;
    strp = getoffset(strp, &rulep->r_time);
  } else
    rulep->r_time = 2 * SECSPERHOUR; /* default = 2:00:00 */
  return strp;
}

/*
 * Given a year, a rule, and the offset from UT at the time that rule takes
 * effect, calculate the year-relative time that rule takes effect.
 */

static int32 transtime(const int year, const struct rule *const rulep,
                       const int32 offset) {
  bool leapyear;
  int32 value;
  int i;
  int d, m1, yy0, yy1, yy2, dow;

  INITIALIZE(value);
  leapyear = isleap(year);
  switch (rulep->r_type) {

  case JULIAN_DAY:

    /*
     * Jn - Julian day, 1 == January 1, 60 == March 1 even in leap
     * years. In non-leap years, or if the day number is 59 or less,
     * just add SECSPERDAY times the day number-1 to the time of
     * January 1, midnight, to get the day.
     */
    value = (rulep->r_day - 1) * SECSPERDAY;
    if (leapyear && rulep->r_day >= 60)
      value += SECSPERDAY;
    break;

  case DAY_OF_YEAR:

    /*
     * n - day of year. Just add SECSPERDAY times the day number to
     * the time of January 1, midnight, to get the day.
     */
    value = rulep->r_day * SECSPERDAY;
    break;

  case MONTH_NTH_DAY_OF_WEEK:

    /*
     * Mm.n.d - nth "dth day" of month m.
     */

    /*
     * Use Zeller's Congruence to get day-of-week of first day of
     * month.
     */
    m1 = (rulep->r_mon + 9) % 12 + 1;
    yy0 = (rulep->r_mon <= 2) ? (year - 1) : year;
    yy1 = yy0 / 100;
    yy2 = yy0 % 100;
    dow = ((26 * m1 - 2) / 10 + 1 + yy2 + yy2 / 4 + yy1 / 4 - 2 * yy1) % 7;
    if (dow < 0)
      dow += DAYSPERWEEK;

    /*
     * "dow" is the day-of-week of the first day of the month. Get the
     * day-of-month (zero-origin) of the first "dow" day of the month.
     */
    d = rulep->r_day - dow;
    if (d < 0)
      d += DAYSPERWEEK;
    for (i = 1; i < rulep->r_week; ++i) {
      if (d + DAYSPERWEEK >= mon_lengths[(int)leapyear][rulep->r_mon - 1])
        break;
      d += DAYSPERWEEK;
    }

    /*
     * "d" is the day-of-month (zero-origin) of the day we want.
     */
    value = d * SECSPERDAY;
    for (i = 0; i < rulep->r_mon - 1; ++i)
      value += mon_lengths[(int)leapyear][i] * SECSPERDAY;
    break;
  }

  /*
   * "value" is the year-relative time of 00:00:00 UT on the day in
   * question. To get the year-relative time of the specified local time on
   * that day, add the transition time and the current offset from UT.
   */
  return value + rulep->r_time + offset;
}

/*
 * Given a POSIX section 8-style TZ string, fill in the rule tables as
 * appropriate.
 * Returns true on success, false on failure.
 */
bool tzparse(const char *name, struct state *sp, bool lastditch) {
  const char *stdname;
  const char *dstname = NULL;
  size_t stdlen;
  size_t dstlen;
  size_t charcnt;
  int32 stdoffset;
  int32 dstoffset;
  char *cp;
  bool load_ok;

  stdname = name;
  if (lastditch) {
    /* Unlike IANA, don't assume name is exactly "GMT" */
    stdlen = strlen(name); /* length of standard zone name */
    name += stdlen;
    stdoffset = 0;
  } else {
    if (*name == '<') {
      name++;
      stdname = name;
      name = getqzname(name, '>');
      if (*name != '>')
        return false;
      stdlen = name - stdname;
      name++;
    } else {
      name = getzname(name);
      stdlen = name - stdname;
    }
    if (*name == '\0') /* we allow empty STD abbrev, unlike IANA */
      return false;
    name = getoffset(name, &stdoffset);
    if (name == NULL)
      return false;
  }
  charcnt = stdlen + 1;
  if (sizeof sp->chars < charcnt)
    return false;

  /*
   * The IANA code always tries to tzload(TZDEFRULES) here.  We do not want
   * to do that; it would be bad news in the lastditch case, where we can't
   * assume pg_open_tzfile() is sane yet.  Moreover, if we did load it and
   * it contains leap-second-dependent info, that would cause problems too.
   * Finally, IANA has deprecated the TZDEFRULES feature, so it presumably
   * will die at some point.  Desupporting it now seems like good
   * future-proofing.
   */
  load_ok = false;
  sp->goback = sp->goahead = false; /* simulate failed tzload() */
  sp->leapcnt = 0;                  /* intentionally assume no leap seconds */

  if (*name != '\0') {
    if (*name == '<') {
      dstname = ++name;
      name = getqzname(name, '>');
      if (*name != '>')
        return false;
      dstlen = name - dstname;
      name++;
    } else {
      dstname = name;
      name = getzname(name);
      dstlen = name - dstname; /* length of DST abbr. */
    }
    if (!dstlen)
      return false;
    charcnt += dstlen + 1;
    if (sizeof sp->chars < charcnt)
      return false;
    if (*name != '\0' && *name != ',' && *name != ';') {
      name = getoffset(name, &dstoffset);
      if (name == NULL)
        return false;
    } else
      dstoffset = stdoffset - SECSPERHOUR;
    if (*name == '\0' && !load_ok)
      name = TZDEFRULESTRING;
    if (*name == ',' || *name == ';') {
      struct rule start;
      struct rule end;
      int year;
      int yearlim;
      int timecnt;
      pg_time_t janfirst;
      int32 janoffset = 0;
      int yearbeg;

      ++name;
      if ((name = getrule(name, &start)) == NULL)
        return false;
      if (*name++ != ',')
        return false;
      if ((name = getrule(name, &end)) == NULL)
        return false;
      if (*name != '\0')
        return false;
      sp->typecnt = 2; /* standard time and DST */

      /*
       * Two transitions per year, from EPOCH_YEAR forward.
       */
      init_ttinfo(&sp->ttis[0], -stdoffset, false, 0);
      init_ttinfo(&sp->ttis[1], -dstoffset, true, stdlen + 1);
      sp->defaulttype = 0;
      timecnt = 0;
      janfirst = 0;
      yearbeg = EPOCH_YEAR;

      do {
        int32 yearsecs = year_lengths[isleap(yearbeg - 1)] * SECSPERDAY;

        yearbeg--;
        if (increment_overflow_time(&janfirst, -yearsecs)) {
          janoffset = -yearsecs;
          break;
        }
      } while (EPOCH_YEAR - YEARSPERREPEAT / 2 < yearbeg);

      yearlim = yearbeg + YEARSPERREPEAT + 1;
      for (year = yearbeg; year < yearlim; year++) {
        int32 starttime = transtime(year, &start, stdoffset),
              endtime = transtime(year, &end, dstoffset);
        int32 yearsecs = (year_lengths[isleap(year)] * SECSPERDAY);
        bool reversed = endtime < starttime;

        if (reversed) {
          int32 swap = starttime;

          starttime = endtime;
          endtime = swap;
        }
        if (reversed ||
            (starttime < endtime &&
             (endtime - starttime < (yearsecs + (stdoffset - dstoffset))))) {
          if (TZ_MAX_TIMES - 2 < timecnt)
            break;
          sp->ats[timecnt] = janfirst;
          if (!increment_overflow_time(&sp->ats[timecnt],
                                       janoffset + starttime))
            sp->types[timecnt++] = !reversed;
          sp->ats[timecnt] = janfirst;
          if (!increment_overflow_time(&sp->ats[timecnt],
                                       janoffset + endtime)) {
            sp->types[timecnt++] = reversed;
            yearlim = year + YEARSPERREPEAT + 1;
          }
        }
        if (increment_overflow_time(&janfirst, janoffset + yearsecs))
          break;
        janoffset = 0;
      }
      sp->timecnt = timecnt;
      if (!timecnt) {
        sp->ttis[0] = sp->ttis[1];
        sp->typecnt = 1; /* Perpetual DST.  */
      } else if (YEARSPERREPEAT < year - yearbeg)
        sp->goback = sp->goahead = true;
    } else {
      int32 theirstdoffset;
      int32 theirdstoffset;
      int32 theiroffset;
      bool isdst;
      int i;
      int j;

      if (*name != '\0')
        return false;

      /*
       * Initial values of theirstdoffset and theirdstoffset.
       */
      theirstdoffset = 0;
      for (i = 0; i < sp->timecnt; ++i) {
        j = sp->types[i];
        if (!sp->ttis[j].tt_isdst) {
          theirstdoffset = -sp->ttis[j].tt_utoff;
          break;
        }
      }
      theirdstoffset = 0;
      for (i = 0; i < sp->timecnt; ++i) {
        j = sp->types[i];
        if (sp->ttis[j].tt_isdst) {
          theirdstoffset = -sp->ttis[j].tt_utoff;
          break;
        }
      }

      /*
       * Initially we're assumed to be in standard time.
       */
      isdst = false;
      theiroffset = theirstdoffset;

      /*
       * Now juggle transition times and types tracking offsets as you
       * do.
       */
      for (i = 0; i < sp->timecnt; ++i) {
        j = sp->types[i];
        sp->types[i] = sp->ttis[j].tt_isdst;
        if (sp->ttis[j].tt_ttisut) {
          /* No adjustment to transition time */
        } else {
          /*
           * If daylight saving time is in effect, and the
           * transition time was not specified as standard time, add
           * the daylight saving time offset to the transition time;
           * otherwise, add the standard time offset to the
           * transition time.
           */
          /*
           * Transitions from DST to DDST will effectively disappear
           * since POSIX provides for only one DST offset.
           */
          if (isdst && !sp->ttis[j].tt_ttisstd) {
            sp->ats[i] += dstoffset - theirdstoffset;
          } else {
            sp->ats[i] += stdoffset - theirstdoffset;
          }
        }
        theiroffset = -sp->ttis[j].tt_utoff;
        if (sp->ttis[j].tt_isdst)
          theirdstoffset = theiroffset;
        else
          theirstdoffset = theiroffset;
      }

      /*
       * Finally, fill in ttis.
       */
      init_ttinfo(&sp->ttis[0], -stdoffset, false, 0);
      init_ttinfo(&sp->ttis[1], -dstoffset, true, stdlen + 1);
      sp->typecnt = 2;
      sp->defaulttype = 0;
    }
  } else {
    dstlen = 0;
    sp->typecnt = 1; /* only standard time */
    sp->timecnt = 0;
    init_ttinfo(&sp->ttis[0], -stdoffset, false, 0);
    sp->defaulttype = 0;
  }
  sp->charcnt = charcnt;
  cp = sp->chars;
  memcpy(cp, stdname, stdlen);
  cp += stdlen;
  *cp++ = '\0';
  if (dstlen != 0) {
    memcpy(cp, dstname, dstlen);
    *(cp + dstlen) = '\0';
  }
  return true;
}

static void gmtload(struct state *const sp) {
  if (tzload(gmt, NULL, sp, true) != 0)
    tzparse(gmt, sp, true);
}

/*
 * The easy way to behave "as if no library function calls" localtime
 * is to not call it, so we drop its guts into "localsub", which can be
 * freely called. (And no, the PANS doesn't require the above behavior,
 * but it *is* desirable.)
 */
static struct pg_tm *localsub(struct state const *sp, pg_time_t const *timep,
                              struct pg_tm *const tmp) {
  const struct ttinfo *ttisp;
  int i;
  struct pg_tm *result;
  const pg_time_t t = *timep;

  if (sp == NULL)
    return gmtsub(timep, 0, tmp);
  if ((sp->goback && t < sp->ats[0]) ||
      (sp->goahead && t > sp->ats[sp->timecnt - 1])) {
    pg_time_t newt = t;
    pg_time_t seconds;
    pg_time_t years;

    if (t < sp->ats[0])
      seconds = sp->ats[0] - t;
    else
      seconds = t - sp->ats[sp->timecnt - 1];
    --seconds;
    years = (seconds / SECSPERREPEAT + 1) * YEARSPERREPEAT;
    seconds = years * AVGSECSPERYEAR;
    if (t < sp->ats[0])
      newt += seconds;
    else
      newt -= seconds;
    if (newt < sp->ats[0] || newt > sp->ats[sp->timecnt - 1])
      return NULL; /* "cannot happen" */
    result = localsub(sp, &newt, tmp);
    if (result) {
      int64 newy;

      newy = result->tm_year;
      if (t < sp->ats[0])
        newy -= years;
      else
        newy += years;
      if (!(INT_MIN <= newy && newy <= INT_MAX))
        return NULL;
      result->tm_year = newy;
    }
    return result;
  }
  if (sp->timecnt == 0 || t < sp->ats[0]) {
    i = sp->defaulttype;
  } else {
    int lo = 1;
    int hi = sp->timecnt;

    while (lo < hi) {
      int mid = (lo + hi) >> 1;

      if (t < sp->ats[mid])
        hi = mid;
      else
        lo = mid + 1;
    }
    i = (int)sp->types[lo - 1];
  }
  ttisp = &sp->ttis[i];

  /*
   * To get (wrong) behavior that's compatible with System V Release 2.0
   * you'd replace the statement below with t += ttisp->tt_utoff;
   * timesub(&t, 0L, sp, tmp);
   */
  result = timesub(&t, ttisp->tt_utoff, sp, tmp);
  if (result) {
    result->tm_isdst = ttisp->tt_isdst;
    result->tm_zone = const_cast<char *>(&sp->chars[ttisp->tt_desigidx]);
  }
  return result;
}

struct pg_tm *pg_localtime(const pg_time_t *timep, const pg_tz *tz) {
  return localsub(&tz->state, timep, &tm);
}

/*
 * gmtsub is to gmtime as localsub is to localtime.
 *
 * Except we have a private "struct state" for GMT, so no sp is passed in.
 */

static struct pg_tm *gmtsub(pg_time_t const *timep, int32 offset,
                            struct pg_tm *tmp) {
  struct pg_tm *result;

  /* GMT timezone state data is kept here */
  static thread_local /*LOCAL*/ struct state *gmtptr = NULL;

  if (gmtptr == NULL) {
    /* Allocate on first use */
    gmtptr = (struct state *)malloc(sizeof(struct state));
    if (gmtptr == NULL)
      return NULL; /* errno should be set by malloc */
    gmtload(gmtptr);
  }

  result = timesub(timep, offset, gmtptr, tmp);

  /*
   * Could get fancy here and deliver something such as "+xx" or "-xx" if
   * offset is non-zero, but this is no time for a treasure hunt.
   */
  if (offset != 0)
    tmp->tm_zone = wildabbr;
  else
    tmp->tm_zone = gmtptr->chars;

  return result;
}

/*
 * Return the number of leap years through the end of the given year
 * where, to make the math easy, the answer for year zero is defined as zero.
 */

static int leaps_thru_end_of_nonneg(int y) { return y / 4 - y / 100 + y / 400; }

static int leaps_thru_end_of(const int y) {
  return (y < 0 ? -1 - leaps_thru_end_of_nonneg(-1 - y)
                : leaps_thru_end_of_nonneg(y));
}

static struct pg_tm *timesub(const pg_time_t *timep, int32 offset,
                             const struct state *sp, struct pg_tm *tmp) {
  const struct lsinfo *lp;
  pg_time_t tdays;
  int idays; /* unsigned would be so 2003 */
  int64 rem;
  int y;
  const int *ip;
  int64 corr;
  bool hit;
  int i;

  corr = 0;
  hit = false;
  i = (sp == NULL) ? 0 : sp->leapcnt;
  while (--i >= 0) {
    lp = &sp->lsis[i];
    if (*timep >= lp->ls_trans) {
      corr = lp->ls_corr;
      hit = (*timep == lp->ls_trans && (i == 0 ? 0 : lp[-1].ls_corr) < corr);
      break;
    }
  }
  y = EPOCH_YEAR;
  tdays = *timep / SECSPERDAY;
  rem = *timep % SECSPERDAY;
  while (tdays < 0 || tdays >= year_lengths[isleap(y)]) {
    int newy;
    pg_time_t tdelta;
    int idelta;
    int leapdays;

    tdelta = tdays / DAYSPERLYEAR;
    if (!((!TYPE_SIGNED(pg_time_t) || INT_MIN <= tdelta) && tdelta <= INT_MAX))
      goto out_of_range;
    idelta = tdelta;
    if (idelta == 0)
      idelta = (tdays < 0) ? -1 : 1;
    newy = y;
    if (increment_overflow(&newy, idelta))
      goto out_of_range;
    leapdays = leaps_thru_end_of(newy - 1) - leaps_thru_end_of(y - 1);
    tdays -= ((pg_time_t)newy - y) * DAYSPERNYEAR;
    tdays -= leapdays;
    y = newy;
  }

  /*
   * Given the range, we can now fearlessly cast...
   */
  idays = tdays;
  rem += offset - corr;
  while (rem < 0) {
    rem += SECSPERDAY;
    --idays;
  }
  while (rem >= SECSPERDAY) {
    rem -= SECSPERDAY;
    ++idays;
  }
  while (idays < 0) {
    if (increment_overflow(&y, -1))
      goto out_of_range;
    idays += year_lengths[isleap(y)];
  }
  while (idays >= year_lengths[isleap(y)]) {
    idays -= year_lengths[isleap(y)];
    if (increment_overflow(&y, 1))
      goto out_of_range;
  }
  tmp->tm_year = y;
  if (increment_overflow(&tmp->tm_year, -TM_YEAR_BASE))
    goto out_of_range;
  tmp->tm_yday = idays;

  /*
   * The "extra" mods below avoid overflow problems.
   */
  tmp->tm_wday =
      EPOCH_WDAY +
      ((y - EPOCH_YEAR) % DAYSPERWEEK) * (DAYSPERNYEAR % DAYSPERWEEK) +
      leaps_thru_end_of(y - 1) - leaps_thru_end_of(EPOCH_YEAR - 1) + idays;
  tmp->tm_wday %= DAYSPERWEEK;
  if (tmp->tm_wday < 0)
    tmp->tm_wday += DAYSPERWEEK;
  tmp->tm_hour = (int)(rem / SECSPERHOUR);
  rem %= SECSPERHOUR;
  tmp->tm_min = (int)(rem / SECSPERMIN);

  /*
   * A positive leap second requires a special representation. This uses
   * "... ??:59:60" et seq.
   */
  tmp->tm_sec = (int)(rem % SECSPERMIN) + hit;
  ip = mon_lengths[isleap(y)];
  for (tmp->tm_mon = 0; idays >= ip[tmp->tm_mon]; ++(tmp->tm_mon))
    idays -= ip[tmp->tm_mon];
  tmp->tm_mday = (int)(idays + 1);
  tmp->tm_isdst = 0;
  tmp->tm_gmtoff = offset;
  return tmp;

out_of_range:
  errno = EOVERFLOW;
  return NULL;
}

/*
 * Normalize logic courtesy Paul Eggert.
 */

static bool increment_overflow(int *ip, int j) {
  int const i = *ip;

  /*----------
   * If i >= 0 there can only be overflow if i + j > INT_MAX
   * or if j > INT_MAX - i; given i >= 0, INT_MAX - i cannot overflow.
   * If i < 0 there can only be overflow if i + j < INT_MIN
   * or if j < INT_MIN - i; given i < 0, INT_MIN - i cannot overflow.
   *----------
   */
  if ((i >= 0) ? (j > INT_MAX - i) : (j < INT_MIN - i))
    return true;
  *ip += j;
  return false;
}

static bool increment_overflow_time(pg_time_t *tp, int32 j) {
  /*----------
   * This is like
   * 'if (! (TIME_T_MIN <= *tp + j && *tp + j <= TIME_T_MAX)) ...',
   * except that it does the right thing even if *tp + j would overflow.
   *----------
   */
  if (!(j < 0 ? (TYPE_SIGNED(pg_time_t) ? TIME_T_MIN - j <= *tp : -1 - j < *tp)
              : *tp <= TIME_T_MAX - j))
    return true;
  *tp += j;
  return false;
}

static int64 leapcorr(struct state const *sp, pg_time_t t) {
  struct lsinfo const *lp;
  int i;

  i = sp->leapcnt;
  while (--i >= 0) {
    lp = &sp->lsis[i];
    if (t >= lp->ls_trans)
      return lp->ls_corr;
  }
  return 0;
}

/*
 * Find the next DST transition time in the given zone after the given time
 *
 * *timep and *tz are input arguments, the other parameters are output values.
 *
 * When the function result is 1, *boundary is set to the pg_time_t
 * representation of the next DST transition time after *timep,
 * *before_gmtoff and *before_isdst are set to the GMT offset and isdst
 * state prevailing just before that boundary (in particular, the state
 * prevailing at *timep), and *after_gmtoff and *after_isdst are set to
 * the state prevailing just after that boundary.
 *
 * When the function result is 0, there is no known DST transition
 * after *timep, but *before_gmtoff and *before_isdst indicate the GMT
 * offset and isdst state prevailing at *timep.  (This would occur in
 * DST-less time zones, or if a zone has permanently ceased using DST.)
 *
 * A function result of -1 indicates failure (this case does not actually
 * occur in our current implementation).
 */
int pg_next_dst_boundary(const pg_time_t *timep, long int *before_gmtoff,
                         int *before_isdst, pg_time_t *boundary,
                         long int *after_gmtoff, int *after_isdst,
                         const pg_tz *tz) {
  const struct state *sp;
  const struct ttinfo *ttisp;
  int i;
  int j;
  const pg_time_t t = *timep;

  sp = &tz->state;
  if (sp->timecnt == 0) {
    /* non-DST zone, use the defaulttype */
    ttisp = &sp->ttis[sp->defaulttype];
    *before_gmtoff = ttisp->tt_utoff;
    *before_isdst = ttisp->tt_isdst;
    return 0;
  }
  if ((sp->goback && t < sp->ats[0]) ||
      (sp->goahead && t > sp->ats[sp->timecnt - 1])) {
    /* For values outside the transition table, extrapolate */
    pg_time_t newt = t;
    pg_time_t seconds;
    pg_time_t tcycles;
    int64 icycles;
    int result;

    if (t < sp->ats[0])
      seconds = sp->ats[0] - t;
    else
      seconds = t - sp->ats[sp->timecnt - 1];
    --seconds;
    tcycles = seconds / YEARSPERREPEAT / AVGSECSPERYEAR;
    ++tcycles;
    icycles = tcycles;
    if (tcycles - icycles >= 1 || icycles - tcycles >= 1)
      return -1;
    seconds = icycles;
    seconds *= YEARSPERREPEAT;
    seconds *= AVGSECSPERYEAR;
    if (t < sp->ats[0])
      newt += seconds;
    else
      newt -= seconds;
    if (newt < sp->ats[0] || newt > sp->ats[sp->timecnt - 1])
      return -1; /* "cannot happen" */

    result = pg_next_dst_boundary(&newt, before_gmtoff, before_isdst, boundary,
                                  after_gmtoff, after_isdst, tz);
    if (t < sp->ats[0])
      *boundary -= seconds;
    else
      *boundary += seconds;
    return result;
  }

  if (t >= sp->ats[sp->timecnt - 1]) {
    /* No known transition > t, so use last known segment's type */
    i = sp->types[sp->timecnt - 1];
    ttisp = &sp->ttis[i];
    *before_gmtoff = ttisp->tt_utoff;
    *before_isdst = ttisp->tt_isdst;
    return 0;
  }
  if (t < sp->ats[0]) {
    /* For "before", use the defaulttype */
    ttisp = &sp->ttis[sp->defaulttype];
    *before_gmtoff = ttisp->tt_utoff;
    *before_isdst = ttisp->tt_isdst;
    *boundary = sp->ats[0];
    /* And for "after", use the first segment's type */
    i = sp->types[0];
    ttisp = &sp->ttis[i];
    *after_gmtoff = ttisp->tt_utoff;
    *after_isdst = ttisp->tt_isdst;
    return 1;
  }
  /* Else search to find the boundary following t */
  {
    int lo = 1;
    int hi = sp->timecnt - 1;

    while (lo < hi) {
      int mid = (lo + hi) >> 1;

      if (t < sp->ats[mid])
        hi = mid;
      else
        lo = mid + 1;
    }
    i = lo;
  }
  j = sp->types[i - 1];
  ttisp = &sp->ttis[j];
  *before_gmtoff = ttisp->tt_utoff;
  *before_isdst = ttisp->tt_isdst;
  *boundary = sp->ats[i];
  j = sp->types[i];
  ttisp = &sp->ttis[j];
  *after_gmtoff = ttisp->tt_utoff;
  *after_isdst = ttisp->tt_isdst;
  return 1;
}

/*
 * Identify a timezone abbreviation's meaning in the given zone
 *
 * Determine the GMT offset and DST flag associated with the abbreviation.
 * This is generally used only when the abbreviation has actually changed
 * meaning over time; therefore, we also take a UTC cutoff time, and return
 * the meaning in use at or most recently before that time, or the meaning
 * in first use after that time if the abbrev was never used before that.
 *
 * On success, returns true and sets *gmtoff and *isdst.  If the abbreviation
 * was never used at all in this zone, returns false.
 *
 * Note: abbrev is matched case-sensitively; it should be all-upper-case.
 */
bool pg_interpret_timezone_abbrev(const char *abbrev, const pg_time_t *timep,
                                  long int *gmtoff, int *isdst,
                                  const pg_tz *tz) {
  const struct state *sp;
  const char *abbrs;
  const struct ttinfo *ttisp;
  int abbrind;
  int cutoff;
  int i;
  const pg_time_t t = *timep;

  sp = &tz->state;

  /*
   * Locate the abbreviation in the zone's abbreviation list.  We assume
   * there are not duplicates in the list.
   */
  abbrs = sp->chars;
  abbrind = 0;
  while (abbrind < sp->charcnt) {
    if (strcmp(abbrev, abbrs + abbrind) == 0)
      break;
    while (abbrs[abbrind] != '\0')
      abbrind++;
    abbrind++;
  }
  if (abbrind >= sp->charcnt)
    return false; /* not there! */

  /*
   * Unlike pg_next_dst_boundary, we needn't sweat about extrapolation
   * (goback/goahead zones).  Finding the newest or oldest meaning of the
   * abbreviation should get us what we want, since extrapolation would just
   * be repeating the newest or oldest meanings.
   *
   * Use binary search to locate the first transition > cutoff time.  (Note
   * that sp->timecnt could be zero, in which case this loop does nothing
   * and only the defaulttype entry will be checked.)
   */
  {
    int lo = 0;
    int hi = sp->timecnt;

    while (lo < hi) {
      int mid = (lo + hi) >> 1;

      if (t < sp->ats[mid])
        hi = mid;
      else
        lo = mid + 1;
    }
    cutoff = lo;
  }

  /*
   * Scan backwards to find the latest interval using the given abbrev
   * before the cutoff time.
   */
  for (i = cutoff - 1; i >= 0; i--) {
    ttisp = &sp->ttis[sp->types[i]];
    if (ttisp->tt_desigidx == abbrind) {
      *gmtoff = ttisp->tt_utoff;
      *isdst = ttisp->tt_isdst;
      return true;
    }
  }

  /*
   * Not found yet; check the defaulttype, which is notionally the era
   * before any of the entries in sp->types[].
   */
  ttisp = &sp->ttis[sp->defaulttype];
  if (ttisp->tt_desigidx == abbrind) {
    *gmtoff = ttisp->tt_utoff;
    *isdst = ttisp->tt_isdst;
    return true;
  }

  /*
   * Not there, so scan forwards to find the first one after the cutoff.
   */
  for (i = cutoff; i < sp->timecnt; i++) {
    ttisp = &sp->ttis[sp->types[i]];
    if (ttisp->tt_desigidx == abbrind) {
      *gmtoff = ttisp->tt_utoff;
      *isdst = ttisp->tt_isdst;
      return true;
    }
  }

  return false; /* hm, not actually used in any interval? */
}

/*
 * Detect whether a timezone abbreviation is defined within the given zone.
 *
 * This is similar to pg_interpret_timezone_abbrev() but is not concerned
 * with a specific point in time.  We want to know if the abbreviation is
 * known at all, and if so whether it has one meaning or several.
 *
 * Returns true if the abbreviation is known, false if not.
 * If the abbreviation is known and has a single meaning (only one value
 * of gmtoff/isdst), sets *isfixed = true and sets *gmtoff and *isdst.
 * If there are multiple meanings, sets *isfixed = false.
 *
 * Note: abbrev is matched case-sensitively; it should be all-upper-case.
 */
bool pg_timezone_abbrev_is_known(const char *abbrev, bool *isfixed,
                                 long int *gmtoff, int *isdst,
                                 const pg_tz *tz) {
  bool result = false;
  const struct state *sp = &tz->state;
  const char *abbrs;
  int abbrind;

  /*
   * Locate the abbreviation in the zone's abbreviation list.  We assume
   * there are not duplicates in the list.
   */
  abbrs = sp->chars;
  abbrind = 0;
  while (abbrind < sp->charcnt) {
    if (strcmp(abbrev, abbrs + abbrind) == 0)
      break;
    while (abbrs[abbrind] != '\0')
      abbrind++;
    abbrind++;
  }
  if (abbrind >= sp->charcnt)
    return false; /* definitely not there */

  /*
   * Scan the ttinfo array to find uses of the abbreviation.
   */
  for (int i = 0; i < sp->typecnt; i++) {
    const struct ttinfo *ttisp = &sp->ttis[i];

    if (ttisp->tt_desigidx == abbrind) {
      if (!result) {
        /* First usage */
        *isfixed = true; /* for the moment */
        *gmtoff = ttisp->tt_utoff;
        *isdst = ttisp->tt_isdst;
        result = true;
      } else {
        /* Second or later usage, does it match? */
        if (*gmtoff != ttisp->tt_utoff || *isdst != ttisp->tt_isdst) {
          *isfixed = false;
          break; /* no point in looking further */
        }
      }
    }
  }

  return result;
}

/*
 * If the given timezone uses only one GMT offset, store that offset
 * into *gmtoff and return true, else return false.
 */
bool pg_get_timezone_offset(const pg_tz *tz, long int *gmtoff) {
  /*
   * The zone could have more than one ttinfo, if it's historically used
   * more than one abbreviation.  We return true as long as they all have
   * the same gmtoff.
   */
  const struct state *sp;
  int i;

  sp = &tz->state;
  for (i = 1; i < sp->typecnt; i++) {
    if (sp->ttis[i].tt_utoff != sp->ttis[0].tt_utoff)
      return false;
  }
  *gmtoff = sp->ttis[0].tt_utoff;
  return true;
}

/*
 * The pack of zones (tzdata.h, the format at the top of
 * tools/gen_tzdata.py). It is checked once, when it is first used.
 */
struct pack_t {
  const unsigned char *base = nullptr;
  size_t size = 0;
  uint32_t nnames = 0, nblobs = 0;
  const unsigned char *names = nullptr; /* nnames of (name offset, blob) */
  const unsigned char *blobs = nullptr; /* nblobs of (offset, size) */
  char release[17] = {0};
};

/* A big-endian number of the pack. */
inline uint32_t be32(const unsigned char *p) {
  return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 |
         (uint32_t)p[3];
}

/* FNV-1a over the pack, the checksum's own four bytes taken as zero. */
uint32_t pack_checksum(const unsigned char *p, size_t n) {
  uint32_t h = 0x811C9DC5u;

  for (size_t i = 0; i < n; i++) {
    unsigned char c = (i >= 36 && i < 40) ? 0 : p[i];

    h = (h ^ c) * 0x01000193u;
  }
  return h;
}

/*
 * Check a pack and describe it.
 * 1. The magic number, the format version and the size.
 * 2. The checksum.
 * 3. The two tables are inside the pack.
 * 4. Every name ends inside the pack, and names to a zone file that is
 *    inside the pack too.
 */
bool pack_check(const unsigned char *p, size_t n, pack_t *out) {
  if (n < 40 || memcmp(p, "TYTZ", 4) != 0 || be32(p + 4) != 1 ||
      be32(p + 32) != n || be32(p + 36) != pack_checksum(p, n)) {
    return false;
  }
  uint64_t nnames = be32(p + 24), nblobs = be32(p + 28);

  if (40 + 8 * nnames + 8 * nblobs > n) {
    return false;
  }
  const unsigned char *names = p + 40;
  const unsigned char *blobs = names + 8 * nnames;

  for (uint64_t i = 0; i < nnames; i++) {
    uint64_t off = be32(names + 8 * i);

    if (off >= n || be32(names + 8 * i + 4) >= nblobs ||
        memchr(p + off, 0, n - off) == nullptr) {
      return false;
    }
  }
  for (uint64_t i = 0; i < nblobs; i++) {
    if ((uint64_t)be32(blobs + 8 * i) + be32(blobs + 8 * i + 4) > n) {
      return false;
    }
  }
  out->base = p;
  out->size = n;
  out->nnames = (uint32_t)nnames;
  out->nblobs = (uint32_t)nblobs;
  out->names = names;
  out->blobs = blobs;
  memcpy(out->release, p + 8, 16);
  out->release[16] = '\0';
  return true;
}

std::mutex g_mu; /* guards the cache and the pack */
std::unordered_map<std::string, ty_tz_t *> g_cache; /* by the upper-case name */
std::atomic<const pack_t *> g_pack{nullptr}; /* the host's pack, once checked */
bool g_zone_found = false; /* a zone is out: the pack is fixed */

/* The pack of this build, checked on its first use. */
const pack_t *pack() {
  static pack_t own;
  static bool ok =
      pack_check(ty_internal::ty_tzpack, ty_internal::ty_tzpack_size, &own);

  const pack_t *host = g_pack.load(std::memory_order_acquire);

  return host ? host : (ok ? &own : nullptr);
}

/* strcasecmp for ASCII, which does not depend on the locale. */
int ci_cmp(const char *a, const char *b) {
  for (;; a++, b++) {
    int x = (unsigned char)*a, y = (unsigned char)*b;

    if (x >= 'A' && x <= 'Z') {
      x += 'a' - 'A';
    }
    if (y >= 'A' && y <= 'Z') {
      y += 'a' - 'A';
    }
    if (x != y || x == 0) {
      return x - y;
    }
  }
}

/*
 * Find a zone file by name in any case (pg_open_tzfile's job): its bytes,
 * and its name as the database spells it.
 */
bool pack_find(const char *name, char *canonname, const unsigned char **data,
               size_t *size) {
  const pack_t *pk = pack();

  if (pk == nullptr || strlen(name) > TZ_STRLEN_MAX) {
    return false;
  }
  uint32_t lo = 0, hi = pk->nnames;

  while (lo < hi) {
    uint32_t mid = lo + (hi - lo) / 2;
    const unsigned char *e = pk->names + 8 * (size_t)mid;
    const char *nm = (const char *)pk->base + be32(e);
    int c = ci_cmp(name, nm);

    if (c == 0) {
      const unsigned char *b = pk->blobs + 8 * (size_t)be32(e + 4);

      *data = pk->base + be32(b);
      *size = be32(b + 4);
      if (canonname) {
        strcpy(canonname, nm);
      }
      return true;
    }
    if (c < 0) {
      hi = mid;
    } else {
      lo = mid + 1;
    }
  }
  return false;
}

/* The upper-case ASCII of a name. */
std::string upper_name(const char *name) {
  std::string s(name);

  for (char &c : s) {
    if (c >= 'a' && c <= 'z') {
      c -= 'a' - 'A';
    }
  }
  return s;
}

/*
 * Find a zone, or make it, as pg_tzset does.
 * 1. A name over TZ_STRLEN_MAX is none.
 * 2. Look in the table of zones found, by the upper-case name.
 * 3. "GMT" is the tzparse definition, never a file: it cannot fail.
 * 4. Else load the zone from the pack; if there is none, the name may be a
 *    POSIX spec (but not one that starts with a colon).
 * 5. Keep it in the table.
 */
ty_tz_t *tzset_find(const char *tzname) {
  if (strlen(tzname) > TZ_STRLEN_MAX) {
    return nullptr;
  }
  std::string up = upper_name(tzname);
  std::lock_guard<std::mutex> lock(g_mu);
  auto it = g_cache.find(up);

  if (it != g_cache.end()) {
    return it->second;
  }
  ty_tz_t *tz = new ty_tz_t;
  char canonname[TZ_STRLEN_MAX + 1];

  if (up == "GMT") {
    if (!tzparse(up.c_str(), &tz->state, true)) {
      delete tz; /* this really, really should not happen */
      return nullptr;
    }
    strcpy(canonname, up.c_str());
  } else if (tzload(up.c_str(), canonname, &tz->state, true) != 0) {
    if (up[0] == ':' || !tzparse(up.c_str(), &tz->state, false)) {
      delete tz; /* an unknown zone */
      return nullptr;
    }
    /* For a POSIX spec, the upper-case name is the canonical one. */
    strcpy(canonname, up.c_str());
  }
  strcpy(tz->TZname, canonname);
  g_cache.emplace(up, tz);
  g_zone_found = true;
  return tz;
}

/* A zone of one offset, west of Greenwich (pg_tzset_offset). */
ty_tz_t *tzset_offset(long gmtoffset) {
  long absoffset = (gmtoffset < 0) ? -gmtoffset : gmtoffset;
  char offsetstr[32];
  char tzname[128];

  snprintf(offsetstr, sizeof(offsetstr), "%02ld", absoffset / SECSPERHOUR);
  absoffset %= SECSPERHOUR;
  if (absoffset != 0) {
    snprintf(offsetstr + strlen(offsetstr),
             sizeof(offsetstr) - strlen(offsetstr), ":%02ld",
             absoffset / SECSPERMIN);
    absoffset %= SECSPERMIN;
    if (absoffset != 0) {
      snprintf(offsetstr + strlen(offsetstr),
               sizeof(offsetstr) - strlen(offsetstr), ":%02ld", absoffset);
    }
  }
  if (gmtoffset > 0) {
    snprintf(tzname, sizeof(tzname), "<-%s>+%s", offsetstr, offsetstr);
  } else {
    snprintf(tzname, sizeof(tzname), "<+%s>-%s", offsetstr, offsetstr);
  }
  return tzset_find(tzname);
}

/* from timestamp.h: the range of dates for the Julian day routines */
#define JULIAN_MINYEAR (-4713)
#define JULIAN_MINMONTH 11
#define JULIAN_MAXYEAR 5874898
#define JULIAN_MAXMONTH 6
#define IS_VALID_JULIAN(y, m, d)                                               \
  (((y) > JULIAN_MINYEAR ||                                                    \
    ((y) == JULIAN_MINYEAR && ((m) >= JULIAN_MINMONTH))) &&                    \
   ((y) < JULIAN_MAXYEAR ||                                                    \
    ((y) == JULIAN_MAXYEAR && ((m) < JULIAN_MAXMONTH))))
#define UNIX_EPOCH_JDATE 2440588
#define SECS_PER_DAY 86400

/*
 * DetermineTimeZoneOffset of datetime.c: the offset, west of Greenwich, of
 * the zone at the local time, with the instant it gives into *tp, and
 * whether it is daylight time into *isdst.
 * 1. Take the local time as UTC: seconds since 1970. If that overflows, the
 *    date is out of range, and the zone is taken as GMT.
 * 2. Find the DST boundary just before or following that time: back up 24
 *    hours, and find the next one (no zone has an offset of 24 hours, and
 *    no two boundaries are 48 hours apart).
 * 3. With no boundary, there is one offset.
 * 4. With one, form the instant by the offset before it and by the offset
 *    after it. If both are before the boundary, or both after, that is the
 *    answer.
 * 5. Else the time falls in a gap or a fold: in a spring-forward gap, the
 *    offset before; in a fall-back fold, the offset after.
 */
int determine_offset(const ty_tm_t *tm, ty_tz_t *tzp, int *isdst,
                     pg_time_t *tp) {
  int date, sec;
  pg_time_t day, mytime, prevtime, boundary, beforetime, aftertime;
  long int before_gmtoff, after_gmtoff;
  int before_isdst, after_isdst;
  int res;

  if (!IS_VALID_JULIAN(tm->year, tm->mon, tm->mday))
    goto overflow;
  date = ty_date2j(tm->year, tm->mon, tm->mday) - UNIX_EPOCH_JDATE;

  day = ((pg_time_t)date) * SECS_PER_DAY;
  if (day / SECS_PER_DAY != date)
    goto overflow;
  sec = tm->sec + (tm->min + tm->hour * MINSPERHOUR) * SECSPERMIN;
  mytime = day + sec;
  /* since sec >= 0, overflow could only be from +day to -mytime */
  if (mytime < 0 && day > 0)
    goto overflow;

  prevtime = mytime - SECS_PER_DAY;
  if (mytime < 0 && prevtime > 0)
    goto overflow;

  res = pg_next_dst_boundary(&prevtime, &before_gmtoff, &before_isdst,
                             &boundary, &after_gmtoff, &after_isdst, tzp);
  if (res < 0)
    goto overflow; /* failure? */

  if (res == 0) {
    /* Non-DST zone, life is simple */
    *isdst = before_isdst;
    *tp = mytime - before_gmtoff;
    return -(int)before_gmtoff;
  }

  beforetime = mytime - before_gmtoff;
  if ((before_gmtoff > 0 && mytime < 0 && beforetime > 0) ||
      (before_gmtoff <= 0 && mytime > 0 && beforetime < 0))
    goto overflow;
  aftertime = mytime - after_gmtoff;
  if ((after_gmtoff > 0 && mytime < 0 && aftertime > 0) ||
      (after_gmtoff <= 0 && mytime > 0 && aftertime < 0))
    goto overflow;

  /*
   * If both before or both after the boundary time, we know what to do. The
   * boundary time itself is considered to be after the transition, which
   * means we can accept aftertime == boundary in the second case.
   */
  if (beforetime < boundary && aftertime < boundary) {
    *isdst = before_isdst;
    *tp = beforetime;
    return -(int)before_gmtoff;
  }
  if (beforetime > boundary && aftertime >= boundary) {
    *isdst = after_isdst;
    *tp = aftertime;
    return -(int)after_gmtoff;
  }

  /*
   * It's an invalid or ambiguous time due to timezone transition.  In a
   * spring-forward transition, prefer the "before" interpretation; in a
   * fall-back transition, prefer "after".
   */
  if (beforetime > aftertime) {
    *isdst = before_isdst;
    *tp = beforetime;
    return -(int)before_gmtoff;
  }
  *isdst = after_isdst;
  *tp = aftertime;
  return -(int)after_gmtoff;

overflow:
  /* Given date is out of range, so assume UTC */
  *isdst = 0;
  *tp = 0;
  return 0;
}

/* The abbreviation in upper case, as pg_interpret_timezone_abbrev wants. */
void upper_abbr(const char *abbr, char *out) {
  size_t n = strlen(abbr);

  if (n > TZ_STRLEN_MAX) {
    n = TZ_STRLEN_MAX;
  }
  for (size_t i = 0; i < n; i++) {
    char c = abbr[i];

    out[i] = (c >= 'a' && c <= 'z') ? (char)(c - ('a' - 'A')) : c;
  }
  out[n] = '\0';
}

/* DetermineTimeZoneAbbrevOffsetInternal of datetime.c: see ty_timezone.h. */
bool abbrev_at(pg_time_t t, const char *abbr, ty_tz_t *tzp, int *offset,
               int *isdst) {
  char upabbr[TZ_STRLEN_MAX + 1];
  long int gmtoff;

  upper_abbr(abbr, upabbr);
  /* Look up the abbrev's meaning at this time in this zone */
  if (pg_interpret_timezone_abbrev(upabbr, &t, &gmtoff, isdst, tzp)) {
    /* Change sign to agree with DetermineTimeZoneOffset() */
    *offset = (int)-gmtoff;
    return true;
  }
  return false;
}

} // namespace

/* The calls of ty_timezone.h. */

/*
 * A lookup by name takes a lock; most hosts ask for the same name again and
 * again, so each thread keeps its last answer, by the exact text.
 */
const ty_tz_t *ty_tz_find(const char *name) {
  static thread_local char last_name[TZ_STRLEN_MAX + 1];
  static thread_local const ty_tz_t *last_tz = nullptr;

  if (last_tz != nullptr && strcmp(last_name, name) == 0) {
    return last_tz;
  }
  const ty_tz_t *tz = tzset_find(name);

  if (tz != nullptr && strlen(name) <= TZ_STRLEN_MAX) {
    strcpy(last_name, name);
    last_tz = tz;
  }
  return tz;
}

const ty_tz_t *ty_tz_from_offset(long west) { return tzset_offset(west); }

const char *ty_tz_name(const ty_tz_t *tz) { return tz->TZname; }

/* pg_localtime and the fields as ty keeps them. */
int ty_tz_localtime(const ty_tz_t *tz, int64_t t, ty_tz_local_t *out) {
  struct pg_tm *tm = pg_localtime(&t, tz);

  if (tm == nullptr) {
    return ty_internal::set_error(TY_ERR_RANGE, "timestamp out of range");
  }
  out->year = tm->tm_year + TM_YEAR_BASE;
  out->mon = tm->tm_mon + 1;
  out->mday = tm->tm_mday;
  out->hour = tm->tm_hour;
  out->min = tm->tm_min;
  out->sec = tm->tm_sec;
  out->wday = tm->tm_wday;
  out->yday = tm->tm_yday;
  out->isdst = tm->tm_isdst;
  out->gmtoff = (int32_t)tm->tm_gmtoff;
  out->abbrev = tm->tm_zone;
  return 0;
}

int ty_tz_offset_for_local(const ty_tz_t *tz, const ty_tm_t *tm, int *isdst,
                           int64_t *utc) {
  pg_time_t t;
  int dst;
  int off = determine_offset(tm, const_cast<ty_tz_t *>(tz), &dst, &t);

  if (isdst) {
    *isdst = dst;
  }
  if (utc) {
    *utc = t;
  }
  return off;
}

int ty_tz_abbrev_offset(const ty_tz_t *tz, const ty_tm_t *tm, const char *abbr,
                        int *isdst) {
  pg_time_t t;
  int abbr_offset, abbr_isdst, dst;
  int zone_offset = determine_offset(tm, const_cast<ty_tz_t *>(tz), &dst, &t);

  /* Try to match the abbreviation to something in the zone definition. */
  if (abbrev_at(t, abbr, const_cast<ty_tz_t *>(tz), &abbr_offset,
                &abbr_isdst)) {
    *isdst = abbr_isdst;
    return abbr_offset;
  }
  *isdst = dst;
  return zone_offset;
}

int ty_tz_abbrev_at(const ty_tz_t *tz, int64_t t, const char *abbr,
                    int *offset_west, int *isdst) {
  return abbrev_at(t, abbr, const_cast<ty_tz_t *>(tz), offset_west, isdst);
}

int ty_tz_abbrev_is_known(const ty_tz_t *tz, const char *abbr, int *fixed,
                          int *offset_west, int *isdst) {
  char upabbr[TZ_STRLEN_MAX + 1];
  long int gmtoff;
  bool isfixed;

  upper_abbr(abbr, upabbr);
  if (pg_timezone_abbrev_is_known(upabbr, &isfixed, &gmtoff, isdst, tz)) {
    *fixed = isfixed;
    *offset_west = (int)-gmtoff;
    return 1;
  }
  return 0;
}

int ty_tz_fixed_offset(const ty_tz_t *tz, long *east) {
  return pg_get_timezone_offset(tz, east);
}

static_assert(sizeof(struct pg_tm) == sizeof(ty_tz_pgtm_t),
              "ty_tz_pgtm_t is pg_tm");

const ty_tz_pgtm_t *ty_tz_localtime_pg(const ty_tz_t *tz, const int64_t *t) {
  return reinterpret_cast<const ty_tz_pgtm_t *>(pg_localtime(t, tz));
}

/* pg_get_next_timezone_abbrev of localtime.c: see ty_timezone.h. */
const char *ty_tz_next_abbrev(const ty_tz_t *tz, int *indx) {
  const struct state *sp = &tz->state;
  const char *abbrs = sp->chars;
  int abbrind = *indx;

  /* If we're still in range, the result is the current abbrev. */
  if (abbrind < 0 || abbrind >= sp->charcnt)
    return NULL;
  const char *result = abbrs + abbrind;

  /* Advance *indx past this abbrev and its trailing null. */
  while (abbrs[abbrind] != '\0')
    abbrind++;
  abbrind++;
  *indx = abbrind;
  return result;
}

/*
 * timestamp2tm with a zone: see ty_timezone.h.
 * 1. Split the instant into a date and a time of day, as PostgreSQL does; a
 *    date that is no Julian day is out of range.
 * 2. Rotate to the zone with pg_localtime, on the seconds since 1970.
 * (PostgreSQL also treats an instant that does not fit a 32-bit pg_time_t
 * as UTC: here pg_time_t has 64 bits, and every timestamp fits.)
 */
int ty_timestamptz_to_local(const ty_tz_t *tz, int64_t ts, ty_tm_t *tm,
                            int32_t *fsec, int *west, int *isdst,
                            const char **abbrev) {
  int64_t date, time = ts;
  const int64_t usecs_per_day = INT64_C(86400000000);

  date = time / usecs_per_day;
  time -= date * usecs_per_day;
  if (time < 0) {
    time += usecs_per_day;
    date -= 1;
  }
  /* add offset to go from J2000 back to standard Julian date */
  date += TY_POSTGRES_EPOCH_JDATE;
  /* Julian day routine does not work for negative Julian days */
  if (date < 0 || date > (int64_t)INT_MAX) {
    return ty_internal::set_error(TY_ERR_RANGE, "timestamp out of range");
  }
  int32_t f = (int32_t)(time % 1000000);
  pg_time_t utime =
      (ts - f) / 1000000 +
      (int64_t)(TY_POSTGRES_EPOCH_JDATE - UNIX_EPOCH_JDATE) * SECS_PER_DAY;
  struct pg_tm *tx = pg_localtime(&utime, tz);

  if (tx == nullptr) {
    return ty_internal::set_error(TY_ERR_RANGE, "timestamp out of range");
  }
  tm->year = tx->tm_year + TM_YEAR_BASE;
  tm->mon = tx->tm_mon + 1;
  tm->mday = tx->tm_mday;
  tm->hour = tx->tm_hour;
  tm->min = tx->tm_min;
  tm->sec = tx->tm_sec;
  *fsec = f;
  *west = -(int)tx->tm_gmtoff;
  if (isdst) {
    *isdst = tx->tm_isdst;
  }
  if (abbrev) {
    *abbrev = tx->tm_zone;
  }
  return 0;
}

/* timestamp2timestamptz_opt_overflow: see ty_timezone.h. */
int ty_timestamp_to_timestamptz(const ty_tz_t *tz, int64_t ts, int64_t *out,
                                int *overflow) {
  ty_tm_t tm;
  int32_t fsec;

  if (overflow) {
    *overflow = 0;
  }
  if (!ty_timestamp_finite(ts)) {
    *out = ts;
    return 0;
  }
  /* We don't expect this to fail, but check it pro forma */
  if (ty_timestamp_to_tm(ts, &tm, &fsec) == 0) {
    int isdst;
    int offset = ty_tz_offset_for_local(tz, &tm, &isdst, nullptr);
    int64_t result = ts + (int64_t)offset * 1000000;

    if (ty_timestamp_valid(result)) {
      *out = result;
      return 0;
    }
    if (overflow) {
      if (result < TY_MIN_TIMESTAMP) {
        *overflow = -1;
        *out = TY_TIMESTAMP_NOBEGIN;
      } else {
        *overflow = 1;
        *out = TY_TIMESTAMP_NOEND;
      }
      return 0;
    }
  }
  return ty_internal::set_error(TY_ERR_RANGE, "timestamp out of range");
}

/* timestamptz2timestamp: see ty_timezone.h. */
int ty_timestamptz_to_timestamp(const ty_tz_t *tz, int64_t ts, int64_t *out) {
  ty_tm_t tm;
  int32_t fsec;
  int west;

  if (!ty_timestamp_finite(ts)) {
    *out = ts;
    return 0;
  }
  if (ty_timestamptz_to_local(tz, ts, &tm, &fsec, &west, nullptr, nullptr) !=
          0 ||
      ty_tm_to_timestamp(&tm, fsec, out) != 0) {
    return ty_internal::set_error(TY_ERR_RANGE, "timestamp out of range");
  }
  return 0;
}

/* date2timestamptz_opt_overflow: see ty_timezone.h. */
int ty_date_to_timestamptz(const ty_tz_t *tz, int32_t d, int64_t *out,
                           int *overflow) {
  int64_t midnight;
  int over = 0;

  if (overflow) {
    *overflow = 0;
  }
  if (ty_date_to_timestamp(d, &midnight, &over) != 0) {
    return -1; /* the error is "date out of range for timestamp" */
  }
  if (!ty_date_finite(d) || over != 0) {
    if (over != 0 && overflow == nullptr) {
      return ty_date_to_ts_error(); /* "date out of range for timestamp" */
    }
    if (overflow) {
      *overflow = over;
    }
    *out = midnight;
    return 0;
  }
  ty_tm_t tm = {};
  int isdst;

  ty_j2date(d + TY_POSTGRES_EPOCH_JDATE, &tm.year, &tm.mon, &tm.mday);
  int offset = ty_tz_offset_for_local(tz, &tm, &isdst, nullptr);
  int64_t result = midnight + (int64_t)offset * 1000000;

  /*
   * Since it is possible to go beyond allowed timestamptz range because of
   * time zone, check for allowed timestamp range after adding tz.
   */
  if (!ty_timestamp_valid(result)) {
    if (overflow) {
      if (result < TY_MIN_TIMESTAMP) {
        *overflow = -1;
        *out = TY_TIMESTAMP_NOBEGIN;
      } else {
        *overflow = 1;
        *out = TY_TIMESTAMP_NOEND;
      }
      return 0;
    }
    return ty_internal::set_error(TY_ERR_RANGE,
                                  "date out of range for timestamp");
  }
  *out = result;
  return 0;
}

/* timestamptz_date: see ty_timezone.h. */
int ty_timestamptz_to_date(const ty_tz_t *tz, int64_t ts, int32_t *out) {
  ty_tm_t tm;
  int32_t fsec;
  int west;

  if (ts == TY_TIMESTAMP_NOBEGIN) {
    *out = TY_DATE_NOBEGIN;
    return 0;
  }
  if (ts == TY_TIMESTAMP_NOEND) {
    *out = TY_DATE_NOEND;
    return 0;
  }
  if (ty_timestamptz_to_local(tz, ts, &tm, &fsec, &west, nullptr, nullptr) !=
      0) {
    return -1;
  }
  *out = ty_date2j(tm.year, tm.mon, tm.mday) - TY_POSTGRES_EPOCH_JDATE;
  return 0;
}

int ty_tz_file(const char *name, char *canonname, const unsigned char **data,
               size_t *size) {
  return pack_find(name, canonname, data, size);
}

const char *ty_tz_data_version(void) {
  const pack_t *pk = pack();

  return pk ? pk->release : "";
}

unsigned ty_tz_name_count(void) {
  const pack_t *pk = pack();

  return pk ? pk->nnames : 0;
}

const char *ty_tz_name_at(unsigned i) {
  const pack_t *pk = pack();

  if (pk == nullptr || i >= pk->nnames) {
    return nullptr;
  }
  return (const char *)pk->base + be32(pk->names + 8 * (size_t)i);
}

int ty_tz_use_data(const void *data, size_t size) {
  static pack_t mine;
  std::lock_guard<std::mutex> lock(g_mu);

  if (g_zone_found ||
      !pack_check(static_cast<const unsigned char *>(data), size, &mine)) {
    return ty_internal::set_error(TY_ERR_INVALID, "not a pack of zones");
  }
  g_pack.store(&mine, std::memory_order_release);
  return 0;
}

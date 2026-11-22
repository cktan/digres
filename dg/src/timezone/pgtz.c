/*-------------------------------------------------------------------------
 *
 * pgtz.c
 *	  Timezone Library Integration Functions
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 2026, CK Tan: the changes for digres,
 * under the MIT License (https://github.com/cktan/digres/blob/main/LICENSE)
 *
 * IDENTIFICATION
 *	  src/timezone/pgtz.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <ctype.h>
#include <fcntl.h>
#include <time.h>

#include "common/file_utils.h"
#include "datatype/timestamp.h"
#include "miscadmin.h"
#include "pgtz.h"
#include "ty_timezone.h"
#include "storage/fd.h"
#include "utils/control_block.h"
#include "utils/hsearch.h"

/* Current session timezone (controlled by TimeZone GUC) */
thread_local /*LOCAL*/ pg_tz *session_timezone = NULL;

/*
 * We keep loaded timezones in a hashtable so we don't have to
 * load and parse the TZ definition file every time one is selected.
 * Because we want timezone names to be found case-insensitively,
 * the hash key is the uppercased name of the zone.
 */
typedef struct {
  /* tznameupper contains the all-upper-case name of the timezone */
  char tznameupper[TZ_STRLEN_MAX + 1];
  pg_tz tz;
} pg_tz_cache;

static thread_local /*LOCAL*/ HTAB *timezone_cache = NULL;

static bool init_timezone_hashtable(void) {
  HASHCTL hash_ctl;

  hash_ctl.keysize = TZ_STRLEN_MAX + 1;
  hash_ctl.entrysize = sizeof(pg_tz_cache);

  timezone_cache =
      hash_create("Timezones", 4, &hash_ctl, HASH_ELEM | HASH_STRINGS);
  if (!timezone_cache)
    return false;

  return true;
}

/*
 * Load a timezone from file or from cache.
 * Does not verify that the timezone is acceptable!
 *
 * "GMT" is always interpreted as the tzparse() definition, without attempting
 * to load a definition from the filesystem.  This has a number of benefits:
 * 1. It's guaranteed to succeed, so we don't have the failure mode wherein
 * the bootstrap default timezone setting doesn't work (as could happen if
 * the OS attempts to supply a leap-second-aware version of "GMT").
 * 2. Because we aren't accessing the filesystem, we can safely initialize
 * the "GMT" zone definition before my_exec_path is known.
 * 3. It's quick enough that we don't waste much time when the bootstrap
 * default timezone setting is later overridden from postgresql.conf.
 */
pg_tz *pg_tzset(const char *tzname) {
  pg_tz_cache *tzp;
  struct state tzstate;
  char uppername[TZ_STRLEN_MAX + 1];
  char canonname[TZ_STRLEN_MAX + 1];
  char *p;

  if (strlen(tzname) > TZ_STRLEN_MAX)
    return NULL; /* not going to fit */

  if (!timezone_cache)
    if (!init_timezone_hashtable())
      return NULL;

  /*
   * Upcase the given name to perform a case-insensitive hashtable search.
   * (We could alternatively downcase it, but we prefer upcase so that we
   * can get consistently upcased results from tzparse() in case the name is
   * a POSIX-style timezone spec.)
   */
  p = uppername;
  while (*tzname)
    *p++ = pg_toupper((unsigned char)*tzname++);
  *p = '\0';

  tzp = (pg_tz_cache *)hash_search(timezone_cache, uppername, HASH_FIND, NULL);
  if (tzp) {
    /* Timezone found in cache, nothing more to do */
    return &tzp->tz;
  }

  /*
   * "GMT" is always sent to tzparse(), as per discussion above.
   */
  if (strcmp(uppername, "GMT") == 0) {
    if (!tzparse(uppername, &tzstate, true)) {
      /* This really, really should not happen ... */
      elog(ERROR, "could not initialize GMT time zone");
    }
    /* Use uppercase name as canonical */
    strcpy(canonname, uppername);
  } else if (tzload(uppername, canonname, &tzstate, true) != 0) {
    if (uppername[0] == ':' || !tzparse(uppername, &tzstate, false)) {
      /* Unknown timezone. Fail our call instead of loading GMT! */
      return NULL;
    }
    /* For POSIX timezone specs, use uppercase name as canonical */
    strcpy(canonname, uppername);
  }

  /* Save timezone in the cache */
  tzp = (pg_tz_cache *)hash_search(timezone_cache, uppername, HASH_ENTER, NULL);

  /* hash_search already copied uppername into the hash key */
  strcpy(tzp->tz.TZname, canonname);
  memcpy(&tzp->tz.state, &tzstate, sizeof(tzstate));

  return &tzp->tz;
}

/*
 * Load a fixed-GMT-offset timezone.
 * This is used for SQL-spec SET TIME ZONE INTERVAL 'foo' cases.
 * It's otherwise equivalent to pg_tzset().
 *
 * The GMT offset is specified in seconds, positive values meaning west of
 * Greenwich (ie, POSIX not ISO sign convention).  However, we use ISO
 * sign convention in the displayable abbreviation for the zone.
 *
 * Caution: this can fail (return NULL) if the specified offset is outside
 * the range allowed by the zic library.
 */
pg_tz *pg_tzset_offset(long gmtoffset) {
  long absoffset = (gmtoffset < 0) ? -gmtoffset : gmtoffset;
  char offsetstr[64];
  char tzname[128];

  snprintf(offsetstr, sizeof(offsetstr), "%02ld", absoffset / SECS_PER_HOUR);
  absoffset %= SECS_PER_HOUR;
  if (absoffset != 0) {
    snprintf(offsetstr + strlen(offsetstr),
             sizeof(offsetstr) - strlen(offsetstr), ":%02ld",
             absoffset / SECS_PER_MINUTE);
    absoffset %= SECS_PER_MINUTE;
    if (absoffset != 0)
      snprintf(offsetstr + strlen(offsetstr),
               sizeof(offsetstr) - strlen(offsetstr), ":%02ld", absoffset);
  }
  if (gmtoffset > 0)
    snprintf(tzname, sizeof(tzname), "<-%s>+%s", offsetstr, offsetstr);
  else
    snprintf(tzname, sizeof(tzname), "<+%s>-%s", offsetstr, offsetstr);

  return pg_tzset(tzname);
}

/*
 * Initialize timezone library
 *
 * This is called before GUC variable initialization begins. Set the
 * process-wide log timezone on the main thread before configuration can
 * enable timestamps. Each thread also gets a valid session_timezone.
 */
void pg_timezone_initialize(void) {
  /*
   * We may not yet know where PGSHAREDIR is (in particular this is true in
   * an EXEC_BACKEND subprocess).  So use "GMT", which pg_tzset forces to be
   * interpreted without reference to the filesystem.  This corresponds to
   * the bootstrap default for these variables in guc_tables.c, although in
   * principle it could be different.
   */
  session_timezone = pg_tzset("GMT");
  if (postmaster_tcb != NULL)
    pcb_main()->log_timezone = session_timezone;
}

/*
 * Functions to enumerate available timezones
 *
 * Note that pg_tzenumerate_next() will return a pointer into the pg_tzenum
 * structure, so the data is only valid up to the next call.
 *
 * All data is allocated using palloc in the current context.
 */
/*
 * The zones are those of ty's data (ty_timezone.h), in the order of the
 * lower-case name; no directory is read.
 */
struct pg_tzenum {
  unsigned next; /* the index of the next name */
  struct pg_tz tz;
};

/* typedef pg_tzenum is declared in pgtime.h */

pg_tzenum *pg_tzenumerate_start(void) {
  return (pg_tzenum *)palloc0(sizeof(pg_tzenum));
}

void pg_tzenumerate_end(pg_tzenum *dir) { pfree(dir); }

pg_tz *pg_tzenumerate_next(pg_tzenum *dir) {
  while (dir->next < ty_tz_name_count()) {
    const char *name = ty_tz_name_at(dir->next++);

    /*
     * Load this timezone using tzload() not pg_tzset(), so we don't fill
     * the cache.  Also, don't ask for the canonical spelling: we already
     * know it.
     */
    if (tzload(name, NULL, &dir->tz.state, true) != 0) {
      /* Zone could not be loaded, ignore it */
      continue;
    }

    if (!pg_tz_acceptable(&dir->tz)) {
      /* Ignore leap-second zones */
      continue;
    }

    /* OK, return the canonical zone name spelling. */
    strlcpy(dir->tz.TZname, name, sizeof(dir->tz.TZname));

    /* Timezone loaded OK. */
    return &dir->tz;
  }

  /* Nothing more found */
  return NULL;
}

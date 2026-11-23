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

#include "miscadmin.h"
#include "pgtime.h"
#include "ty_timezone.h"
#include "utils/control_block.h"

/*
 * The zones are those of ty (ty_timezone.h): ty finds each, keeps it, and
 * knows the rules; this file keeps the setting of the session and the calls
 * as PostgreSQL's code makes them.
 */

/* Current session timezone (controlled by TimeZone GUC) */
thread_local /*LOCAL*/ pg_tz *session_timezone = NULL;

/*
 * Load a timezone, or find it in ty's table: a name in any case, or a POSIX
 * spec. "GMT" is always the tzparse() definition, never a file, so it cannot
 * fail, and the bootstrap default of the timezone setting works.
 */
pg_tz *pg_tzset(const char *tzname) {
  return (pg_tz *)ty_tz_find(tzname);
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
  return (pg_tz *)ty_tz_from_offset(gmtoffset);
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
   * "GMT" is interpreted without reference to the data of the zones. This
   * corresponds to the bootstrap default for these variables in
   * guc_tables.c, although in principle it could be different.
   */
  session_timezone = pg_tzset("GMT");
  /* ty reads the session's zone for the time of the transaction. */
  ty_set_session_zone(session_timezone);
  if (postmaster_tcb != NULL)
    pcb_main()->log_timezone = session_timezone;
}

/*
 * Functions to enumerate available timezones
 *
 * The zones are those of ty's data, in the order of the lower-case name. A
 * zone is a pg_tz of ty's table, good for the life of the process.
 */
struct pg_tzenum {
  unsigned next; /* the index of the next name */
};

/* typedef pg_tzenum is declared in pgtime.h */

pg_tzenum *pg_tzenumerate_start(void) {
  return (pg_tzenum *)palloc0(sizeof(pg_tzenum));
}

void pg_tzenumerate_end(pg_tzenum *dir) { pfree(dir); }

pg_tz *pg_tzenumerate_next(pg_tzenum *dir) {
  while (dir->next < ty_tz_name_count()) {
    pg_tz *tz = pg_tzset(ty_tz_name_at(dir->next++));

    if (tz != NULL)
      return tz;
  }
  return NULL;
}

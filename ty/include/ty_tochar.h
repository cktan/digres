/*
 * Copyright (c) 2026 CK Tan
 * SPDX-License-Identifier: MIT
 */

/*
 * ty_tochar.h -- to_char of timestamp, timestamptz and interval: a format
 * of keywords (YYYY, MM, HH24, Month, TZ, ...) with their prefixes and
 * suffixes (FM, TM, TH), and text. Written for ty, with the keywords, the
 * output and the errors of PostgreSQL's formatting.c, byte for byte.
 *
 * A host compiles a format once (ty_dch_compile) and formats each value by
 * it. The host's locale comes in a ty_tochar_locale_t: NULL is the C locale.
 */
#ifndef TY_TOCHAR_H
#define TY_TOCHAR_H

#include <stddef.h>
#include <stdint.h>

#include "ty.h"
#include "ty_interval.h"
#include "ty_timezone.h"
#include "ty_varlena.h"

/* A compiled format: its nodes, of the alloc function; ty_free frees it. */
typedef struct ty_dch_format_t ty_dch_format_t;

/*
 * Compile the len bytes at fmt, as PostgreSQL's parse_format does for a
 * date: a keyword is matched first, the longest of those that begin alike,
 * after a prefix FM or TM and before a suffix TH, th or SP; a "quoted"
 * string is text, with \ quoting the next character; anything else is a
 * character. Fails only with TY_ERR_NOMEM.
 */
int ty_dch_compile(const char *fmt, size_t len, ty_dch_format_t **out);

/* The names TM asks for. */
enum {
  TY_TOCHAR_FULL_MONTH = 0,
  TY_TOCHAR_ABBREV_MONTH = 1,
  TY_TOCHAR_FULL_DAY = 2,
  TY_TOCHAR_ABBREV_DAY = 3
};
/* The case of a name: MONTH, Month, month. */
enum { TY_TOCHAR_UPPER = 0, TY_TOCHAR_INITCAP = 1, TY_TOCHAR_LOWER = 2 };

/*
 * The host's locale. name gives the localized name (kind above) of month i
 * (0 for January) or day i (0 for Sunday), in case c, in *name and *len;
 * the host cases it under its collation, keeps it until the call returns,
 * and returns 0, or -1 with ty's error set.
 */
typedef struct ty_tochar_locale_t {
  void *ctx;
  int (*name)(void *ctx, int kind, int i, int c, const char **name,
              size_t *len);
} ty_tochar_locale_t;

/*
 * to_char of a timestamp, a timestamptz in the zone tz, and an interval, by
 * the compiled format f. An empty format or an infinite value has no result:
 * 1, and *out is NULL. Else 0 and the text in *out; or -1 with the error:
 * - TY_ERR_INVALID "invalid format specification for an interval value",
 *   for a field of a calendar date in an interval (PostgreSQL's hint is
 *   "Intervals are not tied to specific calendar dates.");
 * - TY_ERR_RANGE "time zone format value too long", "localized string
 *   format value too long", or "timestamp out of range";
 * - TY_ERR_NOMEM.
 */
int ty_timestamp_to_char(int64_t ts, const ty_dch_format_t *f,
                         const ty_tochar_locale_t *loc, ty_varlena_t **out);
int ty_timestamptz_to_char(int64_t ts, const ty_tz_t *tz,
                           const ty_dch_format_t *f,
                           const ty_tochar_locale_t *loc, ty_varlena_t **out);
int ty_interval_to_char(const ty_interval_t *iv, const ty_dch_format_t *f,
                        const ty_tochar_locale_t *loc, ty_varlena_t **out);

#endif /* TY_TOCHAR_H */

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

/*
 * to_char of a number: a format of 9, 0, the point and D, the comma and G,
 * the signs (S, MI, PL, SG, PR), L, B, FM, V, TH, RN and EEEE, as
 * PostgreSQL's NUM_processor.
 *
 * ty_num_compile reads a format and checks it, with PostgreSQL's errors,
 * all TY_ERR_INVALID ("multiple decimal points", "cannot use "S" twice",
 * ""EEEE" is incompatible with other formats" and the rest).
 */
typedef struct ty_num_format_t ty_num_format_t;
int ty_num_compile(const char *fmt, size_t len, ty_num_format_t **out);

/* The parts of a format a host needs to make a number's digits. */
typedef struct ty_num_desc_t {
  int pre;   /* digits before the point */
  int post;  /* digits after it */
  int multi; /* V's power of 10 */
  int roman; /* RN */
  int eeee;  /* EEEE */
} ty_num_desc_t;
void ty_num_describe(const ty_num_format_t *f, ty_num_desc_t *d);

/*
 * The host's locale, for D, G, L and S: its lconv strings, as they are
 * (ty takes PostgreSQL's defaults for those that are empty or NULL). symbols
 * is called only for a format that uses one.
 */
typedef struct ty_num_symbols_t {
  const char *negative_sign;
  const char *positive_sign;
  const char *decimal_point;
  const char *thousands_sep;
  const char *currency_symbol;
} ty_num_symbols_t;
typedef struct ty_num_locale_t {
  void *ctx;
  void (*symbols)(void *ctx, ty_num_symbols_t *out);
} ty_num_locale_t;

/*
 * to_char of an int4, an int8, a float4 and a float8. An empty format gives
 * an empty text. The errors: TY_ERR_RANGE "integer out of range" or "bigint
 * out of range" from V; TY_ERR_INVALID ""nan" is not a number" from TH; and
 * TY_ERR_NOMEM.
 */
int ty_int32_to_char(int32_t v, const ty_num_format_t *f,
                     const ty_num_locale_t *loc, ty_varlena_t **out);
int ty_int64_to_char(int64_t v, const ty_num_format_t *f,
                     const ty_num_locale_t *loc, ty_varlena_t **out);
int ty_float4_to_char(float v, const ty_num_format_t *f,
                      const ty_num_locale_t *loc, ty_varlena_t **out);
int ty_float8_to_char(double v, const ty_num_format_t *f,
                      const ty_num_locale_t *loc, ty_varlena_t **out);

/*
 * For a host that makes the digits itself, as digres does for a decimal:
 * the Roman numeral of n (15 # out of 1 to 3999) in buf, and the format
 * written by digits: numstr is the number without its sign ('+' or '-' in
 * sign), padded by out_pre_spaces, with pre and post as the host has
 * adjusted them.
 */
const char *ty_num_roman(int n, char buf[16]);
int ty_num_to_char_digits(const ty_num_format_t *f, const char *numstr,
                          int out_pre_spaces, int sign, int pre, int post,
                          const ty_num_locale_t *loc, ty_varlena_t **out);

#endif /* TY_TOCHAR_H */

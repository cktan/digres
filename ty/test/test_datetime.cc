/*
 * Copyright (c) 2026 CK Tan
 * SPDX-License-Identifier: MIT
 */

/*
 * Tests of the calendar and the date, time and timestamp arithmetic; the
 * rest are in the vectors.
 */
#include <string.h>

#include <memory>
#include <string>

#include "doctest.h"
#include "ty_datetime.h"
#include "ty_timezone.h"

// Verify the calendar at the epochs.
// 1. 2000-01-01 is Julian day 2451545, a Saturday (6).
// 2. 1970-01-01 is Julian day 2440588, a Thursday (4).
// 3. The Julian day converts back to the date.
TEST_CASE("the calendar at the epochs") {
  int y, m, d;

  // 1. 2000-01-01 is Julian day 2451545, a Saturday (6).
  CHECK(2451545 == ty_date2j(2000, 1, 1));
  CHECK(6 == ty_j2day(2451545));

  // 2. 1970-01-01 is Julian day 2440588, a Thursday (4).
  CHECK(2440588 == ty_date2j(1970, 1, 1));
  CHECK(4 == ty_j2day(2440588));

  // 3. The Julian day converts back to the date.
  ty_j2date(2451545, &y, &m, &d);
  CHECK(y == 2000);
  CHECK(m == 1);
  CHECK(d == 1);
}

// Verify that a month added to the 31st stops at the end of the month.
// 1. 2000-01-31 + 1 month is 2000-02-29 (a leap year).
// 2. 2001-01-31 + 1 month is 2001-02-28.
TEST_CASE("timestamp plus months stops at the end of the month") {
  ty_interval_t month = {0, 0, 1};
  ty_tm_t tm = {2000, 1, 31, 12, 0, 0};
  int64_t ts, r;
  int32_t f;

  // 1. 2000-01-31 + 1 month is 2000-02-29 (a leap year).
  REQUIRE(0 == ty_tm_to_timestamp(&tm, 0, &ts));
  REQUIRE(0 == ty_timestamp_add_interval(ts, &month, &r));
  REQUIRE(0 == ty_timestamp_to_tm(r, &tm, &f));
  CHECK(tm.mon == 2);
  CHECK(tm.mday == 29);

  // 2. 2001-01-31 + 1 month is 2001-02-28.
  tm = {2001, 1, 31, 12, 0, 0};
  REQUIRE(0 == ty_tm_to_timestamp(&tm, 0, &ts));
  REQUIRE(0 == ty_timestamp_add_interval(ts, &month, &r));
  REQUIRE(0 == ty_timestamp_to_tm(r, &tm, &f));
  CHECK(tm.mday == 28);
}

// Verify the errors.
// 1. make_date of month 13 is a field error; of year 5874899, a range error.
// 2. date + days past the end is a range error.
// 3. The infinities of a date have no difference.
TEST_CASE("date and time errors") {
  int32_t d;

  // 1. make_date of month 13 is a field error; of year 5874899, a range
  //    error.
  CHECK(-1 == ty_date_make(2000, 13, 1, &d));
  CHECK(TY_ERR_INVALID == ty_errnum());
  CHECK(std::string(ty_errmsg()) ==
        "date field value out of range: 2000-13-01");
  CHECK(-1 == ty_date_make(5874899, 1, 1, &d));
  CHECK(TY_ERR_RANGE == ty_errnum());

  // 2. date + days past the end is a range error.
  CHECK(0 == ty_date_make(5874897, 12, 31, &d));
  int32_t r;
  CHECK(-1 == ty_date_add_days(d, 1, &r));
  CHECK(std::string(ty_errmsg()) == "date out of range");

  // 3. The infinities of a date have no difference.
  CHECK(-1 == ty_date_diff(TY_DATE_NOEND, 0, &r));
  CHECK(std::string(ty_errmsg()) == "cannot subtract infinite dates");
}

// Verify that the epoch of a timestamp near the end of the range is exact:
// the old numeric division rounded it to 3 digits first.
// 1. Take the last timestamp, whose seconds since the Unix epoch need 13
// digits.
// 2. Check the integer seconds and the microseconds, apart.
// 3. Check the sum is the exact quotient.
TEST_CASE("timestamp epoch is exact at the end of the range") {
  ty_part_t p;
  int64_t ts = INT64_C(9223371331199999999);

  CHECK(0 == ty_timestamp_part(1, TY_UNIT_EPOCH, ts, &p));
  CHECK(TY_PART_SUM == p.kind);
  CHECK(INT64_C(9224318015999) == p.num2);
  CHECK(999999 == p.num);
}

// Verify that the fast path of the words extract is asked with says what the
// tables of words say.
// 1. For each common word, and its upper-case form, ask both ways.
// 2. Check the kind and the code are the same.
TEST_CASE("the common words of extract agree with the tables") {
  const char *words[] = {
      "year",       "month",        "day",          "hour",   "minute",
      "second",     "quarter",      "week",         "decade", "century",
      "millennium", "milliseconds", "microseconds", "epoch",  "dow",
      "doy",        "isodow",       "isoyear",      "julian", "timezone",
      "years",      "yearx",        "da",           "epochs", "",
      "e",          "now",          "today"};

  for (const char *w : words) {
    for (int epoch_only = 0; epoch_only < 2; epoch_only++) {
      int c1 = -1, c2 = -1;
      int r1 = ty_datetime_part_unit(w, epoch_only, &c1);
      int r2 = ty_datetime_part_unit_slow(w, epoch_only, &c2);

      INFO(w);
      CHECK(r1 == r2);
      if (r1 != 0) {
        CHECK(c1 == c2);
      }
    }
  }
}

namespace {

/* A host with a dynamic abbreviation "xst", a broken one "bad", and a clock. */
int hook_abbrev(void *, int, const char *w, int *type, int *offset,
                const void **tz, ty_dtin_extra_t *extra) {
  static int zone;

  *type = TY_ABBREV_NONE;
  *offset = 0;
  *tz = nullptr;
  if (strcmp(w, "xst") == 0) {
    *type = TY_ABBREV_DYNTZ;
    *tz = &zone;
  } else if (strcmp(w, "bad") == 0) {
    extra->timezone = "Nowhere/Land";
    extra->abbrev = "bad";
    return TY_DTERR_BAD_ZONE_ABBREV;
  } else if (strcmp(w, "pst") == 0) {
    *type = TY_ABBREV_TZ;
    *offset = -28800; /* seconds east, as the table of abbreviations has it */
  }
  return 0;
}

const void *hook_zone(void *, const char *name) {
  static int zone;

  return strcmp(name, "america/new_york") == 0 ? &zone : nullptr;
}

int hook_zone_fixed(void *, const void *, long *) { return 0; }

const ty_dtzone_t kHost = {nullptr,         hook_abbrev, hook_zone,
                           hook_zone_fixed, nullptr,     nullptr};

/*
 * The clock of the transaction these tests read: 2031-01-01 04:59:58.5 UTC,
 * which is 2030-12-31 23:59:58.5 in a zone 18000 seconds west of Greenwich,
 * the session's zone. Each test that reads it makes one, and the destructor
 * gives the thread no clock.
 */
struct txn_clock_t {
  int64_t value = INT64_C(978325198500000);

  txn_clock_t() {
    ty_set_xact_clock(&value);
    ty_set_session_zone(ty_tz_from_offset(18000));
  }
  ~txn_clock_t() {
    ty_set_xact_clock(nullptr);
    ty_set_session_zone(nullptr);
  }
};

} // namespace

// Verify the words that need the host: the clock, the zones, and no host.
// 1. With a clock, today, tomorrow, yesterday and now read it.
// 2. A dynamic abbreviation is accepted, and a daylight modifier is not.
// 3. A zone name the host does not know is a code, with the name kept.
// 4. A hook that fails gives its code and its strings.
// 5. Without hooks, no zone word and no clock word is known.
TEST_CASE("the text in of date and time asks the host for zones and clock") {
  int dterr;
  ty_dtin_extra_t x;
  int32_t d;
  int64_t t;
  int32_t today;
  std::unique_ptr<txn_clock_t> clock(new txn_clock_t());

  // 1. With a clock, today, tomorrow, yesterday and now read it.
  CHECK(0 == ty_date_make(2030, 12, 31, &today));
  CHECK(0 == ty_date_parse("today", TY_DATEORDER_MDY, &kHost, &d, &dterr, &x));
  CHECK(today == d);
  CHECK(0 ==
        ty_date_parse("tomorrow", TY_DATEORDER_MDY, &kHost, &d, &dterr, &x));
  CHECK(today + 1 == d);
  CHECK(0 ==
        ty_date_parse("yesterday", TY_DATEORDER_MDY, &kHost, &d, &dterr, &x));
  CHECK(today - 1 == d);
  CHECK(0 == ty_time_parse("now", TY_DATEORDER_MDY, &kHost, &t, &dterr, &x));
  CHECK(((23 * 60 + 59) * 60 + 58) * INT64_C(1000000) + 500000 == t);
  CHECK(0 ==
        ty_timestamp_parse("now", TY_DATEORDER_MDY, &kHost, &t, &dterr, &x));
  CHECK(INT64_C(86400000000) * today + 86398500000 == t);

  // 2. A dynamic abbreviation is accepted, and a daylight modifier is not.
  CHECK(0 == ty_timestamp_parse("2000-01-31 04:05 xst", TY_DATEORDER_MDY,
                                &kHost, &t, &dterr, &x));
  CHECK(0 ==
        ty_time_parse("04:05 xst", TY_DATEORDER_MDY, &kHost, &t, &dterr, &x));
  CHECK(-1 == ty_timestamp_parse("2000-01-31 04:05 xst dst", TY_DATEORDER_MDY,
                                 &kHost, &t, &dterr, &x));
  CHECK(TY_DTERR_BAD_FORMAT == dterr);

  // 3. A zone name the host does not know is a code, with the name kept.
  CHECK(0 == ty_timestamp_parse("2000-01-31 04:05 America/New_York",
                                TY_DATEORDER_MDY, &kHost, &t, &dterr, &x));
  CHECK(-1 == ty_timestamp_parse("Jan 31 2000 04:05 Nowhere/Land",
                                 TY_DATEORDER_MDY, &kHost, &t, &dterr, &x));
  CHECK(TY_DTERR_BAD_TIMEZONE == dterr);
  CHECK(TY_ERR_INVALID == ty_errnum());
  CHECK(std::string("nowhere/land") == x.timezone);

  // 4. A hook that fails gives its code and its strings.
  CHECK(-1 == ty_timestamp_parse("2000-01-31 04:05 bad", TY_DATEORDER_MDY,
                                 &kHost, &t, &dterr, &x));
  CHECK(TY_DTERR_BAD_ZONE_ABBREV == dterr);
  CHECK(std::string("Nowhere/Land") == x.timezone);
  CHECK(std::string("bad") == x.abbrev);

  // 5. Without hooks, no zone word is known; without a clock, no clock word.
  clock.reset();
  CHECK(0 == ty_timestamp_parse("2000-01-31 04:05", TY_DATEORDER_MDY, nullptr,
                                &t, &dterr, &x));
  CHECK(-1 == ty_timestamp_parse("2000-01-31 04:05 pst", TY_DATEORDER_MDY,
                                 nullptr, &t, &dterr, &x));
  CHECK(TY_DTERR_BAD_FORMAT == dterr);
  CHECK(-1 ==
        ty_date_parse("today", TY_DATEORDER_MDY, nullptr, &d, &dterr, &x));
  CHECK(TY_DTERR_BAD_FORMAT == dterr);
  CHECK(0 == ty_date_parse("epoch", TY_DATEORDER_MDY, nullptr, &d, &dterr, &x));
  CHECK(-10957 == d);
}

namespace {

/* What the offset hooks were asked, and what they answer. */
struct offset_log_t {
  int session_west = 7200; /* the session's zone, west of Greenwich */
  int zone_west = 18000;   /* "America/New_York" at the time asked */
  int dyn_west = 3600;     /* "xst" */
  ty_tm_t asked = {};      /* the time of the last question */
  const void *asked_tz = nullptr;
  std::string asked_abbrev;
  int calls = 0;
};

int off_offset(void *ctx, const ty_tm_t *tm, const void *tz) {
  offset_log_t *log = static_cast<offset_log_t *>(ctx);

  log->calls++;
  log->asked = *tm;
  log->asked_tz = tz;
  return tz == nullptr ? log->session_west : log->zone_west;
}

int off_abbrev_offset(void *ctx, const ty_tm_t *tm, const char *abbrev,
                      const void *tz) {
  offset_log_t *log = static_cast<offset_log_t *>(ctx);

  log->calls++;
  log->asked = *tm;
  log->asked_tz = tz;
  log->asked_abbrev = abbrev;
  return log->dyn_west;
}

int off_zone_fixed(void *, const void *, long *) { return 0; }

} // namespace

// Verify that the zone of timestamptz and timetz is asked of the host.
// 1. A named zone: the hook gets the zone and the local time, and the
//    instant is the local time plus its offset.
// 2. No zone in the text: the session's zone, with a NULL zone.
// 3. A zone of the text as an offset or an abbreviation needs no hook.
// 4. A dynamic abbreviation: the hook gets the word and its zone.
// 5. A timetz with a zone that has a daylight time and no date is bad;
//    with a dynamic abbreviation, it is asked with no date.
// 6. With no offset hook, the zone of the text only, and UTC otherwise.
TEST_CASE("the text in of timestamptz and timetz asks the host for offsets") {
  offset_log_t log;
  ty_dtzone_t host = {&log,           hook_abbrev, hook_zone,
                      off_zone_fixed, off_offset,  off_abbrev_offset};
  int dterr;
  ty_dtin_extra_t x;
  int64_t t, local;
  ty_timetz_t tt;

  CHECK(0 == ty_timestamp_parse("2000-01-31 04:05", TY_DATEORDER_MDY, &host,
                                &local, &dterr, &x));

  // 1. A named zone: the hook gets the zone and the local time.
  CHECK(0 == ty_timestamptz_parse("2000-01-31 04:05 America/New_York",
                                  TY_DATEORDER_MDY, &host, &t, &dterr, &x));
  CHECK(local + 18000 * INT64_C(1000000) == t);
  CHECK(nullptr != log.asked_tz);
  CHECK(2000 == log.asked.year);
  CHECK(31 == log.asked.mday);
  CHECK(4 == log.asked.hour);
  CHECK(5 == log.asked.min);

  // 2. No zone in the text: the session's zone, with a NULL zone.
  log.calls = 0;
  CHECK(0 == ty_timestamptz_parse("2000-01-31 04:05", TY_DATEORDER_MDY, &host,
                                  &t, &dterr, &x));
  CHECK(local + 7200 * INT64_C(1000000) == t);
  CHECK(nullptr == log.asked_tz);
  CHECK(1 == log.calls);

  // 3. A zone of the text as an offset or an abbreviation needs no hook.
  log.calls = 0;
  CHECK(0 == ty_timestamptz_parse("2000-01-31 04:05 +02", TY_DATEORDER_MDY,
                                  &host, &t, &dterr, &x));
  CHECK(local - 7200 * INT64_C(1000000) == t);
  CHECK(0 == ty_timestamptz_parse("2000-01-31 04:05 pst", TY_DATEORDER_MDY,
                                  &host, &t, &dterr, &x));
  CHECK(local + 28800 * INT64_C(1000000) == t);
  CHECK(0 == log.calls);

  // 4. A dynamic abbreviation: the hook gets the word and its zone.
  CHECK(0 == ty_timestamptz_parse("2000-01-31 04:05 xst", TY_DATEORDER_MDY,
                                  &host, &t, &dterr, &x));
  CHECK(local + 3600 * INT64_C(1000000) == t);
  CHECK(std::string("xst") == log.asked_abbrev);
  CHECK(nullptr != log.asked_tz);

  // 5. A timetz with a daylight zone and no date is bad; with a dynamic
  //    abbreviation, it is asked with no date.
  CHECK(-1 == ty_timetz_parse("04:05 America/New_York", TY_DATEORDER_MDY, &host,
                              &tt, &dterr, &x));
  CHECK(TY_DTERR_BAD_FORMAT == dterr);
  CHECK(0 == ty_timetz_parse("2000-01-31 04:05 America/New_York",
                             TY_DATEORDER_MDY, &host, &tt, &dterr, &x));
  CHECK(18000 == tt.zone);
  CHECK(0 ==
        ty_timetz_parse("04:05 xst", TY_DATEORDER_MDY, &host, &tt, &dterr, &x));
  CHECK(3600 == tt.zone);
  CHECK(0 == log.asked.mday);
  CHECK(4 == log.asked.hour);
  CHECK(0 ==
        ty_timetz_parse("04:05", TY_DATEORDER_MDY, &host, &tt, &dterr, &x));
  CHECK(7200 == tt.zone);

  // 6. With no offset hook, the zone of the text only, and UTC otherwise.
  ty_dtzone_t bare = host;
  bare.offset = nullptr;
  bare.abbrev_offset = nullptr;
  CHECK(0 == ty_timestamptz_parse("2000-01-31 04:05", TY_DATEORDER_MDY, &bare,
                                  &t, &dterr, &x));
  CHECK(local == t);
  CHECK(0 == ty_timestamptz_parse("2000-01-31 04:05 +02", TY_DATEORDER_MDY,
                                  &bare, &t, &dterr, &x));
  CHECK(local - 7200 * INT64_C(1000000) == t);
}

// Verify ty_timestamp_make: the value, the messages of make_timestamp (which
// words the year as given, as make_date does not), and the edges of the range.
// 1. A timestamp is the date and the time put together, over a grid of dates
//    (BC included) and times.
// 2. The edges of the range: the last microsecond is good, the next is not.
// 3. A field out of its range is TY_ERR_INVALID, and the message has the year
//    as given, for BC too.
// 4. A date or a timestamp that does not fit is TY_ERR_RANGE.
TEST_CASE("ty_timestamp_make puts the fields together, with its messages") {
  int64_t ts, tm;
  int32_t d;

  // 1. The date and the time put together.
  const int32_t years[] = {-4712, -100, -1, 1, 1999, 2000, 2024, 5000};
  const int32_t times[][3] = {{0, 0, 0}, {12, 34, 56}, {23, 59, 59}, {24, 0, 0}};
  for (int32_t y : years) {
    for (int32_t m : {1, 2, 12}) {
      for (const auto &t : times) {
        if (0 != ty_date_make(y, m, 28, &d)) {
          continue;
        }
        /* 24:00:00 is a time, and 24:00:00.789012 is not */
        double sec = t[2] + (t[0] == 24 ? 0.0 : 0.789012);
        REQUIRE(0 == ty_time_make(t[0], t[1], sec, &tm));
        REQUIRE(0 == ty_timestamp_make(y, m, 28, t[0], t[1], sec, &ts));
        CHECK(ts == (int64_t)d * INT64_C(86400000000) + tm);
      }
    }
  }
  REQUIRE(0 == ty_timestamp_make(2024, 7, 4, 12, 34, 56.789012, &ts));
  CHECK(INT64_C(773411696789012) == ts);

  // 2. The edges of the range.
  REQUIRE(0 == ty_timestamp_make(294276, 12, 31, 23, 59, 59.999999, &ts));
  CHECK(ty_timestamp_valid(ts));
  CHECK(-1 == ty_timestamp_make(294276, 12, 31, 24, 0, 0, &ts));
  CHECK(TY_ERR_RANGE == ty_errnum());
  CHECK(std::string("timestamp out of range: 294276-12-31 24:00:00") ==
        ty_errmsg());
  CHECK(-1 == ty_timestamp_make(294277, 1, 1, 0, 0, 0, &ts));
  CHECK(std::string("timestamp out of range: 294277-01-01 0:00:00") ==
        ty_errmsg());

  // 3. A field out of its range.
  CHECK(-1 == ty_timestamp_make(0, 1, 1, 0, 0, 0, &ts));
  CHECK(TY_ERR_INVALID == ty_errnum());
  CHECK(std::string("date field value out of range: 0-01-01") == ty_errmsg());
  CHECK(-1 == ty_timestamp_make(-5, 13, 1, 0, 0, 0, &ts));
  CHECK(std::string("date field value out of range: -5-13-01") == ty_errmsg());
  CHECK(-1 == ty_timestamp_make(2023, 2, 29, 0, 0, 0, &ts));
  CHECK(std::string("date field value out of range: 2023-02-29") == ty_errmsg());
  CHECK(-1 == ty_timestamp_make(2024, 1, 1, 25, 0, 0, &ts));
  CHECK(TY_ERR_INVALID == ty_errnum());
  CHECK(std::string("time field value out of range: 25:00:00") == ty_errmsg());
  CHECK(-1 == ty_timestamp_make(2024, 1, 1, 0, 0, 60.5, &ts));
  CHECK(std::string("time field value out of range: 0:00:60.5") == ty_errmsg());

  // 4. A date, or a timestamp, that does not fit, with the year as given: the
  //    answers are those of make_timestamp before the move.
  CHECK(0 == ty_timestamp_make(-4714, 11, 24, 0, 0, 0, &ts));
  CHECK(-1 == ty_timestamp_make(-4714, 11, 23, 0, 0, 0, &ts));
  CHECK(TY_ERR_RANGE == ty_errnum());
  CHECK(std::string("timestamp out of range: -4714-11-23 0:00:00") ==
        ty_errmsg());
  CHECK(-1 == ty_timestamp_make(-4714, 1, 1, 0, 0, 0, &ts));
  CHECK(TY_ERR_RANGE == ty_errnum());
  CHECK(std::string("date out of range: -4714-01-01") == ty_errmsg());
  CHECK(-1 == ty_timestamp_make(5874898, 1, 1, 0, 0, 0, &ts));
  CHECK(std::string("timestamp out of range: 5874898-01-01 0:00:00") ==
        ty_errmsg());
  CHECK(-1 == ty_timestamp_make(5874899, 1, 1, 0, 0, 0, &ts));
  CHECK(TY_ERR_RANGE == ty_errnum());
  CHECK(std::string("date out of range: 5874899-01-01") == ty_errmsg());
  CHECK(-1 == ty_timestamp_make(INT32_MIN, 1, 1, 0, 0, 0, &ts));
  CHECK(TY_ERR_INVALID == ty_errnum());
  CHECK(std::string("date field value out of range: -2147483648-01-01") ==
        ty_errmsg());
}

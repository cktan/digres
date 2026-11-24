/*
 * Copyright (c) 2026 CK Tan
 * SPDX-License-Identifier: MIT
 */

/*
 * Tests of to_char of timestamp, timestamptz and interval (ty_tochar.h):
 * the text PostgreSQL printed (test/tochar_known.txt, by gen_tochar.py),
 * and the errors, the locale and the edges by hand.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>
#include <vector>

#include "doctest.h"
#include "ty_datetime.h"
#include "ty_tochar.h"
#include "vl.h"

namespace {

/* The bytes of hex digits, "-" for none. */
std::string unhex(const std::string &h) {
  std::string s;

  if ("-" == h) {
    return s;
  }
  for (size_t i = 0; i + 1 < h.size(); i += 2) {
    s.push_back((char)strtol(h.substr(i, 2).c_str(), nullptr, 16));
  }
  return s;
}

/* A compiled format, freed at the end of its scope. */
struct fmt_t {
  ty_dch_format_t *f = nullptr;
  explicit fmt_t(const std::string &s) {
    REQUIRE(0 == ty_dch_compile(s.data(), s.size(), &f));
  }
  ~fmt_t() { ty_free(f); }
};

/* to_char of a timestamp: the text, "NULL", or "ERR " and ty's message. */
std::string ts_char(int64_t ts, const std::string &fmt,
                    const ty_tochar_locale_t *loc = nullptr) {
  fmt_t f(fmt);
  vl_out_t r;
  int rc = ty_timestamp_to_char(ts, f.f, loc, &r);

  return 1 == rc   ? "NULL"
         : 0 == rc ? r.str()
                   : "ERR " + std::string(ty_errmsg());
}

/* to_char of an interval of text iv. */
std::string iv_char(const std::string &iv, const std::string &fmt) {
  ty_interval_t v;
  int dterr;
  fmt_t f(fmt);
  vl_out_t r;

  REQUIRE(0 == ty_interval_parse(iv.c_str(), TY_INTERVAL_FULL_RANGE,
                                 TY_INTSTYLE_POSTGRES, &v, &dterr));
  int rc = ty_interval_to_char(&v, f.f, nullptr, &r);

  return 1 == rc   ? "NULL"
         : 0 == rc ? r.str()
                   : "ERR " + std::string(ty_errmsg());
}

/* A host's locale for the test: French names, upper, initcap or lower. */
int french(void *ctx, int kind, int i, int c, const char **name, size_t *len) {
  static const char *const months[] = {"janvier", "février", "mars"};
  static std::string out;
  std::string s = TY_TOCHAR_FULL_MONTH == kind ? months[i % 3]
                  : TY_TOCHAR_ABBREV_MONTH == kind
                      ? std::string(months[i % 3]).substr(0, 4) + "."
                  : TY_TOCHAR_FULL_DAY == kind ? "dimanche"
                                               : "dim.";

  (void)ctx;
  if (TY_TOCHAR_UPPER == c) {
    for (char &ch : s) {
      ch = (char)toupper((unsigned char)ch);
    }
  } else if (TY_TOCHAR_INITCAP == c) {
    s[0] = (char)toupper((unsigned char)s[0]);
  }
  out = s;
  *name = out.data();
  *len = out.size();
  return 0;
}

/* A host whose name is too long for TM's room. */
int too_long(void *, int, int, int, const char **name, size_t *len) {
  static const std::string s(61, 'x');

  *name = s.data();
  *len = s.size();
  return 0;
}

} // namespace

// Verify every line PostgreSQL printed for to_char, byte for byte.
// 1. Read test/tochar_known.txt: timestamps, timestamptz in 11 zones, and
//    intervals, each by keywords with each prefix and suffix, and mixed
//    formats.
// 2. Run each through ty, and compare the text.
TEST_CASE("to_char prints what PostgreSQL printed") {
  const char *env = getenv("TY_VECTORS");
  std::string dir = env ? env : "test/vectors.txt";
  std::string path = dir.substr(0, dir.rfind('/') + 1) + "tochar_known.txt";
  FILE *fp = fopen(path.c_str(), "r");
  char line[8192];
  int n = 0, bad = 0;

  REQUIRE(nullptr != fp);

  // 1. Read test/tochar_known.txt: timestamps, timestamptz in 11 zones, and
  //    intervals, each by keywords with each prefix and suffix, and mixed
  //    formats.
  while (fgets(line, sizeof line, fp)) {
    char kind[16], a[128], b[64], fmt[4096], want[4096];
    std::string got;

    if (0 == strncmp(line, "tochar.tz ", 10)) {
      if (5 != sscanf(line, "%15s %127s %63s %4095s => %4095s", kind, a, b, fmt,
                      want)) {
        continue;
      }
      // 2. Run each through ty, and compare the text.
      const ty_tz_t *tz = ty_tz_find(a);
      fmt_t f(unhex(fmt));
      vl_out_t r;

      REQUIRE(nullptr != tz);
      int rc =
          ty_timestamptz_to_char(strtoll(b, nullptr, 10), tz, f.f, nullptr, &r);

      got = 0 == rc ? r.str() : "ERR";
    } else {
      if (4 !=
          sscanf(line, "%15s %127s %4095s => %4095s", kind, a, fmt, want)) {
        continue;
      }
      got = 0 == strcmp(kind, "tochar.ts")
                ? ts_char(strtoll(a, nullptr, 10), unhex(fmt))
                : iv_char(unhex(a), unhex(fmt));
    }
    n++;
    if (got != unhex(want)) {
      if (bad++ < 20) {
        INFO(line);
        CHECK(got == unhex(want));
      }
    }
  }
  fclose(fp);
  CHECK(0 == bad);
  CHECK(n > 18000);
}

// Verify the errors, the empty results, and what the vectors cannot show.
// 1. A calendar field of an interval is an error; its clock fields are not.
// 2. An empty format and an infinite value have no result.
// 3. TM takes the host's names, unpadded, in the case of its keyword; a name
//    too long for its room is an error.
// 4. A prefix with no keyword is lost; a quoted string keeps its bytes.
TEST_CASE("to_char errors, locale and edges") {
  const std::string iv_err =
      "ERR invalid format specification for an interval value";

  // 1. A calendar field of an interval is an error; its clock fields are
  //    not.
  for (const char *f : {"Day", "DY", "D", "ID", "Month", "mon", "AD", "b.c.",
                        "TZ", "tz", "TZH", "TZM", "OF", "YYYY Day"}) {
    INFO(f);
    CHECK(iv_char("1 day", f) == iv_err);
  }
  CHECK(iv_char("1 year 2 mons 25:00:01", "YYYY MM DD HH24 HH12 Q RM") ==
        "0001 02 00 25 01 1 II  ");

  // 2. An empty format and an infinite value have no result.
  CHECK(ts_char(0, "") == "NULL");
  CHECK(ts_char(INT64_MAX, "YYYY") == "NULL");
  CHECK(ts_char(INT64_MIN, "YYYY") == "NULL");
  ty_interval_t inf;

  ty_interval_set_noend(&inf);
  {
    fmt_t f("YYYY");
    vl_out_t r;

    CHECK(1 == ty_interval_to_char(&inf, f.f, nullptr, &r));
  }

  // 3. TM takes the host's names, unpadded, in the case of its keyword; a
  //    name too long for its room is an error.
  ty_tochar_locale_t fr = {nullptr, french};
  int64_t feb = 33 * 86400LL * 1000000; /* 2000-02-03, a Thursday */

  CHECK(ts_char(feb, "TMMonth|TMMONTH|TMmonth|TMMon|Month", &fr) ==
        "Février|FéVRIER|février|Fév.|February ");
  CHECK(ts_char(feb, "TMDay TMdy", &fr) == "Dimanche dim.");
  CHECK(ts_char(feb, "TMMonth|TMDay") == "February|Thursday");
  ty_tochar_locale_t big = {nullptr, too_long};

  CHECK(ts_char(feb, "TMMon", &big) ==
        "ERR localized string format value too long");
  CHECK(ts_char(feb, "TMMonth", &big).size() == 61);

  // 4. A prefix with no keyword is lost; a quoted string keeps its bytes.
  CHECK(ts_char(feb, "FM") == "");
  CHECK(ts_char(feb, "FMxTM!") == "x!");
  CHECK(ts_char(feb, "FMxTMy") == "x0");
  CHECK(ts_char(feb, "\"YYYY\\\"\" YYYY") == "YYYY\" 2000");
}

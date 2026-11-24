/*
 * Copyright (c) 2026 CK Tan
 * SPDX-License-Identifier: MIT
 */

/*
 * Tests of date_bin of timestamptz and to_timestamp (ty_datetime.h), the
 * floats' in_range (ty_float.h), the order of text's pattern operators and
 * quote_literal (ty_text.h), by hand and by the vectors of test/gen_rest.py.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>

#include "doctest.h"
#include "ty_datetime.h"
#include "ty_decimal.h"
#include "ty_float.h"
#include "ty_text.h"
#include "vl.h"

namespace {

/* The bytes that the hex digits h stand for; "-" is empty. */
std::string unhex(const std::string &h) {
  std::string s;

  if ("-" == h) {
    return s;
  }
  for (size_t i = 0; i + 1 < h.size(); i += 2) {
    s += (char)strtol(h.substr(i, 2).c_str(), nullptr, 16);
  }
  return s;
}

/* The timestamp that s is: microseconds, inf or -inf. */
int64_t ts_of(const char *s) {
  return 0 == strcmp(s, "inf")    ? TY_TIMESTAMP_NOEND
         : 0 == strcmp(s, "-inf") ? TY_TIMESTAMP_NOBEGIN
                                  : strtoll(s, nullptr, 10);
}

/* A timestamp as a vector writes it. */
std::string ts_text(int64_t t) {
  return TY_TIMESTAMP_NOEND == t     ? "inf"
         : TY_TIMESTAMP_NOBEGIN == t ? "-inf"
                                     : std::to_string(t);
}

/* in_range of two floats as a vector writes it. */
std::string in_range(double val, double base, double offset, int sub,
                     int less) {
  int r;

  if (0 != ty_float64_in_range(val, base, offset, sub, less, &r)) {
    return "err";
  }
  return r ? "t" : "f";
}

/* to_timestamp as a vector writes it. */
std::string epoch(double seconds) {
  int64_t t;

  return 0 == ty_timestamptz_from_epoch(seconds, &t) ? ts_text(t) : "err";
}

/* The part of a timetz as a vector writes it: the number and the float8. */
std::string timetz_part(const char *unit, int64_t time, int zone) {
  ty_timetz_t t = {};
  ty_part_t p;
  int code, type = ty_datetime_part_unit(unit, 1, &code);
  char f8[64], num[TY_DEC_TEXT_SIZE];

  t.time = time;
  t.zone = zone;
  if (0 == type || 0 != ty_timetz_part(2 == type, code, &t, &p)) {
    return "err";
  }
  ty_dec128_t d = {p.num, TY_PART_SCALED == p.kind ? p.scale : 0};
  ty_dec128_format(&d, num);
  f8[ty_float64_format(p.f, 1, f8)] = '\0';
  return std::string(num) + " " + f8;
}

/* quote_literal. */
std::string quote(const std::string &s) {
  vl_t v(s);
  ty_varlena_t *out;

  REQUIRE(0 == ty_quote_literal(v, &out));
  std::string r(ty_vl_data(out), ty_vl_len(out));
  ty_free(out);
  return r;
}

} // namespace

// Verify in_range, to_timestamp, quote_literal and a zone's offset by hand.
// 1. in_range: NaN is above all, infinities that cannot be added hold every
//    value, and a negative or NaN offset is an error.
// 2. to_timestamp: 1970 and 2000, its rounding, its limits and its errors.
// 3. quote_literal: the doubling, and E for a backslash.
// 4. A zone's offset: each form, and what is not one.
TEST_CASE("in_range of floats, to_timestamp, quote_literal") {
  // 1. in_range.
  CHECK(in_range(3, 2, 1, 0, 1) == "t");
  CHECK(in_range(3.5, 2, 1, 0, 1) == "f");
  CHECK(in_range(NAN, NAN, 1, 0, 1) == "t");
  CHECK(in_range(NAN, 1, 1, 0, 1) == "f");
  CHECK(in_range(1, NAN, 1, 0, 1) == "t");
  CHECK(in_range(1, NAN, 1, 0, 0) == "f");
  CHECK(in_range(5, INFINITY, INFINITY, 1, 0) == "t");
  CHECK(in_range(1, 2, -1, 0, 1) == "err");
  CHECK(TY_ERR_INVALID == ty_errnum());
  CHECK(in_range(1, 2, NAN, 0, 1) == "err");

  // 2. to_timestamp.
  CHECK(epoch(0) == "-946684800000000");
  CHECK(epoch(946684800) == "0");
  CHECK(epoch(946684800.000001) == "1");
  CHECK(epoch(-INFINITY) == "-inf");
  CHECK(epoch(1e300) == "err");
  CHECK(std::string(ty_errmsg()) == "timestamp out of range: \"1e+300\"");
  CHECK(epoch(NAN) == "err");
  CHECK(std::string(ty_errmsg()) == "timestamp cannot be NaN");

  // 3. quote_literal.
  CHECK(quote("") == "''");
  CHECK(quote("a'b") == "'a''b'");
  CHECK(quote("a\\b'") == "E'a\\\\b'''");

  // 4. A zone's offset.
  int tz;
  CHECK(0 == ty_zone_offset_parse("+05:30", &tz));
  CHECK(-19800 == tz);
  CHECK(0 == ty_zone_offset_parse("-0800", &tz));
  CHECK(28800 == tz);
  CHECK(0 == ty_zone_offset_parse("+1:02:03", &tz));
  CHECK(-3723 == tz);
  CHECK(TY_DTERR_BAD_FORMAT == ty_zone_offset_parse("05", &tz));
  CHECK(TY_DTERR_BAD_FORMAT == ty_zone_offset_parse("+5x", &tz));
  CHECK(TY_DTERR_TZDISP_OVERFLOW == ty_zone_offset_parse("+99", &tz));
}

// Verify they are digres's, by what a digres built before they moved gave.
// 1. Read test/rest_known.txt.
// 2. bin: date_bin. 3. range8, range4: in_range. 4. epoch: to_timestamp.
// 5. pattern: the sign of the compare. 6. quote: quote_literal.
// 7. timetz: the part, exact and as a float8.
TEST_CASE("date_bin, in_range, to_timestamp and the rest are digres's") {
  const char *env = getenv("TY_VECTORS");
  std::string dir = env ? env : "test/vectors.txt";
  FILE *fp = fopen(
      (dir.substr(0, dir.rfind('/') + 1) + "rest_known.txt").c_str(), "r");
  char line[4096];
  int n = 0, bad = 0;

  REQUIRE(nullptr != fp);

  // 1. Read test/rest_known.txt.
  while (fgets(line, sizeof line, fp)) {
    char k[16], a[1024], b[1024], c[1024], want[1024];
    char s, l;
    int months, days;
    long long us;
    std::string got;

    if (5 == sscanf(line, "bin %d %d %lld %1023s %1023s =>", &months, &days,
                    &us, a, b)) {
      // 2. bin: date_bin.
      ty_interval_t stride;
      int64_t r;

      REQUIRE(1 == sscanf(strstr(line, "=>"), "=> %1023s", want));
      stride.time = us;
      stride.day = days;
      stride.month = months;
      got = 0 == ty_timestamp_bin(&stride, ts_of(a), ts_of(b), &r) ? ts_text(r)
                                                                   : "err";
    } else if (7 == sscanf(line, "%6s %1023s %1023s %1023s %c %c => %1023s", k,
                           a, b, c, &s, &l, want) &&
               0 == strncmp(k, "range", 5)) {
      // 3. range8, range4: in_range.
      bool f4 = '4' == k[5];

      got = in_range(f4 ? strtof(a, nullptr) : strtod(a, nullptr),
                     f4 ? strtof(b, nullptr) : strtod(b, nullptr),
                     strtod(c, nullptr), 't' == s, 't' == l);
    } else if (2 == sscanf(line, "epoch %1023s => %1023s", a, want)) {
      // 4. epoch: to_timestamp.
      got = epoch(strtod(a, nullptr));
    } else if (3 ==
               sscanf(line, "pattern %1023s %1023s => %1023s", a, b, want)) {
      // 5. pattern: the sign of the compare.
      std::string sa = unhex(a), sb = unhex(b);
      vl_t x(sa), y(sb);
      int r = ty_text_cmp(x, y);

      got = std::to_string((r > 0) - (r < 0));
    } else if (2 == sscanf(line, "quote %1023s => %1023s", a, want)) {
      // 6. quote: quote_literal.
      got = quote(unhex(a));
      std::string w = unhex(want);
      snprintf(want, sizeof want, "%s", w.c_str());
    } else if (5 == sscanf(line, "timetz %1023s %lld %d => %1023s %1023s", a,
                           &us, &days, want, b)) {
      // 7. timetz: the part, exact and as a float8.
      got = timetz_part(a, us, days);
      if ("err" != got) {
        std::string both = std::string(want) + " " + b;
        snprintf(want, sizeof want, "%s", both.c_str());
      }
    } else if (4 == sscanf(line, "timetz %1023s %lld %d => %1023s", a, &us,
                           &days, want)) {
      got = timetz_part(a, us, days);
    } else {
      continue;
    }
    n++;
    if (got != want && bad++ < 20) {
      INFO(line);
      CHECK(got == want);
    }
  }
  fclose(fp);
  CHECK(0 == bad);
  CHECK(n > 3000);
}

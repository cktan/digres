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
    REQUIRE(0 == ty_dch_compile(s.data(), s.size(), 0, &f));
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

namespace {

/* to_char of a number of type t (i4, i8, f4, f8) and text v. */
std::string num_char(const std::string &t, const std::string &v,
                     const std::string &fmt,
                     const ty_num_locale_t *loc = nullptr) {
  ty_num_format_t *f;

  if (0 != ty_num_compile(fmt.data(), fmt.size(), &f)) {
    return "ERR " + std::string(ty_errmsg());
  }
  vl_out_t r;
  int rc = "i4" == t ? ty_int32_to_char((int32_t)strtol(v.c_str(), nullptr, 10),
                                        f, loc, &r)
           : "i8" == t
               ? ty_int64_to_char(strtoll(v.c_str(), nullptr, 10), f, loc, &r)
           : "f4" == t
               ? ty_float4_to_char(strtof(v.c_str(), nullptr), f, loc, &r)
               : ty_float8_to_char(strtod(v.c_str(), nullptr), f, loc, &r);

  ty_free(f);
  return 0 == rc ? r.str() : "ERR " + std::string(ty_errmsg());
}

/* A host's locale for the test: German numbers, and a euro. */
void german(void *, ty_num_symbols_t *s) {
  s->negative_sign = "-";
  s->positive_sign = "";
  s->decimal_point = ",";
  s->thousands_sep = ".";
  s->currency_symbol = "€";
}

} // namespace

// Verify every number PostgreSQL printed by to_char, byte for byte.
// 1. Read test/tonum_known.txt: int4, int8, float4 and float8 by 99 formats
//    (the numeric lines are digres's, whose digits it makes).
// 2. Run each through ty, and compare the text.
TEST_CASE("to_char of numbers prints what PostgreSQL printed") {
  const char *env = getenv("TY_VECTORS");
  std::string dir = env ? env : "test/vectors.txt";
  std::string path = dir.substr(0, dir.rfind('/') + 1) + "tonum_known.txt";
  FILE *fp = fopen(path.c_str(), "r");
  char line[8192];
  int n = 0, bad = 0;

  REQUIRE(nullptr != fp);

  // 1. Read test/tonum_known.txt.
  while (fgets(line, sizeof line, fp)) {
    char kind[16], v[128], fmt[4096], want[4096];

    if (4 != sscanf(line, "tonum.%15s %127s %4095s => %4095s", kind, v, fmt,
                    want) ||
        0 == strcmp(kind, "num")) {
      continue;
    }

    // 2. Run each through ty, and compare the text.
    std::string got = num_char(kind, v, unhex(fmt));

    n++;
    if (got != unhex(want) && bad++ < 20) {
      INFO(line);
      CHECK(got == unhex(want));
    }
  }
  fclose(fp);
  CHECK(0 == bad);
  CHECK(n > 6000);
}

// Verify the checks of a number format, the locale, and the errors.
// 1. Each check of the format, with PostgreSQL's message.
// 2. D, G, L and S take the host's symbols; a format with none of them
//    does not ask for them.
// 3. V past the integer's range, and TH of a number that is not one.
TEST_CASE("to_char of numbers: checks, locale and errors") {
  // 1. Each check of the format, with PostgreSQL's message.
  CHECK(num_char("i4", "1", "9.9.9") == "ERR multiple decimal points");
  CHECK(num_char("i4", "1", "9V9.9") ==
        "ERR cannot use \"V\" and decimal point together");
  CHECK(num_char("i4", "1", "S9S") == "ERR cannot use \"S\" twice");
  CHECK(num_char("i4", "1", "MIS9") ==
        "ERR cannot use \"S\" and \"PL\"/\"MI\"/\"SG\"/\"PR\" together");
  CHECK(num_char("i4", "1", "S9MI") ==
        "ERR cannot use \"S\" and \"MI\" together");
  CHECK(num_char("i4", "1", "S9PL") ==
        "ERR cannot use \"S\" and \"PL\" together");
  CHECK(num_char("i4", "1", "S9SG") ==
        "ERR cannot use \"S\" and \"SG\" together");
  CHECK(num_char("i4", "1", "MI9PR") ==
        "ERR cannot use \"PR\" and \"S\"/\"PL\"/\"MI\"/\"SG\" together");
  CHECK(num_char("i4", "1", "PR9") == "ERR \"9\" must be ahead of \"PR\"");
  CHECK(num_char("i4", "1", "PR0") == "ERR \"0\" must be ahead of \"PR\"");
  CHECK(num_char("i4", "1", "RNRN") == "ERR cannot use \"RN\" twice");
  CHECK(num_char("i4", "1", "RNMI") ==
        "ERR \"RN\" is incompatible with other formats");
  CHECK(num_char("i4", "1", "RN9") == "              I");
  CHECK(num_char("i4", "1", "9EEEE9") ==
        "ERR \"EEEE\" must be the last pattern used");
  CHECK(num_char("i4", "1", "9EEEEEEEE") == "ERR cannot use \"EEEE\" twice");
  CHECK(num_char("i4", "1", "FM9EEEE") ==
        "ERR \"EEEE\" is incompatible with other formats");

  // 2. D, G, L and S take the host's symbols; a format with none of them
  //    does not ask for them.
  ty_num_locale_t de = {nullptr, german};

  CHECK(num_char("i4", "1234567", "L9G999G999D99S", &de) == "€1.234.567,00+");
  CHECK(num_char("f8", "-1234.5", "FM9G999D99S", &de) == "1.234,5-");
  CHECK(num_char("i4", "1234567", "9,999,999.99", &de) == " 1,234,567.00");

  // 3. V past the integer's range, and TH of a number that is not one.
  CHECK(num_char("i4", "1", "9V9999999999") == "ERR integer out of range");
  CHECK(num_char("i4", "2147483647", "9V9") == "ERR integer out of range");
  CHECK(num_char("i8", "1", "9V9999999999999999999") ==
        "ERR bigint out of range");
  CHECK(num_char("f8", "nan", "999TH") == "ERR \"NaN\" is not a number");
  CHECK(num_char("i4", "5", "TH") == "ERR \".\" is not a number");
  CHECK(num_char("i4", "5", "") == "");
}

namespace {

/* A host's TM names for the test: the C locale's English, in any case. */
int c_names(void *, int kind, const char *s, int *index, size_t *len) {
  static const char *const full_m[] = {
      "January", "February", "March",     "April",   "May",      "June",
      "July",    "August",   "September", "October", "November", "December"};
  static const char *const abbr_m[] = {"Jan", "Feb", "Mar", "Apr",
                                       "May", "Jun", "Jul", "Aug",
                                       "Sep", "Oct", "Nov", "Dec"};
  static const char *const full_d[] = {"Sunday",    "Monday",   "Tuesday",
                                       "Wednesday", "Thursday", "Friday",
                                       "Saturday"};
  static const char *const abbr_d[] = {"Sun", "Mon", "Tue", "Wed",
                                       "Thu", "Fri", "Sat"};
  const char *const *t = TY_TOCHAR_FULL_MONTH == kind     ? full_m
                         : TY_TOCHAR_ABBREV_MONTH == kind ? abbr_m
                         : TY_TOCHAR_FULL_DAY == kind     ? full_d
                                                          : abbr_d;
  int n = kind <= TY_TOCHAR_ABBREV_MONTH ? 12 : 7;

  for (int i = 0; i < n; i++) {
    size_t l = strlen(t[i]);

    if (0 == strncasecmp(s, t[i], l)) {
      *index = i;
      *len = l;
      return 0;
    }
  }
  return -1;
}

/* The reverse parse as to_timestamp (in UTC) or to_date gives it, or "E "
 * and the hex of the message, detail and hint, as the vectors write it. */
std::string from_char(const std::string &kind, const std::string &in,
                      const std::string &fmt) {
  ty_dch_format_t *f;
  ty_dch_parse_host_t host = {nullptr, c_names, nullptr};
  ty_dch_parsed_t p;
  ty_dch_error_t err;
  auto hex = [](const std::string &s) {
    std::string h;
    char b[3];

    for (unsigned char c : s) {
      snprintf(b, sizeof b, "%02x", c);
      h += b;
    }
    return h.empty() ? std::string("-") : h;
  };
  auto e = [&](const std::string &msg, const std::string &detail,
               const std::string &hint) {
    return "E " + hex(msg) + "|" + hex(detail) + "|" + hex(hint);
  };

  REQUIRE(0 == ty_dch_compile(fmt.data(), fmt.size(), 0, &f));
  int rc = ty_dch_parse(f, in.c_str(), 0, &host, &p, &err);

  ty_free(f);
  if (0 != rc) {
    if (TY_DCH_E_DTERR == err.code) {
      return e((-5 == err.dterr ? "time zone displacement out of range: \""
                                : "date/time field value out of range: \"") +
                   in + "\"",
               "", "");
    }
    return e(ty_errmsg(), err.detail, err.hint);
  }
  if ("ts" == kind) {
    int64_t ts;

    if (0 != ty_dch_to_timestamp(&p, ty_tz_find("UTC"), &ts)) {
      return e(ty_errmsg(), "", "");
    }
    return std::to_string(ts);
  }
  int32_t d;

  if (0 != ty_dch_to_date(&p, &d)) {
    return e(std::string(ty_errmsg()) + ": \"" + in + "\"", "", "");
  }
  return std::to_string(d);
}

} // namespace

// Verify the reverse parse against what PostgreSQL gave, error for error.
// 1. Read test/fromchar_known.txt: texts by formats, round trips of
//    to_char and their perturbations, and PostgreSQL's own cases.
// 2. Skip a format with a zone abbreviation (TZ): the session's table is
//    digres's, and its comparison is digres's.
// 3. Run each through ty as to_timestamp or to_date, and compare the value,
//    or the message, the detail and the hint.
TEST_CASE("to_timestamp and to_date give what PostgreSQL gave") {
  const char *env = getenv("TY_VECTORS");
  std::string dir = env ? env : "test/vectors.txt";
  std::string path = dir.substr(0, dir.rfind('/') + 1) + "fromchar_known.txt";
  FILE *fp = fopen(path.c_str(), "r");
  char line[16384];
  int n = 0, bad = 0;

  REQUIRE(nullptr != fp);

  // 1. Read test/fromchar_known.txt.
  while (fgets(line, sizeof line, fp)) {
    char kind[16], in[4096], fmt[4096];
    const char *arrow = strstr(line, " => ");

    if (nullptr == arrow ||
        3 != sscanf(line, "fromchar.%15s %4095s %4095s", kind, in, fmt)) {
      continue;
    }
    std::string want(arrow + 4);

    while (!want.empty() && ('\n' == want.back() || '\r' == want.back())) {
      want.pop_back();
    }
    std::string f = unhex(fmt);

    // 2. Skip a format with a zone abbreviation (TZ).
    bool tz = false;

    for (size_t i = 0; i + 1 < f.size(); i++) {
      if (('T' == f[i] || 't' == f[i]) &&
          ('Z' == f[i + 1] || 'z' == f[i + 1]) &&
          (i + 2 >= f.size() || !strchr("HhMm", f[i + 2]))) {
        tz = true;
      }
    }
    if (tz) {
      continue;
    }

    // 3. Run each through ty as to_timestamp or to_date.
    std::string got = from_char(kind, unhex(in), f);

    n++;
    if (got != want && bad++ < 20) {
      INFO(line);
      CHECK(got == want);
    }
  }
  fclose(fp);
  CHECK(0 == bad);
  CHECK(n > 7000);
}

// Verify the standard mode of jsonpath's datetime(), and what a format
// holds.
// 1. A character not quoted must be one of "-./,':; " in the format.
// 2. The text must match each separator, fill the format, and end with it.
// 3. The flags: a date, a time, a zone; IW is neither, as PostgreSQL has it.
TEST_CASE("the reverse parse's standard mode and flags") {
  ty_dch_format_t *f;
  ty_dch_parsed_t p;
  ty_dch_error_t err;
  auto parse = [&](const char *fmt, const char *in) {
    REQUIRE(0 == ty_dch_compile(fmt, strlen(fmt), 1, &f));
    int rc = ty_dch_parse(f, in, 1, nullptr, &p, &err);

    ty_free(f);
    return 0 == rc ? std::string("ok") : std::string(ty_errmsg());
  };

  // 1. A character not quoted must be one of "-./,':; " in the format.
  CHECK(-1 == ty_dch_compile("YYYY*MM", 7, 1, &f));
  CHECK(std::string(ty_errmsg()) == "invalid datetime format separator: \"*\"");
  CHECK(-1 == ty_dch_compile("YYYYéMM", 8, 1, &f));
  CHECK(std::string(ty_errmsg()) == "invalid datetime format separator: \"é\"");

  // 2. The text must match each separator, fill the format, and end with it.
  CHECK(parse("YYYY-MM-DD", "2024-02-29") == "ok");
  CHECK(2024 == p.tm.year);
  CHECK(parse("YYYY-MM-DD", "2024/02/29") ==
        "unmatched format separator \"-\"");
  CHECK(parse("YYYY-MM-DD HH24", "2024-02-29") ==
        "input string is too short for datetime format");
  CHECK(parse("YYYY-MM-DD", "2024-02-29 x") ==
        "trailing characters remain in input string after datetime format");
  CHECK(parse("YYYY\"x\"MM", "2024y02") == "unmatched format character \"x\"");
  CHECK(TY_DCH_E_FORMAT == err.code);

  // 3. The flags: a date, a time, a zone; IW is neither.
  struct {
    const char *fmt;
    unsigned flags;
  } cases[] = {{"YYYY-MM-DD", TY_DCH_DATED},
               {"HH24:MI", TY_DCH_TIMED},
               {"TZH", TY_DCH_ZONED},
               {"YYYY HH TZ", TY_DCH_DATED | TY_DCH_TIMED | TY_DCH_ZONED},
               {"IW", 0},
               {"FX", 0},
               {"", 0}};

  for (const auto &c : cases) {
    INFO(c.fmt);
    REQUIRE(0 == ty_dch_compile(c.fmt, strlen(c.fmt), 0, &f));
    CHECK(c.flags == ty_dch_flags(f));
    ty_free(f);
  }
}

/*
 * Copyright (c) 2026 CK Tan
 * SPDX-License-Identifier: MIT
 */

/*
 * Tests of the time zones of ty (include/ty_timezone.h): the zones against
 * test/tz_known.txt, which Python's zoneinfo made from the same files, and
 * the lookup of a zone, a POSIX spec, an abbreviation, and the data.
 */
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

#include "doctest.h"
#include "tzdata.h"
#include "ty_timezone.h"

namespace {

/* The tokens of a line of tz_known.txt. */
std::vector<std::string> split(const std::string &line) {
  std::vector<std::string> out;
  std::istringstream in(line);
  std::string w;

  while (in >> w) {
    out.push_back(w);
  }
  return out;
}

} // namespace

// Verify the zones against what Python's zoneinfo says of the same files.
// 1. For each zone and UTC instant of tz_known.txt, the offset, the DST flag,
//    the abbreviation and the local fields are the same.
// 2. For each zone and local time, the offset west of Greenwich and the DST
//    flag are the same: the offset before a transition for a time it skips,
//    and the offset after it for a time it repeats.
TEST_CASE("the zones of ty agree with zoneinfo on the same files") {
  std::ifstream in("test/tz_known.txt");
  std::string line;
  size_t nl = 0, no = 0, na = 0, nt = 0, np = 0;

  REQUIRE(in.good());
  while (std::getline(in, line)) {
    std::vector<std::string> w = split(line);
    const ty_tz_t *tz = ty_tz_find(w[1].c_str());

    INFO(line);
    REQUIRE(nullptr != tz);
    if (w[0] == "L") {
      // L ZONE EPOCH => OFFSET ISDST ABBREV Y M D h m s
      ty_tz_local_t l;

      REQUIRE(0 == ty_tz_localtime(tz, std::stoll(w[2]), &l));
      CHECK(std::stoi(w[4]) == l.gmtoff);
      CHECK(std::stoi(w[5]) == l.isdst);
      CHECK(w[6] == l.abbrev);
      CHECK(std::stoi(w[7]) == l.year);
      CHECK(std::stoi(w[8]) == l.mon);
      CHECK(std::stoi(w[9]) == l.mday);
      CHECK(std::stoi(w[10]) == l.hour);
      CHECK(std::stoi(w[11]) == l.min);
      CHECK(std::stoi(w[12]) == l.sec);
      nl++;

      // The same instant, as a timestamptz: the fields, the offset west,
      // the timestamp of the local time, and its date.
      int64_t ts = (std::stoll(w[2]) - 946684800) * 1000000; // from 2000
      ty_tm_t tm;
      int32_t fsec;
      int west, isdst2;
      const char *abbr;
      int64_t local;
      int32_t date;

      REQUIRE(0 == ty_timestamptz_to_local(tz, ts, &tm, &fsec, &west, &isdst2,
                                           &abbr));
      CHECK(-std::stoi(w[4]) == west);
      CHECK(std::stoi(w[5]) == isdst2);
      CHECK(w[6] == abbr);
      CHECK(std::stoi(w[7]) == tm.year);
      CHECK(std::stoi(w[8]) == tm.mon);
      CHECK(std::stoi(w[9]) == tm.mday);
      CHECK(std::stoi(w[10]) == tm.hour);
      CHECK(std::stoi(w[11]) == tm.min);
      CHECK(std::stoi(w[12]) == tm.sec);
      CHECK(0 == fsec);
      REQUIRE(0 == ty_timestamptz_to_timestamp(tz, ts, &local));
      CHECK(local == ts + (int64_t)std::stoi(w[4]) * 1000000);
      REQUIRE(0 == ty_timestamptz_to_date(tz, ts, &date));
      CHECK(date ==
            ty_date2j(tm.year, tm.mon, tm.mday) - TY_POSTGRES_EPOCH_JDATE);
    } else if (w[0] == "A") {
      // A ZONE EPOCH MONTHS DAYS USECS => INSTANT (microseconds since 1970)
      ty_interval_t span = {std::stoll(w[5]), (int32_t)std::stoi(w[4]),
                            (int32_t)std::stoi(w[3])};
      int64_t ts = (std::stoll(w[2]) - 946684800) * 1000000, r, sub;
      ty_interval_t negspan;

      REQUIRE(0 == ty_timestamptz_add_interval(tz, ts, &span, &r));
      CHECK(std::stoll(w[7]) - INT64_C(946684800000000) == r);
      // minus the negated interval is the same sum
      REQUIRE(0 == ty_interval_neg(&span, &negspan));
      REQUIRE(0 == ty_timestamptz_sub_interval(tz, ts, &negspan, &sub));
      CHECK(r == sub);
      na++;
    } else if (w[0] == "T") {
      // T ZONE EPOCH UNIT => INSTANT (microseconds since 1970)
      int code, r_code;
      int64_t ts = (std::stoll(w[2]) - 946684800) * 1000000, r;

      REQUIRE(1 == ty_datetime_unit(w[3].c_str(), &code));
      r_code = ty_timestamptz_trunc(tz, code, ts, &r);
      REQUIRE(0 == r_code);
      CHECK(std::stoll(w[5]) - INT64_C(946684800000000) == r);
      nt++;
    } else if (w[0] == "P") {
      // P ZONE EPOCH UNIT => VALUE
      int code, kind;
      int64_t ts = (std::stoll(w[2]) - 946684800) * 1000000;
      ty_part_t p;

      kind = ty_datetime_part_unit(w[3].c_str(), 0, &code);
      REQUIRE(0 != kind);
      REQUIRE(0 == ty_timestamptz_part(tz, kind == 2, code, ts, &p));
      CHECK(std::stod(w[5]) == p.f);
      if (p.kind == TY_PART_INT) {
        CHECK(std::stoll(w[5]) == p.num);
      }
      np++;
    } else {
      // O ZONE Y M D h m s => OFFSET_WEST ISDST
      ty_tm_t tm = {std::stoi(w[2]), std::stoi(w[3]), std::stoi(w[4]),
                    std::stoi(w[5]), std::stoi(w[6]), std::stoi(w[7])};
      int isdst = -1;

      CHECK(std::stoi(w[9]) ==
            ty_tz_offset_for_local(tz, &tm, &isdst, nullptr));
      CHECK(std::stoi(w[10]) == isdst);
      no++;

      // The cast of that local time to timestamptz: the local time plus the
      // offset; and the date's midnight in the zone, by the same rule.
      int64_t local, utc;

      REQUIRE(0 == ty_tm_to_timestamp(&tm, 0, &local));
      REQUIRE(0 == ty_timestamp_to_timestamptz(tz, local, &utc, nullptr));
      CHECK(utc == local + (int64_t)std::stoi(w[9]) * 1000000);
      ty_tm_t midnight = {tm.year, tm.mon, tm.mday, 0, 0, 0};
      int64_t mid_local, mid_utc, date_utc;
      int32_t date =
          ty_date2j(tm.year, tm.mon, tm.mday) - TY_POSTGRES_EPOCH_JDATE;

      REQUIRE(0 == ty_tm_to_timestamp(&midnight, 0, &mid_local));
      REQUIRE(0 ==
              ty_timestamp_to_timestamptz(tz, mid_local, &mid_utc, nullptr));
      REQUIRE(0 == ty_date_to_timestamptz(tz, date, &date_utc, nullptr));
      CHECK(date_utc == mid_utc);
    }
  }
  CHECK(nl > 5000);
  CHECK(no > 3000);
  CHECK(na > 2000);
  CHECK(nt > 3000);
  CHECK(np > 5000);
}

// Verify how a zone is found.
// 1. A name in any case is the same zone, with the database's spelling.
// 2. A name that is no zone is none, as is a colon with no zone, and a name
//    too long. A colon before a zone's name is allowed, as in PostgreSQL.
// 3. "GMT", and a POSIX spec, are zones, and a POSIX spec is upper case.
// 4. A zone of one offset: its name, its offset, its abbreviation.
TEST_CASE("a zone is found by its name, a POSIX spec, or an offset") {
  // 1. A name in any case is the same zone, with the database's spelling.
  const ty_tz_t *ny = ty_tz_find("America/New_York");

  REQUIRE(nullptr != ny);
  CHECK(ny == ty_tz_find("america/new_york"));
  CHECK(ny == ty_tz_find("AMERICA/NEW_YORK"));
  CHECK(std::string("America/New_York") == ty_tz_name(ny));

  // 2. A name that is no zone is none, as is a colon with no zone, and a name
  //    too long. A colon before a zone's name is allowed, as in PostgreSQL.
  CHECK(nullptr == ty_tz_find("Nowhere/Land"));
  CHECK(nullptr == ty_tz_find(""));
  CHECK(nullptr == ty_tz_find(":Nowhere/Land"));
  CHECK(nullptr == ty_tz_find(":UTC+3")); // a colon bars a POSIX spec
  CHECK(nullptr == ty_tz_find(std::string(300, 'a').c_str()));

  // 3. "GMT", and a POSIX spec, are zones, and a POSIX spec is upper case.
  const ty_tz_t *gmt = ty_tz_find("gmt");
  const ty_tz_t *spec = ty_tz_find("est5edt");
  const ty_tz_t *plus3 = ty_tz_find("utc+3");
  long east;

  REQUIRE(nullptr != gmt);
  CHECK(std::string("GMT") == ty_tz_name(gmt));
  CHECK(1 == ty_tz_fixed_offset(gmt, &east));
  CHECK(0 == east);
  REQUIRE(nullptr != spec);
  CHECK(std::string("EST5EDT") == ty_tz_name(spec));
  REQUIRE(nullptr != plus3);
  CHECK(1 == ty_tz_fixed_offset(plus3, &east));
  CHECK(-3 * 3600 == east); // UTC+3 in POSIX is three hours west
  CHECK(0 == ty_tz_fixed_offset(ny, &east));

  // 4. A zone of one offset: its name, its offset, its abbreviation.
  const ty_tz_t *five_west = ty_tz_from_offset(5 * 3600);
  ty_tz_local_t l;

  REQUIRE(nullptr != five_west);
  CHECK(std::string("<-05>+05") == ty_tz_name(five_west));
  REQUIRE(0 == ty_tz_localtime(five_west, 0, &l));
  CHECK(-5 * 3600 == l.gmtoff);
  CHECK(std::string("-05") == l.abbrev);
  CHECK(1969 == l.year);
  CHECK(19 == l.hour);
  const ty_tz_t *colon = ty_tz_find(":america/new_york");

  REQUIRE(nullptr != colon);
  CHECK(std::string("America/New_York") == ty_tz_name(colon));
}

// Verify the abbreviations of a zone.
// 1. EST is known in New York, in any case, with the one meaning.
// 2. MSK has more than one meaning in Moscow, so it is not fixed.
// 3. An abbreviation that the zone never used is not known.
// 4. At a time of EDT, the abbreviation EST forces the standard offset.
TEST_CASE("abbreviations are asked of the zone") {
  const ty_tz_t *ny = ty_tz_find("America/New_York");
  const ty_tz_t *moscow = ty_tz_find("Europe/Moscow");
  int fixed, west, isdst;

  REQUIRE(nullptr != ny);
  REQUIRE(nullptr != moscow);
  // 1. EST is known in New York, in any case, with the one meaning.
  CHECK(1 == ty_tz_abbrev_is_known(ny, "est", &fixed, &west, &isdst));
  CHECK(1 == fixed);
  CHECK(5 * 3600 == west);
  CHECK(0 == isdst);
  CHECK(1 == ty_tz_abbrev_is_known(ny, "EDT", &fixed, &west, &isdst));
  CHECK(4 * 3600 == west);
  CHECK(1 == isdst);

  // 2. MSK has more than one meaning in Moscow, so it is not fixed.
  CHECK(1 == ty_tz_abbrev_is_known(moscow, "msk", &fixed, &west, &isdst));
  CHECK(0 == fixed);

  // 3. An abbreviation that the zone never used is not known.
  CHECK(0 == ty_tz_abbrev_is_known(ny, "PST", &fixed, &west, &isdst));
  CHECK(0 == ty_tz_abbrev_is_known(ny, "xyzzy", &fixed, &west, &isdst));

  // 4. At a time of EDT, the abbreviation EST forces the standard offset.
  ty_tm_t july = {2000, 7, 1, 12, 0, 0};
  ty_tm_t winter = {2000, 1, 1, 12, 0, 0};

  CHECK(4 * 3600 == ty_tz_offset_for_local(ny, &july, &isdst, nullptr));
  CHECK(1 == isdst);
  CHECK(5 * 3600 == ty_tz_abbrev_offset(ny, &july, "est", &isdst));
  CHECK(0 == isdst);
  CHECK(5 * 3600 == ty_tz_abbrev_offset(ny, &winter, "xyzzy", &isdst));
  CHECK(0 == isdst);
}

// Verify the local time of a gap, a fold, and a date out of range.
// 1. New York, 2024-03-10 02:30, which the clock skipped: the offset before
//    the transition, 5 hours west, standard time.
// 2. 2024-11-03 01:30, which it repeated: the offset after, 5 hours.
// 3. A date past the range of Julian days is UTC.
// 4. The instant of the local time is given if asked.
TEST_CASE("a local time in a gap or a fold has one offset") {
  const ty_tz_t *ny = ty_tz_find("America/New_York");
  int isdst;
  int64_t utc;

  REQUIRE(nullptr != ny);
  // 1. 2024-03-10 02:30, which the clock skipped.
  ty_tm_t gap = {2024, 3, 10, 2, 30, 0};

  CHECK(5 * 3600 == ty_tz_offset_for_local(ny, &gap, &isdst, &utc));
  CHECK(0 == isdst);
  CHECK(1710055800 == utc); // 07:30 UTC

  // 2. 2024-11-03 01:30, which it repeated.
  ty_tm_t fold = {2024, 11, 3, 1, 30, 0};

  CHECK(5 * 3600 == ty_tz_offset_for_local(ny, &fold, &isdst, &utc));
  CHECK(0 == isdst);

  // 3. A date past the range of Julian days is UTC.
  ty_tm_t far = {6000000, 1, 1, 0, 0, 0};

  CHECK(0 == ty_tz_offset_for_local(ny, &far, &isdst, &utc));
  CHECK(0 == isdst);
  CHECK(0 == utc);
}

// Verify the data: its release, its names in order, and a bad pack.
// 1. The release is a tzdata release, and the names are many and in order.
// 2. A pack with a bad magic number, a bad checksum, or a short one is
//    refused; so is a good pack once a zone has been found.
TEST_CASE("the data of the zones is checked") {
  // 1. The release is a tzdata release, and the names are many and in order.
  std::string rel = ty_tz_data_version();

  CHECK(rel.size() >= 5);
  CHECK(rel.substr(0, 2) == "20");
  REQUIRE(ty_tz_name_count() > 500);
  for (unsigned i = 1; i < ty_tz_name_count(); i++) {
    std::string a = ty_tz_name_at(i - 1), b = ty_tz_name_at(i);

    for (char &c : a) {
      c = (char)tolower((unsigned char)c);
    }
    for (char &c : b) {
      c = (char)tolower((unsigned char)c);
    }
    REQUIRE(a < b);
  }
  CHECK(nullptr == ty_tz_name_at(ty_tz_name_count()));

  // 2. A bad pack is refused; so is a good one after a zone was found.
  std::vector<unsigned char> pack(ty_internal::ty_tzpack,
                                  ty_internal::ty_tzpack +
                                      ty_internal::ty_tzpack_size);

  REQUIRE(nullptr != ty_tz_find("UTC"));
  CHECK(-1 == ty_tz_use_data(pack.data(), pack.size()));
  CHECK(TY_ERR_INVALID == ty_errnum());
  std::vector<unsigned char> bad = pack;

  bad[0] = 'X';
  CHECK(-1 == ty_tz_use_data(bad.data(), bad.size()));
  bad = pack;
  bad[pack.size() / 2] ^= 1;
  CHECK(-1 == ty_tz_use_data(bad.data(), bad.size()));
  CHECK(-1 == ty_tz_use_data(pack.data(), 20));
}

/*
 * The child of the test of ty_tz_use_data: a process of its own, in which no
 * zone has been found. The pack is in a file, with another release.
 */
int tz_use_data_child(const char *path) {
  std::ifstream in(path, std::ios::binary);
  std::vector<char> pack((std::istreambuf_iterator<char>(in)),
                         std::istreambuf_iterator<char>());

  if (0 != ty_tz_use_data(pack.data(), pack.size())) {
    return 2;
  }
  if (std::string("2999z") != ty_tz_data_version() ||
      nullptr == ty_tz_find("America/New_York")) {
    return 3;
  }
  return 0;
}

// Verify that a host's own pack is used.
// 1. Make a pack from ty's: a new release and a new checksum.
// 2. A process that gives it before it finds a zone gets that release, and
//    finds zones in it.
TEST_CASE("a pack of the host is used in place of ty's own") {
  std::vector<unsigned char> pack(ty_internal::ty_tzpack,
                                  ty_internal::ty_tzpack +
                                      ty_internal::ty_tzpack_size);

  // 1. Make a pack from ty's: a new release and a new checksum.
  memset(&pack[8], 0, 16);
  memcpy(&pack[8], "2999z", 5);
  uint32_t h = 0x811C9DC5u;

  for (size_t i = 0; i < pack.size(); i++) {
    unsigned char c = (i >= 36 && i < 40) ? 0 : pack[i];

    h = (h ^ c) * 0x01000193u;
  }
  for (int k = 0; k < 4; k++) {
    pack[36 + k] = (unsigned char)(h >> (24 - 8 * k));
  }
  std::string path =
      std::string("build/tz_host_pack.") + std::to_string(getpid());
  {
    FILE *f = fopen(path.c_str(), "wb");

    REQUIRE(nullptr != f);
    fwrite(pack.data(), 1, pack.size(), f);
    fclose(f);
  }

  // 2. A process that gives it before it finds a zone gets that release.
  pid_t pid = fork();

  REQUIRE(pid >= 0);
  if (pid == 0) {
    execl("/proc/self/exe", "test_ty", "--tz-use-data-child", path.c_str(),
          (char *)nullptr);
    _exit(127);
  }
  int status = 0;

  REQUIRE(waitpid(pid, &status, 0) == pid);
  unlink(path.c_str());
  CHECK(WIFEXITED(status));
  CHECK(0 == WEXITSTATUS(status));
}

// Verify the shapes of PostgreSQL's library: the fields, and the abbreviations.
// 1. ty_tz_localtime_pg gives the same instant as ty_tz_localtime, with the
//    month from 0 and the year from 1900.
// 2. An instant past the range of pg_time_t is NULL.
// 3. The abbreviations a zone uses are listed once each, to the end.
// 4. A name asked again gives the zone again, whatever was asked between.
TEST_CASE("PostgreSQL's shapes of a zone agree with ty's own") {
  const ty_tz_t *ny = ty_tz_find("America/New_York");
  int64_t t = 1710055800; // 2024-03-10 07:30:00 UTC, the hour after the gap

  REQUIRE(nullptr != ny);
  // 1. ty_tz_localtime_pg gives the same instant as ty_tz_localtime.
  ty_tz_local_t a;
  const ty_tz_pgtm_t *b = ty_tz_localtime_pg(ny, &t);

  REQUIRE(0 == ty_tz_localtime(ny, t, &a));
  REQUIRE(nullptr != b);
  CHECK(a.year == b->tm_year + 1900);
  CHECK(a.mon == b->tm_mon + 1);
  CHECK(a.mday == b->tm_mday);
  CHECK(a.hour == b->tm_hour);
  CHECK(a.min == b->tm_min);
  CHECK(a.sec == b->tm_sec);
  CHECK(a.wday == b->tm_wday);
  CHECK(a.yday == b->tm_yday);
  CHECK(a.isdst == b->tm_isdst);
  CHECK(a.gmtoff == b->tm_gmtoff);
  CHECK(std::string(a.abbrev) == b->tm_zone);

  // 2. An instant past the range of pg_time_t is NULL.
  int64_t huge = INT64_MAX;

  CHECK(nullptr == ty_tz_localtime_pg(ny, &huge));

  // 3. The abbreviations a zone uses are listed once each, to the end.
  std::vector<std::string> abbrs;
  int indx = 0;

  while (const char *w = ty_tz_next_abbrev(ny, &indx)) {
    abbrs.push_back(w);
  }
  CHECK(abbrs.size() >= 4);
  for (const char *want : {"LMT", "EDT", "EST", "EWT", "EPT"}) {
    CHECK(std::find(abbrs.begin(), abbrs.end(), want) != abbrs.end());
  }

  // 4. A name asked again gives the zone again, whatever was asked between.
  const ty_tz_t *tokyo = ty_tz_find("Asia/Tokyo");

  REQUIRE(nullptr != tokyo);
  CHECK(tokyo != ny);
  CHECK(ny == ty_tz_find("America/New_York"));
  CHECK(tokyo == ty_tz_find("Asia/Tokyo"));
  CHECK(nullptr == ty_tz_find("Nowhere/Land"));
  CHECK(nullptr == ty_tz_find("Nowhere/Land"));
  CHECK(tokyo == ty_tz_find("Asia/Tokyo"));
}

// Verify the casts of timestamptz at their edges.
// 1. The infinities pass through every cast.
// 2. A timestamp the zone moves out of range is an error, or an infinity
//    and an overflow flag if one is asked for.
// 3. A date with no timestamp is "date out of range for timestamp".
TEST_CASE("the casts of timestamptz keep the infinities and the edges") {
  const ty_tz_t *tokyo = ty_tz_find("Asia/Tokyo");
  const ty_tz_t *ny = ty_tz_find("America/New_York");
  int64_t out;
  int32_t d;
  int over;

  REQUIRE(nullptr != tokyo);
  // 1. The infinities pass through every cast.
  CHECK(0 ==
        ty_timestamp_to_timestamptz(tokyo, TY_TIMESTAMP_NOEND, &out, nullptr));
  CHECK(TY_TIMESTAMP_NOEND == out);
  CHECK(0 == ty_timestamptz_to_timestamp(tokyo, TY_TIMESTAMP_NOBEGIN, &out));
  CHECK(TY_TIMESTAMP_NOBEGIN == out);
  CHECK(0 == ty_timestamptz_to_date(tokyo, TY_TIMESTAMP_NOEND, &d));
  CHECK(TY_DATE_NOEND == d);
  CHECK(0 == ty_date_to_timestamptz(tokyo, TY_DATE_NOBEGIN, &out, nullptr));
  CHECK(TY_TIMESTAMP_NOBEGIN == out);

  // 2. The largest timestamp, in a zone east of UTC, is out of range.
  int64_t last = TY_END_TIMESTAMP - 1;

  CHECK(-1 == ty_timestamp_to_timestamptz(ny, last, &out, nullptr));
  CHECK(TY_ERR_RANGE == ty_errnum());
  CHECK(std::string("timestamp out of range") == ty_errmsg());
  CHECK(0 == ty_timestamp_to_timestamptz(ny, last, &out, &over));
  CHECK(1 == over);
  CHECK(TY_TIMESTAMP_NOEND == out);
  CHECK(0 == ty_timestamp_to_timestamptz(tokyo, TY_MIN_TIMESTAMP, &out, &over));
  CHECK(-1 == over); // Tokyo is ahead: its midnight is before the first instant
  CHECK(TY_TIMESTAMP_NOBEGIN == out);

  // 3. A date with no timestamp is "date out of range for timestamp".
  int32_t big = TY_TIMESTAMP_END_JULIAN - TY_POSTGRES_EPOCH_JDATE;

  CHECK(-1 == ty_date_to_timestamptz(ny, big, &out, nullptr));
  CHECK(std::string("date out of range for timestamp") == ty_errmsg());
  CHECK(0 == ty_date_to_timestamptz(ny, big, &out, &over));
  CHECK(1 == over);
}

// Verify age of timestamptz: the local times' symbolic difference.
// 1. In UTC it is what age of two timestamps gives.
// 2. In New York, across the change to daylight time, the clock differs by
//    an hour more than the instants: 2 days 1 hour.
// 3. The sign is from the instants: swapped, the interval is negated.
// 4. An infinity is what the difference of timestamps gives.
TEST_CASE("age of timestamptz is the difference of the local times") {
  const ty_tz_t *utc = ty_tz_find("UTC");
  const ty_tz_t *ny = ty_tz_find("America/New_York");
  int64_t a = (INT64_C(1710158400) - 946684800) * 1000000; // 2024-03-11 12:00Z
  int64_t b = (INT64_C(1709985600) - 946684800) * 1000000; // 2024-03-09 12:00Z
  ty_interval_t x, y, z;

  REQUIRE(nullptr != utc);
  REQUIRE(nullptr != ny);
  // 1. In UTC it is what age of two timestamps gives.
  REQUIRE(0 == ty_timestamptz_age(utc, a, b, &x));
  REQUIRE(0 == ty_timestamp_age(a, b, &y));
  CHECK(x.month == y.month);
  CHECK(x.day == y.day);
  CHECK(x.time == y.time);
  CHECK(2 == x.day);
  CHECK(0 == x.time);

  // 2. In New York, across the change to daylight time: 2 days 1 hour.
  REQUIRE(0 == ty_timestamptz_age(ny, a, b, &z));
  CHECK(0 == z.month);
  CHECK(2 == z.day);
  CHECK(TY_USECS_PER_HOUR == z.time);

  // 3. The sign is from the instants: swapped, the interval is negated.
  REQUIRE(0 == ty_timestamptz_age(ny, b, a, &z));
  CHECK(-2 == z.day);
  CHECK(-TY_USECS_PER_HOUR == z.time);

  // 4. An infinity is what the difference of timestamps gives.
  REQUIRE(0 == ty_timestamptz_age(ny, TY_TIMESTAMP_NOEND, a, &z));
  CHECK(ty_interval_is_noend(&z));
  CHECK(-1 ==
        ty_timestamptz_age(ny, TY_TIMESTAMP_NOEND, TY_TIMESTAMP_NOEND, &z));
}

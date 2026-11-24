#!/usr/bin/env python3
# Copyright (c) 2026 CK Tan
# SPDX-License-Identifier: MIT
#
# gen_tochar.py -- write test/tochar_known.txt: what PostgreSQL's to_char
# prints for timestamps, timestamptz in several zones, and intervals, by
# many formats. The text is PostgreSQL's own, so it is taken from a digres
# built before to_char moved into ty (any server with PostgreSQL's
# formatting.c gives it):
#
#   test/gen_tochar.py PSQL_ARGS... > test/tochar_known.txt
#
# PSQL_ARGS is how to connect, such as: -h /tmp/sock -p 5432 -d postgres.
# A line is one of:
#   tochar.ts US FMTHEX => OUTHEX
#   tochar.tz ZONE US FMTHEX => OUTHEX
#   tochar.iv IVHEX FMTHEX => OUTHEX
# US is a timestamp's microseconds since 2000-01-01, IVHEX an interval's
# text as hex digits, FMTHEX the format and OUTHEX the result (- if empty).

import random
import subprocess
import sys

random.seed(20261123)

TIMESTAMPS = [
    "4714-11-24 00:00:00 BC", "0001-12-31 23:59:59.999999 BC",
    "0001-01-01 00:00:00", "0099-03-01 01:02:03", "0999-12-31 12:00:00",
    "1000-01-01 00:00:00.5", "1582-10-15 06:30:00", "1899-12-31 23:00:00",
    "1900-01-01 00:00:00", "1969-12-31 23:59:59", "1970-01-01 00:00:00",
    "1999-12-31 23:59:59.999999", "2000-01-01 00:00:00", "2000-02-29 12:00:00",
    "2001-01-01 11:59:59.000001", "2004-12-31 13:00:00", "2005-01-01 00:00:01",
    "2005-01-02 08:08:08", "2008-12-28 23:00:00", "2008-12-29 00:00:00",
    "2009-12-31 10:10:10.10", "2010-01-03 18:45:00", "2010-01-04 00:00:00",
    "2011-11-11 11:11:11.111111", "2012-12-12 12:12:12.121212",
    "2013-03-13 13:13:13", "2021-02-21 21:21:21", "2022-12-22 22:22:22",
    "2023-01-23 03:03:03", "2024-02-29 13:05:09.123456", "2100-01-01 00:00:00",
    "9999-12-31 23:59:59.999999", "10000-01-01 00:00:00",
    "294276-12-31 23:59:59.999999", "0044-03-15 12:00:00 BC",
    "0100-01-01 00:00:00 BC", "0101-06-01 00:00:00 BC", "1000-01-01 00:00:00 BC",
]
for _ in range(6):
    TIMESTAMPS.append("%04d-%02d-%02d %02d:%02d:%02d.%06d" % (
        random.randrange(1, 3000), random.randrange(1, 13), random.randrange(1, 29),
        random.randrange(24), random.randrange(60), random.randrange(60),
        random.randrange(1000000)))

ZONES = ["UTC", "America/New_York", "Asia/Kolkata", "Australia/Lord_Howe",
         "America/St_Johns", "Europe/London", "Pacific/Chatham", "Asia/Kathmandu",
         "Europe/Amsterdam", "Etc/GMT+12", "Pacific/Kiritimati"]
ZONE_TIMES = ["1880-06-01 12:00:00+00", "1900-01-01 00:00:00+00",
              "1937-07-01 00:00:00+00", "1970-01-01 00:00:00+00",
              "2000-01-01 00:00:00+00", "2016-03-13 06:59:59+00",
              "2016-03-13 07:00:00+00", "2016-11-06 05:30:00+00",
              "2024-06-30 23:59:59.999999+00", "2024-12-31 18:00:00+00",
              "0001-01-01 00:00:00+00", "2100-07-04 04:00:00+00"]

INTERVALS = [
    "0", "1 microsecond", "-1 microsecond", "0.5 seconds", "-0.5 seconds",
    "1 second", "59 seconds", "1 minute", "-1 minute", "1 hour", "11 hours",
    "12 hours", "13 hours", "23 hours", "24 hours", "25 hours", "-25 hours",
    "-12 hours", "-13 hours", "100 hours", "-100 hours", "1 day", "-1 day",
    "7 days", "31 days", "-31 days", "1 mon", "-1 mon", "2 mons", "3 mons",
    "-3 mons", "4 mons", "11 mons", "-11 mons", "12 mons", "-12 mons",
    "1 year", "-1 year", "1 year 2 mons 3 days 04:05:06.789",
    "-1 year -2 mons -3 days -04:05:06.789", "99 years", "100 years",
    "101 years", "-99 years", "-100 years", "-101 years", "999 years",
    "1000 years", "1234 years 5 mons", "-1234 years -5 mons", "9999 years",
    "10000 years", "-10000 years", "178000000 years", "-178000000 years",
    "2562047788 hours 54.775807 seconds", "-2562047788 hours -54.775808 seconds",
    "3 days -04:00:00", "-3 days 04:00:00", "1 mon -1 day", "1 year -1 mon",
    "6 mons 15 days 12:00:00", "0.000001 seconds", "12:34:56.123456",
]

KEYWORDS = [
    "A.D.", "A.M.", "AD", "AM", "B.C.", "BC", "CC", "DAY", "DDD", "DD", "DY",
    "Day", "Dy", "D", "FF1", "FF2", "FF3", "FF4", "FF5", "FF6", "FX", "HH24",
    "HH12", "HH", "IDDD", "ID", "IW", "IYYY", "IYY", "IY", "I", "J", "MI", "MM",
    "MONTH", "MON", "MS", "Month", "Mon", "OF", "P.M.", "PM", "Q", "RM",
    "SSSSS", "SSSS", "SS", "TZH", "TZM", "TZ", "US", "WW", "W", "Y,YYY", "YYYY",
    "YYY", "YY", "Y", "a.d.", "a.m.", "ad", "am", "b.c.", "bc", "cc", "day",
    "ddd", "dd", "dy", "d", "ff1", "ff2", "ff3", "ff4", "ff5", "ff6", "fx",
    "hh24", "hh12", "hh", "iddd", "id", "iw", "iyyy", "iyy", "iy", "i", "j",
    "mi", "mm", "month", "mon", "ms", "of", "p.m.", "pm", "q", "rm", "sssss",
    "ssss", "ss", "tzh", "tzm", "tz", "us", "ww", "w", "y,yyy", "yyyy", "yyy",
    "yy", "y",
]
# the keywords an interval may have: no calendar date, no zone
CALENDAR = {"A.D.", "AD", "B.C.", "BC", "a.d.", "ad", "b.c.", "bc", "DAY", "DY",
            "Day", "Dy", "day", "dy", "D", "d", "ID", "id", "MONTH", "MON",
            "Month", "Mon", "month", "mon", "TZ", "tz", "TZH", "tzh", "TZM",
            "tzm", "OF", "of"}

# a keyword with each prefix and suffix, in one format, split by |
FORMATS = ["%s|FM%s|%sTH|%sth|FM%sth|TM%s|fm%sSP|tm%sTH" % ((k,) * 8)
           for k in KEYWORDS]
# the formats of a zone: the time, the zone, and the extras
ZONE_FORMATS = [f for f in FORMATS
                if f.split("|")[0] in ("TZ", "tz", "TZH", "TZM", "OF", "of",
                                       "HH24", "HH12", "MI", "SS", "US", "AM",
                                       "DD", "Day", "IW", "J")]
EXTRAS = [
    "YYYY-MM-DD HH24:MI:SS.US", "Day, DD Month YYYY", "FMDay, FMDD FMMonth YYYY",
    "HH12:MI:SS AM", "YYYY-MM-DD\"T\"HH24:MI:SS.FF3OF", "\"text\" YYYY",
    "\"a\\\"b\" DD", "\\\"YYYY\\\"", "\"\\\\\" DD", "\"unterminated YYYY",
    "FM", "TM", "FMx", "TMx", "FMTMMonth", "TMFMMonth", "x YYYY y", "日本 YYYY年MM月",
    "é DD ü", "YYYYMMDD", "YYYYYYYY", "MMMM", "DDDD", "HHHH", "MIMI", "SSSSSS",
    "IYYYIW", "Y,YYYY", "RMRM", "FMRM.", "rmFMrm", "WWW", "J-J", "CCCC",
    "Q-Q", "\"", "\"\"", "\\", "\\\\", "  YYYY  ", "YYYY\tMM\nDD", "--", "X",
    "Th", "TH", "th", "SP", "DDTHDDth", "FMHH12FMMI", "HH24MISS", "A.M.P.M.am",
    "FF1FF2FF3FF4FF5FF6", "MSUS", "SSSSS.US", "TZ TZH:TZM OF", "tz-of",
    "IDDD IYYY IW ID", "YYYYth", "Ddd", "dAY", "mONTH", "Mon.Mon",
]
FORMATS += EXTRAS
ZONE_FORMATS += [f for f in EXTRAS if "TZ" in f or "OF" in f or "tz" in f]


def hexs(s):
    return s.encode().hex() or "-"


def lit(s):
    return "'" + s.replace("'", "''") + "'"


sql = []
for ts in TIMESTAMPS:
    v = "timestamp %s" % lit(ts)
    us = "(extract(epoch from %s) * 1000000 - 946684800000000)::int8" % v
    for f in FORMATS:
        sql.append("SELECT 'tochar.ts ' || %s || ' %s => ' || coalesce(nullif(encode(convert_to(to_char(%s, %s), 'UTF8'), 'hex'), ''), '-');"
                   % (us, hexs(f), v, lit(f)))
for z in ZONES:
    sql.append("SET timezone = %s;" % lit(z))
    for ts in ZONE_TIMES:
        v = "timestamptz %s" % lit(ts)
        us = "(extract(epoch from %s) * 1000000 - 946684800000000)::int8" % v
        for f in ZONE_FORMATS:
            sql.append("SELECT 'tochar.tz %s ' || %s || ' %s => ' || coalesce(nullif(encode(convert_to(to_char(%s, %s), 'UTF8'), 'hex'), ''), '-');"
                       % (z, us, hexs(f), v, lit(f)))
sql.append("RESET timezone;")
for iv in INTERVALS:
    for f in FORMATS:
        if f.split("|")[0] in CALENDAR or any(c in f for c in ("Day", "Month", "TZ", "OF", "A.D", "B.C", "DY", "Dy", "dy", "day", "MON", "Mon", "mon")):
            continue
        sql.append("SELECT 'tochar.iv %s %s => ' || coalesce(nullif(encode(convert_to(to_char(interval %s, %s), 'UTF8'), 'hex'), ''), '-');"
                   % (hexs(iv), hexs(f), lit(iv), lit(f)))

res = subprocess.run(["psql", "-X", "-A", "-t", "-q", "-v", "ON_ERROR_STOP=0"] + sys.argv[1:],
                     input="\n".join(sql), capture_output=True, text=True)
sys.stdout.write(res.stdout)
if res.stderr:
    sys.stderr.write(res.stderr)

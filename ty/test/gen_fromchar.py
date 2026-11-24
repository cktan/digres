#!/usr/bin/env python3
# Copyright (c) 2026 CK Tan
# SPDX-License-Identifier: MIT
#
# gen_fromchar.py -- write test/fromchar_known.txt: what PostgreSQL's
# to_timestamp and to_date give, and the errors they raise, for many texts
# and formats. The results are PostgreSQL's own, so they are taken from a
# digres built before the reverse parse moved into ty:
#
#   test/gen_fromchar.py PSQL_ARGS... > test/fromchar_known.txt
#
# A line is "fromchar.KIND TEXTHEX FMTHEX => RESULT": KIND is ts
# (to_timestamp in UTC) or date (to_date), RESULT the timestamp's
# microseconds since 2000-01-01 or the date's days since then, or "E" and
# the hex of the error's message, detail and hint, joined by |.

import random
import subprocess
import sys

random.seed(20261123)

FORMATS = [
    "YYYY-MM-DD HH24:MI:SS", "YYYY-MM-DD HH24:MI:SS.US", "YYYY-MM-DD HH24:MI:SS.MS",
    "YYYY-MM-DD HH12:MI:SS AM", "YYYY-MM-DD HH:MI:SS P.M.", "DD Mon YYYY",
    "DD Month YYYY", "FMDD FMMonth YYYY", "Day DD Mon YYYY", "Dy, DD Mon YYYY",
    "YYYYMMDD", "YYYYMMDDHH24MISS", "YY-MM-DD", "Y-MM-DD", "YYY-MM-DD",
    "Y,YYY-MM-DD", "IYYY-IW-ID", "IYYY-IDDD", "YYYY-DDD", "YYYY WW", "YYYY-MM W",
    "J", "CC YY", "CC", "CC-MM-DD", "YYYY BC", "YYYY-MM-DD AD", "B.C. YYYY",
    "RM YYYY", "rm-YYYY", "MM/DD/YYYY", "DD.MM.YYYY", "HH24:MI", "HH24:MI:SS TZH:TZM",
    "YYYY-MM-DD HH24:MI:SS OF", "YYYY-MM-DD HH24:MI:SSTZH", "YYYY-MM-DD HH24:MI:SS TZ",
    "SSSSS", "YYYY-MM-DD SSSS", "FXYYYY-MM-DD", "FXYYYY MM DD", "FF1", "FF3", "FF6",
    "YYYY-MM-DD HH24:MI:SS.FF2", "YYYYTH MMth DDTH", "Q YYYY-MM", "\"Y\"YYYY",
    "YYYY\"-\"MM", "YYYY-MM-DD\"T\"HH24:MI", "MONTH DD, YYYY", "month dd yyyy",
    "TMMonth YYYY", "YYYY-MM-DD HH24:MI:SS.FF6 TZH",
]
TIMESTAMPS = ["2024-02-29 13:05:09.123456+00", "1999-12-31 23:59:59.999999+00",
              "2000-01-01 00:00:00+00", "0044-03-15 12:00:00+00 BC",
              "2008-12-29 08:08:08.5+00", "2010-01-03 18:45:00+00",
              "1582-10-15 06:30:00+00", "9999-12-31 11:59:59+00"]

PERTURB = [
    lambda s: s, lambda s: " " + s, lambda s: s + " ", lambda s: s.replace(" ", "  "),
    lambda s: s.replace("-", "/"), lambda s: s[:-1], lambda s: s[:len(s) // 2],
    lambda s: s.replace("0", "O", 1), lambda s: s.upper(), lambda s: s.lower(),
    lambda s: s + "x", lambda s: s.replace(" ", ""), lambda s: "-" + s,
]

CASES = [
    ("0097/Feb/16 --> 08:14:30", "YYYY/Mon/DD --> HH:MI:SS"),
    ("97/2/16 8:14:30", "FMYYYY/FMMM/FMDD FMHH:FMMI:FMSS"),
    ("2011$03!18 23_38_15", "YYYY-MM-DD HH24:MI:SS"),
    ("1985 January 12", "YYYY FMMonth DD"),
    ("1985 FMMonth 12", "YYYY \"FMMonth\" DD"),
    ("1985 \\ 12", "YYYY \\\\ DD"),
    ("My birthday-> Year: 1976, Month: May, Day: 16", "\"My birthday-> Year:\" YYYY, \"Month:\" FMMonth, \"Day:\" DD"),
    ("1,582nd VIII 21", "Y,YYYth FMRM DD"),
    ("15 \"text between quote marks\" 98 54 45", "HH24 \"\\\"text between quote marks\\\"\" YY MI SS"),
    ("05121445482000", "MMDDHH24MISSYYYY"),
    ("2000January09Sunday", "YYYYFMMonthDDFMDay"),
    ("97/Feb/16", "YYMonDD"), ("97/Feb/16", "YY:Mon:DD"), ("97/Feb/16", "FXYY:Mon:DD"),
    ("97/Feb/16", "FXYY/Mon/DD"), ("19971116", "YYYYMMDD"),
    ("20000-1116", "YYYY-MMDD"), ("1997 AD 11 16", "YYYY BC MM DD"),
    ("1997 BC 11 16", "YYYY BC MM DD"), ("1997 A.D. 11 16", "YYYY B.C. MM DD"),
    ("1997 B.C. 11 16", "YYYY B.C. MM DD"), ("9-1116", "Y-MMDD"),
    ("95-1116", "YY-MMDD"), ("995-1116", "YYY-MMDD"),
    ("2005426", "YYYYWWD"), ("2005300", "YYYYDDD"), ("2005527", "IYYYIWID"),
    ("005527", "IYYIWID"), ("05527", "IYIWID"), ("5527", "IIWID"),
    ("2005364", "IYYYIDDD"), ("20050302", "YYYYMMDD"),
    ("2005 03 02", "YYYYMMDD"), (" 2005 03 02", "YYYYMMDD"), ("  20050302", "YYYYMMDD"),
    ("2011-12-18 11:38 AM", "YYYY-MM-DD HH12:MI PM"),
    ("2011-12-18 11:38 PM", "YYYY-MM-DD HH12:MI PM"),
    ("2011-12-18 11:38 A.M.", "YYYY-MM-DD HH12:MI P.M."),
    ("2011-12-18 11:38 P.M.", "YYYY-MM-DD HH12:MI P.M."),
    ("2011-12-18 11:38 +05", "YYYY-MM-DD HH12:MI TZH"),
    ("2011-12-18 11:38 -05", "YYYY-MM-DD HH12:MI TZH"),
    ("2011-12-18 11:38 +05:20", "YYYY-MM-DD HH12:MI TZH:TZM"),
    ("2011-12-18 11:38 -05:20", "YYYY-MM-DD HH12:MI TZH:TZM"),
    ("2011-12-18 11:38 20", "YYYY-MM-DD HH12:MI TZM"),
    ("2011-12-18 11:38 EST", "YYYY-MM-DD HH12:MI TZ"),
    ("2011-12-18 11:38 -05", "YYYY-MM-DD HH12:MI TZ"),
    ("2011-12-18 11:38 +01:30", "YYYY-MM-DD HH12:MI TZ"),
    ("2011-12-18 11:38 MSK", "YYYY-MM-DD HH12:MI TZ"),
    ("2011-12-18 00:00 LMT", "YYYY-MM-DD HH24:MI TZ"),
    ("2011-12-18 11:38ESTFOO24", "YYYY-MM-DD HH12:MITZFOOSS"),
    ("2011-12-18 11:38-05FOO24", "YYYY-MM-DD HH12:MITZFOOSS"),
    ("2011-12-18 11:38 JUNK", "YYYY-MM-DD HH12:MI TZ"),
    ("2011-12-18 11:38 ...", "YYYY-MM-DD HH12:MI TZ"),
    ("2011-12-18 11:38 -05", "YYYY-MM-DD HH12:MI OF"),
    ("2011-12-18 11:38 +01:30", "YYYY-MM-DD HH12:MI OF"),
    ("2011-12-18 11:38 +xyz", "YYYY-MM-DD HH12:MI OF"),
    ("2011-12-18 11:38 +01:xyz", "YYYY-MM-DD HH12:MI OF"),
    ("2018-11-02 12:34:56.025", "YYYY-MM-DD HH24:MI:SS.MS"),
    ("2018-11-02 12:34:56", "YYYY-MM-DD HH24:MI:SS.FF1"),
    ("2018-11-02 12:34:56.1234567", "YYYY-MM-DD HH24:MI:SS.FF7"),
    ("44-02-01 11:12:13 BC", "YYYY-MM-DD HH24:MI:SS BC"),
    ("-44-02-01 11:12:13", "YYYY-MM-DD HH24:MI:SS"),
    ("-44-02-01 11:12:13 BC", "YYYY-MM-DD HH24:MI:SS BC"),
    ("2011-12-18 23:38:15", "YYYY-MM-DD  HH24:MI:SS"),
    ("2011-12-18  23:38:15", "YYYY-MM-DD  HH24:MI:SS"),
    ("2000+   JUN", "YYYY/MON"), ("  2000 +JUN", "YYYY/MON"), (" 2000 +JUN", "YYYY//MON"),
    ("2000  +JUN", "YYYY//MON"), ("2000 + JUN", "YYYY MON"), ("2000 ++ JUN", "YYYY  MON"),
    ("2000 + + JUN", "YYYY  MON"), ("2000 + + JUN", "YYYY   MON"),
    ("2000 -10", "YYYY TZH"), ("2000 -10", "YYYY  TZH"),
    ("2005527", "YYYYIWID"), ("19971", "YYYYMMDD"), ("19971)24", "YYYYMMDD"),
    ("Friday 1-January-1999", "DY DD MON YYYY"), ("Friday 1-January-1999", "DAY DD MON YYYY"),
    ("Friday 1-January-1999", "DY DD MON YYY"), ("1997-11-Jan-16", "YYYY-MM-Mon-DD"),
    ("199711xy", "YYYYMMDD"), ("10000000000", "FMYYYY"), ("2016-06-13 25:00:00", "YYYY-MM-DD HH24:MI:SS"),
    ("2016-06-13 15:60:00", "YYYY-MM-DD HH24:MI:SS"), ("2016-06-13 15:50:60", "YYYY-MM-DD HH24:MI:SS"),
    ("2016-06-13 15:50:55", "YYYY-MM-DD HH24:MI:SS"), ("2016-06-13 15:50:55", "YYYY-MM-DD HH:MI:SS"),
    ("2016-13-01 15:50:55", "YYYY-MM-DD HH24:MI:SS"), ("2016-02-30 15:50:55", "YYYY-MM-DD HH24:MI:SS"),
    ("2016-02-29 15:50:55", "YYYY-MM-DD HH24:MI:SS"), ("2015-02-29 15:50:55", "YYYY-MM-DD HH24:MI:SS"),
    ("2015-02-11 86000", "YYYY-MM-DD SSSS"), ("2015-02-11 86400", "YYYY-MM-DD SSSS"),
    ("2015-02-11 86000", "YYYY-MM-DD SSSSS"), ("2015-02-11 86400", "YYYY-MM-DD SSSSS"),
    ("1-1-1 0", "Y-M-D HH24"), ("2020-03-06 00:00:00.123", "YYYY-MM-DD HH24:MI:SS.FF3"),
    ("1-02-03", "YYYY-MM-DD"), ("2020-04-30 12", "YYYY-MM-DD HH"), ("2020-04-30 0", "YYYY-MM-DD HH12"),
    ("20 01 2", "CC YY D"), ("2018 Q1", "YYYY \"Q\"Q"), ("2458872", "J"),
    ("+15 -3", "TZH TZM"), ("2000 99", "YYYY TZH"), ("2000 15:60", "YYYY TZH:TZM"),
    ("1 4 1902", "Q MM YYYY"), ("3 4 21 01", "W MM CC YY"), ("2458872", "J DD"),
    ("1 Jan 2000", "DD MON YYYY"), ("1 Jan 2000", "DD Month YYYY"), ("1 Janx 2000", "DD MON YYYY"),
    ("2000-01-01 1 Monday", "YYYY-MM-DD D Day"), ("2000 300", "IYYY DDD"), ("2000 1 1", "YYYY IW D"),
    ("2000 Jan 1 1", "YYYY Mon DD IW"), ("12", "YYYY"), ("", "YYYY"), ("2000", ""),
    ("1999 999 999", "YYYY DDD DDD"), ("1999 998 999", "YYYY DDD DDD"), ("999", "DDD"),
    ("12345678901", "YYYY"), ("2000 2147483648", "YYYY MS"), ("2000-01-01", "YYYY-MM-DDFX"),
    ("2147483647,999", "Y,YYY"), ("2147484,999", "Y,YYY"), ("x,999", "Y,YYY"),
    ("2000 11", "YYYY HHth"), ("2000 23", "YYYY HH24TH"), ("12 AM", "HH12 AM"), ("12 PM", "HH12 PM"),
    ("13 PM", "HH12 PM"), ("0 AM", "HH12 AM"),
]


def hexs(s):
    return s.encode().hex() or "-"


def lit(s):
    return "'" + s.replace("'", "''") + "'"


# the round trips: to_char then back, with perturbations
pairs = []
for f in FORMATS:
    for ts in TIMESTAMPS:
        pairs.append(("rt", ts, f))
for txt, f in CASES:
    pairs.append(("x", txt, f))

sql = ["SET timezone = 'UTC';", "\\set VERBOSITY default"]
cases = []
for kind, a, f in pairs:
    texts = []
    if kind == "rt":
        texts = [None]
    else:
        texts = [a]
    cases.append((kind, a, f))
for i, (kind, a, f) in enumerate(cases):
    src = "to_char(timestamptz %s, %s)" % (lit(a), lit(f)) if kind == "rt" else lit(a)
    sql.append("\\echo @@T %d" % i)
    sql.append("SELECT %s;" % src)

res = subprocess.run(["psql", "-X", "-A", "-t", "-q", "-v", "ON_ERROR_STOP=0"] + sys.argv[1:],
                     input="\n".join(sql), capture_output=True, text=True)
texts = {}
cur = None
for line in res.stdout.splitlines():
    if line.startswith("@@T "):
        cur = int(line[4:])
    elif cur is not None and cur not in texts:
        texts[cur] = line

# every text, perturbed for the round trips, through to_timestamp and to_date
runs = []
for i, (kind, a, f) in enumerate(cases):
    if i not in texts:
        continue
    base = texts[i]
    variants = [p(base) for p in PERTURB] if kind == "rt" else [base]
    for v in dict.fromkeys(variants):
        runs.append((v, f))
sql = ["SET timezone = 'UTC';"]
for j, (v, f) in enumerate(runs):
    sql.append("\\echo @@R %d ts" % j)
    sql.append("SELECT (extract(epoch from to_timestamp(%s, %s)) * 1000000 - 946684800000000)::int8;" % (lit(v), lit(f)))
    sql.append("\\echo @@R %d date" % j)
    sql.append("SELECT to_date(%s, %s) - date '2000-01-01';" % (lit(v), lit(f)))
sql.append("\\echo @@END")
res = subprocess.run("psql -X -A -t -q -v ON_ERROR_STOP=0 %s 2>&1" % " ".join(sys.argv[1:]),
                     shell=True, input="\n".join(sql), capture_output=True, text=True)
out = {}
key = None
buf = []


def flush():
    if key is None:
        return
    j, kind = key
    if buf and buf[0].startswith("psql:") or (buf and buf[0].startswith("ERROR:")):
        msg = detail = hint = ""
        for b in buf:
            b2 = b.split(": ", 2)[-1] if b.startswith("psql:") else b
            if "ERROR:  " in b:
                msg = b.split("ERROR:  ", 1)[1]
            elif b.startswith("DETAIL:  "):
                detail = b[9:]
            elif b.startswith("HINT:  "):
                hint = b[7:]
        out[key] = "E " + "|".join(hexs(x) for x in (msg, detail, hint))
    elif buf:
        out[key] = buf[0]


for line in res.stdout.splitlines():
    if line.startswith("@@R ") or line == "@@END":
        flush()
        buf = []
        if line == "@@END":
            key = None
            continue
        _, j, kind = line.split()
        key = (int(j), kind)
    else:
        buf.append(line)
for j, (v, f) in enumerate(runs):
    for kind in ("ts", "date"):
        if (j, kind) in out:
            print("fromchar.%s %s %s => %s" % (kind, hexs(v), hexs(f), out[(j, kind)]))

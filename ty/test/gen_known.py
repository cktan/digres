#!/usr/bin/env python3
# Copyright (c) 2026 CK Tan
# SPDX-License-Identifier: MIT
#
# gen_known.py -- write test/known.txt: what PostgreSQL's own code does with
# inputs whose rules are scanf's, a locale's or the like, too many to rewrite
# as a reference. The values are PostgreSQL's, so they are taken from a digres
# built before the type moved into ty (any server with PostgreSQL's code
# gives them):
#
#   test/gen_known.py PSQL_ARGS... > test/known.txt
#
# A line is "OP HEXTEXT => RESULT": HEXTEXT is the input text as hex digits
# (or - if empty), and RESULT the canonical text the server printed, or
# "E INVALID" or "E RANGE" from the SQLSTATE of its error. The inputs are
# generated here, with a fixed seed.

import random
import subprocess
import os
import sys

random.seed(20261121)

OPS = {}


def op(name, sqltype):
    def deco(fn):
        OPS[name] = (sqltype, fn)
        return fn
    return deco


def hexbytes(n):
    return "".join(random.choice("0123456789abcdefABCDEF") for _ in range(n))


def like_pairs():
    """Pairs of a text and a LIKE pattern, with the cases where an escape
    at the end of the pattern is an error only if the match gets to it."""
    out = [("abc", "a%"), ("abc", "a\\"), ("abc", "x\\"), ("abc", "%\\"), ("abc", "abc\\"), ("", "\\"), ("", ""),
           ("", "%"), ("a", "\\"), ("a", "a\\"), ("ab", "%b\\"), ("ab", "_\\"), ("a\\", "a\\\\"),
           ("abc", "%\\b"), ("a%c", "a\\%c"), ("a_c", "a\\_c"), ("abc", "a\\%c"), ("é", "_"), ("日本語", "%本%"),
           ("😀x", "_x"), ("😀x", "__"), ("😀x", "%x"), ("é", "\xc3_"), ("abab", "%ab%ab"), ("aaaa", "%a%a%a%a"),
           ("aaaa", "%a%a%a%a%a"), ("abc", "%%%"), ("abc", "_%_%_"), ("abc", "_%_%_%_"), ("a" * 40, "%a" * 20 + "b"),
           ("x" * 30, "%x" * 15), ("abc", "a%\\"), ("abc", "ab\\c"), ("abc", "\\a%")]
    return out


def like_ops():
    return [(t, p) for t, p in like_pairs()]


OPS["like.pair"] = ("like", like_ops)



def run(psql_args, sql):
    r = subprocess.run(["psql", "-X", "-A", "-t", "-q", "-F", ",", "-v", "VERBOSITY=sqlstate"] + psql_args + ["-c", sql],
                       capture_output=True, text=True)
    if r.returncode == 0:
        return r.stdout.strip()
    err = r.stderr
    if "22P02" in err:
        if "cidr" in sql:
            # a cidr with host bits is the same SQLSTATE: tell it by its message
            r2 = subprocess.run(["psql", "-X", "-A", "-t", "-q"] + psql_args + ["-c", sql], capture_output=True, text=True)
            if "invalid cidr value" in r2.stderr:
                return "E RANGE"
        return "E INVALID"
    if "22003" in err:
        return "E RANGE"
    if "22025" in err:
        return "E INVALID"
    if "54001" in err:
        return "E RANGE"
    return "E " + err.strip().replace(" ", "_")[:40]


def datetime_text_cases():
    """The cases of the text of date, time, timestamp and interval: a kind,
    a date style and order or an interval style, and a value."""
    random.seed(20261122)
    dates = [0, -1, 1, 366, -366, 8000, -2451545, 2147483494 - 2451545 - 1, 3652059, 2451911, -730119, -730120, 1460, 10957]
    dates += [random.randint(-2451545, 2145031948) for _ in range(30)] + [random.randint(-100000, 100000) for _ in range(20)]
    times = [0, 1, 10, 100, 1000, 100000, 999999, 1000000, 3600500000, 45296789012, 86399999999, 86400000000, 12345678, 60000000, 3600000000]
    times += [random.randint(0, 86400000000) for _ in range(20)]
    tss = [0, 1, -1, 86400000000, -86400000000, -211813488000000000, -211813488000000000 + 1, 9223371331200000000 - 1, 86399999999,
           725803200000000, 1000000000000000000, -200000000000000000, 3155760000000000, -3155760000000000, 500, 1500000, 10000, 120000]
    tss += [random.randint(-211813488000000000, 9223371331200000000 - 1) for _ in range(20)] + [random.randint(-10 ** 15, 10 ** 15) for _ in range(30)]
    ivs = [(0, 0, 0), (1, 0, 0), (0, 1, 0), (0, 0, 1), (-1, 0, 0), (0, -1, 0), (0, 0, -1), (1, 1, 1), (-1, -1, -1), (1, -1, 1), (-1, 1, -1),
           (14, 3, 4271000000), (-14, -3, -4271000000), (0, 0, 90000000000), (0, 0, -90000000), (3, 0, 500000), (0, 5, -500000), (12, 0, 0), (-12, 0, 0),
           (0, 0, 3600000000), (0, 0, 59999999), (0, 0, -59999999), (2, 0, 0), (0, 30, 0), (25, 40, 1234567890123), (0, 0, 9223372036854775807), (0, 0, -9223372036854775807),
           (2147483647, 2147483647, 0), (-2147483647, -2147483647, 0), (1, 2, 3), (-1, 2, 3), (1, -2, 3), (1, 2, -3), (0, 1, -1000000)]
    ivs += [(random.randint(-100, 100), random.randint(-100, 100), random.randint(-10 ** 11, 10 ** 11)) for _ in range(30)]
    return dates, times, tss, ivs


def run_batch(psql_args, sets, queries):
    """Run set statements and queries in one psql, and return the lines."""
    script = "".join(sets) + "".join(q + ";\n" for q in queries)
    r = subprocess.run(["psql", "-X", "-A", "-t", "-q"] + psql_args, input=script, capture_output=True, text=True)
    return r.stdout.rstrip("\n").split("\n"), r.stderr


def iso_interval(v):
    """An interval in the ISO 8601 form, whose fields have signs of their own."""
    m, d, t = v
    if m == 0 and d == 0:
        return "%d microseconds" % t  # exact: the seconds of ISO are a float
    sign = "-" if t < 0 else ""
    t = abs(t)
    return "P%dM%dDT%s%d.%06dS" % (m, d, sign, t // 1000000, t % 1000000)


def main_datetime(args):
    dates, times, tss, ivs = datetime_text_cases()
    dstyles = [("Postgres", "MDY", 0, 2), ("Postgres", "DMY", 0, 1), ("ISO", "MDY", 1, 2), ("ISO", "YMD", 1, 0), ("SQL", "MDY", 2, 2), ("SQL", "DMY", 2, 1),
               ("German", "DMY", 3, 1), ("German", "MDY", 3, 2)]
    for name, order, sty, ordn in dstyles:
        sets = ["set datestyle = '%s, %s';\n" % (name, order)]
        out, err = run_batch(args, sets, ["select (date '2000-01-01' + %d)::text" % d for d in dates])
        assert len(out) == len(dates), err
        for d, t in zip(dates, out):
            print("date.out %d %d %d => %s" % (sty, ordn, d, t.encode().hex()))
        out, err = run_batch(args, sets, ["select (time '00:00' + interval '%d microseconds')::text" % t for t in times if t < 86400000000])
        for t, o in zip([t for t in times if t < 86400000000], out):
            print("time.out %d %d %d => %s" % (sty, ordn, t, o.encode().hex()))
        out, err = run_batch(args, sets, ["select (timestamp '2000-01-01' + interval '%d microseconds')::text" % t for t in tss])
        assert len(out) == len(tss), err
        for t, o in zip(tss, out):
            print("ts.out %d %d %d => %s" % (sty, ordn, t, o.encode().hex()))
    ists = [("postgres", 0), ("postgres_verbose", 1), ("sql_standard", 2), ("iso_8601", 3)]
    for name, sty in ists:
        sets = ["set intervalstyle = '%s';\n" % name]
        out, err = run_batch(args, sets, ["select (interval '%s')::text" % iso_interval(v) for v in ivs])
        assert len(out) == len(ivs), err
        for v, o in zip(ivs, out):
            print("iv.out %d %d:%d:%d => %s" % (sty, v[0], v[1], v[2], o.encode().hex()))


def interval_inputs():
    """Interval text: the postgres, sql and ISO 8601 forms, the units and
    their abbreviations, signs, fractions, ago, and many bad ones."""
    random.seed(20261123)
    out = ["1 day", "1 year 2 months 3 days 04:05:06", "@ 1 minute", "-1 days +02:03:04", "1:30", "1:30:45.5", "P1Y2M3DT4H5M6S",
           "P0001-02-03T04:05:06", "1-2", "-1-2", "1 day ago", "ago", "infinity", "-infinity", "+infinity", "1.5 years", "0.5 months",
           "1.2 weeks", "1e3 seconds", "1,000 days", "", " ", "x", "1", "1 2", "1 day 2", "00:00:00", "-00:00:01", "100:00:00", "1:2:3:4",
           "04:05:06.789", "04:05:06,789", "1 microsecond", "1 usec", "1 us", "1 millisecond", "2 ms", "3 msec", "1 second", "1 s", "1 sec", "2 secs",
           "1 minute", "1 m", "1 min", "2 mins", "1 hour", "1 h", "1 hr", "2 hrs", "1 day", "1 d", "2 days", "1 week", "1 w", "2 weeks",
           "1 month", "1 mon", "2 mons", "3 months", "1 year", "1 y", "1 yr", "2 yrs", "3 years", "1 decade", "2 decades", "1 dec", "1 century", "1 c",
           "1 cent", "2 centuries", "1 millennium", "1 mil", "2 millennia", "1 ms 1 us", "1 year 1 year", "-1 year +2 months", "1 year -2 months",
           "+1 day", "- 1 day", "1 day -", "1.5 days", "1.5 hours", "0.000001 seconds", "0.0000005 seconds", "9223372036854775807 microseconds",
           "9223372036854775808 microseconds", "2147483647 months", "2147483648 months", "178000000 years", "178956970 years", "-178956970 years",
           "178956971 years", "2147483647 days", "2147483648 days", "596523 hours", "2562047788 hours", "2562047789 hours",
           "1 day 1 hour ago", "ago 1 day", "@ 1 day ago", "@ 1 day", "@ -1 day", "1 day at", "P", "PT", "P1D", "PT1H", "P1W", "P1Y", "P-1Y", "P1.5Y", "PT0.5S", "P1M", "PT1M", "P1Y2M3W4DT5H6M7.5S",
           "P0001-02-03", "P0001-02-03T04:05:06.5", "P00010203T040506", "P1Y2M", "PT1H30M", "p1d", "P 1D", "P1D ", "PT1H1H", "1-2 3 4:5:6", "-1-2 +3 -4:5:6",
           "1 year 2 months 3 days 4 hours 5 minutes 6 seconds", "1 day 25:00:00", "1 day 00:60:00", "00:00:60", "24:00:00", "1:00", "1::", ":1", "1:", "::",
           "1 hour 1:00", "1 mon 1 mons", "01:02", "01:02:03.123456", "01:02:03.1234567", "-01:02", "+01:02:03", "1 +", "1 -", "+-1 day", "--1 day", "1\tday", "1\nday",
           "1 DAY", "1 Day", "INFINITY", "Ago"]
    units = ["microsecond", "usec", "us", "millisecond", "ms", "second", "sec", "s", "minute", "min", "m", "hour", "hr", "h", "day", "d", "week", "w",
             "month", "mon", "year", "yr", "y", "decade", "century", "millennium", "days", "hours", "months", "years", "foo", "mil"]
    for _ in range(300):
        parts = []
        for _ in range(random.randint(1, 4)):
            sign = random.choice(["", "", "-", "+"])
            num = random.choice([str(random.randint(0, 100)), "%d.%d" % (random.randint(0, 20), random.randint(0, 99)), str(random.randint(0, 3000000)), "0"])
            parts.append("%s%s %s" % (sign, num, random.choice(units)))
        t = " ".join(parts)
        if random.random() < 0.15:
            t += " ago"
        if random.random() < 0.15:
            t += " %02d:%02d:%02d" % (random.randint(0, 30), random.randint(0, 70), random.randint(0, 70))
        if random.random() < 0.1:
            t = "@ " + t
        out.append(t)
    for _ in range(80):
        out.append("P%s%s%s%s" % (random.choice(["", "1Y", "-2Y", "1.5Y"]), random.choice(["", "3M", "-1M"]), random.choice(["", "4D", "2W"]),
                                  random.choice(["", "T5H", "T6M", "T7.25S", "T1H2M3S", "T-1H"])))
    return out


def main_interval(args):
    inputs = interval_inputs()
    for name, sty in (("postgres", 0), ("sql_standard", 2)):
        sets = ["set intervalstyle = '%s';\n" % name]
        for t in inputs:
            lit = t.replace("'", "''").replace("\\", "\\\\").replace("\t", "\\t").replace("\n", "\\n")
            script = "".join(sets) + "select ((E'%s')::interval)::text;\n" % lit
            r = subprocess.run(["psql", "-X", "-A", "-t", "-q", "-v", "VERBOSITY=sqlstate"] + args, input=script, capture_output=True, text=True)
            if r.returncode == 0 and "ERROR" not in r.stderr:
                res = r.stdout.strip().encode().hex() or "-"
            else:
                err = r.stderr
                res = "E 1" if "22008" in err else "E -4" if "22015" in err else "E -1" if "22007" in err else "E ?" + err.strip().replace(" ", "_")[:30]
            print("iv.parse %d %s => %s" % (sty, t.encode().hex() or "-", res))


def main_trunc(args):
    random.seed(20261124)
    tss = [0, 1, -1, 86400000000, -86400000000, -211813488000000000, -211813488000000000 + 1, 9223371331200000000 - 1, 86399999999,
           725803200000000, 1000000000000000000, -200000000000000000, 3155760000000000, -3155760000000000, 500, 1500000, 10000, 120000,
           -2208988800000000, -62135596800000000 - 86400000000, 31622400000000, 63113904000000000, 7 * 86400000000, 1234567890123456]
    tss += [random.randint(-211813488000000000, 9223371331200000000 - 1) for _ in range(25)] + [random.randint(-10 ** 15, 10 ** 15) for _ in range(40)]
    units = ["microseconds", "microsecond", "milliseconds", "millisecond", "second", "seconds", "minute", "hour", "day", "days", "week", "month", "months", "quarter",
             "year", "years", "decade", "century", "millennium", "dow", "doy", "epoch", "julian", "timezone", "foo", "", "ms", "us", "isoyear", "isodow"]
    sets = ["set datestyle = 'ISO, YMD';\n", "set intervalstyle = 'postgres';\n"]
    lit = lambda t: "(timestamp '2000-01-01' + interval '%d microseconds')" % t
    qs, keys = [], []
    for u in units:
        for t in tss:
            qs.append("select date_trunc('%s', %s)::text" % (u, lit(t)))
            keys.append(("ts.trunc", u, t))
    for t in ("infinity", "-infinity"):
        for u in units[:14]:
            qs.append("select date_trunc('%s', '%s'::timestamp)::text" % (u, t))
            keys.append(("ts.trunc", u, 9223372036854775807 if t == "infinity" else -9223372036854775808))
    for a_ in tss[:40]:
        for b_ in random.sample(tss, 4):
            qs.append("select age(%s, %s)::text" % (lit(a_), lit(b_)))
            keys.append(("ts.age", a_, b_))
    strides = [(0, 1, 0), (0, 0, 3600000000), (0, 0, 900000000), (0, 7, 0), (0, 0, 1), (0, 0, 1000000), (0, 15, 43200000000), (0, 0, 0), (0, -1, 0), (1, 0, 0), (0, 0, 7)]
    origins = [0, 86400000000 * 3 + 5, -31622400000000, 725803200000000]
    for st in strides:
        for t in random.sample(tss, 8):
            for o in origins[:3]:
                qs.append("select date_bin(make_interval(0, %d, 0, %d, 0, 0, 0) + interval '%d microseconds', %s, %s)::text" % (st[0], st[1], st[2], lit(t), lit(o)))
                keys.append(("ts.bin", "%d:%d:%d" % st, t, o))
    ivs = [(0, 0, 0), (0, 0, 1), (1, 0, 0), (-1, 0, 0), (13, 40, 3661000001), (-13, -40, -3661000001), (14, 0, 0), (25 * 12 + 7, 1, 0), (12 * 1000 + 5, 400, 0),
           (-12 * 1000 - 5, -400, -1), (7, 0, 0), (-7, 0, 0), (0, 15, 90000000000), (0, 0, 86399999999), (0, 0, -86399999999), (5, 100, -5), (-4, -3, -1500500),
           (2147483646, 2147483647, 9223372036854775806), (-2147483647, -2147483647, -9223372036854775807), (123456, 0, 0), (-123456, 0, 0), (1, 0, 3599999999)]
    for u in units:
        for m, d, t in ivs:
            qs.append("select date_trunc('%s', interval '%d months %d days %d microseconds')::text" % (u, m, d, t))
            keys.append(("iv.trunc", u, "%d:%d:%d" % (m, d, t)))
    for u in units[:14]:
        for t in ("infinity", "-infinity"):
            qs.append("select date_trunc('%s', interval '%s')::text" % (u, t))
            keys.append(("iv.trunc", u, "inf" if t == "infinity" else "-inf"))
    if os.environ.get("ONLY_IV"):
        both = [(q, k) for q, k in zip(qs, keys) if k[0] == "iv.trunc"]
        qs, keys = [b[0] for b in both], [b[1] for b in both]
    # one psql, with a statement per query; errors go to stderr in order: use a marker for each
    out = []
    for q, k in zip(qs, keys):
        script = "".join(sets) + q + ";\n"
        r = subprocess.run(["psql", "-X", "-A", "-t", "-q", "-v", "VERBOSITY=sqlstate"] + args, input=script, capture_output=True, text=True)
        if r.returncode == 0 and "ERROR" not in r.stderr:
            res = r.stdout.strip().encode().hex() or "-"
        else:
            e = r.stderr
            res = "E INVALID" if "0A000" in e else "E UNKNOWN" if "22023" in e else "E RANGE" if "22008" in e else "E FEATURE" if "0A000" in e else "E ?" + e.strip().replace(" ", "_")[:30]
        if k[0] == "ts.trunc":
            print("ts.trunc %s %d => %s" % (k[1].encode().hex() or "-", k[2], res))
        elif k[0] == "iv.trunc":
            print("iv.trunc %s %s => %s" % (k[1].encode().hex() or "-", k[2], res))
        elif k[0] == "ts.age":
            print("ts.age %d %d => %s" % (k[1], k[2], res))
        else:
            print("ts.bin %s %d %d => %s" % (k[1], k[2], k[3], res))


def main_part(args):
    random.seed(20261125)
    tss = [0, 1, -1, 86400000000, -86400000000, -211813488000000000, -211813487999999999, 9223371331200000000 - 1, 86399999999,
           725803200000000, 1000000000000000000, -200000000000000000, 3155760000000000, -3155760000000000, 500, 1500000, 10000, 120000,
           -2208988800000000, -62135596800000000, 31622400000000, 63113904000000000, 7 * 86400000000, 1234567890123456,
           -946684800000000, -946684800000001, 9223371331200000000 - 946684800000000, 9223371331200000000 - 946684800000001,
           -211813488000000000 + 86400000000 * 4, 86400000000 * 365 * 24 + 12345]
    tss += [random.randint(-211813488000000000, 9223371331200000000 - 1) for _ in range(12)] + [random.randint(-10 ** 15, 10 ** 15) for _ in range(12)]
    dates = [0, 1, -1, 365, -365, 2451545, -2451545, 2147483646 - 2451545 if False else 5874897, -2451545 + 1, 3000, -719162, 10957, 365 * 100, 14712 - 24000, 7, -7]
    times = [0, 1, 999999, 1000000, 59999999, 3599999999, 3600000000, 45296789012, 86399999999, 86400000000, 43200000000, 500000]
    ivs = [(0, 0, 0), (0, 0, 1), (1, 0, 0), (-1, 0, 0), (13, 40, 3661000001), (-13, -40, -3661000001), (14, 0, 0), (-14, 0, 0), (25 * 12 + 7, 1, 0),
           (12 * 1000 + 5, 400, 0), (-12 * 1000 - 5, -400, -1), (0, 0, 9223372036854775807), (0, 0, -9223372036854775808),
           (2147483647, 2147483647, 9223372036854775807), (-2147483647, -2147483647, -9223372036854775808), (7, 0, 0), (-1, 0, 0), (-4, 0, 0), (-3, 0, 0), (-11, 0, 0),
           (0, 15, 90000000000), (0, 0, 86399999999), (0, 0, -86399999999), (5, 100, -5)]
    units = ["microseconds", "milliseconds", "second", "minute", "hour", "day", "week", "month", "quarter", "year", "decade", "century", "millennium", "julian",
             "dow", "doy", "isoyear", "isodow", "epoch", "timezone", "timezone_hour", "timezone_minute", "foo", "now", "today", "ms", "us", "d", "h", "min", "sec", "mon", "yr", "dec", "cent", "mil"]
    sets = "set datestyle = 'ISO, YMD';\nset intervalstyle = 'postgres';\nset timezone = 'UTC';\n"
    qs = []
    for u in units:
        for t in tss:
            lit = "(timestamp '2000-01-01' + interval '%d microseconds')" % t
            qs.append(("ts", u, t, "select extract('%s' from %s)::text, date_part('%s', %s)::text" % (u, lit, u, lit)))
        for d in dates:
            lit = "(date '2000-01-01' + %d)" % d
            qs.append(("date", u, d, "select extract('%s' from %s)::text, extract('%s' from %s)::text" % (u, lit, u, lit)))
        for t in times:
            lit = "(time '00:00' + interval '%d microseconds')" % t if t < 86400000000 else "time '24:00:00'"
            qs.append(("time", u, t, "select extract('%s' from %s)::text, date_part('%s', %s)::text" % (u, lit, u, lit)))
        for m, d, t in ivs:
            lit = "make_interval(0, %d, 0, %d, 0, 0, 0) + interval '%d microseconds'" % (m, d, t) if False else "interval '%d months %d days %d microseconds'" % (m, d, t)
            qs.append(("iv", u, "%d:%d:%d" % (m, d, t), "select extract('%s' from %s)::text, date_part('%s', %s)::text" % (u, lit, u, lit)))
    for kind, u, v, q in qs:
        r = subprocess.run(["psql", "-X", "-A", "-t", "-q", "-v", "VERBOSITY=sqlstate"] + args, input=sets + q + ";\n", capture_output=True, text=True)
        if r.returncode == 0 and "ERROR" not in r.stderr:
            parts = r.stdout.strip().split("|")
            res = " ".join(x.encode().hex() or "-" for x in parts) if len(parts) == 2 else "E ?" + r.stdout.strip()
        else:
            e = r.stderr
            res = "E INVALID" if "0A000" in e else "E UNKNOWN" if "22023" in e else "E RANGE" if "22008" in e or "22003" in e else "E ?" + e.strip().replace(" ", "_")[:40]
        print("part.%s %s %s => %s" % (kind, u.encode().hex(), v, res))


def dtin_inputs():
    """Texts for date, time and timestamp in: forms of the date, of the time,
    and of the zone, put together by chance, and some broken at random."""
    random.seed(20261125)
    fixed = ["", " ", "epoch", "infinity", "-infinity", "+infinity", "Infinity", "allballs", "zulu", "z", "current",
             "2000-01-31", "2000/01/31", "01/31/2000", "31/01/2000", "1/2/3", "01-02-03", "1.2.3", "2000.031", "2000-031", "J2451544", "j 2451544", "J2451544.5",
             "Jan 31 2000", "January 31, 2000", "31 Jan 2000", "2000 Jan 31", "Jan-31-2000", "31-Jan-2000", "2000-Jan-31", "Jan-31-00", "31-Jan-00",
             "20000131", "000131", "200001", "2000013", "20000131T040506", "20000131T0405", "20000131 040506", "2000-01-31T04:05:06", "2000-01-31t04:05:06",
             "2000-01-31 BC", "0001-01-01 BC", "0001-01-01", "0000-01-01", "99999-12-31", "5874897-12-31", "5874898-01-01", "4714-11-24 BC", "4714-11-23 BC",
             "5874897-12-31 04:05", "294276-12-31 23:59:59.999999", "294277-01-01", "Feb 29 2000", "Feb 29 1900", "2000-02-30", "2000-13-01", "13/13/13", "0/0/0",
             "1999-12-31 23:59:60", "1999-12-31 23:59:61", "24:00:00", "24:00:01", "24:00", "23:60", "12:00 AM", "12:00 PM", "13:00 PM", "0:00 AM", "11:59:59.999999 PM",
             "04:05:06.789", "04:05:06.7891234", "04:05:06.9999995", "04:05", "04", "040506", "0405", "04:05:06.", ":05", "04:", "T04:05:06", "t040506", "t 04:05",
             "04:05:06 PST", "04:05:06 est", "04:05:06 EDT", "04:05:06 utc", "04:05:06 +05", "04:05:06 -05:30", "04:05:06 +05:30:15", "04:05:06 +24", "04:05:06 +25",
             "04:05:06+05", "04:05:06-05", "04:05:06 america/new_york", "04:05:06 America/New_York", "04:05:06 nowhere/land", "04:05:06 est dst", "04:05:06 dst",
             "04:05:06 pst pst", "04:05:06 pst est", "2000-01-31 04:05:06 PST", "2000-01-31 04:05:06 America/New_York", "2000-01-31 04:05:06 +05:30",
             "2000-01-31 04:05:06 GMT-8", "2000-01-31 04:05:06 gmt+8", "2000-01-31 04:05:06-08", "2000-01-31 04:05:06.5-08:30", "2000-01-31 04:05:06 Z",
             "2000-01-31 04:05:06 zulu", "Mon Jan 31 04:05:06 2000", "Mon Jan 31 04:05:06 2000 PST", "Jan 31 040506 PST 2000", "Jan 31 04:05:06 2000 PST",
             "Monday 2000-01-31", "mon 2000-01-31 04:05", "2000-01-31 mon", "Sat Sun 2000-01-31", "2000-01-31 AD", "2000-01-31 ad bc", "2000-01-31 04:05 am", "2000-01-31 04:05 pm pm",
             "J2451544 04:05 pst", "2451544", "2000-01-31 2000-01-31", "2000-01-31 04:05 04:05", "04:05 2000-01-31", "pst 04:05", "pst", "est dst", "dst", "am", "pm", "bc",
             "2000-01-31 pst 04:05", "1.5", "1,5", "2000-01-31, 04:05", "(2000-01-31)", "2000-01-31;04:05", "2000-01-31\t04:05", "  2000-01-31  04:05  ", "\u00e92000-01-31",
             "2000-01-31 04:05:06 +05:30:60", "2000-01-31 04:05:06 +05:60", "2000-01-31 04:05:06 +", "2000-01-31 04:05:06 -", "2000-01-31 +04:05", "+2000-01-31", "-2000-01-31",
             "99999999999-01-01", "2000-99999999999-01", "2000-01-99999999999", "2000-01-31 99999999999:00", "04:05:99999999999", "04:99999999999:06", "04:05:06.99999999999",
             "9999999999999999999999", "2000-01-31 04:05:06 +99999999999", "-99-01-01", "00-00-00", "99-99-99", "69-01-01", "70-01-01", "100-01-01", "1-1-1", "0-1-1"]
    dates = ["2000-01-31", "2000/1/31", "1/31/2000", "31/1/2000", "12/25/99", "25/12/99", "99-12-25", "Jan 31 2000", "31 Jan 2000", "Jan 31, 2000", "2000 Jan 31",
             "February 29, 2000", "Feb 29 2001", "20000131", "000131", "2000.031", "J2451544", "1999-12-31", "1970-01-01", "0001-01-01", "10000-01-01",
             "2000-02-29", "2001-02-29", "2000-04-31", "1-2-3", "03/04/05", "2000-1-1", "2000-12-31", "1500-02-29", "4713-01-01 BC", "1.2.2000", "12.25.1999"]
    times = ["04:05:06", "04:05", "4:5:6", "04:05:06.789", "04:05:06.789012", "23:59:59.999999", "24:00:00", "12:00:00 AM", "12:00:00 PM", "11:00 pm", "01:00 am",
             "040506", "0405", "04:05:06.5", "00:00:00", "00:00", "13:00 pm", "25:00", "04:60", "04:05:61", "04:05:60", "T04:05:06", "t04:05"]
    zones = ["PST", "pst", "EST", "EDT", "PDT", "UTC", "GMT", "Z", "z", "zulu", "+05", "-05", "+05:30", "-08:00", "+0530", "-0800", "+05:30:15", "America/New_York",
             "Europe/London", "UTC+3", "GMT-8", "est dst", "dst", "pst dst", "nowhere/land", "+25", "+24:00", "xyzzy", "est edt"]
    out = list(fixed)
    for _ in range(240):
        parts = []
        if random.random() < 0.92:
            parts.append(random.choice(dates))
        if random.random() < 0.8:
            parts.append(random.choice(times))
        if random.random() < 0.4:
            parts.append(random.choice(zones))
        if random.random() < 0.1:
            parts.append(random.choice(["bc", "BC", "ad"]))
        if random.random() < 0.08:
            parts.append(random.choice(["mon", "Tuesday", "fri"]))
        if random.random() < 0.12:
            random.shuffle(parts)
        out.append(random.choice([" ", " ", "  ", "T"]).join(parts) if random.random() < 0.97 else ",".join(parts))
    for _ in range(80):
        t = list(random.choice(out[len(fixed):] + dates))
        for _ in range(random.randint(1, 2)):
            i = random.randrange(len(t) + 1)
            if t and random.random() < 0.5 and i < len(t):
                del t[i]
            else:
                t.insert(i, random.choice("0123456789:-/. +abtTZ"))
        out.append("".join(t))
    return out


def main_dtin(args):
    """The text in of date, time and timestamp, by date order: what the old
    code read, as the canonical ISO text, or the code of its error."""
    from concurrent.futures import ThreadPoolExecutor
    inputs = dtin_inputs()
    kinds = ["date", "time", "timestamp"]
    orders = [("YMD", 0), ("DMY", 1), ("MDY", 2)]

    def one(job):
        kind, (order, _), t = job
        lit = t.replace("\\", "\\\\").replace("'", "''").replace("\t", "\\t").replace("\n", "\\n")
        script = "set timezone = 'UTC';\nset datestyle = 'ISO, %s';\nselect (E'%s'::%s)::text;\n" % (order, lit, kind)
        r = subprocess.run(["psql", "-X", "-A", "-t", "-q"] + args, input=script, capture_output=True, text=True)
        e = r.stderr
        if "ERROR" not in e:
            return r.stdout.strip().encode().hex() or "-"
        if "invalid input syntax" in e:
            return "E -1"
        if "date/time field value out of range" in e:
            return "E -3" if "different" in e else "E -2"
        if "time zone displacement out of range" in e:
            return "E -5"
        if "not recognized" in e:
            return "E -7" if "configuration file" in e else "E -6"
        if "out of range" in e:
            return "E R"
        return "E ?" + e.strip().replace(" ", "_")[:40]

    jobs = [(k, o, t) for k in kinds for o in orders for t in inputs]
    with ThreadPoolExecutor(max_workers=4) as ex:
        res = list(ex.map(one, jobs))
    for (kind, order, t), r in zip(jobs, res):
        print("dt.in %s %d %s => %s" % (kind, order[1], t.encode().hex() or "-", r))


def main_dtzin(args):
    """The text in of timestamptz and timetz, by date order and session zone:
    the instant as a UTC timestamp, or the time with its zone, as the old code
    read it. Zones with a daylight time are left to the server's tests."""
    from concurrent.futures import ThreadPoolExecutor
    inputs = [t for t in dtin_inputs() if "america" not in t.lower() and "europe" not in t.lower()]
    kinds = ["timestamptz", "timetz"]
    orders = [("YMD", 0), ("DMY", 1), ("MDY", 2)]
    zones = [("UTC", 0), ("Etc/GMT-5", 1)]

    def one(job):
        kind, (order, _), (zone, _z), t = job
        lit = t.replace("\\", "\\\\").replace("'", "''").replace("\t", "\\t").replace("\n", "\\n")
        expr = "((E'%s')::timestamptz at time zone 'UTC')::text" % lit if kind == "timestamptz" else "(E'%s')::timetz::text" % lit
        script = "set timezone = '%s';\nset datestyle = 'ISO, %s';\nselect %s;\n" % (zone, order, expr)
        r = subprocess.run(["psql", "-X", "-A", "-t", "-q"] + args, input=script, capture_output=True, text=True)
        e = r.stderr
        if "ERROR" not in e:
            return r.stdout.strip().encode().hex() or "-"
        if "invalid input syntax" in e:
            return "E -1"
        if "date/time field value out of range" in e:
            return "E -3" if "different" in e else "E -2"
        if "time zone displacement out of range" in e:
            return "E -5"
        if "not recognized" in e:
            return "E -7" if "configuration file" in e else "E -6"
        if "out of range" in e:
            return "E R"
        return "E ?" + e.strip().replace(" ", "_")[:40]

    jobs = [(k, o, z, t) for k in kinds for o in orders for z in zones for t in inputs]
    with ThreadPoolExecutor(max_workers=4) as ex:
        res = list(ex.map(one, jobs))
    for (kind, order, zone, t), r in zip(jobs, res):
        print("dtz.in %s %d %d %s => %s" % (kind, order[1], zone[1], t.encode().hex() or "-", r))


def main():
    if len(sys.argv) > 1 and sys.argv[1] == "--part":
        sys.argv.pop(1)
        main_part(sys.argv[1:])
        return
    if len(sys.argv) > 1 and sys.argv[1] == "--trunc":
        sys.argv.pop(1)
        main_trunc(sys.argv[1:])
        return
    if len(sys.argv) > 1 and sys.argv[1] == "--interval":
        sys.argv.pop(1)
        main_interval(sys.argv[1:])
        return
    if len(sys.argv) > 1 and sys.argv[1] == "--dtzin":
        sys.argv.pop(1)
        main_dtzin(sys.argv[1:])
        return
    if len(sys.argv) > 1 and sys.argv[1] == "--dtin":
        sys.argv.pop(1)
        main_dtin(sys.argv[1:])
        return
    if len(sys.argv) > 1 and sys.argv[1] == "--datetime":
        sys.argv.pop(1)
        main_datetime(sys.argv[1:])
        return

    args = sys.argv[1:]
    for name, (sqltype, gen) in OPS.items():
        for text in gen():
            if sqltype == "like":
                lit = ""
            else:
                lit = text.replace("'", "''")
            sql = "select E'%s'::%s" % (lit.replace("\\", "\\\\").replace("\n", "\\n").replace("\t", "\\t"), sqltype)
            if sqltype == "inet":
                sql = "select a, host(a), text(a), abbrev(a) from (select E'%s'::inet a) t" % lit.replace("\\", "\\\\").replace("\n", "\\n").replace("\t", "\\t")
            elif sqltype == "cidr":
                sql = "select a, text(a), abbrev(a) from (select E'%s'::cidr a) t" % lit.replace("\\", "\\\\").replace("\n", "\\n").replace("\t", "\\t")
            if sqltype == "like":
                t, pt = text
                esc = lambda x: x.replace("\\", "\\\\").replace("'", "''")
                sql = "select E'%s' like E'%s'" % (esc(t), esc(pt))
                res = run(args, sql)
                res = {"t": "1", "f": "0"}.get(res, res)
                print("%s %s %s => %s" % (name, t.encode().hex() or "-", pt.encode().hex() or "-", res))
                continue
            res = run(args, sql)
            if res == "":
                res = "-"
            tok = text.encode().hex() or "-"
            print("%s %s => %s" % (name, tok, res))


main()

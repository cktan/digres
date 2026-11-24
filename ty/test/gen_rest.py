#!/usr/bin/env python3
# Copyright (c) 2026 CK Tan
# SPDX-License-Identifier: MIT
#
# gen_rest.py -- write test/rest_known.txt: what digres's date_bin of
# timestamptz, in_range of the floats, to_timestamp of a float8, the compare
# of text's pattern operators, and quote_literal give, taken from a digres
# built before they moved into ty:
#
#   test/gen_rest.py PSQL_ARGS... > test/rest_known.txt
#
# A line is one of:
#
#   bin MONTHS DAYS USECS TS ORIGIN => TS    date_bin; a timestamp is the
#                                            microseconds from 2000, inf or
#                                            -inf; err if refused
#   range8 VAL BASE OFFSET SUB LESS => B     in_range of float8: t, f or err
#   range4 VAL BASE OFFSET SUB LESS => B     in_range of float4
#   epoch F8 => TS                           to_timestamp; err if refused
#   pattern AHEX BHEX => SIGN                -1, 0 or 1; hex, - if empty
#   quote INHEX => OUTHEX                    quote_literal
#   timetz UNIT TIME ZONE => NUM F8          extract and date_part of a timetz,
#                                            its microseconds and its zone's
#                                            seconds west; err if refused

import random
import subprocess
import sys

random.seed(17)

PG_EPOCH = 946684800000000


def ts(us):
    if us in ("inf", "-inf"):
        return "timestamptz '%sinfinity'" % us[:-3]
    return ("(timestamptz '2000-01-01 00:00:00+00' + interval '%d days' + "
            "interval '%d microseconds')" % (us // 86400000000, us % 86400000000))


def usecs(e):
    return ("CASE WHEN isfinite(v) THEN ((extract(epoch FROM v) * 1000000)::numeric(36,0)"
            " - %d)::int8 || '' WHEN v > timestamptz 'epoch' THEN 'inf' ELSE '-inf' END "
            "FROM (SELECT %s AS v) x" % (PG_EPOCH, e))


def hexs(b):
    return b.hex() or "-"


lines = []  # the left side of a line, and the SELECT of its right side
BINS = [((0, 0, 900000000), "inf", 0), ((0, 0, 900000000), "-inf", 0),
        ((0, 0, 900000000), 0, "inf"), ((0, 0, 900000000), 0, "-inf"),
        ((1, 0, 0), 5, 0), ((0, 0, 0), 5, 0), ((0, -1, 0), 5, 0),
        ((0, 1, -86400000000), 5, 0), ((0, 2147483647, 0), 5, 0),
        ((0, 1, 0), 8223372036854775807, -211813488000000000),
        ((0, 1000000, 0), -211813488000000000, 0),
        ((0, 1, 0), -211813488000000000, 0)]
for _ in range(200):
    BINS.append(((0, random.choice([0, 0, 1, 7, 30]),
                  random.choice([0, 1, 1000000, 900000000, 3600000000, 12345678])),
                 random.randint(-10**16, 10**16), random.randint(-10**16, 10**16)))
for (m, d, u), t, o in BINS:
    e = ("date_bin(interval '%d months %d days' + interval '%d microseconds', "
         "%s, %s)" % (m, d, u, ts(t), ts(o)))
    lines.append(("bin %d %d %d %s %s" % (m, d, u, t, o), "SELECT " + usecs(e)))

F = ["0", "-0", "1", "-1", "1.5", "nan", "inf", "-inf", "1e308", "-1e308", "3e38",
     "1e-300", "100.25"]
OFF = ["0", "1", "0.5", "-1", "nan", "inf", "1e308"]
for k, t, vals in (("range8", "float8", F),
                   ("range4", "float4", ["0", "-0", "1", "-1", "1.5", "nan", "inf", "-inf",
                                         "3e38", "-3e38", "0.1"])):
    for v in vals:
        for b in vals:
            for o in OFF:
                for s in "tf":
                    for l in "tf":
                        if random.random() < .3:
                            lines.append(("%s %s %s %s %s %s" % (k, v, b, o, s, l),
                                          "SELECT CASE WHEN in_range('%s'::%s, '%s'::%s, "
                                          "'%s'::float8, '%s', '%s') THEN 't' ELSE 'f' END"
                                          % (v, t, b, t, o, s, l)))

E8 = ["0", "-0", "nan", "inf", "-inf", "1", "0.5", "0.0000005", "0.0000015",
      "1700000000.123456", "-210866803200", "-210866803201", "-210866803200.5",
      "9224318015999", "9224318016000", "9224318015999.9999999", "1e300", "-1e300",
      "253402300799.999999"] + [repr(random.uniform(-2e11, 9e12)) for _ in range(120)]
for f in E8:
    lines.append(("epoch " + f, "SELECT " + usecs("to_timestamp('%s'::float8)" % f)))

W = ["", "a", "A", "ab", "abc", "b", "\u00e9", "e", "\u4e2d", "ab ", "a\u0301", "\U0001f600"]
for a in W:
    for b in W:
        lines.append(("pattern %s %s" % (hexs(a.encode()), hexs(b.encode())),
                      "SELECT sign(bttext_pattern_cmp(convert_from('\\x%s', 'UTF8'), "
                      "convert_from('\\x%s', 'UTF8')))" % (a.encode().hex(), b.encode().hex())))

Q = ["", "a", "'", "''", "\\", "a'b", "a\\b", "'\\", "\u00e9'", "x" * 200, "'" * 50] + \
    ["".join(random.choice("a'\\b\u00e9") for _ in range(random.randint(0, 12)))
     for _ in range(60)]
for q in Q:
    lines.append(("quote " + hexs(q.encode()),
                  "SELECT encode(convert_to(quote_literal(convert_from('\\x%s', 'UTF8')), "
                  "'UTF8'), 'hex')" % q.encode().hex()))

UNITS = ["timezone", "timezone_m", "timezone_h", "timezone_hour", "microsecond", "ms",
         "millisecond", "second", "minute", "hour", "day", "month", "year", "epoch",
         "julian", "dow", "now", "fred", "us", "tz"]
TIMETZ = [(0, 0), (86400000000, 0), (45296789012, 18000), (86399999999, -20700),
          (1, 55799), (3661000001, -55799), (43200000000, 30)] + \
         [(random.randint(0, 86400000000), random.randint(-55799, 55799)) for _ in range(20)]
for u in UNITS:
    for t, z in TIMETZ:
        a = abs(z)
        x = ("'%02d:%02d:%02d.%06d%s%02d:%02d:%02d'::timetz" %
             (t // 3600000000, t // 60000000 % 60, t // 1000000 % 60, t % 1000000,
              "-" if z > 0 else "+", a // 3600, a // 60 % 60, a % 60))
        lines.append(("timetz %s %d %d" % (u, t, z),
                      "SELECT extract_timetz('%s', %s) || ' ' || date_part('%s', %s)"
                      .replace("extract_timetz", "pg_catalog.extract") % (u, x, u, x)))

sql = ["SET timezone = 'UTC';"]
for left, right in lines:
    sql.append("\\echo %s =>" % left)
    sql.append(right + ";")
res = subprocess.run(["psql", "-X", "-A", "-t", "-q", "-v", "ON_ERROR_STOP=0",
                      "-v", "VERBOSITY=terse"] + sys.argv[1:], input="\n".join(sql),
                     stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
out = []
for line in res.stdout.splitlines():
    if line.endswith(" =>"):
        out.append(line)
    else:
        out[-1] += " " + ("err" if "ERROR:" in line else line)
        if "ERROR:" in line:
            sys.stderr.write(line + "\n")
sys.stdout.write("\n".join(out) + "\n")

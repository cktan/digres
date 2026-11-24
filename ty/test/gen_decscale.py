#!/usr/bin/env python3
# Copyright (c) 2026 CK Tan
# SPDX-License-Identifier: MIT
#
# gen_decscale.py -- write test/decscale_known.txt: what digres's numeric
# gives for the results that are not exact, at PostgreSQL's scales, and for
# the casts to the integers, taken from a digres built before the scales
# moved into ty:
#
#   test/gen_decscale.py PSQL_ARGS... > test/decscale_known.txt
#
# A line is OP A B => TEXT, or => err STATE. OP is div, sqrt, exp, ln, log,
# pow, int2, int4, int8, typmod (B is P,S) or var (of the two values, the
# sample variance); B is - for an OP of one argument. A and B are read as
# decimal128.

import random
import subprocess
import sys

random.seed(19)

VALS = ["0", "0.00", "1", "-1", "1.000", "2", "10", "100", "0.1", "0.001",
        "9999", "10000", "10001", "99999999", "100000000", "0.9999", "0.0001",
        "1.1", "0.9", "1.05", "0.95", "1.0000000001", "0.99999999999",
        "123456789012345678901234567890123456", "0.000000000000000000000000000001",
        "999999999999999999999999999999999999", "3.14159265358979323846",
        "32767.4", "32767.5", "-32768.5", "-32768.4", "2147483647.4",
        "2147483647.5", "-2147483648.5", "9223372036854775807.4",
        "9223372036854775807.5", "-9223372036854775808.4",
        "-9223372036854775808.5", "100.5", "-2.5", "0.5", "1.5"]


def num():
    n = random.choice([1, 2, 3, 4, 5, 8, 9, 12, 17, 25, 33])
    d = "".join(random.choice("0123456789") for _ in range(n)).lstrip("0") or "1"
    s = random.randint(0, min(36, n + random.choice([0, 0, 2, 6])))
    if s:
        d = d.rjust(s + 1, "0")
        d = d[:-s] + "." + d[-s:]
    return d


def v(x):
    return "numeric_in('%s', 0, -1)" % x


lines = []


def add(op, a, b, e):
    lines.append(("%s %s %s =>" % (op, a, b), e))


pool = VALS + [num() for _ in range(150)]
for a in pool:
    add("sqrt", a, "-", "sqrt(%s)" % v(a))
    add("exp", a, "-", "exp(%s)" % v(a))
    add("ln", a, "-", "ln(%s)" % v(a))
    for t in ("int2", "int4", "int8"):
        add(t, a, "-", "%s::%s" % (v(a), t))
    for p, s in ((3, 1), (20, 2), (36, 10), (5, 5)):
        add("typmod", a, "%d,%d" % (p, s), "pg_catalog.numeric(%s, %d)" % (v(a), ((p << 16) | s) + 4))
for _ in range(700):
    a, b = random.choice(pool), random.choice(pool)
    if random.random() < .3:
        a = "-" + a if a[0] != "-" else a
    add("div", a, b, "%s / %s" % (v(a), v(b)))
    add("log", a.lstrip("-"), b, "log(%s, %s)" % (v(a.lstrip("-")), v(b)))
    add("var", a, b, "var_samp(x) FROM (VALUES (%s), (%s)) t(x)" % (v(a), v(b)))
EXPS = ["0", "0.00", "1", "2", "3", "-1", "-2", "10", "35", "-35", "0.5", "-0.5",
        "1.5", "2.000", "100", "-100", "999999999", "1000000000", "-999999999",
        "10000000000.0", "-10000000001", "0.0001", "33.3"]
for a in pool:
    for e in random.sample(EXPS, 6):
        add("pow", a, e, "power(%s, %s)" % (v(a), v(e)))
for e in EXPS:
    for a in ("0", "0.0", "0.000000000000000000000000000000000000", "-1", "-2.5", "1", "1.0000001"):
        add("pow", a, e, "power(%s, %s)" % (v(a), v(e)))

sql = ["\\set VERBOSITY sqlstate"]
for left, e in lines:
    sql.append("\\echo " + left)
    sql.append("SELECT " + e + ";")
res = subprocess.run(["psql", "-X", "-A", "-t", "-q", "-v", "ON_ERROR_STOP=0"] +
                     sys.argv[1:], input="\n".join(sql), stdout=subprocess.PIPE,
                     stderr=subprocess.STDOUT, text=True)
out = []
for line in res.stdout.splitlines():
    if line.endswith(" =>"):
        out.append(line)
    elif "ERROR:" in line:
        out[-1] += " err " + line.split("ERROR:")[1].strip()
    else:
        out[-1] += " " + line
sys.stdout.write("\n".join(out) + "\n")

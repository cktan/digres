#!/usr/bin/env python3
# Copyright (c) 2026 CK Tan
# SPDX-License-Identifier: MIT
#
# gen_float_text.py -- write test/float_text.txt: what PostgreSQL's float4out
# and float8out print, for ty's float text tests. The values are PostgreSQL's
# own, so they are taken from a digres built before float text moved into ty
# (any server with PostgreSQL's float code gives them):
#
#   test/gen_float_text.py PSQL_ARGS... > test/float_text.txt
#
# PSQL_ARGS is how to connect, such as: -h /tmp/sock -p 5432 -d postgres.
# A line is "f64.fmt VALUE DIGITS => TEXT", or f32.fmt: VALUE is repr() of the
# number, DIGITS the setting of extra_float_digits.

import random
import struct
import subprocess
import sys

random.seed(20261121)


def f32(x):
    return struct.unpack("f", struct.pack("f", x))[0]


def tok(x):
    if x != x:
        return "NaN"
    if x in (float("inf"), float("-inf")):
        return "Infinity" if x > 0 else "-Infinity"
    return repr(x)


vals64 = [0.0, -0.0, 1.0, -1.0, 0.1, 0.3, 1e22, 1e23, 5e-324, 1.7976931348623157e308,
          2.2250738585072014e-308, 9007199254740993.0, 123456789012345678.0, 1.5,
          100.0, 1e15, 1e16, 1e-5, 1e-4, 0.5, 2.5, 1234567.0, float("inf"),
          float("-inf"), float("nan"), 3.141592653589793, 2.718281828459045]
for _ in range(120):
    kind = random.randrange(4)
    if kind == 0:
        vals64.append(struct.unpack("<d", struct.pack("<Q", random.getrandbits(64)))[0])
    elif kind == 1:
        vals64.append(float("%d.%de%d" % (random.randrange(100000), random.randrange(1000000), random.randrange(40) - 20)))
    elif kind == 2:
        vals64.append(float(random.randrange(-10 ** 12, 10 ** 12)))
    else:
        vals64.append(random.randrange(2000000) / (1 + random.randrange(1000)))
vals64 = [v for v in vals64 if v == v or True]
vals32 = [0.0, -0.0, 1.0, -1.0, 0.1, 0.3, 1e10, 122908496.0, 1.4e-45, 3.4028235e38,
          1.17549435e-38, 16777217.0, 1.5, 100.0, 1e7, 1e8, 1e-5, float("inf"), float("nan")]
for _ in range(80):
    kind = random.randrange(3)
    if kind == 0:
        b = random.getrandbits(32)
        v = struct.unpack("f", struct.pack("I", b))[0]
        vals32.append(v)
    elif kind == 1:
        vals32.append(f32(float("%d.%de%d" % (random.randrange(100000), random.randrange(1000000), random.randrange(20) - 10))))
    else:
        vals32.append(f32(random.randrange(2000000) / (1 + random.randrange(1000))))
vals32 = [f32(v) if v == v and abs(v) != float("inf") else v for v in vals32]

sql = []
for k in (3, 1, 0, -1, -4):
    sql.append("SET extra_float_digits = %d;" % k)
    for v in vals64:
        sql.append("SELECT 'f64.fmt %s %d => ' || ('%s'::float8)::text;" % (tok(v), k, tok(v)))
    for v in vals32:
        sql.append("SELECT 'f32.fmt %s %d => ' || ('%s'::float4)::text;" % (tok(v), k, tok(v)))
out = subprocess.run(["psql", "-X", "-A", "-t", "-q"] + sys.argv[1:], input="\n".join(sql),
                     capture_output=True, text=True, check=True).stdout
sys.stdout.write(out)

#!/usr/bin/env python3
# Copyright (c) 2026 CK Tan
# SPDX-License-Identifier: MIT
#
# gen_decin.py -- write test/decin_known.txt: what digres's numeric text in
# and out give, with the scale functions and the float casts. The answers
# are taken from a digres built before decimal's text moved into ty:
#
#   test/gen_decin.py PSQL_ARGS... > test/decin_known.txt
#
# A line is one of:
#
#   decin INHEX P S => ok OUTHEX       the text INHEX read as numeric(P, S)
#   decin INHEX P S => err STATE KIND  refused: STATE is the SQLSTATE, KIND
#                                      nan, digits, field, d64 (a bare
#                                      numeric, and P <= 17, is a decimal64)
#                                      or - by the detail
#   scale TEXT => MIN TRIM             min_scale and trim_scale of TEXT
#   tof8 TEXT => F8 F4                 TEXT::numeric as float8 and float4
#   fromf8 F8 => TEXT                  the float8 F8 as numeric, or err if
#                                      it has none
#   fromf4 F4 => TEXT                  the float4 F4 as numeric, or err
#
# P and S are -1 for no typmod. Hex keeps spaces and quotes in one field.

import random
import subprocess
import sys

random.seed(7)

INPUTS = [
    "0", "-0", "+0", "00", "0.0", "-0.000", "1", "-1", "+1", "  1.5  ",
    "\t2\n", ".5", "5.", ".", "-.", "+.5", "-.5", "1_000", "1__0", "_1",
    "1_", "1._5", "1_.5", "1.5_5", "1e3", "1E-3", "1e+3", "1e", "1e+",
    "1.5e2", "1.5e-2", "1e1_0", "1e1000", "1e-1000", "1e999", "1e-999",
    "1e36", "1e35", "1e-36", "1e-37", "0e1000", "0e-1000", "12.34e1",
    "1 2", "1.2.3", "--1", "+-1", "abc", "", " ", "1a", "a1",
    "00001.5000", "0.5000000000000000000000000000000000000000",
    "99.95", "-99.95", "9.995", "9.9949", "0.05", "0.5", "1.5", "2.5",
    "-2.5", "0.0000000000000000000000000000000000001",
    "0.000000000000000000000000000000000001",
    "123456789012345678901234567890123456",
    "1234567890123456789012345678901234567",
    "123456789012345678901234567890123456.0000",
    "12345678901234567890123456789012345.6",
    "12345678901234567890123456789012345.65",
    "99999999999999999999999999999999999.95",
    "999999999999999999999999999999999999",
    "999999999999999999999999999999999999.5",
    "0." + "1" * 36, "0." + "1" * 37, "0." + "9" * 37, "1." + "0" * 40,
    "1" * 154, "1" * 155, "0." + "0" * 160 + "1", "5" + "0" * 153 + ".5",
    "0x1F", "0X1f", "-0x1F", "0x_1", "0x1_F", "0x", "0xG", "0o17", "0O17",
    "0o8", "0b101", "0B101", "0b2", "0b", "0x" + "F" * 30, "0x" + "F" * 31,
    "0x1.5", "0x1e5", "nan", "NaN", "-nan", "inf", "-inf", "Infinity",
    "-Infinity", "infinity1", "naN ", "1e3x", "  -12.50  ", "1.", "-1.",
]
for _ in range(400):
    ip = "".join(random.choice("0123456789") for _ in range(random.randint(0, 20)))
    fp = "".join(random.choice("0123456789") for _ in range(random.randint(0, 20)))
    s = ip + ("." + fp if fp or random.random() < 0.3 else "")
    if not s or s == ".":
        s = "7"
    if random.random() < 0.3:
        s = "-" + s
    if random.random() < 0.2:
        s += "e" + str(random.randint(-40, 40))
    INPUTS.append(s)

TYPMODS = [(-1, -1), (1, 0), (2, 2), (3, 1), (5, 2), (10, 5), (18, 9),
           (36, 0), (36, 36), (36, 10), (6, 6)]

FLOATS = ["0", "-0", "1", "-1", "0.1", "0.5", "1.5", "123.456",
          "1e-5", "1e-20", "1e20", "1e35", "1e36", "1e37", "1e-40",
          "3.141592653589793", "2.718281828459045", "1234567.891",
          "0.30000000000000004", "999999999999999.9", "1.7976931348623157e308",
          "5e-324", "nan", "inf", "-inf"] + \
         [repr(random.uniform(-1e6, 1e6)) for _ in range(60)] + \
         [repr(random.uniform(-1, 1) * 10 ** random.randint(-30, 30)) for _ in range(60)]


def hexs(s):
    return s.encode().hex() or "-"


def lit(s):
    return "'" + s.replace("'", "''") + "'"


# The inputs are a table's rows, so no cast is folded as a constant, which
# would raise its error in a branch that does not run. numeric_in is called
# with the typmod: a cast to numeric(P, S) reads with none, then rounds.
sql = ["CREATE TEMP TABLE inp (i int, x text);",
       "CREATE TEMP TABLE flt (i int, f text);"]
for i, x in enumerate(INPUTS):
    sql.append("INSERT INTO inp VALUES (%d, %s);" % (i, lit(x)))
for i, f in enumerate(FLOATS):
    sql.append("INSERT INTO flt VALUES (%d, %s);" % (i, lit(f)))
for p, s in TYPMODS:
    t = "numeric" if p < 0 else "numeric(%d,%d)" % (p, s)
    sql.append(
        "SELECT 'decin ' || coalesce(nullif(encode(convert_to(x, 'UTF8'), 'hex'), ''), '-') "
        "|| ' %d %d => ' || CASE WHEN pg_input_is_valid(x, '%s') "
        "THEN 'ok ' || encode(convert_to(numeric_in(x::cstring, 0, %d)::text, 'UTF8'), 'hex') "
        "ELSE (SELECT 'err ' || e.sql_error_code || ' ' || CASE "
        "WHEN e.detail LIKE '%%NaN%%' THEN 'nan' "
        "WHEN e.detail LIKE 'A numeric of precision%%' THEN 'd64' "
        "WHEN e.detail LIKE '%%at most%%' THEN 'digits' "
        "WHEN e.detail LIKE 'A field%%' THEN 'field' ELSE '-' END "
        "FROM pg_input_error_info(x, '%s') e) END FROM inp ORDER BY i;"
        % (p, s, t, -1 if p < 0 else ((p << 16) | (s & 0x7ff)) + 4, t))
sql.append("SELECT 'scale ' || n::text || ' => ' || min_scale(n) || ' ' || "
           "trim_scale(n)::text FROM (SELECT i, x::numeric n FROM inp "
           "WHERE pg_input_is_valid(x, 'numeric') OFFSET 0) v ORDER BY i;")
sql.append("SELECT 'tof8 ' || n::text || ' => ' || n::float8::text || ' ' || "
           "CASE WHEN abs(n) < 3.4e38 THEN n::float4::text ELSE '-' END "
           "FROM (SELECT i, x::numeric n FROM inp "
           "WHERE pg_input_is_valid(x, 'numeric') OFFSET 0) v ORDER BY i;")
# A float's own statement: a cast that raises takes no other row with it,
# and a float with no line after the run is written as err.
# A float that is not a float4 has no fromf4 line.
for f in FLOATS:
    sql.append("SELECT 'f4bad %s' WHERE NOT pg_input_is_valid(%s, 'float4');"
               % (f, lit(f)))
    for k, t in (("fromf8", "float8"), ("fromf4", "float4")):
        sql.append("SELECT '%s %s => ' || %s::%s::numeric::text;" % (k, f, lit(f), t))

res = subprocess.run(["psql", "-X", "-A", "-t", "-q", "-v", "ON_ERROR_STOP=0"] +
                     sys.argv[1:], input="\n".join(sql), capture_output=True,
                     text=True)
out = res.stdout.splitlines()
bad4 = {l.split()[1] for l in out if l.startswith("f4bad ")}
out = [l for l in out if not l.startswith("f4bad ")]
have = {" ".join(l.split()[:2]) for l in out}
for f in FLOATS:
    for k in ("fromf8", "fromf4"):
        if "fromf4" == k and f in bad4:
            continue
        if "%s %s" % (k, f) not in have:
            out.append("%s %s => err" % (k, f))
sys.stdout.write("\n".join(out) + "\n")
if res.stderr:
    sys.stderr.write(res.stderr)

#!/usr/bin/env python3
# Copyright (c) 2026 CK Tan
# SPDX-License-Identifier: MIT
#
# gen_tonum.py -- write test/tonum_known.txt: what PostgreSQL's to_char
# prints for int4, int8, float4, float8 and numeric, by many number formats.
# The text is PostgreSQL's own, so it is taken from a digres built before
# to_char moved into ty:
#
#   test/gen_tonum.py PSQL_ARGS... > test/tonum_known.txt
#
# A line is "tonum.TYPE VALUE FMTHEX => OUTHEX": TYPE is i4, i8, f4, f8 or
# num, VALUE the value's text (repr() of a float), FMTHEX the format and
# OUTHEX the result as hex digits (- if empty). ty's test runs all but num,
# whose digits digres makes; digres's are compared whole.

import subprocess
import sys

FORMATS = [
    "9999", "0000", "9,999", "FM9999", "99.99", "FM99.99", "9.9", "FM9.9",
    "00.00", "S9999", "9999S", "S99.99", "99.99S", "MI9999", "9999MI",
    "PL9999", "SG9999", "9999PR", "FM9999PR", "L9999", "9999D99", "G999G999",
    "FMG999", "9999TH", "9999th", "FM9999th", "FM9999TH", "RN", "rn", "FMRN",
    "FMrn", "9.99EEEE", "9.99eeee", "99.9999EEEE", "9EEEE", "9V999", "99V9",
    "9V9", "B9999", "C9", "\"text\" 999", "FM0.0000", "999.000", "9990.99",
    "0099", "S0099.9", "FMS0.9", "SG99.99", "PL99.99", "99.99PL", "99.99MI",
    "9.999999999999999999", "999,999,999,999,999,999.99", "FM999,999.999",
    "FM9,999.990", "D99", "G", "9 9 9", "S 9", "9TH9", "99999999999999999999",
    "FM99999999999999999999", "0.9", "FM0.9", "9.0", ".99", "FM.99", "0",
    "9", "FM9", "SG9", "MI9", "PL9", "9PR", "S9", "9S", "L", "FML9.99",
    "999D999S", "S999D999", "FM9.99EEEE" if False else "9.9EEEE", "x9y",
    "\\\"9\\\"", "99,99.99,99", "FM99,99.99,99", "9G9G9", "00G00", "TH",
    "9.99th", "-9", "+9", "#9", "9V99999999999",
]

VALUES = {
    "i4": ["0", "1", "-1", "5", "11", "12", "13", "21", "22", "23", "101",
           "111", "112", "1234", "-1234", "99999", "2147483647", "-2147483648",
           "3999", "4000", "42", "-42"],
    "i8": ["0", "1", "-1", "12", "1234", "-1234", "3999", "123456789012",
           "-123456789012", "9223372036854775807", "-9223372036854775808",
           "2147483648", "99999999999999999", "5"],
    "f8": ["0", "-0", "0.5", "-0.5", "1.5", "2.5", "1234.5678", "-1234.5678",
           "1e-10", "10000000000.0", "1e+20", "123456789.98765433", "nan",
           "inf", "-inf", "3999.4", "0.000123", "99.995", "1.005", "12.0",
           "-0.004", "999999999999999.9", "0.1"],
    "f4": ["0", "-0", "0.5", "1.5", "1234.5677", "-1234.5677", "1e-10",
           "10000000000.0", "nan", "inf", "-inf", "3999.4", "0.000123",
           "99.995", "12.0", "0.1", "123456.79"],
    "num": ["0", "1.5", "-1234.5678", "99999.999", "12345678901234567890.12345",
            "0.0001", "-0.0001", "3999", "1e10", "12", "-12", "0.5",
            "1234567890123456789012345678.9"],
}
SQLTYPE = {"i4": "int4", "i8": "int8", "f4": "float4", "f8": "float8",
           "num": "numeric"}


def hexs(s):
    return s.encode().hex() or "-"


def lit(s):
    return "'" + s.replace("'", "''") + "'"


sql = []
for t, vals in VALUES.items():
    for v in vals:
        sv = {"nan": "NaN", "inf": "Infinity", "-inf": "-Infinity"}.get(v, v)
        for f in FORMATS:
            sql.append("SELECT 'tonum.%s %s %s => ' || coalesce(nullif(encode(convert_to(to_char(%s::%s, %s), 'UTF8'), 'hex'), ''), '-');"
                       % (t, v, hexs(f), lit(sv), SQLTYPE[t], lit(f)))

res = subprocess.run(["psql", "-X", "-A", "-t", "-q", "-v", "ON_ERROR_STOP=0"] + sys.argv[1:],
                     input="\n".join(sql), capture_output=True, text=True)
sys.stdout.write(res.stdout)
if res.stderr:
    sys.stderr.write(res.stderr)

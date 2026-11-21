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


@op("macaddr.parse", "macaddr")
def mac_inputs():
    out = []
    for _ in range(60):
        h = hexbytes(12)
        p = [h[i:i + 2] for i in range(0, 12, 2)]
        forms = [":".join(p), "-".join(p), h[:6] + ":" + h[6:], h[:6] + "-" + h[6:],
                 h[0:4] + "." + h[4:8] + "." + h[8:12], h[0:4] + "-" + h[4:8] + "-" + h[8:12], h]
        out.append(random.choice(forms))
    out += ["08:00:2b:01:02:03", "08-00-2b-01-02-03", "08002b:010203", "08002b-010203",
            "0800.2b01.0203", "0800-2b01-0203", "08002b010203", " 08:00:2b:01:02:03",
            "08:00:2b:01:02:03 ", "08:00:2b:01:02:03x", "08:00:2b:01:02", "08:00:2b:01:02:03:04",
            "8:0:2b:1:2:3", "0x8:0x0:2b:1:2:3", "100:00:2b:01:02:03", "-1:0:0:0:0:0", "+8:0:0:0:0:0",
            "g8:00:2b:01:02:03", "", " ", "08:00:2b-01:02:03", "08002b01020", "08002b0102033",
            "0800.2b01.020", "08.00.2b.01.02.03", "08:00:2b:01:02:0", "ff:ff:ff:ff:ff:ff",
            "00:00:00:00:00:00", "08:00:2B:01:02:03\n", "08:00:2b:01:02:03\t"]
    return out


@op("macaddr8.parse", "macaddr8")
def mac8_inputs():
    out = []
    for _ in range(60):
        n = random.choice([6, 8, 8, 6, 7, 5, 9])
        h = hexbytes(2 * n)
        p = [h[i:i + 2] for i in range(0, 2 * n, 2)]
        sp = random.choice([":", "-", ".", "", ":", "-"])
        out.append(sp.join(p))
    out += ["08:00:2b:01:02:03:04:05", "08-00-2b-01-02-03-04-05", "0800.2b01.0203.0405", "0800:2b01:0203:0405",
            "08002b0102030405", "08:00:2b:01:02:03", "08002b010203", " 08:00:2b:01:02:03:04:05",
            "08:00:2b:01:02:03:04:05 ", "08:00:2b:01:02:03:04:05x", "08:00:2b:01:02:03:04:05:06",
            "08:00:2b-01:02:03:04:05", "0:0:2b:1:2:3", "g8:00:2b:01:02:03", "", " ", "08:00:2b:01:02",
            "08.00.2b.01.02.03", "0800-2b01-0203", "ff:ff:ff:ff:ff:ff:ff:ff", "00:00:00:00:00:00:00:00",
            "08:00:2b:01:02:03:04", "08:00:2b:01:02:03 ", "08:00:2b:01:02:03 4", "\t08002b0102030405"]
    return out


def run(psql_args, sql):
    r = subprocess.run(["psql", "-X", "-A", "-t", "-q", "-v", "VERBOSITY=sqlstate"] + psql_args + ["-c", sql],
                       capture_output=True, text=True)
    if r.returncode == 0:
        return r.stdout.strip()
    err = r.stderr
    if "22P02" in err:
        return "E INVALID"
    if "22003" in err:
        return "E RANGE"
    return "E " + err.strip().replace(" ", "_")[:40]


def main():
    args = sys.argv[1:]
    for name, (sqltype, gen) in OPS.items():
        for text in gen():
            lit = text.replace("'", "''")
            sql = "select E'%s'::%s" % (lit.replace("\\", "\\\\").replace("\n", "\\n").replace("\t", "\\t"), sqltype)
            res = run(args, sql)
            tok = text.encode().hex() or "-"
            print("%s %s => %s" % (name, tok, res))


main()

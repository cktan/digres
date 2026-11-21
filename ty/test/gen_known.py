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


def inet_inputs():
    """Text of inet and cidr values: good and bad, v4 and v6, the short
    forms the ISC parser reads, and random addresses with random masks."""
    out = ["1.2.3.4", "1.2.3.4/24", "10", "10.1", "10.1.2", "10/8", "10.1/16", "10.1.2/24", "255.255.255.255/32",
           "0.0.0.0/0", "0.0.0.0", "1.2.3.4/33", "1.2.3.4/", "1.2.3.4/-1", "256.1.1.1", "01.2.3.4", "0x0a010203",
           "0xc0a80101/24", "0xc0a8", " 1.2.3.4", "1.2.3.4 ", "1.2.3", "1.2.3.4.5", "1..2.3", "a.b.c.d", "1.2.3.4/8",
           "192.168.1.0/24", "192.168.1.5/24", "192.168.0.0/16", "128.0.0.0/1", "128.0.0.0/2", "10.0.0.0/7", "172.16.0.0/12",
           "1.2.3.4/0", "1.2.3.4/32", "1.2.3.4/31", "1.2.3.4/x", "1.2.3.4//8", "1.2.3.4/8/8", "224", "224/4", "192.168", "192.168/16",
           "::", "::1", "::1/128", "1::", "1:2:3:4:5:6:7:8", "1:2:3:4:5:6:7:8/64", "::ffff:1.2.3.4", "::ffff:1.2.3.4/96",
           "::1.2.3.4", "1:2:3:4:5:6:1.2.3.4", "1::2::3", "12345::", "g::", ":::", "1:2:3:4:5:6:7", "::/0", "ffff::/16",
           "2001:db8::/32", "2001:db8::1/129", "2001:db8::1", "2001:db8:0:0:0:0:0:1", "2001:0db8:0000:0000:0000:0000:0000:0001",
           "2001:db8::", "fe80::1%eth0", "FE80::ABCD", "1:0:0:2:0:0:0:3", "1:0:0:0:2:0:0:3", "0:0:0:0:0:0:0:1", "1:2:3:4:5:6:7:0",
           "0:2:3:4:5:6:7:8", "1:2:3:4:5:6:7::", "::2:3:4:5:6:7:8", "1::8", "1:2::7:8", "::ffff:0:0", "64:ff9b::1.2.3.4", "::ffff:255.255.255.255",
           "::1/0", "8000::/1", "ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffff", "ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffff/128", "ffff:ffff::/32", "::ffff:1.2.3", "::1.2.3.4.5", "1:2:3:4:5:6:7:8:9",
           "", " ", "/", "/8", "abc", "1.2.3.4/08", "1.2.3.4/008", "::1/", "::1/-1", "[::1]", "1.2.3.4\n"]
    for _ in range(120):
        a = ".".join(str(random.randint(0, 255)) for _ in range(4))
        out.append(a + random.choice(["", "", "/" + str(random.randint(0, 32))]))
    for _ in range(120):
        w = ["%x" % random.choice([0, 0, 0, 1, random.randint(0, 65535)]) for _ in range(8)]
        t = ":".join(w)
        if random.random() < 0.5:
            i = random.randint(0, 7)
            j = random.randint(i + 1, 8)
            t = ":".join(w[:i]) + "::" + ":".join(w[j:])
        out.append(t + random.choice(["", "", "/" + str(random.randint(0, 128))]))
    for _ in range(60):
        # cidr-style: an address with the host bits cleared, and with them not
        n = random.randint(0, 32)
        v = random.getrandbits(32)
        if random.random() < 0.7:
            v &= ~((1 << (32 - n)) - 1) & 0xFFFFFFFF
        out.append(".".join(str((v >> s) & 255) for s in (24, 16, 8, 0)) + "/" + str(n))
        n = random.randint(0, 128)
        v = random.getrandbits(128)
        if random.random() < 0.7:
            v &= ~((1 << (128 - n)) - 1) & ((1 << 128) - 1)
        out.append(":".join("%x" % ((v >> (16 * k)) & 0xFFFF) for k in range(7, -1, -1)) + "/" + str(n))
    return out


OPS["inet.parse"] = ("inet", inet_inputs)
def tid_inputs():
    out = ["(0,1)", "(1,2)", "(4294967295,65535)", "(4294967296,1)", "(-1,1)", "(1,65536)", "(1,2", "1,2)", "(1;2)",
           " (1,2)", "(1,2) ", "( 1,2)", "(1, 2)", "(a,b)", "", "(,)", "(1,)", "(,1)", "(+1,2)", "(4294967295,0)",
           "(0,0)", "(2147483648,1)", "(-2147483648,1)", "(-2147483649,1)", "(18446744073709551615,1)",
           "(18446744073709551616,1)", "(1,2)x", "(1,2))", "((1,2)", "(1,2,3)", "1,2", "(01,002)", "(1,-1)", "(1,+2)",
           "(0x10,1)", "(1e2,1)", "(1.5,1)", "(1,65535)", "(65535,65535)"]
    for _ in range(80):
        out.append("(%d,%d)" % (random.choice([random.getrandbits(32), random.getrandbits(16), random.getrandbits(8), 0]),
                                random.choice([random.getrandbits(16), random.getrandbits(8), 0, 65535])))
    return out


def lsn_inputs():
    out = ["0/0", "FFFFFFFF/FFFFFFFF", "1/2", "a/B", "12345678/9ABCDEF0", "123456789/1", "1/123456789", "/1", "1/", "1", "",
           "1/2/3", " 1/2", "1/2 ", "g/1", "1/g", "0/00000000", "00000000/0", "000000000/0", "ffffffff/0", "0/ffffffff",
           "1//2", "-1/2", "+1/2", "0x1/2", "1/0x2", "1\\n/2"]
    for _ in range(80):
        out.append("%x/%x" % (random.getrandbits(random.choice([8, 16, 32])), random.getrandbits(random.choice([8, 16, 32]))))
    return out


def char_inputs():
    out = ["a", "", "ab", "\\101", "\\377", "\\400", "\\777", "\\18", "\\", "\\1", "\\0000", "\\000", "\\200",
           "\\177", "\\176", "A", "z", "~", "\\x41", "\\a", "\\101x", "x\\101", "\u00e9", "\u20ac", "0", "9", "\\060",
           "\\134", "\\047", "\\042"]
    for _ in range(40):
        out.append("\\%03o" % random.randint(0, 511))
    return out


OPS["tid.parse"] = ("tid", tid_inputs)
OPS["lsn.parse"] = ("pg_lsn", lsn_inputs)
OPS["char.parse"] = ('"char"', char_inputs)

OPS["cidr.parse"] = ("cidr", inet_inputs)


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
    return "E " + err.strip().replace(" ", "_")[:40]


def main():
    args = sys.argv[1:]
    for name, (sqltype, gen) in OPS.items():
        for text in gen():
            lit = text.replace("'", "''")
            sql = "select E'%s'::%s" % (lit.replace("\\", "\\\\").replace("\n", "\\n").replace("\t", "\\t"), sqltype)
            if sqltype == "inet":
                sql = "select a, host(a), text(a), abbrev(a) from (select E'%s'::inet a) t" % lit.replace("\\", "\\\\").replace("\n", "\\n").replace("\t", "\\t")
            elif sqltype == "cidr":
                sql = "select a, text(a), abbrev(a) from (select E'%s'::cidr a) t" % lit.replace("\\", "\\\\").replace("\n", "\\n").replace("\t", "\\t")
            res = run(args, sql)
            if res == "":
                res = "-"
            tok = text.encode().hex() or "-"
            print("%s %s => %s" % (name, tok, res))


main()

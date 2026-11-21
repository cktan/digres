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


OPS["tid.parse"] = ("tid", tid_inputs)
OPS["lsn.parse"] = ("pg_lsn", lsn_inputs)
OPS["char.parse"] = ('"char"', char_inputs)
def money_inputs():
    out = ["$1.23", "1.23", "-$1.23", "($1.23)", "$1,234.56", "1,234", "1234567.891", "0.005", "0.004", "  $ 5 ", "$5 -",
           "5-", "+5", "$+5", "$-5", "92233720368547758.07", "-92233720368547758.08", "92233720368547758.08",
           "-92233720368547758.09", "abc", "$", "", "1.2.3", "1,2,3", "$1,2", "$ -1", "-1$", "1.", ".5", "$.5", "0", "-0",
           "$0.00", "1e5", "12 34", "1 000", "$1,000,000.00", "1,000,000", "($1)", "(1", "1)", "- 5", "$ - 5", "5.5", "5.45",
           "5.455", "5.445", "5.4", "99999999999999999.99", "922337203685477580.7", "$$5", "5$$"]
    for _ in range(60):
        n = random.randint(0, 10 ** random.randint(1, 17))
        t = "%d.%02d" % (n // 100, n % 100) if random.random() < 0.7 else str(n)
        out.append(random.choice(["", "$", "-", "-$", "($"]) + t + random.choice(["", "", ")", " "]))
    return out


OPS["like.pair"] = ("like", like_ops)
OPS["money.parse"] = ("money", money_inputs)

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


def main():
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

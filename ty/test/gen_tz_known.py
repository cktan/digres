#!/usr/bin/env python3
# Copyright (c) 2026 CK Tan
# SPDX-License-Identifier: MIT
#
# gen_tz_known.py -- write test/tz_known.txt: what the zones of ty should
# give, taken from a reader that is not ty's: Python's zoneinfo, on the very
# files that PostgreSQL's zic wrote (../TZDATA.md):
#
#   test/gen_tz_known.py --zones DIR > test/tz_known.txt
#
# A line of the first kind says what a UTC instant is in a zone:
#   L ZONE EPOCH => OFFSET_EAST ISDST ABBREV Y M D h m s
# A line of the second says what offset a local time gets, by PostgreSQL's
# rule for a time a transition skips (the offset before it) or repeats (the
# offset after it); the offset is west of Greenwich, PostgreSQL's sign:
#   O ZONE Y M D h m s => OFFSET_WEST ISDST
# The instants and the local times are fixed, or found around the
# transitions of the zone, so the same data gives the same file.

import argparse
import datetime as dt
import os
import sys
import zoneinfo

# A year is at most 9999 in Python.
INSTANTS = ["1800-01-01 00:00:00", "1900-06-15 12:00:00", "1945-06-01 00:00:00", "1970-01-01 00:00:00",
            "1985-07-01 06:30:00", "1995-01-15 18:45:10", "2000-01-01 00:00:00", "2005-04-10 08:00:00",
            "2010-07-01 12:00:00", "2015-11-01 05:30:00", "2019-03-31 01:00:00", "2020-10-25 01:00:00",
            "2022-12-31 23:59:59", "2024-03-10 07:00:00", "2024-11-03 06:30:00", "2037-12-31 12:00:00",
            "2040-06-01 00:00:00", "2100-07-01 00:00:00", "3000-01-01 00:00:00", "9000-07-01 12:00:00"]
EXTRA_LOCAL = ["2000-04-02 02:30:00", "2000-10-29 01:30:00", "2024-03-10 02:30:00", "2024-11-03 01:30:00",
               "2024-03-31 02:30:00", "2024-10-27 02:30:00", "1900-01-01 00:00:00", "2500-06-15 12:00:00"]
UTC = dt.timezone.utc


def epoch_of(s):
    return int(dt.datetime.strptime(s, "%Y-%m-%d %H:%M:%S").replace(tzinfo=UTC).timestamp())


def at(z, epoch):
    return dt.datetime.fromtimestamp(epoch, z)


def off(d):
    return int(d.utcoffset().total_seconds())


def isdst(d):
    return 1 if d.dst() else 0


def key(z, epoch):
    d = at(z, epoch)
    return (off(d), isdst(d), d.tzname())


def transitions(z):
    """The instants from 1900 to 2040 where the offset or the DST flag changes."""
    out = []
    lo = epoch_of("1900-01-01 00:00:00")
    end = epoch_of("2040-01-01 00:00:00")
    step = 30 * 86400
    t = lo
    prev = key(z, t)
    while t < end:
        n = min(t + step, end)
        k = key(z, n)
        if k != prev:
            a, b = t, n  # the change is in (a, b]
            while b - a > 1:
                m = (a + b) // 2
                if key(z, m) == prev:
                    a = m
                else:
                    b = m
            out.append(b)
            prev = key(z, b)
            t = b
            continue
        t, prev = n, k
    return out


def fields(d):
    return "%d %d %d %d %d %d" % (d.year, d.month, d.day, d.hour, d.minute, d.second)


def local_offset(z, naive):
    """PostgreSQL's offset (west) and DST flag for a local time."""
    d0 = naive.replace(tzinfo=z, fold=0)
    d1 = naive.replace(tzinfo=z, fold=1)
    o0, o1 = off(d0), off(d1)
    if o0 == o1:
        d = d0
    elif o0 > o1:
        d = d1  # a fold (the clock went back): the offset after
    else:
        d = d0  # a gap (the clock went forward): the offset before
    return -off(d), isdst(d)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--zones", required=True)
    args = ap.parse_args()
    zoneinfo.reset_tzpath([os.path.abspath(args.zones)])
    names = []
    seen = {}
    for dirpath, dirnames, filenames in os.walk(args.zones):
        dirnames.sort()
        for fn in sorted(filenames):
            p = os.path.join(dirpath, fn)
            n = os.path.relpath(p, args.zones).replace(os.sep, "/")
            with open(p, "rb") as f:
                data = f.read()
            if data[:4] != b"TZif":
                continue
            if data not in seen:  # one name for each distinct file
                seen[data] = n
                names.append(n)
    names.sort()
    for n in names:
        z = zoneinfo.ZoneInfo(n)
        for s in INSTANTS:
            e = epoch_of(s)
            d = at(z, e)
            print("L %s %d => %d %d %s %s" % (n, e, off(d), isdst(d), d.tzname(), fields(d)))
        locals_ = list(EXTRA_LOCAL)
        trs = transitions(z)
        if len(trs) > 5:
            trs = [trs[i * (len(trs) - 1) // 4] for i in range(5)]
        for t in trs:
            before, after = off(at(z, t - 1)), off(at(z, t))
            lo, hi = sorted((t + before, t + after))  # the local times the transition skips or repeats
            width = hi - lo
            for le in (lo - 1, lo, lo + width // 2, hi - 1, hi):
                locals_.append(dt.datetime.fromtimestamp(le, UTC).strftime("%Y-%m-%d %H:%M:%S"))
        for s in locals_:
            naive = dt.datetime.strptime(s, "%Y-%m-%d %H:%M:%S")
            w, i = local_offset(z, naive)
            print("O %s %s => %d %d" % (n, fields(naive), w, i))


main()

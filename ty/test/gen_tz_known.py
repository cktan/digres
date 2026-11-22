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
# A line of the third kind says what interval arithmetic gives in a zone (a
# day is a day of the clock, a month a month of the calendar, the
# microseconds exact), a fourth what date_trunc gives, a fifth what extract
# gives (the epoch is the instant):
#   A ZONE EPOCH MONTHS DAYS USECS => EPOCH
#   T ZONE EPOCH UNIT => EPOCH
#   P ZONE EPOCH UNIT => VALUE
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


DELTAS = [(0, 1, 0), (1, 0, 0), (-1, -1, 0), (0, 0, 3600 * 1000000), (13, 40, 90000000000)]
ARITH_INSTANTS = ["1985-07-01 06:30:00", "2000-04-01 12:00:00", "2024-03-09 12:00:00", "2024-11-02 12:00:00", "2024-01-31 10:30:00", "2037-12-31 12:00:00"]
UNITS = ["hour", "day", "week", "month", "year", "quarter", "decade"]
PARTS = ["timezone", "timezone_hour", "timezone_minute", "hour", "day", "month", "year", "dow", "doy", "epoch", "isoyear", "week"]


def add_months(naive, months):
    """The local time plus months, the day cut to the end of the month."""
    total = naive.year * 12 + (naive.month - 1) + months
    y, m = divmod(total, 12)
    m += 1
    import calendar
    day = min(naive.day, calendar.monthrange(y, m)[1])
    return naive.replace(year=y, month=m, day=day)


def utc_of_local(z, naive):
    """The instant of a local time by PostgreSQL's rule, in microseconds."""
    w, _ = local_offset(z, naive)
    return int(naive.replace(tzinfo=UTC).timestamp()) * 1000000 + w * 1000000


def arith(z, e, months, days, usecs):
    """PostgreSQL's timestamptz + interval, in microseconds, or None if Python can't."""
    naive = at(z, e).replace(tzinfo=None)
    micro = e * 1000000
    try:
        if months:
            naive = add_months(naive, months)
            micro = utc_of_local(z, naive)
            naive = at(z, micro // 1000000).replace(tzinfo=None)
        if days:
            naive = naive + dt.timedelta(days=days)
            micro = utc_of_local(z, naive)
        return micro + usecs
    except (OverflowError, ValueError):
        return None


def trunc(z, e, unit):
    d = at(z, e).replace(tzinfo=None)
    if unit == "hour":
        r = d.replace(minute=0, second=0)
        w, _ = off(at(z, e)), 0
        return int(r.replace(tzinfo=UTC).timestamp()) * 1000000 - off(at(z, e)) * 1000000
    if unit == "day":
        r = d.replace(hour=0, minute=0, second=0)
    elif unit == "week":
        r = (d - dt.timedelta(days=d.weekday())).replace(hour=0, minute=0, second=0)
    elif unit == "month":
        r = d.replace(day=1, hour=0, minute=0, second=0)
    elif unit == "quarter":
        r = d.replace(month=3 * ((d.month - 1) // 3) + 1, day=1, hour=0, minute=0, second=0)
    elif unit == "year":
        r = d.replace(month=1, day=1, hour=0, minute=0, second=0)
    elif unit == "decade":
        r = d.replace(year=(d.year // 10) * 10, month=1, day=1, hour=0, minute=0, second=0)
    return utc_of_local(z, r)


def part(z, e, unit):
    d = at(z, e)
    o = off(d)
    if unit == "timezone":
        return o
    if unit == "timezone_hour":
        return int(o / 3600)
    if unit == "timezone_minute":
        return int(o / 60) % 60 if o >= 0 else -(int(-o / 60) % 60)
    if unit == "hour":
        return d.hour
    if unit == "day":
        return d.day
    if unit == "month":
        return d.month
    if unit == "year":
        return d.year
    if unit == "dow":
        return (d.weekday() + 1) % 7
    if unit == "doy":
        return d.timetuple().tm_yday
    if unit == "epoch":
        return e
    if unit == "isoyear":
        return d.isocalendar()[0]
    if unit == "week":
        return d.isocalendar()[1]


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
        if names.index(n) % 4 == 0:  # every fourth zone: the arithmetic, trunc and parts
            for s in ARITH_INSTANTS:
                e = epoch_of(s)
                for m, d_, u in DELTAS:
                    r = arith(z, e, m, d_, u)
                    if r is not None:
                        print("A %s %d %d %d %d => %d" % (n, e, m, d_, u, r))
                for unit in UNITS:
                    print("T %s %d %s => %d" % (n, e, unit, trunc(z, e, unit)))
                for unit in PARTS:
                    print("P %s %d %s => %d" % (n, e, unit, part(z, e, unit)))


main()

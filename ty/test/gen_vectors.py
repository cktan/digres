#!/usr/bin/env python3
# Copyright (c) 2026 CK Tan
# SPDX-License-Identifier: MIT
#
# gen_vectors.py -- write test/vectors.txt: test vectors for ty's decimal
# operations, from a reference of plain Python integers that follows the
# rules of ../DESIGN.md and ty_decimal.h, not ty's code. The run is
# reproducible: the seed is fixed, so the file only changes with this one.
#
#   test/gen_vectors.py > test/vectors.txt
#
# A line is "OP ARGS => RESULT". An integer value is a decimal integer
# and a scale an integer. RESULT is "V S", a number, or "E NAME" for an
# error: RANGE, DIVZERO or INVALID.
#
# The rules, as the operations of two values of one type give them:
# exact, or an error if the result does not fit; a quotient, a round and
# a fit round half away from zero; trunc and a cut cut toward zero.

import math
import random
import sys
from decimal import ROUND_HALF_UP, Decimal, getcontext

getcontext().prec = 80

random.seed(20261120)

D64 = 10**17 - 1
D128 = 10**36 - 1

out = []


def emit(op, args, res):
    """One line: the operation, its arguments, and its result."""
    if isinstance(res, tuple) and res[0] == "E":
        r = "E " + res[1]
    elif isinstance(res, tuple):
        r = "%d %d" % res
    else:
        r = "%d" % res
    out.append("%s %s => %s" % (op, " ".join(str(a) for a in args), r))


def rnd(n, d):
    """n / d rounded half away from zero, d positive."""
    q, r = divmod(abs(n), d)
    if 2 * r >= d:
        q += 1
    return q if n >= 0 else -q


def cut(n, d):
    """n / d cut toward zero, d positive."""
    q = abs(n) // d
    return q if n >= 0 else -q


def digits(v):
    return len(str(abs(v)))


def align(a, sa, b, sb):
    s = max(sa, sb)
    return a * 10 ** (s - sa), b * 10 ** (s - sb), s


def gen_value(maxv, maxs):
    """A random value and scale: edges, powers of ten, and random digits."""
    s = random.choice([0, 0, 1, 2, 3, 5, maxs // 2, maxs - 1, maxs, random.randint(0, maxs)])
    kind = random.random()
    if kind < 0.15:
        v = random.choice([0, 1, -1, maxv, -maxv, maxv - 1, 10 ** random.randint(0, len(str(maxv)) - 1)])
    elif kind < 0.30:
        v = random.randint(-1000, 1000)
    else:
        nd = random.randint(1, len(str(maxv)))
        v = random.randint(0, 10 ** nd - 1)
        v = min(v, maxv)
        if random.random() < 0.5:
            v = -v
    return v, s


def gen_pair(maxv, maxs):
    a = gen_value(maxv, maxs)
    b = gen_value(maxv, maxs)
    if random.random() < 0.1:
        b = (a[0], random.randint(0, maxs))
    return a, b


def result_or_range(v, s, maxv):
    if abs(v) > maxv:
        return ("E", "RANGE")
    return (v, s)


def do_add(a, sa, b, sb, maxv, maxs, sub=False):
    x, y, s = align(a, sa, b, sb)
    r = x - y if sub else x + y
    return result_or_range(r, s, maxv)


def do_mul(a, sa, b, sb, maxv, maxs):
    p = a * b
    s = sa + sb
    if s > maxs or abs(p) > maxv:
        return ("E", "RANGE")
    return (p, s)


def do_mod(a, sa, b, sb):
    x, y, s = align(a, sa, b, sb)
    if y == 0:
        return ("E", "DIVZERO")
    r = abs(x) % abs(y)
    return (-r if x < 0 else r, s)


def do_gcd(a, sa, b, sb, maxv):
    x, y, s = align(a, sa, b, sb)
    return result_or_range(math.gcd(x, y), s, maxv)


def do_lcm(a, sa, b, sb, maxv):
    x, y, s = align(a, sa, b, sb)
    if x == 0 or y == 0:
        return (0, s)
    return result_or_range(abs(x * y) // math.gcd(x, y), s, maxv)


def do_cmp(a, sa, b, sb):
    x, y, _ = align(a, sa, b, sb)
    return (x > y) - (x < y)


def do_round(v, s, n, maxv, maxs, trunc, is64):
    """round/trunc to n places: n is the result's scale."""
    f = cut if trunc else rnd
    if is64:
        if n >= s:
            if n > maxs:
                return ("E", "RANGE")
            return result_or_range(v * 10 ** (n - s), n, maxv)
        if n >= 0:
            return (f(v, 10 ** (s - n)), n)
        drop = s - n
        q = 0 if drop > 18 else f(v, 10 ** drop)
        return result_or_range(q * 10 ** (-n), 0, maxv) if q != 0 else (0, 0)
    if n > maxs:
        n = maxs
    if n >= s:
        return result_or_range(v * 10 ** (n - s), n, maxv)
    if n >= 0:
        return (f(v, 10 ** (s - n)), n)
    drop = s - n
    q = 0 if drop > maxs else f(v, 10 ** drop)
    return result_or_range(q * 10 ** (-n), 0, maxv) if q != 0 else (0, 0)


def do_ceil_floor(v, s, floor):
    p = 10 ** s
    q = cut(v, p)
    if abs(v) % p != 0:
        if floor and v < 0:
            q -= 1
        if not floor and v > 0:
            q += 1
    return (q, 0)


def do_div128(a, sa, b, sb, rscale, is_cut):
    if b == 0:
        return ("E", "DIVZERO")
    e = rscale + sb - sa
    if e >= 0:
        num, den = abs(a) * 10 ** e, abs(b)
    else:
        num, den = abs(a), abs(b) * 10 ** (-e)
    q, rem = divmod(num, den)
    if not is_cut and 2 * rem >= den:
        q += 1
    if q > D128:
        return ("E", "RANGE")
    neg = (a < 0) != (b < 0) and q != 0
    return (-q if neg else q, rscale)


def fit64(v, s):
    """A decimal128 fitted to a decimal64, as ty does."""
    if s > 17:
        v = rnd(v, 10 ** (s - 17))
        s = 17
    d = digits(v)
    if d > 17 and d - 17 <= s:
        ex = d - 17
        v = rnd(v, 10 ** ex)
        s -= ex
        d = digits(v)
        if d > 17 and s > 0:
            v = rnd(v, 10)
            s -= 1
    if abs(v) > D64:
        return ("E", "RANGE")
    return (v, s)


def do_sqrt(v, s, rscale):
    if v < 0:
        return ("E", "INVALID")
    e = 2 * rscale - s
    n = v * 10 ** e if e >= 0 else v // 10 ** (-e)
    root = math.isqrt(n)
    rem = n - root * root
    if rem > root:
        root += 1
    if root > D128:
        return ("E", "RANGE")
    return (root, rscale)


def do_width_bucket(op, os_, b1, b1s, b2, b2s, count):
    if count <= 0:
        return ("E", "INVALID")
    s = max(os_, b1s, b2s)
    o = op * 10 ** (s - os_)
    lo = b1 * 10 ** (s - b1s)
    hi = b2 * 10 ** (s - b2s)
    if lo == hi:
        return ("E", "INVALID")
    if lo < hi:
        if o < lo:
            return 0
        if o >= hi:
            return count + 1
        num, den = o - lo, hi - lo
    else:
        if o > lo:
            return 0
        if o <= hi:
            return count + 1
        num, den = lo - o, lo - hi
    return num * count // den + 1


def main():
    for pre, maxv, maxs, is64 in (("d64", D64, 17, True), ("d128", D128, 36, False)):
        n_rand = 150
        pairs = [gen_pair(maxv, maxs) for _ in range(n_rand)]
        # Edges: the ends of the range, at scales that do and do not align.
        for av in (maxv, -maxv, 1, 0):
            for bv in (maxv, -maxv, 1, 0, 7):
                for sa, sb in ((0, 0), (0, maxs), (maxs, 0), (maxs, maxs), (1, maxs)):
                    pairs.append(((av, sa), (bv, sb)))
        for (a, sa), (b, sb) in pairs:
            emit(pre + ".add", (a, sa, b, sb), do_add(a, sa, b, sb, maxv, maxs))
            emit(pre + ".sub", (a, sa, b, sb), do_add(a, sa, b, sb, maxv, maxs, True))
            emit(pre + ".mul", (a, sa, b, sb), do_mul(a, sa, b, sb, maxv, maxs))
            emit(pre + ".mod", (a, sa, b, sb), do_mod(a, sa, b, sb))
            emit(pre + ".gcd", (a, sa, b, sb), do_gcd(a, sa, b, sb, maxv))
            emit(pre + ".lcm", (a, sa, b, sb), do_lcm(a, sa, b, sb, maxv))
            emit(pre + ".cmp", (a, sa, b, sb), do_cmp(a, sa, b, sb))
        vals = [gen_value(maxv, maxs) for _ in range(120)]
        vals += [(maxv, 0), (-maxv, 0), (maxv, maxs), (0, maxs), (5, 1), (-5, 1), (15, 1), (-15, 1), (125, 2), (-125, 2)]
        for v, s in vals:
            for n in (-40, -20, -18, -3, -1, 0, 1, 2, 5, maxs - 1, maxs, maxs + 1, maxs + 4, random.randint(-5, maxs + 2)):
                emit(pre + ".round", (v, s, n), do_round(v, s, n, maxv, maxs, False, is64))
                emit(pre + ".trunc", (v, s, n), do_round(v, s, n, maxv, maxs, True, is64))
            emit(pre + ".ceil", (v, s), do_ceil_floor(v, s, False))
            emit(pre + ".floor", (v, s), do_ceil_floor(v, s, True))
    # Division: the quotient, at a scale the caller picks.
    for _ in range(300):
        (a, sa), (b, sb) = gen_pair(D128, 36)
        r = random.choice([0, 1, 5, 16, 20, 30, 36, random.randint(0, 36)])
        c = random.choice([0, 1])
        emit("d128.div", (a, sa, b, sb, r, c), do_div128(a, sa, b, sb, r, bool(c)))
    for a, sa, b, sb, r in ((1, 36, D128, 0, 0), (D128, 36, D128, 0, 0), (D128 // 2 + 1, 36, 1, 0, 0),
                            (5, 36, 7, 0, 3), (D128, 36, 3, 0, 36), (D128, 0, 1, 0, 36), (1, 0, 3, 0, 20), (2, 0, 3, 0, 20)):
        for c in (0, 1):
            emit("d128.div", (a, sa, b, sb, r, c), do_div128(a, sa, b, sb, r, bool(c)))
    # A quotient that rounds to nothing: the divisor at the scale of the
    # dividend passes 128 bits, so the dividend is below half of it.
    for bv in (D128, D128 - 12345, 10 ** 30 + 7, 3 * 10 ** 33, 10 ** 35 + 999):
        for av in (D128, D128 // 2 + 1, 5 * 10 ** 35, 1, -D128):
            for r in (0, 1, 2):
                for c in (0, 1):
                    emit("d128.div", (av, 36, bv, 0, r, c),
                         do_div128(av, 36, bv, 0, r, bool(c)))
    # The same, where the divisor's overflowed product wraps to 2^37: a
    # dividend of 7e10 to 1.3e11 is then half of it or more, as the wrapped
    # number, but is below half of the true divisor.
    wrap = 2 * pow(5 ** 36, -1, 2 ** 92) % 2 ** 92
    for av in (70000000000, 100000000000, 130000000000, -100000000000):
        for c in (0, 1):
            emit("d128.div", (av, 36, wrap, 0, 0, c),
                 do_div128(av, 36, wrap, 0, 0, bool(c)))
    for _ in range(300):
        (a, sa), (b, sb) = gen_pair(D64, 17)
        r = random.choice([0, 1, 5, 16, 20, 30, 36, random.randint(0, 36)])
        q = do_div128(a, sa, b, sb, r, False)
        res = q if q[0] == "E" else fit64(q[0], q[1])
        emit("d64.div", (a, sa, b, sb, r), res)
    for _ in range(200):
        v, s = gen_value(D128, 36)
        emit("d64.fit", (v, s), fit64(v, s) if abs(v) <= D128 else ("E", "RANGE"))
    for v, s in ((D128, 20), (D128, 36), (10 ** 17, 0), (10 ** 18, 0), (10 ** 19 + 5 * 10 ** 2, 20), (D64, 17)):
        emit("d64.fit", (v, s), fit64(v, s))
    # Square roots.
    for _ in range(200):
        v, s = gen_value(D128, 36)
        r = random.choice([0, 1, 2, 5, 16, 20, 30, 36, max(s, random.randint(0, 36))])
        emit("d128.sqrt", (v, s, r), do_sqrt(v, s, r))
    for v, s, r in ((2, 0, 20), (3, 0, 1), (1600, 2, 2), (0, 3, 3), (D128, 0, 0), (D128, 36, 36), (-4, 0, 2), (10 ** 35, 0, 1)):
        emit("d128.sqrt", (v, s, r), do_sqrt(v, s, r))
    # exp, ln, log and the powers, to at most 16 places: ty computes them in
    # long double, some 33 digits, so a result of that many places is exact
    # but for a value on the very edge of a rounding.
    def dec(v, s):
        return Decimal(v).scaleb(-s)

    def at(x, r):
        q = x.quantize(Decimal(1).scaleb(-r), rounding=ROUND_HALF_UP)
        return (int(q.scaleb(r)), r)

    for _ in range(120):
        v, s = random.randint(-30000, 30000), 3
        r = random.randint(0, 16)
        emit("d128.exp", (v, s, r), at(dec(v, s).exp(), r))
    for _ in range(120):
        v, s = random.randint(1, 10 ** 12), random.choice([0, 2, 4, 6])
        r = random.randint(0, 16)
        emit("d128.ln", (v, s, r), at(dec(v, s).ln(), r))
    for _ in range(120):
        b, bs = random.randint(2, 10 ** 6), random.choice([0, 2])
        v, s = random.randint(1, 10 ** 9), random.choice([0, 3])
        r = random.randint(0, 16)
        if dec(b, bs) == 1:
            continue
        emit("d128.log", (b, bs, v, s, r), at(dec(v, s).ln() / dec(b, bs).ln(), r))
    for _ in range(150):
        v, s = random.choice([-1, 1]) * random.randint(1, 99999), random.choice([0, 1, 2, 3])
        n = random.randint(-12, 12)
        r = random.randint(0, 14)
        x = dec(v, s) ** n
        if abs(x) >= 10 ** 15 or (abs(x) < Decimal(10) ** -12 and x != 0):
            continue
        emit("d128.powi", (v, s, n, r), at(x, r))
    for _ in range(120):
        v, s = random.randint(1, 99999), random.choice([0, 1, 2])
        ev, es = random.choice([5, 15, 25, 125, -5, -15, 314159, 2718, -75]), random.choice([1, 1, 2, 3, 1, 1, 5, 3, 2])
        r = random.randint(0, 14)
        x = (dec(v, s).ln() * dec(ev, es)).exp()
        if abs(x) >= 10 ** 15 or x < Decimal(10) ** -12:
            continue
        emit("d128.powr", (v, s, ev, es, r), at(x, r))
    # Factorial.
    for n in list(range(0, 36)) + [-1, 40, 100]:
        f = math.factorial(n) if n >= 0 else None
        if n < 0:
            res = ("E", "INVALID")
        elif f > D128:
            res = ("E", "RANGE")
        else:
            res = (f, 0)
        emit("d128.fact", (n,), res)
    # width_bucket.
    for _ in range(300):
        lo, los = gen_value(10 ** 12, 8)
        hi, his = gen_value(10 ** 12, 8)
        op, ops_ = gen_value(10 ** 12, 8)
        cnt = random.choice([1, 2, 5, 10, 100, 1000, 2147483647, 0, -3])
        r = do_width_bucket(op, ops_, lo, los, hi, his, cnt)
        emit("d128.wb", (op, ops_, lo, los, hi, his, cnt), r)
    sys.stdout.write("\n".join(out) + "\n")


main()

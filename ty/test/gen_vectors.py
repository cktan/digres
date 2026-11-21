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
# The boolean parse (bool.parse TEXT => 1, 0 or E INVALID) follows the
# integers. The integer operations (i16, i32 and i64) are at the end: "OP A B => V",
# or "E RANGE" or "E DIVZERO". They follow ../include/ty_int.h: exact or
# an error; a quotient cuts toward zero; the remainder has the sign of the
# dividend; a shift is by 0 to N-1 bits, or RANGE.
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
    elif isinstance(res, str):
        r = res
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
    # exp, ln, log and the powers, to at most 12 digits: ty computes them in
    # double, some 15 digits, so a result of that many digits is exact but
    # for a value on the very edge of a rounding.
    def dec(v, s):
        return Decimal(v).scaleb(-s)

    def at(x, r):
        q = x.quantize(Decimal(1).scaleb(-r), rounding=ROUND_HALF_UP)
        return (int(q.scaleb(r)), r)

    def places(x):
        """A number of places that leaves a result of at most 12 digits."""
        ip = len(str(int(abs(x))))
        return random.randint(0, max(0, 12 - ip))

    for _ in range(120):
        v, s = random.randint(-20000, 20000), 3
        x = dec(v, s).exp()
        r = places(x)
        emit("d128.exp", (v, s, r), at(x, r))
    for _ in range(120):
        v, s = random.randint(1, 10 ** 12), random.choice([0, 2, 4, 6])
        x = dec(v, s).ln()
        r = places(x)
        emit("d128.ln", (v, s, r), at(x, r))
    for _ in range(120):
        b, bs = random.randint(2, 10 ** 6), random.choice([0, 2])
        v, s = random.randint(1, 10 ** 9), random.choice([0, 3])
        if dec(b, bs) == 1:
            continue
        x = dec(v, s).ln() / dec(b, bs).ln()
        r = places(x)
        emit("d128.log", (b, bs, v, s, r), at(x, r))
    for _ in range(150):
        v, s = random.choice([-1, 1]) * random.randint(1, 99999), random.choice([0, 1, 2, 3])
        n = random.randint(-12, 12)
        x = dec(v, s) ** n
        if abs(x) >= 10 ** 12 or (abs(x) < Decimal(10) ** -12 and x != 0):
            continue
        r = places(x)
        emit("d128.powi", (v, s, n, r), at(x, r))
    for _ in range(120):
        v, s = random.randint(1, 99999), random.choice([0, 1, 2])
        ev, es = random.choice([5, 15, 25, 125, -5, -15, 314159, 2718, -75]), random.choice([1, 1, 2, 3, 1, 1, 5, 3, 2])
        x = (dec(v, s).ln() * dec(ev, es)).exp()
        if abs(x) >= 10 ** 12 or x < Decimal(10) ** -12:
            continue
        r = places(x)
        emit("d128.powr", (v, s, ev, es, r), at(x, r))
    # A power of the base is exact, for the bases 10 and 2.
    for e in range(0, 13):
        emit("d128.log", (10, 0, 10 ** e, 0, 6), (e * 10 ** 6, 6))
    for e in range(0, 21):
        emit("d128.log", (2, 0, 2 ** e, 0, 6), (e * 10 ** 6, 6))
    # Overflow and underflow of a double are errors.
    emit("d128.exp", (1000, 0, 5), ("E", "RANGE"))
    emit("d128.exp", (-1000, 0, 5), ("E", "UNDERFLOW"))
    emit("d128.powi", (10, 0, 400, 0), ("E", "RANGE"))
    emit("d128.powi", (1, 1, 400, 0), ("E", "UNDERFLOW"))
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
    gen_ints()
    gen_bools()
    sys.stdout.write("\n".join(out) + "\n")


def trunc_div(a, b):
    """a / b cut toward zero, b not zero."""
    q = abs(a) // abs(b)
    return q if (a >= 0) == (b >= 0) else -q


def wrap(v, bits):
    """v as a signed integer of that many bits, the high bits lost."""
    v &= (1 << bits) - 1
    return v - (1 << bits) if v >> (bits - 1) else v


def int_values(bits):
    """Edges, small values, and random ones of every length."""
    lo, hi = -(1 << (bits - 1)), (1 << (bits - 1)) - 1
    vals = {0, 1, -1, 2, -2, 3, -3, 10, -10, lo, lo + 1, hi, hi - 1,
            hi // 2, lo // 2, 1 << (bits // 2), -(1 << (bits // 2))}
    for _ in range(40):
        nb = random.randint(1, bits - 1)
        vals.add(random.randint(-(1 << nb), (1 << nb) - 1))
    return sorted(v for v in vals if lo <= v <= hi)


def bool_of(t):
    """True, False, or None, for a text with no space in it: the rule of
    ty_bool.h, written from its words."""
    t = t.lower()
    n = len(t)
    if n == 0:
        return None
    for word, val in (("true", True), ("yes", True), ("false", False),
                      ("no", False)):
        if n <= len(word) and word.startswith(t):
            return val
    if t == "on":
        return True
    if n >= 2 and n <= 3 and "off".startswith(t):
        return False
    if t == "1":
        return True
    if t == "0":
        return False
    return None


def gen_bools():
    """The vectors of the boolean parse: the words, their cut-short forms,
    and near misses, in both cases."""
    texts = []
    for w in ("true", "false", "yes", "no", "on", "off", "1", "0", "t", "f",
              "y", "n", "o", "tru", "tr", "fals", "fal", "ye", "of", "truee",
              "yess", "noo", "onn", "offf", "10", "01", "00", "2", "x", "tx",
              "ono", "nop", "fa1", "-1", "+1", "1.0", "ofx"):
        texts += [w, w.upper(), w.capitalize()]
    for t in sorted(set(texts)):
        v = bool_of(t)
        emit("bool.parse", (t,), ("E", "INVALID") if v is None else int(v))


def gen_ints():
    """The vectors of the integer operations, for each width."""
    for bits in (16, 32, 64):
        p = "i%d" % bits
        lo, hi = -(1 << (bits - 1)), (1 << (bits - 1)) - 1
        vals = int_values(bits)

        def fit(v):
            return v if lo <= v <= hi else ("E", "RANGE")

        pairs = [(a, b) for a in vals for b in vals]
        random.shuffle(pairs)
        for a, b in pairs[:600]:
            emit(p + ".add", (a, b), fit(a + b))
            emit(p + ".sub", (a, b), fit(a - b))
            emit(p + ".mul", (a, b), fit(a * b))
            emit(p + ".div", (a, b), ("E", "DIVZERO") if b == 0
                 else fit(trunc_div(a, b)))
            emit(p + ".mod", (a, b), ("E", "DIVZERO") if b == 0
                 else a - b * trunc_div(a, b))
            emit(p + ".gcd", (a, b), fit(math.gcd(a, b)))
            emit(p + ".lcm", (a, b), fit(0 if a == 0 or b == 0
                                         else abs(a * b) // math.gcd(a, b)))
            emit(p + ".larger", (a, b), max(a, b))
            emit(p + ".smaller", (a, b), min(a, b))
            emit(p + ".and", (a, b), a & b)
            emit(p + ".or", (a, b), a | b)
            emit(p + ".xor", (a, b), a ^ b)
        for a in vals:
            emit(p + ".neg", (a,), fit(-a))
            emit(p + ".abs", (a,), fit(abs(a)))
            emit(p + ".inc", (a,), fit(a + 1))
            emit(p + ".dec", (a,), fit(a - 1))
            emit(p + ".not", (a,), ~a)
            for n in (-1, 0, 1, 2, bits // 2, bits - 1, bits, bits + 1):
                ok = 0 <= n < bits
                emit(p + ".shl", (a, n), wrap(a << n, bits) if ok else ("E", "RANGE"))
                emit(p + ".shr", (a, n), (a >> n) if ok else ("E", "RANGE"))
    # Narrowing.
    for src, dst in ((32, 16), (64, 32), (64, 16)):
        dlo, dhi = -(1 << (dst - 1)), (1 << (dst - 1)) - 1
        for a in int_values(src) + [dlo - 1, dhi + 1, dlo, dhi]:
            if -(1 << (src - 1)) <= a < (1 << (src - 1)):
                emit("i%d.to%d" % (src, dst), (a,),
                     a if dlo <= a <= dhi else ("E", "RANGE"))
    # An int64 from and to a double.
    for a in int_values(64):
        emit("i64.todouble", (a,), repr(float(a)))
    doubles = [0.0, 0.5, 1.5, 2.5, -0.5, -1.5, -2.5, 0.49999999999999994,
               9223372036854775807.0, 9223372036854775808.0,
               -9223372036854775808.0, -9223372036854777856.0, 1e19, -1e19,
               float("inf"), float("-inf"), float("nan"), 4503599627370497.5,
               123456789.5, -123456789.5]
    for _ in range(60):
        doubles.append(random.uniform(-1e19, 1e19))
        doubles.append(random.uniform(-1e5, 1e5))
    for d in doubles:
        if d != d or d in (float("inf"), float("-inf")):
            res = ("E", "RANGE")
        else:
            n = round(d)
            res = n if -(1 << 63) <= n < (1 << 63) else ("E", "RANGE")
        emit("i64.fromdouble", (repr(d),), res)


main()

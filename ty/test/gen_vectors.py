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
import re
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
    gen_uuids()
    gen_byteas()
    gen_hashes()
    gen_encodings()
    gen_int_text()
    gen_floats()
    gen_float_parse()
    gen_trig()
    gen_float_aggs()
    gen_bits()
    gen_inet()
    gen_text()
    gen_like()
    gen_money()
    gen_interval()
    gen_datetime()
    gen_geo()
    gen_typmod()
    gen_timetz()
    gen_overlaps()
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


def uuid_of(t):
    """The 32 digits, lowercase, of a uuid text, or None: 32 hexadecimal
    digits, a hyphen allowed after each group of four, and all in braces
    if you like (ty_uuid.h)."""
    m = re.match(r"^(\{)?" + "-?".join(["([0-9a-fA-F]{4})"] * 8) +
                 r"(?(1)\})$", t)
    return "".join(m.groups()[1:]).lower() if m else None


def uuid_texts():
    """Good and bad uuid texts, in many forms."""
    out = ["", "{", "}", "{}", "-"]
    for _ in range(60):
        h = "%032x" % random.getrandbits(128)
        g = [h[i:i + 4] for i in range(0, 32, 4)]
        form = random.randrange(8)
        if form == 0:
            t = "-".join([h[:8], h[8:12], h[12:16], h[16:20], h[20:]])
        elif form == 1:
            t = h
        elif form == 2:
            t = "{" + "-".join([h[:8], h[8:12], h[12:16], h[16:20], h[20:]]) + "}"
        elif form == 3:
            t = "".join(x + ("-" if random.random() < .5 and i < 7 else "")
                        for i, x in enumerate(g))
        elif form == 4:
            t = ("-".join(g)).upper()
        elif form == 5:
            t = h[:-1]
        elif form == 6:
            t = h + "0"
        else:
            t = "{" + h
        if random.random() < .15:
            i = random.randrange(len(t))
            t = t[:i] + random.choice("gz-{}.x") + t[i + 1:]
        out.append(t)
    out += ["a0eebc99-9c0b-4ef8-bb6d-6bb9bd380a11",
            "{a0eebc99-9c0b-4ef8-bb6d-6bb9bd380a11}",
            "A0EEBC99-9C0B-4EF8-BB6D-6BB9BD380A11",
            "a0ee-bc99-9c0b-4ef8-bb6d-6bb9-bd38-0a11",
            "a0eebc999c0b4ef8bb6d6bb9bd380a11",
            "-a0eebc999c0b4ef8bb6d6bb9bd380a11",
            "a0eebc99-9c0b-4ef8-bb6d-6bb9bd380a11-",
            "a0eebc99--9c0b-4ef8-bb6d-6bb9bd380a11"]
    return [t for t in out if not any(c.isspace() for c in t)]


def gen_uuids():
    """The vectors of uuid: parse, format, compare, version, time."""
    for t in sorted(set(uuid_texts())):
        h = uuid_of(t)
        emit("uuid.parse", (t if t else '""',), ("E", "INVALID") if h is None else h)
    hexes = ["%032x" % random.getrandbits(128) for _ in range(80)]
    for ver in range(0, 9):
        for var in (0x00, 0x80, 0xc0, 0xe0):
            b = bytearray(random.getrandbits(8) for _ in range(16))
            b[6] = (b[6] & 0x0f) | (ver << 4)
            b[8] = (b[8] & 0x3f) | var
            hexes.append(b.hex())
    hexes += ["0" * 32, "f" * 32]
    for h in hexes:
        emit("uuid.format", (h,),
             "-".join([h[:8], h[8:12], h[12:16], h[16:20], h[20:]]))
        b = bytes.fromhex(h)
        var_ok = (b[8] & 0xc0) == 0x80
        emit("uuid.version", (h,), (b[6] >> 4) if var_ok else -1)
        res = "NONE"
        if var_ok and b[6] >> 4 == 1:
            low = int.from_bytes(b[0:4], "big")
            mid = int.from_bytes(b[4:6], "big")
            hi = ((b[6] & 0x0f) << 8) | b[7]
            ticks = (hi << 48) | (mid << 32) | low
            res = str(ticks // 10 - 12219292800 * 1000000)
        elif var_ok and b[6] >> 4 == 7:
            res = str(int.from_bytes(b[0:6], "big") * 1000)
        emit("uuid.unixus", (h,), res)
    for h in hexes + ["0" * 31 + "1", "f" * 31 + "e", "00" * 8 + "ff" * 8,
                      "ff" * 8 + "00" * 8, "0" * 32, "f" * 32]:
        n = int(h, 16)
        emit("uuid.inc", (h,), ("E", "RANGE") if n == 2 ** 128 - 1 else "%032x" % (n + 1))
        emit("uuid.dec", (h,), ("E", "RANGE") if n == 0 else "%032x" % (n - 1))
    for _ in range(200):
        a, b2 = random.choice(hexes), random.choice(hexes)
        if random.random() < .1:
            b2 = a
        emit("uuid.cmp", (a, b2), (a > b2) - (a < b2))


def hx(b):
    """A bytea as a token: its hexadecimal digits, or - if it is empty."""
    return b.hex() if b else "-"


def rand_bytes(maxlen=20):
    """Random bytes of a random length, the edge values among them."""
    n = random.choice([0, 1, 2, 3, 4, 7, 8, 9, random.randint(0, maxlen)])
    return bytes(random.choice([0, 1, 0x1f, 0x20, 0x5c, 0x7e, 0x7f, 0x80, 0xff,
                                random.getrandbits(8)]) for _ in range(n))


def gen_byteas():
    """The vectors of bytea: text, compare, cat, reverse, substring,
    overlay, position, bits and bytes, and integers."""
    # Text: the escaped form and the hex form read, and written.
    texts = ["", "abc", "a\\\\b", "\\000", "\\377", "\\400", "\\378",
             "\\", "a\\", "\\1", "\\12", "\\x", "\\x61", "\\x6", "\\x6g",
             "\\xG1", "\\xAbCd", "\\x00ff", "x\\x61", "\\\\x61", "\\0001",
             "\\x6162636465666768"]
    for t in texts:
        res = ("E", "INVALID")
        try:
            if t.startswith("\\x"):
                h = t[2:]
                if len(h) % 2 == 0 and re.fullmatch(r"[0-9a-fA-F]*", h):
                    res = hx(bytes.fromhex(h))
            else:
                out = bytearray()
                i = 0
                ok = True
                while i < len(t):
                    if t[i] != "\\":
                        out.append(ord(t[i]))
                        i += 1
                    elif re.fullmatch(r"[0-3][0-7][0-7]", t[i + 1:i + 4]):
                        out.append(int(t[i + 1:i + 4], 8))
                        i += 4
                    elif t[i + 1:i + 2] == "\\":
                        out.append(0x5c)
                        i += 2
                    else:
                        ok = False
                        break
                if ok:
                    res = hx(bytes(out))
        except ValueError:
            res = ("E", "INVALID")
        emit("bytea.parse", (t if t else '""',), res)
    for _ in range(80):
        b = rand_bytes()
        emit("bytea.fmthex", (hx(b),), "\\x" + b.hex())
        if 0x20 not in b:
            e = ""
            for c in b:
                e += "\\\\" if c == 0x5c else ("\\%03o" % c if c < 0x20 or c > 0x7e else chr(c))
            emit("bytea.fmtesc", (hx(b),), e if e else '""')
    pairs = [(rand_bytes(6), rand_bytes(6)) for _ in range(150)]
    for a, b in pairs:
        c = (a > b) - (a < b)
        emit("bytea.cmp", (hx(a), hx(b)), c)
        for nm, fn in (("eq", a == b), ("ne", a != b), ("lt", a < b),
                       ("le", a <= b), ("gt", a > b), ("ge", a >= b)):
            emit("bytea." + nm, (hx(a), hx(b)), int(fn))
        emit("bytea.cat", (hx(a), hx(b)), hx(a + b))
        emit("bytea.position", (hx(a), hx(b)), a.find(b) + 1)
    for _ in range(40):
        a = rand_bytes()
        emit("bytea.reverse", (hx(a),), hx(a[::-1]))
        emit("bytea.bitcount", (hx(a),), bin(int.from_bytes(a, "big")).count("1"))
        for idx in (-1, 0, 1, len(a) - 1, len(a), 7, 9):
            if len(a) > 0 and 0 <= idx < len(a):
                emit("bytea.getbyte", (hx(a), idx), a[idx])
            else:
                emit("bytea.getbyte", (hx(a), idx), ("E", "RANGE"))
        for idx in (-1, 0, 5, 8 * len(a) - 1, 8 * len(a), 8 * len(a) + 3):
            if 0 <= idx < 8 * len(a):
                emit("bytea.getbit", (hx(a), idx), (a[idx // 8] >> (idx % 8)) & 1)
            else:
                emit("bytea.getbit", (hx(a), idx), ("E", "RANGE"))
        for idx in (-1, 0, len(a) - 1, len(a)):
            for val in (0, 255, 256, -1, 65):
                if 0 <= idx < len(a):
                    r = bytearray(a)
                    r[idx] = val & 0xff
                    emit("bytea.setbyte", (hx(a), idx, val), hx(bytes(r)))
                else:
                    emit("bytea.setbyte", (hx(a), idx, val), ("E", "RANGE"))
        for idx in (-1, 0, 3, 8 * len(a) - 1, 8 * len(a)):
            for bit in (0, 1, 2, -1):
                if not 0 <= idx < 8 * len(a):
                    res = ("E", "RANGE")
                elif bit not in (0, 1):
                    res = ("E", "INVALID")
                else:
                    r = bytearray(a)
                    if bit:
                        r[idx // 8] |= 1 << (idx % 8)
                    else:
                        r[idx // 8] &= ~(1 << (idx % 8)) & 0xff
                    res = hx(bytes(r))
                emit("bytea.setbit", (hx(a), idx, bit), res)
    big = 2 ** 31 - 1
    for start in (-5, -1, 0, 1, 2, 3, 10, big, -big - 1):
        for ln in (-1, 0, 1, 2, 5, 100, big, 2 ** 31 - 2):
            for has in (1,):
                if ln < 0:
                    res = ("E", "INVALID")
                else:
                    end = start + ln
                    first = max(start, 1)
                    if end > big:
                        res = (first - 1, -1)
                    elif end < 1:
                        res = (0, 0)
                    else:
                        res = (first - 1, end - first)
                emit("bytea.substr", (start, ln, 1), res)
        emit("bytea.substr", (start, 0, 0), (max(start, 1) - 1, -1))
    for _ in range(300):
        a, b = rand_bytes(8), rand_bytes(4)
        sp = random.choice([-1, 0, 1, 2, 3, 5, 9, 12, big])
        sl = random.choice([-5, -1, 0, 1, 2, 4, 20, big])
        if sp <= 0:
            res = ("E", "INVALID")
        elif sp + sl > big:
            res = ("E", "RANGE")
        else:
            res = hx(a[:sp - 1] + b + a[max(sp + sl, 1) - 1:])
        emit("bytea.overlay", (hx(a), hx(b), sp, sl), res)
    for _ in range(60):
        a = bytes(random.getrandbits(8) for _ in range(random.randint(0, 10)))
        for bits in (16, 32, 64):
            if len(a) > bits // 8:
                res = ("E", "RANGE")
            else:
                res = wrap(int.from_bytes(a, "big"), bits)
            emit("bytea.toint%d" % bits, (hx(a),), res)
    for bits in (16, 32, 64):
        for v in int_values(bits):
            emit("bytea.fromint%d" % bits, (v,),
                 hx((v & ((1 << bits) - 1)).to_bytes(bits // 8, "big")))


M32 = 0xFFFFFFFF


def rot32(x, k):
    """x rotated left by k bits, in 32 bits."""
    return ((x << k) | (x >> (32 - k))) & M32


def lk_mix(a, b, c):
    """Bob Jenkins's mix of three 32-bit values, lookup3."""
    a = (a - c) & M32; a ^= rot32(c, 4); c = (c + b) & M32
    b = (b - a) & M32; b ^= rot32(a, 6); a = (a + c) & M32
    c = (c - b) & M32; c ^= rot32(b, 8); b = (b + a) & M32
    a = (a - c) & M32; a ^= rot32(c, 16); c = (c + b) & M32
    b = (b - a) & M32; b ^= rot32(a, 19); a = (a + c) & M32
    c = (c - b) & M32; c ^= rot32(b, 4); b = (b + a) & M32
    return a, b, c


def lk_final(a, b, c):
    """Bob Jenkins's final mix of three 32-bit values, lookup3."""
    c ^= b; c = (c - rot32(b, 14)) & M32
    a ^= c; a = (a - rot32(c, 11)) & M32
    b ^= a; b = (b - rot32(a, 25)) & M32
    c ^= b; c = (c - rot32(b, 16)) & M32
    a ^= c; a = (a - rot32(c, 4)) & M32
    b ^= a; b = (b - rot32(a, 14)) & M32
    c ^= b; c = (c - rot32(b, 24)) & M32
    return a, b, c


def le32(b):
    """Up to 4 bytes as a little-endian number."""
    return int.from_bytes(b, "little")


def pg_hash_bytes(k, seed):
    """PostgreSQL's hash_bytes_extended of the bytes k, from its algorithm
    (lookup3, little-endian): 64 bits; the low 32 are hash_bytes."""
    n = len(k)
    a = b = c = (0x9E3779B9 + n + 3923095) & M32
    if seed:
        a = (a + (seed >> 32)) & M32
        b = (b + (seed & M32)) & M32
        a, b, c = lk_mix(a, b, c)
    i = 0
    while n - i >= 12:
        a = (a + le32(k[i:i + 4])) & M32
        b = (b + le32(k[i + 4:i + 8])) & M32
        c = (c + le32(k[i + 8:i + 12])) & M32
        a, b, c = lk_mix(a, b, c)
        i += 12
    t = k[i:] + bytes(12 - (n - i))
    a = (a + le32(t[0:4])) & M32
    b = (b + le32(t[4:8])) & M32
    c = (c + (le32(t[8:12]) << 8)) & M32  # the low byte of c is the length's
    a, b, c = lk_final(a, b, c)
    return (b << 32) | c


def pg_hash_u32(x, seed):
    """PostgreSQL's hash_bytes_uint32_extended of a 32-bit value."""
    return pg_hash_bytes((x & M32).to_bytes(4, "little"), seed)


def pg_hash_int(v, seed):
    """The hash of an integer of any width: that of the int64 of its value,
    its high half folded in (the hashint8 of PostgreSQL)."""
    lo = v & M32
    hi = (v >> 32) & M32
    lo ^= hi if v >= 0 else (~hi & M32)
    return pg_hash_u32(lo, seed)


def gen_hashes():
    """The vectors of every hash: PostgreSQL's algorithm, on each type's
    bytes."""
    seeds = [0, 1, 2 ** 32, 123456789012345, 2 ** 64 - 1]
    for n in list(range(0, 40)) + [64, 100]:
        for _ in range(2):
            k = bytes(random.getrandbits(8) for _ in range(n))
            sd = random.choice(seeds)
            emit("hash.bytes", (hx(k), sd), pg_hash_bytes(k, sd))
            emit("bytea.hash", (hx(k), sd), pg_hash_bytes(k, sd))
    for x in [0, 1, 2, 255, 2 ** 31, 2 ** 32 - 1] + [random.getrandbits(32) for _ in range(30)]:
        sd = random.choice(seeds)
        emit("hash.u32", (x, sd), pg_hash_u32(x, sd))
    for bits in (16, 32, 64):
        for v in int_values(bits):
            sd = random.choice(seeds)
            emit("i%d.hash" % bits, (v, sd), pg_hash_int(v, sd))
    for sd in seeds:
        emit("bool.hash", (0, sd), pg_hash_u32(0, sd))
        emit("bool.hash", (1, sd), pg_hash_u32(1, sd))
    for _ in range(40):
        h = "%032x" % random.getrandbits(128)
        sd = random.choice(seeds)
        emit("uuid.hash", (h, sd), pg_hash_bytes(bytes.fromhex(h), sd))
    for _ in range(120):
        v, sc = gen_value(D128, 36)
        sd = random.choice(seeds)
        # the key is of the shortest form: no zeros at the end past the
        # point; zero has scale 0
        v1, s1 = v, sc
        while s1 > 0 and v1 % 10 == 0:
            v1 //= 10
            s1 -= 1
        if v1 == 0:
            s1 = 0
        key = (v1 % (1 << 128)).to_bytes(16, "little") + s1.to_bytes(2, "little") + bytes(14)
        emit("d128.hash", (v, sc, sd), pg_hash_bytes(key, sd))
        if sc < 36:
            # the same value at one more place hashes alike
            emit("d128.hash", (v * 10 if abs(v * 10) <= D128 else v, sc + 1 if abs(v * 10) <= D128 else sc, sd),
                 pg_hash_bytes(key, sd))


B64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"


def ref_encode(enc, b):
    """ENCODE of the bytes b, from the rules of ty_bytea.h."""
    if enc == "hex":
        return b.hex().encode()
    if enc == "escape":
        out = bytearray()
        for c in b:
            if c == 0 or c >= 0x80:
                out += b"\\%03o" % c
            elif c == 0x5c:
                out += b"\\\\"
            else:
                out.append(c)
        return bytes(out)
    # base64: a line feed after each 76 characters of whole groups; the
    # last partial group, padded, follows with none
    full, rest = divmod(len(b), 3)
    out = ""
    for g in range(full):
        x = int.from_bytes(b[3 * g:3 * g + 3], "big")
        out += "".join(B64[(x >> sh) & 63] for sh in (18, 12, 6, 0))
        if len(out.replace("\n", "")) % 76 == 0 and len(out.replace("\n", "")) > 0 \
                and not out.endswith("\n"):
            out += "\n"
    if rest:
        x = int.from_bytes(b[3 * full:] + bytes(3 - rest), "big")
        out += B64[(x >> 18) & 63] + B64[(x >> 12) & 63]
        out += B64[(x >> 6) & 63] if rest == 2 else "="
        out += "="
    return out.encode()


def ref_decode(enc, t):
    """DECODE of the text t (bytes): the data, or None for a fault."""
    if enc == "hex":
        i, out = 0, bytearray()
        while i < len(t):
            if t[i] in b" \n\t\r":
                i += 1
                continue
            h = chr(t[i]) if t[i] < 128 else "?"
            if h not in "0123456789abcdefABCDEF":
                return None
            i += 1
            if i >= len(t):
                return None
            l = chr(t[i]) if t[i] < 128 else "?"
            if l not in "0123456789abcdefABCDEF":
                return None
            i += 1
            out.append(int(h + l, 16))
        return bytes(out)
    if enc == "escape":
        i, out = 0, bytearray()
        while i < len(t):
            if t[i] != 0x5c:
                out.append(t[i])
                i += 1
            elif i + 3 < len(t) and 0x30 <= t[i + 1] <= 0x33 and \
                    0x30 <= t[i + 2] <= 0x37 and 0x30 <= t[i + 3] <= 0x37:
                out.append(int(bytes(t[i + 1:i + 4]), 8))
                i += 4
            elif i + 1 < len(t) and t[i + 1] == 0x5c:
                out.append(0x5c)
                i += 2
            else:
                return None
        return bytes(out)
    # base64
    out, buf, pos, end = bytearray(), 0, 0, 0
    for c in t:
        if c in b" \t\n\r":
            continue
        if c == 0x3d:
            if not end:
                if pos == 2:
                    end = 1
                elif pos == 3:
                    end = 2
                else:
                    return None
            b = 0
        else:
            if not 0 < c < 127 or chr(c) not in B64:
                return None
            b = B64.index(chr(c))
        buf = ((buf << 6) + b) & 0xFFFFFF
        pos += 1
        if pos == 4:
            out.append((buf >> 16) & 255)
            if end == 0 or end > 1:
                out.append((buf >> 8) & 255)
            if end == 0 or end > 2:
                out.append(buf & 255)
            buf, pos = 0, 0
    return None if pos else bytes(out)


def gen_encodings():
    """The vectors of encode and decode, for hex, base64 and escape."""
    for enc in ("hex", "base64", "escape"):
        for _ in range(120):
            b = rand_bytes(150) if random.random() < .5 else bytes(
                random.getrandbits(8) for _ in range(random.choice([0, 1, 2, 3, 56, 57, 58, 114, 115, 171])))
            t = ref_encode(enc, b)
            emit("bytea.enc", (enc, hx(b)), hx(t))
            assert ref_decode(enc, t) == b
            emit("bytea.dec", (enc, hx(t)), hx(b))
            # a fault, or a harmless change, in the text
            m = bytearray(t)
            kind = random.randrange(5)
            if m and kind == 0:
                m[random.randrange(len(m))] = random.choice(b"=\\ \n\tz/+-\x80\xc3")
            elif m and kind == 1:
                del m[random.randrange(len(m))]
            elif kind == 2:
                m.insert(random.randrange(len(m) + 1), random.choice(b" \n=\\0"))
            elif m and kind == 3:
                m = m[:random.randrange(len(m))]
            else:
                m += random.choice([b"=", b"==", b" ", b"\\", b"\\1", b"AA"])
            r = ref_decode(enc, bytes(m))
            emit("bytea.dec", (enc, hx(bytes(m))), ("E", "INVALID") if r is None else hx(r))


def ref_parse_int(t, bits):
    """The integer of a text of bytes t, as ty_int.h says: a port of the
    rules, a digit at a time. Returns an int, "RANGE" or "INVALID"."""
    mag = 1 << (bits - 1)
    sp = b" \t\n\v\f\r"
    i = 0
    while i < len(t) and t[i] in sp:
        i += 1
    neg = False
    if i < len(t) and t[i] == 0x2d:
        neg = True
        i += 1
    elif i < len(t) and t[i] == 0x2b:
        i += 1
    base = 10
    pre = t[i:i + 2].lower()
    if pre == b"0x":
        base, i = 16, i + 2
    elif pre == b"0o":
        base, i = 8, i + 2
    elif pre == b"0b":
        base, i = 2, i + 2
    first = i
    tmp = 0

    def dig(k):
        if k >= len(t):
            return None
        d = "0123456789abcdef".find(chr(t[k]).lower()) if t[k] < 128 else -1
        return d if 0 <= d < base else None

    while True:
        d = dig(i)
        if d is not None:
            if tmp > mag // base:
                return "RANGE"
            tmp = tmp * base + d
            i += 1
        elif i < len(t) and t[i] == 0x5f:
            if base == 10 and i == first:
                return "INVALID"
            i += 1
            if dig(i) is None:
                return "INVALID"
        else:
            break
    if i == first:
        return "INVALID"
    while i < len(t) and t[i] in sp:
        i += 1
    if i != len(t):
        return "INVALID"
    if neg:
        return "RANGE" if tmp > mag else -tmp
    return "RANGE" if tmp > mag - 1 else tmp


def gen_int_text():
    """The vectors of the text of the integers: read, in many forms, and
    write."""
    for bits in (16, 32, 64):
        lo, hi = -(1 << (bits - 1)), (1 << (bits - 1)) - 1
        texts = [b"", b"0", b"-0", b"+0", b"-", b"+", b" ", b"x", b"0x", b"0b",
                 b"0o", b"1_000", b"_1", b"1_", b"1__0", b"0x_1", b"0x1_",
                 b"0b1_0", b"0o7_7", b"0_1", b"1 2", b" 1", b"1 ", b"\t-5\n",
                 b"--1", b"+-1", b"1e3", b"1.5", b"0X1F", b"0O17", b"0B101",
                 b"-0x8000", b"0x7fff", b"0xFFFF", b"0x8000", b"-0b1000_0000",
                 b"\xc3\xa9", b"1\xc3\xa9", str(hi).encode(), str(lo).encode(),
                 str(hi + 1).encode(), str(lo - 1).encode(),
                 str(hi * 10).encode(), (str(hi) + "x").encode(),
                 ("9" * 30 + "x").encode(), ("9" * 30).encode()]
        for _ in range(120):
            v = random.randint(lo, hi) if random.random() < .5 else random.choice(
                [random.randint(-1000, 1000), lo, hi, lo + 1, hi - 1])
            form = random.randrange(7)
            if form == 0:
                t = str(v)
            elif form == 1:
                t = "%+d" % v
            elif form == 2:
                t = " \t" + str(v) + "\n "
            elif form == 3:
                d = str(abs(v))
                d = "_".join([d[i:i + 3] for i in range(0, len(d), 3)])
                t = ("-" if v < 0 else "") + d
            elif form == 4:
                t = ("-" if v < 0 else "") + random.choice(["0x%X", "0x%x", "0X%x"]) % abs(v)
            elif form == 5:
                t = ("-" if v < 0 else "") + random.choice(["0o%o", "0O%o"]) % abs(v)
            else:
                t = ("-" if v < 0 else "") + "0b" + bin(abs(v))[2:]
            if random.random() < .15:
                k = random.randrange(len(t) + 1)
                t = t[:k] + random.choice("_ x-+9.") + t[k:]
            texts.append(t.encode())
        for t in texts:
            r = ref_parse_int(t, bits)
            emit("i%d.parse" % bits, (hx(t),), ("E", r) if isinstance(r, str) else r)
        for v in int_values(bits):
            emit("i%d.fmt" % bits, (v,), str(v))
    for v in [0, 1, 9, 10, 99, 100, 12345, 2 ** 31, 2 ** 32 - 1]:
        emit("u32.fmt", (v,), str(v))
    for v in [0, 1, 99, 10 ** 10 - 1, 10 ** 10, 10 ** 18, 2 ** 63, 2 ** 64 - 1]:
        emit("u64.fmt", (v,), str(v))
    for _ in range(100):
        emit("u64.fmt", (random.getrandbits(random.randint(1, 64)),), None) if False else None


import struct

INF = float("inf")
NAN = float("nan")


def f32(x):
    """x rounded to a float32 (as a Python float)."""
    if x != x or x in (INF, -INF):
        return x
    try:
        return struct.unpack("f", struct.pack("f", x))[0]
    except OverflowError:
        return INF if x > 0 else -INF


def ftok(x):
    """A float as a token; repr round-trips, and the specials are named."""
    if x != x:
        return "nan"
    if x == INF:
        return "inf"
    if x == -INF:
        return "-inf"
    return repr(x)


def fres(x):
    return ftok(x)


def zsign(r, a):
    """r, with the sign of a if r is zero: rint, ceil and the like keep it."""
    return math.copysign(0.0, a) if r == 0 else r


def float_values(bits):
    """Edge and random floats of the width."""
    cv = f32 if bits == 32 else (lambda v: v)
    hi = 3.4028234663852886e38 if bits == 32 else 1.7976931348623157e308
    tiny = 1.1754943508222875e-38 if bits == 32 else 2.2250738585072014e-308
    den = 1.401298464324817e-45 if bits == 32 else 5e-324
    vals = [0.0, -0.0, 1.0, -1.0, 0.5, 1.5, 2.5, -2.5, 3.5, 0.1, 10.0, 1e3,
            hi, -hi, tiny, -tiny, den, -den, INF, -INF, NAN, 2.0 ** 31,
            -2.0 ** 31, 2.0 ** 63, -2.0 ** 63, 32767.5, -32768.5, 123456.789]
    for _ in range(25):
        e = random.randint(-30, 30)
        vals.append(cv(random.uniform(-1, 1) * 10.0 ** e))
    for _ in range(8):
        vals.append(cv(random.choice([-1, 1]) * random.uniform(1, 9) * 10.0 ** random.randint(-300 if bits == 64 else -37, 300 if bits == 64 else 37)))
    return [cv(v) for v in vals]


def f_check(bits, r, a, b, op):
    """The rules of ty_float.h for a result r of a op b: a value, or an error
    name."""
    zero = 0.0
    if r in (INF, -INF) and a not in (INF, -INF) and (b is None or b not in (INF, -INF)):
        return ("E", "RANGE")
    if op == "mul" and r == 0 and a != 0 and b != 0:
        return ("E", "UNDERFLOW")
    if op == "div" and r == 0 and a != 0 and b not in (INF, -INF):
        return ("E", "UNDERFLOW")
    return r


def ref_arith(bits, op, a, b):
    cv = f32 if bits == 32 else (lambda v: v)
    try:
        if op == "add":
            r = a + b
        elif op == "sub":
            r = a - b
        elif op == "mul":
            r = a * b
        else:
            if b == 0 and a == a:
                return ("E", "DIVZERO")
            r = a / b if b != 0 else NAN
    except OverflowError:
        r = INF
    r = cv(r)
    if op in ("add", "sub"):
        if r in (INF, -INF) and a not in (INF, -INF) and b not in (INF, -INF):
            return ("E", "RANGE")
        return r
    if op == "mul":
        return f_check(bits, r, a, b, "mul")
    if r in (INF, -INF) and a not in (INF, -INF):
        return ("E", "RANGE")
    if r == 0 and a != 0 and b not in (INF, -INF):
        return ("E", "UNDERFLOW")
    return r


def fnan_cmp(a, b):
    """-1, 0, 1 with every NaN equal and last."""
    an, bn = a != a, b != b
    if an and bn:
        return 0
    if an:
        return 1
    if bn:
        return -1
    return (a > b) - (a < b)


def ref_hash_float(a, seed):
    if a == 0:
        return seed
    if a != a:
        a = struct.unpack("<d", bytes.fromhex("000000000000f87f"))[0]
    return pg_hash_bytes(struct.pack("<d", a), seed)


def gen_floats():
    """The vectors of float32 and float64: arithmetic, compare, conversion,
    rounding, powers, logs, width_bucket and the hash."""
    import math
    for bits in (32, 64):
        p = "f%d" % bits
        cv = f32 if bits == 32 else (lambda v: v)
        vals = float_values(bits)
        pairs = [(a, b) for a in vals for b in vals]
        random.shuffle(pairs)
        for a, b in pairs[:500]:
            for op in ("add", "sub", "mul", "div"):
                r = ref_arith(bits, op, a, b)
                emit("%s.%s" % (p, op), (ftok(a), ftok(b)), ("E", r[1]) if isinstance(r, tuple) else ftok(r))
            emit(p + ".cmp", (ftok(a), ftok(b)), fnan_cmp(a, b))
            c = fnan_cmp(a, b)
            for nm, v in (("eq", c == 0), ("ne", c != 0), ("lt", c < 0),
                          ("le", c <= 0), ("gt", c > 0), ("ge", c >= 0)):
                emit(p + "." + nm, (ftok(a), ftok(b)), int(v))
            emit(p + ".larger", (ftok(a), ftok(b)), ftok(a if fnan_cmp(a, b) > 0 else b))
            emit(p + ".smaller", (ftok(a), ftok(b)), ftok(a if fnan_cmp(a, b) < 0 else b))
        for a in vals:
            emit(p + ".neg", (ftok(a),), ftok(-a))
            emit(p + ".abs", (ftok(a),), ftok(abs(a)))
            sd = random.choice([0, 1, 2 ** 40, 2 ** 64 - 1])
            emit(p + ".hash", (ftok(a), sd), ref_hash_float(a, sd))
            for bi in (16, 32, 64):
                if a != a or a in (INF, -INF):
                    res = ("E", "RANGE")
                else:
                    n = round(a)
                    res = n if -(1 << (bi - 1)) <= n < (1 << (bi - 1)) else ("E", "RANGE")
                emit("%s.toint%d" % (p, bi), (ftok(a),), res)
    # float64 to float32 and back
    for a in float_values(64):
        if a != a or a in (INF, -INF):
            r = a
        else:
            r = f32(a)
            if r in (INF, -INF):
                emit("f64.to32", (ftok(a),), ("E", "RANGE"))
                continue
            if r == 0 and a != 0:
                emit("f64.to32", (ftok(a),), ("E", "UNDERFLOW"))
                continue
        emit("f64.to32", (ftok(a),), ftok(r))
    # rounding, powers, logs
    for a in float_values(64):
        if a == a and a not in (INF, -INF):
            emit("f64.round", (ftok(a),), ftok(zsign(float(round(a)), a)) if abs(a) < 2 ** 52 else ftok(a))
            emit("f64.ceil", (ftok(a),), ftok(zsign(float(math.ceil(a)), a)) if abs(a) < 2 ** 52 else ftok(a))
            emit("f64.floor", (ftok(a),), ftok(zsign(float(math.floor(a)), a)) if abs(a) < 2 ** 52 else ftok(a))
            emit("f64.trunc", (ftok(a),), ftok(zsign(float(math.trunc(a)), a)) if abs(a) < 2 ** 52 else ftok(a))
        emit("f64.sign", (ftok(a),), ftok(1.0 if a > 0 else -1.0 if a < 0 else 0.0))
        # sqrt
        if a < 0:
            r = ("E", "INVALID")
        elif a != a:
            r = NAN
        else:
            r = math.sqrt(a) if a != INF else INF
        emit("f64.sqrt", (ftok(a),), ("E", r[1]) if isinstance(r, tuple) else ftok(r))
        if a == a:
            c = math.copysign(abs(a) ** (1.0 / 3.0), a) if a not in (INF, -INF) else a
        # ln / log10
        for nm, fn in (("ln", math.log), ("log10", math.log10)):
            if a == 0:
                r = ("E", "INVALID")
            elif a < 0:
                r = ("E", "INVALID")
            elif a != a:
                r = NAN
            elif a == INF:
                r = INF
            else:
                r = fn(a)
            emit("f64." + nm, (ftok(a),), ("E", r[1]) if isinstance(r, tuple) else ftok(r))
        # exp, away from the subnormal results
        if a != a:
            r = NAN
        elif a == INF:
            r = INF
        elif a == -INF:
            r = 0.0
        else:
            try:
                r = math.exp(a)
            except OverflowError:
                r = ("E", "RANGE")
            if not isinstance(r, tuple) and r == 0:
                r = ("E", "UNDERFLOW")
            if not isinstance(r, tuple) and 0 < r < 2.2250738585072014e-308:
                r = None
        if r is not None:
            emit("f64.exp", (ftok(a),), ("E", r[1]) if isinstance(r, tuple) else ftok(r))
    base = float_values(64)
    for a in base[:30]:
        for b in [0.0, 1.0, -1.0, 2.0, 0.5, -2.0, 3.0, 10.0, -0.5, INF, -INF, NAN, 1e3, -1e3, 700.0]:
            if a != a:
                r = NAN if (b != b or b != 0.0) else 1.0
            elif b != b:
                r = NAN if a != 1.0 else 1.0
            elif a == 0 and b < 0:
                r = ("E", "INVALID")
            elif a < 0 and b not in (INF, -INF) and math.floor(b) != b:
                r = ("E", "INVALID")
            elif b in (INF, -INF) or a in (INF, -INF):
                try:
                    r = math.pow(a, b)
                except (OverflowError, ValueError):
                    r = None
            else:
                try:
                    r = math.pow(a, b)
                except OverflowError:
                    r = ("E", "RANGE")
                except ValueError:
                    r = None
                if r is not None and not isinstance(r, tuple):
                    if r in (INF, -INF):
                        r = ("E", "RANGE")
                    elif r == 0 and a != 0:
                        r = ("E", "UNDERFLOW")
                    elif 0 < abs(r) < 2.2250738585072014e-308:
                        r = None
            if r is not None:
                emit("f64.pow", (ftok(a), ftok(b)), ("E", r[1]) if isinstance(r, tuple) else ftok(r))
    # width_bucket
    for _ in range(300):
        b1 = random.choice([0.0, -10.0, 5.0, 1e308, -1e308, 100.0, 0.5])
        b2 = random.choice([10.0, 0.0, -5.0, 1e308, -1e308, 0.5, 100.0, INF, NAN])
        op = random.choice([-1e309 if False else -INF, INF, NAN, 0.0, 5.0, 7.5, -3.0, 1e308, 99.9, 0.25])
        cnt = random.choice([1, 2, 5, 10, 2 ** 31 - 1, 0, -1])
        if cnt <= 0:
            r = ("E", "INVALID")
        elif op != op or b1 != b1 or b2 != b2:
            r = ("E", "INVALID")
        elif b1 in (INF, -INF) or b2 in (INF, -INF):
            r = ("E", "INVALID")
        elif b1 == b2:
            r = ("E", "INVALID")
        else:
            lo, hi, asc = (b1, b2, True) if b1 < b2 else (b2, b1, False)
            below = op < b1 if asc else op > b1
            beyond = op >= b2 if asc else op <= b2
            if below:
                r = 0
            elif beyond:
                r = ("E", "RANGE") if cnt == 2 ** 31 - 1 else cnt + 1
            else:
                # the same double arithmetic as the C, step for step
                if asc:
                    num, den = op - b1, b2 - b1
                    if den in (INF, -INF):
                        num, den = op / 2 - b1 / 2, b2 / 2 - b1 / 2
                else:
                    num, den = b1 - op, b1 - b2
                    if den in (INF, -INF):
                        num, den = b1 / 2 - op / 2, b1 / 2 - b2 / 2
                if op in (INF, -INF):
                    r = None
                else:
                    r = int(cnt * (num / den))
                    r = min(r, cnt - 1) + 1
        if r is not None:
            emit("f64.wb", (ftok(op), ftok(b1), ftok(b2), cnt), r)


def ref_parse_float(t, bits):
    """The float of a text of bytes t, as ty_float.h says: a float, or
    "RANGE" or "INVALID"."""
    import re
    from decimal import Decimal
    sp = b" \t\n\v\f\r"
    i = 0
    while i < len(t) and t[i] in sp:
        i += 1
    if i == len(t):
        return "INVALID"
    rest = t[i:]
    m = re.match(rb"[+-]?(?:\d+\.?\d*|\.\d+)(?:[eE][+-]?\d+)?", rest)
    cv = f32 if bits == 32 else (lambda v: v)
    if m:
        txt = m.group(0).decode()
        val = cv(float(txt)) if bits == 64 else None
        if bits == 32:
            try:
                val = f32(float(txt))
            except OverflowError:
                val = INF
        end = m.end()
        if val in (INF, -INF) or (val == 0 and Decimal(txt) != 0):
            return "RANGE"
    else:
        m = re.match(rb"([+-]?)(?:(infinity|inf)|nan(?:\([A-Za-z0-9_]*\))?)", rest, re.I)
        if not m:
            return "INVALID"
        if m.group(2):
            val = -INF if m.group(1) == b"-" else INF
        else:
            val = NAN
        end = m.end()
    tail = rest[end:]
    j = 0
    while j < len(tail) and tail[j] in sp:
        j += 1
    if j != len(tail):
        return "INVALID"
    return val


def gen_float_parse():
    """The vectors of float text read, in many forms."""
    texts = [b"", b" ", b"x", b"1", b"-1", b"+1", b"1.", b".5", b"-.5", b"1e5",
             b"1E+5", b"1e-5", b"1e", b"1e+", b".", b"-", b"+", b"1.5.5",
             b"1 2", b" 1 ", b"\t1\n", b"0e0", b"0.0000e5", b"00012", b"1_0",
             b"NaN", b"nan", b"NAN", b"-nan", b"nan(123)", b"nan(", b"Infinity",
             b"-Infinity", b"+Infinity", b"inf", b"-inf", b"+inf", b"INF",
             b"infin", b"infinity ", b"in", b"1e308", b"1e309", b"-1e309",
             b"1e-323", b"1e-324", b"1e-400", b"0.1", b"3.4028235e38",
             b"3.4028236e38", b"3.5e38", b"1e-45", b"1e-46", b"1e-50",
             b"1.7976931348623157e308", b"1.7976931348623159e308",
             b"123456789012345678901234567890", b"0.000000000000000000001",
             b"1,5", b"1e5x", b"1ex", b"\xc3\xa9", b"1\xc3\xa9"]
    for _ in range(150):
        mant = "%d.%d" % (random.randrange(10 ** random.randint(1, 18)), random.randrange(10 ** random.randint(1, 18)))
        e = random.choice(["", "e%d" % random.randint(-330, 310), "E+%d" % random.randint(0, 40)])
        t = random.choice(["", "-", "+"]) + mant + e
        if random.random() < .2:
            t = " " + t + "\t"
        if random.random() < .1:
            k = random.randrange(len(t) + 1)
            t = t[:k] + random.choice("x_,.e-+ ") + t[k:]
        texts.append(t.encode())
    for bits in (32, 64):
        for t in texts:
            r = ref_parse_float(t, bits)
            if isinstance(r, str):
                res = ("E", r)
            else:
                res = ftok(r)
            emit("f%d.parse" % bits, (hx(t),), res)


RPD = 0.0174532925199432957692


def _deg_consts():
    sin_30 = math.sin(30.0 * RPD)
    omc60 = 1.0 - math.cos(60.0 * RPD)
    asin05 = math.asin(0.5)
    acos05 = math.acos(0.5)
    atan1 = math.atan(1.0)

    def sind0_30(x):
        return (math.sin(x * RPD) / sin_30) / 2.0

    def cosd0_60(x):
        return 1.0 - ((1.0 - math.cos(x * RPD)) / omc60) / 2.0

    def sq1(x):
        return sind0_30(x) if x <= 30.0 else cosd0_60(90.0 - x)

    def cq1(x):
        return cosd0_60(x) if x <= 60.0 else sind0_30(90.0 - x)

    tan45 = sq1(45.0) / cq1(45.0)
    cot45 = cq1(45.0) / sq1(45.0)
    return sin_30, omc60, asin05, acos05, atan1, sq1, cq1, tan45, cot45


def _fold(x, neg0, neg180, neg90):
    sign = 1
    x = math.fmod(x, 360.0)
    if x < 0.0:
        x = -x
        sign = -sign if neg0 else sign
    if x > 180.0:
        x = 360.0 - x
        sign = -sign if neg180 else sign
    if x > 90.0:
        x = 180.0 - x
        sign = -sign if neg90 else sign
    return x, sign


def fdiv(a, b):
    """a / b as C does it: an infinity or NaN for a zero divisor."""
    if b == 0:
        if a == 0 or a != a:
            return NAN
        return math.copysign(INF, a) * math.copysign(1.0, b)
    return a / b


def ref_unary(name, a):
    """The result of the float64 function `name` at a: a float, or an error
    name; None if the case is not a rule (a subnormal result of the libm)."""
    OOR = ("E", "INVALID")
    RNG = ("E", "RANGE")
    sin30, omc60, asin05, acos05, atan1, sq1, cq1, tan45, cot45 = _deg_consts()
    if a != a and name not in ("sinh", "cosh", "tanh", "asinh", "acosh", "atanh", "erf", "erfc", "lgamma"):
        return NAN
    inf = a in (INF, -INF)

    def fin(v):
        return RNG if v in (INF, -INF) else v

    if name in ("acos", "asin", "acosd", "asind"):
        if a < -1 or a > 1:
            return OOR
        if name == "acos":
            return fin(math.acos(a))
        if name == "asin":
            return fin(math.asin(a))
        def asd(x):
            return (math.asin(x) / asin05) * 30.0 if x <= 0.5 else 90.0 - (math.acos(x) / acos05) * 60.0
        def acd(x):
            return 90.0 - (math.asin(x) / asin05) * 30.0 if x <= 0.5 else (math.acos(x) / acos05) * 60.0
        if name == "acosd":
            return fin(acd(a) if a >= 0 else 90.0 + asd(-a))
        return fin(asd(a) if a >= 0 else -asd(-a))
    if name == "atan":
        return fin(math.atan(a))
    if name == "atand":
        return fin((math.atan(a) / atan1) * 45.0)
    if name in ("cos", "sin", "tan", "cot"):
        if inf:
            return OOR
        if name == "cos":
            return fin(math.cos(a))
        if name == "sin":
            return fin(math.sin(a))
        if name == "tan":
            return math.tan(a)
        t = math.tan(a)
        return INF if t == 0 and math.copysign(1, t) > 0 else (-INF if t == 0 else 1.0 / t)
    if name in ("cosd", "sind", "tand", "cotd"):
        if inf:
            return OOR
        if name == "cosd":
            x, sg = _fold(a, False, False, True)
            return fin(sg * cq1(x))
        if name == "sind":
            x, sg = _fold(a, True, True, False)
            return fin(sg * sq1(x))
        x, sg = _fold(a, True, True, True)
        if name == "tand":
            v = sg * (fdiv(fdiv(sq1(x), cq1(x)), tan45))
        else:
            v = sg * (fdiv(fdiv(cq1(x), sq1(x)), cot45))
        return 0.0 if v == 0 else v
    if name in ("sinh", "cosh", "tanh", "asinh", "acosh", "atanh", "erf", "erfc"):
        if a != a:
            return NAN
        try:
            if name == "sinh":
                return math.sinh(a)
            if name == "cosh":
                v = math.cosh(a)
                return ("E", "UNDERFLOW") if v == 0 else v
            if name == "tanh":
                return math.tanh(a)
            if name == "asinh":
                return math.asinh(a)
            if name == "acosh":
                return OOR if a < 1.0 else math.acosh(a)
            if name == "atanh":
                if a < -1 or a > 1:
                    return OOR
                return -INF if a == -1 else INF if a == 1 else math.atanh(a)
            if name == "erf":
                return math.erf(a)
            return math.erfc(a)
        except OverflowError:
            return (INF if a > 0 else -INF) if name == "sinh" else INF if name == "cosh" else None
    if name == "gamma":
        if a == INF:
            return INF
        if a == -INF:
            return RNG
        # Python's gamma is not the libm's, so only what is exact in both:
        # a positive integer is a factorial, and the poles are errors.
        if a == int(a) and 1 <= a <= 20:
            return float(math.factorial(int(a) - 1))
        if a == int(a) and a <= 0:
            return RNG
        if a > 171.7:
            return RNG
        return None
    if name == "lgamma":
        if a != a:
            return NAN
        if a in (INF, -INF):
            return INF
        # as for gamma: only the exact and the errors
        if a in (1.0, 2.0):
            return 0.0
        if a == int(a) and a <= 0:
            return RNG
        if a > 2.5e305:
            return RNG
        return None
    raise Exception(name)


def gen_trig():
    """The vectors of the trigonometric, hyperbolic and special functions."""
    base = float_values(64) + [30.0, 45.0, 60.0, 90.0, 120.0, 135.0, 150.0, 180.0, 210.0, 270.0,
                               300.0, 315.0, 330.0, 360.0, 720.5, -30.0, -45.0, -90.0,
                               -180.0, 0.5, -0.5, 0.25, 0.75, 1.0000000000000002, 3.0, 0.1,
                               -1.5, 5.5, 20.0, 170.0, 171.5, 1e5, 710.0, 711.0, -711.0,
                               -2.0, -3.0, -4.5, 100.0, 200.0, 1e-8, 1e10]
    names = ["acos", "asin", "atan", "cos", "sin", "tan", "cot", "acosd", "asind",
             "atand", "cosd", "sind", "tand", "cotd", "sinh", "cosh", "tanh",
             "asinh", "acosh", "atanh", "erf", "erfc", "gamma", "lgamma"]
    for nm in names:
        for a in base:
            r = ref_unary(nm, a)
            if r is None:
                continue
            emit("f64." + nm, (ftok(a),), ("E", r[1]) if isinstance(r, tuple) else ftok(r))
    # atan2 and atan2d
    pts = [0.0, -0.0, 1.0, -1.0, 2.0, -3.5, INF, -INF, NAN, 1e308, 1e-300]
    atan1 = math.atan(1.0)
    for y in pts:
        for x in pts:
            if y != y or x != x:
                r1 = r2 = NAN
            else:
                r1 = math.atan2(y, x)
                r2 = (r1 / atan1) * 45.0
            emit("f64.atan2", (ftok(y), ftok(x)), ftok(r1))
            emit("f64.atan2d", (ftok(y), ftok(x)), ftok(r2))
    # degrees and radians: the checks of div and mul
    for a in base:
        r = ref_arith(64, "div", a, RPD)
        emit("f64.degrees", (ftok(a),), ("E", r[1]) if isinstance(r, tuple) else ftok(r))
        r = ref_arith(64, "mul", a, RPD)
        emit("f64.radians", (ftok(a),), ("E", r[1]) if isinstance(r, tuple) else ftok(r))


def isinf_(x):
    return x in (INF, -INF)


def ref_var_accum(st, x):
    """Youngs-Cramer step; st = [n, sx, sxx]. Returns the new state or None
    for an overflow."""
    n0, sx0, sxx0 = st
    n = n0 + 1.0
    sx = sx0 + x
    sxx = sxx0
    if n0 > 0.0:
        tmp = x * n - sx
        try:
            sxx = sxx0 + tmp * tmp / (n * n0)
        except OverflowError:
            sxx = INF
        if isinf_(sx) or isinf_(sxx):
            if not isinf_(sx0) and not isinf_(x):
                return None
            sxx = NAN
    elif x != x or isinf_(x):
        sxx = NAN
    return [n, sx, sxx]


def safe(f):
    """Evaluate f() with C's IEEE results for overflow and zero division."""
    try:
        return f()
    except OverflowError:
        return INF
    except ZeroDivisionError:
        return NAN


def ref_var_final(name, st):
    n, sx, sxx = st
    if name == "avg":
        return None if n == 0 else fdiv(sx, n)
    if name == "pop":
        return None if n == 0 else fdiv(sxx, n)
    if name == "samp":
        return None if n <= 1 else fdiv(sxx, n - 1.0)
    if name == "stddevpop":
        if n == 0:
            return None
        v = fdiv(sxx, n)
        return NAN if v != v or v < 0 else math.sqrt(v) if v != INF else INF
    if name == "stddevsamp":
        if n <= 1:
            return None
        v = fdiv(sxx, n - 1.0)
        return NAN if v != v or v < 0 else math.sqrt(v) if v != INF else INF


def stok(st):
    return " ".join(ftok(v) for v in st)


def gen_float_aggs():
    """The vectors of the statistical aggregates: states built by a run of
    values, combined, and the final functions."""
    sets = []
    for _ in range(40):
        k = random.choice([0, 1, 2, 3, 5, 10])
        pool = random.choice([
            lambda: random.uniform(-100, 100),
            lambda: float(random.randint(-5, 5)),
            lambda: random.choice([1e308, -1e308, 1.0, 2.0, 0.5]),
            lambda: random.choice([INF, -INF, NAN, 1.0, 2.0, 0.0]) if random.random() < .3 else random.uniform(-1, 1),
        ])
        sets.append([pool() for _ in range(k)])
    for xs in sets:
        st = [0.0, 0.0, 0.0]
        ok = True
        for x in xs:
            st = ref_var_accum(st, x)
            if st is None:
                ok = False
                break
        emit("f64.vaccum", ((" ".join(ftok(x) for x in xs)) or "-",),
             stok(st) if ok else ("E", "RANGE"))
        if ok:
            for nm in ("avg", "pop", "samp", "stddevpop", "stddevsamp"):
                r = ref_var_final(nm, st)
                emit("f64.vfinal", (nm, stok(st)), "NULL" if r is None else ftok(r))
    states = []
    for xs in sets:
        st = [0.0, 0.0, 0.0]
        good = True
        for x in xs:
            st = ref_var_accum(st, x)
            if st is None:
                good = False
                break
        if good:
            states.append(st)
    for _ in range(80):
        a, b = random.choice(states), random.choice(states)
        if a[0] == 0:
            r = list(b)
        elif b[0] == 0:
            r = list(a)
        else:
            n = a[0] + b[0]
            sx = a[1] + b[1]
            if isinf_(sx) and not isinf_(a[1]) and not isinf_(b[1]):
                emit("f64.vcombine", (stok(a), stok(b)), ("E", "RANGE"))
                continue
            tmp = fdiv(a[1], a[0]) - fdiv(b[1], b[0])
            sxx = a[2] + b[2] + safe(lambda: a[0] * b[0] * tmp * tmp / n)
            if isinf_(sxx) and not isinf_(a[2]) and not isinf_(b[2]):
                emit("f64.vcombine", (stok(a), stok(b)), ("E", "RANGE"))
                continue
            r = [n, sx, sxx]
        emit("f64.vcombine", (stok(a), stok(b)), stok(r))
    # regression: a run of pairs, then the finals
    for _ in range(40):
        k = random.choice([0, 1, 2, 3, 6, 10])
        pairs = [(random.uniform(-50, 50), random.uniform(-50, 50)) if random.random() < .6
                 else (float(random.randint(-3, 3)), float(random.randint(-3, 3))) for _ in range(k)]
        if random.random() < .1:
            pairs = [(1.0, 2.0)] * k
        st = [0.0] * 6
        for (y, x) in pairs:
            n0, sx0, sxx0, sy0, syy0, sxy0 = st
            n = n0 + 1.0
            sx = sx0 + x
            sy = sy0 + y
            if n0 > 0.0:
                tx = x * n - sx
                ty = y * n - sy
                sc = 1.0 / (n * n0)
                sxx = sxx0 + tx * tx * sc
                syy = syy0 + ty * ty * sc
                sxy = sxy0 + tx * ty * sc
            else:
                sxx, syy, sxy = sxx0, syy0, sxy0
            st = [n, sx, sxx, sy, syy, sxy]
        emit("f64.raccum", (" ".join("%s %s" % (ftok(y), ftok(x)) for y, x in pairs) or "-",), stok(st))
        n, sx, sxx, sy, syy, sxy = st
        def fin(name):
            if name in ("sxx", "syy", "sxy", "avgx", "avgy", "covarpop", "slope", "intercept", "r2", "corr") and n < 1:
                return None
            if name == "sxx": return sxx
            if name == "syy": return syy
            if name == "sxy": return sxy
            if name == "avgx": return sx / n
            if name == "avgy": return sy / n
            if name == "covarpop": return sxy / n
            if name == "covarsamp": return None if n < 2 else sxy / (n - 1.0)
            if name == "corr":
                if sxx == 0 or syy == 0: return None
                return sxy / math.sqrt(sxx * syy)
            if name == "r2":
                if sxx == 0: return None
                if syy == 0: return 1.0
                return (sxy * sxy) / (sxx * syy)
            if name == "slope":
                return None if sxx == 0 else sxy / sxx
            if name == "intercept":
                return None if sxx == 0 else (sy - sx * sxy / sxx) / n
        for nm in ("sxx", "syy", "sxy", "avgx", "avgy", "covarpop", "covarsamp", "corr", "r2", "slope", "intercept"):
            r = fin(nm)
            emit("f64.rfinal", (nm, stok(st)), "NULL" if r is None else ftok(r))


def btok(s):
    """A bit string as a token: its 0s and 1s, or - if empty."""
    return s if s else "-"


def rand_bits(maxn=40):
    """A random bit string, of a length that is, now and then, a multiple
    of 8, and with edge strings of all zeros and all ones."""
    n = random.choice([0, 1, 2, 3, 7, 8, 9, 15, 16, 17, 24, random.randint(0, maxn), random.randint(0, maxn)])
    kind = random.random()
    if kind < 0.1:
        return "0" * n
    if kind < 0.2:
        return "1" * n
    return "".join(random.choice("01") for _ in range(n))


def gen_bits():
    """The vectors of bit strings, from a reference of Python strings of
    0 and 1."""
    big = 2 ** 31 - 1
    # text in
    for t in ["", "b", "B", "x", "X", "101", "b101", "B0110", "x1F", "XaB", "x", "xg", "b102",
              "0", "1", "b0", "xFFFF", "x0", "bb", "xx", "b 1", "1 0", "x1 ", "x\xc3\xa9", "b\xc3\xa9",
              "10101010101010101", "x123456789abcdef"]:
        if t.startswith(("b", "B")) or not t.startswith(("x", "X")):
            body = t[1:] if t.startswith(("b", "B")) else t
            ok = all(c in "01" for c in body)
            r = body if ok else None
        else:
            body = t[1:]
            ok = all(c in "0123456789abcdefABCDEF" for c in body) and t.isascii()
            r = "".join("{:04b}".format(int(c, 16)) for c in body) if ok else None
        raw = t.encode("latin-1") if t.isascii() else t.encode("utf-8", "surrogateescape")
        emit("bits.parse", (hx(raw),), ("E", "INVALID") if r is None else btok(r))
    for _ in range(150):
        a, b = rand_bits(), rand_bits()
        emit("bits.cmp", (btok(a), btok(b)), (a > b) - (a < b))
        emit("bits.eq", (btok(a), btok(b)), int(a == b))
        emit("bits.cat", (btok(a), btok(b)), btok(a + b))
        emit("bits.position", (btok(a), btok(b)), (a.find(b) + 1) if a and len(b) <= len(a) else 0)
        for op, fn in (("and", lambda x, y: "".join(str(int(p) & int(q)) for p, q in zip(x, y))),
                       ("or", lambda x, y: "".join(str(int(p) | int(q)) for p, q in zip(x, y))),
                       ("xor", lambda x, y: "".join(str(int(p) ^ int(q)) for p, q in zip(x, y)))):
            emit("bits." + op, (btok(a), btok(b)), ("E", "INVALID") if len(a) != len(b) else btok(fn(a, b)))
        emit("bits.overlay", (btok(a), btok(b), 0, 0), ("E", "INVALID"))
        for sp in (1, 2, 3, 8, 9, len(a) + 1, len(a) + 5):
            for sl in (0, 1, 3, 8, big, -2):
                if sp + sl > big:
                    res = ("E", "RANGE")
                else:
                    res = btok(a[:sp - 1] + b + a[max(sp + sl, 1) - 1:])
                emit("bits.overlay", (btok(a), btok(b), sp, sl), res)
    for _ in range(100):
        a = rand_bits(70)
        emit("bits.count", (btok(a),), a.count("1"))
        emit("bits.not", (btok(a),), btok("".join("1" if c == "0" else "0" for c in a)))
        for n in (0, 1, 3, 7, 8, 9, 16, len(a), len(a) + 3, -1, -5, -9, -len(a)):
            sl = (a[n:] + "0" * min(n, len(a))) if n >= 0 else ("0" * min(-n, len(a)) + a[:max(len(a) + n, 0)])
            emit("bits.shl", (btok(a), n), btok(sl if n >= 0 else sl))
            sr = ("0" * min(n, len(a)) + a[:max(len(a) - n, 0)]) if n >= 0 else (a[-n:] + "0" * min(-n, len(a)))
            emit("bits.shr", (btok(a), n), btok(sr))
        for st in (-3, 0, 1, 2, 5, 8, 9, len(a), len(a) + 1):
            for ln in (None, 0, 1, 4, 9, 100, big, -1):
                s1 = max(st, 1)
                if ln is None:
                    e1 = len(a) + 1
                elif ln < 0:
                    emit("bits.substr", (btok(a), st, ln, 1), ("E", "INVALID"))
                    continue
                else:
                    e = st + ln
                    e1 = len(a) + 1 if e > big else min(e, len(a) + 1)
                res = "" if (s1 > len(a) or e1 <= s1) else a[s1 - 1:e1 - 1]
                emit("bits.substr", (btok(a), st, ln if ln is not None else 0, 0 if ln is None else 1), btok(res))
        for i in (-1, 0, 1, len(a) - 1, len(a), 5):
            emit("bits.get", (btok(a), i), a[i] if 0 <= i < len(a) else ("E", "RANGE"))
            for bit in (0, 1, 2, -1):
                if not 0 <= i < len(a):
                    res = ("E", "RANGE")
                elif bit not in (0, 1):
                    res = ("E", "INVALID")
                else:
                    res = btok(a[:i] + str(bit) + a[i + 1:])
                emit("bits.set", (btok(a), i, bit), res)
        for ln in (0, 1, 5, 8, 33, 70):
            emit("bits.resize", (btok(a), ln), btok((a + "0" * ln)[:ln]) if ln <= len(a) or True else None)
        if len(a) <= 32:
            emit("bits.toint32", (btok(a),), (lambda v: v - 2 ** 32 if v >= 2 ** 31 else v)(int(a, 2) if a else 0))
        else:
            emit("bits.toint32", (btok(a),), ("E", "RANGE"))
        if len(a) <= 64:
            emit("bits.toint64", (btok(a),), (lambda v: v - 2 ** 64 if v >= 2 ** 63 else v)(int(a, 2) if a else 0))
        else:
            emit("bits.toint64", (btok(a),), ("E", "RANGE"))
    for _ in range(60):
        v = random.choice(int_values(32))
        ln = random.choice([-2, 0, 1, 3, 8, 16, 31, 32, 33, 40, 70])
        n = 1 if ln <= 0 else ln
        emit("bits.fromint32", (v, ln), btok(format(v % (1 << n), "0%db" % n)))
        v = random.choice(int_values(64))
        ln = random.choice([-2, 0, 1, 3, 8, 32, 63, 64, 65, 100])
        n = 1 if ln <= 0 else ln
        emit("bits.fromint64", (v, ln), btok(format(v % (1 << n), "0%db" % n)))


def itok(fam, bits, addr):
    """An inet value as a token: family (4 or 6), mask length, address hex."""
    n = 8 if fam == 4 else 32
    return "%d:%d:%0*x" % (fam, bits, n, addr)


def ip_mask(fam, bits):
    """The mask of the first bits of an address of family fam."""
    mb = 32 if fam == 4 else 128
    return ((1 << bits) - 1) << (mb - bits)


def rand_inets():
    """Pairs of inet values that share prefixes: the cases that matter are
    the ones where the networks nearly agree."""
    out = []
    for fam in (4, 6):
        mb = 32 if fam == 4 else 128
        for _ in range(120):
            base = random.getrandbits(mb)
            if random.random() < 0.3:
                base = random.choice([0, (1 << mb) - 1, 1 << (mb - 1), 1])
            ba = random.choice([0, mb, 8, 16, mb - 1, 1, random.randint(0, mb), random.randint(0, mb)])
            other = base
            if random.random() < 0.7:
                flip = random.randint(0, mb - 1)
                other = base ^ (1 << flip)
            bb = random.choice([ba, ba, random.randint(0, mb), 0, mb, ba - 1 if ba > 0 else 0, ba + 1 if ba < mb else mb])
            out.append(((fam, ba, base), (fam, bb, other)))
    for _ in range(20):
        out.append(((4, random.randint(0, 32), random.getrandbits(32)), (6, random.randint(0, 128), random.getrandbits(128))))
    return out


def gen_inet():
    """The vectors of inet and cidr, from Python integers: an address is an
    int, a mask is a prefix of ones."""
    def prefix(x, bits):
        fam, b, a = x
        mb = 32 if fam == 4 else 128
        return a >> (mb - bits) if bits else 0

    for x, y in rand_inets():
        fx, bx, ax = x
        fy, by, ay = y
        t, u = itok(*x), itok(*y)
        mbx = 32 if fx == 4 else 128
        if fx == fy:
            m = min(bx, by)
            px, py = prefix(x, m), prefix(y, m)
            if px != py:
                c = (px > py) - (px < py)
            elif bx != by:
                c = (bx > by) - (bx < by)
            else:
                c = (ax > ay) - (ax < ay)
        else:
            c = (fx > fy) - (fx < fy)
        emit("inet.cmp", (t, u), c)
        same = fx == fy
        emit("inet.sub", (t, u), int(same and bx > by and prefix(x, by) == prefix(y, by)))
        emit("inet.subeq", (t, u), int(same and bx >= by and prefix(x, by) == prefix(y, by)))
        emit("inet.sup", (t, u), int(same and bx < by and prefix(x, bx) == prefix(y, bx)))
        emit("inet.supeq", (t, u), int(same and bx <= by and prefix(x, bx) == prefix(y, bx)))
        emit("inet.overlap", (t, u), int(same and prefix(x, min(bx, by)) == prefix(y, min(bx, by))))
        full = (1 << mbx) - 1
        mask = ip_mask(fx, bx)
        emit("inet.broadcast", (t,), itok(fx, bx, ax | (full ^ mask)))
        emit("inet.network", (t,), itok(fx, bx, ax & mask))
        emit("inet.netmask", (t,), itok(fx, mbx, mask))
        emit("inet.hostmask", (t,), itok(fx, mbx, full ^ mask))
        emit("inet.not", (t,), itok(fx, bx, ax ^ full))
        if same:
            m = min(bx, by)
            common = 0
            while common < m and ((ax >> (mbx - 1 - common)) & 1) == ((ay >> (mbx - 1 - common)) & 1):
                common += 1
            emit("inet.merge", (t, u), itok(fx, common, ax & ip_mask(fx, common)))
            emit("inet.and", (t, u), itok(fx, max(bx, by), ax & ay))
            emit("inet.or", (t, u), itok(fx, max(bx, by), ax | ay))
            d = ax - ay
            emit("inet.subinet", (t, u), d if -2 ** 63 <= d < 2 ** 63 else ("E", "RANGE"))
        else:
            emit("inet.merge", (t, u), ("E", "INVALID"))
            emit("inet.and", (t, u), ("E", "INVALID"))
            emit("inet.or", (t, u), ("E", "INVALID"))
            emit("inet.subinet", (t, u), ("E", "INVALID"))
        for n in (0, 1, -1, 255, 256, -256, 2 ** 31, -2 ** 31, 2 ** 32, 2 ** 63 - 1, -2 ** 63, random.randint(-2 ** 40, 2 ** 40), random.randint(-2 ** 63, 2 ** 63 - 1)):
            r = ax + n
            emit("inet.add", (t, n), itok(fx, bx, r) if 0 <= r <= full else ("E", "RANGE"))
        nb = random.randint(0, mbx)
        emit("inet.cidrmask", (t, nb), itok(fx, nb, ax & ip_mask(fx, nb)))


def ttok(s):
    """A text as a token: the hex of its UTF-8 bytes, or - if empty."""
    return hx(s.encode("utf-8"))


TEXT_MAX = 0x3fffffff - 4
ALPHABET = list("abcxyz ABC,.-_/0123") + ["é", "ñ", "ß", "€", "日", "本", "😀", "𝄞", "\u0100", "\u07ff", "\u0800", "\uffff", "\U0010ffff"]


def rand_text(maxlen=12, pool=None):
    """A random text of a random number of characters; the characters are
    of 1, 2, 3 and 4 bytes, and the strings repeat a few of them so that
    searches find things."""
    pool = pool or random.sample(ALPHABET, random.randint(2, 6))
    return "".join(random.choice(pool) for _ in range(random.choice([0, 0, 1, 2, 3, random.randint(0, maxlen), random.randint(0, maxlen)])))


def gen_text():
    """The vectors of text, from a reference of Python strings, whose
    characters are code points, as UTF-8 text's are."""
    big = 2 ** 31 - 1
    for _ in range(260):
        pool = random.sample(ALPHABET, random.randint(2, 5))
        a, b, c = rand_text(14, pool), rand_text(4, pool), rand_text(4, pool)
        ta, tb, tc = ttok(a), ttok(b), ttok(c)
        emit("text.length", (ta,), len(a))
        ab, bb = a.encode(), b.encode()
        emit("text.cmp", (ta, tb), (ab > bb) - (ab < bb))
        emit("text.starts_with", (ta, tb), int(ab.startswith(bb)))
        emit("text.position", (ta, tb), (a.find(b) + 1) if b else 1)
        emit("text.reverse", (ta,), ttok(a[::-1]))
        emit("text.replace", (ta, tb, tc), ttok(a.replace(b, c)) if a and b else ta)
        for n in (0, 1, 2, 3, -1, -2, 5, 20, -20, big, -big - 1):
            emit("text.left", (ta, n), ttok(a[:n] if n >= 0 else a[:max(len(a) + n, 0)]))
            emit("text.right", (ta, n), ttok((a[len(a) - n:] if 0 < n < len(a) else (a if n >= len(a) else "")) if n >= 0 else a[min(-n, len(a)):] if n != -big - 1 else ""))
        for st in (-2, 0, 1, 2, 3, 5, 9, len(a), len(a) + 1, big, -big):
            for ln in (None, 0, 1, 2, 4, 9, 100, big, -1):
                if ln is None:
                    emit("text.substr", (ta, st, 0, 0), ttok(a[max(st, 1) - 1:]))
                elif ln < 0:
                    emit("text.substr", (ta, st, ln, 1), ("E", "INVALID"))
                else:
                    e = st + ln
                    if e > big:
                        res = a[max(st, 1) - 1:]
                    elif e <= 1:
                        res = ""
                    else:
                        res = a[max(st, 1) - 1:e - 1]
                    emit("text.substr", (ta, st, ln, 1), ttok(res))
        for sp in (-1, 0, 1, 2, 4, len(a) + 1, len(a) + 3):
            for sl in (0, 1, 3, big, -2):
                if sp <= 0:
                    res = ("E", "INVALID")
                elif sp + sl > big:
                    res = ("E", "RANGE")
                else:
                    res = ttok(a[:sp - 1] + b + a[max(sp + sl, 1) - 1:])
                emit("text.overlay", (ta, tb, sp, sl), res)
        for n in (0, 1, 2, 3, -1, -2, 7):
            if n == 0:
                emit("text.split_part", (ta, tb, 0), ("E", "INVALID"))
                continue
            if not a:
                res = ""
            elif not b:
                res = a if n in (1, -1) else ""
            else:
                parts = a.split(b)
                if n > 0:
                    res = parts[n - 1] if n <= len(parts) else ""
                else:
                    res = parts[n] if -n <= len(parts) else ""
            emit("text.split_part", (ta, tb, n), ttok(res))
        for n in (0, 1, 2, 3, -1):
            emit("text.repeat", (ta, n), ttok(a * max(n, 0)))
        emit("text.repeat", (ta, big), ("E", "RANGE") if a else "-")
        for n in (-1, 0, 1, 2, 5, 9, 20):
            t = a[:max(n, 0)]
            eff = max(n, 0) if b else len(t)
            fill = (b * (eff // len(b) + 1)) if b else ""
            padn = eff - len(t)
            pad = ""
            if b:
                pad = "".join(b[i % len(b)] for i in range(padn))
            emit("text.lpad", (ta, n, tb), ttok(pad + t))
            emit("text.rpad", (ta, n, tb), ttok(t + pad))
        emit("text.lpad", (ta, big, tb), ("E", "RANGE") if b else ttok(a))
        emit("text.rpad", (ta, 300000000, tb), ("E", "RANGE") if b else ttok(a))
        for mode, l, r in (("b", 1, 1), ("l", 1, 0), ("r", 0, 1)):
            res = a
            if a and b:
                if l:
                    i = 0
                    while i < len(res) and res[i] in b:
                        i += 1
                    res = res[i:]
                if r:
                    j = len(res)
                    while j > 0 and res[j - 1] in b:
                        j -= 1
                    res = res[:j]
            emit("text.trim", (ta, tb, mode), ttok(res))
        out_chars = []
        for ch in a:
            i = b.find(ch)
            if i < 0:
                out_chars.append(ch)
            elif i < len(c):
                out_chars.append(c[i])
        emit("text.translate", (ta, tb, tc), ttok("".join(out_chars)))
        emit("text.ascii", (ta,), ord(a[0]) if a else 0)
    # character(n), character varying(n), name
    for _ in range(200):
        pool = random.sample(ALPHABET, random.randint(2, 5)) + [" ", " "]
        a = rand_text(14, pool)
        ta = ttok(a)
        emit("bpchar.truelen", (ta,), len(a.encode().rstrip(b" ")))
        for n in (0, 1, 2, 3, 5, 9, 20):
            emit("text.cliplen", (ta, n), len(a[:n].encode()))
            emit("text.clipbytes", (ta, n), max([len(a[:i].encode()) for i in range(len(a) + 1) if len(a[:i].encode()) <= n]))
            for force in (0, 1):
                for pad in (0, 1):
                    if len(a) > n:
                        kept = a[:n]
                        if not force and a[n:].strip(" ") != "":
                            res = ("E", "RANGE")
                        else:
                            k = len(kept.encode())
                            res = (k, k)
                    else:
                        k = len(a.encode())
                        res = (k, k + (n - len(a) if pad else 0))
                    emit("bpchar.fit", (ta, n, force, pad), res)
        b = rand_text(14, pool)
        tb = ttok(b)
        x, y = a.encode().rstrip(b" "), b.encode().rstrip(b" ")
        emit("bpchar.cmp", (ta, tb), (x > y) - (x < y))
        raw = a.encode()
        cut = max([len(a[:i].encode()) for i in range(len(a) + 1) if len(a[:i].encode()) <= 63])
        emit("name.parse", (ta,), hx(raw[:cut]) if len(raw) >= 64 else ta)
    for _ in range(20):
        a = "".join(random.choice(["a", "é", "日", "😀"]) for _ in range(random.randint(30, 80)))
        raw = a.encode()
        cut = max([len(a[:i].encode()) for i in range(len(a) + 1) if len(a[:i].encode()) <= 63])
        emit("name.parse", (ttok(a),), hx(raw[:cut]) if len(raw) >= 64 else ttok(a))
    # the case of ASCII letters
    for _ in range(120):
        t = "".join(random.choice(["a", "B", "z", "Z", "0", "9", " ", "-", "_", "é", "日", "😀", "'", "x1", "AbC", "dEf"]) for _ in range(random.randint(0, 12)))
        tt = ttok(t)
        emit("text.lower", (tt,), ttok("".join(chr(ord(c) + 32) if "A" <= c <= "Z" else c for c in t)))
        emit("text.upper", (tt,), ttok("".join(chr(ord(c) - 32) if "a" <= c <= "z" else c for c in t)))
        res = ""
        was = False
        for c in t:
            c = c.lower() if was and c.isascii() else (c.upper() if c.isascii() else c)
            if c.isascii() and (c.isalpha() or c.isdigit()):
                was = True
            else:
                was = False
            res += c
        emit("text.initcap", (tt,), ttok(res))
    for cp in [-1, 0, 1, 65, 127, 128, 233, 2047, 2048, 8364, 55295, 55296, 57343, 57344, 65535, 65536, 128512, 1114111, 1114112, 2 ** 31 - 1] + [random.randint(1, 1114111) for _ in range(60)]:
        if cp < 0:
            res = ("E", "INVALID")
        elif cp == 0 or cp > 1114111 or 55296 <= cp <= 57343:
            res = ("E", "RANGE")
        else:
            res = ttok(chr(cp))
        emit("text.chr", (cp,), res)


def gen_like():
    """LIKE and the escape translation, from a reference of Python regular
    expressions (a pattern with a backslash at its end is in known.txt: the
    error depends on where the match gets to)."""
    import re
    chars = ["a", "b", "é", "日", "😀"]
    for _ in range(600):
        toks = []
        for _ in range(random.randint(0, 6)):
            k = random.random()
            if k < 0.35:
                toks.append(("lit", random.choice(chars)))
            elif k < 0.6:
                toks.append(("any", "%"))
            elif k < 0.8:
                toks.append(("one", "_"))
            else:
                toks.append(("esc", random.choice(chars + ["%", "_", "\\"])))
        pat = ""
        rx = ""
        for kind, v in toks:
            if kind == "lit":
                pat += v
                rx += re.escape(v)
            elif kind == "any":
                pat += "%"
                rx += ".*"
            elif kind == "one":
                pat += "_"
                rx += "."
            else:
                pat += "\\" + v
                rx += re.escape(v)
        text = "".join(random.choice(chars + chars[:2]) for _ in range(random.randint(0, 8)))
        emit("text.like", (ttok(text), ttok(pat)), int(re.fullmatch(rx, text, re.DOTALL) is not None))
    # the escape translation
    for _ in range(200):
        pool = ["a", "b", "\\", "#", "é", "%", "_", "#"]
        pat = "".join(random.choice(pool) for _ in range(random.randint(0, 8)))
        esc = random.choice(["", "#", "\\", "é", "ab", "x"])
        if len(esc) > 1:
            emit("text.like_escape", (ttok(pat), ttok(esc)), ("E", "INVALID"))
            continue
        if esc == "":
            res = pat.replace("\\", "\\\\")
        elif esc == "\\":
            res = pat
        else:
            res = ""
            after = False
            for ch in pat:
                if ch == esc and not after:
                    res += "\\"
                    after = True
                elif ch == "\\":
                    res += "\\" if after else "\\\\"
                    after = False
                else:
                    res += ch
                    after = False
        emit("text.like_escape", (ttok(pat), ttok(esc)), ttok(res))


I64MIN, I64MAX = -2 ** 63, 2 ** 63 - 1


def money_values():
    out = [0, 1, -1, 99, 100, -100, 101, 999, 1000, 100000, 123456789, I64MAX, I64MIN, I64MAX - 1, I64MIN + 1,
           12345678901234567, 100000000000000000, 100000000000000001]
    out += [random.randint(-10 ** random.randint(1, 18), 10 ** random.randint(1, 18)) for _ in range(40)]
    return out


def mnum(words, n):
    """A port of cash_words' helper for a number below 1000."""
    small = ["zero", "one", "two", "three", "four", "five", "six", "seven", "eight", "nine", "ten", "eleven", "twelve",
             "thirteen", "fourteen", "fifteen", "sixteen", "seventeen", "eighteen", "nineteen", "twenty", "thirty",
             "forty", "fifty", "sixty", "seventy", "eighty", "ninety"]
    big = small[18:]
    tu = n % 100
    if n <= 20:
        return small[n]
    if tu == 0:
        return "%s hundred" % small[n // 100]
    if n > 99:
        if n % 10 == 0 and tu > 10:
            return "%s hundred %s" % (small[n // 100], big[tu // 10])
        if tu < 20:
            return "%s hundred and %s" % (small[n // 100], small[tu])
        return "%s hundred %s %s" % (small[n // 100], big[tu // 10], small[tu % 10])
    if n % 10 == 0 and tu > 10:
        return big[tu // 10]
    if tu < 20:
        return small[tu]
    return "%s %s" % (big[tu // 10], small[tu % 10])


def money_words(v):
    s = ""
    if v < 0:
        v = -v
        s += "minus "
    dollars = v // 100
    m0 = v % 100
    for div, name in ((100000000000000000, " quadrillion "), (100000000000000, " trillion "), (100000000000, " billion "),
                      (100000000, " million "), (100000, " thousand ")):
        m = (v // div) % 1000
        if m:
            s += mnum(None, m) + name
    m1 = (v // 100) % 1000
    if m1:
        s += mnum(None, m1)
    if dollars == 0:
        s += "zero"
    s += " dollar and " if dollars == 1 else " dollars and "
    s += mnum(None, m0)
    s += " cent" if m0 == 1 else " cents"
    return s[0].upper() + s[1:]


def money_format(v, fmt):
    """A port of cash_out; fmt is a dict of the lconv fields."""
    points = fmt["frac"] if 0 <= fmt["frac"] <= 10 else 2
    grp = fmt["grp"] if 0 < fmt["grp"] <= 6 else 3
    d = fmt["dec"] if len(fmt["dec"]) == 1 else "."
    ss = fmt["thou"] if fmt["thou"] else ("," if d != "," else ".")
    cs = fmt["cur"] or "$"
    if v < 0:
        sg = fmt["neg"] or "-"
        posn, csp, sep = fmt["nposn"], fmt["ncs"], fmt["nsep"]
    else:
        sg = fmt["pos"]
        posn, csp, sep = fmt["pposn"], fmt["pcs"], fmt["psep"]
    u = abs(v)
    buf = ""
    pos = points
    while True:
        if points and pos == 0:
            buf = d + buf
        elif pos < 0 and pos % grp == 0 or (pos < 0 and (-pos) % grp == 0):
            buf = ss + buf
        buf = str(u % 10) + buf
        u //= 10
        pos -= 1
        if not (u or pos >= 0):
            break
    sp1 = " " if sep == 1 else ""
    sp2 = " " if sep == 2 else ""
    if posn == 0:
        return "(%s%s%s)" % ((cs, sp1, buf) if csp else (buf, sp1, cs))
    if posn == 2:
        return ("%s%s%s%s%s" % (cs, sp1, buf, sp2, sg)) if csp else ("%s%s%s%s%s" % (buf, sp1, cs, sp2, sg))
    if posn == 3:
        return ("%s%s%s%s%s" % (sg, sp2, cs, sp1, buf)) if csp else ("%s%s%s%s%s" % (buf, sp1, sg, sp2, cs))
    if posn == 4:
        return ("%s%s%s%s%s" % (cs, sp2, sg, sp1, buf)) if csp else ("%s%s%s%s%s" % (buf, sp1, cs, sp2, sg))
    return ("%s%s%s%s%s" % (sg, sp2, cs, sp1, buf)) if csp else ("%s%s%s%s%s" % (sg, sp2, buf, sp1, cs))


def gen_money():
    """The vectors of money: arithmetic from Python integers, the words and the
    text from a port of PostgreSQL's code, over made-up locales."""
    vals = money_values()
    for a in vals:
        emit("money.words", (a,), ttok(money_words(a)))
        for b in random.sample(vals, 4):
            r = a + b
            emit("money.add", (a, b), r if I64MIN <= r <= I64MAX else ("E", "RANGE"))
            r = a - b
            emit("money.sub", (a, b), r if I64MIN <= r <= I64MAX else ("E", "RANGE"))
            r = a * b
            emit("money.mul_i64", (a, b), r if I64MIN <= r <= I64MAX else ("E", "RANGE"))
            if b == 0:
                emit("money.div_i64", (a, b), ("E", "DIVZERO"))
                emit("money.div_money", (a, b), ("E", "DIVZERO"))
            else:
                q = abs(a) // abs(b) * (1 if (a < 0) == (b < 0) else -1)
                emit("money.div_i64", (a, b), q if I64MIN <= q <= I64MAX else ("E", "RANGE"))
        emit("money.div_i64", (a, -1), -a if a != I64MIN else ("E", "RANGE"))
    fmts = [dict(frac=2, dec=".", thou=",", cur="$", pos="", neg="-", grp=3, pcs=1, ncs=1, psep=0, nsep=0, pposn=1, nposn=1),
            dict(frac=0, dec="", thou=".", cur="€", pos="+", neg="-", grp=3, pcs=0, ncs=0, psep=1, nsep=1, pposn=2, nposn=2),
            dict(frac=3, dec=",", thou=" ", cur="kr", pos="", neg="~", grp=2, pcs=0, ncs=1, psep=2, nsep=2, pposn=3, nposn=4),
            dict(frac=2, dec=".", thou=",", cur="USD", pos="", neg="", grp=3, pcs=1, ncs=1, psep=1, nsep=2, pposn=0, nposn=0),
            dict(frac=127, dec="", thou="", cur="", pos="", neg="", grp=0, pcs=127, ncs=127, psep=127, nsep=127, pposn=127, nposn=127),
            dict(frac=4, dec="..", thou="'", cur="CHF", pos="", neg="-", grp=4, pcs=1, ncs=0, psep=1, nsep=0, pposn=4, nposn=3)]
    for fi, fmt in enumerate(fmts):
        for a in vals:
            emit("money.format", (fi, a), ttok(money_format(a, fmt)))
    for a in vals:
        for f in (0.5, 2.0, -3.0, 1.5, 0.0, 1e10, 1e300, 0.001):
            r = float(a) * f
            if r != r or r in (float("inf"), float("-inf")) or r >= 2.0 ** 63 or r < -2.0 ** 63:
                res = ("E", "RANGE")
            else:
                res = int(round(r))
            emit("money.mul_f64", (a, repr(f)), res)
            if f != 0.0:
                r = float(a) / f
                if r != r or r in (float("inf"), float("-inf")) or r >= 2.0 ** 63 or r < -2.0 ** 63:
                    res = ("E", "RANGE")
                else:
                    res = int(round(r))
                emit("money.div_f64", (a, repr(f)), res)
            else:
                emit("money.div_f64", (a, repr(f)), ("E", "DIVZERO"))


I32MIN, I32MAX = -2 ** 31, 2 ** 31 - 1


def ivtok(m, d, t):
    """An interval as a token: months, days, microseconds."""
    return "%d:%d:%d" % (m, d, t)


def iv_nobegin():
    return (I32MIN, I32MIN, I64MIN)


def iv_noend():
    return (I32MAX, I32MAX, I64MAX)


def iv_inf(v):
    return v == iv_nobegin() or v == iv_noend()


def iv_values():
    out = [(0, 0, 0), (1, 0, 0), (0, 1, 0), (0, 0, 1), (-1, 0, 0), (0, -1, 0), (0, 0, -1), (1, 1, 1), (-1, -1, -1),
           (1, -1, 1), (0, 29, 86399999999), (0, 30, 0), (0, -30, 0), (13, 45, 90000000000), (-13, -45, -90000000000),
           (0, 1, -1), (0, -1, 1), (1, 0, -1), (-1, 0, 1), (0, 0, 86400000000), (0, 0, -86400000000),
           (I32MAX, 0, 0), (I32MIN + 1, 0, 0), (0, I32MAX, 0), (0, I32MIN + 1, 0), (0, 0, I64MAX), (0, 0, I64MIN + 1),
           (I32MAX - 1, I32MAX - 1, I64MAX - 1), (I32MIN + 1, I32MIN + 1, I64MIN + 1), (0, 0, I64MAX - 1),
           iv_nobegin(), iv_noend(), (I32MAX, I32MAX, 0), (0, I32MIN, 0), (5, 0, I64MIN), (0, 5, I64MAX)]
    for _ in range(40):
        out.append((random.choice([0, random.randint(-100, 100), random.randint(I32MIN, I32MAX)]),
                    random.choice([0, random.randint(-100, 100), random.randint(I32MIN, I32MAX)]),
                    random.choice([0, random.randint(-10 ** 11, 10 ** 11), random.randint(I64MIN, I64MAX)])))
    return out


def iv_span(v):
    return v[2] + (v[0] * 30 + v[1]) * 86400000000


def iv_fit(v):
    """A result is out of range if a field passes its type, or it has
    all three at an end (that is infinity)."""
    return I32MIN <= v[0] <= I32MAX and I32MIN <= v[1] <= I32MAX and I64MIN <= v[2] <= I64MAX and not iv_inf(v)


def tsround(j):
    return round(j * 1000000.0) / 1000000.0


def iv_scale(v, f, mul):
    """The finite part of interval_mul and interval_div, from the same
    steps in Python's float arithmetic."""
    sc = (lambda x: x * f) if mul else (lambda x: x / f)
    import math
    r = scale = None
    rm = sc(float(v[0]))
    if math.isnan(rm) or not (-2147483648.0 <= rm < 2147483648.0):
        return None
    rm = int(rm)
    rd = sc(float(v[1]))
    if math.isnan(rd) or not (-2147483648.0 <= rd < 2147483648.0):
        return None
    rd = int(rd)
    mrd = tsround((sc(float(v[0])) - rm) * 30)
    secr = tsround((sc(float(v[1])) - rd + mrd - int(mrd)) * 86400.0)
    if abs(secr) >= 86400.0:
        rd += int(secr / 86400.0)
        if not (I32MIN <= rd <= I32MAX):
            return None
        secr -= int(secr / 86400.0) * 86400.0
    rd += int(mrd)
    if not (I32MIN <= rd <= I32MAX):
        return None
    rt = sc(float(v[2])) + secr * 1000000.0
    if math.isnan(rt) or math.isinf(rt):
        return None
    rt = round(rt)
    if not (-9223372036854775808 <= rt < 9223372036854775808):
        return None
    res = (rm, rd, int(rt))
    return None if iv_inf(res) else res


def gen_interval():
    """The vectors of interval: the order and the sum from Python integers,
    the scaling and the justify functions from a port of PostgreSQL's steps."""
    import math
    vals = iv_values()
    for a in vals:
        emit("iv.sign", (ivtok(*a),), (iv_span(a) > 0) - (iv_span(a) < 0))
        t = iv_span(a) & (2 ** 64 - 1)
        for b in random.sample(vals, 6):
            emit("iv.cmp", (ivtok(*a), ivtok(*b)), (iv_span(a) > iv_span(b)) - (iv_span(a) < iv_span(b)))
            # add
            if a == iv_nobegin():
                res = ("E", "RANGE") if b == iv_noend() else iv_nobegin()
            elif a == iv_noend():
                res = ("E", "RANGE") if b == iv_nobegin() else iv_noend()
            elif iv_inf(b):
                res = b
            else:
                r = (a[0] + b[0], a[1] + b[1], a[2] + b[2])
                res = r if iv_fit(r) else ("E", "RANGE")
            emit("iv.add", (ivtok(*a), ivtok(*b)), ivtok(*res) if isinstance(res, tuple) and res[0] != "E" else res)
            # sub
            if a == iv_nobegin():
                res = ("E", "RANGE") if b == iv_nobegin() else iv_nobegin()
            elif a == iv_noend():
                res = ("E", "RANGE") if b == iv_noend() else iv_noend()
            elif b == iv_nobegin():
                res = iv_noend()
            elif b == iv_noend():
                res = iv_nobegin()
            else:
                r = (a[0] - b[0], a[1] - b[1], a[2] - b[2])
                res = r if iv_fit(r) else ("E", "RANGE")
            emit("iv.sub", (ivtok(*a), ivtok(*b)), ivtok(*res) if isinstance(res, tuple) and res[0] != "E" else res)
        # neg
        if a == iv_nobegin():
            res = iv_noend()
        elif a == iv_noend():
            res = iv_nobegin()
        else:
            r = (-a[0], -a[1], -a[2])
            res = r if iv_fit(r) else ("E", "RANGE")
        emit("iv.neg", (ivtok(*a),), ivtok(*res) if res[0] != "E" else res)
        # mul and div
        for f in (0.0, 1.0, -1.0, 2.0, 0.5, -0.5, 1.5, 3.0, 0.1, 7.0, 1e10, 1e-10, 12.5, float("inf"), float("-inf"), float("nan"), -3.25):
            for op in ("mul", "div"):
                if op == "div" and f == 0.0:
                    emit("iv.div", (ivtok(*a), repr(f)), ("E", "DIVZERO"))
                    continue
                if math.isnan(f):
                    res = ("E", "RANGE")
                elif iv_inf(a):
                    if (op == "mul" and f == 0.0) or (op == "div" and math.isinf(f)):
                        res = ("E", "RANGE")
                    elif f < 0.0:
                        res = iv_noend() if a == iv_nobegin() else iv_nobegin()
                    else:
                        res = a
                elif op == "mul" and math.isinf(f):
                    sg = (iv_span(a) > 0) - (iv_span(a) < 0)
                    res = ("E", "RANGE") if sg == 0 else (iv_nobegin() if f * sg < 0 else iv_noend())
                else:
                    res = iv_scale(a, f, op == "mul")
                    if res is None:
                        res = ("E", "RANGE")
                emit("iv." + op, (ivtok(*a), repr(f)), ivtok(*res) if res[0] != "E" else res)
        # justify
        if iv_inf(a):
            for nm in ("justify_hours", "justify_days", "justify"):
                emit("iv." + nm, (ivtok(*a),), ivtok(*a))
            continue

        def tmod(t):
            q = abs(t) // 86400000000 * (1 if t >= 0 else -1)
            return q, t - q * 86400000000
        m, d, t = a
        # justify_hours
        q, t2 = tmod(t)
        d2 = d + q
        res = None
        if not (I32MIN <= d2 <= I32MAX):
            res = ("E", "RANGE")
        else:
            if d2 > 0 and t2 < 0:
                t2 += 86400000000
                d2 -= 1
            elif d2 < 0 and t2 > 0:
                t2 -= 86400000000
                d2 += 1
            res = (m, d2, t2)
        emit("iv.justify_hours", (ivtok(*a),), ivtok(*res) if res[0] != "E" else res)
        # justify_days
        wm = int(d / 30)
        d2 = d - wm * 30
        m2 = m + wm
        if not (I32MIN <= m2 <= I32MAX):
            res = ("E", "RANGE")
        else:
            if m2 > 0 and d2 < 0:
                d2 += 30
                m2 -= 1
            elif m2 < 0 and d2 > 0:
                d2 -= 30
                m2 += 1
            res = (m2, d2, t)
        emit("iv.justify_days", (ivtok(*a),), ivtok(*res) if res[0] != "E" else res)
        # justify_interval
        m2, d2, t2 = m, d, t
        bad = False
        if (d2 > 0 and t2 > 0) or (d2 < 0 and t2 < 0):
            wm = int(d2 / 30)
            d2 -= wm * 30
            m2 += wm
            if not (I32MIN <= m2 <= I32MAX):
                bad = True
        if not bad:
            q, t2 = tmod(t2)
            d2 = d2 + q
            d2 = ((d2 + 2 ** 31) % 2 ** 32) - 2 ** 31  # cut to an int32
            wm = int(d2 / 30)
            d2 -= wm * 30
            m2 += wm
            if not (I32MIN <= m2 <= I32MAX):
                bad = True
        if bad:
            res = ("E", "RANGE")
        else:
            if m2 > 0 and (d2 < 0 or (d2 == 0 and t2 < 0)):
                d2 += 30
                m2 -= 1
            elif m2 < 0 and (d2 > 0 or (d2 == 0 and t2 > 0)):
                d2 -= 30
                m2 += 1
            if d2 > 0 and t2 < 0:
                t2 += 86400000000
                d2 -= 1
            elif d2 < 0 and t2 > 0:
                t2 -= 86400000000
                d2 += 1
            res = (m2, d2, t2)
        emit("iv.justify", (ivtok(*a),), ivtok(*res) if res[0] != "E" else res)
    for a in vals:
        for seed in (0,):
            pass


def cdiv(a, b):
    """C integer division: toward zero."""
    q = abs(a) // abs(b)
    return q if (a >= 0) == (b >= 0) else -q


def cmod(a, b):
    return a - cdiv(a, b) * b


def jdn(y, m, d):
    """The Julian day of a Gregorian date, by the formula of the
    Wikipedia article (not PostgreSQL's)."""
    a = (14 - m) // 12
    yy = y + 4800 - a
    mm = m + 12 * a - 3
    return d + (153 * mm + 2) // 5 + 365 * yy + yy // 4 - yy // 100 + yy // 400 - 32045


def jdn_inv(j):
    """The date of a Julian day (Richards' algorithm)."""
    f = j + 1401 + (((4 * j + 274277) // 146097) * 3) // 4 - 38
    e = 4 * f + 3
    g = (e % 1461) // 4
    h = 5 * g + 2
    d = (h % 153) // 5 + 1
    m = ((h // 153 + 2) % 12) + 1
    y = e // 1461 - 4716 + (12 + 2 - m) // 12
    return y, m, d


EPOCH_J = 2451545
US_DAY = 86400000000
MIN_TS, END_TS = -211813488000000000, 9223371331200000000


def valid_jul(y, m):
    return (y > -4713 or (y == -4713 and m >= 11)) and (y < 5874898 or (y == 5874898 and m < 6))


def ts_to_tm(ts):
    days = ts // US_DAY
    tod = ts % US_DAY
    jd = days + EPOCH_J
    if jd < 0 or jd > 2 ** 31 - 1:
        return None
    y, m, d = jdn_inv(jd)
    return (y, m, d, tod // 3600000000, tod // 60000000 % 60, tod // 1000000 % 60, tod % 1000000)


def tm_to_ts(t):
    y, m, d, h, mi, s, f = t
    if not valid_jul(y, m):
        return None
    r = (jdn(y, m, d) - EPOCH_J) * US_DAY + ((h * 60 + mi) * 60 + s) * 1000000 + f
    if not (I64MIN <= r <= I64MAX) or not (MIN_TS <= r < END_TS):
        return None
    return r


def dim(y, m):
    return [31, 29 if (y % 4 == 0 and (y % 100 != 0 or y % 400 == 0)) else 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31][m - 1]


def ts_add(ts, iv):
    mo, dy, tm = iv
    if iv == iv_nobegin():
        return None if ts == I64MAX else I64MIN
    if iv == iv_noend():
        return None if ts == I64MIN else I64MAX
    if ts in (I64MIN, I64MAX):
        return ts
    if mo != 0:
        t = ts_to_tm(ts)
        if t is None:
            return None
        y, m, d, h, mi, s, f = t
        m2 = m + mo
        if not (I32MIN <= m2 <= I32MAX):
            return None
        m = m2
        if m > 12:
            y += cdiv(m - 1, 12)
            m = cmod(m - 1, 12) + 1
        elif m < 1:
            y += cdiv(m, 12) - 1
            m = cmod(m, 12) + 12
        if d > dim(y, m):
            d = dim(y, m)
        ts = tm_to_ts((y, m, d, h, mi, s, f))
        if ts is None:
            return None
    if dy != 0:
        t = ts_to_tm(ts)
        if t is None:
            return None
        y, m, d, h, mi, s, f = t
        j = jdn(y, m, d) + dy
        if not (I32MIN <= j <= I32MAX) or j < 0:
            return None
        y, m, d = jdn_inv(j)
        ts = tm_to_ts((y, m, d, h, mi, s, f))
        if ts is None:
            return None
    ts += tm
    if not (I64MIN <= ts <= I64MAX) or not (MIN_TS <= ts < END_TS):
        return None
    return ts


def gen_datetime():
    """The vectors of the calendar, date, time and timestamp, from Python
    integers and a second formulation of the Julian day."""
    # the calendar
    dates = [(2000, 1, 1), (1970, 1, 1), (1, 1, 1), (0, 12, 31), (0, 1, 1), (-4712, 1, 1), (-4713, 11, 24), (1900, 2, 28),
             (2000, 2, 29), (2100, 2, 28), (9999, 12, 31), (294276, 12, 31), (5874897, 12, 31), (5874898, 5, 31),
             (1582, 10, 4), (1582, 10, 15), (4, 2, 29), (100, 3, 1), (400, 2, 29)]
    for _ in range(200):
        y = random.choice([random.randint(-4712, 3000), random.randint(1900, 2100), random.randint(3000, 300000), random.randint(1, 9999)])
        m = random.randint(1, 12)
        dates.append((y, m, random.randint(1, dim(y, m))))
    for y, m, d in dates:
        j = jdn(y, m, d)
        if j < 0 or j > 2 ** 31 - 1:
            continue
        emit("cal.date2j", (y, m, d), j)
        emit("cal.j2date", (j,), "%d:%d:%d" % (y, m, d))
        emit("cal.j2day", (j,), (j + 1) % 7)
    for j in [0, 1, 2451545, 2440588, 2147483494, 2147483000, 1721426, 2299160, 2299161] + [random.randint(0, 2147483494) for _ in range(60)]:
        y, m, d = jdn_inv(j)
        emit("cal.j2date", (j,), "%d:%d:%d" % (y, m, d))
        emit("cal.j2day", (j,), (j + 1) % 7)
    # date
    for y, m, d in dates:
        for yy in (y, -y):
            if yy == 0 and y != 0:
                continue
            bc = yy < 0
            ya = -yy if bc else yy
            tyr = -(ya - 1) if bc else ya
            if ya <= 0 or not (1 <= m <= 12) or d > dim(tyr, m):
                res = ("E", "INVALID")
            elif not valid_jul(tyr, m):
                res = ("E", "RANGE")
            else:
                dn = jdn(tyr, m, d) - EPOCH_J
                res = dn if (-EPOCH_J <= dn < 2147483494 - EPOCH_J) else ("E", "RANGE")
            emit("date.make", (yy, m, d), res)
    for yy, m, d in [(2000, 13, 1), (2000, 0, 1), (2000, 1, 0), (2000, 1, 32), (2001, 2, 29), (0, 1, 1), (I32MIN, 1, 1), (-1, 2, 29), (-4, 2, 29), (-5, 2, 29), (5874898, 6, 3), (5874898, 5, 31), (5874899, 1, 1), (-4713, 11, 24), (-4713, 10, 1), (-4714, 1, 1)]:
        bc = yy < 0
        if yy == I32MIN:
            res = ("E", "INVALID")
        else:
            ya = -yy if bc else yy
            tyr = -(ya - 1) if bc else ya
            if ya <= 0 or not (1 <= m <= 12) or d < 1 or d > 31 or d > dim(tyr, m):
                res = ("E", "INVALID")
            elif not valid_jul(tyr, m):
                res = ("E", "RANGE")
            else:
                dn = jdn(tyr, m, d) - EPOCH_J
                res = dn if (-EPOCH_J <= dn < 2147483494 - EPOCH_J) else ("E", "RANGE")
        emit("date.make", (yy, m, d), res)
    dvals = [0, 1, -1, 366, -365, 8000, -2451545, 2147483494 - 2451545 - 1, 2147483494 - 2451545, I32MIN, I32MAX, 2145031948, -2451546]
    dvals += [random.randint(-2451545, 2145031948) for _ in range(20)]
    for dn in dvals:
        for n in (0, 1, -1, 30, 366, I32MAX, I32MIN, 1000000, -1000000):
            for nm, f in (("add_days", lambda a, b: a + b), ("sub_days", lambda a, b: a - b)):
                if dn in (I32MIN, I32MAX):
                    res = dn
                else:
                    r = f(dn, n)
                    res = r if (I32MIN <= r <= I32MAX and -2451545 <= r < 2147483494 - 2451545) else ("E", "RANGE")
                emit("date." + nm, (dn, n), res)
        for dm in random.sample(dvals, 3):
            if dn in (I32MIN, I32MAX) or dm in (I32MIN, I32MAX):
                res = ("E", "RANGE")
            else:
                r = (dn - dm + 2 ** 31) % 2 ** 32 - 2 ** 31
                res = r
            emit("date.diff", (dn, dm), res)
    # time
    for h, mi, sc in [(0, 0, 0.0), (23, 59, 59.999999), (24, 0, 0.0), (24, 0, 0.000001), (24, 1, 0.0), (0, 60, 0.0), (-1, 0, 0.0), (0, -1, 0.0), (0, 0, 60.0),
                      (0, 0, 60.000001), (0, 0, -0.5), (12, 30, 15.5), (12, 30, 0.0000004), (12, 30, 0.0000005), (12, 30, float("nan")), (0, 0, 1e300)]:
        if math_isnan(sc) or h < 0 or h > 24 or mi < 0 or mi >= 60:
            res = ("E", "INVALID")
        else:
            v = round(sc * 1000000.0)
            if v < 0 or v > 60000000 or ((h * 60 + mi) * 60) * 1000000 + v > US_DAY:
                res = ("E", "INVALID")
            else:
                res = ((h * 60 + mi) * 60) * 1000000 + v
        emit("time.make", (h, mi, repr(sc)), res)
    for t in [0, 1, US_DAY - 1, 12 * 3600000000, 86399999999]:
        for iv in random.sample(iv_values(), 12):
            if iv_inf(iv):
                emit("time.add_interval", (t, ivtok(*iv)), ("E", "RANGE"))
                emit("time.sub_interval", (t, ivtok(*iv)), ("E", "RANGE"))
                continue
            for nm, sgn in (("add_interval", 1), ("sub_interval", -1)):
                r = (t + sgn * iv[2] + 2 ** 63) % 2 ** 64 - 2 ** 63
                r = r - cdiv(r, US_DAY) * US_DAY
                if r < 0:
                    r += US_DAY
                emit("time." + nm, (t, ivtok(*iv)), r)
        emit("time.diff", (t, 5), ivtok(0, 0, t - 5))
    # timestamp
    tss = [0, 1, -1, US_DAY, -US_DAY, MIN_TS, MIN_TS + 1, END_TS - 1, 86399999999, -86400000001, 725803200000000, 1000000000000000000,
           -200000000000000000, I64MIN, I64MAX, END_TS, MIN_TS - 1, 3155760000000000, -3155760000000000]
    tss += [random.randint(MIN_TS, END_TS - 1) for _ in range(20)] + [random.randint(-10 ** 15, 10 ** 15) for _ in range(20)]
    for ts in tss:
        if ts in (I64MIN, I64MAX):
            continue  # infinity is never broken down
        t = ts_to_tm(ts)
        emit("ts.to_tm", (ts,), "%d:%d:%d:%d:%d:%d:%d" % t if t else ("E", "RANGE"))
        if t is not None:
            r = tm_to_ts(t)
            emit("ts.from_tm", ("%d:%d:%d:%d:%d:%d:%d" % t,), r if r is not None else ("E", "RANGE"))
    for t in [(2000, 1, 1, 0, 0, 0, 0), (294276, 12, 31, 23, 59, 59, 999999), (294277, 1, 1, 0, 0, 0, 0), (-4713, 11, 24, 0, 0, 0, 0), (-4713, 11, 23, 23, 59, 59, 0),
              (5874898, 5, 31, 0, 0, 0, 0), (1, 1, 1, 0, 0, 0, 0), (0, 1, 1, 0, 0, 0, 0)]:
        r = tm_to_ts(t)
        emit("ts.from_tm", ("%d:%d:%d:%d:%d:%d:%d" % t,), r if r is not None else ("E", "RANGE"))
    ivs = iv_values()
    for ts in random.sample(tss, 25) + [0, MIN_TS, END_TS - 1, 725803200000000, 3155760000000000, I64MIN, I64MAX]:
        for iv in random.sample(ivs, 12) + [(1, 0, 0), (-1, 0, 0), (12, 0, 0), (0, 1, 0), (1, 1, 1), (0, 0, 86400000000), (13, -40, 5)]:
            r = ts_add(ts, iv)
            emit("ts.add_interval", (ts, ivtok(*iv)), r if r is not None else ("E", "RANGE"))
            # the negation
            neg = None if iv_inf(iv) and False else iv
            if iv == iv_nobegin():
                niv = iv_noend()
            elif iv == iv_noend():
                niv = iv_nobegin()
            else:
                niv = (-iv[0], -iv[1], -iv[2])
                if not iv_fit(niv):
                    niv = None
            r = None if niv is None else ts_add(ts, niv)
            emit("ts.sub_interval", (ts, ivtok(*iv)), r if r is not None else ("E", "RANGE"))
        for ts2 in random.sample(tss, 3):
            if ts in (I64MIN, I64MAX) or ts2 in (I64MIN, I64MAX):
                if ts == I64MIN:
                    res = ("E", "RANGE") if ts2 == I64MIN else ivtok(*iv_nobegin())
                elif ts == I64MAX:
                    res = ("E", "RANGE") if ts2 == I64MAX else ivtok(*iv_noend())
                elif ts2 == I64MIN:
                    res = ivtok(*iv_noend())
                else:
                    res = ivtok(*iv_nobegin())
            else:
                dd = ts - ts2
                if not (I64MIN <= dd <= I64MAX):
                    res = ("E", "RANGE")
                else:
                    q = cdiv(dd, US_DAY)
                    t2 = dd - q * US_DAY
                    d2 = q
                    if d2 > 0 and t2 < 0:
                        t2 += US_DAY
                        d2 -= 1
                    elif d2 < 0 and t2 > 0:
                        t2 -= US_DAY
                        d2 += 1
                    res = ivtok(0, d2, t2) if I32MIN <= d2 <= I32MAX else ("E", "RANGE")
            emit("ts.diff", (ts, ts2), res)


def math_isnan(x):
    return x != x


class GeoErr(Exception):
    pass


import math as _m

GEO_POOL = [0.0, -0.0, 1.0, -1.0, 2.0, 0.5, 1e-6, -1e-6, 1.0000005, 0.9999995, 1.000002, 3e-6, 1e308, -1e308, 1e-300, -1e-300,
            float("inf"), float("-inf"), float("nan"), 1e154, 1e-160, 123.456, -78.9, 1e10, 2.5e-7]
EPS = 1.0e-06


def fh(x):
    """A float as a token: hex, or nan, inf, -inf."""
    if x != x:
        return "nan"
    if x in (float("inf"), float("-inf")):
        return "inf" if x > 0 else "-inf"
    return x.hex()


def g_add(a, b):
    r = a + b
    if _m.isinf(r) and not _m.isinf(a) and not _m.isinf(b):
        raise GeoErr("RANGE")
    return r


def g_sub(a, b):
    r = a - b
    if _m.isinf(r) and not _m.isinf(a) and not _m.isinf(b):
        raise GeoErr("RANGE")
    return r


def g_mul(a, b):
    r = a * b
    if _m.isinf(r) and not _m.isinf(a) and not _m.isinf(b):
        raise GeoErr("RANGE")
    if r == 0 and a != 0 and b != 0:
        raise GeoErr("UNDERFLOW")
    return r


def g_div(a, b):
    if b == 0 and a == a:
        raise GeoErr("DIVZERO")
    if b == 0:
        return float("nan")
    r = a / b
    if _m.isinf(r) and not _m.isinf(a):
        raise GeoErr("RANGE")
    if r == 0 and a != 0 and not _m.isinf(b):
        raise GeoErr("UNDERFLOW")
    return r


def fp_eq(a, b):
    return a == b or abs(a - b) <= EPS


def fp_lt(a, b):
    return a + EPS < b


def fp_le(a, b):
    return a <= b + EPS


def fp_gt(a, b):
    return a > b + EPS


def fp_ge(a, b):
    return a + EPS >= b


def f8eq(a, b):
    return (b != b) if a != a else (b == b and a == b)


def g_hypot(x, y):
    if _m.isinf(x) or _m.isinf(y):
        return float("inf")
    if x != x or y != y:
        return float("nan")
    x, y = abs(x), abs(y)
    if x < y:
        x, y = y, x
    if y == 0.0:
        return x
    yx = y / x
    r = x * _m.sqrt(1.0 + yx * yx)
    if _m.isinf(r):
        raise GeoErr("RANGE")
    if r == 0.0:
        raise GeoErr("UNDERFLOW")
    return r


def pt_eq(p, q):
    if any(v != v for v in (p[0], p[1], q[0], q[1])):
        return int(f8eq(p[0], q[0]) and f8eq(p[1], q[1]))
    return int(fp_eq(p[0], q[0]) and fp_eq(p[1], q[1]))


def pt_tok(p):
    return "%s:%s" % (fh(p[0]), fh(p[1]))


def bx_tok(b):
    return "%s:%s:%s:%s" % (fh(b[0][0]), fh(b[0][1]), fh(b[1][0]), fh(b[1][1]))


def geo_emit(op, args, fn):
    try:
        r = fn()
        emit(op, args, r)
    except GeoErr as e:
        emit(op, args, ("E", str(e)))


def f8gt(a, b):
    """float8 order: a NaN is above everything and equal to a NaN."""
    return int((not (b != b)) if a != a else (b == b and a > b))


def f8lt(a, b):
    return int((a == a) and (b != b or a < b))


def g_pt_add(p, q):
    return (g_add(p[0], q[0]), g_add(p[1], q[1]))


def g_pt_sub(p, q):
    return (g_sub(p[0], q[0]), g_sub(p[1], q[1]))


def g_pt_mul(p, q):
    x = g_sub(g_mul(p[0], q[0]), g_mul(p[1], q[1]))
    y = g_add(g_mul(p[0], q[1]), g_mul(p[1], q[0]))
    return (x, y)


def g_pt_div(p, q):
    d = g_add(g_mul(q[0], q[0]), g_mul(q[1], q[1]))
    x = g_div(g_add(g_mul(p[0], q[0]), g_mul(p[1], q[1])), d)
    y = g_div(g_sub(g_mul(p[1], q[0]), g_mul(p[0], q[1])), d)
    return (x, y)


def g_bx_construct(a, b):
    hx, lx = (a[0], b[0]) if f8gt(a[0], b[0]) else (b[0], a[0])
    hy, ly = (a[1], b[1]) if f8gt(a[1], b[1]) else (b[1], a[1])
    return ((hx, hy), (lx, ly))


def g_bx_ar(b):
    return g_mul(g_sub(b[0][0], b[1][0]), g_sub(b[0][1], b[1][1]))


def g_bx_cn(b):
    return (g_div(g_add(b[0][0], b[1][0]), 2.0), g_div(g_add(b[0][1], b[1][1]), 2.0))


def g_bx_ov(a, b):
    return int(fp_le(a[1][0], b[0][0]) and fp_le(b[1][0], a[0][0]) and fp_le(a[1][1], b[0][1]) and fp_le(b[1][1], a[0][1]))


def gen_geo():
    """The vectors of point and box: the fuzzy compares and the float
    rules of PostgreSQL's code, ported to Python floats."""
    import random as rd
    def rp():
        return (rd.choice(GEO_POOL), rd.choice(GEO_POOL))
    pts = [rp() for _ in range(60)] + [(1.0, 2.0), (1.0, 2.0000005), (1.0000005, 2.0), (0.0, 0.0), (1.0, 1.0)]
    for _ in range(300):
        p, q = rd.choice(pts), rd.choice(pts)
        a, b = pt_tok(p), pt_tok(q)
        emit("pt.eq", (a, b), pt_eq(p, q))
        emit("pt.left", (a, b), int(fp_lt(p[0], q[0])))
        emit("pt.right", (a, b), int(fp_gt(p[0], q[0])))
        emit("pt.above", (a, b), int(fp_gt(p[1], q[1])))
        emit("pt.below", (a, b), int(fp_lt(p[1], q[1])))
        emit("pt.vert", (a, b), int(fp_eq(p[0], q[0])))
        emit("pt.horiz", (a, b), int(fp_eq(p[1], q[1])))
        geo_emit("pt.add", (a, b), lambda: pt_tok(g_pt_add(p, q)))
        geo_emit("pt.sub", (a, b), lambda: pt_tok(g_pt_sub(p, q)))
        geo_emit("pt.mul", (a, b), lambda: pt_tok(g_pt_mul(p, q)))
        geo_emit("pt.div", (a, b), lambda: pt_tok(g_pt_div(p, q)))
        geo_emit("pt.distance", (a, b), lambda: fh(g_hypot(g_sub(p[0], q[0]), g_sub(p[1], q[1]))))

        def slope():
            if fp_eq(p[0], q[0]):
                return float("inf")
            if fp_eq(p[1], q[1]):
                return 0.0
            return g_div(g_sub(p[1], q[1]), g_sub(p[0], q[0]))

        def invslope():
            if fp_eq(p[0], q[0]):
                return 0.0
            if fp_eq(p[1], q[1]):
                return float("inf")
            return g_div(g_sub(p[0], q[0]), g_sub(q[1], p[1]))
        geo_emit("pt.slope", (a, b), lambda: fh(slope()))
        geo_emit("pt.invslope", (a, b), lambda: fh(invslope()))
        geo_emit("pt.hypot", (fh(p[0]), fh(p[1])), lambda: fh(g_hypot(p[0], p[1])))
    bxs = []
    for _ in range(80):
        p, q = rd.choice(pts), rd.choice(pts)
        bxs.append(g_bx_construct(p, q))
        bxs.append((p, q))   # corners in any order: normalize is a separate op
    bxs += [((2.0, 2.0), (0.0, 0.0)), ((1.0, 1.0), (1.0, 1.0)), ((2.0000005, 2.0), (0.0, 0.0)), ((3.0, 3.0), (2.0000005, 2.0000005))]
    for _ in range(300):
        b1, b2 = rd.choice(bxs), rd.choice(bxs)
        pt = rd.choice(pts)
        a, b, c = bx_tok(b1), bx_tok(b2), pt_tok(pt)
        emit("bx.construct", (pt_tok(b1[0]), pt_tok(b1[1])), bx_tok(g_bx_construct(b1[0], b1[1])))
        n = ((b1[0][0], b1[0][1]), (b1[1][0], b1[1][1]))
        hx, lx = (b1[0][0], b1[1][0])
        if f8lt(b1[0][0], b1[1][0]):
            hx, lx = lx, hx
        hy, ly = (b1[0][1], b1[1][1])
        if f8lt(b1[0][1], b1[1][1]):
            hy, ly = ly, hy
        emit("bx.normalize", (a,), bx_tok(((hx, hy), (lx, ly))))
        emit("bx.same", (a, b), int(pt_eq(b1[0], b2[0]) and pt_eq(b1[1], b2[1])))
        emit("bx.overlap", (a, b), g_bx_ov(b1, b2))
        emit("bx.contain", (a, b), int(fp_ge(b1[0][0], b2[0][0]) and fp_le(b1[1][0], b2[1][0]) and fp_ge(b1[0][1], b2[0][1]) and fp_le(b1[1][1], b2[1][1])))
        emit("bx.left", (a, b), int(fp_lt(b1[0][0], b2[1][0])))
        emit("bx.overleft", (a, b), int(fp_le(b1[0][0], b2[0][0])))
        emit("bx.right", (a, b), int(fp_gt(b1[1][0], b2[0][0])))
        emit("bx.overright", (a, b), int(fp_ge(b1[1][0], b2[1][0])))
        emit("bx.below", (a, b), int(fp_lt(b1[0][1], b2[1][1])))
        emit("bx.overbelow", (a, b), int(fp_le(b1[0][1], b2[0][1])))
        emit("bx.above", (a, b), int(fp_gt(b1[1][1], b2[0][1])))
        emit("bx.overabove", (a, b), int(fp_ge(b1[1][1], b2[1][1])))
        emit("bx.below_eq", (a, b), int(fp_le(b1[0][1], b2[1][1])))
        emit("bx.above_eq", (a, b), int(fp_ge(b1[1][1], b2[0][1])))
        geo_emit("bx.width", (a,), lambda: fh(g_sub(b1[0][0], b1[1][0])))
        geo_emit("bx.height", (a,), lambda: fh(g_sub(b1[0][1], b1[1][1])))
        geo_emit("bx.area", (a,), lambda: fh(g_bx_ar(b1)))
        geo_emit("bx.center", (a,), lambda: pt_tok(g_bx_cn(b1)))

        def dist():
            return fh(g_hypot(g_sub(g_bx_cn(b1)[0], g_bx_cn(b2)[0]), g_sub(g_bx_cn(b1)[1], g_bx_cn(b2)[1])))
        geo_emit("bx.distance", (a, b), dist)

        def inter():
            if not g_bx_ov(b1, b2):
                return "-"
            def fmin(x, y):
                return x if f8lt(x, y) else y
            def fmax(x, y):
                return x if f8gt(x, y) else y
            return bx_tok(((fmin(b1[0][0], b2[0][0]), fmin(b1[0][1], b2[0][1])), (fmax(b1[1][0], b2[1][0]), fmax(b1[1][1], b2[1][1]))))
        emit("bx.intersect", (a, b), inter())
        geo_emit("bx.add", (a, c), lambda: bx_tok((g_pt_add(b1[0], pt), g_pt_add(b1[1], pt))))
        geo_emit("bx.sub", (a, c), lambda: bx_tok((g_pt_sub(b1[0], pt), g_pt_sub(b1[1], pt))))
        geo_emit("bx.mul", (a, c), lambda: bx_tok(g_bx_construct(g_pt_mul(b1[0], pt), g_pt_mul(b1[1], pt))))
        geo_emit("bx.div", (a, c), lambda: bx_tok(g_bx_construct(g_pt_div(b1[0], pt), g_pt_div(b1[1], pt))))
        for nm, fn in (("lt", fp_lt), ("gt", fp_gt), ("eq", fp_eq), ("le", fp_le), ("ge", fp_ge)):
            geo_emit("bx." + nm, (a, b), lambda fn=fn: int(fn(g_bx_ar(b1), g_bx_ar(b2))))


def gen_timetz():
    """timetz: the order, the hash, and RANGE offsets of time and timetz."""
    times = [0, 1, 43200000000, 86399999999, 86400000000, 3600000000, 45296789012, 500000]
    zones = [0, 3600, -3600, 43200, -43200, 59 * 3600 + 3540, -(59 * 3600 + 3540), 18000, 1]
    seeds = [0, 1, 2 ** 32, 123456789012345, 2 ** 64 - 1]
    for _ in range(300):
        a = (random.choice(times + [random.randint(0, 86400000000)]), random.choice(zones))
        b = (random.choice(times + [random.randint(0, 86400000000)]), random.choice(zones))
        ta, tb = a[0] + a[1] * 1000000, b[0] + b[1] * 1000000
        c = (ta > tb) - (ta < tb) or (a[1] > b[1]) - (a[1] < b[1])
        emit("timetz.cmp", ("%d:%d" % a, "%d:%d" % b), c)
    for a in [(t, z) for t in times for z in zones[:5]]:
        for sd in seeds[:3]:
            h = pg_hash_int(a[0], sd) ^ pg_hash_u32(a[1], sd)
            emit("timetz.hashx", ("%d:%d" % a, sd), h)
    offs = [(0, 0, 0), (0, 0, 1), (0, 5, 3600000000), (1, 1, 86400000000), (0, 0, -1), (0, 0, I64MAX), (0, 0, I64MIN), (0, 0, 43200000000)]
    for _ in range(300):
        a = (random.choice(times), random.choice(zones))
        b = (random.choice(times), random.choice(zones))
        o = random.choice(offs)
        sub, less = random.randint(0, 1), random.randint(0, 1)
        args = ("%d:%d" % a, "%d:%d" % b, "%d:%d:%d" % o, sub, less)
        if o[2] < 0:
            emit("timetz.inrange", args, ("E", "INVALID"))
            emit("time.inrange", (a[0], b[0], "%d:%d:%d" % o, sub, less), ("E", "INVALID"))
            continue
        if sub:
            sm = b[0] - o[2]
        else:
            sm = b[0] + o[2]
        if not sub and sm > I64MAX:
            emit("timetz.inrange", args, int(bool(less)))
            emit("time.inrange", (a[0], b[0], "%d:%d:%d" % o, sub, less), int(bool(less)))
            continue
        ta = a[0] + a[1] * 1000000
        ts = sm + b[1] * 1000000
        c = (ta > ts) - (ta < ts) or (a[1] > b[1]) - (a[1] < b[1])
        emit("timetz.inrange", args, int(c <= 0 if less else c >= 0))
        emit("time.inrange", (a[0], b[0], "%d:%d:%d" % o, sub, less), int(a[0] <= sm if less else a[0] >= sm))


def overlaps_ref(a, b, gt, lt):
    """OVERLAPS by the SQL standard (None is a null end); 1, 0 or 2 (null)."""
    ts1, te1 = a
    ts2, te2 = b
    if ts1 is None:
        if te1 is None:
            return 2
        ts1, te1 = te1, None
    elif te1 is not None and gt(ts1, te1):
        ts1, te1 = te1, ts1
    if ts2 is None:
        if te2 is None:
            return 2
        ts2, te2 = te2, None
    elif te2 is not None and gt(ts2, te2):
        ts2, te2 = te2, ts2
    if gt(ts1, ts2):
        if te2 is None:
            return 2
        if lt(ts1, te2):
            return 1
        if te1 is None:
            return 2
        return 0
    if lt(ts1, ts2):
        if te1 is None:
            return 2
        if lt(ts2, te1):
            return 1
        if te2 is None:
            return 2
        return 0
    if te1 is None or te2 is None:
        return 2
    return 1


def gen_overlaps():
    """OVERLAPS of timestamps (int64) and of timetz, with null ends."""
    vals = [0, 1, 2, 3, 10, -5, 86400000000, I64MAX, I64MIN]
    for _ in range(600):
        ends = [random.choice(vals + [None, None]) for _ in range(4)]
        tok = lambda x: "N" if x is None else str(x)
        r = overlaps_ref((ends[0], ends[1]), (ends[2], ends[3]), lambda x, y: x > y, lambda x, y: x < y)
        emit("overlaps.i64", tuple(tok(x) for x in ends), r)
    tzs = [(0, 0), (43200000000, 0), (43200000000, 3600), (43200000000, -3600), (0, 43200), (86399999999, -43200), (1, 1)]
    key = lambda x: (x[0] + x[1] * 1000000, x[1])
    for _ in range(600):
        ends = [random.choice(tzs + [None]) for _ in range(4)]
        tok = lambda x: "N" if x is None else "%d:%d" % x
        r = overlaps_ref((ends[0], ends[1]), (ends[2], ends[3]), lambda x, y: key(x) > key(y), lambda x, y: key(x) < key(y))
        emit("overlaps.timetz", tuple(tok(x) for x in ends), r)


def gen_typmod():
    """The typmod of interval and timestamp, and make_interval."""
    Y, M, D, H, MI, S = 1 << 2, 1 << 1, 1 << 3, 1 << 10, 1 << 11, 1 << 12
    ranges = [0x7FFF, Y, M, Y | M, D, H, MI, S, D | H, D | H | MI, D | H | MI | S, H | MI, H | MI | S, MI | S, 3, 0]
    scales = [1000000, 100000, 10000, 1000, 100, 10, 1]
    offs = [500000, 50000, 5000, 500, 50, 5, 0]
    ivs = [v for v in iv_values()]
    for rng in ranges:
        for prec in (0, 1, 3, 6, 0xFFFF, 7, 100):
            typmod = (rng << 16) | prec
            for v in random.sample(ivs, 12):
                m, d, t = v
                if iv_inf(v):
                    emit("iv.typmod", (ivtok(*v), typmod), ivtok(*v))
                    continue
                if rng == 0x7FFF:
                    pass
                elif rng == Y:
                    m, d, t = cdiv(m, 12) * 12, 0, 0
                elif rng in (M, Y | M):
                    d, t = 0, 0
                elif rng == D:
                    t = 0
                elif rng in (H, D | H):
                    t = cdiv(t, 3600000000) * 3600000000
                elif rng in (MI, D | H | MI, H | MI):
                    t = cdiv(t, 60000000) * 60000000
                elif rng in (S, D | H | MI | S, H | MI | S, MI | S):
                    pass
                else:
                    emit("iv.typmod", (ivtok(*v), typmod), ("E", "INVALID"))
                    continue
                if prec != 0xFFFF:
                    if prec > 6:
                        emit("iv.typmod", (ivtok(*v), typmod), ("E", "INVALID"))
                        continue
                    t = t + offs[prec] if t >= 0 else t - offs[prec]
                    if not (I64MIN <= t <= I64MAX):
                        emit("iv.typmod", (ivtok(*v), typmod), ("E", "RANGE"))
                        continue
                    t = t - cmod(t, scales[prec])
                emit("iv.typmod", (ivtok(*v), typmod), ivtok(m, d, t))
    for ts in [0, 1, -1, 499999, 500000, 500001, -499999, -500000, -500001, 1234567, -1234567, 86400000000 + 123456, MIN_TS, END_TS - 1, I64MIN, I64MAX, 999999, 1500000, 2500000, -1500000, -2500000]:
        for prec in (-1, 0, 1, 2, 3, 4, 5, 6, 7, -2):
            if ts in (I64MIN, I64MAX) or prec in (-1, 6):
                emit("ts.typmod", (ts, prec), ts)
            elif prec < 0 or prec > 6:
                emit("ts.typmod", (ts, prec), ("E", "INVALID"))
            elif ts >= 0:
                emit("ts.typmod", (ts, prec), (ts + offs[prec]) // scales[prec] * scales[prec])
            else:
                emit("ts.typmod", (ts, prec), -(((-ts) + offs[prec]) // scales[prec] * scales[prec]))
    for t in [0, 1, 499999, 500000, 500001, 86399999999, 86400000000, 1234567, 999999, 1500000, 45296789012]:
        for prec in (-1, 0, 1, 2, 3, 4, 5, 6, 7, 100):
            if prec < 0 or prec > 6:
                emit("time.typmod", (t, prec), t)
            else:
                emit("time.typmod", (t, prec), (t + offs[prec]) // scales[prec] * scales[prec])
    for _ in range(200):
        a = [random.choice([0, 1, -1, 10, random.randint(-100, 100), I32MAX, I32MIN, 178956970, -178956970, 306783378]) for _ in range(6)]
        sec = random.choice([0.0, 1.5, -1.5, 59.999999, 1e-7, 5e-7, 1e12, -1e12, 1e13, 9.3e12, float("inf"), float("nan"), 0.1234567, 123456789.123456])
        years, months, weeks, days, hours, mins = a
        res = None
        if sec != sec or sec in (float("inf"), float("-inf")):
            res = ("E", "RANGE")
        else:
            mo = years * 12
            if not (I32MIN <= mo <= I32MAX):
                res = ("E", "RANGE")
            else:
                mo += months
                if not (I32MIN <= mo <= I32MAX):
                    res = ("E", "RANGE")
                else:
                    dy = weeks * 7
                    if not (I32MIN <= dy <= I32MAX) or not (I32MIN <= dy + days <= I32MAX):
                        res = ("E", "RANGE")
                    else:
                        dy += days
                        tm = hours * 3600000000 + mins * 60000000
                        try:
                            us = sec * 1000000.0
                            if _m.isinf(us):
                                res = ("E", "RANGE")
                            else:
                                us = round(us)
                                if not (-9223372036854775808 <= us < 9223372036854775808) or not (I64MIN <= tm + int(us) <= I64MAX):
                                    res = ("E", "RANGE")
                                else:
                                    r = (mo, dy, tm + int(us))
                                    res = ivtok(*r) if not iv_inf(r) else ("E", "RANGE")
                        except OverflowError:
                            res = ("E", "RANGE")
        emit("iv.make", ("%d:%d:%d:%d:%d:%d:%s" % (years, months, weeks, days, hours, mins, repr(sec)),), res)


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
            emit(p + ".cmp", (a, b), (a > b) - (a < b))
            for nm, fn in (("eq", a == b), ("ne", a != b), ("lt", a < b),
                           ("le", a <= b), ("gt", a > b), ("ge", a >= b)):
                emit(p + "." + nm, (a, b), int(fn))
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
    # The window frame of RANGE offsets, in int64.
    for _ in range(300):
        vals = int_values(64)
        val, base = random.choice(vals), random.choice(vals)
        off = random.choice(vals + [0, 5, 2 ** 62])
        for sub in (0, 1):
            for less in (0, 1):
                if off < 0:
                    res = ("E", "INVALID")
                else:
                    bound = base - off if sub else base + off
                    res = int(val <= bound) if less else int(val >= bound)
                emit("int.inrange", (val, base, off, sub, less), res)
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

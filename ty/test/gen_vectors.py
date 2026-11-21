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

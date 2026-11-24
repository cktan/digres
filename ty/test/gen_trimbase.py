#!/usr/bin/env python3
# Copyright (c) 2026 CK Tan
# SPDX-License-Identifier: MIT
#
# gen_trimbase.py -- write test/trimbase_known.txt: what digres's to_bin,
# to_oct and to_hex give for int4 and int8, and its btrim, ltrim and rtrim
# of bytea, taken from a digres built before they moved into ty:
#
#   test/gen_trimbase.py PSQL_ARGS... > test/trimbase_known.txt
#
# A line is one of:
#
#   base W V => BIN OCT HEX       to_bin, to_oct and to_hex of V, an int4
#                                 if W is 4, an int8 if W is 8
#   trim AHEX SETHEX => B L R     btrim, ltrim and rtrim of the bytea AHEX
#                                 by the bytes SETHEX; hex, - if empty

import random
import subprocess
import sys

random.seed(11)

INTS4 = [0, 1, -1, 2, 7, 8, 15, 16, 255, 256, 2147483647, -2147483648,
         123456789, -123456789] + [random.randint(-2**31, 2**31 - 1) for _ in range(60)]
INTS8 = [0, 1, -1, 2**32, 2**63 - 1, -2**63, 1234567890123, -1234567890123] + \
        [random.randint(-2**63, 2**63 - 1) for _ in range(60)]

TRIMS = [("", ""), ("", "00"), ("000102", ""), ("000102", "00"),
         ("000102", "02"), ("000102", "0002"), ("000000", "00"),
         ("0a0b0c0b0a", "0a0b"), ("ff00ff", "ff"), ("4142434241", "41"),
         ("4142434241", "4241"), ("010203", "040506")]
for _ in range(120):
    alphabet = random.sample(range(256), random.randint(1, 5))
    a = bytes(random.choice(alphabet) for _ in range(random.randint(0, 12)))
    s = bytes(random.choice(alphabet) for _ in range(random.randint(0, 3)))
    TRIMS.append((a.hex(), s.hex()))

sql = []
for v in INTS4:
    sql.append("SELECT 'base 4 %d => ' || to_bin('%d'::int4) || ' ' || to_oct('%d'::int4) "
               "|| ' ' || to_hex('%d'::int4);" % (v, v, v, v))
for v in INTS8:
    sql.append("SELECT 'base 8 %d => ' || to_bin('%d'::int8) || ' ' || to_oct('%d'::int8) "
               "|| ' ' || to_hex('%d'::int8);" % (v, v, v, v))
for a, s in TRIMS:
    h = "coalesce(nullif(encode(%s, 'hex'), ''), '-')"
    sql.append("SELECT 'trim %s %s => ' || %s || ' ' || %s || ' ' || %s;"
               % (a or "-", s or "-",
                  h % ("btrim('\\x%s'::bytea, '\\x%s'::bytea)" % (a, s)),
                  h % ("ltrim('\\x%s'::bytea, '\\x%s'::bytea)" % (a, s)),
                  h % ("rtrim('\\x%s'::bytea, '\\x%s'::bytea)" % (a, s))))

res = subprocess.run(["psql", "-X", "-A", "-t", "-q", "-v", "ON_ERROR_STOP=0"] +
                     sys.argv[1:], input="\n".join(sql), capture_output=True,
                     text=True)
sys.stdout.write(res.stdout)
if res.stderr:
    sys.stderr.write(res.stderr)

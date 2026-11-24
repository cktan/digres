#!/usr/bin/env python3
# Copyright (c) 2026 CK Tan
# SPDX-License-Identifier: MIT
#
# gen_unicode.py -- write test/unicode_known.txt: what digres's normalize,
# IS NORMALIZED, unicode_assigned and unistr give, taken from a digres built
# before they moved into ty:
#
#   test/gen_unicode.py PSQL_ARGS... > test/unicode_known.txt
#
# A line is one of:
#
#   all FORM LO HI => HASH NORMAL  of each character from LO to HI, the hash
#                                  (hashtextextended) of their normal forms
#                                  joined by commas, and how many are in it
#   assigned LO HI => N            how many of them are assigned
#   norm FORM INHEX => OUTHEX B    the normal form of a text, and whether the
#                                  text is in it, t or f; hex, - if empty
#   unistr INHEX => ok OUTHEX      unistr of a text
#   unistr INHEX => err STATE      refused, with the SQLSTATE

import random
import subprocess
import sys

random.seed(13)

FORMS = ["NFC", "NFD", "NFKC", "NFKD"]
STEP = 4096


def hexs(s):
    return s.encode().hex() or "-"


def lit(s):
    return "convert_from('\\x%s'::bytea, 'UTF8')" % s.encode().hex()


# Marks of many classes, starters that compose with them, Hangul's parts and
# syllables, and characters with long compatibility forms.
MARKS = [0x300, 0x301, 0x308, 0x30a, 0x323, 0x327, 0x328, 0x331, 0x345, 0x59a,
         0x5b0, 0x64b, 0x651, 0x93c, 0x94d, 0xe38, 0x1dce, 0x302a, 0x3099]
STARTS = [0x41, 0x61, 0x65, 0x6f, 0x73, 0xe9, 0x1e69, 0x3b1, 0x3c9, 0x915,
          0x9c7, 0x9be, 0x304b, 0x30ab, 0x1100, 0x1112, 0x1161, 0x1175, 0x11a8,
          0x11c2, 0xac00, 0xac01, 0xd7a3, 0xfb01, 0xfdfa, 0x2460, 0x1d15e,
          0x1f82, 0x212b, 0x2126, 0x10000, 0x10ffff, 0xfffd, 0x20]
TEXTS = ["", "abc", "e\u0301", "\u0301e", "q\u0307\u0323", "\u1e9b\u0323",
         "\u0344", "\ufdfa" * 3]
for _ in range(400):
    TEXTS.append("".join(chr(random.choice(MARKS if random.random() < .5 else STARTS))
                         for _ in range(random.randint(1, 10))))
for _ in range(100):
    TEXTS.append("".join(chr(random.choice([c for c in (random.randint(1, 0x2fff),
                                                        random.randint(0xf900, 0xffff))
                                            if not 0xd800 <= c < 0xe000]))
                         for _ in range(random.randint(1, 10))))

ESCAPES = ["", "abc", "\\\\", "a\\\\b", "\\", "\\\\\\", "\\0061", "\\u0061",
           "\\+000061", "\\U00000061", "\\00e9\\u00E9", "\\0000", "\\u0000",
           "\\+000000", "\\U00000000", "\\U0010FFFF", "\\U00110000",
           "\\UFFFFFFFF", "\\+110000", "\\+10ffff", "\\+FFFFFF", "\\d83d\\de00",
           "\\ud83d\\ude00", "\\+00d83d\\+00de00", "\\U0000d83d\\U0000de00",
           "\\d83d", "\\de00", "\\d83da", "\\d83d\\\\", "\\d83d\\0061",
           "\\d83d\\d83d\\de00", "\\d83d\\zzzz", "\\006", "\\u006", "\\+00006",
           "\\U0000006", "\\006g", "\\u006g", "\\x61", "\\U", "\\u", "\\+",
           "\\u0061b", "\\0061b\\0062", "\u00e9\\0301", "\\d83d\\0000",
           "\\00410042", "\\u\\0061", "\\D83D\\DE00", "\\ud83d\u00e9"]
for _ in range(150):
    ESCAPES.append("".join(random.choice(
        ["a", "\\\\", "\\0061", "\\u20ac", "\\+01F600", "\\U0001F600", "\\d83d",
         "\\de00", "\\", "u", "\\0000", "\u00e9", "\\+110000"])
        for _ in range(random.randint(1, 4))))

sql = ["CREATE TEMP TABLE c AS SELECT g, chr(g) AS s FROM generate_series(1, 1114111) g "
       "WHERE g NOT BETWEEN 55296 AND 57343;"]
for f in FORMS:
    sql.append("SELECT 'all %s ' || g / %d * %d || ' ' || g / %d * %d + %d || ' => ' || "
               "hashtextextended(string_agg(normalize(s, %s), ',' ORDER BY g), 0) || ' ' || "
               "count(*) FILTER (WHERE s IS %s NORMALIZED) FROM c GROUP BY g / %d "
               "ORDER BY g / %d;" % (f, STEP, STEP, STEP, STEP, STEP - 1, f, f, STEP, STEP))
sql.append("SELECT 'assigned ' || g / %d * %d || ' ' || g / %d * %d + %d || ' => ' || "
           "count(*) FILTER (WHERE unicode_assigned(s)) FROM c GROUP BY g / %d "
           "ORDER BY g / %d;" % (STEP, STEP, STEP, STEP, STEP - 1, STEP, STEP))
for t in TEXTS:
    for f in FORMS:
        sql.append("SELECT 'norm %s %s => ' || coalesce(nullif(encode(convert_to("
                   "normalize(%s, %s), 'UTF8'), 'hex'), ''), '-') || ' ' || "
                   "CASE WHEN %s IS %s NORMALIZED THEN 't' ELSE 'f' END;"
                   % (f, hexs(t), lit(t), f, lit(t), f))
sql.append("\\set VERBOSITY sqlstate")
for e in ESCAPES:
    sql.append("\\echo unistr %s =>" % hexs(e))
    sql.append("SELECT 'ok ' || coalesce(nullif(encode(convert_to(unistr(%s), 'UTF8'), "
               "'hex'), ''), '-');" % lit(e))

res = subprocess.run(["psql", "-X", "-A", "-t", "-q", "-v", "ON_ERROR_STOP=0"] +
                     sys.argv[1:], input="\n".join(sql), stdout=subprocess.PIPE,
                     stderr=subprocess.STDOUT, text=True)
# An unistr line is its echo and what came after it: the result or the error.
out = []
for line in res.stdout.splitlines():
    if out and out[-1].endswith("=>"):
        out[-1] += " " + (line if line.startswith("ok ") else
                          "err " + line.split("ERROR:")[1].strip())
    else:
        out.append(line)
sys.stdout.write("\n".join(out) + "\n")

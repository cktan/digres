#!/usr/bin/env python3
# Copyright (c) 2026 CK Tan
# SPDX-License-Identifier: MIT
#
# gen_tzdata.py -- write the time zones of ty, in the format of
# ty/src/tzdata.h: ty/src/tzdata.cc, the pack as C data, or the pack alone.
#
# The input is a directory of zone files that PostgreSQL's own zic made from
# the tzdata.zi of the PostgreSQL source tree; ../TZDATA.md says how, and
# what to do when PostgreSQL has newer data. Run it from the top:
#
#   ty/tools/gen_tzdata.py --zones DIR --tzdata-zi FILE > ty/src/tzdata.cc
#   ty/tools/gen_tzdata.py --zones DIR --tzdata-zi FILE --pack tz.pack
#
# The output is the same for the same input: names and zones are sorted, and
# no date or path is written. Zones that are the same bytes (a link to
# another) are written once.
#
# The pack (all numbers are 4 bytes, big-endian):
#   header, 40 bytes:  "TYTZ", format version (1), release (16 bytes, NUL
#                      padded), name count, blob count, size of the pack,
#                      checksum (FNV-1a of the pack, this field as zero)
#   names:             name count of (offset of the name, blob number),
#                      sorted by the lower-case name
#   blobs:             blob count of (offset, size) of a zone file
#   names' text:       each name, NUL ended
#   zone files:        the bytes of each
# An offset is from the start of the pack.

import argparse
import hashlib
import os
import struct
import sys


def find_zones(root):
    """The zone files of root: {name: bytes}, name as the path under root."""
    zones = {}
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames.sort()
        for fn in sorted(filenames):
            path = os.path.join(dirpath, fn)
            name = os.path.relpath(path, root).replace(os.sep, "/")
            with open(path, "rb") as f:
                data = f.read()
            if data[:4] != b"TZif":
                continue  # not a zone: skip it, as PostgreSQL's scan does
            zones[name] = data
    return zones


def version_of(tzdata_zi):
    """The tzdata release, from the first line of tzdata.zi: '# version X'."""
    with open(tzdata_zi, "r") as f:
        first = f.readline().strip()
    if not first.startswith("# version "):
        sys.exit("gen_tzdata: no '# version' line in " + tzdata_zi)
    return first[len("# version "):]


def sha256_of(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        h.update(f.read())
    return h.hexdigest()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--zones", required=True, help="the directory zic wrote")
    ap.add_argument("--tzdata-zi", required=True, help="the zic input file")
    ap.add_argument("--pack", help="write the pack to this file, not the C file")
    args = ap.parse_args()

    zones = find_zones(args.zones)
    if not zones:
        sys.exit("gen_tzdata: no zone file in " + args.zones)
    for must in ("UTC", "America/New_York", "Europe/London"):
        if must not in zones:
            sys.exit("gen_tzdata: no zone " + must + ": wrong directory?")
    version = version_of(args.tzdata_zi)

    # One blob for each distinct content, numbered by the first name in order.
    names = sorted(zones, key=lambda n: (n.lower(), n))
    for a, b in zip(names, names[1:]):
        if a.lower() == b.lower():
            sys.exit("gen_tzdata: names that differ in case only: %s, %s" % (a, b))
    blob_of = {}
    blobs = []
    for n in names:
        d = zones[n]
        if d not in blob_of:
            blob_of[d] = len(blobs)
            blobs.append(d)

    pack = build_pack(version, names, zones, blobs, blob_of)
    if args.pack:
        with open(args.pack, "wb") as f:
            f.write(pack)
        return
    write_cc(sys.stdout, version, sha256_of(args.tzdata_zi), names, blobs, pack)


def fnv1a(data):
    """FNV-1a, 32 bits: the checksum of the pack."""
    h = 0x811C9DC5
    for b in data:
        h = ((h ^ b) * 0x01000193) & 0xFFFFFFFF
    return h


def build_pack(version, names, zones, blobs, blob_of):
    """The bytes of the pack; see the top of this file."""
    u32 = lambda v: struct.pack(">I", v)
    header_size = 40
    names_off = header_size
    blobs_off = names_off + 8 * len(names)
    text_off = blobs_off + 8 * len(blobs)
    text = b""
    name_offs = []
    for n in names:
        name_offs.append(text_off + len(text))
        text += n.encode("ascii") + b"\0"
    data_off = text_off + len(text)
    # Zone files start at a multiple of 4, for a reader that wants it.
    pad = (-data_off) % 4
    data_off += pad
    blob_offs = []
    data = b""
    for b in blobs:
        blob_offs.append(data_off + len(data))
        data += b + b"\0" * ((-len(b)) % 4)
    total = data_off + len(data)
    rel = version.encode("ascii")
    if len(rel) > 16:
        sys.exit("gen_tzdata: release too long: " + version)
    body = b"".join(u32(name_offs[i]) + u32(blob_of[zones[n]]) for i, n in enumerate(names))
    body += b"".join(u32(blob_offs[i]) + u32(len(b)) for i, b in enumerate(blobs))
    body += text + b"\0" * pad + data
    hdr = b"TYTZ" + u32(1) + rel.ljust(16, b"\0") + u32(len(names)) + u32(len(blobs)) + u32(total)
    without = hdr + u32(0) + body
    assert len(without) == total
    return hdr + u32(fnv1a(without)) + body


def write_cc(out, version, sha, names, blobs, pack):
    """tzdata.cc: the pack as an array."""
    out.write("""// clang-format off: the file is as the generator wrote it
/*
 * tzdata.cc -- the time zones of ty, as data. GENERATED: do not edit.
 *
 * tzdata version: %s
 * sha256 of tzdata.zi: %s
 * zones: %d names, %d distinct zone files, %d bytes in the pack
 *
 * Made by ty/tools/gen_tzdata.py from the files PostgreSQL's zic wrote;
 * ty/TZDATA.md says how to make it again. The tz database is in the public
 * domain.
 */
#include "tzdata.h"

namespace ty_internal {

const unsigned char ty_tzpack[] = {""" % (version, sha, len(names), len(blobs), len(pack)))
    for j in range(0, len(pack), 24):
        out.write("\n    " + ",".join(str(x) for x in pack[j:j + 24]) + ",")
    out.write("""
};

const unsigned ty_tzpack_size = %d;

} // namespace ty_internal
""" % len(pack))


main()

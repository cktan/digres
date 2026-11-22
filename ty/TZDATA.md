# The time zone data of ty

ty holds the time zone database as C data, in `src/tzdata.cc`.
The code that converts between UTC and local time reads zones
from there, from memory. It never reads a file.

`src/tzdata.cc` is made by a script. Do not edit it by hand.
This note says how it is made, and how to make it again when
PostgreSQL has newer data.

## 1. Where the data comes from

The data is the `tzdata.zi` of the PostgreSQL source tree in
`ext/`, that is `ext/pg18.6/src/timezone/data/tzdata.zi`. That
file is the IANA time zone database in zic's input form. A minor
release of PostgreSQL brings a newer one.

PostgreSQL's own compiler of zones, `zic`, turns it into zone
files. `ext/` builds `zic` with the rest of PostgreSQL:
`ext/pg18.6/src/timezone/zic`. PostgreSQL installs these files
with no option, so ty makes them the same way.

The first line of `tzdata.zi` gives the release:
`# version 2026c`. The generator copies it into `tzdata.cc`, with
the sha256 of `tzdata.zi`, so a file says what it was made from.

ty follows PostgreSQL's data, not the operating system's.

digres uses the zones of ty and no others. Its own copy of the
IANA library is gone: the zone loader, the rules and the list of
zones (the view `pg_timezone_names`) are ty's. So digres and ty
always agree on a zone, and a newer tzdata reaches digres when ty
has it. The `SYSTEMTZDIR` of `pg_config.h` is no longer used.

## 2. How to make `tzdata.cc`

Run each step from the top of the repository. Files go under
`bench/build/tzdata`, not under `/tmp`.

1. Get the newer PostgreSQL source into `ext/`. Change `PG18` in
   `ext/ext.mk` to the new directory, put its tarball in
   `ext/dl/`, and run `make -C ext`. Skip this step if the
   source in `ext/` is already the one you want.
2. Read the release:
   `head -1 ext/pg18.6/src/timezone/data/tzdata.zi`.
3. Compile the zones:

       rm -rf bench/build/tzdata && mkdir -p bench/build/tzdata
       ext/pg18.6/src/timezone/zic -d bench/build/tzdata/zone \
           ext/pg18.6/src/timezone/data/tzdata.zi

4. Generate the file:

       ty/tools/gen_tzdata.py --zones bench/build/tzdata/zone \
           --tzdata-zi ext/pg18.6/src/timezone/data/tzdata.zi \
           > ty/src/tzdata.cc

5. Check that it is the same each time. Run step 4 to another
   file and compare with `cmp`. The output has no date and no
   path, so two runs must match.
6. Read the header of `ty/src/tzdata.cc`: the version, the number
   of names, the number of distinct zone files and the bytes.
   Look at `git diff --stat` too. A release that changes a few
   zones changes a few blobs.
7. Make the reference answers of the tests again, from the same
   zone files, with Python's `zoneinfo`, which does not share
   ty's code:

       ty/test/gen_tz_known.py --zones bench/build/tzdata/zone \
           > ty/test/tz_known.txt

8. Run the tests of ty: `DEBUG=1 make -C ty -j4 test`. A zone
   whose rules changed in the new release changes both files
   alike, so a failure here is a fault in ty, not in the data.
9. Commit `tzdata.cc` and `tz_known.txt` with the release in the
   message, for example "ty: tzdata 2026c to 2026d".

## 3. What the generator does

`tools/gen_tzdata.py` reads every file under the zone directory
that starts with `TZif`. Other files are skipped. The name of a
zone is its path under the directory, as in `America/New_York`.

1. It sorts the names by their lower-case form, so that a search
   is case-insensitive, as PostgreSQL's `pg_tzset` is.
2. It stops if two names differ only in case. PostgreSQL assumes
   that none does.
3. It stops if `UTC`, `America/New_York` or `Europe/London` is
   missing. That means the directory is wrong.
4. It writes each distinct zone file once. A name that links to
   another zone shares its file. For 2026c this is 598 names and
   341 files of 201,486 bytes, in a pack of 218,672 bytes.
5. It writes the table of names, each with the number of its
   file.
6. It writes the pack as `tzdata.cc`, or alone with `--pack`.

The file holds the **pack**: one array of bytes, `ty_tzpack`, and
its size, as `src/tzdata.h` says. The pack has a header (the
magic number `TYTZ`, the format version, the release, the counts,
the size and a checksum), a table of names sorted by their
lower-case form, a table of zone files, the text of the names and
the zone files. All numbers are four bytes, big-endian. The top
of `tools/gen_tzdata.py` has the exact layout. ty checks the
magic number, the version, the size, the checksum and every
offset when it first uses the pack.

`--pack FILE` writes the pack alone. A host can map such a file
and give it to ty with `ty_tz_use_data()`, before it finds its
first zone, in place of ty's own.

## 4. Notes

- The zone files have the slim format, which is zic's default.
  Each file has a rule for the years after its last transition.
  The code that reads a zone reads this form and the fat form.
- No zone with leap seconds is made: zic is not given `-L`.
- `tzdata.cc` starts with `// clang-format off`, so that `make
  format` leaves it as the generator wrote it, and step 5 holds.
- The abbreviation tables of PostgreSQL are not part of this
  data. They are the setting `timezone_abbreviations`, with the
  files in `dg/src/timezone/tznames`, and belong to the host.
- The tz database is in the public domain. `tzdata.cc` needs no
  copyright notice other than its own header.

# digres

digres is a PostgreSQL 18 database on a new storage engine. It
speaks PostgreSQL's wire protocol and runs PostgreSQL's planner
and executor, so `psql`, `pgbench` and any PostgreSQL driver
connect to it unchanged. Under the executor, everything is
new:

- **One process, a thread a session**: no fork per connection,
  and no background processes.
- **Tables organized by their primary key**, in RocksDB, an
  ordered key-value store, through sm, a storage stack that is
  a library of its own.
- **MVCC with no VACUUM to schedule**: old row versions are
  collected automatically.
- **SERIALIZABLE isolation** from sm's own concurrency control.
- **A database is one directory**, its rows, catalog and WAL
  in it.

[`DG-vs-PG.md`](DG-vs-PG.md) says how digres differs from
PostgreSQL 18, and [`PERF.md`](PERF.md) how fast it runs
against it. The repository holds digres and the libraries it
links, in four parts:

| Where | What it is |
| --- | --- |
| [`sm/`](sm/) | sm, the storage stack: a library of its own, for digres or another SQL database system. An MVCC storage manager on RocksDB, types and tuples over it, and schema and index maintenance over those, with one API, `sm/include/sm.h` |
| [`dg/`](dg/) | digres: a fork of PostgreSQL 18.6 that runs on the stack ([`DG-vs-PG.md`](DG-vs-PG.md)). It compiles as C++, is one process with a thread a session, and serves PostgreSQL's own `psql`, its tables sm's; [`dg/README.md`](dg/README.md) says what runs, and [`sm/PGFORK.md`](sm/PGFORK.md#10-digres-how-the-fork-is-to-be-made) §10 how it is being made |
| [`sq/`](sq/) | the SQL parser: PostgreSQL 18's SQL in, a tree of nodes out, as a library with one API, `sq/include/sq.h`. It is built from nothing outside `sq/` |
| [`aa/`](aa/) | roles, authentication and authorization, a library on sm that digres links ([`aa/AA_DESIGN.md`](aa/AA_DESIGN.md), [`aa/USAGE.md`](aa/USAGE.md)) |
| [`bench/`](bench/) | digres against PostgreSQL 18, query by query (`bench/run.sh`) |

To try digres, from a build to a query:
[`QUICKSTART.md`](QUICKSTART.md).

## How the parts fit

```
 psql, pgbench, any PostgreSQL client
        │  PostgreSQL's wire protocol
        ▼
 dg/  digres: one process, a thread a session
        parse ──────────────► sq/  the SQL parser
        plan, execute            (PostgreSQL 18's planner and
                                  executor, unchanged)
        roles, privileges ──► aa/  on sm
        tables, indexes ────► sm/include/sm.h, from
        │                     dg/src/backend/access/sm/
        ▼
 sm/  the storage stack, each level on the one below
        sm3  schema and index maintenance: catalog, index
             builds, scans, statistics
        sm2  types and tuples: key encoding, tuple format, row
             and index relations
        sm1  MVCC: stores, transactions, isolation, locks, 2PC,
             recovery, garbage collection
        RocksDB  bytes: ordered, durable, crash-atomic
     sm0  errors, helpers and the system services (sm_sys.h)
          that sm and dg both build on; depends on nothing
```

- **digres keeps PostgreSQL's planner and executor** and
  replaces what is under them: a table is an sm store (one
  RocksDB column family), a row an sm2 tuple, a scan an sm
  cursor. Its catalog is sm's; PostgreSQL's catalog tables are
  made from it on demand
  ([`dg/SYSCATALOG.md`](dg/SYSCATALOG.md)).
- **A database is its directory**, `base/X`, its rows and WAL
  in one RocksDB instance there ([`dg/CODE.md`](dg/CODE.md)).
- **Each level owns its notions**: row versions, transactions
  and visibility are sm1's; columns, types and index entries
  sm2's; tables and index definitions sm3's. Each has a design
  document: [`sm/sm1/SM1_DESIGN.md`](sm/sm1/SM1_DESIGN.md),
  [`sm/sm2/SM2_DESIGN.md`](sm/sm2/SM2_DESIGN.md),
  [`sm/sm3/SM3_DESIGN.md`](sm/sm3/SM3_DESIGN.md), and
  [`sm/sm0/README.md`](sm/sm0/README.md).
- **`sm/rel/`** is a Lisp-like scripting language, Janet, that
  drives `sm.h`. It tests the stack without SQL.

Start with [`sm/README.md`](sm/README.md). Before changing code,
read [`CLAUDE.md`](CLAUDE.md), then [`sm/CODE.md`](sm/CODE.md)
for `sm/` and `aa/`, or [`dg/CODE.md`](dg/CODE.md) for `dg/`.

## Prerequisites

Developed on Ubuntu 24.04, aarch64; the build must also work
on x86_64, and with GCC 15 (Ubuntu 26.10).

**`./SETUP_UBUNTU_DEV.sh`** does all of this section on a
minimal Ubuntu machine. Each step is skipped once its result is
there, so it can be run again after a failure; `SRC=` sets where
it clones Janet, `~/p` by default. It does:

- **apt packages**: the list below, and `ca-certificates`.
- **Janet**: commit `86ca958e`, installed into `/usr/local`.
- **`PATH`**: `ext/pg18.6/install/bin` first, in `~/.profile`.

By hand, on Ubuntu:

```
sudo apt install build-essential cmake git wget flex bison python3 \
    clang-format libicu-dev \
    libsnappy-dev liblz4-dev libzstd-dev zlib1g-dev libbz2-dev \
    libreadline-dev libssl-dev openssl pkg-config bc \
    linux-tools-generic
```

`libssl-dev` is OpenSSL, which digres always links for TLS.
`wget` downloads `ext/`'s sources. `libsnappy-dev` to
`libbz2-dev` are RocksDB's compression libraries.
`libreadline-dev` and `pkg-config` are for building PostgreSQL
18.6, `openssl` for the certificates its TLS tests make, `bc`
for `bench.old/btree.sh`. No package holds PostgreSQL or
RocksDB: `ext/` builds both, with the server, `psql`,
`pgbench` and `libpq`.

- **`ext/`: PostgreSQL 18.6 and RocksDB**, downloaded with
  `wget` and built there by `make` the first time
  ([`ext/Makefile`](ext/Makefile)); `make -j4 ext` builds them
  alone. Both take a while, once.
  - `ext/pg18.6`, PostgreSQL 18.6, the version digres forked,
    built in its source tree with OpenSSL and installed into
    its `install/`, optimized, with no `--enable-cassert`.
    pgregress, the isolation specs and the regression suite run
    its `psql`, `pg_regress` and isolation tester; its `psql`
    tests digres's TLS (`dg/test/tls.sh`); the benchmark compares
    digres with its server
    ([`bench.old/DESIGN.md`](bench.old/DESIGN.md#2-optimized-builds) §2).
    `PG18_SRC` names another tree.
  - `ext/rocksdb-11.13.2`, RocksDB, static: `release/`,
    `-O3 -DNDEBUG`, which the debug and release builds both link,
    and `tsan/`, built by `make tsan`. `ext/ext.mk` names the
    version; a new one is a new directory.
- **Janet**, for `sm/rel`, which embeds its interpreter: built
  from source and installed into `/usr/local`, where
  `sm/rel/Makefile` looks for `janet.h` and `libjanet`. The
  tests run on 1.43.0-dev, commit `86ca958e`.

  ```
  git clone https://github.com/janet-lang/janet.git
  cd janet && git checkout 86ca958e
  make -j4 && sudo make install && sudo ldconfig
  ```
- **`psql` and `pgbench`**, `ext/pg18.6`'s: digres serves
  PostgreSQL's own clients. Put them on `PATH` by hand: `export
  PATH=$HOME/p/digres/ext/pg18.6/install/bin:$PATH`, in
  `~/.profile` to keep it (§Connect). dg's tests put them
  first themselves.

## Building

From the top, as each part's own Makefile is driven by the
one there (`Makefile`):

```
export DEBUG=1; make -j4    # debug: dg/build/digres
unset DEBUG; make -j4       # release: dg/build/digres-release
make -j4 tsan               # dg/build/digres-tsan, and its tests
make format                 # clang-format sm, sq and aa
make clean
```

| Part | Debug, `DEBUG=1 make -j4` | Release, `make -j4` |
| --- | --- | --- |
| dg | `dg/build/digres`, `-O0 -g`, with `Assert()` | `dg/build/digres-release`, `-O2 -DNDEBUG` |
| sm | `sm/build/libsm.a`, `-O0 -g`, with `assert()` | `sm/build/release/libsm.a`, `-O3 -DNDEBUG` |
| aa | `aa/build/libaa.a`, `-O0 -g`, with `assert()` | `aa/build/release/libaa.a`, `-O3 -DNDEBUG` |
| sq | `sq/build/libsq.a`, `-O0 -g`, with `assert()` | `sq/build/libsq-release.a`, `-O2 -DNDEBUG` |
| RocksDB 11.13.2 | `ext/`'s, `-O3 -DNDEBUG`, static | the same |
| For | the tests, and running by hand | the benchmark |

The TSan build, `dg/build/digres-tsan`, is digres under the
thread sanitizer, sm and RocksDB instrumented with it: for
changes to threads, latches, locks or shared state.

- **`make clean` keeps `ext/`**: it is outside every `build/`.
  `make -C ext clean` removes it, and the next `make` downloads
  and builds it again.
- **`digres --build` says how a binary was built**: the flags
  of digres, sq and sm, and which RocksDB it links. A release
  build shows `-DNDEBUG` on each, and RocksDB `static`.
- **Flags are not tracked** by the make of dg and sq: after
  changing them, delete `dg/build/rel` and `sq/build/rel` before
  building the release again.
- **A part alone** builds in its own directory, as
  `make -C sm/sm1 -j4`, `DEBUG=1` or not.
- **`make format`** before a commit. dg is formatted by the
  lines a change touches ([`dg/CODE.md`](dg/CODE.md#8-style) §8).

## Running digres

### Make a data directory

```
dg/build/digres --init /path/to/data
```

`digres --init` makes a data directory, as PostgreSQL's
`initdb` does; digres has no `initdb` (`--init --help` lists
its options). It makes the database `postgres`, and
`pg_global`, which holds the roles and which no client
connects to, with one superuser:

- **`-U NAME`**: the superuser, by default your OS user name.
- **`-W`** (prompt) or **`--pwfile=FILE`**: its password.
  Without one, only local socket connections work.
- The encoding is UTF8 and the collation C. Authentication is
  not configurable: a local socket connection is admitted
  when the OS user is the role (peer), a TCP connection by the
  role's password (SCRAM-SHA-256). There is no `pg_hba.conf`.

### Start the server

```
dg/build/digres -D /path/to/data -p 5432 -k /tmp
```

- It runs in the foreground and logs to stderr; Ctrl-C, or
  `kill -INT $(head -1 /path/to/data/postmaster.pid)`, shuts
  it down.
- **`-p`**: the TCP port, 5432 by default. **`-k`**: the
  socket's directory, `/tmp` by default; keep its path short,
  as a socket's may not pass 107 bytes. It listens on
  `localhost`.
- **Settings**: `-c NAME=VALUE`, or lines in
  `/path/to/data/postgresql.conf`, which `--init` does not
  write. `shared_buffers` (128MB by default) is the size of
  RocksDB's block cache: the benchmark uses 2GB.
- The release build, `dg/build/digres-release`, runs the same
  way.

### Connect

```
psql -h /tmp -p 5432 -d postgres                 # the socket, as your OS user
psql -h localhost -p 5432 -U NAME -d postgres    # TCP, with the password
```

- **Use PostgreSQL 18's `psql`** (`ext/pg18.6/install/bin`,
  above). Ubuntu 24.04's `psql` 16 connects and runs SQL, and
  `\d`, `\dt` and `\di` work, but `\l` fails, as it does
  against PostgreSQL 18 itself: it reads
  `pg_database.daticulocale`, which PostgreSQL 17 renamed
  `datlocale`. Any PostgreSQL driver connects.
- `CREATE DATABASE app` makes another database; each is a
  directory, `base/app`, of its own.
- What digres does not run yet fails with an error, not a
  wrong answer ([`dg/UNSUPPORTED.md`](dg/UNSUPPORTED.md)).

## Testing

```
make -j4 test
```

It runs every part's tests on the debug build, whatever
`DEBUG` is, in order: sm, sq, aa, then digres, which runs on
the other three, stopping at the first that fails. Every
commit that touches source passes it. digres's part ends with
these lines:

```
run: PASSED (124 files)
run: PASSED (12 files, alone and in 8 sessions)
initdb: PASSED
server: PASSED
slt: PASSED (49 files on digres, 10711 records, 11 skipped)
all 231 ok
```

One part's alone is `make -j4 test-sm`, `test-sq`, `test-aa` or
`test-dg`, on the debug build too. The output is long: save it
to a file and read that.

**Which test is running**: while a doctest binary runs, a file
beside it, `BINARY.active` (`sm/sm2/build/test_sm2.active`, for
one), holds the test case it is in, its name, then its file and
line. It goes when the run ends; a run that crashed or hangs
leaves it naming its test
(`sm/sm0/include/sm0_doctest_active.h`).

```
cat sm/*/build/*.active aa/build/*.active sq/build/*.active 2>/dev/null
```

What each is:

- **sm**: each level's unit tests (`sm/smN/test`), the
  isolation scenarios of sm1's concurrency control
  (`sm/sm1/mvcc`, its specification: change one only with
  the maintainer's approval), and `rel` (`sm/rel/test`), which
  drives `sm.h` from scripts.
- **sq**: the parser's tests, unit tests and the SQL files of
  `sq/test`; `make -C sq corpus-test` checks it over
  PostgreSQL's regression SQL.
- **aa**: its unit tests.
- **dg**: the `.sql` files of `dg/test`, alone and in eight
  sessions; `initdb`; `server.sh`, a server driven by `psql`; the
  sqllogictest files of `dg/test/slt`, whose results are
  PostgreSQL 18's
  ([`dg/test/slt/README.md`](dg/test/slt/README.md)); and
  pgregress, PostgreSQL 18's regression files with what digres
  does not run commented out, against PostgreSQL's output
  ([`dg/test/pgregress/README.md`](dg/test/pgregress/README.md)).

**Keep test data off `/tmp`.** Set `TMPDIR` to a directory on a
real disk, on every machine, as `/tmp` may be in memory:

```bash
mkdir -p bench/build/tmp
export TMPDIR=$PWD/bench/build/tmp
```

Run on their own, outside `make test`:

| Command | What it runs |
| --- | --- |
| `make -C dg -j4 slt` | every sqllogictest file, `select5.test` too |
| `dg/test/slt.sh FILE...` | some of them; `--pg18` runs them on PostgreSQL 18 instead |
| `make -C dg -j4 pgregress` | the ported regression files alone |
| `make -C dg -j4 isolation` | PostgreSQL 18's isolation specs, against `dg/test/isolation_specs.txt`; for any change to locks or waits; ten minutes |
| `make -C dg -j4 regress` | PostgreSQL 18's whole regression suite, unported: only measures what is missing |
| `make -j4 tsan` | the TSan build and its tests; for any change to threads, latches, locks or shared state |

A test is written with a one-line synopsis and numbered steps,
and is shown to fail by breaking what it checks before it is
committed ([`CLAUDE.md`](CLAUDE.md#5-test-structure) §5).

## Benchmarks

`bench/` measures digres against PostgreSQL 18, on optimized
builds only ([`bench.old/CODE.md`](bench.old/CODE.md)): a debug
build's number measures the build. `perf` must be allowed to
count, which a system with `kernel.perf_event_paranoid` above 1
refuses: `sudo sysctl kernel.perf_event_paranoid=1`, until the
next reboot. To keep it:

```
echo 'kernel.perf_event_paranoid = 1' |
    sudo tee /etc/sysctl.d/90-perf.conf
```

A virtual machine may also offer no hardware counters at all,
or only some, so see which it has before relying on them:

```
perf stat -e instructions,cycles -- true
```

If `instructions` says `<not supported>`, as on a GCP
`n4a-standard-4`, which has `cycles` alone,
`bench.old/btree.sh`, `bench.old/relq6/run.sh` and the
instruction counts below cannot run there.

```
DG_PORT=5432 PG_PORT=5433 make -j4 bench  # both servers up
```

- `make bench` builds `dg/build/digres-release`, then runs
  `bench/run.sh`. Start that build's server before the run.
- `run.sh` makes the database `bench` again on each server
  (`bench/load.sql`: `usertable`, 10,000 rows), then times q0 to
  q4 (`bench/q0-pkget.sql`, `q1-sk10.sql`, `q2-sk180.sql`,
  `q3-skfetchrow.sql` and `q4-seqscan.sql`). q0 and q4 run once.
  q1 to q3 run on regular (lsm) indexes, then on btree ones
  (`mkregidx.sql`, `mkbtidx.sql`). Each query is pgbench,
  prepared, one client, for `Q_SECONDS` (10). It prints tps,
  latency, and digres's latency over PostgreSQL's; the logs go to
  `bench/build/run`.
- It refuses a debug build of either side: a server whose
  `debug_assertions` is on, or a digres whose `--build` is not
  a release's.
- The queries:
  - q0: one row by the primary key.
  - q1: 8 entries of the `lastname` index, a count.
  - q2: 183 entries of the `state` index, a count.
  - q3: 8 rows, found by the `lastname` index.
  - q4: every row, a sum, no index.
- [`PERF.md`](PERF.md) has the throughput of digres, PostgreSQL
  18, MyRocks and InnoDB on q0 to q4.
- The old benchmark is `bench.old/run.sh`. Its queries, Q1 to
  Q11, are in [`bench.old/DESIGN.md`](bench.old/DESIGN.md).
- Judge digres against PostgreSQL, never against an older
  digres.
- **On a machine whose clock varies** (a VM, a laptop),
  latency swings run to run with the same work. Compare
  instructions a statement instead: pin the server
  (`taskset -c 0-3`) and pgbench (`taskset -c 4`), and count
  the server's with `perf stat -e instructions -p PID` over a
  fixed `pgbench -t N`.
- `bench.old/smbench/`, `bench.old/getseek/` and
  `bench.old/relq6/` measure layers below SQL
  ([`bench.old/README.md`](bench.old/README.md)).

## Where things are

| Document | What it holds |
| --- | --- |
| [`QUICKSTART.md`](QUICKSTART.md) | build, make a database, connect, and run a query |
| [`sm/README.md`](sm/README.md) | the stack's levels, and each one's design document |
| [`sm/NEXT.md`](sm/NEXT.md) | what is to be built next, in order |
| [`DG-vs-PG.md`](DG-vs-PG.md) | how digres differs from PostgreSQL 18 |
| [`PERF.md`](PERF.md) | how fast digres runs against PostgreSQL 18, MyRocks and InnoDB |
| [`dg/README.md`](dg/README.md), [`dg/DESIGN.md`](dg/DESIGN.md) | what digres is, and its decisions of record |
| [`dg/WORKNOTE.md`](dg/WORKNOTE.md) | every step taken in digres, and why |
| [`sm/BTREEINDEX.md`](sm/BTREEINDEX.md) and the other notes in `sm/` | designs of a feature each, built or proposed |

## License

The project's original code is under the MIT License: see
[`LICENSE`](LICENSE). Files containing only original code use
`SPDX-License-Identifier: MIT`.

Files derived from PostgreSQL retain its notices. A separate
"Portions Copyright" notice licenses this project's original
additions under MIT. That notice does not relicense upstream
code. Formatting alone does not warrant a new copyright notice.

The main upstream components and their exceptions are below.
Each file's notices identify the terms that apply to it.

| Code | License | Notice |
| --- | --- | --- |
| PostgreSQL-derived code in `dg/`, except the components below | PostgreSQL License for upstream portions; MIT for this project's original additions | [`dg/COPYRIGHT`](dg/COPYRIGHT), [`LICENSE`](LICENSE), and file headers |
| Original code in `dg/` | MIT | [`LICENSE`](LICENSE) and file headers |
| `dg/src/backend/regex/` | Henry Spencer's and Tcl's licenses | [`dg/src/backend/regex/COPYRIGHT`](dg/src/backend/regex/COPYRIGHT) |
| PostgreSQL-derived grammar, scanner, nodes and helpers in `sq/` | PostgreSQL License for upstream portions; MIT for this project's original additions | [`sq/COPYRIGHT`](sq/COPYRIGHT), [`LICENSE`](LICENSE), and file headers |
| Ryu files in `dg/src/common/`, `dg/src/include/common/shortest_dec.h` and `sm/sm2/src/ryu/` | PostgreSQL License for PostgreSQL portions; Boost Software License 1.0 for Ryu; MIT for this project's original additions where marked | [`dg/COPYRIGHT`](dg/COPYRIGHT), [`LICENSES/BSL-1.0.txt`](LICENSES/BSL-1.0.txt), and file headers |
| `dg/src/timezone/localtime.c`, `private.h` and `tzfile.h` | Upstream code is public domain; this project's original additions are MIT where marked | File headers and [`LICENSE`](LICENSE) |
| `dg/src/timezone/strftime.c` | BSD-3-Clause for upstream code; MIT for this project's original additions where marked | [File notice](dg/src/timezone/strftime.c) and [`LICENSE`](LICENSE) |
| `sm/sm0/include/doctest.h`, by Viktor Kirilov | MIT | the header of the file |
| `dg/test/slt/sqlite/`, SQLite's sqllogictest | public domain, or a license of the user's choice | [`dg/test/slt/sqlite/COPYRIGHT.md`](dg/test/slt/sqlite/COPYRIGHT.md) |

RocksDB, ICU, OpenSSL and Janet are linked, not copied into the
repository. Each keeps its own license. Include the applicable
license texts and notices when distributing code from this
repository, including copies of individual components.

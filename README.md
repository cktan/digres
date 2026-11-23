# digres

**Postgres reimagined, with a different process model and a
key-value storage engine.**

digres runs PostgreSQL 18's planner and executor on sm, its
own storage engine, which keeps its data in RocksDB for now. It
speaks PostgreSQL's wire protocol, so `psql`, `pgbench` and any
PostgreSQL driver connect to it unchanged.

A table is a row store or a column store. A row store table,
the default, is a RocksDB column family. A row's key is its
primary key in an order-preserving encoding, and its value is
the tuple. A column store table keeps its rows column by column,
in immutable files.

**digres is a work in progress, and much remains to be done.**
It is not ready for production. It has no PL/pgSQL, no
triggers, no partitioning, no replication and no base backup
yet. What it does not run fails with an error, not a wrong
answer ([`dg/UNSUPPORTED.md`](dg/UNSUPPORTED.md)). It is also
slower than PostgreSQL on every query we measure: 1.2 to 2.9
times PostgreSQL's latency, on one client
([`PERF.md`](PERF.md)). [`sm/NEXT.md`](sm/NEXT.md) lists what
comes next.

## What is new

digres keeps PostgreSQL's analyzer, planner and executor. Much
of what is above them, and everything under them, is new.

### Above the executor

- **One process, with one thread per session**: no fork per
  connection, and no background processes; background work
  runs in threads. A session's globals are `thread_local`.
- **C++, with exceptions**: the backend compiles as C++20, and
  `ereport(ERROR)` throws an exception, not a `siglongjmp`.
- **The SQL parser is a library of its own**, `sq/`:
  PostgreSQL 18's grammar and scanner, with no dependency on
  the backend.
- **Roles, authentication and authorization are redone** in
  `aa/`, a library on sm. `pg_hba.conf` is removed: a local
  login is by peer or password, a TCP login by SCRAM-SHA-256. A
  privilege check is a lookup in memory.

### Under the executor

- **Tables organized by their primary key**, through sm, a
  storage library of its own.
- **sm does its own MVCC**: visibility, locks and SERIALIZABLE
  isolation ([`sm/MVCC_OVERVIEW.md`](sm/MVCC_OVERVIEW.md)). It
  opens a plain `rocksdb::DB`, not a `TransactionDB`, and
  commits a transaction as one `WriteBatch`. RocksDB is an
  ordered, durable, atomic store.
- **MVCC with no VACUUM to schedule**: a RocksDB compaction
  filter and one worker thread per database remove the row
  versions that no snapshot can see
  ([`sm/GC.md`](sm/GC.md)).
- **A database is one directory**, `base/X`: one RocksDB
  instance holds its rows, its catalog and RocksDB's WAL.
  PostgreSQL's WAL and checkpointer are removed.
- **A column store, for analytics**: `CREATE TABLE ... WITH
  (storage = columnar)` keeps a table column by column
  ([`sm/COLSTORE.md`](sm/COLSTORE.md)).
  - New rows go to a row store first. A worker thread seals
    them in groups of 10k rows: one compressed, immutable file
    for each column.
  - A scan reads only the columns that the query uses. It skips
    each group of rows that the predicate cannot match, from
    the min, max and filter of each column, kept in RocksDB.
  - `UPDATE` and `DELETE` work on sealed rows. A delete marks
    the row dead in its group, and an update also writes the
    new version to the row store. When a fifth of a group is
    dead, the worker writes a new version of the group with
    the live rows alone. A group under half live goes back to
    the row store, to be sealed again with other rows.
  - A value is 2 KB at most. Put a longer value in a row store
    table, and join the two.
  - A column store table has no index and no key. digres
    ignores `PRIMARY KEY`, `UNIQUE`, foreign keys and `CREATE
    INDEX` on it, with a warning. It refuses `SERIALIZABLE`
    and `TABLESAMPLE` on it.
- **The column store needs another executor for its full
  benefit.** PostgreSQL's executor takes one row at a time.
  So a scan of a column store table makes each row again from
  its columns, and the executor evaluates it row by row. The
  scan reads less data, but it does no work on a column as a
  batch. A columnar executor, which works on batches of column
  values, is still to be built.

## Get started

| To | Read |
| --- | --- |
| go from a fresh checkout to a query | [`QUICKSTART.md`](QUICKSTART.md) |
| install the prerequisites and build | [`BUILDING.md`](BUILDING.md) |
| make a database, start the server and connect | [`RUNNING.md`](RUNNING.md) |
| run the tests | [`TESTING.md`](TESTING.md) |
| run the benchmarks | [`bench/README.md`](bench/README.md) |
| see the benchmark results | [`PERF.md`](PERF.md) |
| see how digres differs from PostgreSQL 18 | [`DG-vs-PG.md`](DG-vs-PG.md), [`dg/UNSUPPORTED.md`](dg/UNSUPPORTED.md) |

## How it is built

```
 psql, pgbench, any PostgreSQL client
        │  PostgreSQL's wire protocol
        ▼
 dg/  digres: one process, one thread per session
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
  RocksDB column family), a row is an sm2 tuple, and a scan is
  an sm cursor. sm owns the catalog; digres makes PostgreSQL's
  catalog tables from it on demand
  ([`dg/SYSCATALOG.md`](dg/SYSCATALOG.md)).
- **Each level owns its notions**: sm1 owns row versions,
  transactions and visibility; sm2 owns columns, types and
  index entries; sm3 owns tables and index definitions.
- **Two kinds of secondary index**: an LSM index, the default,
  keeps each entry as a RocksDB key. A B-tree index is a B-link
  tree whose pages are RocksDB values
  ([`sm/BTREEINDEX.md`](sm/BTREEINDEX.md)).
- **`sm/rel/`** is a Lisp-like scripting language, Janet, that
  drives `sm.h`. It tests the stack without SQL.

The repository holds digres and the libraries it links:

| Where | What it is |
| --- | --- |
| [`sm/`](sm/) | sm, the storage stack: a library of its own, for digres or another SQL database system. An MVCC storage manager on RocksDB, types and tuples over it, and schema and index maintenance over those, with one API, `sm/include/sm.h` |
| [`dg/`](dg/) | digres: a fork of PostgreSQL 18.6 that runs on the stack. It compiles as C++, is one process with one thread per session, and serves PostgreSQL's own `psql`; sm stores its tables |
| [`sq/`](sq/) | the SQL parser: PostgreSQL 18's SQL in, a tree of nodes out, as a library with one API, `sq/include/sq.h`. It is built from nothing outside `sq/` |
| [`aa/`](aa/) | roles, authentication and authorization, a library on sm that digres links |
| [`ty/`](ty/) | the operations of the SQL types, a library of its own that sm and digres call. It is built from nothing outside `ty/` |
| [`bench/`](bench/) | digres against PostgreSQL 18, query by query |

## Design documents

Start with [`sm/README.md`](sm/README.md). Before changing code,
read [`sm/CODE.md`](sm/CODE.md) for `sm/`, `aa/` and `ty/`, or
[`dg/CODE.md`](dg/CODE.md) for `dg/`.

| Document | What it holds |
| --- | --- |
| [`sm/README.md`](sm/README.md) | the stack's levels, and each one's design document |
| [`sm/sm1/SM1_DESIGN.md`](sm/sm1/SM1_DESIGN.md), [`sm/sm2/SM2_DESIGN.md`](sm/sm2/SM2_DESIGN.md), [`sm/sm3/SM3_DESIGN.md`](sm/sm3/SM3_DESIGN.md), [`sm/sm0/README.md`](sm/sm0/README.md) | the design of each level |
| [`sm/BTREEINDEX.md`](sm/BTREEINDEX.md), [`sm/COLSTORE.md`](sm/COLSTORE.md) and the other notes in `sm/` | designs of a feature each, built or proposed |
| [`sm/NEXT.md`](sm/NEXT.md) | what is to be built next, in order |
| [`dg/README.md`](dg/README.md), [`dg/DESIGN.md`](dg/DESIGN.md) | what digres runs, and its decisions of record |
| [`sm/PGFORK.md`](sm/PGFORK.md#10-digres-how-the-fork-is-to-be-made) | how the fork of PostgreSQL is made |
| [`aa/AA_DESIGN.md`](aa/AA_DESIGN.md), [`aa/USAGE.md`](aa/USAGE.md) | roles, authentication and authorization |
| [`ty/DESIGN.md`](ty/DESIGN.md) | the operations of the SQL types, and how they get memory |
| [`dg/WORKNOTE.md`](dg/WORKNOTE.md) | every step taken in digres, and why |

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
| Ryu files in `dg/src/common/`, `dg/src/include/common/shortest_dec.h`, `sm/sm2/src/ryu/` and `ty/src/ryu/` | PostgreSQL License for PostgreSQL portions; Boost Software License 1.0 for Ryu; MIT for this project's original additions where marked | [`dg/COPYRIGHT`](dg/COPYRIGHT), [`LICENSES/BSL-1.0.txt`](LICENSES/BSL-1.0.txt), and file headers |
| `dg/src/timezone/private.h` | Upstream code is public domain; this project's original additions are MIT where marked | File header and [`LICENSE`](LICENSE) |
| `dg/src/timezone/strftime.c` | BSD-3-Clause for upstream code; MIT for this project's original additions where marked | [File notice](dg/src/timezone/strftime.c) and [`LICENSE`](LICENSE) |
| PostgreSQL-derived code in `ty/`: `src/dtparse.cc`, `src/hash.cc` and `include/ty_hash.h` | PostgreSQL License for upstream portions; MIT for this project's original additions | [`ty/COPYRIGHT`](ty/COPYRIGHT), [`LICENSE`](LICENSE), and file headers |
| `ty/src/timezone.cc`, from PostgreSQL's copy of the IANA tz library | Public domain for the IANA code; PostgreSQL License for PostgreSQL's changes; MIT for this project's original additions | [`ty/COPYRIGHT`](ty/COPYRIGHT) and the file header |
| `ty/src/tzdata.cc`, the IANA time zone data that PostgreSQL's `zic` compiled | Public domain | The file header and [`ty/TZDATA.md`](ty/TZDATA.md) |
| `ext/doctest/doctest.h`, by Viktor Kirilov, with an option of this project's added | MIT | [`ext/doctest/LICENSE.txt`](ext/doctest/LICENSE.txt) and the header of the file |
| RocksDB, downloaded into `ext/` at build time and linked into the digres binary; not in the repository | Apache License 2.0, of RocksDB's choice of Apache 2.0 or GPLv2; BSD-3-Clause for its code from LevelDB | A binary release carries RocksDB's `LICENSE.Apache` and `LICENSE.leveldb` |
| `dg/test/slt/sqlite/`, SQLite's sqllogictest | public domain, or a license of the user's choice | [`dg/test/slt/sqlite/COPYRIGHT.md`](dg/test/slt/sqlite/COPYRIGHT.md) |

RocksDB, ICU, OpenSSL and Janet are linked, not copied into the
repository. Each keeps its own license. Include the applicable
license texts and notices when distributing code from this
repository, including copies of individual components.

DuckDB's SQL tests are fetched when `dg/test/duck/duck.sh` runs,
not copied into the repository.

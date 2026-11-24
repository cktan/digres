# DG vs. PG: digres and PostgreSQL 18

`digres` (DG) is a hard fork of PostgreSQL 18.6 (PG) that runs on
the sm storage stack (`sm/`). DG keeps PG's wire protocol,
analyzer, planner, and executor. It replaces the process model,
storage engine, transaction manager, catalog, and authentication
layer. It moves the operations of the SQL types into a library,
`ty/`. [`PERF.md`](PERF.md) measures how fast DG runs against PG.

> **Roles, authentication, and authorization are redone.** DG
> does not keep PG's role and privilege machinery. A new
> library, `aa/`, stores roles and ACLs, checks logins, and
> answers privilege checks. PG's code calls it (§3).

| Area | PostgreSQL 18 (PG) | digres (DG) |
| --- | --- | --- |
| Wire protocol & clients | PG v3 protocol; `psql`, `libpq`, `pgbench` | Unchanged; uses stock PG 18 clients |
| Source language | C | Strict C++20 (`-fno-rtti`), errors as C++ exceptions |
| SQL type operations | Built into the backend (`utils/adt`) | Standalone library (`ty/`) that both `sm` and DG call |
| Time zone data | Files on disk | Compiled into `ty/` |
| Process model | Multi-process (`fork` per connection) | Single process, one `std::thread` per session |
| Shared state | SysV / POSIX shared memory (`ShmemAlloc`) | Process heap (`pcb` and `shmem`); other globals are `thread_local` |
| SQL parser | Built into the backend (`gram.y`, `scan.l`) | Standalone library (`sq/`) of original PG sql parser linked into DG |
| Storage engine | Heap files, 8 KB pages, `smgr`, buffer pool | RocksDB via `sm/` (one column family per store); immutable files for a column store |
| Row identity | Physical tuple ID (`ctid`: block + offset) | Primary key bytes, or hidden `int8` `_id` if keyless or a column store |
| Table organization | Unordered heap + separate B-tree for PK | Index-organized (clustered on PK or `_id`) |
| Column store | None | `WITH (storage = columnar)`, for analytics |
| Database portability | Cluster-bound (shared XIDs, OIDs, WAL, `global/`) | Self-contained directory (`base/X` holds data, catalog, WAL); its import not built yet |
| Garbage collection | `VACUUM` and autovacuum processes | Automatic in `sm1`, and a btree's dead entries by a background btree reaper; `VACUUM` compacts RocksDB stores and cleans btree leaves |
| Durability & recovery | PG WAL (`pg_wal`), checkpointer, redo recovery | RocksDB WAL and flush; `CHECKPOINT` is a no-op |
| Transactions & MVCC | Heap `xmin`/`xmax`, `pg_xact` SLRU, snapshots | `sm1` MVCC on RocksDB; no XID columns on rows |
| `SERIALIZABLE` | Predicate locks and SSI in shared memory | `sm1` serializable isolation |
| Row & table locks | Shared-memory lock tables + heap tuple header bits | All in `sm1`'s lock manager (5 table modes, row locks in memory); DG has none, but for advisory locks |
| System catalog | Heap tables (`pg_class`, `pg_attribute`, ...) | Compiled-in arrays + `sm3` catalog; virtual read-only relations |
| Roles & auth | `pg_authid` heap table, `pg_hba.conf`, TLS, GSSAPI | **Redone**: new `aa/` library in `pg_global`; fixed peer / SCRAM-SHA-256 rules; TLS by one setting; no GSSAPI |
| Index access methods | B-tree, Hash, GiST, SP-GiST, GIN, BRIN | `lsm` (default) and `btree`, a B-link tree, both in `sm`; no bitmap scans |
| Procedural languages | PL/pgSQL, SQL functions, C extensions | Removed (SPI remains for a future language) |

---

## 1. Architecture and Process Model

- **One process, one thread per session.** PG forks a child
  process for each client connection and runs auxiliary processes
  for background work (autovacuum, checkpointer, WAL writer,
  background writer, syslogger). DG runs in a single process.
  Each client connection is a `std::thread`. All background
  processes are removed. `sm` does background work in threads.
- **Thread-local globals.** Every writable global and file-scope
  static variable in DG is marked `thread_local /*LOCAL*/`. The
  build verifies this before linking. Only two structs are shared
  by the threads, `pcb` and `shmem`:
  - `pcb`: only the main thread writes it. After the first
    thread starts, the main thread writes only its atomic
    fields, such as the reload time.
  - `shmem`: every thread can write it. Each field is atomic,
    guarded by a mutex, or an object that guards itself.
  - Two more structs belong to one thread each: `tcb` is a
    thread's own state, and `postmaster_tcb` is the main
    thread's.
- **`log_min_messages` and `log_min_error_statement` are the
  server's, not a session's.** In PG a superuser can `SET` them
  for one session. In DG they are `PGC_SIGHUP`: a `SET` fails with
  "cannot be changed now", as does a client's startup option. They
  change with the configuration file, `ALTER SYSTEM` and a reload.
  `errstart()` reads one atomic copy in `pcb`. `CREATE EXTENSION`
  no longer raises `log_min_messages` for its script.
- **Four settings are fixed, not settings.**
  `standard_conforming_strings` is on, `backslash_quote` is
  `safe_encoding`, `escape_string_warning` is on and `xmloption`
  is `content`: the values PG has by default. `SET`, `SHOW` and
  `postgresql.conf` of any of them fail with "unrecognized
  configuration parameter". So `SET standard_conforming_strings
  = off`, with its backslash escapes in ordinary strings, is
  refused. A string reads as the SQL standard says; `E'...'`
  still escapes. The server still reports
  `standard_conforming_strings = on` when a connection starts,
  as `psql` and libpq lex a string by that report.
- **Exceptions instead of `sigsetjmp`.** DG compiles all backend
  code as C++20. `ereport(ERROR)` throws `pg_error_t`, and
  `proc_exit()` throws `proc_exit_t` to unwind the session thread
  without terminating the server process. The `PG_TRY`
  macros are removed: code uses `try` and `catch`.
- **Signals are interrupt bits.** The postmaster blocks signals
  and reads them via `signalfd`. Session threads wake and cancel
  each other through `sm_sys.h` interrupt flags and latches
  rather than OS signals.
- **No single-user or bootstrap mode.** PG's `postgres --single`
  runs one backend on standard input, and `--boot` builds the
  catalogs for `initdb`. DG refuses both, and has no `initdb`:
  `digres --init` makes a data directory through sm, and a
  server under a debugger serves every session in one process.
- **Standalone parser (`sq/`).** The SQL grammar and scanner live
  in `sq/` with no dependency on the backend. DG links `libsq.a`
  and translates parse nodes directly into its memory contexts.
- **Standalone type operations (`ty/`).** In PG, the operations
  of a type are in the backend, tied to its memory contexts and
  its errors. In DG, they are pure functions in `ty/`
  ([`ty/DESIGN.md`](ty/DESIGN.md)), with no dependency on `sm` or
  the backend. DG's SQL functions are wrappers that call them.
  The types are the integers, floats, decimals, bool, uuid,
  bytea, the string types, dates and times, and `interval`. The
  answers are PG's, except for `numeric` (§6). Some parts stay in
  DG, such as the binary forms, the regular expression engine,
  the array functions, `money`, and `jsonb`
  ([`sm/TYPEMOVE_PLAN.md`](sm/TYPEMOVE_PLAN.md) §7, "What stays
  in digres for good").
- **Time zone data is compiled in.** PG reads time zone files
  from disk. DG has the IANA data in `ty/`
  ([`ty/TZDATA.md`](ty/TZDATA.md)), so new data needs a new
  build. Zone names and POSIX specs work as in PG.

## 2. Storage Engine and Table Layout

- **A database is its directory.** In PG, all databases share a
  single WAL, transaction ID counter, and cluster catalog. In DG,
  a database `X` lives entirely inside `base/X` as one RocksDB
  instance with its own WAL, catalog, sequences, and ID counters.
  It shares nothing on disk with other databases. The import of
  such a directory into another instance is not built yet.
- **Index-organized tables.** PG stores rows in an unordered heap
  and builds a separate B-tree file for the primary key. DG
  stores every row store table, the default kind, in primary-key
  order in `sm`, so a sequential scan reads rows in key order.
  This does not set the order of a query's result: the plan can
  use a secondary index, a join, or another operator. Use `ORDER
  BY` when the result needs an order. A primary key index uses
  zero extra bytes. A primary key is capped at 1024 encoded
  bytes.
- **Row keys replace `ctid`.** PG identifies physical tuples by
  `ctid` (page and slot). DG has no pages or `ctid`. A table
  without a primary key gets a hidden `int8` system column named
  `_id` as its key. System columns `xmin`, `xmax`, `cmin`, and
  `cmax` do not exist.
- **Added in DG: the Column Store.** PG has no column store. In
  DG, `CREATE TABLE ... WITH (storage = columnar)` makes a table
  for analytics ([`sm/COLSTORE.md`](sm/COLSTORE.md)):
  - **Writes.** New rows go to a row store in RocksDB. A worker
    thread seals them in groups of 10k rows, as one compressed,
    immutable file for each column. `COPY` into an empty table
    writes the files directly.
  - **Scans.** A scan reads only the columns the query uses. It
    skips each group of rows that the predicate cannot match, by
    the min, max, and filter of each column.
  - **Updates and deletes** work on sealed rows. A worker thread
    rewrites a group when a fifth of its rows are dead.
  - **Isolation.** `READ COMMITTED` and `REPEATABLE READ` work.
    A `SERIALIZABLE` transaction that uses the table fails.
  - **No index and no key.** `PRIMARY KEY`, `UNIQUE`, `FOREIGN
    KEY`, and `CREATE INDEX` give a warning and make nothing.
    A foreign key that references the table, `ON CONFLICT` with
    a column list, and `TABLESAMPLE` fail. `NOT NULL`, `CHECK`,
    and generated columns apply.
  - **Limits.** A value is 2,048 bytes at most. A row of more
    than 64 KB fails in a scan.
  - **Executor.** PG's executor still takes one row at a time.
    A vector-aware executor is not built yet.
- **PG's storage layers are removed.** PG's buffer manager,
  `smgr`, free-space map, visibility map, TOAST tables, and
  `xlog.c` are removed. `sm` stores a long value out of line, as
  TOAST did, and `sm1` caches the pages of `btree` indexes in a
  buffer pool of its own. `shared_buffers` configures the
  RocksDB block cache size.
- **Maintenance commands.** `sm` reclaims dead versions
  automatically and RocksDB bounds recovery. `VACUUM` compacts
  each table's store. `CLUSTER` and `REINDEX` warn and do
  nothing; `CHECKPOINT` does nothing.

## 3. System Catalog, Roles, and Authentication

- **No physical catalog tables.** PG bootstraps dozens of heap
  tables in `pg_catalog` during `initdb`. DG has no physical
  catalog tables. Built-in objects (types, functions, operators,
  casts) are compiled into the binary (`cat_builtin.c`). User
  schemas, tables, indexes, views, and sequences live in `sm3`'s
  catalog.
- **On-demand catalog relations.** To support `psql` (`\d`,
  `\dt`, `\di`, `\l`, `\du`) and client drivers, DG exposes
  read-only virtual catalog relations
  ([`dg/SYSCATALOG.md`](dg/SYSCATALOG.md)). A
  sequential scan on `pg_class` or `pg_attribute` synthesizes
  rows on the fly from compiled-in data and `sm3`.
- **Roles, authentication, and authorization: redone
  (`aa/`).** DG replaces PG's implementation with `aa/`, a
  library of its own on `sm`
  ([`aa/AA_DESIGN.md`](aa/AA_DESIGN.md)). It includes
  no PG header. PG's `acl.c`, `aclchk.c`, `auth.c`, `crypt.c`,
  and others call `aa/` in place of the shared catalogs.
  The design differs from PG's:
  - **Storage.** `sm` has no shared catalogs, and no
    transaction spans two databases. So roles, memberships,
    owners, and ACLs live in a separate database, `pg_global`,
    that no client connects to.
  - **Checks.** `aa/` holds all of `pg_global` in memory as
    immutable snapshots. A repeated privilege check is a
    lookup, with no lock and no catalog read.
  - **Commit.** A change to an ACL commits in `pg_global` and
    in the user's database in a fixed order. A crash between
    the two commits can leave fewer privileges, never more.
- **Roles and authentication rules.** `pg_hba.conf` and
  `pg_ident.conf` are removed. Local Unix socket connections
  authenticate via OS peer match or password; TCP connections
  require SCRAM-SHA-256. Column and function privileges, `ALTER
  ROLE ... SET`, and `ALTER DEFAULT PRIVILEGES` fail; role DDL is
  refused in a transaction block.
- **TLS by one setting; no GSSAPI.** DG builds with OpenSSL.
  With `ssl` on, every TCP connection must use TLS, and a local
  socket connection never does
  ([`aa/ROLE.md`](aa/ROLE.md#8-decisions) §8). With `ssl` off,
  a TCP connection is not encrypted. GSSAPI is not built.
- **Passwords.** Logins use SCRAM, which sends no password. A
  password in cleartext, set by SQL, is refused over TCP
  without TLS. Send it hashed, as psql's `\password` does. The
  local socket and TLS connections accept it
  ([`aa/ROLE.md`](aa/ROLE.md#51-a-password-set-in-cleartext)
  §5.1). A logged statement never shows a password. An MD5
  password is refused on every connection.
- **Fixed encoding and locale.** Every DG database uses `UTF8`
  encoding and collation `C`. A column's `COLLATE`, `CREATE
  COLLATION`, and an index key collation other than the
  column's are refused.
- **Missing introspection.** `information_schema`, the
  `pg_stat_*` views, and the system views such as `pg_locks`,
  `pg_settings`, `pg_indexes`, and `pg_timezone_names` are
  missing. Some have a function in their place: `pg_stats()`,
  `pg_lock_status()`, `pg_show_all_settings()`, and
  `pg_timezone_names()`. `pg_description` is empty, and `COMMENT
  ON` is refused.
- **`pg_dump` does not run.** It calls `pg_is_in_recovery()` when
  it connects, and DG does not have that function.

## 4. Transactions, Locking, and Concurrency

- **`sm1` transactions.** Transaction state, snapshots,
  savepoints, visibility, and durability belong to `sm1`.
  Two-phase commit (`PREPARE TRANSACTION`), exported snapshots,
  and XID inspection functions (`txid_current()`) are removed.
- **Isolation levels.** `READ COMMITTED`, `REPEATABLE READ`, and
  `SERIALIZABLE` are implemented by `sm1`. PG's predicate lock
  manager and SLRU are removed.
- **The locks are in `sm`.** PG's lock manager and multixacts are
  removed, and DG has none of its own. `sm1` holds every lock on
  a table, a row, a schema, or another object, and has its own
  deadlock detector. PG's eight `LOCK TABLE` modes map onto
  `sm1`'s five modes, and `FOR KEY SHARE` / `FOR NO KEY UPDATE`
  map to `FOR SHARE` / `FOR UPDATE`. There are no lock wait
  queues: a soft deadlock aborts a transaction where PG reorders
  the waiters. `pg_lock_status()` shows `sm`'s mode names.
- **Advisory locks are the one exception.** DG keeps them in an
  in-memory table of its own (`advisory_lock.c`). Neither
  detector sees a deadlock through an advisory lock and a lock
  of `sm`: `lock_timeout` or a cancel ends it.
- **The clock of a transaction is `sm`'s.** DG reads it when the
  transaction starts. So `now()` is a little later than
  `statement_timestamp()` in a transaction of one statement. In
  PG, the two are equal there.
- **Behavioral differences under concurrency:**
  - Updating a primary key column in a way that collides with an
    existing row during the scan (e.g., `UPDATE t SET k = k + 1`)
    can fail with a duplicate key error because rows are scanned
    in key order.
  - If another transaction updates a row's primary key, a
    concurrent writer fails with `SQLSTATE 40001` (serialization
    failure) at all isolation levels instead of following a
    tuple update chain.
  - Rolling back to a savepoint closes any scan opened inside
    that savepoint (`SQLSTATE 0A000` on subsequent `FETCH`).
  - Creating an index on a populated table builds the index in
    the background after `COMMIT` and waits before returning; in
    an explicit transaction block, a duplicate-key failure in
    `CREATE UNIQUE INDEX` is reported at `COMMIT` after prior
    statements have committed.

## 5. Queries, Indexes, and Plans

- **Two secondary index types (`lsm` and `btree`).** Hash, GIN,
  GiST, SP-GiST, and BRIN access methods are removed; DG
  supports `UNIQUE`, partial (`WHERE`), covering (`INCLUDE`),
  and expression indexes in two formats
  ([`sm/BTREEINDEX.md`](sm/BTREEINDEX.md)):
  - **`lsm` (default, `USING lsm`):** Stores each secondary
    index entry as an individual MVCC key-value record in
    RocksDB's LSM tree.
  - **`btree` (`USING btree`):** Stores secondary index entries
    in 4 KB Lehman-Yao B-link tree pages stored as RocksDB
    values and cached in `sm1`'s buffer pool, where rows decide
    visibility.
- **Index limits.** An `interval` column cannot be an index key.
  `NULLS NOT DISTINCT` and non-default operator classes are
  refused, and so is `DROP INDEX CONCURRENTLY`. A row
  comparison, `(a, b) > (...)`, is not an index qual. A table a
  statement writes or locks gets no index-only scan.
- **No bitmap scans or backward index scans.** Because rows have
  no page numbers, `BitmapIndexScan` and `BitmapHeapScan` do not
  exist. Descending scans on ascending indexes use an explicit
  sort.
- **Serial execution.** Parallel query workers, parallel index
  builds, and LLVM JIT compilation are removed.
- **Bulk load on `COPY`.** `COPY` into an empty row store table
  loads rows via sorted bulk files directly into RocksDB.

## 6. SQL Surface and Data Types

- **Supported column types.** Columns may use `bool`, `int2`,
  `int4`, `int8`, `float4`, `float8`, `numeric`, `text`,
  `varchar`, `char(n)`, `name`, `bytea`, `date`, `time`,
  `timestamp`, `timestamptz`, `interval`, and `uuid`.
- **Constrained, fixed-size `numeric`.** PG stores `numeric` as a
  variable-length (`varlena`) array of base-10000 digits. DG
  replaces it with two fixed-size binary decimal types matching
  `sm` bit for bit ([`dg/DECIMAL.md`](dg/DECIMAL.md)): 8-byte
  pass-by-value `decimal64` (1 to 17 digits) and 16-byte
  `decimal128` (18 to 36 digits), with no `varlena` header,
  `NaN`, or `Infinity` (unconstrained `numeric` columns default
  to `numeric(17,3)`).
- **`numeric` arithmetic gives an error where PG gives more
  digits** ([`dg/DECIMAL.md`](dg/DECIMAL.md#25-arithmetic) §2.5):
  - `+`, `-`, and `*` of two `decimal64` give a `decimal64`, as
    `int4` with `int4` gives an `int4`. The result is exact, or
    "numeric field overflow" past 17 digits: `9999999999999999.9
    + 0.1` fails. Cast one side to `numeric(18,1)` or wider to
    get a `decimal128`, of 36 digits at most.
  - `/` rounds to the digits its type holds: `70.0 / 70` prints
    16 places, and PG prints 20.
  - `exp`, `ln`, `log`, `power` with a fraction, `stddev`, and
    `variance` of a `numeric` are computed in `double`, to about
    15 digits. A result too small for a `double` is an error:
    `0.1 ^ 1000` fails, and PG gives 0.
  - `sum` fails past 36 digits. PG's never overflows.
  - A literal with an exponent (`1e3`), or with more than 36
    digits, is a `float8`.
  - `int8` and `decimal64` have no common type: `coalesce(n,
    1.5)` fails for an `int8` column `n`.
- **Unsupported column types.** Arrays, JSON/JSONB, XML, ranges,
  geometric types, network types (`inet`, `cidr`, `macaddr`),
  bit strings, `timetz`, `money`, `"char"`, `oid`, `xid`, `tid`,
  `pg_lsn`, and composite/user-defined types cannot be stored as
  table columns. Expressions can use most of them. There is no
  XML: its functions fail, as in a PG built without libxml. The
  full-text search types (`tsvector`, `tsquery`) are removed.
- **ALTER TABLE.** DG takes `ADD COLUMN`, `DROP COLUMN`,
  `ALTER COLUMN TYPE`, `SET`/`DROP NOT NULL`, `SET`/`DROP
  DEFAULT`, and `RENAME`. It takes `ADD` and `DROP CONSTRAINT`
  of a foreign key, a primary key, a unique or a `CHECK`
  constraint, and `VALIDATE CONSTRAINT` of a foreign key or a
  `CHECK`. It refuses `USING INDEX`, `SET SCHEMA`, `ALTER
  INDEX`, and `ALTER TYPE`. `ALTER COLUMN TYPE` refuses a column
  an expression or partial index, a foreign key, or an identity
  sequence uses, a `COLLATE`, and a `USING` that reads another
  column the same statement retypes
  ([`dg/ALTERTYPE.md`](dg/ALTERTYPE.md)).
- **`VALIDATE CONSTRAINT` of a foreign key.** As in
  PostgreSQL, no writer of either table waits for it, nor it
  for them. The flag shows to the session that set it at
  commit, not before ([`dg/UNSUPPORTED.md`](dg/UNSUPPORTED.md),
  "DDL").
- **Sequences.** A sequence takes `START`, `INCREMENT`, and
  `AS`. `MINVALUE`, `MAXVALUE`, `CACHE`, `CYCLE`, `OWNED BY`,
  `ALTER SEQUENCE`, `setval()`, and `TRUNCATE ... RESTART
  IDENTITY` are refused.
- **Queries.** A whole-row reference (`row_to_json(t)`), a
  `RANGE` window frame with an offset, and backward movement of
  a cursor not declared `SCROLL` are refused. `ANALYZE` of a
  column list is refused, and `ANALYZE` skips a temporary
  table. Built-in functions have no default arguments.
- **Removed or refused SQL features.** Table inheritance,
  declarative partitioning, `MERGE`, triggers, rules, row-level
  security, `LISTEN`/`NOTIFY`, materialized views, foreign data
  wrappers, tablespaces, large objects, `EXCLUDE` constraints,
  `DEFERRABLE` constraints, `NOT ENFORCED` and `MATCH PARTIAL`
  foreign keys, unlogged tables, temporary sequences, `ON COMMIT`
  on a permanent table, foreign tables, composite types, domains,
  operators, aggregates, `CREATE DATABASE` options, `ALTER
  DATABASE`, extensions and `LOAD`, and replication
  (`PUBLICATION`, `SUBSCRIPTION`, base backup, PITR, archiving,
  hot standby) are removed or refused
  ([`dg/UNSUPPORTED.md`](dg/UNSUPPORTED.md)).
- **Generated columns.** `CREATE TABLE` takes stored and
  virtual generated columns
  ([`sm/GENERATED.md`](sm/GENERATED.md)). DG refuses `ADD
  COLUMN` of a generated column, `SET` and `DROP EXPRESSION`,
  `ALTER COLUMN TYPE` of a generated column, and a `NOT NULL`
  virtual column ([`dg/UNSUPPORTED.md`](dg/UNSUPPORTED.md)).
- **Added in DG: `DEFAULT ... ON UPDATE`.** Because DG has no
  triggers, it extends column definitions with `DEFAULT expr ON
  UPDATE` ([`dg/ONUPDATE.md`](dg/ONUPDATE.md)), which writes
  `expr` whenever an `UPDATE` does not explicitly assign that
  column.

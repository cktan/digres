# COLSTORE: the Column Store

The Column Store is a second kind of table. It keeps a table's
rows column by column, in immutable files, and answers every
query with a scan. It has no primary key and no secondary index.
Each scan skips the parts of the table that its predicate cannot
match, from statistics kept in RocksDB.

A scan hands out sm2 tuples, so a row of more than 64 KB, long
values included, fails in a scan. [`NEXT.md`](NEXT.md) lists
this and the other parts not built. §16 lists the questions that
are still open.

---

## 1. A worked example

Table `ev` is a column store table with three columns: `ts
timestamptz`, `kind text`, `amount int4`.

```sql
INSERT INTO ev VALUES (now(), 'click', 3);
    -- _id 1 is drawn. The row goes to the delta store, a
    -- keyless row store in RocksDB: delta key _id 1.

-- ... 9,999 more inserts: _id 2 .. 10000 ...
    -- The delta store now holds 10k rows: the seal runs.

-- The seal, a transaction of its own:
    -- 1. Erase delta rows 1 .. 10000 with sealed tombstones.
    -- 2. Write three CG files, one for each column, and the
    --    _id CG: version 0 of RG 1, fsid 4 (1 << 2 | 0).
    --    Fsync them.
    -- 3. Put the RG key of fsid 4: the files, and each CG's
    --    min, max, null count and more. Put a filter key for
    --    each CG: a bloom filter or an exact set.
    -- 4. Commit.

UPDATE ev SET amount = 4 WHERE _id = 900;
    -- Row 900 is at rno 899 of fsid 4, its place. Two writes:
    --   a delete marker at (fsid 4, rno 899)
    --   a new version at delta key _id 900
    -- The row keeps _id 900.

DELETE FROM ev WHERE _id <= 2000;
    -- 1,999 more markers, and the delta row 900 deleted: a
    -- fifth of the RG is dead. The sealer compacts RG 1 into
    -- version 1, fsid 5: its 8,000 live rows, renumbered from
    -- rno 0. A snapshot older than
    -- the compaction still reads fsid 4 and its markers.

SELECT sum(amount) FROM ev WHERE kind = 'view';
    -- 1. Read the RG keys at the snapshot: fsid 5.
    -- 2. 'view' is between kind's min and max. Read kind's
    --    filter key: 'view' is not in the exact set. Skip the
    --    RG. No file is read.
    -- 3. Scan the delta store.
```

---

## 2. Terms

| Term | Meaning |
| --- | --- |
| row group (RG) | a set of at most 10k rows of one table, sealed together; the unit of pruning and of a scan's batch |
| column group (CG) | the values of one column in one RG, as an array; one immutable file |
| delta store | the table's keyless row store in RocksDB; new rows and new versions of rows go there first |
| seal | the transaction that moves rows from the delta store into a new RG |
| sealed tombstone | the delta tombstone that the seal writes; a writer sees through it (§10.2) |
| version | one of the four file sets of an RG: 0 from the seal or a bulk load, 1 to 3 from RG compaction; a snapshot sees one of them |
| `fsid` | the file set id of a version: `rg_id << 2 \| version` |
| `rno` | a row's position in a version; a new version renumbers its rows |
| place | `(fsid, rno)`: a row's name in a version of an RG; `fsid` 0 for the delta store |
| dropped set (D) | the `rno`s of version 0 that a version does not hold, as a bitmap, in the version's RG key |
| delete marker | a versioned key `(fsid, rno)` that says the version's row `rno` is deleted |
| shadow marker | a delete marker at an older place of a row, which its writer's own snapshot reads |
| validmap | a versioned key of a version; bit `rno` set means the row is deleted |
| fold | the job that moves a version's markers into its validmap |
| RG lock | a lock-table name of an RG, which writers hold IX and the sealer's jobs X on |
| moved-in version | a delta row that elimination writes, from an RG; a writer sees through it (§11.3) |
| cleanup | the job that erases an old version's markers and validmap, once no snapshot sees it |
| RG key | the versioned key that holds a version's metadata and the statistics of its CGs |
| filter key | the versioned key that holds one CG's bloom filter or exact set |
| zonemap | a CG's statistics in the RG key: min, max, null count and flags |
| key bytes | a datum in its type's order-preserving key encoding (`type_t::key_encode`), of a varlena's payload past its header; memcmp order is SQL order |
| RG compaction | the rewrite of an RG with many dead rows into its next version |
| elimination | the move of an RG's live rows into the delta store, as moved-in versions |
| the longest value | `col_max_value`, 2,048 bytes: the most a varlena value of a column store table may hold, its header not counted |

---

## 3. Decisions

| Decision | Choice | Section |
| --- | --- | --- |
| where a CG's bytes live | one immutable file for each CG, outside RocksDB; on a local file system, later in S3 with a local file cache | §4 |
| where metadata lives | RocksDB, in the table's one store, on local disk | §4 |
| the write path of small writes | the delta store, a keyless row store in RocksDB | §9 |
| the write path of a bulk load | RGs written directly in the statement's transaction; the rows past the last full RG go to the delta store | §9.2 |
| row identity | `_id`, drawn at insert, kept for the row's life, stored as a hidden CG | §8 |
| a lookup by `_id` | a scan for `_id = n`, pruned by each RG's `_id` min and max; no map from `_id` to `rg_id` | §8 |
| the TID that digres carries | `_id`; a write takes the row's place from the scan on it | §8 |
| locks | row locks on delta key `_id`; IX on the RG for a writer, X for the sealer's jobs | §9.1 |
| isolation | READ COMMITTED and REPEATABLE READ; a SERIALIZABLE transaction that reads or writes a column store table fails | §9.1 |
| how digres makes one | `CREATE TABLE ev (...) WITH (storage = columnar)`; sm3's catalog records the kind | §3 |
| the executor | PostgreSQL's, a row at a time, until the Column Store is stable; then an executor for columnar operations | §3 |
| `relpages` | the bytes of all CGs and of the delta store, divided by 8 KB | §7 |
| an update or delete of a row in an RG | a delete marker at the row's newest place, plus a reinsert into the delta store for an update | §9 |
| deleted rows | a marker for each, folded into the version's validmap, a plain bitmap, at 1% of its rows | §11.1 |
| metadata keys | one RG key for each version; one filter key for each CG | §4.3 |
| pruning statistics | in RocksDB and in each file | §6 |
| min and max | key bytes, cut to 32 bytes | §6.1 |
| dictionary | for each CG, sorted in key order | §5.2 |
| encodings | plain, dictionary, delta for sorted integers; zstd on each CG | §5 |
| the longest value | 2,048 bytes; a longer one is refused at its write, and goes into a row store table that a query joins | §5.4 |
| RG size | 10k rows | §7 |
| the seal | pessimistic; its tombstones are transparent to first-committer-wins | §10 |
| RG compaction | at 20% dead rows, the next version of the same RG, at most 3; then elimination, whose moved-in versions are transparent in the same way | §11 |

The constants (10k rows, 2,048 bytes, 32 bytes, 4 KB, 20%, 50%)
may change. An RG key records the real sizes of its CGs,
so a change of a constant does not make an existing file wrong.
A test can give a catalog smaller RGs (`sm3::cat_set_rg_rows`),
so that a seal, an RG compaction and an elimination come after a
few hundred rows. The 20% and 50% are of the RG size set.

**Which level owns each part:**

| Part | Level | Why |
| --- | --- | --- |
| the sealed tombstone, and the moved-in version's rule in first-committer-wins | sm1 | rules of the write path; sm1 knows no table |
| file sets: the file I/O of CGs, their key class, their GC and the sweep at open; later S3 and the local file cache | sm1, beside the Buffer Manager | sm1 owns the I/O of every byte, the order of fsync and RG key, and GC (§4.5) |
| the CG file format, the encodings, the dictionary, the zonemap and the filter, and the pruning test on key bytes | sm2 | they need types and their key encodings; the pruning test compares key bytes and knows no type |
| the RG keys, the seal, the fold, RG compaction, elimination, the cleanup, the scan, the lookup by `_id`, and the locks of §9.1 | sm3 | they need the table, its columns and predicates, as the `tbl_*` calls of a row store table do |

---

## 4. Where bytes live

### 4.1 Files

- **One file for each CG.** A file is written once and never
  changed, renamed, appended to or overwritten.
- **A file's path is `colstore/<storeid>/<fsid /
  1024>/<fsid>.<name>`**, under the RocksDB directory of the
  database, or of the temporary instance for a temporary table.
  `<name>` is the column's attnum in the Catalog of sm3, which a
  column keeps for good, and which a dropped column does not
  give back ([`CATALOG.md`](CATALOG.md)). The `_id` CG's name is
  `id`.
  `rg_id`s come from a sequence of the table and are never used
  again, and each version of an RG has an `fsid` of its own. So a
  path is unique for the life of the database. S3's keys will
  take the same path.
- **A CG is never replaced under its `fsid`.** RG compaction
  writes every CG of the next version, under the next `fsid`
  (§11.2). So an RG key is put once and erased once.
- **A directory holds the files of 1024 RGs**, about 30k files
  for 30 columns. A table of 1B rows has about 3M files.
  `DROP TABLE` removes the store's directory tree.
- **A file is durable before anything refers to it.** The writer
  syncs the file (`fdatasync`) and its directories, then puts
  the RG key that
  names it.
- **A file holds no transaction state.** It has no xid and no
  version. Its RG key decides which snapshots see it.

### 4.2 S3 and the local file cache

Files will move to S3. Local disk then holds a cache of them.

- **A cached file never becomes stale**, as a file never
  changes. The cache needs eviction and no invalidation.
- **RocksDB stays on local disk**: the delta store, the
  markers, the RG keys and the filter keys. A decision about
  commit, visibility or GC never needs an S3 listing.
- **A scan reads S3 only for a CG that it must read.** Every
  statistic that prunes is in RocksDB (§6).
- **The cache's size, its eviction and its read-through from S3
  are decided with the work on S3.** On a local file system the
  files are the data, and nothing is cached.

### 4.3 Keys in the table's store

A column store table has one sm1 store, as every table has
([`CATALOG.md`](CATALOG.md) §2). It has no indexes, so it uses
the relids of indexes for its own keys. Its RG keys are file
set keys of sm1 (§4.5), in a key class of their own:

| Class or relid | What | Key in RocksDB | Kind |
| --- | --- | --- | --- |
| relid `0x0001` | delta rows | `0x01 ‖ 0x0001 ‖ be64(_id)` | chained: the keyless row store as it is |
| relid `0x0002` | delete markers | `0x00 ‖ 0x0002 ‖ be64(fsid) ‖ be32(rno) ‖ ~wts ‖ ~seq` | versioned |
| relid `0x0003` | validmaps | `0x00 ‖ 0x0003 ‖ be64(fsid) ‖ ~wts ‖ ~seq` | versioned |
| relid `0x0004` | filter keys | `0x00 ‖ 0x0004 ‖ be64(fsid) ‖ be16(attnum + 1) ‖ ~wts ‖ ~seq`; the `_id` CG's is 0 | versioned |
| relid `0x0005` | RG lock names | `0x00 ‖ 0x0005 ‖ be64(rg_id)`; never written | a lock-table name only |
| relid `0xFFFF` | TOAST of delta rows | as in a row store table | versioned |
| class `0x05` | RG keys: sm1's file set keys | `0x05 ‖ be64(fsid) ‖ ~wts ‖ ~seq` | versioned, of sm1's file set class |

- **RG keys and filter keys are apart**, so a scan of RG keys
  reads no filter key.
- **The class is `0x05`, past the pages' `0x04`.** sm1 takes
  every key below `0x04` for a row version: a store's emptiness,
  a rollback's range delete of a load, and the newest OLD id of
  a chained store all read that range. A file set key must stay
  out of it.
- **A snapshot's set of RGs is a scan of class `0x05` at the
  snapshot.** It needs no visibility rule of its own.
- **An RG key and its filter keys are put once and erased
  once.** The seal, a bulk load or RG compaction puts them. RG
  compaction erases them. Nothing changes them between.
  `TRUNCATE` and `DROP TABLE` erase no key: the table's old
  store goes, with its directory of files (§14).
- **A marker names its version.** A version renumbers its rows:
  `rno` 899 of version 1 is not `rno` 899 of version 0. A scan of
  a version reads the markers of its own `fsid` alone.
- **The value of a marker is empty.** The value of a validmap is
  a plain bitmap of the version's row count; a version with no
  validmap key has no bit set. It is mostly zeros, which
  RocksDB's block compression makes nearly free.
- **The RG lock's key is a lock-table name only**
  (`sm1::lock_row` with relid `0x0005`). One name covers every
  version of the RG. No key of data has this name, so a lock on
  it waits for no writer of a key.
- **The newest version of an RG** is the one RG key of its
  `fsid`s that is live at the newest state. No key means the RG
  is eliminated.

### 4.4 The value of an RG key

| Field | For each |
| --- | --- |
| format version | RG |
| row count | RG |
| `_id` min and max | RG |
| version 0's row count, and the dropped set D, a bitmap of it | RG, in versions 1 to 3 |
| the count of CGs | RG |
| attnum | CG |
| file size | CG |
| CRC32C of the file | CG |
| zonemap (§6.1) | CG |

The encoding of a CG is in the CG file's tail (§5.3), not in the
RG key.

The value is about 50 bytes for each column. A table of 30
columns has RG keys of about 1.5 KB.

### 4.5 File sets

A file set is sm1's: a group of immutable files, and the
versioned key that names them. sm1 knows no RG. A version of an
RG of sm3 is one file set, with `fsid` `rg_id << 2 | version`, as
`sm1::fileset_put` takes an `fsid` once. An RG has at most four
versions: 0 from the seal or a bulk load, 1 to 3 from RG
compaction (§11.2). The RG key's value
(§4.4) is the file set's payload, which sm1 does not read.

sm1 does the whole life of a file:

1. Write the files of a file set, and fsync them and their
   directory.
2. Put the file set key in the caller's transaction, with the
   payload.
3. Unlink the files at an abort or a rollback to a savepoint,
   from the transaction's list (§13).
4. Erase the file set key in the caller's transaction.
5. Delete the files in GC, when no version of the key is
   committed and visible to a snapshot, or prepared (§13).
6. At open, scan class `0x05` of each store, and delete the
   files of each `fsid` whose newest committed or prepared
   version is not a put (§13).

- **sm3 never uses a file path.** It reads a CG through sm1, by
  `fsid` and the file's name (`id`, `3`).
- **S3 and the local file cache are sm1's too.** The levels
  above see no change when files move to S3.
- **The key class is sm1's, so the compaction filter and the GC
  worker know it.** They need no code of sm3.

---

## 5. A CG file

### 5.1 Encodings

| Encoding | For |
| --- | --- |
| plain | every type; the fallback |
| dictionary | where it is smaller than plain (§5.2) |
| delta, bit-packed | `int2`, `int4` and `int8` in ascending order, with no null: the `_id` CG of a sealed or loaded RG |

**A CG holds every value of its column, and no TOAST pointer.**
A varlena value is at most 2,048 bytes (§5.4).

After the encoding, the CG is compressed with zstd. The tail
tags the encoding, so a new encoding does not change the format
of an existing file.

The rows of an RG that `rewrite_table` wrote (§9.2) may not be in
`_id` order, so its `_id` CG then gets plain encoding.

### 5.2 The dictionary

- **Each CG has its own dictionary**, in the file. It never
  changes, and it needs no MVCC, no lock and no GC.
- **The dictionary holds datums, sorted in key order.** The codes
  keep the order of the values.
- **The values are bit-packed codes.**
- **The writer of the CG chooses.** It uses a dictionary if the
  dictionary and its codes are smaller than plain.
- **A read expands each code to its datum.** digres evaluates a
  predicate on each row's datums. A predicate evaluated on codes
  waits for the columnar executor.

### 5.3 The layout of a file

A CG file holds, in this order (`sm2/src/col/cg.cc`):

1. The body, compressed with zstd: the encoding, the row count,
   a flags byte that says which bitmaps follow, a null bitmap
   and a long bitmap where a row needs them, then
   the values as the encoding lays them out, the dictionary
   first in a CG with one (§5.1, §5.2).
2. The zonemap (§6.1).
3. The filter: the exact set or the bloom filter (§6.3).
4. A tail of 38 bytes: a magic number, a format number, the
   encoding, a flags byte (0), the row count, four sizes (the
   compressed body, the body before compression, the zonemap and
   the filter), the CRC32C of the file before it, and the magic
   number again.

- **A file describes itself without RocksDB.** The tail is read
  with one small read, or one ranged GET from S3.
- **The RG key also holds each file's CRC32C** (§4.4). RocksDB
  is the authority, so the key says what the file must be.
- **Each read of a CG checks it**, from disk now, from S3
  later. sm1's `fileset_read` checks no CRC32C; it fails with
  `SM_ERR_CORRUPT` for a missing file, or one shorter than the
  range read. Its callers check the bytes:
  - sm3 compares the file's size and CRC32C with the RG key's.
  - sm2's `cg_open` checks the tail's CRC32C.

  It costs one pass at memory speed, which is far less than
  zstd's decompression.
- **A mismatch fails the scan** with a data corruption error.
  Nothing repairs the file.
- **An upload to S3 carries the same CRC32C** in S3's checksum
  header, `x-amz-checksum-crc32c`, so S3 checks the upload from
  end to end.

### 5.4 The longest value

**A varlena value of a column store table is at most 2,048
bytes**, its header not counted (`col_max_value` in
`sm3/src/colstore/colstore.h`). A longer one is refused at its
write with `SM_ERR_VALUETOOLONG`, which digres reports as
SQLSTATE `54000`, program limit exceeded. A user keeps a longer
value, a document or a long URL, in a row store table, and joins
it.

- **Every write checks it** (`need_values_fit` in
  `sm3/src/table/table.cc`): an insert, an update, an update of a
  row of an RG, a bulk load's row, a rewrite's row
  (`rewrite_table`), and an `ADD COLUMN`'s missing value. A TOAST
  pointer names a value of the table's own, checked when it was
  written.
- **So every value is in its CG.** A CG has no pointer to
  another file, and an RG's file set holds one file for each
  column. A scan reads a value with its CG.
- **The delta store still keeps a row past 2,000 bytes in
  TOAST**, as a row store table does. The seal reads each value
  out of line whole, and erases the chunks (§10.1).
- **A row of many long values can still pass 64 KB.** A scan
  hands out each row as an sm2 tuple, of 64 KB at most, so a row
  of more than 31 values near 2,048 bytes fails in a scan
  ([`NEXT.md`](NEXT.md)).
- **The limit can change.** It is one constant, and no file
  records it.

---

## 6. Statistics and pruning

### 6.1 The zonemap of a CG

| Statistic | Encoding | Prunes |
| --- | --- | --- |
| min, max | key bytes; finite values only for the types of `has_nan` and `has_*_inf` | `=`, `<`, `<=`, `>`, `>=`, `BETWEEN`, `IN` |
| `min_cut`, `max_cut` | bits: min, or max, cut to 32 bytes; a cut min is still a lower bound; a cut max compares as its prefix's successor | keeps a long `text` from making the RG key large |
| null count | `uint32`; has-null is `> 0`, all-null is `= row count` | `IS NULL`, `IS NOT NULL`, every strict predicate on an all-null CG |
| distinct count | `uint32`, exact when the CG is written | nothing reads it yet |
| `has_nan` | bit; `float4`, `float8` | `x = 'NaN'`, `x <> 'NaN'`, and ranges with finite min and max |
| `has_pos_inf`, `has_neg_inf` | bits; the types of `has_nan`, and `date`, `timestamp`, `timestamptz`, `interval` | `x = 'infinity'`, and ranges with finite min and max |
| `sorted` | bit: the CG is in ascending key order | nothing reads it yet |
| collation version | for each collated column | turns off pruning by value for a CG whose version is not the column's |

These follow and need no field:

- **A CG of one value** has `min = max` and a null count of 0.
- **`count(*)` of an RG** is its row count less its visible
  delete markers.

### 6.2 The rules of pruning

- **A range test with NaN and infinity.** In sm2's key order,
  NaN is above `+infinity`, as in PostgreSQL. So `x > c` can
  skip a CG if and only if `max <= c`, `has_nan` is clear and
  `has_pos_inf` is clear. The other comparisons follow by
  symmetry.
- **A collation version that is not the column's turns off
  pruning by value for the CG** (`sm2::cg_may_match`). The min
  and max of `text` begin with the collation's sort key, and a
  new ICU version can change it. Without this rule, a scan would
  skip rows that match. The null count still prunes.

### 6.3 The filter of a CG

Each CG has one filter key: an exact set, a bloom filter, or
none, for a CG of nulls alone, which no equality matches.

- **Every value is in the filter**, by its key. A `char(n)` key
  leaves out the trailing spaces, so `'a'` padded to 2,000 bytes
  has the key `'a'`, which an equality on `'a'` finds.
- **An exact set** when the key bytes of the CG's distinct
  values total 4 KB or less: a sorted list of those key bytes,
  apart from any dictionary. It answers `=` and `IN` with no
  false positive.
- **A bloom filter** otherwise: 10 bits for each distinct value,
  for 1% false positives, with 7 probes. The hash of the key
  bytes is FNV-1a, then the finalizer of splitmix64; it is part
  of the file format. At 10k rows the filter is at most
  12.5 KB.
- **Under a non-deterministic collation, equal strings share a
  key**: the key is the sort key alone, with no original string
  after it. So `'a' = 'A'` under a case-insensitive collation
  hashes the same.
- **Every column has a filter.** There is no option and no GUC.

---

## 7. The size of an RG

**An RG holds 10k rows.** The seal and a bulk load make only
RGs of 10k rows:

- The seal takes 10k rows, or undoes itself (§10.1).
- A bulk load writes full RGs, and sends the rows past the last
  one to the delta store (§9.2).

Only RG compaction makes a version of fewer rows: the live rows
of the version that it rewrites (§11.2).

- **The size of a CG needs no limit.** A value of a CG is at
  most 2,048 bytes (§5.4). So a CG holds at most about 20 MB
  before encoding. If the row limit or the value's limit is
  raised, a limit on a CG's size comes back with it.
- **An RG is a scan's unit**, as a page is to PostgreSQL's
  `heapgetpage()`: a scan reads an RG and applies its markers.
  It is also the unit of `ANALYZE`'s sample.
- **An RG is not a page of the cost model.** It holds 10k rows,
  and a page of PostgreSQL holds about 100. Counted as pages,
  RGs would make a scan look 100 times cheaper than it is, and
  a nested loop over the table look almost free. So `relpages`
  is the bytes of all the CGs and of the delta store, divided
  by 8 KB. `reltuples` is the row counts of the RG keys plus the
  delta store's. Neither needs a file read.
- **The cost of a scan does not see the projection.** A scan of
  2 of 30 columns costs as a scan of all of them. A cost model
  that sees it waits for the columnar executor.

---

## 8. Row identity

- **A row's name is its `_id`**, a 64-bit number from the
  table's `_id` sequence, as in a keyless row store table
  ([`ROWSTORE.md`](ROWSTORE.md) §2.3).
- **An update keeps the `_id`.** So "the newest version of
  `_id` 900" is one question with one answer, wherever the row
  is.
- **Each RG stores its rows' `_id`s as a hidden CG.** It is
  sorted and delta-encoded in an RG of the seal or a bulk load,
  and RG compaction keeps its order. In an RG that
  `rewrite_table` wrote (§9.2), it may not be sorted, and is
  plain.
- **The `_id` CG is like any other CG.** It has a zonemap and a
  filter key (§6), and nothing more. There is no map from `_id`
  to `rg_id`.
- **The `_id` ranges of RGs can overlap.** An updated row has a
  dead copy in an old RG, and a later seal puts its new version
  into a new RG. The validmaps and markers decide which copy is
  live.
- **A row's place is its position in a version**, `(fsid,
  rno)`. It is not stable: RG compaction renumbers a version's
  rows, and elimination moves them into the delta store. The
  `_id` is stable, and names the row for its life.

A lookup of `_id` is a scan with the predicate `_id = n`:

1. Read the RG keys at the snapshot.
2. Skip each RG whose `_id` min and max do not hold `n`.
3. Read the validmap and the delete markers of each version that
   is left: its dead `rno`s (`col_dead`).
4. Read the version's `_id` CG, and walk it row by row to `n`. A
   copy at a dead `rno` is dead. A read of the row (`col_read`)
   reads every CG of the version that holds it.
5. Read the delta key `n`.

- **Steps 1 and 2 read RocksDB alone.** The `_id` filter is not
  read: `col_find_latest` and `col_read` pass no bound.
- **The read of the RG keys reads every RG key**, so a lookup
  costs in proportion to the RGs of the table. A lookup that
  reads the `_id` filter, finds `n` by a binary search, and
  reads one marker is in [`NEXT.md`](NEXT.md).

**digres carries the `_id` alone**, as for a keyless row store
table. A write takes the row's place from the scan instead:

1. If the scan that read the row is still on it, the write names
   the row `(place, _id)`, with the scan's place
   (`sm_row_scan_place`): no lookup. A plain `UPDATE ... WHERE`
   or `DELETE ... WHERE` is so.
2. Else, as after a join, a sort or a TID scan, the write names
   the row by its `_id`, and sm reads the delta key `n`. If the
   delta store has no version of it, or a seal's tombstone, sm
   looks up the row's place as above, at the newest committed
   state (`col_find_latest`). No row there, of a row the
   transaction sees, is a row changed since: the write fails as
   over another's marker (§9 step 6), `SM_ERR_CONFLICT` above READ
   COMMITTED.
3. A follow after `SM_ERR_SEALED` is a lookup too.

- **The place's `fsid` is 0 for a row in the delta store.**
- **The place is the one the scan's snapshot sees.** The write
  maps it to the newest version (§8.1, §9).
- **A read by `_id` is the same**: the delta store, then the
  lookup, at the snapshot, or newest for `sm_tbl_read_latest`. A
  seal's tombstone, read newest (`SM_ERR_SEALED`), sends the read to
  the lookup too.
- **A row lock is on the delta key `_id`** wherever the row is
  (§9.1), so it needs no place, and a renumbering moves no lock.

| Call | What |
| --- | --- |
| `sm_row_place_t sm_row_scan_place(const sm_row_scan_t*)` | the place of the row the scan handed out last |
| `int sm_tbl_delete_at(sm_tbl_t*, sm_row_place_t, uint64_t id)` | a delete by name |
| `int sm_tbl_update_at(sm_tbl_t*, sm_row_place_t, uint64_t id, values, isnull, n)` | an update by name |

### 8.1 The map of an `rno` to a newer version

Each version `k` of 1 to 3 keeps `D_k`, the `rno`s of version 0
that it does not hold. A version holds the rows of version 0
that are not in its D, in their order. So, for an `rno` of
version `i` and a newer version `k` (`rg_map_rno`):

1. Map `rno_i` to version 0: the `rno_i`-th `rno` of version 0
   not in `D_i`. For version 0 itself, it is `rno_i`.
2. If that `rno_0` is in `D_k`, the row is not in version `k`.
3. Else `rno_k` is `rno_0` less the count of `D_k`'s bits below
   `rno_0`.

The writer reads `D_i` from the RG key its snapshot sees, and
`D_k` from the newest. Each is 1,250 bytes for 10k rows. The map
costs a popcount over each.

---

## 9. Writes

| Statement | The row is in the delta store | The row is in a version of an RG |
| --- | --- | --- |
| `INSERT` | a new `_id`; `insert_new` at its delta key | — |
| `DELETE` | a tombstone at its delta key | a delete marker at the row's newest place |
| `UPDATE` | a new version at its delta key | a delete marker at the row's newest place, and the new version at delta key `_id` |

**Every lock and check stays at the row.** A write of a row of
an RG, at the place its snapshot sees (`mark_deleted` in
`sm3/src/table/table.cc`):

1. Take the row's exclusive lock on its delta key `_id`, as any
   write does. A second writer, or a `FOR UPDATE` holder, is
   waited for.
2. Take IX on the RG (§9.1). It waits while the fold, RG
   compaction or elimination holds X, and keeps them out to the
   commit.
3. Read the RG's newest version at the newest state
   (`newest_place`):
   1. It is the version the snapshot sees: the newest place is
      the snapshot's place.
   2. It is newer, and the snapshot sees the place's version: map
      the `rno` (§8.1). A row in the newest version's dropped
      set was deleted or updated since: the write fails as over
      another's marker (step 6).
   3. It is newer, and the snapshot does not see the place's
      version, as for a place an earlier transaction read: find
      the row by its `_id`, at the newest state.
   4. The RG has no version: it is eliminated. Write the row at
      its delta key, as for a row of the delta store, where it is
      a moved-in version or a newer one (§11.3).
4. Read the newest version's validmap at the newest state. A bit
   set means the row was deleted or updated, and folded: the write
   fails as over another's marker (step 6), `SM_ERR_CONFLICT` above
   READ COMMITTED, `SM_ERR_UPDATED` or `SM_ERR_NOTFOUND` at READ
   COMMITTED. A bit the snapshot sees, above READ COMMITTED, is
   `SM_ERR_NOTFOUND`.
5. Insert the delete marker at the newest place, a versioned key,
   as a unique index entry is. First-committer-wins applies to it
   as to any key. If the newest place is not the snapshot's,
   insert a shadow marker at the snapshot's place too.
6. A marker there already: if the running statement put it, the
   statement changed the row already: the marker's lock, taken as
   a row read, fails with `SM_ERR_SELFMODIFIED`, which digres
   skips, as for a row store table. Otherwise, at READ COMMITTED,
   the row was deleted since (`SM_ERR_NOTFOUND`), or updated
   since, with its new version in the delta store
   (`SM_ERR_UPDATED`, which the caller answers by a read of the
   newest version).

- **First-committer-wins judges at the newest place.** Two
  writers of one row, of any snapshots, meet at one marker key.
- **A shadow marker is for the writer's own reads.** It commits
  after every snapshot that sees its version, except the
  writer's. So no other transaction sees it. It needs no lock: X
  on the row's `_id` covers it.
- **A write by `_id` alone hides the row too.** After it writes
  the row at its newest place, or over a moved-in version that
  its snapshot does not see (`sm1::wrote_over_moved_in`), it
  finds the place its snapshot sees the row at. A place other
  than the one it wrote gets a shadow marker (`shadow_seen`).
- **A delete older than the seal hides the delta row.** A writer
  whose snapshot is older than the seal of the row reads the row
  in the delta store, live, and deletes it by its marker in
  version 0, which its snapshot cannot see. So it puts a tombstone
  of its own over the seal's (`sm2::rel_hide_sealed`): an empty
  version, which an insert may put over a sealed tombstone, and
  its erase. Another transaction sees a plain tombstone over the
  seal's, and the marker in version 0. An update needs none: its
  new version at the delta key hides the old one.
- **An update of a row in an RG writes two keys in one
  transaction.** The delta key may hold a sealed tombstone. The
  insert goes in over it, as over any tombstone.
- **A savepoint rollback undoes markers and delta writes** with
  the undo list, as for any key.
- **A wide update copies whole rows into the delta store.** The
  seal later puts them into new RGs.

### 9.1 Locks

Rows move: the seal moves them from the delta store into an RG,
RG compaction renumbers them in a new version, and elimination
moves them back into the delta store. A lock on a location misses
a write after a move. So every row lock names the `_id`, in the
key space of the delta store, `0x0001 ‖ be64(_id)`. That key
space is `_id` space.

**Row locks.** `FOR UPDATE` and `FOR SHARE` lock delta key
`_id` with `sm2::rel_lock`, which is `sm1::chain_lock_row` on the
table's chained store, wherever the row is. A move never
takes a lock's row away.

**RG locks.** A writer of a row of an RG takes IX on the RG's
lock name (§4.3). The jobs of the sealer that change an RG take
X, NOWAIT, and give up on a conflict. The lock modes are sm1's
([`LOCKMGR.md`](LOCKMGR.md) §1.4).

| Action | Table | RG | Row |
| --- | --- | --- | --- |
| `INSERT` | IX | none | none: the `_id` is new |
| `DELETE`, `UPDATE` of a row of an RG | IX | IX | X on delta key `_id` |
| `DELETE`, `UPDATE` of a delta row | IX | none | X on delta key `_id` |
| `FOR UPDATE`, `FOR SHARE` | IX, IS | none | X, S on delta key `_id` |
| plain `SELECT` | IS | none | none |
| the seal | IX | none | a try of X on each row's delta key; a holder's row is passed over |
| the fold, RG compaction | IX | X, NOWAIT | none |
| elimination | IX | X, NOWAIT | a try of X on each row's delta key; a holder makes it give up |
| the cleanup | IX | none | none |

- **NOWAIT keeps the sealer out of deadlocks.** A writer takes
  only IX at the RG, and IX is compatible with IX. The sealer's
  jobs never wait.
- **A writer checks the newest version after IX.** A writer can
  wait for IX while RG compaction or elimination holds X, and get
  it after that commits. So after IX, it reads the RG's newest
  version (§9 step 3).
- **A writer checks the validmap's bit after IX.** The fold
  erases a marker after it sets the bit. A writer that came to
  the row by its `_id`, after a join or a sort, could insert a
  marker over that erase and act on a row that is gone. The
  writer holds IX, so no fold runs between its check and its
  insert.
- **A prepared transaction keeps its IX**, so the sealer's jobs
  give up on its RG until `COMMIT PREPARED` or `ROLLBACK
  PREPARED`.

**No SERIALIZABLE transaction uses a column store table.** A
read or a write of one in a SERIALIZABLE transaction fails with
`SM_ERR_UNSUPPORTED`, and digres reports it as a feature not
supported, SQLSTATE `0A000`. A column store table is for
analytics, which runs at READ COMMITTED or REPEATABLE READ. The
refusal frees the Column Store from SSI:

- A scan takes no SIREAD lock, so pruning, the merge of delete
  markers, and a later batch interface need no lock rule.
- A delete of a row in an RG, which writes a marker and no
  delta key, needs no report to SSI of the delta key.
- A bulk load needs no report of the `_id`s of its RGs.
- A move of a row never meets a SIREAD lock.

### 9.2 A bulk load

`COPY`, `CREATE TABLE AS`, `INSERT ... SELECT` and
`rewrite_table` write RGs directly, in the transaction of the
statement. They do not pass through the delta store.

1. Draw each row's `_id` from the table's sequence, one at a
   time, as sm1 has no draw of a batch. `rewrite_table`
   (`copy_rows`) keeps each row's old `_id` instead.
2. Add 10k rows to the RG being built, in memory.
3. Write the CG files, and put the file set key and the filter
   keys of a new RG (§4.5).
4. Repeat until fewer than 10k rows are left.
5. Insert the rows that are left into the delta store, as
   `INSERT` does. A later seal takes them.

- **A table with rows can take a bulk load.** A new RG
  conflicts with no key that is there, so the load takes no
  lock beyond what an `INSERT` takes.
- **An RG's `_id`s ascend**, as they come from the sequence in
  the load's order. So its `_id` range is tight. An RG of
  `rewrite_table` holds old `_id`s, in the order it reads the
  old RGs and then the delta store.
- **An abort or a rollback to a savepoint** unlinks the files,
  from the transaction's list, and the undo list removes the
  keys (§13).
- **The memory is one RG**: 10k rows, of values of 2,048 bytes
  at most, until the RG is written.

---

## 10. The seal

### 10.1 The steps

The seal moves rows from the delta store into a new RG. It is a
transaction of its own.

1. Read the delta store's `_id`s in order. With fewer than 10k,
   stop: there is nothing to seal.
2. Set a savepoint.
3. For each row, until 10k are taken (§7):
   1. Try the row's exclusive lock. If another transaction holds
      a lock on the row, a reader's `FOR SHARE` included, skip
      the row. It stays in the delta store, and a later seal
      takes it.
   2. Erase the row with a sealed tombstone. The erase gives
      back the exact row it replaced. A row changed or gone
      since step 1 is skipped.
   3. Add the row to the RG being built. A value out of line in
      TOAST is read back whole.
4. With fewer than 10k rows taken, roll back to the savepoint:
   no partial RG.
5. Draw a new `rg_id`. Write each CG's file of its version 0,
   `fsid` `rg_id << 2`, and fsync the files and the directory.
6. Put the RG key, and a filter key for each CG. The new version
   has no validmap.
7. Erase the TOAST chunks of the rows taken, at once. A
   snapshot older than the seal's commit still sees them.
8. Commit.

- **The seal is pessimistic.** Its tombstones lock the rows
  from their erase until the commit. A writer of one of those
  rows waits for that time.
- **A snapshot sees each row once.** A snapshot older than the
  seal's commit sees the delta rows and no RG key. A newer one
  sees the sealed tombstones and the RG key. One commit changes
  both.
- **An abort of the seal leaves files that no RG key names.**
  They are orphans (§13).

### 10.2 The sealed tombstone

The seal does not change a row's content. So it must not fail a
user's transaction. PostgreSQL does not fail a transaction
because VACUUM ran, and the seal keeps to the same rule.

A sealed tombstone is a tombstone with a new flag, `0x40`,
`version_sealed`. The flags `0x01` to `0x08` of a version, and
`0x10` and `0x20` of an OLD entry, are in use, and a CURR
version keeps its flags when it moves to OLD. In the write path
([`ROWSTORE.md`](ROWSTORE.md) §4):

- **A reader** sees it as any tombstone. The row is in the RG.
- **First-committer-wins sees through it.** The check of a
  newer commit, step 8 of the write path, judges the write
  against the version under it, the row's own commit, in
  `prev`. It does not judge against the seal's commit.
- **A writer that passes that check gets `SM_ERR_SEALED`.** The
  Column Store then finds `_id` in the RGs, and writes the
  delete marker, and the new version for an update.
- **At READ COMMITTED**, a writer that waited on the seal finds
  the committed sealed tombstone, and follows `_id` in the
  same way.
- **An insert** goes in over a sealed tombstone, as over any
  tombstone.
- **The Garbage Collector** treats it as a moved tombstone
  ([`ROWSTORE.md`](ROWSTORE.md) §2.5).

An example at REPEATABLE READ:

```sql
-- Session U, snapshot 100. Row 900 committed at 80.
BEGIN ISOLATION LEVEL REPEATABLE READ;
SELECT count(*) FROM ev;
-- The seal commits at 105: delta rows 1 .. 10000 go to RG 1.
UPDATE ev SET amount = 4 WHERE _id = 900;
    -- Delta key 900: a sealed tombstone at 105. Under it, the
    -- version committed at 80 <= 100: no conflict.
    -- SM_ERR_SEALED: find _id 900 in RG 1.
    -- Write the marker (rg 1, _id 900) and delta key 900.
```

### 10.3 Isolation of the seal

**The seal runs at READ COMMITTED.** No SERIALIZABLE transaction
uses the table (§9.1), and a writer below SERIALIZABLE takes no
part in SSI ([`XCTMGR.md`](XCTMGR.md) §7.5). So the seal makes
no rw-edge.

### 10.4 When the seal runs

- **A seal job runs on the sealer of sm3**
  (`sm3/src/colstore/sealer.cc`): one worker thread for each
  catalog, with a queue of jobs, as the btree reaper has its
  own: a seal of a table, and a fold, an RG compaction or an
  elimination of a version (§11.5). A job is queued once until it
  starts. The cleanups wait in a set of their own, tried each
  second (§11.4). It has its own thread, not one of the reapers'
  pool: a seal writes and fsyncs an RG's files, which would delay
  the passes of the btree and row reapers.
- **The insert path counts, for each table, in memory,** the
  rows of its delta store, an update's new version of a row of
  an RG among them: each transaction counts its own, and
  hands them to the sealer at its commit, so a seal never starts
  before the rows it is to take are committed. At 10k rows the
  table is queued (§7). A prepared transaction's counts wait,
  by its gid, for its `COMMIT PREPARED` (`sm3::commit_prepared`);
  `ROLLBACK PREPARED` drops them. A restart loses them, and a
  scan catches the rows (below).
- **A scan that reads more than 10k rows of a delta store also
  queues a seal job.** The counts start at 0 when the database
  opens, so this catches a delta store that grew before a
  restart. Until then, a delta store can hold up to twice the
  limit.
- **The seal takes no partial RG.** An idle table keeps fewer
  than 10k rows in its delta store. A scan reads them as rows
  of a row store table.
- **One transaction for each RG.** A job that finds more than
  one RG's worth of rows seals them one RG at a time, until the
  delta store is below the limit. So the seal's row locks last
  for one RG only.

---

## 11. RG compaction

RG compaction, and the jobs beside it, rewrite an RG that has
many dead rows. They are GC, not a write path. Each is a job of
the sealer (§10.4), in a READ COMMITTED transaction of its own
(§10.3), at the newest committed state, and each holds X on the
RG, NOWAIT (§9.1): a writer of the RG makes it give up, and the
next markers committed on the RG queue it again.

| Job | When | What |
| --- | --- | --- |
| the fold | markers of 1% of a version's rows | the markers into the version's validmap |
| RG compaction | 20% of a version's rows dead, versions 0 to 2 | the next version, with the live rows alone |
| elimination | 20% of version 3's rows dead, or an RG under half of 10k rows live | the live rows into the delta store |
| the cleanup | no snapshot sees an old version | its markers and validmap erased |

### 11.1 The fold

1. Take X on the RG, NOWAIT.
2. Read the version's validmap and markers at the newest state.
3. Put a new validmap: the old bits, and a bit for each marker.
4. Erase each marker that step 2 read.
5. Commit. The lock is held to the commit.

- **The fold threshold is the most markers a scan reads for a
  version**: 100 of 10k. A scan reads a version's validmap, one
  key, and then each of its markers, one key each. A fold writes
  a new validmap version, 1,250 bytes, that stays until the
  watermark passes it. So a lower threshold makes scans cheaper,
  and costs more folds.
- **The fold needs no watermark.** It holds X on the RG, so no
  marker is in flight. A snapshot older than the fold still sees
  the old validmap and the markers, as its put and erases are
  versions.
- **Markers stay per row.** sm1 makes the uncommitted version of
  a key its lock ([`MVCC_OVERVIEW.md`](MVCC_OVERVIEW.md) §5.1).
  If writers wrote the validmap, a delete would lock the whole RG
  to its commit.

### 11.2 RG compaction into the next version

`col_compact` (`sm3/src/colstore/colstore.cc`), on version `v`
of 0 to 2:

1. Take X on the RG, NOWAIT.
2. Read version `v`'s RG key, validmap and markers at the newest
   state: its dead rows. `D_{v+1}` is `D_v` with the dead rows
   mapped to version 0.
3. Write every CG of version `v+1`, with the live rows alone, in
   their order. Fsync the files and the directory.
4. Put the RG key of `fsid(rg, v+1)`, with its zonemap and
   `D_{v+1}`, and its filter keys. The new version has no
   validmap.
5. Erase the RG key of `fsid(rg, v)` and its filter keys.
6. Commit; then give the sealer a cleanup of `fsid(rg, v)`
   (§11.4).

- **A snapshot older than the commit reads version `v`**, and
  its markers, which step 5 does not erase. A newer one reads
  version `v+1`. RG compaction does not wait for old snapshots,
  and a reader takes no lock: analytic queries are long, and
  they would hold back a compaction that waited for them.
- **No row lock is tried.** A row lock is on the row's `_id`,
  which the next version keeps. A `FOR SHARE` or `FOR UPDATE`
  holder keeps its lock, and does not stop RG compaction.
- **A writer older than the compaction** maps its `rno` (§8.1),
  writes the marker at the newest place, and a shadow marker at
  its own (§9).
- **Every CG is written again.** The new version holds no dead
  row. Its `_id` CG stays in order if the old one was.

### 11.3 Elimination

`col_eliminate`, on the newest version of an RG:

1. Take X on the RG, NOWAIT.
2. Read the version's live rows at the newest state.
3. For each, try the exclusive lock of its delta key `_id`. A
   holder, a reader's `FOR SHARE` included, makes it give up: the
   write of the delta key would wait for it.
4. Put each at delta key `_id` as a moved-in version
   (`sm2::rel_movein_at`, through `sm1::insert_moved_in`). A
   value over 2,000 bytes goes to TOAST, as an insert's does. The
   rows count toward the next seal.
5. Erase the version's RG key and its filter keys.
6. Commit; then give the sealer a cleanup of the version (§11.4).

**A moved-in version** is a live version with the flags
`version_sealed` and `version_moved_in`. Elimination changes no
row's content, so it must not fail a user's transaction:

- **A reader** sees it as any version.
- **First-committer-wins sees through it**, and through a sealed
  tombstone under it, as for a sealed tombstone (§10.2). It
  judges a write by the first committed version under them that
  is neither. With no such version, as for a row of a bulk load,
  a write does not conflict.
- **A write that passes** goes on over it as over any live
  version: it does not fail with `SM_ERR_SEALED`, and
  `sm1::read_latest` returns it.
- **A writer older than it is told**
  (`sm1::wrote_over_moved_in`): the write went over a moved-in
  version that its snapshot does not see. The writer puts a
  shadow marker at the place its snapshot sees the row at (§9).
- **The Garbage Collector** treats it as any version.

- **The rows are written twice**, into the delta store now, and
  into a new RG at the next seal. About 4,100 rows of 10,000 are
  left at version 3's 20%: 10,000, then 8,000, 6,400, 5,120, and
  4,096.
- **An RG under half live is eliminated** at any version. Its
  rows wait in the delta store, as rows, until the next seal
  puts them into a full RG with others. There is no merge of
  small RGs.
- **A snapshot older than the commit reads the version.** A
  newer one reads the moved-in delta rows. Each sees each row
  once.

### 11.4 The cleanup of an old version

A version's markers and validmap outlive its RG key: a snapshot
that sees the version reads them, and a writer of that snapshot
can add a shadow marker. The cleanup (`col_cleanup`):

1. Ask sm1 whether the version's `_id` filter key is settled
   gone (`sm1::read_settled_versioned`): its newest version is a
   committed erase at or below the GC watermark. RG compaction
   and elimination erase it in the commit that erases the RG key,
   so no snapshot then sees the version. If not, keep the hint.
2. Erase each marker of the `fsid`, shadow markers included, and
   its validmap, at the newest state. Commit.

- **The erases are versions**, as any erase. The compaction
  filter drops them and the keys under them later.
- **sm1 needs no new call.** `sm1::read_settled` cannot judge a
  file set key, of class `0x05`. The filter key is a versioned
  key, and its erase commits with the RG key's.
- **A prepared writer holds the watermark back**, across a
  restart too, so the cleanup waits for its shadow marker.
- **A restart loses the hints.** The sealer's first pass finds
  each old version with markers or a validmap left, and no RG
  key (`col_orphans()`), and gives each a hint.

### 11.5 When the jobs run

- **A commit counts** the delete markers it committed on each
  version, in memory; a prepared transaction's at its `COMMIT
  PREPARED`, as its rows' (§10.4). At 1% of a full RG's rows it
  queues a fold; at 20%, 2,000, a job that compacts or
  eliminates. Below 20%, the dead rows cost a scan less than a
  rewrite of the RG costs.
- **A scan counts too.** A scan reads each version's validmap
  and markers, so it counts them at no cost, and queues a job
  at 20% of that version's rows, or a fold at 1%. The counts in
  memory start at 0 when the database opens, and a scan catches
  what they miss.
- **The job decides** (`rg_plan()` in
  `sm3/src/colstore/sealer.cc`): an RG under half of 10k rows
  live is eliminated; else one 20% dead is compacted, or
  eliminated at version 3; else nothing is done.
- **The cleanups are tried each second**, while any is left, as
  the reapers' passes are.
- **GC keeps an old version's files** until the erase of its key
  is at or below the watermark (§13).

---

## 12. Scans

All access is by scan. A scan takes three things from digres:

| Input | What it is |
| --- | --- |
| a snapshot | as every scan takes |
| the projection | the set of columns that the plan needs; the scan reads only their CGs, and the `_id` CG |
| the bounds | a conjunction of bounds, one or more for each column: a range (`lo` and `hi`, each inclusive, exclusive or open), an `IN` set, `IS NULL` or `IS NOT NULL` |

- **digres derives the bounds when the scan begins**
  (`qual_bounds()` in `dg/src/backend/access/sm/smstore.c`),
  from the scan's quals of the forms `Var op Const`, `Const op
  Var`, `Var = ANY (array)`, `IS NULL` and `IS NOT NULL`. `op` is
  `<`, `<=`, `=`, `>=` or `>` of the default btree operator
  family of the column's type, with both operands of that type
  and the column's collation. A decimal constant is the
  column's integer at or below it for a lower bound or an
  equality, at or above it for an upper bound, within the
  column's precision (`numeric_bound()`), so a bound takes in
  every row the qual would. sm encodes the values to key bytes
  (`sm_row_scanopen_columns`). A qual of another form, such as
  an `OR`, an expression, a `Param` or `LIKE`, gives no bound.
- **Pruning is not filtering.** A scan skips RGs that cannot
  match, and hands out every live row of the other RGs. The
  executor of digres applies the whole qual to each row, the
  bounded columns too. So the rule of sm3 holds: a scan filters
  nothing ([`sm3/SM3_DESIGN.md`](sm3/SM3_DESIGN.md) §5).
- **A scan hands out one row at a time** through `sm.h`, as an
  sm2 tuple that sm3 forms from the projected columns' datums,
  with null for a column not read. So a row of more than 64 KB
  fails. Datums handed out with no tuple are in
  [`NEXT.md`](NEXT.md). The row's name is `(place, _id)` (§8).
- **PostgreSQL's table access method has no projection
  argument for a sequential scan.** digres takes the columns
  from the scan's plan: its target list and quals. Its planner
  gives a column store table's scan no physical target list
  (`use_physical_tlist()`), which would name every column.
- **A scan of a table the statement writes or locks reads every
  column.** The write takes the old row from the scan on it.
  digres never takes a row of a scan of some columns as the
  table's row.
- **`TABLESAMPLE` is refused**, with SQLSTATE `0A000`
  (`smtable_scan_begin()`). A sample scan reads rows by rising
  `_id`, and a column store table's scan gives its RGs' rows,
  then the delta store's: rows would be lost
  ([`../QUESTION.md`](../QUESTION.md) 4).
- **A batch interface waits for the columnar executor.** It
  changes the rule of sm3 that its interface is one tuple at a
  time, so it is decided with that executor.

A scan at a snapshot:

1. Reads the RG keys at the snapshot: of each RG, the one
   version that the snapshot sees.
2. For each bounded column (`may_hold`): reads its filter key
   if its bound is an equality or `IN`, then skips the RG if its
   zonemap or its filter proves there is no match (§6.2).
3. A filter key is read before its zonemap is tested, so an RG
   that a zonemap skips can cost a read of a filter key.
4. Reads the CGs that the query needs, from the local cache or
   from S3.
5. Reads the version's validmap, and its delete markers under
   `0x0002 ‖ be64(fsid)`, into a bitmap of dead rows
   (`col_dead`), and skips each row whose `rno` is set. The
   bitmap is a selection vector for a later columnar executor.
6. Queues a fold of the version at markers of 1% of its rows, or
   a job that compacts or eliminates it at 20% dead (§11.5).
7. Scans the delta store, as a keyless row store table.

### 12.1 ANALYZE

| Statistic | How |
| --- | --- |
| row count | exact: the row counts of the RG keys, less their dead rows, the bits of their validmaps and their markers, plus the delta store's count; no file is read |
| null fraction | from the sample, as for a row store table |
| the sample, for most common values, histograms and distinct counts | up to 100 RGs at random from the RG keys; from each, a share of the rows wanted in proportion to its live rows, at random places; the delta store read whole, and its share of the rows wanted kept at random, in proportion to its share of the live rows |
| the size, for `relpages` | the bytes of the delta store and of every CG, from the RG keys; no file is read |
| the estimators | those of sm3, on the sample, as for a row store table |

- **The cost of a sample is in RGs, not in rows.** A read of one
  row of a CG reads and decompresses the whole CG.
- **The delta store is read whole.** The seal keeps it at about
  one RG's rows, so a whole read costs what a sample would, and
  its row count is exact.
- **Many RGs, few rows from each.** The rows of an RG were
  inserted together, and are alike. At the default target of
  100, three whole RGs give the 30,000 rows wanted, from three
  places only, and the distinct counts come out low. One
  hundred RGs of 300 rows each spread the sample over the
  table.

---

## 13. Durability and GC of files

**RocksDB is the authority over files.** A file's presence on
disk or in S3 proves nothing: it can be from a transaction in
flight or aborted, or be one that an old snapshot still reads.
Which files are live is decided from the RG key versions alone.
A listing of a directory or of S3 never decides it.

- **A commit is still the status record**
  ([`XCTMGR.md`](XCTMGR.md)). A file is durable before the RG
  key that names it is put, so a committed RG key never names a
  missing file.
- **A transaction keeps a list of the files it wrote.** An
  abort or a rollback to a savepoint unlinks them, as an abort
  of a bulk load unlinks its files
  ([`BULKLOAD.md`](BULKLOAD.md) §5.6). An abort still writes
  nothing to RocksDB.
- **A crash is an abort, except for a prepared transaction.**
  When the database opens, a sweep (`fileset_sweep`) goes
  through each store's directory. The newest committed or
  prepared version of an `fsid`'s RG key decides:
  - a prepared version, or a committed put, keeps the files;
  - a committed erase, or no such version, deletes them. A
    version in flight at the crash is dead.

  No snapshot is older than the open, so a committed erase
  leaves no reader. The sweep also removes the directory of a
  store that is gone.
- **GC deletes the files of an `fsid` when no RG key version of
  it is committed and visible to a snapshot, or prepared.** Two
  cases reach this rule:
  - the erase of the RG key, by RG compaction or elimination, is
    at or below the watermark ([`GC.md`](GC.md));
  - the put of the RG key is dead. The compaction filter, as it
    drops the dead version, queues the `fsid`, and the GC worker
    deletes the files. This covers every `ROLLBACK PREPARED`: a
    prepare ends the transaction's list of files.
- **An unlink that fails leaves the file** until the sweep of
  the next open. There is no other sweep while the database
  runs.
- **Online backup is out of scope**, as for a row store table
  ([`DBMGR.md`](DBMGR.md) §1). An offline copy of the database
  home holds `colstore/` with the rest.

---

## 14. DDL

| Statement | What it does |
| --- | --- |
| `TRUNCATE` | swaps the store, as for a row store table ([`CATALOG.md`](CATALOG.md)); when sm1 drops the old store, it removes the store's directory `colstore/<storeid>/` with it; an abort brings the old store back, its files too |
| `ADD COLUMN` with a constant default, or none | no rewrite: an RG key with no CG for the column's attnum gives every row of the RG the column's missing value, as PostgreSQL's `attmissingval` does; a new RG has a CG for the column |
| `ADD COLUMN` with a volatile default | a rewrite into a new store |
| `DROP COLUMN` | no rewrite: a scan ignores the CG of a dropped attnum, and RG compaction leaves it out of the next version; an attnum is never used again, so an old CG is never taken for a new column |
| `ALTER COLUMN TYPE`, and every other rewrite | `rewrite_table` into a new store: it reads every row and writes RGs as a bulk load does (§9.2); the old store is dropped at commit |

- **Each reader of an RG key handles a CG that is not there.**
  It is the cost of an `ADD COLUMN` with no rewrite.
- **A temporary table is a temporary store** in the temporary
  instance, as for a row store table
  ([`TEMPTABLE.md`](TEMPTABLE.md)). Its files are in
  `colstore/` under the temporary instance's home, and never go
  to S3.

### 14.1 Keys, indexes and constraints

A column store table is for analytics. It has no index, no
primary key, no unique key and no foreign key. digres takes the
SQL that would make one, does not make it, and gives a
warning, so a script written for a row store table runs.

| SQL on a column store table | What digres does |
| --- | --- |
| `PRIMARY KEY` | ignored, with a warning; the `NOT NULL` that it implies on its columns is kept, as it needs no index |
| `UNIQUE`, `EXCLUDE` | ignored, with a warning |
| `REFERENCES`, `FOREIGN KEY`, from the table | ignored, with a warning |
| `CREATE INDEX` | ignored, with a warning; no index is made |
| `NOT NULL`, `CHECK`, generated columns | applied, as for a row store table |

- **A foreign key of a row store table that references a column
  store table fails**, as PostgreSQL's does when the referenced
  columns have no unique key. The statement is on the row store
  table, and nothing in it is ignored.

---

## 15. Tests

A test goes to each layer that can express its scenario
([`DESIGN.md`](DESIGN.md#4-testing-across-levels) §4). No
existing scenario of `sm1/mvcc` changed. The tests not written
yet are in [`NEXT.md`](NEXT.md).

### 15.1 mvcc: the rules of sm1

The scenarios `s01` to `s12` in `sm1/mvcc/in`. Each runs twice,
on versioned and on chained stores, as every scenario does. The
driver has the verbs `seal <row>`, an erase with a sealed
tombstone, `sealput <row> <value>`, a live sealed version, and
`movein <row> <value>`, a moved-in version. The sessions that
seal run at READ COMMITTED.

| Scenario | Expected |
| --- | --- |
| `s01`: an RR writer, whose snapshot is older than the seal of its row, writes the row; an insert over a sealed tombstone | `sealed`, not `conflict`; the insert is `ok` |
| `s02`: an RR writer, whose row changed after its snapshot and was then sealed, writes the row | `conflict`: the change under the tombstone decides |
| `s03`: an RC writer waits on a seal in flight, and the seal commits | `BLOCK`, then `sealed` |
| `s04`: an RC writer waits on a seal in flight, and the seal aborts | `BLOCK`, then the write succeeds |
| `s05`: a reader before and after the seal's commit | the row, then no row |
| `s06`: a moved marker | every write over it is `sealed`; a reader sees it as any version |
| `s07`: a plain erase in the seal's place (negative) | the RR writer gets `conflict` |
| `s08`: a row lock over a sealed tombstone | judged by the version under it |
| `s09`: a seal waits on a writer in flight, which commits, then on one that aborts | `updated`, and the row keeps the writer's value; then `ok`, and the row is sealed |
| `s10`: a moved-in version, over a sealed tombstone and where no version is | a reader sees it; an RR writer older than it writes over it; an insert is `dupkey` |
| `s11`: a moved-in version over a change committed after the writer's snapshot | `conflict`, for a write and for a row lock |
| `s12`: an RC writer waits on a moved-in version in flight | `BLOCK`, then `ok` over it |

The Column Store's notions above sm1, such as RGs, scans and
pruning, cannot be expressed here. §15.3 and §15.4 test them.

### 15.2 sm1 and sm2: unit tests

`sm1/test/test_fileset.cc`, file sets (§4.5, §13): a committed
file set read back; an abort and a rollback to a savepoint
unlink its files; the sweep at open; a prepared file set across
a restart; an erased file set's files kept for an old snapshot,
then deleted by GC; an erase judged by newer commits; a missing
file is corruption; a dropped store's directory goes.

`sm2/test/test_cg.cc`, the CG file and the statistics (§5, §6):
a CG of each encoding read back; a changed byte is corruption;
NaN and infinity in the flags; pruning skips only CGs that
cannot match; a bpchar CG's filter matches a padded value; a CG
of another collation version is not pruned by value; a
dictionary only where it is smaller than plain; the exact set to
4 KB of keys and a bloom filter above, with few
false positives; a case-insensitive collation's filter, which
matches another case; **pruning never skips a match**, for
random CGs and random bounds, against a test of every value.

### 15.3 sm3: unit tests

`sm3/test/test_colstore.cc`:

1. The kind across a restart; an index refused; a SERIALIZABLE
   transaction refused; a dropped table's sequence.
2. A bulk load: whole RGs, the rest in the delta store; an
   abort.
3. A scan: every row, its columns, and the bounds that skip RGs.
4. An update and a delete of a row in an RG, by `(place, _id)`
   and by `_id` alone; concurrent writers at each level.
5. The seal: no partial RG; a locked row passed over; a writer
   that follows a sealed row; the job at an RG's worth of rows;
   an update's new version counted.
6. RG compaction: the live rows rewritten into the next version;
   the give-up on a writer; a writer older than one compaction,
   and than two; the trigger at 20%; the fold; the cleanup; the
   elimination of version 3 and of an RG under half live, with
   writers older than it, by place and by `_id`.
7. `ANALYZE`: the sample of RGs and of the delta store.
8. DDL: `TRUNCATE`, `ADD COLUMN`, `DROP COLUMN` and a rewrite.
9. A changed byte in a CG file fails a scan of its column, and
   not a scan of the others.
10. A reader's `FOR SHARE` lock holds off elimination, not RG
    compaction, which keeps the lock; a prepared writer holds off
    RG compaction and the seal until it ends.
11. A bulk load rolled back to a savepoint leaves no row, no RG
    and no file.
12. **A column store table agrees with a row store table.** One
    random workload of inserts, updates by `_id` and by `(place,
    _id)`, and deletes, at READ COMMITTED and REPEATABLE READ,
    with seals and RG compactions between, runs on both; every
    50 steps a scan of each gives the model, and an old
    REPEATABLE READ snapshot gives the model as it was.
13. `ANALYZE`'s null fraction counts the nulls of the RGs and of
    the delta store.
14. With TOAST on, the seal reads each value out of line whole,
    erases its chunks, and a snapshot older than the seal still
    reads them.
15. A scan's bounds on a text column, a range and an equality,
    skip only the RGs that cannot match.
16. A second delete or update of a row of an RG in one statement
    fails with `SM_ERR_SELFMODIFIED`; in the next statement, the
    first one's result is seen.
17. A prepared transaction's delta rows and delete markers count
    toward the seal and RG compaction at its `COMMIT PREPARED`,
    and not after `ROLLBACK PREPARED`.
18. A value of 2,048 bytes is taken and one of 2,049 refused, on
    every write; a row store table takes the longer one.
19. The fold: markers into the validmap, which every snapshot
    agrees on; it gives up on a writer, and writers of one RG do
    not wait for each other; a write of a folded row fails as of
    a deleted one; the sealer folds at 1%.
20. RG compaction into the next version: an old snapshot reads
    the version it saw; a writer older than one compaction, and
    than two, deletes the right row and no longer sees it; two
    writers on each side of a compaction conflict.
21. The cleanup: an old version's markers go once no snapshot
    sees it, and not before; it waits for a prepared writer
    across a restart; the sealer's first pass finds an old
    version that a restart left.
22. Elimination: of version 3, with an RR writer older than it,
    by place and by `_id`, and one whose row changed since, which
    fails with `SM_ERR_CONFLICT`; of an RG under half live, whose
    rows a later seal takes; a write by `_id` in a statement
    marks its row once.
23. A delete by a writer older than the seal hides the delta row,
    and an update's new version hides the old one; the newest
    version of a sealed row is read from its RG; a READ COMMITTED
    write of a row updated since and folded is `SM_ERR_UPDATED`;
    an RR write by `_id` of a row deleted since conflicts;
    a writer of a row that a newer version dropped fails, at each
    level; an older writer's update, out of line, over a moved-in
    row hides the row; a writer of a row deleted before an
    elimination fails; RG compaction of a version with no live row
    leaves none.
24. **Random workloads across the sealer's jobs**, run by hand
    between the steps: a REPEATABLE READ writer and a reader of the
    same age, whose views are their snapshots and the writer's own
    writes, each row once, with savepoints rolled back and rounds
    prepared before `COMMIT PREPARED`; READ COMMITTED writers of the
    same rows, retried after `SM_ERR_UPDATED` as digres retries,
    which end as one after the other; a REPEATABLE READ writer of
    any row, whose write fails with `SM_ERR_CONFLICT` exactly on a
    row changed since, and ANALYZE's count of live rows exact after
    each round. `SM3_SEED` picks another workload, `SM3_SEALER`
    runs the sealer's own jobs too, and `SM3_TOAST` writes values
    out of line.

A test can give its catalog smaller RGs (§3), so that a seal or
an RG compaction comes after a few hundred rows.

### 15.4 rel and digres

`rel/test/test_colstore.rel`, through `sm.h`: a load and a scan
across a restart; SERIALIZABLE and an index refused; a delete by
name and an abort; the seal's job; an update and a delete by
`_id` alone; a scan of some columns, with bounds; an update
counted toward the seal; two loads prepared across a restart,
one committed and one aborted; bounds on a text column; a
prepared transaction's inserts sealed after `COMMIT PREPARED`;
the fold; RG compaction into the next version, with a REPEATABLE
READ writer older than it; elimination of an RG under half live,
with a writer older than it; a `FOR SHARE` holder across an RG
compaction, which keeps its lock.
`rel/test/test_colstore_jobs.rel` runs the sealer's jobs by hand
(`fold-rg`, `compact-rg`, `eliminate-rg`, `cleanup-rg`), on RGs of
1,000 rows (`set-rg-rows`), and reads each version's dead rows and
markers (`row-groups`, `markers-of`): the fold, with a snapshot
older than it; the fold's give-up on a writer; a delete of a folded
row; RG compaction into versions 1 to 3, with a writer older than
two of them; the elimination of version 3, with an older writer's
update by name (`update-at`) and the cleanup that waits for it; a
`FOR SHARE` holder, which stops elimination and not RG compaction;
`SM_ERR_CONFLICT` and `SM_ERR_UPDATED` for a row changed since; a
prepared delete across a restart; a delete older than the seal.
`rel/test/test_toast.rel`, with TOAST on: 10k rows of values of
2,000 bytes inserted one at a time, sealed by the seal's job,
read back whole; a value of 2,049 bytes refused.

`dg/test/columnar.sql`: `CREATE TABLE ... WITH (storage =
columnar)`; the warnings of §14.1 but `EXCLUDE`'s, and the `NOT
NULL` kept from an ignored `PRIMARY KEY`; `INSERT ... SELECT`
into RGs; `UPDATE`, `DELETE ... RETURNING` and `SELECT ... FOR
UPDATE`; a `DELETE` by `_id`, which sm finds in its RG; an
`UPDATE` at REPEATABLE READ; `EXPLAIN VERBOSE` of a scan of some
columns, and an `UPDATE` whose scan reads every column;
queries with bounds, whose results equal a row store table's;
`ALTER TABLE` and `TRUNCATE`; `CREATE TABLE AS` and `COPY`; a
SERIALIZABLE transaction refused with `0A000`; `storage =
'row'`, and another value refused; a foreign key that
references a column store table, which fails; bounds on a
`numeric(10,2)` column of constants at a finer scale or past its
precision, against a row store copy; `TABLESAMPLE` refused with
`0A000`; a `DELETE ... USING` that matches a row of an RG twice,
at REPEATABLE READ, deletes it once; a value of 2,049 bytes
refused with `54000`.
`dg/test/columnar_jobs.sql`: `DELETE`s and `UPDATE`s of a quarter of
a table's rows each, which queue the sealer to compact and then
eliminate its RGs while the statements that follow write them; a
REPEATABLE READ transaction across them. The jobs run when they
run, alone and in 8 sessions at once; every result is the same
whenever they do.

### 15.5 Layers with no test

- **aa** has no change.
- **sq** has no change: `WITH (storage = columnar)` is a
  reloption, which its grammar parses already: `columnar` is an
  identifier, as any value of a reloption may be.
- **pgregress** has no test of a column store table, as
  PostgreSQL has no such table.

---

## 16. Open questions

Each question has a proposal where there is one. None is
decided.

### 16.1 The seal and RG compaction

1. **Do the seal's locks last too long on S3?** The pessimistic
   seal holds its tombstones through the file writes. With
   S3, that is hundreds of ms. The alternative is optimistic:
   write the files first, then erase, and redo the files on a
   conflict.

### 16.2 Deferred

1. **The place at insert** ([`../QUESTION.md`](../QUESTION.md)
   5). A row would get its place, `(rg_id, rno)` of an open RG,
   at its insert, and keep it through the seal. Row locks would
   then be on the place, an insert would never wait for a seal
   (the next RG is drawn while one seals), and the seal would
   take S on the RG, NOWAIT, and every row of it, a hole for each
   `rno` whose insert aborted. It changes the value of every
   delta row, or costs every insert a second key; row locks on
   `_id` need neither.
2. **Logical decoding** ([`LOGICALDECODE.md`](LOGICALDECODE.md)),
   which is not built. When it is: an insert into the delta
   store or a bulk load's RG emits an insert for each row; a
   delete marker emits a delete, with the row's values read
   from the RG's CGs; an update emits one update; a shadow
   marker, the seal, the fold, RG compaction, elimination and the
   cleanup emit nothing, as they change no row.
3. **Deferred features:** column patches for wide updates, a
   `sum` in the zonemap, the byte length of varlena values,
   `LIKE 'abc%'` pruning, RLE, frame-of-reference and FSST.

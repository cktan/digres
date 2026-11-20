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
    --    _id CG: RG 1. Fsync them.
    -- 3. Put the RG key of RG 1: the files, and each CG's
    --    min, max, null count and more. Put a filter key for
    --    each CG: a bloom filter or an exact set.
    -- 4. Commit.

UPDATE ev SET amount = 4 WHERE _id = 900;
    -- Row 900 is in RG 1. Two writes:
    --   a delete marker at (rg 1, _id 900)
    --   a new version at delta key _id 900
    -- The row keeps _id 900.

SELECT sum(amount) FROM ev WHERE kind = 'view';
    -- 1. Read the RG keys at the snapshot.
    -- 2. RG 1: 'view' is between kind's min and max. Read
    --    kind's filter key: 'view' is not in the exact set.
    --    Skip RG 1. No file is read.
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
| delete marker | a versioned key `(rg_id, _id)` that says the RG's copy of the row is deleted |
| moved marker | a delete marker that RG compaction writes; a writer sees through it (§11) |
| RG key | the versioned key that holds an RG's metadata and the statistics of its CGs |
| filter key | the versioned key that holds one CG's bloom filter or exact set |
| zonemap | a CG's statistics in the RG key: min, max, null count and flags |
| key bytes | a datum in sm2's order-preserving key encoding, `encode()`; memcmp order is SQL order |
| RG compaction | the rewrite of an RG with many delete markers into a new RG |
| long value | a value of a varlena type longer than 1024 bytes; a CG holds a pointer to it |
| long-value file | the file that holds the long values of one CG |

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
| the TID that digres carries | `(rg_id, _id)`; `rg_id` 0 for a row in the delta store | §8 |
| locks | row locks on delta key `_id` | §9.1 |
| isolation | READ COMMITTED and REPEATABLE READ; a SERIALIZABLE transaction that reads or writes a column store table fails | §9.1 |
| how digres makes one | `CREATE TABLE ev (...) WITH (storage = columnar)`; sm3's catalog records the kind | §3 |
| the executor | PostgreSQL's, a row at a time, until the Column Store is stable; then an executor for columnar operations | §3 |
| `relpages` | the bytes of all CGs and of the delta store, divided by 8 KB | §7 |
| an update or delete of a row in an RG | a delete marker, plus a reinsert into the delta store for an update | §9 |
| metadata keys | one RG key for each RG; one filter key for each CG | §4.3 |
| pruning statistics | in RocksDB and in each file | §6 |
| min and max | key bytes, cut to 32 bytes | §6.1 |
| dictionary | for each CG, sorted in key order | §5.2 |
| encodings | plain, dictionary, delta for sorted integers; zstd on each CG | §5 |
| long values | out of line, in a long-value file of the RG's file set; left out of the filter, kept in min and max | §5.4 |
| RG size | 10k rows | §7 |
| the seal | pessimistic; its tombstones are transparent to first-committer-wins | §10 |
| RG compaction | moved markers, transparent in the same way | §11 |

The constants (10k rows, 1024 bytes, 32 bytes, 4 KB, 20%, 50%)
may change. An RG key records the real sizes of its CGs,
so a change of a constant does not make an existing file wrong.
A test can give a catalog smaller RGs (`sm3::cat_set_rg_rows`),
so that a seal, an RG compaction and a merge come after a few
hundred rows. The 20% and 50% are of the RG size set.

**Which level owns each part:**

| Part | Level | Why |
| --- | --- | --- |
| the sealed tombstone, and the moved marker's rule in first-committer-wins | sm1 | rules of the write path; sm1 knows no table |
| file sets: the file I/O of CGs, their key class, their GC and the sweep at open; later S3 and the local file cache | sm1, beside the Buffer Manager | sm1 owns the I/O of every byte, the order of fsync and RG key, and GC (§4.5) |
| the CG file format, the encodings, the dictionary, the zonemap and the filter, and the pruning test on key bytes | sm2 | they need types and `encode()`; the pruning test compares key bytes and knows no type |
| the RG keys, the seal, RG compaction, the scan, the lookup by `_id`, and the locks of §9.1 | sm3 | they need the table, its columns and predicates, as the `tbl_*` calls of a row store table do |

---

## 4. Where bytes live

### 4.1 Files

- **One file for each CG.** A file is written once and never
  changed, renamed, appended to or overwritten.
- **A file's path is `colstore/<storeid>/<rg_id /
  1024>/<rg_id>.<name>`**, under the RocksDB directory of the
  database, or of the temporary instance for a temporary table.
  `<name>` is the column's attnum in the Catalog of sm3, which a
  column keeps for good, and which a dropped column does not
  give back ([`CATALOG.md`](CATALOG.md)). The `_id` CG's name is
  `id`. A long-value file's name is `<attnum>.long` (§5.4).
  `rg_id`s come from a sequence of the table and are never used
  again. So a path is unique for the life of the database. S3's
  keys will take the same path.
- **A CG is never replaced under its `rg_id`.** Each rewrite,
  of rows or of one column, makes a new RG with a new `rg_id`
  (§11). So a file name needs no version, and an RG key is put
  once and erased once.
- **A directory holds the files of 1024 RGs**, about 30k files
  for 30 columns. A table of 1B rows has about 3M files.
  `DROP TABLE` removes the store's directory tree.
- **A file is durable before anything refers to it.** The writer
  fsyncs the file and its directory, then puts the RG key that
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
| relid `0x0002` | delete markers | `0x00 ‖ 0x0002 ‖ be64(rg_id) ‖ be64(_id) ‖ ~wts ‖ ~seq` | versioned |
| relid `0x0003` | unused | | |
| relid `0x0004` | filter keys | `0x00 ‖ 0x0004 ‖ be64(rg_id) ‖ be16(attnum + 1) ‖ ~wts ‖ ~seq`; the `_id` CG's is 0 | versioned |
| relid `0xFFFF` | TOAST of delta rows | as in a row store table | versioned |
| class `0x05` | RG keys: sm1's file set keys | `0x05 ‖ be64(rg_id) ‖ ~wts ‖ ~seq` | versioned, of sm1's file set class |

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
- **The key of a delete marker holds the `rg_id`.** One `_id`
  can have copies in more than one RG: a dead copy in an older
  RG, and the live copy in a newer RG.

### 4.4 The value of an RG key

| Field | For each |
| --- | --- |
| format version | RG |
| row count | RG |
| `_id` min and max | RG |
| the count of CGs | RG |
| attnum | CG |
| file size | CG |
| CRC32C of the file | CG |
| size and CRC32C of the long-value file; 0 if none | CG |
| zonemap (§6.1) | CG |

The encoding of a CG is in the CG file's tail (§5.3), not in the
RG key.

The value is about 50 bytes for each column. A table of 30
columns has RG keys of about 1.5 KB.

### 4.5 File sets

A file set is sm1's: a group of immutable files, and the
versioned key that names them. sm1 knows no RG. An RG of sm3 is
one file set, with `fsid` equal to `rg_id`, and the RG key's
value (§4.4) is the file set's payload, which sm1 does not read.

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
   files that no version names (§13).

- **sm3 never uses a file path.** It reads a CG through sm1, by
  `rg_id` and the file's name (`id`, `3`, `3.long`).
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
| dictionary | a CG with no long value, where it is smaller than plain (§5.2) |
| delta, bit-packed | `int2`, `int4` and `int8` in ascending order, with no null: the `_id` CG of a sealed or loaded RG |

**A CG holds no TOAST pointer.** A value of 1024 bytes or less
is in the CG. A long value is in the CG's long-value file
(§5.4), and the CG holds a pointer to it.

After the encoding, the CG is compressed with zstd. The tail
tags the encoding, so a new encoding does not change the format
of an existing file.

The rows of an RG that RG compaction merged (§11.1) are in the
order of the RGs merged, so its `_id` CG is not in ascending
order, and gets plain encoding.

### 5.2 The dictionary

- **Each CG has its own dictionary**, in the file. It never
  changes, and it needs no MVCC, no lock and no GC.
- **The dictionary holds datums, sorted in key order.** The codes
  keep the order of the values.
- **The values are bit-packed codes.**
- **The writer of the CG chooses.** It uses a dictionary if the
  dictionary and its codes are smaller than plain. A CG with a
  long value never has one.
- **A read expands each code to its datum.** digres evaluates a
  predicate on each row's datums. A predicate evaluated on codes
  waits for the columnar executor.

### 5.3 The layout of a file

A CG file holds, in this order (`sm2/src/col/cg.cc`):

1. The body, compressed with zstd: the encoding, the row count,
   a null bitmap and a long bitmap where a row needs them, then
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
  later. sm1's `fileset_read` checks nothing; its callers do:
  - sm3 compares the file's size and CRC32C with the RG key's.
  - sm2's `cg_open` checks the tail's CRC32C.
  - sm2's `cg_long_check` checks a long value's CRC32C.

  It costs one pass at memory speed, which is far less than
  zstd's decompression.
- **A mismatch fails the scan** with a data corruption error.
  Nothing repairs the file.
- **An upload to S3 carries the same CRC32C** in S3's checksum
  header, `x-amz-checksum-crc32c`, so S3 checks the upload from
  end to end.

### 5.4 Long values

A long value is a value of a varlena type (`text`, `bytea`,
`jsonb` and the others) longer than 1024 bytes. Nobody filters
on it by equality. So it is kept out of line, as a URL is kept
in place of a document.

- **A CG holds a pointer of 16 bytes in its place**:
  `(offset, length, CRC32C)` in the CG's long-value file
  (`sm2::cg_long_t`). Only the
  pointer counts toward the size of the CG (§7), so an RG of
  long values still holds 10k rows.
- **The long-value file is `<rg_id>.<attnum>.long`**, in the
  RG's file set (§4.5). It lives and dies with the RG, and goes
  to S3 with the other files. A CG with no long value has none.
- **The writer of an RG holds its rows in memory**, long values
  whole, and builds the long-value file in memory at the RG's
  flush (`rg_build_add`, `sm2::cg_build`). The file has no size
  target.
- **Each long value has a CRC32C of its own**, in its pointer
  in the CG. A read of one value is a ranged read, or one
  ranged GET from S3, and checks that CRC32C. The CRC32C of the
  whole file (§5.3) is for a read of the whole file.
- **A scan reads each long value of a column it reads**, for
  each row, as it reads the row (`rg_read_next`). A query that
  neither tests nor returns the column reads no long value
  (§12).
- **A long value has one owner, its RG.** No delta row points
  into an RG's file:
  - The delta store keeps its long values in TOAST, in
    RocksDB, as a row store table does.
  - The seal copies each long value from TOAST into the new
    RG's long-value file, and erases the chunks (§10.1).
  - An update of a row in an RG copies the row's long values
    into the TOAST of the new delta row (§9).
  - RG compaction copies the long values of the rows that it
    moves into the new RG's long-value files (§11).

---

## 6. Statistics and pruning

### 6.1 The zonemap of a CG

| Statistic | Encoding | Prunes |
| --- | --- | --- |
| min, max | key bytes; finite values only for the types of `has_nan` and `has_*_inf` | `=`, `<`, `<=`, `>`, `>=`, `BETWEEN`, `IN` |
| inexact bit | min and max cut to 32 bytes; a cut min is still a lower bound; a cut max compares as its prefix's successor | keeps a long `text` from making the RG key large |
| null count | `uint32`; has-null is `> 0`, all-null is `= row count` | `IS NULL`, `IS NOT NULL`, every strict predicate on an all-null CG |
| distinct count | `uint32`, exact when the CG is written | nothing reads it yet |
| `has_nan` | bit; `float4`, `float8`, `numeric` | `x = 'NaN'`, `x <> 'NaN'`, and ranges with finite min and max |
| `has_pos_inf`, `has_neg_inf` | bits; the types of `has_nan`, and `date`, `timestamp`, `timestamptz` | `x = 'infinity'`, and ranges with finite min and max |
| `has_long` | bit: the CG holds a long value (§5.4) | tells the filter's rule of §6.3 |
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

Each CG has one filter key: an exact set or a bloom filter.

- **A long value is in no filter.** It is in min and max, cut
  to 32 bytes as every value is. Out of max, a long value
  `'zzz…'` would let `x > 'y'` skip a CG that holds a match.
- **The filter of a CG with `has_long` does not prune.** The
  size of a long value's key says nothing of the value:
  - A `char(n)` key leaves out the trailing spaces. The long
    value `'a'` padded to 2,000 bytes has the key `'a'`.
  - Under a non-deterministic collation, strings of different
    lengths can be equal.

  Min and max still prune such a CG.
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

Only RG compaction makes an RG of fewer rows: the live rows of
the RGs that it rewrites (§11).

- **The size of a CG needs no limit.** A value of a CG is at
  most 1024 bytes, as a long value is out of line (§5.4). So a
  CG holds at most about 10 MB before encoding. If the row
  limit is raised, a limit on a CG's size comes back with it.
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
  sorted and delta-encoded, except in a merged RG (§5.1).
- **The `_id` CG is like any other CG.** It has a zonemap and a
  filter key (§6), and nothing more. There is no map from `_id`
  to `rg_id`.
- **The `_id` ranges of RGs can overlap.** An updated row has a
  dead copy in an old RG, and a later seal puts its new version
  into a new RG. The delete markers decide which copy is live.

A lookup of `_id` is a scan with the predicate `_id = n`:

1. Read the RG keys at the snapshot.
2. Skip each RG whose `_id` min and max do not hold `n`.
3. Read the delete markers of each RG that is left, into a set.
4. Read the RG's CGs, and walk its `_id` CG row by row to `n`.
   A copy whose `_id` is in the set of step 3 is dead.
5. Read the delta key `n`.

- **Steps 1 and 2 read RocksDB alone.** The `_id` filter is not
  read: `col_find_latest` and `col_read` pass no bound.
- **The read of the RG keys reads every RG key**, so a lookup
  costs in proportion to the RGs of the table. A lookup that
  reads the `_id` filter, finds `n` by a binary search, and
  reads one marker is in [`NEXT.md`](NEXT.md).

**digres carries the `_id` alone**, as for a keyless row store
table. A write takes the `rg_id` from the scan instead:

1. If the scan that read the row is still on it, the write names
   the row `(rg_id, _id)`, with the scan's `rg_id`
   (`sm_row_scan_rg`): no lookup. A plain `UPDATE ... WHERE` or
   `DELETE ... WHERE` is so.
2. Else, as after a join, a sort or a TID scan, the write names
   the row by its `_id`, and sm reads the delta key `n`. If the
   delta store has no version of it, or a seal's tombstone, sm
   looks up the row's RG as above, at the newest committed state.
3. A follow after `SM_ERR_SEALED` or a moved marker is a lookup
   too.

- **`rg_id` is 0 for a row in the delta store.**
- **A read by `_id` is the same**: the delta store, then the
  lookup, at the snapshot, or newest for `sm_tbl_read_latest`.
- **A row lock is on the delta key `_id`** wherever the row is
  (§9.1), so it needs no `rg_id`.

---

## 9. Writes

| Statement | The row is in the delta store | The row is in RG `r` |
| --- | --- | --- |
| `INSERT` | a new `_id`; `insert_new` at its delta key | — |
| `DELETE` | a tombstone at its delta key | a delete marker at `(r, _id)` |
| `UPDATE` | a new version at its delta key | a delete marker at `(r, _id)`, and the new version at delta key `_id` |

- **Every lock and check stays at the row.** A write of a row in
  an RG (`mark_deleted`):
  1. Takes the row's exclusive lock on its delta key `_id`, as
     any write does. A second writer, or a `FOR UPDATE` holder,
     is waited for.
  2. Inserts the delete marker, a versioned key, as a unique
     index entry is. First-committer-wins applies to it as to any
     key.
  3. Meets a marker there already: at READ COMMITTED, the row
     was deleted since (`SM_ERR_NOTFOUND`), or updated since,
     with its new version in the delta store (`SM_ERR_UPDATED`,
     which the caller answers by a read of the newest version).
  4. Meets a moved marker (`SM_ERR_SEALED`): finds the row's new
     RG, and tries there, at most 8 times (§11).
- **An update of a row in an RG writes two keys in one
  transaction.** The delta key may hold a sealed tombstone. The
  insert goes in over it, as over any tombstone.
- **An update of a row in an RG copies its long values** from
  the RG's long-value files into the TOAST of the new delta
  row. The delta row never points into an RG's file (§5.4).
- **A savepoint rollback undoes markers and delta writes** with
  the undo list, as for any key.
- **A wide update copies whole rows into the delta store.** The
  seal later puts them into new RGs.

### 9.1 Locks

Rows move: the seal moves them from the delta store into an RG,
and RG compaction moves them from one RG to another. A lock on a
location misses a write after a move. So every lock names the
`_id`, in the key space of the delta store, `0x0001 ‖
be64(_id)`. That key space is `_id` space.

**Row locks.** `FOR UPDATE` and `FOR SHARE` lock delta key
`_id` with `sm1::lock_row`, wherever the row is. A move never
takes a lock's row away.

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
3. Write the CG files and the long-value files, and put the file
   set key and the filter keys of a new RG (§4.5).
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
- **The memory is one RG**: 10k rows, long values whole, until
  the RG is written.

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
5. Draw a new `rg_id`. Write each CG's file and long-value file,
   and fsync the files and the directory.
6. Put the RG key, and a filter key for each CG.
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
  catalog, with a queue of tables, as the btree reaper has its
  own. A table is queued once until its job starts.
- **The insert path counts, for each table, in memory,** the
  rows of its delta store, an update's new version of a row of
  an RG among them: each transaction counts its own, and
  hands them to the sealer at its commit, so a seal never starts
  before the rows it is to take are committed. At 10k rows the
  table is queued (§7).
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

RG compaction rewrites an RG that has many delete markers. It
puts the live rows into a new RG and erases the old RG key.
It is GC, not a write path.

- **It writes a moved marker at `(old rg, _id)` for each row
  that it moves.** A moved marker is a delete marker with the
  flag `version_sealed`. It is transparent as a sealed
  tombstone is (§10.2): first-committer-wins judges against the
  row's own commit, and a writer follows `_id` to the new RG.
- **The `rg_id` of the new RG is new.** Its files have new
  names. The long values of the rows that it moves are copied
  into the new RG's long-value files.
- **It runs at READ COMMITTED**, as the seal does (§10.3).

**RG compaction cannot skip a row.** It erases the old RG key,
so each row of the old RG must move or be dead. A row with a
writer in flight or prepared is neither yet:

- If RG compaction copies the row and the writer's delete
  commits, the copy is live: a lost delete.
- If RG compaction leaves the row out and the writer aborts, the
  row is gone: a lost row.

So RG compaction (`col_compact`) does these steps, at the newest
committed state:

1. Set a savepoint.
2. For each RG to compact, read its delete markers, then its
   live rows.
3. For each live row, try its exclusive lock. If another
   transaction holds a lock on the row, a reader's `FOR SHARE`
   included, roll back to the savepoint and give up: the next
   markers committed on the RG queue it again.
4. Write a moved marker for the row, and add it to the new RG.
5. Write the CG files, put the new RG key and its filter keys,
   erase each old RG key, and commit. The row locks are held to
   the commit.

- **A writer that comes after the lock waits on it**, then finds
  the moved marker, and follows `_id` to the new RG (§10.2).
- **A transaction that stays prepared for a long time keeps its
  RG from RG compaction** for that time. RG compaction is GC, so
  it can wait.

### 11.1 When RG compaction runs

- **At 20% dead rows.** A commit counts the delete markers it
  committed on each RG, in memory. When they reach 20% of a full
  RG's rows, 2,000, it queues an RG compaction job on the
  sealer's queue (§10.4). Below 20%, the dead rows cost a scan
  less than a rewrite of the RG costs.
- **A scan counts too.** A scan reads each RG's markers into a
  set, so it counts them at no cost, and queues a job when they
  are 20% of that RG's rows. The counts in memory start at 0
  when the database opens, and a scan catches what they miss. A
  scan also catches a small RG, whose 20% is under 2,000.
- **Small RGs are merged.** When the job compacts an RG with
  fewer than 50% of 10k rows live, it takes with it each small
  RG next to it in `rg_id` order, after it and then before it,
  while the live rows of all fit one RG. They go into one new
  RG, by the steps above, with a moved marker for each row
  (`merge_set()` in `sm3/src/colstore/sealer.cc`). RG compaction
  can leave a small RG; a bulk load leaves none (§9.2). Without
  merges, small files collect.
- **An RG with nothing to gain is left.** A job for an RG that
  merges with no other and is less than 20% dead does nothing.
- **A snapshot needs nothing more.** An old snapshot sees the
  old RG key, and not the new one. GC keeps the old RG's files
  until the erase of its key is at or below the watermark
  (§13).

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
  and the column's collation. sm encodes the values to key
  bytes (`sm_row_scanopen_columns`). A qual of another form,
  such as an `OR`, an expression, a `Param` or `LIKE`, gives no
  bound.
- **Pruning is not filtering.** A scan skips RGs that cannot
  match, and hands out every live row of the other RGs. The
  executor of digres applies the whole qual to each row, the
  bounded columns too. So the rule of sm3 holds: a scan filters
  nothing ([`sm3/SM3_DESIGN.md`](sm3/SM3_DESIGN.md) §5).
- **A scan hands out one row at a time** through `sm.h`, as an
  sm2 tuple that sm3 forms from the projected columns' datums,
  with null for a column not read. So a row of more than 64 KB
  fails. Datums handed out with no tuple are in
  [`NEXT.md`](NEXT.md). The row's name is `(rg_id, _id)` (§8).
- **PostgreSQL's table access method has no projection
  argument for a sequential scan.** digres takes the columns
  from the scan's plan: its target list and quals. Its planner
  gives a column store table's scan no physical target list
  (`use_physical_tlist()`), which would name every column.
- **A scan of a table the statement writes or locks reads every
  column.** The write takes the old row from the scan on it.
  digres never takes a row of a scan of some columns as the
  table's row.
- **A batch interface waits for the columnar executor.** It
  changes the rule of sm3 that its interface is one tuple at a
  time, so it is decided with that executor.

A scan at a snapshot:

1. Reads the RG keys at the snapshot.
2. Skips each RG that the zonemaps prove has no match (§6.2).
3. For an RG that is left, reads the filter key of each column
   in an equality or `IN` predicate, and skips the RG if one
   proves there is no match.
4. Reads the CGs that the query needs, from the local cache or
   from S3.
5. Reads the RG's delete markers under `0x0002 ‖ be64(rg_id)`
   into a set, and skips each row whose `_id` is in it.
6. Scans the delta store, as a keyless row store table.

### 12.1 ANALYZE

| Statistic | How |
| --- | --- |
| row count | exact: the row counts of the RG keys, less their visible delete markers, plus the delta store's count; no file is read |
| null fraction | from the sample, as for a row store table |
| the sample, for most common values, histograms and distinct counts | up to 100 RGs at random from the RG keys; from each, a share of the rows wanted in proportion to its live rows, at random places; the delta store read whole, and its share of the rows wanted kept at random, in proportion to its share of the live rows |
| the size, for `relpages` | the bytes of the delta store and of every CG, their long-value files included, from the RG keys; no file is read |
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
  prepared version of an `rg_id`'s RG key decides:
  - a prepared version, or a committed put, keeps the files;
  - a committed erase, or no such version, deletes them. A
    version in flight at the crash is dead.

  No snapshot is older than the open, so a committed erase
  leaves no reader. The sweep also removes the directory of a
  store that is gone.
- **GC deletes the files of an `rg_id` when no RG key version of
  it is committed and visible to a snapshot, or prepared.** Two
  cases reach this rule:
  - the erase of the RG key, by RG compaction, is at or below
    the watermark ([`GC.md`](GC.md));
  - the put of the RG key is dead. The compaction filter or the
    GC worker deletes the files when it drops the dead version.
    This covers a `ROLLBACK PREPARED` after a restart, when the
    transaction's list is gone.
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
| `DROP COLUMN` | no rewrite: a scan ignores the CG of a dropped attnum, and RG compaction leaves it out of a new RG; an attnum is never used again, so an old CG is never taken for a new column |
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

The scenarios `s01` to `s08` in `sm1/mvcc/in`. Each runs twice,
on versioned and on chained stores, as every scenario does. The
driver has the verbs `seal <row>`, an erase with a sealed
tombstone, and `sealput <row> <value>`, a moved marker. The
sessions that seal run at READ COMMITTED.

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
cannot match; a long value with a short key is not skipped by
the filter; a CG of another collation version is not pruned by
value; **pruning never skips a match**, for random CGs and
random bounds, against a test of every value.

### 15.3 sm3: unit tests

`sm3/test/test_colstore.cc`:

1. The kind across a restart; an index refused; a SERIALIZABLE
   transaction refused; a dropped table's sequence.
2. A bulk load: whole RGs, the rest in the delta store; an
   abort.
3. A scan: every row, its columns, and the bounds that skip RGs.
4. An update and a delete of a row in an RG, by `(rg_id, _id)`
   and by `_id` alone; concurrent writers at each level.
5. The seal: no partial RG; a locked row passed over; a writer
   that follows a sealed row; the job at an RG's worth of rows;
   an update's new version counted.
6. RG compaction: the live rows rewritten; the give-up on a
   writer; a writer that follows a moved row; the trigger at
   20%; the merge of small RGs.
7. `ANALYZE`: the sample of RGs and of the delta store.
8. DDL: `TRUNCATE`, `ADD COLUMN`, `DROP COLUMN` and a rewrite.

A test can give its catalog smaller RGs (§3), so that a seal or
an RG compaction comes after a few hundred rows.

### 15.4 rel and digres

`rel/test/test_colstore.rel`, through `sm.h`: a load and a scan
across a restart; SERIALIZABLE and an index refused; a delete by
name and an abort; the seal's job; an update and a delete by
`_id` alone; a scan of some columns, with bounds; an update
counted toward the seal.

`dg/test/columnar.sql`: `CREATE TABLE ... WITH (storage =
columnar)`; the warnings of §14.1, and the `NOT NULL` kept from
an ignored `PRIMARY KEY`; `INSERT ... SELECT` into RGs;
`UPDATE`, `DELETE ... RETURNING` and `SELECT ... FOR UPDATE`;
queries with bounds, whose results equal a row store table's;
`ALTER TABLE` and `TRUNCATE`; `CREATE TABLE AS` and `COPY`; a
SERIALIZABLE transaction refused with `0A000`; `storage =
'row'`.

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

2. **Logical decoding** ([`LOGICALDECODE.md`](LOGICALDECODE.md)),
   which is not built. When it is: an insert into the delta
   store or a bulk load's RG emits an insert for each row; a
   delete marker emits a delete, with the row's values read
   from the RG's CGs; an update emits one update; the seal and
   RG compaction emit nothing, as they change no row.
3. **Deferred features:** column patches for wide updates, a
   `sum` in the zonemap, the byte length of varlena values,
   `LIKE 'abc%'` pruning, RLE, frame-of-reference and FSST.

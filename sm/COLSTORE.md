# COLSTORE: the Column Store

The Column Store is a second kind of table. It keeps a table's
rows column by column, in immutable files, and answers every
query with a scan. It has no primary key and no secondary index.
Each scan skips the parts of the table that its predicate cannot
match, from statistics kept in RocksDB.

**Status.** Design only. Nothing is built. §16 lists the
questions that are still open.

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
| a lookup by `_id` | a scan with `_id = n`, pruned by the `_id` CG's zonemap and bloom filter; no map from `_id` to `rg_id` | §8 |
| the TID that digres carries | `(rg_id, _id)`; `rg_id` 0 for a row in the delta store | §8 |
| locks | row locks and SIREAD locks on delta key `_id`; a read RG locks its `_id` range | §9.1 |
| how digres makes one | `CREATE TABLE ev (...) WITH (storage = column)`; sm3's catalog records the kind | §3 |
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
| the seal | pessimistic; its tombstones are transparent to first-committer-wins; exempt from SSI | §10 |
| RG compaction | moved markers, transparent in the same way | §11 |

The constants (10k rows, 1024 bytes, 32 bytes, 4 KB, 20%, 50%)
may change. An RG key records the real sizes of its CGs,
so a change of a constant does not make an existing file wrong.

**Which level owns each part:**

| Part | Level | Why |
| --- | --- | --- |
| the sealed tombstone; the moved marker's rule in first-committer-wins; the seal's exemption from SSI; a range form of `ssi_write_key` | sm1 | rules of the write path and of SSI; sm1 knows no table |
| file sets: the file I/O of CGs, their key class, their GC and the sweep at open; later S3 and the local file cache | sm1, beside the Buffer Manager | sm1 owns the I/O of every byte, the order of fsync and RG key, and GC (§4.5) |
| the CG file format, the encodings, the dictionary, the zonemap and the filter, and the pruning test on key bytes | sm2 | they need types and `encode()`; the pruning test compares key bytes and knows no type |
| the RG keys, the seal, RG compaction, the scan, the lookup by `_id`, and the locks of §9.1 | sm3 | they need the table, its columns and predicates, as the `tbl_*` calls of a row store table do |

---

## 4. Where bytes live

### 4.1 Files

- **One file for each CG.** A file is written once and never
  changed, renamed, appended to or overwritten.
- **A file's path is `<db home>/colstore/<storeid>/<rg_id /
  1024>/<rg_id>.<attnum>`.** `<attnum>` is the column's attnum
  in the Catalog of sm3, which a column keeps for good, and
  which a dropped column does not give back
  ([`CATALOG.md`](CATALOG.md)). `rg_id`s come from a sequence of
  the table and are never used again. So a path is unique for
  the life of the database. S3's keys will take the same path.
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
| relid `0x0004` | filter keys | `0x00 ‖ 0x0004 ‖ be64(rg_id) ‖ be16(col) ‖ ~wts ‖ ~seq` | versioned |
| relid `0xFFFF` | TOAST of delta rows | as in a row store table | versioned |
| class `0x03` | RG keys: sm1's file set keys | `0x03 ‖ be64(rg_id) ‖ ~wts ‖ ~seq` | versioned, of sm1's file set class |

- **RG keys and filter keys are apart**, so a scan of RG keys
  reads no filter key.
- **A snapshot's set of RGs is a scan of class `0x03` at the
  snapshot.** It needs no visibility rule of its own.
- **An RG key and its filter keys are put once and erased
  once.** The seal or RG compaction puts them. RG compaction
  or `TRUNCATE` erases them. Nothing changes them between.
- **The key of a delete marker holds the `rg_id`.** One `_id`
  can have copies in more than one RG: a dead copy in an older
  RG, and the live copy in a newer RG.

### 4.4 The value of an RG key

| Field | For each |
| --- | --- |
| row count | RG |
| `_id` min and max | RG |
| file size | CG |
| CRC32C of the file | CG |
| size and CRC32C of the long-value file, if any | CG |
| encoding | CG |
| zonemap (§6.1) | CG |

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
6. At open, scan class `0x03` of each store, and delete the
   files that no version names (§13).

- **sm3 never uses a file path.** It reads a CG through sm1, by
  `rg_id` and attnum.
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
| dictionary | any type with repeated values (§5.2) |
| delta, bit-packed | sorted integers; the `_id` CG always |

**A CG holds no TOAST pointer.** A value of 1024 bytes or less
is in the CG. A long value is in the CG's long-value file
(§5.4), and the CG holds a pointer to it.

After the encoding, the CG is compressed with zstd. The tail
tags the encoding, so a new encoding does not change the format
of an existing file.

### 5.2 The dictionary

- **Each CG has its own dictionary**, in the file. It never
  changes, and it needs no MVCC, no lock and no GC.
- **The dictionary is sorted in key order.** The codes keep the
  order of the values. A range predicate becomes a range of
  codes, found by two binary searches in the dictionary.
- **The values are bit-packed codes.**
- **The seal chooses for each CG.** It uses a dictionary if the
  dictionary and its codes are smaller than plain.
- **A predicate is evaluated once for each distinct value.**
  That gives a bitmap of the codes that match. The scan then
  tests codes, not values.

### 5.3 The layout of a file

A CG file holds, in this order:

1. The values, encoded (§5.1) and compressed with zstd.
2. The dictionary, if the CG has one.
3. The filter: the exact set or the bloom filter (§6.3).
4. The zonemap (§6.1).
5. A tail of fixed size: the offset of each part above, the
   encoding tag, a format number, the row count, the CRC32C of
   the file before the tail, and a magic number.

- **A file describes itself without RocksDB.** The tail is read
  with one small read, or one ranged GET from S3.
- **The RG key also holds each file's CRC32C** (§4.4). RocksDB
  is the authority, so the key says what the file must be.
- **sm1 checks the CRC32C on each read from disk or S3** into
  memory, not on a hit in memory. It costs one pass at memory
  speed, which is far less than zstd's decompression.
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

- **A CG holds a pointer of about 16 bytes in its place**:
  `(offset, length)` in the CG's long-value file. Only the
  pointer counts toward the size of the CG (§7), so an RG of
  long values still holds 10k rows.
- **The long-value file is `<rg_id>.<attnum>.long`**, in the
  RG's file set (§4.5). It lives and dies with the RG, and goes
  to S3 with the other files. A CG with no long value has none.
- **The seal writes the long values into the file as it adds
  rows.** It does not hold them in memory. The file has no size
  target.
- **Each long value is stored with its own CRC32C.** A read of
  one value is a ranged read, or one ranged GET from S3, and
  checks that CRC32C. The CRC32C of the whole file (§5.3) is
  for a read of the whole file.
- **digres gets an external pointer datum** and fetches the
  value only when it uses it, as PostgreSQL does with TOAST. A
  query that neither tests nor returns the column reads no long
  value.
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
| distinct count | `uint32`, exact at the seal | sizes the filter; the planner |
| `has_nan` | bit; `float4`, `float8`, `numeric` | `x = 'NaN'`, `x <> 'NaN'`, and ranges with finite min and max |
| `has_pos_inf`, `has_neg_inf` | bits; the types of `has_nan`, and `date`, `timestamp`, `timestamptz` | `x = 'infinity'`, and ranges with finite min and max |
| `has_long` | bit: the CG holds a long value (§5.4) | tells the filter's rule of §6.3 |
| `sorted` | bit: the CG is in ascending key order | a binary search in the CG; an early stop |
| collation version | for each collated column | turns pruning off for a CG whose version is not the column's |

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
  pruning for the CG.** The min and max of `text` begin with
  the collation's sort key, and a new ICU version can change
  it. Without this rule, a scan would skip rows that match.
- **`min` and `max` can answer `min()` and `max()`** of an RG
  with no visible delete markers when the inexact bit is
  clear.

### 6.3 The filter of a CG

Each CG has one filter key: an exact set or a bloom filter.

- **A long value is in no filter.** It is in min and max, cut
  to 32 bytes as every value is. Out of max, a long value
  `'zzz…'` would let `x > 'y'` skip a CG that holds a match.
- **A filter of a CG with `has_long` prunes only for a constant
  of 1024 bytes or less, under a deterministic collation.** A
  long value cannot equal such a constant. Under a
  non-deterministic collation, strings of different lengths can
  be equal, so the filter does not prune.
- **An exact set** when the key bytes of the CG's distinct
  values total 4 KB or less. For a CG with a dictionary, the
  set is the dictionary. It answers `=` and `IN` with no false
  positive.
- **A bloom filter** otherwise: 10 bits for each distinct value,
  for 1% false positives, over a hash of the key bytes. At 10k
  rows it is at most 12.5 KB.
- **The hash of a non-deterministic collation's key covers only
  the sort key part.** The key also holds the original string,
  and `'a' = 'A'` under a case-insensitive collation must hash
  the same.
- **Every column has a filter.** There is no option and no GUC.

---

## 7. The size of an RG

**An RG holds 10k rows.** The seal closes an RG when it has
10k rows. An RG has fewer only when the seal finds fewer rows
that it can take: rows skipped for a writer in flight (§10.1),
or the last RG of a bulk load.

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
- **Each RG stores its rows' `_id`s as a hidden CG**, sorted,
  delta-encoded.
- **The `_id` CG is like any other CG.** It has a zonemap and a
  filter key (§6), and nothing more. There is no map from `_id`
  to `rg_id`.
- **The `_id` ranges of RGs can overlap.** An updated row has a
  dead copy in an old RG, and a later seal puts its new version
  into a new RG. The delete markers decide which copy is live.

A lookup of `_id` is a scan with the predicate `_id = n`:

1. Read the RG keys at the snapshot.
2. Skip each RG whose `_id` min and max do not hold `n`.
3. Skip each RG whose `_id` bloom filter does not hold `n`.
4. Read the `_id` CG of each RG that is left, and find `n` by a
   binary search.
5. Read the RG's delete marker at `(rg_id, n)`, if any, to
   decide if this copy is live.
6. Read the delta key `n`.

- **Steps 1 to 3 read RocksDB alone.** A bloom filter's false
  positive costs one CG read.
- **The read of the RG keys reads every RG key**, so a lookup
  costs in proportion to the RGs of the table. Wide `_id`
  ranges, as of an RG of reinserted rows, leave the bloom
  filter to do the work.

**The TID that digres carries is `(rg_id, _id)`.** `rg_id` is 0
for a row in the delta store. A scan knows the RG of each row it
returns, so an `UPDATE` or a `DELETE` that a scan drives writes
`(rg_id, _id)` with no lookup. Only a follow after
`SM_ERR_SEALED` or a moved marker is a lookup.

---

## 9. Writes

| Statement | The row is in the delta store | The row is in RG `r` |
| --- | --- | --- |
| `INSERT` | a new `_id`; `insert_new` at its delta key | — |
| `DELETE` | a tombstone at its delta key | a delete marker at `(r, _id)` |
| `UPDATE` | a new version at its delta key | a delete marker at `(r, _id)`, and the new version at delta key `_id` |

- **Every lock and check stays at the row.** A delete marker is
  a versioned key, as a unique index entry is. An uncommitted
  marker is the row's lock. A second writer waits on it, and
  first-committer-wins and the RC exchange apply to it as to
  any key.
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

**SIREAD locks.** These sm1 calls give each one:

| Event | SIREAD lock or check |
| --- | --- |
| a scan of the delta store | its range, as `sm1::scan` registers it |
| a scan reads RG `r` | `sm1::ssi_read_range` over delta keys `[_id min of r, _id max of r]` |
| a scan skips RG `r` by pruning | none |
| an update of a row in an RG | none more: the reinsert writes delta key `_id` |
| a delete of a row in an RG | `sm1::ssi_write_key` of delta key `_id`, with the marker; a check with no RocksDB write |
| the seal, RG compaction | none (§10.3) |

- **A skipped RG needs no lock.** Its zonemap proves that no row
  of it matches. A write that makes a row of it match is an
  update, and its reinsert writes delta key `_id`, which the
  scan's delta range covers.
- **A scan merges adjacent ranges as it goes.** RGs that seals
  made in `_id` order have ranges that touch, so a full scan
  registers one range or a few. Each write walks a store's
  ranges in a list ([`XCTMGR.md`](XCTMGR.md) §7.7), so one
  range for each RG would cost too much.
- **An RG of reinserted rows has a wide `_id` range.** A lock on
  it costs false serialization failures, which can be retried.
  RG compaction narrows the range. PostgreSQL takes a lock on
  the whole relation for a sequential scan, so this lock is
  finer than PostgreSQL's.
- **A unit smaller than an RG is of no use.** A scan reads a
  whole RG.

An example of the rule:

```sql
-- R, SERIALIZABLE, snapshot 100
SELECT * FROM ev WHERE kind = 'x';
    -- reads delta row 900: SIREAD on the delta range
-- The seal commits at 105: row 900 goes to RG 1.
-- W
DELETE FROM ev WHERE _id = 900;
    -- the marker (rg 1, _id 900), and ssi_write_key of delta
    -- key 900: R's range covers it, so the edge R -rw-> W is
    -- made. A lock on RG 1 would miss it: R's snapshot does
    -- not see RG 1.
```

### 9.2 A bulk load

`COPY`, `CREATE TABLE AS`, `INSERT ... SELECT` and
`rewrite_table` write RGs directly, in the transaction of the
statement. They do not pass through the delta store.

1. Draw 10k `_id`s from the table's sequence, as one batch.
2. Add 10k rows to the CGs being built. Write each long value
   into its long-value file as the row is added.
3. Write the CG files, and put the file set key and the filter
   keys of a new RG (§4.5).
4. Report a write over the RG's `_id` range in the delta key
   space to SSI.
5. Repeat until fewer than 10k rows are left.
6. Insert the rows that are left into the delta store, as
   `INSERT` does. A later seal takes them.

- **A table with rows can take a bulk load.** A new RG
  conflicts with no key that is there, so the load takes no
  lock beyond what an `INSERT` takes.
- **An RG's `_id`s are contiguous**, as they come from one
  batch. So its `_id` range is tight.
- **The report to SSI makes a load a phantom** for a
  SERIALIZABLE scan whose range of delta keys covers the RG's
  `_id`s (§9.1). It needs a form of `sm1::ssi_write_key` that
  takes a range of keys.
- **An abort or a rollback to a savepoint** unlinks the files,
  from the transaction's list, and the undo list removes the
  keys (§13).
- **The memory is one RG**: 10k rows of at most 1024 bytes a
  value in a CG. Long values go to their files as they come.

---

## 10. The seal

### 10.1 The steps

The seal moves rows from the delta store into a new RG. It is a
transaction of its own.

1. Draw a new `rg_id`.
2. Read delta keys in `_id` order. Erase each row with a sealed
   tombstone. The erase gives back the exact row it replaced.
   Skip a row whose writer is in flight or prepared. The row
   stays in the delta store, and a later seal takes it.
3. Detoast each value of the row. Write each long value into
   the CG's long-value file, and keep its pointer for the CG.
   Erase the row's TOAST chunks, as a delete does. A snapshot
   older than the seal's commit still sees the chunks.
4. Add each row to the CGs being built. Stop at 10k rows
   (§7).
5. Write each CG's file, and fsync the files and the directory.
6. Put the RG key, and a filter key for each CG.
7. Commit.

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

### 10.3 SSI

**The seal's writes do not feed SSI.** A reader of a delta row
would otherwise get an rw-edge to the seal. A move that changes
nothing could then make the reader the pivot of a dangerous
structure, and fail it.

### 10.4 When the seal runs

- **A seal job runs on the builder queue of sm3**, the queue
  and the worker threads that build indexes. A table has at
  most one seal job waiting.
- **The insert path counts, for each table, in memory,** the
  rows of its delta store. It queues a seal job when the count
  reaches 10k rows (§7).
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
- **Its writes do not feed SSI**, for the reason of §10.3.

**RG compaction cannot skip a row.** It erases the old RG key,
so each row of the old RG must move or be dead. A row with a
writer in flight or prepared is neither yet:

- If RG compaction copies the row and the writer's delete
  commits, the copy is live: a lost delete.
- If RG compaction leaves the row out and the writer aborts, the
  row is gone: a lost row.

So RG compaction does these steps:

1. Scan the old RG's delete markers at their newest versions.
2. If a marker's writer is in flight or prepared, stop, and try
   the RG again later. Nothing is written yet.
3. Otherwise, write the moved markers, the CG files, the new RG
   key and its filter keys, erase the old RG key, and commit.

- **A writer that starts after the scan of the markers waits
  on a moved marker**, and then follows `_id` to the new RG
  (§10.2).
- **A transaction that stays prepared for a long time keeps its
  RG from RG compaction** for that time. RG compaction is GC, so
  it can wait.

### 11.1 When RG compaction runs

- **At 20% dead rows.** The path that writes delete markers
  counts the committed markers of each RG in memory. When they
  reach 20% of the RG's rows, it queues an RG compaction job on
  the builder queue of sm3. Below 20%, the dead rows cost a scan
  less than a rewrite of the RG costs.
- **A scan counts too.** A scan reads each RG's markers to
  merge them with the `_id` CG, so it counts them at no cost,
  and queues a job at 20%. The counts in memory start at 0 when
  the database opens, and a scan catches what they miss.
- **Small RGs are merged.** An RG with fewer than 50% of 10k
  rows is merged with RGs near it in `rg_id` into one new RG,
  by the steps above, with a moved marker for each row. A bulk
  load leaves a partial last RG, and RG compaction can leave a
  small one. Without merges, small files collect.
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

- **The planner of digres derives the bounds** from quals of the
  form `Var op Const` and `Var op Param`, as it derives the key
  range of an index. sm2 encodes them to key bytes. A qual of
  another form, such as an `OR` across columns, an expression
  or `LIKE`, gives no bound.
- **Pruning is not filtering.** A scan skips RGs that cannot
  match, and hands out every live row of the other RGs. The
  executor of digres applies the whole qual to each row, the
  bounded columns too. So the rule of sm3 holds: a scan filters
  nothing ([`sm3/SM3_DESIGN.md`](sm3/SM3_DESIGN.md) §5).
- **A scan hands out one row at a time** through `sm.h`, as the
  datums of the projected columns, into the virtual slot of
  digres. It builds no sm2 tuple. The row's TID is `(rg_id,
  _id)` (§8).
- **PostgreSQL's table access method has no projection
  argument for a sequential scan.** digres adds one.
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
5. Reads the RG's delete markers under `0x0002 ‖ be64(rg_id)`,
   in `_id` order, and merge-joins them with the `_id` CG.
6. Scans the delta store, as a keyless row store table.

### 12.1 ANALYZE

| Statistic | How |
| --- | --- |
| row count | exact: the row counts of the RG keys, less their visible delete markers, plus the delta store's count; no file is read |
| null fraction | exact: the null counts of the zonemaps; no file is read |
| the sample, for most common values, histograms and distinct counts | up to 100 RGs at random from the RG keys; from each, the analyzed columns, and about `300 × target / 100` live rows at random places; the delta store sampled as a keyless table ([`STATISTICS.md`](STATISTICS.md) §3.3), in proportion to its share of the rows |
| the estimators | those of sm3, on the sample, as for a row store table |

- **The cost of a sample is in RGs, not in rows.** A read of one
  row of a CG reads and decompresses the whole CG.
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
  When the database opens, a sweep of the store's directories
  deletes each file whose `rg_id` has no RG key version that is
  committed or prepared. A version in flight at the crash is
  dead. A prepared transaction's RG keys name its files, so the
  sweep keeps them.
- **GC deletes the files of an `rg_id` when no RG key version of
  it is committed and visible to a snapshot, or prepared.** Two
  cases reach this rule:
  - the erase of the RG key, by RG compaction or `TRUNCATE`, is
    at or below the watermark ([`GC.md`](GC.md));
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
  to S3. A temporary store takes no SIREAD lock.

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
existing scenario of `sm1/mvcc` changes. Each new test opens with
the one line that says what it proves.

### 15.1 mvcc: the rules of sm1

New scenarios in `sm1/mvcc/in`, a series of their own. Each runs
twice, on versioned and on chained stores, as every scenario
does: the sealed tombstone is the chained form of the rule, and
the moved marker the versioned form. The driver gets a verb
`seal <row>`, an erase with a sealed tombstone, and a session
kind whose writes do not feed SSI, for the seal and RG
compaction.

| Scenario | Expected |
| --- | --- |
| an RR writer, whose snapshot is older than the seal of its row, writes the row | `SEALED`, not `CONFLICT` |
| an RR writer, whose row changed after its snapshot and was then sealed, writes the row | `CONFLICT`: the change under the tombstone decides |
| an RC writer waits on a seal in flight, and the seal commits | `BLOCK`, then `SEALED` |
| an RC writer waits on a seal in flight, and the seal aborts | `BLOCK`, then the write succeeds |
| a reader before and after the seal's commit | the row, then no row |
| an insert over a sealed tombstone | `ok` |
| a SERIALIZABLE reader of a row, then a seal of it | no rw-edge: the reader commits |
| the same with a plain erase (negative) | the reader fails with a serialization failure |
| a seal meets a writer in flight, or prepared | the seal skips the row; the writer is not blocked |
| a write over a range that a SERIALIZABLE scan read, by the range form of `ssi_write_key` | an rw-edge; out of the range, none |

The Column Store's notions above sm1, such as RGs, scans and
pruning, cannot be expressed here. §15.3 and §15.4 test them.

### 15.2 sm1 and sm2: unit tests

`sm1/test`, file sets (§4.5, §13):

1. Write a file set, put its key, commit, and read the files.
2. Abort, and roll back to a savepoint: the files are unlinked.
3. Close the database with a file set's key in flight, and open
   it: the sweep deletes the files.
4. Prepare, close, and open: the files stay. `COMMIT PREPARED`
   keeps them; `ROLLBACK PREPARED` lets GC delete them.
5. Erase a key while an old snapshot is open: the snapshot
   reads the files; past the watermark, GC deletes them.
6. Change a byte of a file: a read fails with a data corruption
   error. The same for one long value.
7. Drop the store: its directory goes.

`sm2/test`, the CG file and the statistics (§5, §6):

1. Write and read a CG of each type, in each encoding: plain,
   dictionary and delta.
2. The choice of a dictionary when it is smaller; a range of
   codes from a range of values.
3. Min and max as key bytes, cut at 32 bytes with the inexact
   bit; null counts; the NaN and infinity flags; `sorted`; the
   collation version.
4. The exact set at 4 KB and less, and a bloom filter above; the
   hash of a non-deterministic collation's key.
5. Long values: the pointer, the long-value file, `has_long`
   and its rule.
6. **Pruning never skips a match.** For random CGs and random
   bounds, compare each skip against a test of every value.

### 15.3 sm3: unit tests

1. The seal: an RG of 10k rows; a row with a writer in flight
   or prepared skipped; TOAST chunks erased; the RG key and the
   filter keys put.
2. A snapshot before a seal and one after see each row once.
3. A scan: the projection, the bounds, the merge of delete
   markers, and the delta store.
4. An update and a delete of a row in an RG: the delete marker,
   the reinsert, the TID `(rg_id, _id)`, and the follow of `_id`
   after `SEALED`.
5. The locks of §9.1: a row lock and a SIREAD range on delta key
   `_id`, across a seal and across RG compaction.
6. RG compaction: the trigger at 20%; the merge of small RGs;
   the moved markers and their follow; the give-up on a writer
   in flight or prepared.
7. A bulk load: whole RGs, the rest in the delta store, an
   abort, and a rollback to a savepoint.
8. `ANALYZE`: the exact row count and null fraction; the sample.
9. DDL: `TRUNCATE`; `ADD COLUMN` with no CG in old RGs; `DROP
   COLUMN`; a rewrite.
10. **A column store table agrees with a row store table.** Run
    one random workload on both, at each isolation level and
    with seals and RG compactions between, and compare every
    scan.

### 15.4 rel and digres

`rel/test`, through `sm.h`: a column store table across
sessions; a seal between statements; an abort; a new attach,
after which the sweep runs; 2PC across a restart.

`dg/test`:

- `.sql` files for `run.sh`: `CREATE TABLE ... WITH (storage =
  column)`; the warnings of §14.1, and the `NOT NULL` kept from
  an ignored `PRIMARY KEY`; a foreign key that references a
  column store table, which fails; queries with bounds, whose
  results equal a row store table's; `COPY`; `TRUNCATE`;
  `ALTER TABLE`; `ANALYZE`.
- `server.sh`: an RR update while a seal runs, which succeeds;
  a SERIALIZABLE scan and a concurrent bulk load, which make a
  phantom.

### 15.5 Layers with no test

- **aa** has no change.
- **sq** has no change: `WITH (storage = column)` is a
  reloption, which its grammar parses already.
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

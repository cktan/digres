# COLSTORE_PLAN — building the Column Store

Status: steps 1 to 13 are built; step 14, the documents, is
left. Step 13 reads the option `storage` in
`access/sm/smstore.c`, as dg's tables keep no reloptions, and
has no `ANALYZE` test: `run.sh`'s tables are temporary, and sm
does not analyze those.
[`COLSTORE.md`](COLSTORE.md) is the design.
This file says in what order to build it, where each part goes,
and how each step is checked.

**Warning: no PostgreSQL code goes into `sm/`.** Write the CG
format, the encodings, the bloom filter and the file sets from
scratch.

---

## 1. How the work is cut

- **Bottom up, one level at a time.** sm1 first, then sm2, then
  sm3, then the C API, then digres. Each step builds on steps
  that are done and tested.
- **One step, one branch.** A step is built on a branch
  `wip-colstore-N`, and merged into `main` when the user says
  so.
- **Each step is a full plan, act and validate cycle.** The next
  step starts only when the last one passes its checks.
- **A module that nothing calls yet is tested by its unit
  tests.** The full suites (sm, then sq, then dg) run before
  each commit from the step that wires the Column Store into
  digres (step 13), and in the step that changes a path a row
  store table uses: the transparent move (step 2).
- **The row store must not get slower.** The transparent move
  (step 2) changes sm1's write path. It compares the
  instructions for each statement of the row store benchmark,
  before and after, with `perf stat` on the release build.

---

## 2. Decisions

The user took these on 2026-11-19.

| Decision | Decided | Not taken |
| --- | --- | --- |
| the order of steps 5 to 13 | as below: every part of sm3, then the C API, then digres | a thin slice first: a table, a bulk load, a scan with no pruning, end to end through digres; then the rest in sm3. It shows the whole path early, but each later step touches every level again |
| one flag for both moves | `version_sealed`, `0x40`, on a sealed tombstone (a chained key) and on a moved marker (a versioned key); one rule in the write path | two flags; they would have the same rule |
| zstd | link `libzstd`, which RocksDB links already | none: the design needs it |
| the builder queue | give a job a kind: an index build, a seal, or RG compaction; built instead as a worker of its own, the sealer, pending [`../QUESTION.md`](../QUESTION.md) question 2 | a second queue and its own threads |
| the bloom filter | sm's own, in sm2, over a 64-bit hash of key bytes | RocksDB's filter code, which is not a public interface |

---

## 3. The steps

### Step 1: confirm the starting state

1. Pull `main`, and build the debug build of every level.
2. Run the suites of sm, sq and dg, each to its own log under
   `bench/build`, and keep the logs.
3. Confirm the values that later steps take: key class `0x05`
   and version flag `0x40` are unused in `sm1/src/version.h` and
   `sm1/src/chain.h`; relids `0x0002` to `0x0004` are free in a
   table with no index.
4. Record the row store benchmark's instructions for each
   statement, for the transparent move (step 2).

→ verify: every suite passes, or its failures are the known
ones of `main`. The values are free.

### Step 2: sm1, the transparent move

The sealed tombstone and the moved marker
([`COLSTORE.md`](COLSTORE.md) §10.2, §11).

1. Add `version_sealed` (`0x40`) to `sm1/src/version.h`, and
   `SM_ERR_SEALED` to `include/sm_error.h`.
2. Add `sm1::erase_sealed`, the erase of a chained key with a
   sealed tombstone, beside `sm1::erase_moved`, and the put of a
   versioned key with the flag.
3. In the write path (`sm1/src/access.cc`, `chain.cc`), make the
   check of a newer commit judge a flagged version by the
   version under it. A write that passes the check over a
   flagged version fails with `SM_ERR_SEALED`.
4. Make the Garbage Collector keep the flag when it stamps, and
   treat the version as a moved tombstone.
5. Add the driver verb `seal <row>` to `sm1/mvcc/driver`, and
   the scenarios of [`COLSTORE.md`](COLSTORE.md) §15.1 that test
   the write path, as a new series.

→ verify: the new scenarios pass on versioned and on chained
stores; every existing scenario passes unchanged; the full
suites pass; the row store's instructions for each statement do
not change.

### Step 3: sm1, file sets

[`COLSTORE.md`](COLSTORE.md) §4.5 and §13.

1. Add key class `0x05`, a versioned key of a file set, to the
   comparator and the prefix extractor.
2. Write the files of a file set under `<db home>/colstore/`,
   through sm0's VFD, and fsync each file and its directory.
3. Put and erase a file set key in a transaction.
4. Keep a list of the files a transaction wrote. Unlink them at
   an abort and at a rollback to a savepoint.
5. Read a whole file, and a range of one, and check the CRC32C.
6. Delete the files in GC: in the compaction filter and the GC
   worker, when a put is dead, and when an erase is at or below
   the watermark.
7. Sweep the store's directories when the database opens.
8. Remove a store's directory when sm1 drops the store.
9. Write `sm1/test` tests for each of the seven cases of
   [`COLSTORE.md`](COLSTORE.md) §15.2.

→ verify: the new unit tests pass, under the thread sanitizer
too. Nothing calls file sets yet, so the unit tests suffice.

### Step 4: sm2, the CG file and its statistics

[`COLSTORE.md`](COLSTORE.md) §5 and §6.

1. Write the encodings: plain, dictionary with bit-packed codes,
   and delta for sorted integers; then zstd.
2. Write the file's layout and its tail, and the long-value
   file, with a CRC32C for each long value.
3. Build the zonemap of a CG: min and max as key bytes cut at 32
   bytes, the null count, the distinct count, the flags, and
   the collation version.
4. Build the filter: the exact set, or the bloom filter.
5. Write the pruning test: given the bounds of one column and a
   CG's statistics, skip or read.
6. Write the `sm2/test` tests of §15.2, with the property test:
   pruning never skips a match.

→ verify: the unit tests pass. Nothing calls sm2's new code
yet.

### Step 5: sm3, the catalog

1. Record the table's kind, row store or column store, in its
   `TABLE` entry, and make the sequence `sm3.rg.<oid>`.
2. Refuse an index, a primary key and a unique key on a column
   store table. digres ignores them before they reach sm3
   (step 13, digres), so sm3 only refuses.
3. Refuse a read and a write of a column store table in a
   SERIALIZABLE transaction ([`COLSTORE.md`](COLSTORE.md) §9.1).
4. Test the kind across a restart, and the refusals.

→ verify: the sm3 unit tests pass.

### Step 6: sm3, inserts and bulk loads

[`COLSTORE.md`](COLSTORE.md) §9 and §9.2.

1. Insert a row into the delta store: the keyless row store as
   it is, under relid `0x0001`.
2. Write RGs from a bulk load: `_id`s in batches of 10k, CG
   files and long-value files, the file set key, and the filter
   keys. Put the rest into the delta store.
3. Test an abort and a rollback to a savepoint of a load.

→ verify: the sm3 unit tests of a load pass.

### Step 7: sm3, the scan

[`COLSTORE.md`](COLSTORE.md) §12.

1. Read the RG keys at the snapshot, and prune by the bounds.
2. Read the projected CGs through sm1, and merge the delete
   markers with the `_id` CG.
3. Scan the delta store after the RGs.
4. Hand out one row at a time, as datums.
5. Test the scan, and start the test that a column store table
   agrees with a row store table, for inserts and loads.

→ verify: the sm3 tests pass; the agreement test passes at READ
COMMITTED and REPEATABLE READ.

### Step 8: sm3, updates and deletes

[`COLSTORE.md`](COLSTORE.md) §8, §9 and §9.1.

1. Name a row by `(rg_id, _id)`, with `rg_id` 0 in the delta
   store.
2. Delete a row in an RG with a marker.
3. Update a row in an RG: the marker, and the reinsert with its
   long values copied into TOAST.
4. Look up `_id` by a scan with `_id = n`, and follow it after
   `SM_ERR_SEALED`.
5. Lock a row on its delta key.
6. Add updates and deletes to the agreement test.

→ verify: the sm3 tests pass; the agreement test passes with
updates and deletes.

### Step 9: sm3, the seal

[`COLSTORE.md`](COLSTORE.md) §10.

1. Add the seal's job: the sealer, a worker thread for each
   catalog (`sm3/src/colstore/sealer.cc`), as the btree reaper
   has, pending [`../QUESTION.md`](../QUESTION.md) question 2.
2. Seal by the steps of §10.1: sealed tombstones, the skip of a
   writer in flight or prepared, TOAST chunks erased, the RG
   written, one transaction for each RG.
3. Count delta rows in the insert path, and in a scan, and queue
   a seal at 10k.
4. Add seals, between and during statements, to the agreement
   test.

→ verify: the sm3 tests pass; the agreement test passes with
seals; the RR and RC cases of §15.1 hold for a whole table.

### Step 10: sm3, RG compaction

[`COLSTORE.md`](COLSTORE.md) §11.

1. Add the RG compaction job: the check of the markers first,
   then moved markers, the new RG, and the erase of the old RG
   key.
2. Count committed markers for each RG, and queue a job at 20%.
3. Merge RGs with fewer than 5k rows.
4. Add RG compaction to the agreement test.

→ verify: the sm3 tests pass; the agreement test passes with
RG compaction; a writer in flight makes RG compaction give up.

### Step 11: sm3, ANALYZE and DDL

[`COLSTORE.md`](COLSTORE.md) §7, §12.1 and §14.

1. Give the size and the row count from the RG keys, for
   `relpages` and `reltuples`.
2. `ANALYZE`: the exact counts, and the sample of RGs.
3. `TRUNCATE`, `ADD COLUMN` with no rewrite, `DROP COLUMN`, and
   a rewrite.

→ verify: the sm3 tests of §15.3 all pass.

### Step 12: the C API, and rel

1. Add to `include/sm.h` and `src/sm.cc`: the table's kind at
   create, a scan with a projection and bounds, and the row
   name `(rg_id, _id)`.
2. Document the calls in [`SM_API.md`](SM_API.md).
3. Write the `rel/test` tests of §15.4, with transactions
   indented as `.rel` files indent them.

→ verify: `make test` of sm passes, the rel tests included.

### Step 13: digres

1. Add the reloption `storage`, with the values `row` and
   `columnar`, in `access/common/reloptions.c`.
2. Choose the routine by the table's kind in
   `access/sm/smtable.c`.
3. Ignore a primary key, a unique key, an exclusion, a foreign
   key and `CREATE INDEX` on a column store table, with a
   warning; keep the `NOT NULL` of a primary key.
4. Pass the projection, and the bounds from the quals of a
   sequential scan, to the scan.
5. Name a row of a row group in a write: the scan's `rg_id` when
   the scan is on the row, else sm's lookup by `_id`
   ([`COLSTORE.md`](COLSTORE.md) §8); the `RowKey` carries the
   `_id` alone.
6. Make `COPY`, `CREATE TABLE AS` and `INSERT ... SELECT` bulk
   loads of a column store table, with rows or without.
7. Write the `dg/test` tests of §15.4.

→ verify: the full suites pass, sm, then sq, then dg.

### Step 14: the documents

1. Change [`COLSTORE.md`](COLSTORE.md) to describe the code as
   it stands, with its code paths, and remove its status line.
2. Update [`GLOSSARY.md`](GLOSSARY.md),
   [`README.md`](README.md), [`NEXT.md`](NEXT.md) and
   [`FEATURELIST.md`](FEATURELIST.md).
3. Delete this file.

→ verify: no document says that the Column Store is not built.

---

## 4. What is not in this plan

The open questions and the deferred features of
[`COLSTORE.md`](COLSTORE.md) §16: S3 and the local file cache,
logical decoding, column patches, and the encodings and
statistics listed there. The columnar executor is a project of
its own.

# Questions

## 6. COLSTORE2: what versioned compaction keeps of the old one (2026-11-21)

**Branch:** `wip-colstore2-1`.

**Question.** RG compaction now writes the next version of a row
group in place (`COLSTORE2.md` §6.7). Three parts of the old
compaction stay for now. Are they to go next, in this order?

```sql
-- ev: rg 7, version 0, 10,000 rows; 2,000 deleted.
-- The sealer compacts rg 7: version 1, fsid(7, 1), 8,000 rows,
-- renumbered; no moved marker. A FOR SHARE holder of one of its
-- rows makes the compaction give up, as before.
```

| Kept | Why | What would replace it |
| --- | --- | --- |
| a try of each live row's lock on delta key `_id` | row locks stay on `_id` (question 5), so a `FOR SHARE` holder still holds compaction off, as the tests of it say | nothing needed: a renumbering moves no `_id` lock; drop the tries, and change those tests |
| the old path, new row groups and moved markers, for a merge of small row groups and for version 3 | elimination (`COLSTORE2.md` §6.8) needs the moved-in version in sm1 | elimination, with `sm1::chain_insert_moved_in` and its mvcc scenarios |
| the markers and validmap of an old version, never erased | the cleanup (§6.9) needs `sm1::fileset_settled` | the cleanup pass of the sealer |

**Recommendation, built:** keep all three for this step: each is
correct, and the old path is what the tests cover. Next: the
cleanup, as it is small and ends a leak; then elimination; then
drop the row-lock tries.

## 5. COLSTORE2: the place at insert waits (2026-11-21)

**Branch:** `wip-colstore2-1`, from `main` at `7e954535`.

**Question.** The plan of `sm/COLSTORE2.md` §9 draws a row's
place, `(rg_id, rno)`, at its insert, as its third step. A delta
row's value then begins with its place (§4). That changes the
value of every delta row, which sm2's row store paths read for
the column store and the row store alike: `tbl_read`, the scans,
TOAST, the seal. The other way is a second key for each delta
row, which costs every insert one more write. Is the place at
insert to be built before the validmap, the RG locks and the
versions?

```sql
-- With the place at insert: _id 900 is at (rg 7, rno 899) from
-- its INSERT; the seal keeps it, and its lock's name with it.
-- Without it: the row has no rno until the seal; its lock is on
-- delta key _id 900, before the seal and after it.
```

| Option | What changes | Cost |
| --- | --- | --- |
| A. Build it later; row locks stay on delta key `_id` | nothing in the delta store | the seal still try-locks each row and passes over a locked one; an insert still waits on nothing |
| B. Build it now, the place at the head of the value | every read of a delta row strips 12 bytes | a change across sm2's and sm3's row paths |
| C. Build it now, a key of its own for each delta row | the insert writes a second key | one more RocksDB write for every insert, a hot path |

**Recommendation, built:** A. The validmap, the fold, the RG
locks and versioned compaction need no place at insert. A row
lock on delta key `_id` does not move when RG compaction
renumbers a row group, so versioned compaction keeps every row
lock as it is. The steps built on this branch are, in order: the
place in the scan and the writes (§6.11) with markers keyed by
`(fsid, rno)`; then the RG lock, the validmap and the fold.

## 4. TABLESAMPLE on a column store table (2026-11-20)

**Branch:** `wip-colstore-31`, from `wip-colstore-30`.

**Question.** A sample scan (`TABLESAMPLE SYSTEM` or `BERNOULLI`)
reads rows by rising rowno, the `_id`. A column store table's scan
gives its row groups' rows, in `rg_id` order, then its delta
store's, so a row whose `_id` is below one already read is lost:

```sql
-- c: one row group, _id 1 .. 10000.
UPDATE c SET amount = 1 WHERE id = 1;  -- _id 1 to the delta store
SELECT count(*) FROM c TABLESAMPLE BERNOULLI (100);
    -- 9,999: the delta's _id 1 comes after _id 10000.
```

| Option | What changes | Cost |
| --- | --- | --- |
| A. Refuse it, SQLSTATE `0A000`, as SERIALIZABLE is | `smtable_scan_begin()` | no sample of a column store table |
| B. A sample scan in `_id` order | a scan of sm3 that merges the row groups' `_id` CGs and the delta store | a merge across every row group, for a query that is rare on an analytics table |

**Recommendation, built:** A. It is safe, and costs one test.
`ANALYZE` does not use it: sm samples a column store table itself
(`COLSTORE.md` §12.1). B can come when someone asks for it.

## 3. How digres names a row of a row group (2026-11-19)

**Branch:** `wip-colstore-10`, from `wip-colstore-9` at
`2f2e6f27`, the point to return to.

**Question.** A row of a row group is named by `(rg_id, _id)`.
digres's `RowKey` carries the `_id` alone, so an `UPDATE` or a
`DELETE` by it finds no row, and changes nothing, without an
error. Until this is decided, digres refuses `UPDATE`, `DELETE`
and row locks of a column store table. How does the row group
reach sm?

| Option | What changes | Cost |
| --- | --- | --- |
| A. The plan: a second system column, `_rg`, in the `RowKey` | slots (`tts_rg`), the planner's junk columns (`preptlist.c`, `appendinfo.c`), TID scans, `WHERE CURRENT OF`, the retry after `SM_ERR_UPDATED`; `_rg` becomes a reserved name in every table | the most code in shared PostgreSQL paths; no cost a row |
| B. sm finds it: a write by `_id` alone looks in the delta store, then in the row groups whose `_id` zonemap holds it | sm3's `tbl_update`/`tbl_delete`; dg drops its refusals | a lookup a row: the RG keys, and the `_id` CG of each RG that may hold it |
| C. The scan tells: when the scan that read the row is still on it, its row group is taken from the scan (`scan_at()` in `smstore.c`, `sm_row_scan_rg`); else B's lookup | `smstore_update`/`smstore_delete`, `tbl_scan_key`; B's lookup in sm3 | no cost for a scan feeding the write directly; a lookup for a plan that reorders rows (a join, a sort) |

**Recommendation, taken (the user said yes on C):** C. A plain `UPDATE ... WHERE` or `DELETE
... WHERE` is a scan under the write, which C serves at no cost.
It changes no PostgreSQL code outside `access/sm`, and reserves
no column name. B's lookup is its fallback, and is correct for
every plan.

It changes the plan's step 13: the row name is not carried in
the `RowKey`.

## 2. The seal's job: the builder queue, or a worker of its own (2026-11-19)

**Branch:** `wip-colstore-8`, from `wip-colstore-7` at
`a5ba2754`, the point to return to.

**Question.** The plan's decisions say the seal's job goes on the
builder queue, its jobs given a kind. Built, the job is a worker
of its own, the sealer (`sm/sm3/src/colstore/sealer.cc`), as the
btree reaper is. Keep it?

**Recommendation, taken:** keep the sealer. The builder's job is
an index's: its sets of active and building jobs and its waits
are keyed by index oid, and its worker loop checks the index's
state. A kind of job would change each of them, and every index
build with them. The sealer is 200 lines, one thread a catalog,
attached and detached beside the builder in `sm/src/sm.cc`, and
touches no index build.

**Alternative:** give the builder's jobs a kind, as decided: one
pool of threads for index builds, seals and, later, RG compaction.

## 1. Column store tables and SERIALIZABLE (2026-11-19)

**Branch:** `wip-colstore-q1`, from `wip-colstore-1` at
`bf7d7622`.

**Question.** The user said the Column Store may restrict
SERIALIZABLE if that gains much freedom. Restrict it how?

**Recommendation, taken:** a SERIALIZABLE transaction that reads
or writes a column store table fails with `SM_ERR_INVALID`,
which digres reports as a feature not supported
(`sm/COLSTORE.md` §9.1). What it frees:

- no SIREAD lock in a scan, so pruning and a later batch
  interface need no lock rule;
- no report to SSI of a delete that writes only a marker;
- no `sm1::ssi_write_range`, and no report of a bulk load's
  `_id`s: the plan's step "sm1, SSI for moves and loads" is gone.

**Alternative:** run a SERIALIZABLE transaction's reads and
writes of a column store table as REPEATABLE READ, with no
error. Scripts that set `default_transaction_isolation` to
serializable would then work, but SERIALIZABLE would no longer
mean serializable, silently.

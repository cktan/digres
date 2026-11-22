# COLSTORE2: places, versions and the validmap

This document is a design for a change to the Column Store
([`COLSTORE.md`](COLSTORE.md)). It has these parts:

- A row gets its place, `(rg_id, rno)`, at its insert. The seal
  does not change it.
- An RG has versions. RG compaction writes a new version with
  the live rows alone, and renumbers them. After the fourth
  version, the RG is eliminated: its live rows go back into the
  delta store.
- A delete marker and the validmap belong to one version of an
  RG. The validmap is a plain bitmap of the version's deleted
  rows.
- A writer locks a row as in a heap table: IX on the RG, then X
  on the row's place.
- A writer whose snapshot sees an older place of the row writes
  the newest place, and a shadow marker at the older one.
- An insert never waits for a seal. The sealer is a worker
  thread in the shape of the reapers.

The goal is a scan that gives each RG's deleted rows as one
bitmap, for vector operations, with no loss of concurrency.
Writers of different rows of one RG never wait for each other,
and a reader never blocks RG compaction.

An RG is like a large page of a heap. Each CG file is a slot
array of one column, and `rno` is the index into each slot
array. In a table of three columns, the row at `rno` 5 is
`(cg[0][5], cg[1][5], cg[2][5])`.

---

## 1. A worked example

Table `ev` has an open RG, `rg 7`, with no row yet.

```sql
-- T1 .. T4 run at REPEATABLE READ.
T1: BEGIN; INSERT INTO ev ...;          -- 10,000 rows
    -- _id 1 .. 10000 at rno 0 .. 9999 of rg 7. IX on rg 7.
    -- rno 9999 fills rg 7: the next insert opens rg 8, and
    -- the sealer gets a hint for rg 7.
T1: COMMIT;
-- The sealer seals rg 7: version 0, fsid(7, 0).
-- _id 900 stays rno 899.

T2: BEGIN; DELETE FROM ev WHERE _id <= 2000;
    -- IX on rg 7; X on (fsid(7, 0), rno) for each row;
    -- a marker for each row.
T2: COMMIT;
-- 2,000 dead rows of 10,000: 20%. The sealer gets a hint.

T3: BEGIN; SELECT 1;                     -- T3's snapshot

-- The sealer compacts rg 7 into version 1, fsid(7, 1):
    -- 1. X on rg 7, NOWAIT: granted.
    -- 2. The dropped set D: rno 0 .. 1999 of version 0.
    -- 3. Version 1 holds the 8,000 live rows: _id 2001 is at
    --    rno 0, and _id 2900 is at rno 899.
    -- 4. Put fsid(7, 1) with D; erase fsid(7, 0). Commit.

T4: BEGIN; DELETE FROM ev WHERE _id = 2900;
    -- T4's snapshot sees version 1: rno 899.
    -- A marker at (fsid(7, 1), rno 899).
T4: COMMIT;

T3: DELETE FROM ev WHERE _id = 3900;
    -- T3's snapshot sees version 0: rno 3899.
    -- The newest version is 1: rno 3899 - rank_D(3899) = 1899.
    -- X on (fsid(7, 1), rno 1899), and a marker there.
    -- A shadow marker at (fsid(7, 0), rno 3899), so that T3's
    -- own reads of version 0 do not see the row.
T3: COMMIT;
```

A snapshot older than the compaction reads version 0 and its
markers. A newer one reads version 1 and its markers. Both see
the same rows.

---

## 2. Terms

| Term | Meaning |
| --- | --- |
| place | `(rg_id, rno)` of a row in an open RG, or `(fsid, rno)` of a row in a version of a sealed RG |
| `rno` | a row's position in an open RG, drawn at its insert, or in a version of an RG |
| open RG | the RG that a table's inserts draw `rno`s from; its rows are in the delta store, and it has no RG key |
| version | one of the four file sets of an RG: 0 from the seal or a bulk load, 1 to 3 from RG compaction |
| `fsid` | the file set id of a version: `rg_id << 2 \| version` |
| hole | an `rno` of an open RG with no row: its insert aborted, or its row was deleted before the seal |
| dropped set (D) | the `rno`s of version 0 that a version does not hold, as a bitmap, in the version's RG key |
| validmap | a versioned key of a version; bit `rno` set means the row is deleted, or is a hole |
| delete marker | a versioned key `(fsid, rno)` that says the version's row `rno` is deleted |
| shadow marker | a delete marker at an older place of a row, which its writer's own snapshot reads |
| fold | the job that moves a version's markers into its validmap |
| elimination | the job that moves an RG's live rows into the delta store, and erases the RG |
| moved-in version | a version of a delta row that elimination writes; first-committer-wins sees through it |
| RG lock | a lock-table hold on an RG's lock key, in one of Gray's modes |
| hint | an entry in the sealer's queue in memory: a seal, a fold, an RG compaction, an elimination or a cleanup that may be due |

Terms of [`COLSTORE.md`](COLSTORE.md) §2 keep their meaning,
except for the moved marker, which this design removes.

---

## 3. Decisions

| Item | Decision |
| --- | --- |
| a row's place | drawn at its insert; the seal keeps it; RG compaction renumbers it; an update of a row of a sealed RG, and elimination, give a new place in the open RG |
| `_id` | kept: drawn at insert, kept for the row's life, as now; a writer finds an eliminated row by it |
| versions | at most four for each RG, 0 to 3, each a file set of its own `fsid`; a snapshot sees one of them |
| RG compaction | at 20% dead rows of the newest version; writes the next version with the live rows alone |
| elimination | at the 20% of version 3; the live rows go into the delta store as moved-in versions |
| a marker's key | `(fsid, rno)`, one key for each deleted row of a version |
| the validmap | one versioned key for each version; a plain bitmap; the seal writes it for holes, the fold for markers |
| the zonemap | stays in the RG key, one for each version |
| a writer's locks | IX on the table, IX on the RG, X on the row's newest place |
| a writer older than a version | writes the newest place, found by the dropped sets, and a shadow marker at the place its snapshot sees |
| a reader | takes no RG lock, and reads the version its snapshot sees |
| an insert | IX on the open RG; never waits for a seal |
| the seal of RG `N` | S on `N`, NOWAIT; takes every row of `N`, skips none |
| RG compaction, the fold, elimination | X on the RG, NOWAIT |
| the fold threshold | markers of 1% of a version's rows, 100 of 10k |
| the markers and validmap of an old version | range-deleted when no snapshot sees the version |
| the sealer | one worker thread for each catalog, which takes hints from a queue, in the shape of the reapers |
| a restart | closes every open RG; the sealer's first pass queues them |

**Two keys, not one.** The zonemap never changes in a version,
and the validmap changes at each fold. In one key, each fold
would rewrite the zonemap, about 50 bytes for each column. The
RG key is also sm1's file set key, which is put once and erased
once ([`COLSTORE.md`](COLSTORE.md) §4.3). sm1 decides a file's
life from that key's versions.

**Markers stay per row.** sm1 makes the uncommitted version of a
key its lock ([`MVCC_OVERVIEW.md`](MVCC_OVERVIEW.md) §5.1). If
writers wrote the validmap, a delete would lock the whole RG to
its commit. A second writer of another row would wait, and at
REPEATABLE READ it would then fail with `SM_ERR_CONFLICT`.
PostgreSQL fails neither.

**The validmap is a plain bitmap.** It is mostly zeros, and
RocksDB's block compression makes those nearly free.

**The fold threshold is 1% of a version's rows.** A scan reads
a version's validmap, one key, and then each of its markers, one
key each. The fold moves markers into the validmap, so the
threshold is the most markers a scan reads for a version: 100
of 10k. A fold writes a new validmap version, 1,250 bytes, that
stays until the watermark passes it. So a lower threshold makes
scans cheaper, and costs more folds and more validmap versions.
RG compaction at 20% starts the version again with no marker.

**The fold needs no watermark.** It holds X on the RG, so no
marker is in flight. Every marker it reads is committed. A
snapshot older than the fold still sees the old validmap and the
markers, as the fold's put and erases are versions.

**A version is a file set of its own.** `fsid = rg_id << 2 |
version` gives each version a new `fsid`, as
`sm1::fileset_put` requires. RG compaction puts the new
version's key and erases the old one's in one transaction. sm1's
GC of file sets then deletes the old files once no snapshot sees
them, as now. sm1 does not change.

**A version renumbers its rows, so a marker names its
version.** Row `rno` 899 of version 1 is not row `rno` 899 of
version 0. A marker at `(fsid, rno)` is read only by a scan of
that version. So a snapshot of version 0 never applies a marker
of version 1 to its rows.

**MVCC, not a wait, keeps an old snapshot's version.** New
snapshots see the new version at once. An older snapshot keeps
the version it sees. RG compaction does not wait for old
snapshots, and a reader takes no lock or pin. Analytic queries
are long, and they would hold back a compaction that waited for
them.

**The place is drawn at the insert, so the seal moves no lock.**
A row's lock is on its place before the seal and after it. A
`FOR SHARE` holder keeps its lock across the seal, and the seal
does not wait for it. A writer that meets the seal's tombstone
knows the row's place from the version it read, and needs no
lookup.

**`_id` stays.** An update of a row of a sealed RG puts the new
version in the open RG, under a new place, as a non-HOT update
of a heap puts a new tuple on another page. Elimination moves
rows too. A writer that comes to the old place finds the row by
its `_id`, as now.

---

## 4. Keys in the table's store

| Class or relid | What | Key in RocksDB | Kind |
| --- | --- | --- | --- |
| relid `0x0001` | delta rows | `0x01 ‖ 0x0001 ‖ be64(_id)`; the value begins with `be64(rg_id) ‖ be32(rno)` | chained |
| relid `0x0002` | delete markers | `0x00 ‖ 0x0002 ‖ be64(fsid) ‖ be32(rno) ‖ ~wts ‖ ~seq` | versioned |
| relid `0x0003` | validmaps | `0x00 ‖ 0x0003 ‖ be64(fsid) ‖ ~wts ‖ ~seq` | versioned |
| relid `0x0004` | filter keys | `0x00 ‖ 0x0004 ‖ be64(fsid) ‖ be16(attnum + 1) ‖ ~wts ‖ ~seq` | versioned |
| relid `0x0005` | RG lock names | `0x00 ‖ 0x0005 ‖ be64(rg_id)`; never written | a lock-table name only |
| relid `0xFFFF` | TOAST of delta rows | unchanged | versioned |
| class `0x05` | RG keys: sm1's file set keys | `0x05 ‖ be64(fsid) ‖ ~wts ‖ ~seq` | versioned |

- **A delta row carries its place.** Each version of a delta
  row begins with its `(rg_id, rno)`. An update of a delta row
  keeps them. An update of a row of a sealed RG, and an
  elimination, write the new version with a new place in the
  open RG.
- **The value of an RG key** is that of
  [`COLSTORE.md`](COLSTORE.md) §4.4, and, in versions 1 to 3,
  the dropped set D: a bitmap of version 0's row count.
- **The value of a marker is empty**, as now.
- **The value of a validmap is a plain bitmap** of the
  version's row count. A version with no validmap key has no
  bit set.
- **The RG lock's key** is a lock-table name only:
  `sm1::lock_row` on `0x00 ‖ 0x0005 ‖ be64(rg_id)`. One name
  covers every version of the RG. No key of data has this name:
  `sm1::lock_row` reads the newest version of its key, and
  waits for a writer of it in flight
  ([`LOCKMGR.md`](LOCKMGR.md) §1.5). On a validmap's key, an IS
  would wait for the seal's write of the validmap.
- **A row lock's key is the marker's key of the row's newest
  place, without the version suffix.** For a row of an open RG,
  it is `0x00 ‖ 0x0002 ‖ be64(rg_id << 2) ‖ be32(rno)`.
- **A file's path** is that of [`COLSTORE.md`](COLSTORE.md)
  §4.1, by `fsid`.
- **`rno` is `be32`**, so the RG size of
  [`COLSTORE.md`](COLSTORE.md) §7 can change without a new key
  format.
- **The newest version of an RG** is the one key under
  `0x05 ‖ be64(rg_id << 2)` to `0x05 ‖ be64(rg_id << 2 | 3)`
  that is live at the newest state. No key means that the RG is
  eliminated, or not sealed.

---

## 5. Locks

The lock modes are sm1's ([`LOCKMGR.md`](LOCKMGR.md) §1.4).

| Action | Table | RG | Row |
| --- | --- | --- | --- |
| `INSERT` | IX | IX on the open RG | none: the place is new |
| `DELETE`, `UPDATE` of a row | IX | IX on the row's RG | X on the row's newest place |
| an update's new version, for a row of a sealed RG | IX | IX on the open RG | none: the place is new |
| `FOR UPDATE` | IX | IX | X |
| `FOR SHARE` | IS | IS | S |
| plain `SELECT` | IS | none | none |
| the seal of `N` | IX | S on `N`, NOWAIT | none |
| RG compaction, the fold | IX | X, NOWAIT | none |
| elimination | IX | X, NOWAIT, and IX on the open RG | none |

**A mover holds its source's lock.** A lock on a place misses a
write after the row moves. So each move locks the place the row
leaves:

- RG compaction and elimination hold X on the RG. That excludes
  each writer (IX) and each `FOR SHARE` holder (IS) of its rows.
  No row lock of the RG is held when they commit.
- The seal does not move a row: the place stays. It holds S on
  `N`, which excludes each writer of a row of `N` (IX), and lets
  each `FOR SHARE` holder (IS) keep its lock.
- A new version, and a moved-in version, are not visible until
  their mover commits. No transaction can lock them before then.

**A writer checks the newest version after IX.** A writer can
wait for IX while RG compaction or elimination holds X, and get
IX after it commits. A writer can also hold a snapshot that is
older than a version. So after IX, a writer of a row of a sealed
RG reads the newest version of the RG (§4), and writes the row's
place in it (§6.3).

**An open RG and an eliminated RG both have no RG key.** A
writer does not confuse them. It reads delta key `_id` first. A
live version there is a row of an open RG, or a moved-in row. A
sealed tombstone sends the writer to the sealed RG of the place
in the version it read.

**A writer checks the validmap's bit after X.** The fold erases
a marker after it sets the bit. A writer that came to the row
by its `_id`, after a join or a sort, could insert a marker over
that erase and act on a row that is gone. The writer holds IX,
so no fold runs between its check and its insert.

**NOWAIT keeps the sealer out of deadlocks.** A writer takes
only IX at the RG, and IX is compatible with IX. The seal, RG
compaction, the fold and elimination never wait. A prepared
transaction keeps its IX in its `PREPARED` record, so it keeps
them from its RG across a restart.

---

## 6. The procedures

### 6.1 An insert

1. Take IX on the table, as now.
2. Draw the row's `_id` from the table's sequence, as now.
3. Draw the next `rno` of the table's open RG. If the open RG
   has all its `rno`s, draw a new `rg_id`, make it the open RG,
   and give the sealer a hint to seal the old one.
4. Take IX on the open RG. A transaction takes it once for each
   RG.
5. Insert the row at delta key `_id`, with `(rg_id, rno)` at
   the head of its value.

- **The open RG and its counter are in memory**, one for each
  table, in the table's entry of the sealer. The draw is one
  step under a mutex, as the draw of `_id` from its sequence.
- **An insert never waits for a seal.** The seal of `N` starts
  after `N` has all its `rno`s, and inserts go to `N+1`.
- **An abort, or a rollback to a savepoint, leaves a hole.**
  The `rno` is not given again.
- **A bulk load** writes version 0 of RGs of new `rg_id`s
  directly, as now ([`COLSTORE.md`](COLSTORE.md) §9.2). The
  rows that are left go into the delta store by these steps.

### 6.2 A scan of an RG

1. Read the RG keys at the snapshot, and prune by each one's
   zonemap, as now. A snapshot sees one version of an RG.
2. Read the version's validmap at the snapshot.
3. Read the version's markers at the snapshot, and set bit
   `rno` for each. A shadow marker the transaction wrote is
   among them.
4. Give the version's rows with the bitmap as the selection
   vector.

- **`count(*)` of a version** is its row count less the set
  bits.
- **A lookup by `_id`** walks the version's `_id` CG to `n`
  ([`COLSTORE.md`](COLSTORE.md) §8), then tests the bit. The
  position where it stops is `rno`.

### 6.3 A write of a row

`tbl_delete_at` and `tbl_update_at` (`sm3/src/table/table.cc`),
for a row at the place its snapshot sees:

1. Take IX on the table, as now.
2. Take IX on the RG (`sm1::lock_row`, `SM_LOCKMODE_IX`).
3. A row of an open RG, or a moved-in row:
   1. Take X on `(rg_id, rno)`.
   2. Write delta key `_id`, a tombstone or the new version, as
      now. The new version keeps its place.
   3. A sealed tombstone there (`SM_ERR_SEALED`) means that the
      RG is sealed: go to step 4 with version 0 of the same
      place.
   4. If the snapshot's place is in a sealed RG, as for a writer
      that followed an eliminated row, insert a shadow marker
      there.
   5. Stop: steps 4 to 9 are for a row of a sealed RG.
4. A row of a sealed RG: read the RG's newest version (§4).
   1. If the RG has no version, it is eliminated: find the row
      by its `_id` at the newest state, a moved-in delta row,
      and go to step 2 with its place.
   2. If the newest version is the one the snapshot sees, the
      newest place is the snapshot's place.
   3. Else map the snapshot's `rno` to the newest version
      (§6.4). If the row is in the newest version's dropped set,
      it was deleted: read its marker in the snapshot's version
      at the newest state, which gives the errors of step 7.
5. Take X on the newest place. It waits for a `FOR SHARE` holder
   or a writer of the row.
6. Read the newest version's validmap at the newest state. If
   bit `rno` is set, fail with `SM_ERR_NOTFOUND`.
7. Insert the marker at the newest place. A marker there already
   gives the errors of [`COLSTORE.md`](COLSTORE.md) §9:
   `SM_ERR_SELFMODIFIED`, `SM_ERR_NOTFOUND` or `SM_ERR_UPDATED`.
8. If the snapshot's place is not the newest place, insert a
   shadow marker at the snapshot's place.
9. For an update, put the new version at delta key `_id`, with
   a new place drawn as an insert draws it (§6.1 steps 3 and 4).

- **First-committer-wins judges at the newest place.** Two
  writers of one row, of any snapshots, meet at one marker key.
- **A shadow marker is for the writer's own reads.** It commits
  after every snapshot that sees its version, except the
  writer's. So no other transaction sees it. It needs no lock:
  X on the newest place covers it.
- **A writer that follows an eliminated row** writes a shadow
  marker at the place its snapshot sees, too (step 3.4). Its own
  reads of the old version then do not see the row that it
  deleted.

A writer knows the place its snapshot sees in one of three
ways:

- **From the scan** (§6.11). A plain `UPDATE ... WHERE` or
  `DELETE ... WHERE` names the row so.
- **From the delta row**, whose value begins with its place.
- **From a lookup by `_id`** at the snapshot (§6.2).

### 6.4 The map of an `rno` to a newer version

Each version `k` of 1 to 3 keeps `D_k`, the `rno`s of version 0
that it does not hold. A version holds the rows of version 0
that are not in its D, in their order. So, for an `rno` of
version `i` and a newer version `k`:

1. Map `rno_i` to version 0: the `rno_i`-th `rno` of version 0
   not in `D_i`. For version 0 itself, it is `rno_i`.
2. If that `rno_0` is in `D_k`, the row is not in version `k`.
3. Else `rno_k` is `rno_0` less the count of `D_k`'s bits below
   `rno_0`.

The writer reads `D_i` from the RG key its snapshot sees, and
`D_k` from the newest. Each is 1,250 bytes for 10k rows. The map
costs a popcount over each.

### 6.5 The seal of RG `N`

The seal is a pass of the sealer (§6.10), at READ COMMITTED.

1. Take S on `N`, NOWAIT. On a conflict, keep the hint for the
   next pass.
2. Scan the delta store at the newest state. Take each row whose
   place is in `N`.
3. Erase each row taken with a sealed tombstone, as now
   ([`COLSTORE.md`](COLSTORE.md) §10.2).
4. Build version 0 of `N`: row `rno` at position `rno`. The row
   count is one more than the last `rno` drawn in `N`. A value
   out of line in TOAST is read back whole.
5. Write each CG's file, and fsync the files and the directory.
6. Put the RG key of `fsid(N, 0)`, a filter key for each CG, and
   a validmap with a bit for each hole. With no hole, put no
   validmap.
7. Erase the TOAST chunks of the rows taken, as now.
8. Commit. The lock is held to the commit.

- **The seal takes every row of `N`.** S on `N` excludes every
  writer of a row of `N`, so no row of `N` is in flight. It
  takes no row lock.
- **A hole is in each CG**, as a null in a column's CG, and as
  the `_id` before it, or 0 at `rno` 0, in the `_id` CG. The
  zonemap and the filter leave holes out: `cg_build` takes the
  holes' bitmap.
- **A `FOR SHARE` holder of a row of `N` does not stop the
  seal**, and keeps its lock: the lock's name does not change.
- **A writer of a row of `N` waits for the seal's commit**, as
  now ([`COLSTORE.md`](COLSTORE.md) §10.1). It lasts the write
  and fsync of one RG's files.
- **RGs can seal out of order.** A long or prepared transaction
  keeps `N` from its seal, and `N+1` can seal first. No rule
  needs the order of `rg_id`s.

### 6.6 The fold

The fold is a pass of the sealer (§6.10), at READ COMMITTED,
on the newest version of an RG.

1. Take X on the RG, NOWAIT. On a conflict, keep the hint for
   the next pass.
2. Read the newest version at the newest state. If the RG has
   none, or it is not the hint's version, stop.
3. Read the version's validmap and markers at the newest state.
4. Put a new validmap: the old bits, and a bit for each marker.
5. Erase each marker that step 3 read.
6. Commit. The lock is held to the commit.

### 6.7 RG compaction

`col_compact` (`sm3/src/colstore/colstore.cc`), a pass of the
sealer (§6.10), at READ COMMITTED, on version `v` of 0 to 2:

1. Take X on the RG, NOWAIT. On a conflict, keep the hint for
   the next pass.
2. Read the newest version at the newest state. If it is not
   `v`, stop.
3. Read version `v`'s validmap and markers at the newest state:
   its dead rows. `D_{v+1}` is `D_v` with the dead rows mapped
   to version 0.
4. Write every CG of version `v+1`, with the live rows alone, in
   their order. Fsync the files and the directory.
5. Put the RG key of `fsid(rg, v+1)`, with its zonemap and
   `D_{v+1}`, and its filter keys. The new version has no
   validmap.
6. Erase the RG key of `fsid(rg, v)` and its filter keys.
7. Give the sealer a hint to clean up `fsid(rg, v)` (§6.9).
8. Commit. The lock is held to the commit.

- **Every CG is written again.** The new version holds no dead
  row. Its `_id` CG stays in order if the old one was, so it
  keeps delta encoding.
- **A snapshot older than step 8 reads version `v`**, and its
  markers, which step 6 does not erase. A newer one reads
  version `v+1`.
- **No row lock is held**, as X on the RG excludes every holder.
  A writer that waited for IX through the compaction maps its
  `rno` by §6.4.

### 6.8 Elimination

A pass of the sealer (§6.10), at READ COMMITTED, on version 3,
at its 20% dead rows:

1. Take X on the RG, NOWAIT. On a conflict, keep the hint for
   the next pass.
2. Read the newest version at the newest state. If it is not
   version 3, stop.
3. Read its validmap and markers at the newest state.
4. For each live row, draw a new place as an insert draws it
   (§6.1 steps 3 and 4), and put the row at delta key `_id` as
   a moved-in version (`sm1::chain_insert_moved_in`, new). A
   value over 2,000 bytes goes to TOAST, as an insert's does.
5. Erase the RG key of `fsid(rg, 3)` and its filter keys.
6. Give the sealer a hint to clean up `fsid(rg, 3)` (§6.9).
7. Commit. The lock is held to the commit.

**A moved-in version** is a new kind of version of a chained
key, in sm1, as the sealed tombstone is
([`COLSTORE.md`](COLSTORE.md) §10.2). Elimination changes no
row's content, so it must not fail a user's transaction:

- **A reader** sees it as any version.
- **First-committer-wins sees through it**, and through a
  sealed tombstone under it. It judges a write by the first
  version under them that is neither. With no such version, as
  for a row of a bulk load, a write does not conflict.
- **A write that passes** writes over it as over any version:
  it does not fail with `SM_ERR_SEALED`.
- **The Garbage Collector** treats it as any version.

- **The rows are written twice**, into the delta store now, and
  into a new RG at the next seal. About 4,100 rows of 10,000
  are left at version 3's 20%: 10,000, then 8,000, 6,400,
  5,120, and 4,096.
- **A snapshot older than step 7 reads version 3.** A newer one
  reads the moved-in delta rows. Each sees each row once.

### 6.9 The cleanup of an old version

A version's markers and validmap outlive its RG key: a snapshot
that sees the version reads them, and a writer of that snapshot
can add a shadow marker. The cleanup is a pass of the sealer
(§6.10):

1. Ask sm1 whether the RG key of `fsid` is settled gone
   (`sm1::fileset_settled`, new): its newest version is a
   committed erase at or below the GC watermark. If not, keep the
   hint for the next pass.
2. In a transaction, erase each key of `fsid` under relids
   `0x0002` and `0x0003`: its markers, shadow markers included,
   and its validmap. Commit.

- **No snapshot sees the version**, so no reader reads the keys
  and no writer adds one.
- **The erases are versions**, as any erase. The compaction
  filter drops them and the keys under them later. sm1 has no
  range delete outside MVCC, and needs none: a version has at
  most a few thousand such keys.
- **`sm1::fileset_settled` is new.** `sm1::read_settled` judges
  a key of a store's own classes: in a chained store, a chained
  key. A file set key is of class `0x05`. The new call does for
  it what `read_settled` does for a versioned key
  (`versioned_settled` in `sm1/src/access.cc`).
- **A restart loses the hints.** The sealer's first pass lists
  the `fsid`s of the marker and validmap keys that have no RG
  key, and gives a hint for each.

### 6.10 The sealer

The sealer is one worker thread for each catalog
(`sm3/src/colstore/sealer.cc`), in the shape of the reapers'
threads ([`GC.md`](GC.md) §13.5). It has its own thread, not
one of the reapers' pool: a seal writes and fsyncs an RG's
files, which would delay the passes of the btree and row
reapers.

1. A writer gives a hint to the sealer's queue in memory:
   - an insert that fills the open RG: seal that RG;
   - a commit or a scan that counts markers on a version of 1%
     of its rows, 100 of 10k: fold it;
   - a commit or a scan that counts 20% of a version's rows
     dead: compact it, or eliminate it at version 3;
   - RG compaction and elimination: clean up the old version.
2. Each second, the thread looks at the queue, with no session.
   An empty queue ends the pass.
3. If hints wait, the thread opens a session, and runs each
   hint's job (§6.5 to §6.9). Each job judges again: a job
   whose RG or version is gone, or has nothing to do, drops its
   hint.
4. A job that gives up on a lock, or must wait for the
   watermark, keeps its hint for the next pass.
5. The thread closes the session at the end of the pass, so the
   db reaper ([`DBMGR.md`](DBMGR.md) §5.2) can still close an
   idle database.

These go from the sealer, as a full open RG is a fact that the
counter of §6.1 knows:

- the count of delta rows that each transaction hands over at
  its commit;
- the wait of a prepared transaction's counts for its `COMMIT
  PREPARED`: S NOWAIT fails while its IX is held;
- the seal that a scan of more than 10k delta rows queues;
- the merge of small RGs of [`COLSTORE.md`](COLSTORE.md) §11.1:
  elimination returns a shrunk RG's rows to the delta store.

**A restart closes every open RG.** The counters are lost. The
first insert into a table after the open draws a new `rg_id`.
The sealer's first pass scans each column store table's delta
store, and gives a hint for each `rg_id` it finds there: each
is an RG that is not sealed.

### 6.11 The interface

A place goes from sm's scan to digres, and back to sm's write.

| Call | Now | With this design |
| --- | --- | --- |
| `sm.h` | `uint64_t sm_row_scan_rg(const sm_row_scan_t*)` | `sm_row_place_t sm_row_scan_place(const sm_row_scan_t*)` |
| `sm.h` | `int sm_tbl_delete_at(sm_tbl_t*, uint64_t rg_id, uint64_t id)` | `int sm_tbl_delete_at(sm_tbl_t*, sm_row_place_t place, uint64_t id)` |
| `sm.h` | `int sm_tbl_update_at(sm_tbl_t*, uint64_t rg_id, uint64_t id, values, isnull, n)` | `int sm_tbl_update_at(sm_tbl_t*, sm_row_place_t place, uint64_t id, values, isnull, n)` |
| sm3 | `tbl_delete_at`, `tbl_update_at`, by `rg_id` | the same calls, by `sm_row_place_t` |

```cpp
/* A row's place in a version of a sealed RG (../COLSTORE2.md §2). */
struct sm_row_place_t {
    uint64_t fsid; /* rg_id << 2 | version; 0: a row of the delta store */
    uint32_t rno;
};
```

- **`fsid` 0 is a row of the delta store**, as `rg_id` 0 is now.
  The writer reads delta key `_id`, whose value holds the place.
- **The place is the one the scan's snapshot sees.** The write
  maps it to the newest version (§6.3).
- **digres changes at three places** in
  `dg/src/backend/access/sm/smstore.c`: `scan_rg()` returns an
  `sm_row_place_t`, and `smstore_update()` and the delete pass
  it to `sm_tbl_update_at()` and `sm_tbl_delete_at()`. This is
  code that digres added.

---

## 7. The cases

| Case | Result |
| --- | --- |
| two inserts at once | two `rno`s of the open RG; IX and IX; no wait |
| an insert while the old open RG seals | it goes to the new open RG; no wait |
| two writers, different rows, one RG | IX and IX; two markers or two delta writes; no wait |
| two writers, one row | the second waits on X; then a marker or a version there: RC `SM_ERR_UPDATED` or `SM_ERR_NOTFOUND`, RR `SM_ERR_CONFLICT` |
| two writers, one row, of snapshots on each side of a compaction | both write the newest place; first-committer-wins judges there |
| update of a row of an open RG | the new version at delta key `_id`, with the same place |
| update of a row of a sealed RG | a marker, and the new version at delta key `_id` with a new place in the open RG |
| update A to B to A | the first update moves the row to the open RG; later ones keep that place until its seal |
| delete, then reinsert | the insert draws a new `_id` and a new place |
| an insert that aborts | its `rno` is a hole; the seal sets its bit |
| `FOR SHARE` on a row of `N` during the seal of `N` | IS and S; the seal goes on; the lock stays |
| a writer of a row of `N` during the seal of `N` | waits on IX for the seal's commit, then follows the sealed tombstone to version 0 of `N`, the same place |
| a long or prepared writer in `N` | `N` waits to seal; inserts go on in `N+1` |
| old RR reader across a compaction | reads the version its snapshot sees; takes no lock; the compaction does not wait for it |
| RR writer older than a compaction | maps its `rno` (§6.4); a marker at the newest place, and a shadow marker at its own; its reads hide the row |
| RR writer older than two compactions | the same: the map goes through version 0 |
| RR writer of a row that a newer version dropped | the row's marker in its own version gives `SM_ERR_CONFLICT` |
| RR writer older than an elimination | follows `_id` to the moved-in row; first-committer-wins sees through it; a shadow marker at its own place |
| RR writer older than the seal | the sealed tombstone is transparent to first-committer-wins, as now; the writer goes to version 0 of the same place |
| RC writer that waited on a compaction | the same as an RR writer older than it, at the newest state |
| a writer by `_id` after a fold | step 6 of §6.3 finds the bit, and fails with `SM_ERR_NOTFOUND` |
| savepoint rollback | the undo list removes the marker, the shadow marker or the delta write; an `rno` drawn is a hole; locks stay, as sm1 does |
| abort | as any key; each `rno` drawn is a hole |
| crash | a version in flight is dead; no lock outlives the crash; every open RG is closed; the hints are found again (§6.9, §6.10) |
| 2PC across restart | the `PREPARED` record keeps IX and X; the seal, compaction, the fold and elimination give up on the RG |
| open cursor | a scan's snapshot; no lock; `WHERE CURRENT OF` writes as a writer of that snapshot |
| SSI | refused for a column store table, as now |
| GC watermark | an old version's files, markers and validmap stay until no snapshot sees it |
| `TRUNCATE`, `DROP TABLE`, ALTER rewrite | the old store goes, with its keys; the sealer drops the hints of a store that is gone |
| bulk load, and its rollback | version 0 of new RGs, no hole, no validmap; the rest as inserts |
| CREATE INDEX, DROP INDEX, TOAST in an RG, temp tables | not in a column store table |
| a hash collision of two lock keys | a false conflict only ([`LOCKMGR.md`](LOCKMGR.md) §4.9) |
| logical decoding (deferred) | a marker is a delete; a shadow marker, the seal, the fold, RG compaction and elimination emit nothing |

---

## 8. Changes to COLSTORE.md

| Section | Change |
| --- | --- |
| §2 Terms | place, open RG, version, `fsid`, hole, dropped set, validmap, shadow marker, fold, elimination, moved-in version, hint; the moved marker goes |
| §4.1 Files | a file's path is by `fsid` |
| §4.3 Keys | the keys of §4 |
| §4.4 The value of an RG key | the dropped set |
| §5.1 Encodings | an RG of the seal can hold `_id`s that are not in order, from updates of sealed rows; RG compaction keeps the order |
| §8 Row identity | the place is drawn at insert; versions renumber it; the map of §6.4 |
| §9 Writes | the steps of §6.1 and §6.3 |
| §9.1 Locks | the table of §5 |
| §10 The seal | the steps of §6.5 |
| §10.4 When the seal runs | the sealer of §6.10 |
| §11 RG compaction | the steps of §6.6 to §6.9 |
| §12 Scans | the steps of §6.2 |
| §15 Tests | the tests of §10 |

---

## 9. The plan

1. **Add the place to the scan's row and to the writes**: the
   interface of §6.11, digres's three call sites, and the lookup
   by `_id` → verify: sm3 unit tests, rel, dg.
2. **Name file sets, markers, validmaps and filter keys by
   `fsid`**, with version 0 alone, and key markers by `rno` →
   verify: the full suites, unchanged results.
3. **Draw the place at insert**: the open RG, its counter, the
   place at the head of a delta row's value, and the seal that
   keeps it (§6.1, §6.5) → verify: sm3 unit tests, rel.
4. **Lock the RG and the row** (§5, §6.3), with the check of the
   newest version → verify: the new sm3 unit tests and `.rel`
   scenarios of §10.
5. **The validmap and the fold** (§6.2, §6.6) → verify: sm3
   unit tests, rel, dg.
6. **RG compaction by versions**, with the dropped sets, the map
   of §6.4, shadow markers, `sm1::fileset_settled` and the
   cleanup (§6.7, §6.9) → verify: sm1's unit test of
   `fileset_settled`, the compaction tests of
   `sm3/test/test_colstore.cc`, the new ones of §10.
7. **The moved-in version in sm1** → verify: new mvcc
   scenarios (§10).
8. **Elimination** (§6.8), in place of the merge of small RGs →
   verify: sm3 unit tests, rel.
9. **The sealer by hints** (§6.10), and the restart pass →
   verify: sm3 unit tests, dg `server.sh` across a restart.
10. **Update COLSTORE.md** (§8), and remove this document.

---

## 10. Tests

- **mvcc** (`sm1/mvcc`), new scenarios for the moved-in version.
  No existing scenario changes.
  - a reader sees a moved-in version as any version;
  - an RR writer older than a moved-in version writes over it,
    and commits;
  - an RR writer older than a moved-in version over a version
    committed after its snapshot fails with `SM_ERR_CONFLICT`;
  - an RC writer that waited on the mover writes over it.
- **sm1 unit tests:** `fileset_settled` is 2 for a file set key
  that a snapshot still sees, and 1 once its erase is below the
  watermark.
- **sm3 unit tests** (`sm3/test/test_colstore.cc`):
  - an insert while the old open RG seals does not wait, and
    goes to the new open RG;
  - an insert that aborts leaves a hole, which the seal sets in
    the validmap, and a scan skips;
  - a `FOR SHARE` holder of a row does not stop the seal of its
    RG, and keeps its lock;
  - a long writer in `N` keeps `N` from its seal, and `N+1`
    seals first;
  - two writers delete different rows of one RG at REPEATABLE
    READ, and both commit;
  - two writers delete one row: the second waits, then fails;
  - RG compaction gives up on an IX holder and on an IS
    (`FOR SHARE`) holder, and not on a reader;
  - RG compaction writes a version with the live rows alone; a
    snapshot older than it counts the same rows as one newer;
  - an RR writer older than one compaction, and one older than
    two, deletes the right row, and does not see it afterward;
  - two writers of one row, of snapshots on each side of a
    compaction: the second fails;
  - an RR writer older than an elimination deletes the moved-in
    row, and does not see it afterward;
  - a writer by `_id` after a fold fails with `SM_ERR_NOTFOUND`;
  - the cleanup removes an old version's markers and validmap
    after the watermark passes, and not before;
  - a prepared delete keeps the seal, RG compaction, the fold
    and elimination from its RG across a restart;
  - a restart closes the open RG, and the sealer's first pass
    seals it.
- **rel** (`rel/test/test_colstore.rel`): two writers of
  different rows of one RG, two writers of one row, an insert
  during a seal, and an RR writer older than a compaction, in
  two sessions.
- **dg** (`dg/test`): two sessions delete different rows of one
  RG at REPEATABLE READ, and both commit; `server.sh`: rows
  inserted before a restart are sealed after it.
- **Cannot test:** the scan's use of the bitmap as a selection
  vector waits for the columnar executor
  ([`COLSTORE.md`](COLSTORE.md) §12).

---

## 11. Open questions

1. **A writer's own delete after the seal.** An RR writer whose
   snapshot is older than the seal writes its marker into
   version 0, which its snapshot cannot see. Its own scans read
   the delta row, where the row looks live. A shadow write over
   the sealed tombstone would hide it, but it changes the rule
   of the sealed tombstone, and an mvcc scenario
   (`s06`) with it. This is a question of
   [`COLSTORE.md`](COLSTORE.md) as it stands too. A test must
   show it before a fix is designed.
2. **The seal scans the whole delta store.** The delta store is
   keyed by `_id`, not by place, so the seal of `N` reads the
   rows of every open RG to find those of `N`. Long transactions
   that hold several open RGs make each seal read more.
3. **The `_id`s of a sealed RG are not always in order.** An
   updated or eliminated row keeps its low `_id` in a new place.
   Its RG's `_id` CG then gets plain encoding, and a wider `_id`
   range, which prunes a lookup by `_id` less.
4. **The small RGs of a restart.** A restart closes open RGs
   that are not full, and the seal makes small RGs of them.
   Elimination takes only a version 3. A rule to eliminate a
   small RG, or to place its rows again before the seal, is not
   decided.

# MVCC_OVERVIEW: how sm does MVCC

sm keeps many versions of each row, and gives each transaction a
snapshot that decides which versions it sees. Level 1, sm1, does
all of this. RocksDB is only an ordered, durable, crash-atomic
key-value store under it: sm1 opens a plain `rocksdb::DB`, not a
`TransactionDB`, and writes each change as a `WriteBatch`.

This document is a map of the design. The design of record is in
the module documents that §12 lists. Each section here names the
one it summarizes.

---

## 1. A worked example

Table `acct` has a primary key `id`. Its row 42 holds `bal = 7`,
written by xid 80 and committed at 90. Now `visible_ts`, the
newest timestamp a snapshot may see, is 100.

```sql
-- Session R
BEGIN ISOLATION LEVEL REPEATABLE READ;
SELECT bal FROM acct WHERE id = 42;
    -- snapshot_ts = 100. One Get of CURR finds the version of
    -- xid 80, committed at 90 <= 100: bal = 7.

-- Session W
BEGIN;
UPDATE acct SET bal = 9 WHERE id = 42;
    -- The first write: xid = 101, then wts = 102.
    -- One WriteBatch, not synced:
    --   CURR 42  = (xid 101, wts 102, prev = OLD 7, bal 9)
    --   OLD 7    = (xid 80, displaced by 101, bal 7)
COMMIT;
    -- commit_ts = 103. Write COMMITTED at CF:txn[101], and
    -- sync the WAL: this is the commit point.
    -- visible_ts rises to 103.

-- Session R
SELECT bal FROM acct WHERE id = 42;
    -- Get CURR: wts 102 > snapshot 100, so not visible.
    -- Follow prev, Get OLD 7: bal = 7, the same as before.
UPDATE acct SET bal = bal + 1 WHERE id = 42;
    -- CURR committed after R's snapshot: first-committer-wins.
    -- R fails with SM_ERR_CONFLICT, and is aborted.
```

A new session now takes snapshot 103. Its `Get` of CURR finds xid
101, the commit cache answers "committed at 103", and it sees
`bal = 9`.

Later, R has ended and no snapshot is older than 103. A RocksDB
compaction then drops OLD 7, as every snapshot sees its
displacer first. It also writes 103 into CURR's header in place
of xid 101, so no reader looks up xid 101 again.

---

## 2. Terms

| Term | Meaning |
| --- | --- |
| `global_ts` | the one 64-bit counter that gives every xid, `wts` and `commit_ts` |
| xid | a transaction's id, taken at its first write; a reader has none |
| `wts` | the write timestamp, taken at each write; it orders a key's versions |
| `commit_ts` | the timestamp a transaction takes when it commits |
| `visible_ts` | the newest timestamp below which every commit is published |
| snapshot | one number, `snapshot_ts`, read from `visible_ts` |
| version | one value of a key, with its writer's xid in its header |
| tombstone | a version that records a delete |
| CURR, OLD | where a table row's newest version, and its older ones, are kept |
| status record | the entry in `CF:txn`, the transaction table, that says a transaction committed |
| stamp | a `commit_ts` that compaction writes into a version's header, in place of the xid |
| watermark | the oldest live snapshot: every version committed at or below it is seen by all |

---

## 3. Versions on disk

ROWSTORE.md §3 has the design of record.

A store is one RocksDB column family. It holds two kinds of key:

- **A chained key** holds a table's rows. The newest version is
  in CURR, under the row's plain key. The older versions are in
  OLD, each under an id of its own, linked by `prev`. A read by
  key is one `Get`, and a scan reads plain keys.
- **A versioned key** holds LSM index entries, TOAST values and
  the catalog. Each version is a key of its own: `userkey ‖ ~wts
  ‖ ~seq`. The complement makes the newest version sort first.

A B-tree index entry has no version. Its row decides visibility:
a scan reads the row at its snapshot, and checks that the row
still has the entry's key (BTREEINDEX.md).

Every version's header holds its writer's xid. No version holds
its writer's fate: a reader finds that by the xid (§4.3).

**A key's versions are in commit order.** Two writers of one key
are serialized: the second waits for the first to end (§5.1),
and only then takes its `wts`. So a later commit has a higher
`wts`. Ordering by xid would fail: a READ COMMITTED transaction
takes its xid early, and can overwrite a commit newer than it.

---

## 4. Snapshots and visibility

XCTMGR.md §3 and §9 have the design of record.

### 4.1 A snapshot is one number

A snapshot is `snapshot_ts`, read from `visible_ts`. It has no
list of transactions in flight, so it costs nothing to take, and
a visibility test is integer comparisons. `sm1::begin` registers
its snapshot, for the watermark (§10).

### 4.2 The visibility test

```
if V.xid == view.xid:              # own write
    visible iff V.seq <= view.seq_hwm
elif V.wts > view.snapshot_ts:     # written after the snapshot,
    invisible                      # so committed after it too
else:
    commit_ts = V's stamp, or resolve V.xid (§4.3)
    visible iff committed and commit_ts <= view.snapshot_ts
```

A visible tombstone means the key does not exist. A reader of a
chained key that does not see CURR follows `prev` into OLD, and
tests each version the same way.

### 4.3 Resolving an xid

A reader finds a writer's fate in this order:

1. The version's header: a stamp means committed at that
   `commit_ts`.
2. The active table, in memory: the writer is in flight, running
   or prepared.
3. The commit cache, in memory: committed at this `commit_ts`.
4. `CF:txn`: the status record says committed, aborted or
   prepared.
5. None of these: the writer never committed, so the version is
   dead.

The last step is why recovery has no undo pass. One process has
the database open, so the active table lists every live writer.

### 4.4 No snapshot sees a gap

**Warning: a snapshot must not see a commit at or below it whose
status is not yet published.** So commits publish in
`commit_ts` order. A commit claims its `commit_ts` and joins the
commit queue in one hold of a mutex. After a batch of commits is
synced, `visible_ts` rises to one below the oldest commit still
in the queue. One slow batch thus holds back new snapshots, for
about one WAL sync.

---

## 5. Writes

ROWSTORE.md §4 and LOCKMGR.md §1 have the design of record.

### 5.1 A write puts its version at once

A write puts its version into RocksDB before commit, unsynced,
under a latch on its key. The latch is a mutex held only inside
the call, so the check and the put are one step. A chained write
is one `WriteBatch`: the old CURR moves to OLD, and the new
version goes to CURR.

**The uncommitted version is the row's lock.** A writer that
finds another transaction's version in flight lets the latch go,
waits for that transaction to end, and looks again. No
lock-table entry is made, as in PostgreSQL, whose tuple holds
its writer's xid.

### 5.2 A write meets a newer commit

A write checks the key's newest committed version against its
transaction's begin snapshot:

| Level | Newest version committed after the snapshot |
| --- | --- |
| REPEATABLE READ, SERIALIZABLE | first-committer-wins: fail with `SM_ERR_CONFLICT`, and abort |
| READ COMMITTED | fail with `SM_ERR_UPDATED`, and lock the key; the caller reads the newest version with `sm1::read_latest`, checks its `WHERE` clause again, and writes again |

The READ COMMITTED exchange is what PostgreSQL's executor does
after a lock wait. sm1 enforces the read: a write repeated
without it fails again.

### 5.3 Unique keys

A unique index entry holds no row key, so one key means one row.
An insert is judged against the newest committed version, not
the snapshot. A second inserter of a key waits for the first,
and fails with `SM_ERR_DUPKEY` if the first commits
(LSMINDEX.md §4.1).

---

## 6. Commit, abort and crash

XCTMGR.md §4.4, §8 and §10 have the design of record.

- **The status record is the commit.** The versions are already
  in RocksDB. Before the record is synced they resolve as in
  flight, and after it as committed. No transaction is ever
  half-visible.
- **An abort writes nothing.** No record already means "not
  committed". An abort costs the same for any number of writes.
  Its versions stay until a later write of the key, a reader's
  repair hint, or GC removes them.
- **A crash is an abort.** No data version needs redo or undo.
  RocksDB's WAL brings back what was written, and the status
  records decide what is visible.
- **A prepared transaction** has a `PREPARED` record, always
  synced. It resolves as in flight until its coordinator commits
  or aborts it, after a restart too.

---

## 7. Statements and savepoints

XCTMGR.md §5 and §6 have the design of record.

- **A READ COMMITTED statement reads at one snapshot.** Its
  first read takes it, not its start. So a statement that waits
  on a lock still sees what committed while it waited.
- **A statement does not see its own writes.** A mark on the
  transaction's write count hides the writes made after the
  statement began. It is PostgreSQL's command id, and it stops
  the Halloween problem.
- **A savepoint is a mark, and a rollback deletes versions.** A
  write is eager, and one xid covers the whole transaction, so a
  rolled-back write must be removed. While a savepoint is set,
  each write is listed in an undo list of keys, not of values.
  The list is capped, 64 MB by default. Past the cap, the
  savepoints are lost, and the transaction can only abort.
  PostgreSQL instead gives each savepoint an xid of its own.

---

## 8. Isolation levels

XCTMGR.md §4.3 and §7 have the design of record.

| Level | Reads at | What it adds |
| --- | --- | --- |
| READ COMMITTED | a snapshot per statement | the write, re-read, write exchange (§5.2) |
| REPEATABLE READ | the begin snapshot | snapshot isolation: first-committer-wins |
| SERIALIZABLE | the begin snapshot | snapshot isolation and SSI |

Snapshot isolation allows write skew: two transactions read the
same rows, and each writes a row the other read. SERIALIZABLE
adds SSI (serializable snapshot isolation), after Cahill, Röhm
and Fekete (2008), with the refinements of Ports and Grittner
(2012):

1. A read leaves a SIREAD lock, a note of the key or range it
   read. It blocks nobody.
2. A write that meets a SIREAD lock of a concurrent transaction
   records an rw edge.
3. Two edges in a row, `T1 -rw-> T2 -rw-> T3`, are a dangerous
   structure. The commit or prepare of a transaction in one
   fails with `SM_ERR_SERIALIZATION`.

A scan's SIREAD lock is a range, so a later insert into the
range is caught. The scenarios in `sm1/mvcc` specify every
level's behaviour ([`sm1/mvcc/MVCC.md`](sm1/mvcc/MVCC.md)).

---

## 9. Locks

LOCKMGR.md §1 has the design of record. The word "lock" means
one of three things:

| Mechanism | Held | Used for |
| --- | --- | --- |
| an uncommitted version | to the transaction's end | every write of a key (§5.1) |
| a lock-table entry, in Gray's five modes | to the transaction's end | `FOR UPDATE`, `FOR SHARE`, table locks, and order on what is not written |
| a latch on a key | inside one call | making check-then-put one step |

A wait on a lock-table entry or on a writer is part of deadlock
detection. The victim fails with `SM_ERR_DEADLOCK`.

---

## 10. Garbage collection

GC.md has the design of record. There is no VACUUM to schedule.

- **The watermark** is the oldest registered snapshot, or
  `visible_ts` if none is registered. A version committed at or
  below it is seen by every snapshot.
- **The compaction filter** judges each key that a RocksDB
  compaction visits, so collection adds no pass of its own:
  - a dead version is dropped, or, in CURR, queued for repair;
  - a committed version is stamped once it is at or below the
    watermark;
  - an older version that every snapshot passes is dropped;
  - a tombstone at or below the watermark is removed, with the
    versions it covers.
- **One worker thread a database** repairs CURR, purges
  tombstones, drops the column families of dropped stores, and
  sweeps from `CF:txn` the status records no unstamped version
  needs.
- **The reapers** remove what compaction does not reach soon:
  OLD entries behind a settled CURR, and the dead entries of a
  B-tree index.
- **An idle transaction holds back GC**, as in any MVCC engine.
  GC warns once about a snapshot older than 60 seconds.
- **`VACUUM` compacts the stores**, which runs the filter now.

---

## 11. How it differs from PostgreSQL

| | PostgreSQL 18 | sm |
| --- | --- | --- |
| A version's writer | `xmin` and `xmax` in the tuple header | the writer's xid in the header; a delete is a tombstone version |
| A snapshot | `xmin`, `xmax` and a list of xids in flight | one number |
| A writer's fate | `pg_xact`, and hint bits on the tuple | the active table, the commit cache, `CF:txn`, and a stamp |
| An update | a new tuple in the heap, HOT if on the same page | the new version in CURR, the old one moved to OLD, in one batch |
| A savepoint | a subtransaction with an xid of its own | a mark, and an undo list of keys |
| An abort | marks the xid aborted in `pg_xact` | writes nothing |
| Recovery | redo of the WAL | RocksDB's WAL; no redo or undo of versions |
| Old versions | VACUUM and autovacuum | the compaction filter, the GC worker and the reapers |
| Xid wraparound | 32-bit xids, frozen by VACUUM | 64-bit timestamps; no wraparound |

---

## 12. Where to read more

| Document | What it holds |
| --- | --- |
| [`XCTMGR.md`](XCTMGR.md) | timestamps, snapshots, visibility, statements, savepoints, SSI, commit, the commit cache, 2PC |
| [`ROWSTORE.md`](ROWSTORE.md) | keys and versions in a store, the write path, the read path |
| [`LOCKMGR.md`](LOCKMGR.md) | row and object locks, waits, deadlocks |
| [`GC.md`](GC.md) | the watermark, the compaction filter, the GC worker, the reapers |
| [`LSMINDEX.md`](LSMINDEX.md), [`BTREEINDEX.md`](BTREEINDEX.md) | how an index entry is judged visible, and unique checks |
| [`DBMGR.md`](DBMGR.md) | `CF:txn`, the WAL, group sync and recovery |
| [`sm1/mvcc/MVCC.md`](sm1/mvcc/MVCC.md) | the isolation specification, as a table of scenarios |
| [`sm1/mvcc/POSTGRES.md`](sm1/mvcc/POSTGRES.md) | that specification against PostgreSQL's isolation tests |

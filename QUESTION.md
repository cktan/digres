# Questions

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

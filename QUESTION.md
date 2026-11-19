# Questions

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

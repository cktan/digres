# FX_PLAN: columnar execution, local and on a cache ring

This is a plan. Nothing in it is built but the parts that
section 9 names. It sets the direction for the columnar work, so
read it before a design or implementation choice in `../ty/`, in
sm2's column store, or in digres's scans: a choice that this
plan would have to undo needs the user's approval first.

## 1. The goal

A query on a column store table runs its scan, its filter, its
projection and its aggregation in batches, on the vectors of the
table's column groups (CGs). It runs in the digres process, or on
workers beside the nodes of a cache ring, near the CG files.
PostgreSQL's planner and executor stay above it.

An example, over a `lineitem` table of 1,000 row groups (RGs):

```sql
SELECT sum(l_extendedprice * l_discount)
FROM lineitem
WHERE l_shipdate >= '1994-01-01' AND l_discount < 0.07;
```

Each RG's fragment reads three CGs, runs the filter and the
product as one ty program, and sends back one partial sum. The
primary adds the 1,000 sums, and the rows of the delta store.

## 2. Terms

| Term | Meaning |
| --- | --- |
| primary | the digres server: it plans, holds sm1, the delta store and the catalog, and does every write |
| fragment | the part of a query a worker runs on one RG: scan, filter, projection, then partial aggregation |
| fragment executor, `fx` | the library that runs a fragment, in the primary or on a worker |
| worker | a process beside a cache node that runs fragments on the CGs that node caches |
| ring | the chord ring of distcache nodes ([distcache-cpp](https://github.com/cktan/distcache-cpp)) |
| placement hash | a 32-bit hash the caller gives with each object; the ring places the object by it |

## 3. Decisions

| Question | Decision | Rejected |
| --- | --- | --- |
| how columnar execution joins digres | a columnar subtree inside PostgreSQL's plan, grown from the bottom: scan, filter and projection, then aggregation, then hash join | a scan pushdown alone, with every node above row by row; a second executor for whole queries |
| where execution runs | in the primary, or on workers beside the ring's nodes | always on the primary |
| where the columnar operators live | `fx/`, a new top-level part, outside dg; it links no PostgreSQL, no sm1 and no RocksDB | in dg's executor; a level of sm; ty |
| what a worker links | `fx`, the CG reader library (section 7), ty, zstd, ICU, an HTTP library | sm2 whole |
| the CG cache | distcache's ring; each node caches CG files on its local disk | a cache on the primary alone; a shared cache tier read over the network |
| how an RG's CGs land on one node | the caller's placement hash, one for each RG, kept in the cache path `cache/_<8 hex>/<bucket>/<key>` ([distcache-cpp issue 1](https://github.com/cktan/distcache-cpp/issues/1)) | a hash of a key's prefix; an RG in one S3 object |
| how a worker reads a CG | from the file in its node's cache directory, by `cg_open`; through the node's `GET` on a miss | over HTTP each time |
| the primary on the ring | a client only: it sends each RG's fragment to the owner of its hash, and merges | a node of the ring too |
| with no ring | the primary runs the fragments itself, over a local CG cache, by the same `fx` | a path of its own |
| what a fragment returns | first the rows that pass; then partial aggregates | rows always |
| the delta store and writes | on the primary, by sm1, as now | |
| an expression in a fragment | a ty program, as its text; the worker compiles it | bytecode on the wire |
| the order of a fragment's errors | any error fails the statement, as in PostgreSQL's parallel query, where the first worker to fail decides the error | PostgreSQL's row order across workers |

**Why remote work needs no coordination.** A CG file never
changes (`../sm/COLSTORE.md` §5.2). A fragment reads only, holds
no state, and gives the same answer if it runs again. So a
worker that fails costs a retry on the RG's new owner.

**What a fragment takes from the primary.** The primary reads
the RG versions its snapshot sees, and their dead rows (the
validmap and the delete markers, which are in RocksDB). It sends
each worker the CG keys and a bitmap of dead rows, 1 bit a row.
The worker needs no snapshot and no sm1.

## 4. How a query runs

1. The primary plans the query. A columnar node of the plan
   holds the fragment: the columns, the ty program of the
   filter, the projection, and the aggregation.
2. When the scan begins, the primary reads the RGs its snapshot
   sees, prunes them by their zonemaps and filters
   (`../sm/COLSTORE.md` §12), and makes each RG's dead-row
   bitmap.
3. With a ring, it sends each RG's fragment to the owner of the
   RG's placement hash. With none, it runs each in its own
   process.
4. A worker opens the RG's CGs from its cache, decodes them to
   vectors (`cg_vec`), marks the dead rows `TY_VEC_NOTVALID`,
   runs the program, and squeezes the rows that pass
   (`tyvec_squeeze`, `tyvec_dict_mark`).
5. It sends back the rows that pass, or a partial aggregate.
6. The primary scans the delta store itself, merges, and hands
   the result to the plan node above, as rows or as batches.

## 5. Where the gain is

- **A filter that keeps few rows, and an aggregation.** Many
  workers read RGs at once, and send back few bytes.
- **No gain for `SELECT *` of a large table.** Every byte still
  comes back through the primary. The planner sends a fragment
  out only if its estimate says the fragment makes the data
  smaller.
- **S3's latency is the cache's to hide.** A worker reads a hit
  from its local disk. The placement hash sends an RG to the
  same node each time, so its cache stays warm. New CGs go into
  the owner's cache when a seal or a compaction writes them
  (distcache's `PUT`).

## 6. The steps

1. **Convert a SQL expression to a ty program** (dg). The
   scan's quals, at scan begin: `Var`, `Const`, a `Param` known
   then, the built-in operators by function OID, `AND`, `OR`,
   `NOT`, the `IS` tests, `RelabelType` and the numeric casts,
   on int2 to int8, float4, float8, bool, date and the
   timestamps. Only the longest run of quals that converts, from
   the first, goes into the program; the rest stay with
   PostgreSQL. → verify: a debug GUC that also runs each
   converted program on every row, against PostgreSQL's result,
   through `run.sh` and pgregress.
2. **Split the CG reader out of sm2** (section 7). → verify: a
   program that builds, opens and reads a CG links with no sm1
   and no RocksDB; sm, aa and dg's suites pass.
3. **`fx`, stage 1, local:** a fragment of scan, filter and
   projection on one RG, in the primary's process, giving the
   rows that pass; a columnar scan node in dg that runs it over
   the RGs and merges the delta store. → verify: the same rows
   as the scan without it, on `run.sh`'s column store tables;
   then a Q6-like scan against PostgreSQL.
4. **`fx`, stage 2: partial aggregation.** `count`, `sum`,
   `min`, `max` and `avg` with no `GROUP BY`, then with a hash
   `GROUP BY`. Partial states are combined on the primary, with
   PostgreSQL's results: a decimal sum's scale, an overflow's
   error. → verify: random queries against PostgreSQL; Q1 and
   Q6.
5. **Remote fragments**, after distcache's placement hash: a
   worker beside each node, the fragment and its result on the
   wire, retry on a failed node. → verify: the same results on
   a ring of 3 as locally; a node stopped during a query.
6. **Hash join**: a small table sent to every worker first.

## 7. The CG reader split

What `../sm/sm2/src/col/cg.cc` needs, measured on its object
files:

- **It links no sm1 object and no RocksDB.** The closure of
  `cg.o` is sm0's `crc.o`, `error.o` and `format.o`; sm2's
  type objects (`type.o`, `bool.o`, `int.o`, `float.o`,
  `decimal.o`, `datetime.o`, `interval.o`, `text.o`, `uuid.o`,
  `collation.o`) and ryu; and `tuple.o`, for `varlena_payload`
  alone. Outside the repository: zstd, ICU and libm. It needs
  no `key.o`, and links nothing of ty: `cg.cc` takes only the
  flag constants of `ty_vec.h`.
- **Only the header ties it to sm1.** `sm2.h` includes `sm1.h`,
  and names `sm1::` about 60 times, almost all in the rel, the
  index and the catalog interfaces. Of the parts the CG reader
  uses, only `key_budget()` does, by `sm1::max_keylen`.

The split:

1. Split `sm2.h`: `sm2_type.h` (types, collation, varlena, tuple
   descriptors) and `sm2_cg.h` (a CG's build and read), neither
   including `sm1.h`. `sm2.h` keeps the rel, index and catalog
   interfaces and includes both, so no caller changes.
   `key_budget()` moves into `sm2.h`.
2. Move `varlena_payload` beside the type code, so the library
   takes no `tuple.o`.
3. Build `libsm2cg.a` of those objects, linked with no sm1.
   sm2's own library stays as it is.
4. A test links a program with `libsm2cg.a`, ty, zstd and ICU
   alone: it builds a CG, opens it and reads its vectors. A
   change that pulls in sm1 fails its link.

## 8. Rules for work now

These hold now, so that the steps above stay possible:

- **ty, and the CG reader, depend on no PostgreSQL, no sm1 and
  no RocksDB.** A vector operation or an engine feature goes in
  ty, not in dg.
- **A ty program travels as its text.** Bytecode stays in
  memory, in its process.
- **A columnar operator holds no state between batches but its
  own**, and reads its inputs as vectors and flags (`ty_vec.h`).
- **Each column has its own flags.** `tyvec_squeeze` takes a
  vector with no failed row: its errors are raised first.
- **A decimal column's vector holds its integers alone**,
  `int64_t` or `ty_int128_t`, as its scale is the column's.
- **A CG is named by its key, not by a local path.** Code that
  opens a CG takes a file name or bytes from the caller, so a
  worker can open one from its cache directory.
- **Nothing above the delta store assumes rows come from RocksDB
  alone.** A scan of a column store table merges RGs read
  elsewhere with the delta store.

## 9. What is built

- ty's vector operations, the evaluation engine
  (`../ty/EVAL_PLAN.md`), `tyvec_squeeze` for each width, and
  `tyvec_dict_mark`.
- sm2's CG decode to vectors (`cg_vec`,
  `../sm/COLDECODE_PLAN.md`).
- Nothing of sections 6 and 7.

## 10. Open questions

1. How the planner chooses a fragment, local or remote: by its
   estimate of the rows and bytes a fragment sends back.
2. The fragment's form on the wire: the Janet-like language
   grown from expressions to fragments, `(scan ...)`,
   `(filter ...)`, `(partial-agg ...)`, or a binary form.
3. The form of a partial aggregate on the wire.
4. A predicate on a dictionary's codes, with `tyvec_dict_mark`,
   in place of the flat values.
5. A worker's platform: long-lived processes beside each node,
   with their caches warm; serverless functions are possible if
   a fragment holds no state.
6. Joins of two large tables: on the primary until a shuffle is
   designed.

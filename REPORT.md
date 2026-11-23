# REPORT: a columnar executor for the Column Store

This report studies DuckDB's executor, for use on digres's
column store tables. It also proposes a new interface of sm that
hands out columns, and vector operations in `ty/`. The date is
2026-11-21. The sources are DuckDB v2.0.1 and pg_duckdb v1.2.1,
read at `bench/build/study/` (§12).

---

## 0. Summary

- **The Column Store saves reads, not work.** A scan reads only
  the columns a query uses, and skips row groups. Then it makes
  each row again, and PostgreSQL's executor takes one row at a
  time. Each value is copied about three times on the way
  (§2).
- **DuckDB's executor cannot be taken alone.** Its operators
  need its `ClientContext`, catalog, planner and buffer
  manager: 97 of the 229 files of `src/execution` use
  `ClientContext`. To take the executor is to take DuckDB,
  676k lines and 372k more of third-party code (§3.4).
- **DuckDB can be embedded whole.** One instance for the
  process, one connection a session, and `threads=1` to start,
  so each query runs on its session's thread. An sm scan is a
  DuckDB `TableFunction` that fills DuckDB's vectors from the
  column files, mostly with no copy (§3.3, §8.5).
- **DuckDB's answers differ from PostgreSQL's** in at least ten
  ways: `7/2` is 3.5, `sum(int4)` is a 128-bit integer, a float
  divided by zero is infinity, and more (§5). Each is a wrong
  answer, which digres's rule forbids. A list of allowed plan
  nodes, types and functions keeps out what is not proven
  equal; the rest stays on PostgreSQL's executor.
- **The sm interface and the vector operations come first.**
  They serve any columnar executor: DuckDB's, one of our own,
  or a batch mode in PostgreSQL's. Neither depends on DuckDB
  (§8, §9).

**Recommendation:** build in this order (§7):
1. columnar decode in sm2;
2. the batch scan in `sm.h`;
3. the vector operations in `ty/`;
4. a prototype of DuckDB under a CustomScan, for one plan
   shape: a scan, a filter, a projection and an aggregate, on
   column store tables only;
5. a decision from measured results: widen DuckDB's use, or
   build our own operators.

---

## 1. Terms

| Term | Meaning |
| --- | --- |
| RG | row group: at most 10k rows of a column store table, sealed together |
| CG | column group: the values of one column in one RG; one immutable file |
| delta store | the table's row store for new rows, before a seal |
| delete marker | a key that marks one row of an RG deleted |
| batch | the rows of one part of an RG, or of the delta store, handed out as one vector per column |
| vector | the values of one column for the rows of a batch, as an array |
| validity bitmap | one bit a row: 1 if the value is not null |
| selection vector | an array of row positions in a batch: the rows still in play |
| morsel | the unit of work of one thread in a parallel scan; here, one RG |
| offload | running part of a query on DuckDB's executor, not PostgreSQL's |
| allow list | the plan nodes, types and functions that may be offloaded |

---

## 2. The scan today

A scan of `SELECT sum(amount) FROM ev WHERE kind = 'view'` reads
the CGs of `amount` and `kind`, for each RG that may match:

1. `cg_open` decompresses the CG file, and decodes every row's
   value into a `std::string` of its own
   (`sm/sm2/src/col/cg.cc:376`, `:641`). Dictionary codes and
   integer deltas are expanded here.
2. `rg_read_next` takes the next row whose `_id` is not in the
   RG's set of deleted `_id`s (a `std::set`), and gives its
   values as datums (`sm/sm3/src/colstore/colstore.h:130`).
3. `col_scan_next` forms an sm2 tuple of those values
   (`colstore.h:176`). This is where a row is limited to 64 KB.
4. digres calls `sm_row_scannxt` and `sm_pin_deform`, which take
   the tuple apart into PostgreSQL's slot
   (`dg/src/backend/access/sm/smstore.c:3891`).
5. PostgreSQL's executor tests `kind = 'view'` and adds `amount`,
   one row at a time.

The costs:

- **Three copies of each value:** into the decoded `std::string`,
  into the tuple, and from the tuple into the slot for a value
  passed by value.
- **One allocation a value** that is longer than the small
  string buffer of `std::string`.
- **No use of the columnar layout:** no code sees a dictionary,
  a run of equal values or an array of integers.

---

## 3. DuckDB's executor

### 3.1 The data format

- **A `DataChunk` is a list of vectors**, at most
  `STANDARD_VECTOR_SIZE` rows, 2048, set at compile time
  (`duckdb/src/include/duckdb/common/vector_size.hpp:16`). An RG
  of 10k rows is five chunks.
- **A vector is flat, constant, dictionary, sequence or FSST**
  (`enums/vector_type.hpp:15-22`). A dictionary vector is a
  dictionary and a selection vector of `uint32_t` codes
  (`vector.hpp:92-96`).
- **The validity bitmap is `uint64_t` words, bit 1 for a valid
  row**, and absent when no row is null
  (`validity_mask.hpp:50`, `:73`).
- **A string is a 16-byte `string_t`**: a `uint32_t` length,
  then 12 bytes in line for a string of 12 bytes or less, or a
  4-byte prefix and a pointer (`string_type.hpp:24-37`).
- **A vector can borrow memory.** `FlatVector::SetData` points a
  vector at outside memory, and `AddAuxiliaryData` keeps an
  object alive for as long as the vector's buffer lives
  (`flat_vector.cpp:472-487`, `vector_buffer.hpp:119`). DuckDB's
  own Arrow scan works so
  (`function/table/arrow_conversion.cpp:93`).
- **Its physical types match sm's widths**, but not all of
  sm's encodings:

| Type | sm2's native form (`sm/sm2/TYPE.md` §3) | DuckDB's | To convert |
| --- | --- | --- | --- |
| int2, int4, int8, float4, float8, bool | the C type | the C type | nothing |
| decimal64, decimal128 | value × 10^s, shifted left 5 or 6 bits, with s in the low bits | value × 10^s, int64 or int128 | an arithmetic shift right |
| date | int32 days since 2000-01-01 | int32 days since 1970-01-01 | add 10,957 |
| timestamp, timestamptz | int64 µs since 2000-01-01 | int64 µs since 1970-01-01 | add 946,684,800,000,000 |
| uuid | 16 bytes, big-endian | int128, top bit flipped (`uuid.cpp:68`) | a byte swap and a bit flip |
| text, varchar, bytea | a varlena: a header, then the bytes | `string_t` | a 16-byte header a row; the bytes stay |
| bpchar | padded with blanks | plain VARCHAR, no padding rules | not equal: keep out (§5) |

### 3.2 The execution model

- **Push-based pipelines.** A pipeline runs a source, then
  operators, then a sink (`parallel/pipeline.hpp:121`).
  `PipelineExecutor` pushes each chunk through the operators
  before it asks the source for the next one
  (`parallel/pipeline_executor.cpp:560-653`).
- **Morsel-driven parallelism.** A pipeline's threads are the
  least of its source's, operators' and sink's `MaxThreads`
  (`parallel/pipeline.cpp:128-163`).
- **The calling thread works too.** `Executor::ExecuteTask` makes
  the caller run the query's tasks
  (`parallel/executor.cpp:468-512`). With `threads=1` DuckDB
  starts no worker thread, so a query runs on its session's
  thread (`parallel/task_scheduler.cpp:387-402`). Also set
  `async_threads=0`: its pool starts at startup
  (`main/database.cpp:335-337`).
- **Spill to disk.** Hash join, hash aggregate and sort spill
  through DuckDB's buffer manager to `temp_directory`. Its
  default is `.tmp` in the current directory
  (`main/config.cpp:674-681`), so digres must set it under the
  data directory. `memory_limit` defaults to 80% of the
  machine's memory (`config.cpp:664-670`).
- **Cancel.** `ClientContext::Interrupt()` sets a flag, which
  long loops poll (`main/client_context.cpp:1141-1144`).

### 3.3 The scan seam

A **`TableFunction`** is the smallest seam
(`function/table_function.hpp:430-596`):

- `bind` gives the column types; `init_global` and
  `init_local` make the scan's state; `function` fills one
  chunk of at most 2048 rows, and 0 rows ends the scan.
- With `projection_pushdown`, `init_global` gets the columns to
  read (`column_ids`).
- With `filter_pushdown`, it gets the filters (`TableFilterSet`).
  **A filter the scan takes is not applied again above it**
  (`optimizer/pushdown/pushdown_get.cpp:113-170`): the scan must
  apply it exactly, or decline it with `supports_pushdown_type`.
- `ExpressionFilter::CheckStatistics` tests a filter against min
  and max, and Parquet's reader uses it to skip row groups
  (`extension/parquet/parquet_reader.cpp:1670`).
- `statistics` and `cardinality` give the optimizer column
  statistics and a row count.
- `GlobalTableFunctionState::MaxThreads()` sets the scan's
  threads: the morsels.

A **`StorageExtension`** with a `Catalog` of its own is needed
only if DuckDB looks up tables by name, as for SQL text. That is
about 15 functions to write, and a copy of digres's catalog
(`catalog/catalog.hpp:140-405`).

### 3.4 Coupling and size

- **The operators are not separable.** 97 of the 229 `.cpp`
  files of `src/execution` use `ClientContext` (529 uses).
  `PhysicalHashJoin` is made from a `LogicalOperator`
  (`execution/operator/join/physical_hash_join.hpp:44`). The
  expression executor takes a `ClientContext`
  (`execution/expression_executor.hpp:27-30`).
- **A plan can skip the parser and binder.** A
  `LogicalPlanStatement` takes a `LogicalOperator` tree built by
  hand (`parser/statement/logical_plan_statement.hpp:16`). It
  still goes through the optimizer, whose passes can be turned
  off one at a time.
- **Physical plans built by hand save little.** The planner of
  physical operators is about 4,500 lines
  (`execution/physical_plan_generator.cpp`,
  `execution/physical_plan/`).
- **Substrait is not in the tree.**
- **`sum`, `avg` and most aggregates are in the
  `core_functions` extension**, which is linked by default.
- **License MIT; C++17 or later; exceptions required; RTTI
  used.** Its settings, scheduler and allocator belong to one
  `DatabaseInstance`; no global singleton was found.

| Part of DuckDB `src` | Lines |
| --- | --- |
| all of `src` | 675,977 |
| execution (operators 45,156) | 66,860 |
| function | 61,649 |
| common | 88,475 |
| storage | 54,618 |
| parser | 50,035 |
| planner | 48,052 |
| optimizer (48 passes) | 44,248 |
| third_party | 372,185 |

---

## 4. pg_duckdb: what it does

pg_duckdb is a PostgreSQL extension that runs queries on
DuckDB. It pins DuckDB v2.0.1, the version studied here.

- **The query goes to DuckDB as SQL text.** A planner hook
  turns the `Query` back into SQL with PostgreSQL's ruleutils,
  copied once for each PostgreSQL version (13,696 lines for 18),
  and DuckDB parses it again
  (`pg_duckdb/src/pgduckdb_planner.cpp:179-234`). The plan is one
  CustomScan.
- **DuckDB never reads a heap page.** Each scan of a PostgreSQL
  table is turned back into SQL text, which PostgreSQL's own
  executor runs, row by row
  (`src/scan/postgres_table_reader.cpp:56-103`).
- **Every call into PostgreSQL holds one global mutex**, as
  PostgreSQL is not thread-safe
  (`include/pgduckdb/pgduckdb_process_lock.hpp:12-19`).
- **Results come back one value at a time**, through DuckDB's
  `Value` type. Its own code marks this a FIXME
  (`src/pgduckdb_node.cpp:391-402`).
- **Each DuckDB error is `XX000`.** The SQLSTATE is lost.
- **A query DuckDB cannot prepare falls back** to PostgreSQL
  when DuckDB was optional; it errors when DuckDB was required.
- **It sets `default_null_order='postgres'`** and keeps
  `TimeZone` in step. It does not map collations, nor set
  integer division.
- **No savepoints, no 2PC, no mixed writes.** Those rules exist
  because DuckDB has its own MVCC.
- **Its own code is about 17,800 lines**, and 77 regression
  files, which list the traps of semantics.

**For digres:**

| Copy | Avoid |
| --- | --- |
| a CustomScan that holds the offloaded plan | SQL text in both directions |
| fall back when DuckDB is optional and cannot prepare | the `Value` API on the result path |
| `default_null_order='postgres'`, the time zone kept in step | losing the SQLSTATE |
| the cancel loop: `Interrupt`, cancel tasks, drain | one DuckDB instance a session, with all cores and 4 GB each |
| their regression files, as a list of traps | a row count as the only statistic |

**What is simpler in digres:** one process, so one DuckDB
instance and one memory limit for the server; C++ exceptions, so
no `sigsetjmp` guards; and its own column files, so a scan fills
vectors with no PostgreSQL code at all. Mixed writes are no
problem: DuckDB only reads, at sm's snapshot.

**What is harder:** a DuckDB worker thread has none of a
session's thread-local state. So code that runs on one must be
sm's and ty's only, never PostgreSQL's; the snapshot is taken on
the session's thread and passed in.

---

## 5. Where DuckDB's answers differ from PostgreSQL's

Each row is a wrong answer if a query that meets it is
offloaded as it is. The fix column says how the prototype
handles it.

| Case | PostgreSQL | DuckDB, by default | Fix |
| --- | --- | --- | --- |
| `7 / 2` on integers | 3 | 3.5: `/` casts integers to double (`function/scalar/operator/arithmetic.cpp:1503-1513`) | register ty's division as `/` |
| `x / 0.0` on floats | error | infinity or NaN (`ieee_floating_point_ops`) | register ty's, or set it off |
| `sum(int4)` | int8 | 128-bit integer (`core_functions/aggregate/distributive/sum.cpp:407-440`) | register our `sum` |
| `avg(int4)` | numeric | double | register our `avg` |
| NULLs in `ORDER BY x DESC` | first | last | `default_null_order='postgres'` |
| decimal division | digres's rule: the type of the operands | double, or SQL Server's scale rules | register ty's |
| text order and equality under ICU | ICU's, by the column's collation | bytes (`memcmp`); its "icu" collator is its own code | offload text tests only for a `C` collation, or pass sm2's sort keys |
| `bpchar` | trailing blanks ignored | plain VARCHAR | keep out |
| `timestamptz` and time zones | the session's `TimeZone`, PostgreSQL's zone data | UTC in core; the icu extension's own zone table | keep out, or convert in digres |
| `x + 1 = 5` | an overflow error when x is the largest int4 | rewritten to `x = 4`: no error (`optimizer/rule/move_constants.cpp`) | turn off that pass, or accept it |
| float to text | PostgreSQL's shortest form, `Infinity`, `NaN` | `fmt` (`common/operator/string_cast.cpp:70-72`), not checked | text output stays in digres |
| an error's SQLSTATE | kept | `what()` only (`parallel/executor_task.cpp:57-61`) | keep the error in a thread-local (§8.5) |

What is equal, from the code: integer overflow raises an
error; NaN sorts above every number; a float to integer cast
rounds half to even; an interval's month is 30 days.

**A function we register replaces DuckDB's.** One with the same
name and argument types overrides the built-in one
(`include/duckdb/function/function_set_base.hpp:60`). So `ty`'s
operations can be what DuckDB runs. DuckDB's optimizer still
assumes its own `+`: its rewrite rules match by name
(`optimizer/rule/arithmetic_simplification.cpp:18`).

---

## 6. The options

| Option | What it is | Cost | Risk |
| --- | --- | --- | --- |
| A. DuckDB, a plan built by hand | embed DuckDB; build a `LogicalOperator` tree from PostgreSQL's plan for an allowed part; run it under a CustomScan; the sm scan is a `TableFunction` | a translator, about 5k–15k lines, and a type bridge | §5; DuckDB's optimizer reorders; a dependency of 1M lines whose insides change by version (v2.0 rewrote `Vector`) |
| B. DuckDB, SQL text | as pg_duckdb: deparse to SQL, a `Catalog` of sm's tables | less translator, a copied catalog | the highest: DuckDB binds the query again, with its casts and its functions |
| C. our own operators | a scan, a filter, a projection and a hash aggregate on batches, on ty's vector operations, under PostgreSQL's executor; joins and sorts stay PostgreSQL's | about 5k–10k lines for the first operators | slower to reach joins, sorts, windows and spill |
| D. DuckDB's operators alone | fork `src/execution` | 150k lines or more, tied to all of DuckDB | not recommended |

A and C share everything below the executor: the sm batch scan
(§8) and the vector operations (§9). C keeps one set of
semantics; A gains years of DuckDB's work on joins, sorts,
windows and spill, at the cost of §5.

---

## 7. The recommendation, as steps

1. **Columnar decode in sm2.** `cg_open` decodes into arrays: a
   fixed-width array, an array of string views over one buffer,
   or a dictionary's codes and its values. No `std::string` a
   value. → verify: `sm/sm2/test/test_cg.cc`, the full suites
   unchanged, and the scan's instructions measured before and
   after.
2. **The batch scan in `sm.h`** (§8): open, next batch, hold and
   release. → verify: sm3 and rel tests that a batch scan gives
   the same rows as a row scan, under a random workload with
   updates, deletes, seals and compaction.
3. **The vector operations in `ty/`** (§9), for the integers,
   bool, decimal and the compares. → verify: unit tests and
   Python vectors, as for ty's scalar operations; null and
   selection rules tested.
4. **The DuckDB prototype** (option A), for one plan shape: an
   aggregate over a filter over a scan of a column store table.
   - DuckDB v2.0.1 linked statically into digres, in its own
     translation units; one instance; `threads=1`,
     `async_threads=0`, `temp_directory` under the data
     directory.
   - The scan is a `TableFunction` on the batch scan.
   - ty's operations registered for the operators of §5.
   - An allow list; anything else stays on PostgreSQL's
     executor.
   → verify: `dg/test` with offload on and off gives the same
   answers; time measured on queries of the TPC-H Q1 and Q6
   kind, against digres's row path and PostgreSQL.
5. **The decision**, from step 4's numbers: widen the allow
   list to joins, sorts and windows (option A), or build our
   own operators (option C).

Steps 1 to 3 are useful under either option, and come first.

---

## 8. A new sm interface: batches of columns

### 8.1 Rules

- **Neutral.** The batch is plain arrays, so any executor can
  take it: DuckDB's, our own, or PostgreSQL's in a batch mode.
  sm does not include DuckDB.
- **sm's forms, not an executor's.** A date is days since
  2000-01-01, as sm stores it; an executor converts if it must
  (§3.1). sm does not change its format for one executor.
- **What is encoded stays encoded.** A dictionary CG goes out
  as codes and a dictionary; an executor may test the
  dictionary once, not each row.
- **No tuple.** A batch has no limit of 64 KB a row.
- **A live row is in the selection vector.** A deleted row is
  left out of the selection; no value is moved.
- **Pruning is not filtering**, as today: bounds skip RGs, and
  the executor applies its filters.
- **One reader, one thread, to start.** The scan runs on its
  session's thread. Parallel morsels are a later step (§10).

### 8.2 The interface

```c
/* The form of one column of a batch. */
enum sm_vec_kind_t {
    SM_VEC_FLAT,  /* n values */
    SM_VEC_DICT,  /* n codes into a dictionary of dict_n values */
    SM_VEC_CONST, /* one value for every row */
};

/* A string, binary or text value: the bytes, no varlena header.
   A string of 12 bytes or less is in line; a longer one has its
   first 4 bytes in prefix and points at the rest. */
typedef struct sm_str_t {
    uint32_t len;
    union {
        char inl[12];
        struct { char prefix[4]; const char* ptr; } ref;
    };
} sm_str_t;

/* One column of a batch. */
typedef struct sm_vec_t {
    sm_vec_kind_t   kind;
    const void*     data;   /* FLAT, CONST: the values; DICT: the
                               dictionary's values */
    const uint64_t* valid;  /* bit i 1: row i not null; NULL:
                               no null */
    const uint32_t* codes;  /* DICT: a code a row */
    uint32_t        dict_n; /* DICT: the dictionary's values */
} sm_vec_t;

/* One batch: at most the scan's batch_rows rows, from one RG or
   from the delta store. */
typedef struct sm_batch_t {
    uint64_t        rg_id;  /* 0: the delta store */
    uint32_t        n;      /* rows in each vector */
    const uint32_t* sel;    /* the live rows, n_sel of them;
                               NULL: all n */
    uint32_t        n_sel;
    const int64_t*  ids;    /* the _id of each row */
    const sm_vec_t* cols;   /* one a column asked for, in order */
} sm_batch_t;

typedef struct sm_batch_scan_t sm_batch_scan_t;
typedef struct sm_batch_hold_t sm_batch_hold_t;

/* Open a batch scan of a column store table at the transaction's
   view: the columns attnums, the RGs that may meet bounds, then
   the delta store, in batches of batch_rows rows at most. */
int sm_batch_scanopen(sm_tbl_t* tbl, const int32_t* attnums,
                      size_t ncols, const sm_col_bound_t* bounds,
                      size_t nbounds, uint32_t batch_rows,
                      sm_batch_scan_t** out);

/* The next batch: 0 with it, 1 at the end, -1. It is valid
   until the next call, unless held. */
int sm_batch_scannxt(sm_batch_scan_t* scan, sm_batch_t* out);

/* Keep the memory of the last batch past the next call, until
   sm_batch_release. Release may run on any thread. */
sm_batch_hold_t* sm_batch_hold(sm_batch_scan_t* scan);
void             sm_batch_release(sm_batch_hold_t* hold);

void sm_batch_scanclose(sm_batch_scan_t* scan);
```

`sm_str_t` has the layout of DuckDB's `string_t` and of the
"German string" of Umbra and Velox, so DuckDB takes an array of
them with no copy. Arrow's string view differs only in its last
8 bytes: a buffer index and an offset, not a pointer.

### 8.3 How a batch is made

1. **Open:** read the RG keys at the transaction's view, and
   drop the RGs that the bounds rule out, as `col_scan_open`
   does today.
2. **For each RG:** read and decode the CGs asked for, and the
   `_id` CG, in the columnar form of step 1 of §7.
3. Read the RG's delete markers at the view, as `rg_read_open`
   does. Make the selection: each position whose `_id` has no
   marker.
4. Hand out the RG in batches of `batch_rows`: each vector
   points into the decoded CG, at an offset. No value is
   copied.
5. **The delta store last:** scan its rows at the view, and
   append each value to a builder a column, up to `batch_rows`
   rows. A value stored in TOAST is read whole, into the
   batch's buffer.
6. A held batch keeps its decoded RG by a reference count. The
   decoded memory refers to no sm1 state, so its release is
   safe on any thread.

### 8.4 The cases

| Case | What the batch scan does |
| --- | --- |
| TOAST | none in a CG (a value is 2,048 bytes at most); a delta store row's TOAST values are read whole into the batch (step 5) |
| update of an RG row | its marker drops it from the RG's selection, and its new version comes from the delta store: once |
| update A to B to A, delete and insert again | one version an `_id` at the view, as today's scan |
| abort, rollback to a savepoint, crash | nothing new: the view is the one `rg_read_open` and the delta scan use |
| a writer in flight, or prepared (2PC) | its markers and rows follow the same visibility as today |
| REPEATABLE READ, an old snapshot | the RG keys and markers are read at the transaction's view, as today |
| READ COMMITTED's re-read of a row | only for a write or a lock: those scans stay on the row path |
| SERIALIZABLE | refused for a column store table, as today |
| a seal or RG compaction in flight | the RG keys at the view decide; as today's scan |
| an open cursor | a batch is held across fetches; files are read as `rg_read_open` reads them: no new case |
| the statement writes or locks the table | the row path, as today: its scan must read every column |
| `TRUNCATE`, `DROP`, an `ALTER` that rewrites | the table lock that a scan holds keeps them out, as today |
| a bulk load and its rollback | the transaction's own RGs are seen by its later statements, as today |
| a temporary table | the same scan, on the temporary instance |
| GC watermark, the sweep of files | as today's scan |
| cancel | the executor checks between batches |

The batch scan changes no rule of visibility. It makes a
selection where today's scan skips a row, so the two scans must
give the same rows. Step 2 of §7 tests exactly that.

### 8.5 Onto DuckDB's `TableFunction`

- `bind`: sm's types to DuckDB's `LogicalType`; a column of a
  type not on the allow list keeps the query off DuckDB.
- `init_global`: `sm_batch_scanopen`, with `batch_rows` 2048,
  the columns from `column_ids`, and bounds made from the
  filters. `MaxThreads()` is 1 to start.
- `function`: `sm_batch_scannxt`, then for each column:
  - fixed width, with no conversion: `FlatVector::SetData` on
    the vector's data, and an `AuxiliaryDataHolder` that owns
    an `sm_batch_hold`;
  - date, timestamp, decimal, uuid: convert into the chunk's
    own vector (§3.1);
  - string: the `sm_str_t` array, the same layout;
  - dictionary: `Vector::Dictionary` with the codes as its
    selection;
  - the live rows: `DataChunk::Slice` by the batch's selection.
- Filters: take only what the scan can apply exactly with ty's
  operations; decline the rest with `supports_pushdown_type`,
  and DuckDB applies them above the scan.
- Errors: with `threads=1`, the error happens on the session's
  thread. The scan and ty's functions keep the error, its
  SQLSTATE included, in a thread-local before they throw.
  digres raises the kept error when DuckDB's exception reaches
  the CustomScan.
- Results: the CustomScan reads the result chunks' flat vectors
  into its slot, not through `Value`. An aggregate's result is
  small.

---

## 9. Vector operations in `ty/`

Today `ty/` holds scalar operations:
`ty_int32_add(a, b, &r)` gives `r`, or `TY_ERR_RANGE` on
overflow (`ty/include/ty_int.h:149`). A columnar executor needs
the same operations on arrays:

```c
/* out[i] = a[i] + b[i] for each row i of sel, n_sel of them
   (every row of n when sel is NULL), skipping null rows.
   0, or -1 with TY_ERR_RANGE and *bad the first row that
   overflows. */
int ty_int32_add_vec(const int32_t* a, const int32_t* b,
                     const uint64_t* valid, const uint32_t* sel,
                     uint32_t n_sel, uint32_t n, int32_t* out,
                     uint32_t* bad);
```

The rules:

- **One meaning.** A vector operation gives, for each row, what
  the scalar one gives. Tests compare the two on every row.
- **A null row is not computed.** A null row's slot holds any
  bits, so an operation that can fail must skip it. If not, it
  can raise an error that no row deserves.
- **An operation that can fail runs only on the selected
  rows.** In `SELECT a / b FROM t WHERE b <> 0`, a division
  over every row of the batch divides by zero on the rows the
  filter dropped. PostgreSQL raises no error there.
- **An operation that cannot fail may run on every row**: a
  compare, a hash, a move. That is faster, and the compiler
  can vectorize it.
- **No branch a row for overflow.** Add with wrap-around into
  `out`, and OR the overflow flags of the rows. Test the flag
  once a batch; scan for `*bad` only when it is set.
- **The result of a compare is a selection vector or a
  bitmap**, so a filter narrows the selection with no copy.
- **Decimal follows its rule:** `decimal64` with `decimal64`
  gives `decimal64`, exact, or an error (`dg/DECIMAL.md`).

Under option A, these are the functions digres registers in
DuckDB for the operators of §5. Under option C, they are the
executor's own.

---

## 10. Open questions

1. **Parallel morsels.** Can a thread that is not the session's
   read an RG at the session's snapshot? Reading a CG file is
   plain bytes, but the delete markers are read through sm1, at
   the transaction's view. sm1's transaction is used by one
   thread today. Until this is settled, `threads=1`.
2. **Text under ICU.** Should a batch also give sm2's sort keys
   of a text column, so that any executor sorts and groups by
   bytes? It costs a key a value; it makes DuckDB's `memcmp`
   order right.
3. **The size of a batch.** DuckDB's vector is 2048 rows and an
   RG is 10k; an RG ends in a batch of 1,808 rows. That is
   harmless, but an executor of our own could take a whole RG.
4. **DuckDB's optimizer.** Which of its 48 passes stay on, as
   PostgreSQL's planner has already ordered the joins?
5. **The dependency.** DuckDB is 1M lines, and its insides
   change between versions. A prototype pinned to v2.0.1 can
   tell its cost in upgrades only over time.

---

## 11. Not covered

- A join of a column store table with a row store table:
  under option A, a row store scan would need its own
  `TableFunction`, from tuples.
- Writes through DuckDB: never. Every write stays on
  PostgreSQL's executor and sm's row path.
- DuckDB's file formats (Parquet, CSV): out of scope.

---

## 12. Sources

- DuckDB v2.0.1, commit 8d78114e, cloned at
  `bench/build/study/duckdb`. Paths in §3 and §5 are relative
  to its root.
- pg_duckdb v1.2.1, cloned at `bench/build/study/pg_duckdb`. It
  pins the same DuckDB commit. Paths in §4 are relative to its
  root.
- digres: `sm/COLSTORE.md`, `sm/sm2/TYPE.md`, `dg/DECIMAL.md`,
  `sm/include/sm.h`, `sm/sm2/src/col/cg.cc`,
  `sm/sm3/src/colstore/colstore.h`,
  `dg/src/backend/access/sm/smstore.c`, `ty/include/ty_int.h`.
- No code was built or run for this report.

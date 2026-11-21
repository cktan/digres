# TYPEMOVE_PLAN — types move into ty

Status: decimal is moved (2026-11-21); the other types are not.
The library `ty/` holds every operation of `decimal64` and
`decimal128`, with its error state and its per-thread allocator
(§3); `decimal.c` in digres keeps only the PostgreSQL glue. This
plan settled the interface of an operation before any code moved,
and §7 says what was done and what it cost. The item is in
[`NEXT.md`](NEXT.md), item 11.

Every fact below was read from the code on 2026-11-09. Each
number marked "grep" is a count, not an audit.

---

## 1. What this is

A type's operations (add, round, `sqrt`, compare, hash) are in
digres today. sm2 keeps a type's storage only
([`sm2/TYPE.md`](sm2/TYPE.md)). The item moves the operations
into [`ty/`](../ty/DESIGN.md), a library of its own that depends
on nothing else in the repository. sm uses ty for the type API,
and digres calls it too. There are two reasons:

- sm evaluates expressions without digres (an index expression,
  a partial index, a check).
- sm plugs into another SQL system, not into digres alone.

Moved code cannot call `palloc()`. It cannot call `ereport()`
or read a session's setting either. These three are the
design problem. The arithmetic itself is not.

## 2. Where things stand

- **sm2's type table is already C++ with owned results.** A
  `type_t` has `in`, `out`, `key_encode` and more, and they
  write into a `std::string&` (`sm2/include/sm2.h`). This is
  fine for a storage function. It is not the call digres makes
  for `a + b` in a hot loop.
- **A datum is PostgreSQL's `Datum` layout** (`datum_t`).
  `decimal64` is by value: no memory for a result. `decimal128`
  is by reference, 16 bytes.
- **`arena_t` cannot be backed by `palloc()`** (`sm0/include/
  sm0_arena.h`). It cuts pieces from blocks it takes with
  `new`, it has no mutex, and it frees all at once. An earlier
  line of [`NEXT.md`](NEXT.md) said digres backs an arena with
  `palloc()`. §3 does not need it: an arena serves only sm's
  own threads.
- **digres's `decimal.c` is new code, not PostgreSQL's**: 3,575
  lines, 159 functions with `PG_FUNCTION_ARGS`, 15 `palloc`
  calls and 51 `ereport` or `elog` calls (grep). Most `palloc`
  calls are in wrappers: a value copied out, an output string,
  an aggregate's state, a set-returning function's state. The
  arithmetic core seems to need none. This is why decimal is
  the first type to move.
- **The errors are mostly a few kinds** (grep of `ERRCODE_` in
  `decimal.c`): out of range 20, invalid parameter 7, bad text
  5, bad binary 4, power 4, log 2, width bucket 2, division by
  zero 1, others 2.
- **sm's error codes have no kind for these.** `SM_ERR_INVALID`
  is the nearest (`include/sm_error.h`). The last is
  `SM_ERR_MOVED`.

## 3. Decision 1: where a result gets its memory

Settled on 2026-11-20. Three kinds of result, three answers:

| Result | Example | Memory |
| --- | --- | --- |
| By value | `decimal64 + - * / decimal64`, `int4 * int4` | none: it is the return value |
| Fixed width by reference | `decimal128 + decimal128`, `uuid`, `interval` | the caller's storage: an out-parameter |
| Variable width | `text || text`, `replace()`, `repeat()` | the calling thread's alloc function, below |

The first two need no design. A variable-width result gets its
memory from the alloc function of the thread that calls ty. The
function is general: nothing in it is specific to PostgreSQL.
The rules are in [`ty/DESIGN.md`](../ty/DESIGN.md) §3; this
section says what they mean for sm.

### 3.1 The functions

```
typedef void* (*ty_alloc_fn)(size_t size);
typedef void  (*ty_free_fn)(void* ptr);

int ty_set_alloc(ty_alloc_fn alloc, ty_free_fn free);
```

- They have the signatures of `malloc` and `free`, and the same
  rules as the allocator of sq (`sq/include/sq.h`).
- `ty_set_alloc` sets them for the calling thread only. A
  thread that sets none has `malloc` and `free`.
- `alloc` returns memory aligned to at least 8 bytes, or NULL.
  `free` takes a pointer that `alloc` returned before; ty never
  passes it NULL.
- Neither function throws or calls `longjmp`. A host whose
  allocator throws wraps it: the wrapper catches and returns
  NULL.
- ty calls them only on the thread that set them, so they need
  no thread safety of their own.

### 3.2 Who sets them

- **The host sets them on its own threads.** digres sets them
  once in each backend thread, at its start.
- **sm sets them on its own threads**: the index-build workers,
  the garbage collector, a bulk load's. Each thread gets
  functions that cut from an arena (`arena_t`) of its own. The
  arena has no mutex, and it needs none, as only its thread uses
  it. The functions have no argument, so each reads its
  thread's arena from a `thread_local`.
- **A datum made on an sm thread never goes to the host.** It is
  read in place, or copied into the index, and its arena is reset
  between rows.
- The host does not call `ty_set_alloc` on an sm thread, and sm
  does not call it on a host thread.

### 3.3 What uses them

Only a variable-width datum that ty makes and returns to the
caller:

- the result of a moved type function, such as `text || text`;
- later, a value read through a TOAST pointer. Today
  `sm_toast_fetch` fills a `std::string`, and digres copies it
  into its own memory. With the functions, the copy goes.

Nothing else changes:

- A row or an index entry from a cursor or a pin points into
  engine memory, as `sm.h` says.
- A list, a `std::string` that a call fills, and a handle stay
  as they are.
- sm2's storage functions (`in`, `out`, `key_encode`,
  `tup_encode`) keep `std::string`: their bytes go to storage,
  not to the host.

### 3.4 Who frees what

- **A result that ty returns is the caller's.** ty has no call
  to free one. The caller frees it as its own alloc function
  allows: one piece with its free, or all at once, as a
  PostgreSQL memory context does.
- **ty frees only what it allocated and did not return.** For
  example, to make X bigger, ty frees X and allocates a bigger
  X. There is no `realloc`.
- **On an error, ty frees what it allocated** and returns no
  result.

### 3.5 When alloc returns NULL

1. The call frees what it allocated.
2. The call sets `TY_ERR_NOMEM` and returns -1.
3. sm maps the code to `SM_ERR_NOMEM`, and the host maps it to
   its own error.

This is not the policy of [`DESIGN.md`](DESIGN.md), that every
`std::bad_alloc` is fatal. That policy is for sm's own
allocations, which can leave sm's state half changed. A
result's allocation changes no state, so its failure is safe
to report.

### 3.6 digres's functions

digres's `alloc` calls
`MemoryContextAllocExtended(CurrentMemoryContext, size,
MCXT_ALLOC_NO_OOM | MCXT_ALLOC_HUGE)`. It wraps the call in
`PG_TRY`, as the allocator of sq does, because a request over
the 1 GB limit raises an ERROR even with `MCXT_ALLOC_NO_OOM`.
The wrapper flushes the error and returns NULL. digres's `free`
calls `pfree()`. A result goes into the memory context that is
current at the call, and digres need not free it, as the context
does. This is one host's functions, not the design.

### 3.7 The options

| Option | For | Against |
| --- | --- | --- |
| **A. A buffer and a length.** The call returns the length it needs. | Allocates nothing. | The caller may call twice. The work may run twice, or the call must keep the result. |
| **B. An `arena_t`.** | Exists. Frees at once. | Cannot be backed by a host's allocator as built. A host copies each result into its own memory. |
| **C. An allocator passed to each call.** | Ownership is plain. Works on any thread. | An extra argument on each variable call. |
| **D. An allocator hook per session.** | No extra argument. | A session's setting is state, which the move is to remove. |
| **E. An allocator hook per process.** | No extra argument. | One setting for all threads: an sm worker would call the host's `palloc`. Needs an explicit allocator inside sm to avoid it. |
| **F. An allocator per thread, in ty.** | No extra argument. A host thread and an sm thread each have the memory they need. | A setting for each thread: each must set it at its start. |

**Chosen: F.** A is kept for `text` functions whose size is
known before the work. B serves sm's own threads, inside F. C, D
and E are rejected.

## 4. Decision 2: how an error is reported

Moved code cannot `ereport()`. digres must still say
PostgreSQL's SQLSTATE and text, so the 231 regression files
pass unchanged.

1. The operation returns -1, as every call of ty does.
2. The operation sets ty's error state, which is per thread,
   with a kind and a short message.
3. The host maps the kind to its own code and composes its own
   text from the operands it still holds.

Settled on 2026-11-20: ty keeps its own error codes in
`ty/include/ty_error.h`, and no `SM_ERR_` code is added for a
type. A type appends its kinds when it moves. The kinds in §2
are the first list.

How sm reports a ty error that reaches one of its own calls is
an open question below.

## 5. Decision 3: the call itself

Rules from [`NEXT.md`](NEXT.md): a plain C call, no handle, no
session lookup, and a statement's instructions do not move.

- The call is a C-style function in [`ty/include/ty.h`](
  ../ty/include/ty.h), or a header of ty's beside it: not in the
  2,103 lines of `sm.h`. Proposed name pattern:
  `ty_dec64_add(const ty_dec64_t*, const ty_dec64_t*,
  ty_dec64_t* result)`. A decimal is a value and a scale:
  the host packs it into a Datum, as digres does, and ty takes
  the two parts.
- sm2's `type_t` table stays for storage. The operations are
  not entries in it. sm2 calls ty where it evaluates one.
- digres's wrapper keeps `PG_FUNCTION_ARGS`, the fmgr, the
  aggregate state, the set-returning functions, the support
  nodes, the sort support and the typmod rules. It unpacks the
  datums and calls ty.
- **Cost.** A wrapper adds one call from digres into `libty.a`.
  Not measured. The measure is in §7, step "baseline".

## 6. Decision 4: what moves, by type

| Order | Type | Moves | Stays in digres |
| --- | --- | --- | --- |
| 1 | decimal | arithmetic, rounding, compare, hash, `sqrt`, `ln`, `exp`, `log`, power, no 512-bit magnitude (dropped) | wrappers, aggregates, series, planner support, typmod, error text |
| 2 | integers, bool, uuid, bytea | the same shape: pure functions | wrappers, error text |
| 3 | date, time, interval, float text, text | hold: they read `DateStyle`, `TimeZone`, `extra_float_digits` and collation | until the hooks are settled |

Step 3 is a separate note. Its hooks are the hard part. Code
copied from PostgreSQL keeps its copyright notice
([`../CLAUDE.md`](../CLAUDE.md)).

## 7. Steps

0. **The library.** Make `ty/`, with its error state and
   `ty_set_alloc`. → verify: its unit tests pass, with a
   counting alloc and free, and a second thread that has its
   own. Done on 2026-11-20.
1. **Baseline.** Record instructions a statement for three
   decimal queries, as [`../ISSUE.md`](../ISSUE.md) §2.1 does,
   and run the numeric regression files. → verify: numbers
   saved, files pass. Taken on 2026-11-20, after step 2, by
   callgrind: this VM's PMU counts cycles and not instructions,
   so `perf stat` cannot. The old binary is the release build
   with the old `decimal.c` and catalog; the new is HEAD. Each
   ran a prepared `select count(*) from dt where EXPR` over 2,000
   rows of `numeric(10,2)`, 20 and 60 statements, one run each:
   the difference over 40 statements is the count a statement.

   | EXPR | old | new | change |
   | --- | --- | --- | --- |
   | `a + b > 1000` | 5,369,321 | 4,417,015 | -17.7% |
   | `a * b > 100000` | 5,394,971 | 4,488,746 | -16.8% |
   | `(a + b) - (a - b) * 2 > 0` | 5,907,925 | 4,995,572 | -15.4% |

   On the final tree, after every move below (2026-11-21), the
   same three read 4,457,700, 4,534,525 and 5,031,062: 17.0%,
   16.0% and 14.8% fewer than the old binary, and 0.9%, 1.0% and
   0.7% more than after step 2, the cost of the later moves.

   About 450 to 480 instructions fewer for each row: the
   `palloc` of a `decimal128` result, and the compare of a
   `decimal128` with a literal. The first run of the first
   table, `a + b`, read 4,726,761 and 3,707,388 (-21.6%): the
   first start of a server on a loaded data directory costs more
   than the next. Each point is now taken after a run to warm it
   up, and `a + b` was taken again; the other two did not move.
2. **One function end to end.** Move `decimal64` add into ty
   (`ty_dec64_add`, in `ty/include/ty_decimal.h`). Its
   result is a `decimal64`, by value, and it fails with
   `TY_ERR_RANGE` past 17 digits, as decided on 2026-11-20: a
   by-reference `decimal128` result cost a `palloc` a row. A
   caller who wants more digits adds a `decimal128` to a
   `decimal64`. `-` and `*` follow in ty (`ty_dec64_sub`,
   `_mul`), exact or an error; `/` follows, rounded to 17
   digits, in 128 bits. This is decision 2's first error. →
   verify: the regression files pass, and the instruction count
   is within the limit of the open question below. The files
   that print a result's type, or a sum past 17 digits, are
   changed on purpose (`select_decimal64`, `select_numeric`).
   Built on 2026-11-20.
   The instruction count is in step 1: it fell.
3. **The rest of decimal's core**, in groups: arithmetic, then
   rounding, then functions of the exponent. → verify: the
   same, after each group. Done so far: `+`, `-` and `*` (step
   2), `neg`, `abs` and `sign`, which are inline in
   `ty_decimal.h`: a call that cannot fail has no out-parameter;
   and `%` and `gcd`, with `TY_ERR_DIVZERO` as the second error
   kind; and `lcm`, a `decimal64` that is an error past 17
   digits, as `*` is; and the compare, `ty_dec64_cmp`. Next:
   the hash, which waits for `decimal128`, as both hash alike.
   `decimal128` `+`, `-` and `*` moved too, under the rule that
   `decimal64` and `decimal128` work as `int32` and `int64` do:
   exact, or an error past 36 digits, never rounded and never
   widened. They cost `where a + b > 1000` over 2,000 rows of
   `numeric(30,4)` +0.84% and `a * b > 100000` +0.51%: in the
   limit, as the equal-scale path was in line, and is a call
   now. `decimal128`'s `neg`, `abs`, `sign`, `round`, `trunc`,
   `ceil`, `floor` and the compare of two scales moved too, all
   in 128 bits and out of the wide path: `where round(a, 2) >
   500` fell 70.9%, `where abs(a - b) > 100` 28.5%, and
   `ORDER BY a` 6.4% (that compare is the equal-scale one, which
   digres still does itself). `%`, `gcd` and `lcm` moved as well,
   in 128 bits, with no 512-bit path: where a value does not
   fit 128 bits at the larger scale, ty takes the remainder a
   place at a time. `where a % (b + 1) > 10` fell 63.0% and
   `where gcd(a, b) > 1` 89.5%. An `lcm` of two coprime
   17-digit values at scale 17 is now an error; it was rounded
   to fit.

   The 512-bit magnitude is dropped (decided 2026-11-20): a
   value that would pass 36 digits is an error, and nothing is
   rounded but a quotient. Division is first: `decimal128` `/`
   and `div()`, and `decimal64` `/`, are in ty, made a place or
   a chunk of places at a time in 128 bits. `where a / (b + 1) >
   1` fell 80.0% on `numeric(30,4)` and 81.7% on `numeric(10,2)`.
   The exponent group is in ty too, in 128 bits: `sqrt` a digit
   at a time, `exp`, `ln`, `log` and the powers in `double`
   (decided 2026-11-20: no `long double`; an overflow or an
   underflow is an error; `log` with base 10 or 2 is `log10` or
   `log2`).
   `where sqrt(a) > 5` fell 96.7%, `where ln(a + 1) > 1` 44.0%
   and `where power(a, 2) > 100` 50.7%, on `numeric(30,4)`.
   `generate_series`, `width_bucket`, `inc`, the factorial and
   the window `in_range` are in ty too: `where width_bucket(a,
   0, 1500, 10) = 5` fell 66.5% and `generate_series(1, 2000)`
   36.0%.
   The text out of a value, the conversions to a float, an
   integer and `pg_lsn`, `random(min, max)`, `min_scale` and
   `trim_scale` are on 128-bit values too: `sum(length(a::text))`
   fell 36.0% and `where a::float8 > 500` 32.8%.
   Text input, its typmod, and the binary receive and send are
   off `wide_t` too: the input reads the digits into a string of
   up to 154, rounds a long input once by its first dropped digit
   under a typmod, and then makes a 128-bit value.
   `where (a::text)::numeric > 500` fell 56.9%.
   The aggregates' sums are off `mag_t` and `wide_t` too: the
   sum is a 128-bit value, exact to 36 digits, and the squares
   a double. Then `mag_t` and `wide_t` and every helper on
   them were deleted from `decimal.c`: 3,910 lines to 3,444
   (3,580 before the work began). The hash then moved to ty,
   with its own 64-bit function (`ty_dec128_hash`), and the hot
   small functions went in line, in `ty_decimal.h`: the three
   baseline queries then read 4,408,326, 4,462,233 and 4,812,383
   instructions a statement, 17.9%, 17.4% and 18.5% fewer than
   the old binary, and 0.2%, 0.6% and 3.7% fewer than after step
   2. What is left of `decimal.c` is PostgreSQL glue, and it
   stays.
   The rounding group is
   done too: `round`, `trunc`, `ceil` and `floor`, in 64 bits.
   `where round(a, 1) > 500` over the 2,000 rows fell from
   14,631,134 to 4,375,507 instructions a statement (-70%), as
   the old path was the 512-bit one; `where floor(a) > 500` rose
   from 4,202,753 to 4,230,396 (+0.66%, in the limit): `floor`
   is a call into ty, not in line.

   The compare is a lesson in cost, measured by callgrind on
   the baseline's table (2,000 rows, 20 and 60 statements). A
   digres wrapper that unpacked both words and called ty's
   compare in line cost `ORDER BY a` 3.8% more instructions,
   `where a > b` 0.2% and `max(a)` 0.3%: over the limit. So
   digres keeps its test of one scale, where the order of the
   words is the order of the values (its own packing), and
   calls `ty_dec64_cmp_scaled` only for two scales. Then the
   three are 0.6%, 0.3% and 0.8% fewer.
4. **The allocator on each thread** (§3.2): digres sets its
   functions in each backend thread, and sm sets an arena's on
   each of its own threads. Decimal does not need it. This is
   before the first variable-width type moves. → verify: unit
   tests with a counting alloc and free find every piece freed
   once, by the caller for a result and by ty for the rest; an
   alloc that returns NULL gives `TY_ERR_NOMEM` and leaks
   nothing; an index build on sm's own thread never calls the
   host's functions.
5. **Tests at every layer** the change reaches: ty unit tests
   for each moved function (new), sm2 unit tests where sm2
   calls it, `rel` if an expression index uses one, and
   pgregress for digres. The mvcc scenarios do not apply:
   nothing here touches concurrency.

## 8. Open questions

1. How large a rise in a statement's instructions is the limit?
   This note proposes 1%.
2. Decision 1: settled, option F (§3).
3. Decision 2: settled, ty's own error codes (§4).
4. How does sm report a ty error that reaches one of its calls?
   One `SM_ERR_TYPE` and a copy of ty's message, or a mapping
   of each ty kind to an `SM_ERR_` code.
5. The header names for the operations inside ty: one header
   for a type, `ty_decimal.h`, with `ty_dec64_t` and
   `ty_dec128_t`. Is that the pattern for the next types?
6. Does `sm_toast_fetch` change to return a datum from the
   thread's alloc function, or does a new call do it, so that
   `std::string` callers stay as they are?

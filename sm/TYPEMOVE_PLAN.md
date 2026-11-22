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
   (`ty_dec128_hash`, on PostgreSQL's algorithm, which moved to
   ty with it: `ty/DESIGN.md` section 10), and the hot
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
5. **The next group**, after decimal: integers, bool, uuid and
   bytea, as [`NEXT.md`](NEXT.md) item 11 says. For each type: a
   baseline, the operations in ty, then the tests of every
   layer, as for decimal; `bytea`'s results go through the
   allocator of §3. → verify: the regression files pass, and no
   statement rises by more than 1%.
   - **Integers: done (2026-11-21).** `ty_int.h` and
     `src/int.cc`: add, sub, mul, div, mod, neg, abs, inc, dec,
     gcd, lcm, larger, smaller, the bit operations, shifts,
     the narrowings and the `int64`-double conversions, for
     `int16`, `int32` and `int64`. digres's `int.c` and `int8.c`
     call them. The tests: 5 hand cases and about 25,000
     vectors; digres `all 231 ok`. One change: a shift count
     outside 0 to the width less one is `TY_ERR_RANGE`
     (`ty/DESIGN.md` §6). The aggregate transitions
     (`int8inc` and the like), the compares, the text and
     binary forms, `generate_series` and the window `in_range`
     stay in digres.
   - **Integers, measured by callgrind**, instructions a
     statement over 100,000 rows, old and new: `int4`
     arithmetic 257,250,549 and 257,251,274 (+0.00%); `int8`
     247,138,294 and 247,420,911 (+0.11%); `int2` 281,683,442
     and 282,185,798 (+0.18%); bit operations, `abs` and `gcd`
     296,682,450 and 298,764,434 (+0.70%).
   - **Floats moved (2026-11-21), unattended.**
     The user asked for as many types as could be done in a
     long run; the hooks of section 8 were settled by passing a
     setting in: `ty_floatN_format` takes `extra_float_digits` as
     an argument. `ty_float.h`: arithmetic with PostgreSQL's
     overflow, underflow and division-by-zero rules; NaN-last
     order; the conversions; rounding; `sqrt`, `cbrt`, `exp`,
     `ln`, `log10` and `pow`; all of the trigonometry, the
     degree functions, the hyperbolics, `erf` and `gamma`;
     `width_bucket`; the hash; the text in and out; and the
     arithmetic of the statistical aggregates. PostgreSQL's Ryu
     is in `ty/src/ryu/`, and digres's copy is gone. The tests,
     besides ty's vectors: the new `dg/test/select_float.sql`
     prints the same, byte for byte, on a digres built before
     the move, so the move kept the behavior; and
     `float_text.txt` and `known.txt` hold what that digres
     printed. Measured by callgrind (old, new): float
     arithmetic and compares -0.02%; float sort -0.00%; hash
     aggregate on a float +0.29%; float text in and out
     +0.45%; float aggregates +0.02%; **a query of nothing but
     ten math functions +1.39%**, over the 1% limit. The call
     into ty, and the degree functions (`sind` +37 instructions
     a call, from a function that is not in line), cost it; the
     common path of the others is in line. The user should say
     if 1.4% on such a query is too much; the fix would be to
     put the degree functions in line too. Found on the way:
     the Ryu shim, copied from sm2, lacked `SIZEOF_SIZE_T`, so
     the Ryu code took its 32-bit path, 17% slower; ty's is
     fixed, sm2's `src/ryu/postgres_fe.h` has the same defect
     (the output is the same, only slower).
   - **text moved (2026-11-21), unattended.** The user asked
     for the text group. digres has one encoding, UTF8, and
     the C collation by default, so ty holds the UTF-8 text
     operations for the C collation (`ty_text.h`): the order,
     the equality, the hash, `length`, `substr`, `left`,
     `right`, `reverse`, `overlay`, `position`, `replace`,
     `split_part`, `repeat`, `lpad`, `rpad`, the trims,
     `translate`, `ascii` and `chr`. A collation that is not C
     or not deterministic still takes digres's old path. Left
     in digres: the case functions, `like`, the regular
     expressions, `format`, and the functions on arrays of
     text. `varchar`, `bpchar` and `name` followed (same day):
     the cut and pad to n characters, the order and hash that
     ignore trailing spaces, and the cut of a name; the test
     is `dg/test/select_char_types.sql`; measured by callgrind
     (old, new): compares of character(n) +0.28%, its text in
     and `length` -4.36%, a sort of varchar +0.11%, a sort of
     name -0.36%. The tests: about 51,000
     vectors from a Python reference, `test_text.cc`, and
     `dg/test/select_text_ops.sql`, which prints the same as the
     digres before the move. Measured by callgrind (old, new):
     `repeat` and `length` -21.90%; `substr`, `left`, `right`
     and `reverse` -30.24%; `position`, `replace` and
     `split_part` -6.98%; the pad and trim functions
     -49.97%; a sort of text -0.03%; compares and a group by
     -1.29%.
   - **LIKE moved (2026-11-21), unattended.** `ty_text_like`
     for text and bytea, and `ty_text_like_escape`, with
     PostgreSQL's lazy error for a backslash at the end of
     the pattern; a nondeterministic collation keeps the old
     matcher in digres. The tests: 800 vectors from a Python
     regular-expression reference, known answers from the old
     digres for 34 pairs, and `dg/test/select_like_ops.sql`,
     which prints the same as the digres before the move.
     Measured by callgrind (old, new): LIKE -12.61% and
     -5.64%, ILIKE -8.91%. The first version was +1.55% on
     ILIKE; `memchr` finds the starts of a match after a `%`.
   - **interval arithmetic moved (2026-11-21), unattended.**
     `ty_interval.h`: the order, the sign, the hash, `-`, `+`
     and `-` of two values, `*` and `/` by a float, and the
     three justify functions, with the infinities. Left in
     digres: the text (it reads `DateStyle` and
     `IntervalStyle`), `extract`, `date_trunc`, `age`, the
     typmod, and the arithmetic with dates and timestamps. The
     tests: about 4,300 vectors, from Python integers and a port
     of PostgreSQL's steps in Python floats; `test_interval.cc`;
     and `dg/test/select_interval_ops.sql`, which prints the
     same as the digres before the move. Measured by callgrind
     (old, new): a sort +0.13%; `sum` and `max` +0.93%; a group
     by -1.03%; `justify_interval` +0.57%. The first version
     was +2.97% on the sort: a product of 128 by 64 bits, where
     64 by 64 is enough.
   - **date, time and timestamp arithmetic moved
     (2026-11-21), unattended.** `ty_datetime.h`: the Julian
     day and back, `make_date` and `make_time`, `date + n`,
     `date - n`, `date - date`, `time + interval`, `time -
     interval`, `time - time`, a timestamp as broken-down
     time and back, `timestamp + interval`, `timestamp -
     interval` and `timestamp - timestamp`. No time zone and
     no text: those read `DateStyle` and the zone, and stay
     in digres until they are passed in, as the locale of
     money is. The tests: about 3,500 vectors from Python
     integers and a second formulation of the Julian day,
     `test_datetime.cc`, and
     `dg/test/select_datetime_ops.sql`, which prints the same
     as the digres before the move. Measured by callgrind
     (old, new): date arithmetic +0.16%; `timestamp +
     interval` +0.27%; `timestamp - timestamp` -1.67%;
     `make_date`, `make_time` and `time + interval` +0.17%.
     The first version was +4.26% on the date arithmetic: the
     calls were not in line.
   - **Every thread of digres sets ty's alloc; `a || b` moved
     (2026-11-21).** `ty_thread_init()` calls
     `ty_set_alloc(palloc_extended with MCXT_ALLOC_NO_OOM,
     pfree)`, so the main, session and adopted threads all have
     it. `ty_text_concat` makes `a || b` in the thread's alloc
     memory, after a headroom for the varlena header; it is in
     line in `ty_text.h`, with `ty_alloc` and `ty_free` in
     `ty.h` (the thread's functions are read in line). A result
     past 1 GB is "requested length too large", where digres
     said "invalid memory alloc request size". Measured by
     callgrind (old, new) on a table: concat of two columns
     +0.12%; of a column and a literal -0.05%; of a column and
     a number as text -0.03%. The first version, a call to
     ty with a call to a function that calls `palloc_extended`,
     was +1.7% to +1.9%. sm's threads set ty's alloc to an
     arena of the thread (next item).
   - **sm links ty; its threads set ty's alloc (2026-11-22).**
     `sm1::ty_thread_init()` is the first act of the garbage
     collector, the database reaper, the log zeroing, the
     index builders, the sealer and the btree reapers. A piece
     is a multiple of 8 bytes of a `thread_local` arena that the
     thread's end frees, and `free` does nothing. The timer
     thread of sm0 has none: sm0 depends on nothing. The
     Makefiles of sm1, sm2, sm3, rel and aa link `libty.a`, from
     `ty/ty.mk`; digres's link line now has ty after sm. No call
     of sm allocates through ty yet.
   - **ty reads varlenas; ty allocates the result (2026-11-22).**
     A text, bytea or bpchar parameter is a PostgreSQL varlena:
     a 1-byte or a 4-byte header, and no toast. dg detoasts
     first. A result is a `ty_varlena_t **` that ty allocates
     with the thread's alloc, with a 4-byte header. A function
     with nothing to change may give its input back. The
     old (pointer, length) code is `tyr_*` in `src/text_raw.h`
     and `src/bytea_raw.h`; the varlena wrappers are in the
     same translation unit, so the compiler inlines them.
     Callgrind, against the build before the change: compare,
     `=`, `LIKE`, sort, hash, `position` and `ascii` are
     within 0.25%. A statement that makes a string: `char(n)`
     and `varchar(n)` casts +4.7%; `substr`, `left`, `right`,
     `split_part` +3.95%; `lpad`, `btrim`, `reverse`, `repeat`
     +2.6%; bytea `||`, `reverse`, `overlay` +3.0%; `encode`,
     `decode` +3.0%; `replace`, `translate` +0.9%. The cost is
     40 to 60 instructions for each result: the call through
     the alloc function pointer, `palloc_extended`, and the
     wrapper. The user accepted the cost: a leaner dg alloc
     function, or a result buffer from dg, only moves the
     allocation into dg.
   - **The C-ctype case functions moved (2026-11-21).**
     `ty_ascii_lower`, `ty_ascii_upper` and `ty_ascii_initcap`,
     for `lower`, `upper` and `initcap` in the C ctype. The
     tests: vectors, and `dg/test/select_case_ops.sql`, which
     prints the same as the digres before the move. Measured
     by callgrind (old, new): -34.22%.
   - **The text out of date, time, timestamp, timestamptz and
     interval moved (2026-11-21).** The settings are passed in:
     the date style, the date order and the interval style (a
     third kind of hook of section 8: the host reads the
     setting). The text in stays in digres. The tests: about
     1,600 lines in `known.txt` from the old digres, in 8 date
     styles and 4 interval styles, and
     `dg/test/select_datetime_text.sql`, with time zones, which
     prints the same as before. Measured by callgrind (old,
     new): timestamp text -7.43%; date text -6.73%; interval
     text -52.84%; timestamptz text -7.81%.
   - **The text in of interval moved (2026-11-21).**
     `ty_interval_parse`: the tokenizer, the decoders of the
     postgres, sql and ISO 8601 forms, and the tables of words.
     The code is PostgreSQL's, moved from `datetime.c` with its
     notice into `ty/src/dtparse.cc`; the interval style is
     passed in. The typmod and the messages stay in digres. Not
     yet moved: the tokenizer and the tables are also still in
     `datetime.c`, for the text in of dates, times and
     timestamps, which moves next. The tests: about 1,070
     texts in `known.txt` from the old digres, with the codes
     of the errors, and `dg/test/select_interval_in.sql`.
     Measured by callgrind (old, new): the postgres form
     -14.08%; the ISO 8601 form +0.30%; the clock form +0.21%.
   - **The text in of date, time and timestamp moved
     (2026-11-22).** `ty_date_parse`, `ty_time_parse` and
     `ty_timestamp_parse`, in `ty/src/dtparse.cc` (renamed from
     `interval_in.cc`, as it now holds both). PostgreSQL's
     `DecodeDateTime` and `DecodeTimeOnly` and their helpers
     moved with their notice. The date order is an argument. The
     words of time zones and the clock are the host's hooks,
     `ty_dtzone_t`: a zone abbreviation, a zone name, whether a
     zone has one offset, and the time of the transaction (the
     fourth kind of setting of section 8: the host reads it,
     because a zone database cannot be passed). The offset of a
     zone is not computed: no caller of the three reads it,
     which saves work. The text in of timestamptz and timetz
     stays in digres, and uses the same decoders next. The
     tests: 4,383 known answers from the old digres (three
     types, three date orders), hand tests of the hooks, and
     `dg/test/select_datetime_in.sql`, which prints the same as
     the digres before the move, with the message of each
     error. Measured by callgrind, the cost of the input
     function a statement (old, new), over 100,000 rows of
     text: `date` -4.0%; `timestamp` -3.7%; `time` -15.3%;
     `timestamp` with a numeric zone +0.2%; `timestamp` in the
     word form with a zone abbreviation +1.1%, which is +0.6% of
     the statement. The first version was +10% on the word
     form: libc `tolower` in place of the ASCII one, and a
     wrapper and a translation of the codes on each of the three
     lookups of a word. The totals of a statement carried the
     noise of RocksDB (the table was rewritten between runs), so
     the cost of the function is what is compared.
   - **The typmod of interval, timestamp and time, and
     `make_interval` moved (2026-11-21).**
     `ty_interval_adjust_typmod`, `ty_timestamp_adjust_typmod`,
     `ty_time_adjust_typmod` and `ty_interval_make`. The tests:
     about 1,800 vectors from a Python port, and
     `dg/test/select_typmod_ops.sql`, which prints the same as
     the digres before the move; the cost of the casts is
     +0.24%.
   - **date_trunc, age and date_bin of timestamp, and the ISO
     week functions moved (2026-11-21).** The tests: 3,122
     known answers from the old digres (`known.txt`), and
     `dg/test/select_trunc_ops.sql`, which prints the same as
     the digres before the move. Cost: +0.25% to +0.27%.
   - **timetz: order, hash, interval arithmetic and RANGE
     offsets moved (2026-11-21).** `ty_timetz_cmp`,
     `ty_timetz_hash`, `ty_time_in_range`, `ty_timetz_in_range`;
     `ty_time_add_interval` and `ty_time_sub_interval` are
     inline now. The tests: about 1,500 vectors from a Python
     port, and `dg/test/select_timetz_ops.sql`, which prints
     the same as the digres before the move. timetz cannot be
     stored, hashed or used as a window order in digres yet,
     so those paths have ty tests only. Cost: +0.75% on
     `max` of a timetz sum.
   - **extract and date_part of timestamp, date, time and
     interval moved (2026-11-21).** `ty_timestamp_part`,
     `ty_date_part`, `ty_time_part`, `ty_interval_part`, and
     `ty_datetime_part_unit`. The tests: about 3,700 known
     answers from the old digres (`known.txt`), and
     `dg/test/select_part_ops.sql`, which prints the same as
     the digres before the move. Three differences stay out
     of the tests: the epoch of a timestamp at the end of the
     range is exact now, and the old digres could not make the
     julian numeric of a timestamp past the year 290000. Cost:
     -6% to -15%, because the common unit words skip the
     tables of words; before that fast path it was +5%.
   - **date_trunc of interval moved (2026-11-21).**
     `ty_interval_trunc`. The tests: 688 known answers from the
     old digres, and `dg/test/select_interval_trunc.sql`, which
     prints the same as the digres before the move. Cost:
     +0.28%.
   - **OVERLAPS of timestamp, date, time and timetz moved
     (2026-11-21).** `ty_overlaps_i64` and
     `ty_overlaps_timetz`. The tests: 1,200 vectors from a
     Python port of the SQL rule, and
     `dg/test/select_overlaps_ops.sql`, which prints the same
     as the digres before the move. Cost: -5%.
   - **Conversions of date, timestamp, time and interval
     moved (2026-11-21).** `ty_date_to_timestamp`,
     `ty_timestamp_to_date`, `ty_timestamp_to_time`,
     `ty_datetime_to_timestamp`, `ty_interval_to_time`. The
     tests: about 300 vectors from a Python port, and
     `dg/test/select_dateconv_ops.sql`, which prints the same
     as the digres before the move.
   - **Warning on the callgrind numbers of the typmod, trunc,
     age, bin, interval trunc and conversion moves above.**
     They were taken with `count(*)` over a subquery, and the
     planner drops the unused outputs of a subquery: they did
     not run the functions. Measured again with `max()` over
     the values, the typmod cast was +1.3% and `date_bin`
     +1.3% as functions, out of line; both are inline in the
     header now (typmod +0.14%, bin -7%). `date_trunc` of
     timestamp and interval, `age`, and the cast to interval(p)
     were not remeasured by function; their query totals were
     +1.5% to +2.7% against the old binary, which includes the
     earlier moves of `interval * double` and
     `timestamp + interval`. Measured again on tables, so the
     query holds no earlier-moved operator: `date_trunc` of a
     timestamp -0.1%, `age` -1.6%, the cast to timestamp(p)
     +0.07%, to interval(p) +0.64%, `date_trunc` of an interval
     +0.90% (after its cases fell through in a switch).
   - **After the first pass, "move all": done (2026-11-21).**
     The user asked that every operation of a type move, so
     these followed: the integer compares and the btree compare,
     sort and skip functions; `in_range`; the integer text in
     and out (`numutils.c`); the bool text, order and logic;
     uuid increment and decrement; the bytea compares and
     `encode` and `decode`; and the hash of every type, on
     PostgreSQL's algorithm. What stays in digres is glue: the
     entry points, errors, memory contexts, aggregate states,
     TOAST slicing, and the sort support's abbreviated keys.
   - **"Move all", measured by callgrind** on the final tree,
     instructions a statement over 100,000 rows, old and new
     (the old is the binary before any of this): `int4`
     arithmetic +0.02%; `int8` +0.14%; `int2` +0.20%; bit
     operations, `abs` and `gcd` +0.71%; uuid +0.26%; bool
     -1.12%; bytea -0.63%; a hash join on `int4` +0.37%; a
     group by on `int4` +0.24%, on uuid +0.17%, on bytea
     +0.09%; integer text in and out -0.93%; bytea `encode`
     and `decode` -1.54%. Two were over the limit at first.
     Integer text read +1.90%: the host had to call `strlen`,
     and the reader tested the end at each byte; `ty_intN_parse_z`
     reads to the NUL, and the first loop tests only the digit.
     `encode` and `decode` +2.98%: the name of the encoding was
     compared a byte at a time through a lambda, which cost
     more than the codecs, and the hex digits used branches;
     the name is now matched by its first letter, and the digits
     by a table.
   - **bool: done (2026-11-21).** `ty_bool.h`: `ty_bool_parse`,
     the words of PostgreSQL's boolean input, cut short, in any
     case. digres's `parse_bool_with_len` calls it, and so do
     `boolin` and the GUC parse. The compares, the output (`t`
     and `f`, `true` and `false`), the hash and the aggregate
     states stay in digres.
   - **The hash moved, all of it, with PostgreSQL's algorithm.**
     `ty_hash.h` and `src/hash.cc` hold Bob Jenkins's lookup3
     from digres's `src/common/hashfn.c`, with PostgreSQL's
     notice; digres's four hash functions call it. The hash of
     the integers, bool, uuid, bytea and decimal is built on it.
     No value changed, so no partition moved and no regression
     file changed. The tests: values PostgreSQL printed, taken
     from a digres built before the move, and vectors of a
     Python reference of lookup3. (An earlier draft gave ty a
     hash of its own, and left the integers' in digres; it was
     wrong, and is gone.)
   - **uuid: done (2026-11-21).** `ty_uuid.h`: parse, format,
     compare, hash, version, the time of a version 1 or 7, and
     the making of a version 4 and a version 7. digres's
     `uuid.c` calls them; the sort and skip supports and the
     clock stay. The hash is PostgreSQL's.
   - **Tests of the three, at each layer.** ty: unit tests and
     vectors (above). digres: `dg/test/select_int.sql` and
     `select_bool_uuid.sql` for what a client reads, and a loop
     in `server.sh` for each error text. sm and `rel`: not
     applicable, as sm does not call these yet. mvcc: not
     applicable.
   - **bytea: done (2026-11-21).** `ty_bytea.h`: the two text
     forms, compare, hash, `cat`, `reverse`, the part a
     SUBSTRING names, `overlay`, `position`, `bit_count`, the
     get and set of a byte and a bit, and the integer casts.
     digres's `varlena.c` calls them. Not moved: `encode` and
     `decode`, `string_agg`, the sort support, the compares
     other than `byteacmp`. (Later moved, but `string_agg`
     and the sort support: see "move all" above.)
   - **bytea does not use the allocator of section 3.** Every
     result has a size the caller can tell before the call,
     so the caller gives the buffer and ty allocates nothing.
     digres then writes a result after the 4 bytes of a
     varlena's header with no copy. The allocator stays for a
     result whose size only ty can know. `ty/DESIGN.md`
     section 9 says so.
   - **uuid and bytea, measured by callgrind**, instructions a
     statement over 100,000 rows, old and new: uuid compare,
     text and `<>` 257,963,415 and 258,936,418 (+0.38%); bool
     parse 281,008,832 and 281,223,557 (+0.08%); a bytea query
     of `substring`, `||`, `get_byte`, `position`, `bit_count`,
     `::text` and `reverse` 413,972,236 and 411,606,932
     (-0.57%). The first bytea build read 5.8% more: the hex
     output, `bit_count` and `reverse` worked a byte at a time,
     and `get_byte` and the substring range were calls. They
     now work 8 bytes at a time, use a table of digit pairs,
     and are in line.
6. **Tests at every layer** the change reaches: ty unit tests
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
   `ty_dec128_t`. Settled by the integers: one header for the
   family, `ty_int.h`, with functions `ty_int16_*`,
   `ty_int32_*` and `ty_int64_*` on the C types, and no struct.
6. Does `sm_toast_fetch` change to return a datum from the
   thread's alloc function, or does a new call do it, so that
   `std::string` callers stay as they are?

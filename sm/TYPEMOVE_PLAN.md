# TYPEMOVE_PLAN — types move into sm

Status: draft, nothing built. It settles the interface of an
operation before any code moves. The item is in
[`NEXT.md`](NEXT.md), item 11.

Every fact below was read from the code on 2026-11-09. Each
number marked "grep" is a count, not an audit.

---

## 1. What this is

A type's operations (add, round, `sqrt`, compare, hash) are in
digres today. sm2 keeps a type's storage only
([`sm2/TYPE.md`](sm2/TYPE.md)). The item moves the operations
into sm, for two reasons:

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

Settled on 2026-11-19. Three kinds of result, three answers:

| Result | Example | Memory |
| --- | --- | --- |
| By value | `decimal64 + decimal64`, `int4 * int4` | none: it is the return value |
| Fixed width by reference | `decimal128`, `uuid`, `interval` | the caller's storage: an out-parameter |
| Variable width | `text || text`, `replace()`, `repeat()` | the host's allocator hook, below |

The first two need no design. A variable-width result gets
its memory from an allocator hook that the host installs for
the process. sm has hosts other than digres, so the hook is
general: nothing in it is specific to PostgreSQL.

### 3.1 The hook

```
typedef void* (*sm_alloc_fn)(void* arg, size_t size) noexcept;
typedef void  (*sm_free_fn)(void* arg, void* p) noexcept;

int  sm_set_alloc(sm_alloc_fn alloc, sm_free_fn free, void* arg);
void sm_free(void* p);
```

- `sm_set_alloc` installs the hook for the process, as
  `sm_set_log` installs the log hook.
- The host calls it once, before `sm_attach` and before any
  call that returns a variable-width result. A second call
  fails with `SM_ERR_INVALID`: sm frees its own memory through
  the hook, and a changed hook would free memory that another
  allocator gave.
- A host that does not call it gets `malloc()` and `free()`.
- `sm_free` frees a result through the installed hook, so a
  host can free a result without knowing which allocator gave
  it.

### 3.2 What the hook must do

1. Return memory aligned to at least 8 bytes, or NULL when it
   cannot allocate.
2. Never throw an exception, and never `longjmp`. The types
   are `noexcept`, as `sm_log_fn` is. A host whose allocator
   throws wraps it: the wrapper catches and returns NULL.
3. Be safe to call on every host thread that calls sm.
4. In `free`, accept any pointer that `alloc` returned. sm
   never passes NULL.

Why NULL and not an exception: no exception crosses `sm.h`'s
boundary. An exception from the hook would pass through
functions of sm2 and sm3 that are `noexcept`, and the
catch-all of sm's entry points would hide what it was.

### 3.3 What uses the hook

Only a variable-width datum that sm makes and returns to the
caller:

- the result of a moved type function, such as `text || text`;
- later, a value read through a TOAST pointer. Today
  `sm_toast_fetch` fills a `std::string`, and digres copies it
  into its own memory. With the hook, the copy goes.

Nothing else changes:

- A row or an index entry from a cursor or a pin points into
  engine memory, as `sm.h` says.
- A list, a `std::string` that a call fills, and a handle stay
  as they are.
- sm2's storage functions (`in`, `out`, `key_encode`,
  `tup_encode`) keep `std::string`: their bytes go to storage,
  not to the host.

### 3.4 Who frees what

- **A result that sm returns is the caller's.** The caller
  frees it with its own allocator, or with `sm_free`.
- **sm frees only what it allocated and did not return.** For
  example, to make X bigger, sm frees X and allocates a bigger
  X. There is no `realloc` hook.
- **On an error, sm frees what it allocated** and returns no
  result.

### 3.5 When the hook returns NULL

1. The call frees what it allocated.
2. The call sets `SM_ERR_NOMEM` and returns -1.
3. The host maps the code to its own error.

This is not the policy of [`DESIGN.md`](DESIGN.md), that every
`std::bad_alloc` is fatal. That policy is for sm's own
allocations, which can leave sm's state half changed. A
result's allocation changes no state, so its failure is safe
to report.

### 3.6 sm's own threads

sm's own threads, such as the index-build workers, the
garbage collector and a bulk load's, never call the hook. They
allocate with `malloc()` and `free()`, or with an `arena_t`. A
datum made on one of them never goes to the host.

So inside sm, code that makes a datum takes its allocator as
an argument, `sm_mem_t`: an alloc function, a free function
and an arg. An entry point on a host thread passes the
installed hook. sm's own threads pass `malloc()` and `free()`.
The argument is explicit, so a worker cannot use the hook by
mistake. `sm_mem_t` is a small new type in sm0.

### 3.7 digres's hook

digres's `alloc` calls
`MemoryContextAllocExtended(CurrentMemoryContext, size,
MCXT_ALLOC_NO_OOM | MCXT_ALLOC_HUGE)`, and its `free` calls
`pfree()`. A result goes into the memory context that is
current at the call. `MCXT_ALLOC_NO_OOM` makes it return
NULL, not raise an error. `MCXT_ALLOC_HUGE` permits a datum
up to the 1 GB limit of a varlena. This is one host's hook,
not the design.

### 3.8 The options

| Option | For | Against |
| --- | --- | --- |
| **A. A buffer and a length.** The call returns the length it needs. | Allocates nothing. | The caller may call twice. The work may run twice, or the call must keep the result. |
| **B. An `arena_t`.** | Exists. Frees at once. | Cannot be backed by a host's allocator as built. A host copies each result into its own memory. |
| **C. An allocator passed to each call.** | Ownership is plain. Works on any thread. | An extra argument on each variable call, and the type calls take no handle (§5). |
| **D. An allocator hook per session.** | No extra argument. | A session's setting is state, which the move is to remove. |
| **E. An allocator hook per process**, with C inside sm. | No extra argument at the API. Explicit inside sm, so sm's own threads cannot use the hook. | A process-wide setting; it cannot change once set. |

**Chosen: E.** A is kept for `text` functions whose size is
known before the work. B is kept for sm's own threads. D is
rejected.

## 4. Decision 2: how an error is reported

Moved code cannot `ereport()`. digres must still say
PostgreSQL's SQLSTATE and text, so the 231 regression files
pass unchanged.

1. The operation returns a non-zero `int`.
2. The operation sets sm's error state ([`sm0/README.md`](
   sm0/README.md)) with a kind and a short message.
3. The host maps the kind to its own code and composes its own
   text from the operands it still holds.

Options for the kind:

| Option | For | Against |
| --- | --- | --- |
| **A. New `SM_ERR_` codes** (`SM_ERR_RANGE`, `SM_ERR_DIVZERO`). | One mechanism. | The list is for the storage stack; types add many kinds. |
| **B. One `SM_ERR_TYPE` and a second field, `sm_type_err_t`.** | The list stays short. A type adds a kind without a change to `sm_error.h`. | A second field to read and set. |

**Recommendation: B.** The kinds in §2 are the first list.

## 5. Decision 3: the call itself

Rules from [`NEXT.md`](NEXT.md): a plain C call, no handle, no
session lookup, and a statement's instructions do not move.

- The call is a C-style function in a header of its own, not
  in the 2,103 lines of `sm.h`. Proposed name pattern:
  `sm_dec128_add(const sm_dec128_t*, const sm_dec128_t*,
  sm_dec128_t* result)`.
- sm2's `type_t` table stays for storage. The operations are
  not entries in it.
- digres's wrapper keeps `PG_FUNCTION_ARGS`, the fmgr, the
  aggregate state, the set-returning functions, the support
  nodes, the sort support and the typmod rules. It unpacks the
  datums and calls sm.
- **Cost.** A wrapper adds one call from digres into `libsm.a`.
  Not measured. The measure is in §7, step 1.

## 6. Decision 4: what moves, by type

| Order | Type | Moves | Stays in digres |
| --- | --- | --- | --- |
| 1 | decimal | arithmetic, rounding, compare, hash, `sqrt`, `ln`, `exp`, `log`, power, the 512-bit magnitude | wrappers, aggregates, series, planner support, typmod, error text |
| 2 | integers, bool, uuid, bytea | the same shape: pure functions | wrappers, error text |
| 3 | date, time, interval, float text, text | hold: they read `DateStyle`, `TimeZone`, `extra_float_digits` and collation | until the hooks are settled |

Step 3 is a separate note. Its hooks are the hard part. Code
copied from PostgreSQL keeps its copyright notice
([`../CLAUDE.md`](../CLAUDE.md)).

## 7. Steps

1. **Baseline.** Record instructions a statement for three
   decimal queries with `perf stat`, as
   [`../ISSUE.md`](../ISSUE.md) §2.1 does, and run the
   numeric regression files. → verify: numbers saved, files
   pass.
2. **One function end to end.** Move `decimal64` add with
   decision 1's by-value result and decision 2's error. →
   verify: the regression files pass unchanged, and the
   instruction count is within the limit of the open question
   below.
3. **The rest of decimal's core**, in groups: arithmetic, then
   rounding, then functions of the exponent. → verify: the
   same, after each group.
4. **The allocator hook** (§3): `sm_set_alloc`, `sm_free` and
   sm0's `sm_mem_t`, before the first variable-width type
   moves. Decimal does not need it. → verify: unit tests with
   a counting hook find every allocation freed once, by the
   caller for a result and by sm for the rest; a hook that
   returns NULL gives `SM_ERR_NOMEM` and leaks nothing; a
   second `sm_set_alloc` fails with `SM_ERR_INVALID`; an index
   build on sm's own thread never calls the hook.
5. **Tests at every layer** the change reaches: sm2 unit tests
   for each moved function (new), `rel` if an expression index
   uses one, and pgregress for digres. The mvcc scenarios do
   not apply: nothing here touches concurrency.

## 8. Open questions

1. How large a rise in a statement's instructions is the limit?
   This note proposes 1%.
2. Decision 1: settled, option E (§3).
3. Decision 2: option A or B?
4. The header name and place for the operations.
5. Does `sm_toast_fetch` change to return a datum from the
   hook, or does a new call do it, so that `std::string`
   callers stay as they are?

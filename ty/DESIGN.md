# DESIGN — ty, the operations of the SQL types

ty is a library of pure functions over the SQL types. sm calls
it, and digres calls it. It holds the arithmetic and the rules of
a type, and none of what a host adds: no session, no catalog, no
`palloc()`, no `ereport()`.

- [`README.md`](README.md) says how to build and test it.
- `include/ty.h` is the interface.
- This document says how ty is put together, and why.
- [`../sm/TYPEMOVE_PLAN.md`](../sm/TYPEMOVE_PLAN.md) says what
  moves into ty, and in what order.

Nothing is in ty yet but the error state and the memory
functions. The first type to move is decimal.

---

## 1. Where ty stands

- **ty depends on nothing else in the repository**, as sq does.
  It needs only the C++ standard library and POSIX.
- **sm and digres depend on ty.** sm uses it for the type API.
  digres calls it from its wrappers, which keep
  `PG_FUNCTION_ARGS`, the aggregates and the error text.
- **ty has its own error state and its own error codes.** sm
  maps a ty error to its own code. digres maps it to a SQLSTATE.
- **ty follows [`../sm/CODE.md`](../sm/CODE.md)**, with these
  changes: its names start with `ty_`, not `sm_`, and its errors
  are in `include/ty_error.h`.

## 2. The call

The interface is C in style and compiled as C++, as `sm.h` is.

- A call is a plain function. It takes no handle and looks
  nothing up, so a statement's instruction count does not move.
- A call returns 0 on success and -1 on failure. A call that
  yields a value gives it through its last parameter, set only on
  success.
- A call that cannot fail returns its result, and may be an
  inline function in the header, so that a host's wrapper costs
  no call: `ty_dec64_neg`, `_abs` and `_sign` are. So are the
  equal-scale paths of `add` and `sub`, and the whole of `mul`,
  for both types, each with an out-of-line `_scaled` function
  for two scales and the cold `ty_err_range()` for the error. So
  is the usual case of `ty_dec64_cmp`. A host that packs a
  decimal
  so that its words order as its values do may compare the
  words itself, as digres does: it costs a sort less than
  unpacking both (`../sm/TYPEMOVE_PLAN.md`, step 3).
- The error state is per thread: `ty_errnum()`, `ty_errmsg()`,
  `ty_errname()`. A message is copied into a buffer of the
  thread, so an error can be recorded when memory has run out.
- No exception crosses the interface, in either direction.

## 3. Memory

Three kinds of result, three answers:

| Result | Example | Memory |
| --- | --- | --- |
| By value | `decimal64 + decimal64` | none: it is the return value |
| Fixed width by reference | `decimal128`, `uuid` | the caller's storage: an out-parameter |
| Variable width | `text || text`, `replace()` | the thread's alloc function |

### 3.1 The alloc and free functions

```cpp
typedef void* (*ty_alloc_fn)(size_t size);
typedef void (*ty_free_fn)(void* ptr);

int ty_set_alloc(ty_alloc_fn alloc, ty_free_fn free);
```

These are the rules of every allocator in this repository: the
`sq_allocator_t` of `../sq/include/sq.h` follows them too.

1. `alloc` returns `size` bytes, aligned to at least 8, or NULL
   when it cannot.
2. `free` takes a pointer that `alloc` returned before. ty never
   passes it NULL.
3. Neither function may throw or `longjmp`. A host whose
   allocator throws wraps it: the wrapper catches the exception
   and returns NULL.
4. ty calls them only on the thread that set them. They need no
   thread safety of their own.
5. They have the signatures of `malloc` and `free`: no argument
   for a context.

A function that allocates fails with `TY_ERR_NOMEM` when `alloc`
returns NULL.

### 3.2 A setting belongs to a thread

- `ty_set_alloc` sets the functions of the calling thread, and
  no other thread.
- A thread that sets none has `malloc` and `free`.
- Both arguments NULL restores that default. Only one NULL fails
  with `TY_ERR_INVALID` and changes nothing.
- The host calls it at the start of each of its threads. sm
  calls it at the start of each of its own threads, such as an
  index-build worker, with functions that cut from an arena of
  the thread.
- A thread calls it before its first call that returns a result.
  It does not change the functions while a result of the thread
  is live, as `free` would no longer match `alloc`.

Why per thread, and not per process:

- A host thread and sm's own thread need different memory. A
  digres backend thread takes a result from a memory context. An
  index-build worker, which is not a backend, takes it from an
  arena. A process-wide setting cannot do both.
- No call takes an allocator for an argument, so the type
  calls stay as plain as a call can be.
- The functions have no argument, so an arena-backed `alloc`
  reads its arena from a `thread_local` of its own.

### 3.3 Who frees what

- **A result that ty returns is the caller's.** ty has no call
  to free one. The caller frees a result in the way its alloc
  function allows: one piece with its own free, or all at once,
  as a memory context does.
- **ty frees only what it allocated and did not return.** For
  example, to make X bigger, ty frees X and allocates a bigger
  X. There is no `realloc`.
- **On an error, ty frees what it allocated** and returns no
  result.

## 4. Errors

- ty has its own codes, in `include/ty_error.h`:
  `TY_ERR_INVALID`, `TY_ERR_NOMEM`, `TY_ERR_EXCEPTION`,
  `TY_ERR_RANGE`, `TY_ERR_DIVZERO` and `TY_ERR_UNDERFLOW`
  today. The last is a result of `exp` or a power that is not
  zero, but is zero or below the smallest normal double.
- `exp`, `ln`, `log` and the powers are computed in `double`,
  never `long double`: some 15 digits. `ty_dec128_to_double`
  and `_from_double` convert a value, rounded once.
- A type adds its own kinds when it moves: out of range,
  division by zero, bad text. Each is appended, and no value is
  reused.
- A host maps the kind to its own code, and writes its own text
  from the operands it still holds.

## 5. Copyright

Code copied from PostgreSQL keeps its copyright notice
([`../CLAUDE.md`](../CLAUDE.md)). The decimal code and the
integer code are new code, written for ty, and have none. The
integer code takes the cases and the errors of PostgreSQL's
`int.c` and `int8.c`, and none of their text.

## 6. The integers

`ty_int.h` holds the operations of `int16`, `int32` and
`int64`, as `ty_decimal.h` holds those of the decimals. The
rule is the same: an operation on two values of one width gives
that width, exact or an error, and a caller who wants a wider
result widens an operand first. The mixed widths of SQL
(`int2 + int4`) are a wrapper's job: it passes the narrow
operand to the wider function.

- **Add, subtract, multiply, negate, abs, increment and
  decrement** fail with `TY_ERR_RANGE` on overflow.
- **Divide and remainder** fail with `TY_ERR_DIVZERO` for a zero
  divisor. The quotient cuts toward zero; the remainder has the
  sign of the dividend. `MIN / -1` is `TY_ERR_RANGE`; `x % -1`
  is 0 for every `x`.
- **`gcd` and `lcm`** are never negative. `gcd(MIN, 0)` and
  `gcd(MIN, MIN)` are `TY_ERR_RANGE`, as the answer is -`MIN`.
- **A shift** is by 0 to the width less one. Another count is
  `TY_ERR_RANGE`: C leaves it undefined, and PostgreSQL gave
  what the machine gave. This is a change for `int2`: its
  shifts went through `int`, so a count of 16 to 31 gave 0 or
  the sign.
- **A narrowing** (`int32` to `int16`, and so on) fails with
  `TY_ERR_RANGE` if the value does not fit.
- **A double to `int64`** rounds to the nearest, a tie to the
  even, and fails with `TY_ERR_RANGE` for NaN or a value that
  does not fit.
- **A compare is not here.** C's operators do it, and a wrapper
  uses them.
- **The small functions are in line**: `add`, `sub`, `mul`,
  `div`, `mod`, `neg`, `abs`, the bit operations and the
  shifts. Only `gcd`, `lcm` and the double conversion are out
  of line, in `src/int.cc`.

The tests of the integers are in `test/test_int.cc`, by hand, and in
`test/vectors.txt`, about 25,000 lines of a reference in plain
Python integers for the three widths. Those of bool are in
`test/test_bool.cc` and 87 vectors.

## 7. bool

`ty_bool.h` holds `ty_bool_parse`. It takes the words of
PostgreSQL's boolean input: `true`, `yes`, `on`, `1` and `false`,
`no`, `off`, `0`, any case, and each cut short but `on` and `1`
and `0`, which are whole. `o` fails, as both `on` and `off` begin
with it. It skips no space: a caller that allows one trims first.
A compare of two bools is C's, and a wrapper uses it.

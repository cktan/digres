# DESIGN — ty, the operations of the SQL types

ty is a library of pure functions over the SQL types. sm calls
it, and digres calls it. It holds the arithmetic and the rules of
a type, and none of what a host adds: no session, no catalog, no
`palloc()`, no `ereport()`.

- [`README.md`](README.md) says how to build and test it.
- [`CODE.md`](CODE.md) holds the rules for code in ty, those of
  a simple vector operation among them.
- `include/ty.h` is the interface.
- This document says how ty is put together, and why.
- [`../sm/TYPEMOVE_PLAN.md`](../sm/TYPEMOVE_PLAN.md) says what
  moves into ty, and in what order.

ty holds the operations of the integers, floats, decimals, bool,
uuid, bytea, the string types, `date`, `time`, `timetz`,
`timestamp`, `timestamptz` and `interval`. `money`, `"char"`,
`oid`, arrays and `jsonb` are out of its scope.

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
  `ty_erridx()`, `ty_errname()`. A message is copied into a
  buffer of the thread, so an error can be recorded when memory
  has run out. `ty_erridx()` is the row an operation on vectors
  failed on (§21), and 0 for any other error.
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
- The host calls it at the start of each of its threads. digres
  calls it in `ty_thread_init()`, which the main thread, each
  session thread and each adopted thread run: the functions are
  `palloc_extended()` with `MCXT_ALLOC_NO_OOM` (and NULL past
  `MaxAllocSize`, so that none throws) and `pfree()`, so a result
  is in the memory context of the call. sm calls it at the start
  of each of its own threads (the garbage collector, the reaper
  of databases, the zeroing of the log, the index builders, the
  sealer and the btree reapers). Each makes an `sm0::arena_t` of
  its own, and gives ty a lambda that cuts from it (a piece is a
  multiple of 8 bytes, and `free` does nothing). The thread keeps
  the arena as its handle: a thread that runs jobs in a loop
  calls `arena.reset()` after each job, so that the arena does
  not grow without end, and no result of ty from a job may live
  past it. The arena goes at the thread's end. The timer thread
  of sm0 calls none, and sm0 does not link ty. An index-build
  thread that digres adopts is given the functions of digres
  then. sm's builds link ty (`ty.mk`).
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

### 3.2.1 The clock of the transaction

The start time of a transaction belongs to sm, not to ty and not
to digres. ty reads it for the operations that depend on it:
"now", CURRENT_DATE and the like, and the conversions that need
today's date or the offset a zone has now.

- sm1 keeps the time in the session, as a `timestamptz`
  (`int64_t`, microseconds since 2000-01-01 UTC). A host starts
  the clock with `sm_xact_clock_start` when its own transaction
  begins: sm reads the wall clock, with no sm transaction and no
  snapshot. Without that call, sm1 reads the clock when
  `sm1::begin` takes the first snapshot.
- `ty_set_xact_clock` gives ty a pointer to that value, for the
  calling thread. ty reads through the pointer at each call, so a
  new transaction needs no call to ty. `ty_xact_start` gives the
  time. It fails with `TY_ERR_INVALID` when the thread has no
  clock, or no transaction.
- A thread can have more than one sm session. sm opens one for
  its own work on the thread of its caller. `ty_xact_clock` says
  whose clock ty reads. A session clears the pointer only if it
  is its own. A session whose host never starts the clock takes
  the pointer only when ty has none, or the clock ty has holds no
  transaction.
- digres starts the clock in `StartTransaction`, so a call that
  reads the time, as the text in of 'today' does, has one before
  any call that needs an sm transaction. It keeps the time sm
  gave, so `now()` for a row reads a variable.
- `now()` is the time of BEGIN, as in PostgreSQL. PostgreSQL
  takes the time of the statement that begins the transaction,
  so `now() = statement_timestamp()` is true in a transaction of
  one statement. digres reads the clock again when the
  transaction begins, so there it is false, by some microseconds.
- `ty_set_session_zone` gives ty the session's zone, for the
  calling thread, as a setting of that thread. `ty_xact_now` gives
  the time of the transaction as the fields of that zone, kept for
  the instant and the zone. CURRENT_DATE, CURRENT_TIME and
  LOCALTIME, `time_timetz` and the offset of a zone at the start
  of the transaction (`ty_timetz_at_zone`, `ty_timetz_at_abbrev`)
  read it.

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

### 3.4 The varlena

A value of variable width, text, bytea, `character(n)` and
`character varying(n)`, is a varlena: `include/ty_varlena.h`. It
is PostgreSQL's form, so that digres passes its Datum to ty and
takes ty's result with no copy.

- A varlena has a header, then the bytes. A short header is one
  byte and gives the size of the whole, up to 127. A long header
  is four bytes and gives the size, up to `0x3fffffff`.
- ty reads both forms. A value that ty makes has a long header.
- **ty does not handle toast.** A varlena that is compressed or
  stored outside the row is not an input. The host makes the
  bytes plain first: digres uses `PG_GETARG_TEXT_PP`, which
  detoasts and leaves a short header as it is.
- A function that makes a value calls `ty_vl_new`, which
  allocates with the thread's alloc function. A value past
  `TY_VARLENA_MAX_LEN` is `TY_ERR_RANGE` ("requested length too
  large"), and a NULL from the alloc function is `TY_ERR_NOMEM`.
- The host frees a value as its alloc function says. A function
  that gives its input back, as `replace` does when it finds
  nothing, sets the output to the input: the host must not free
  both.

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
([`../CLAUDE.md`](../CLAUDE.md)). The files that hold it are
`src/hash.cc` and `include/ty_hash.h` (section 10),
`src/dtparse.cc` (section 19), `src/timezone.cc` (section 20),
and `src/unicode.cc`, `src/unicode_category.cc` and
`src/unicode/` (section 25). The
decimal code and the integer code are new code, written for ty,
and have none. The
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
- **Compare**: `ty_intN_cmp` (-1, 0, 1) and `_eq`, `_ne`, `_lt`,
  `_le`, `_gt`, `_ge`, in line. A compare of two widths widens
  the narrower: `ty_int64_lt(a16, b64)`. digres's btree compares,
  sort comparators and skip support (`inc`, `dec`) call them.
- **`ty_int_in_range`** is the test of a window frame of RANGE
  offsets, for every width, done in int64; a negative offset is
  `TY_ERR_INVALID`.
- **Text.** `ty_intN_parse` reads decimal text, or `0x`, `0o` and
  `0b` forms, with a sign, space at both ends, and a single
  underscore between digits. A value that does not fit is
  `TY_ERR_RANGE`, found as soon as the digits pass what the type
  holds, so before a fault further on, as PostgreSQL does; other
  text is `TY_ERR_INVALID`. `ty_intN_format` writes the digits,
  and `ty_uint32_format_n` and `ty_uint64_format_n` the digits of
  an unsigned value. digres's `pg_strtoint*`, `pg_itoa`,
  `pg_ltoa`, `pg_lltoa`, `pg_ultoa_n` and `pg_ulltoa_n` call
  them. `ty_uint64_to_base` writes the digits of `to_bin`,
  `to_oct` and `to_hex`, in line so a constant base folds.
- **The small functions are in line**: `add`, `sub`,
  `mul`, `div`, `mod`, `neg`, `abs`, the bit operations and the
  shifts. Only `gcd`, `lcm` and the double conversion are out of
  line, in `src/int.cc`.

The tests of the integers are in `test/test_int.cc`, by hand, and
in `test/vectors.txt`, about 25,000 lines of a reference in plain
Python integers for the three widths. Those of bool are in
`test/test_bool.cc` and 87 vectors.

## 7. bool

`ty_bool.h` holds `ty_bool_parse`. It takes the words of
PostgreSQL's boolean input: `true`, `yes`, `on`, `1` and `false`,
`no`, `off`, `0`, any case, and each cut short but `on` and `1`
and `0`, which are whole. `o` fails, as both `on` and `off` begin
with it. It skips no space: a caller that allows one trims first.
`ty_bool_parse_text` is SQL's input: it skips the space at both
ends, then parses. `ty_bool_out` is `t` or `f`, and
`ty_bool_text` `true` or `false`. `ty_bool_cmp` and the
predicates put false below true; `ty_bool_and` and `ty_bool_or`
are the logic of the aggregates' state functions;
`ty_bool_from_int32` and `ty_bool_to_int32` are the casts. The
hash is in section 10.

## 8. uuid

`ty_uuid.h` holds a uuid as its 16 bytes, `ty_uuid_t`, and these
functions:

- **`ty_uuid_parse`** reads 32 hexadecimal digits, a hyphen
  allowed after each group of four, in braces if you like, in
  any case. Other text is `TY_ERR_INVALID`.
- **`ty_uuid_format`** writes 8-4-4-4-12 lowercase digits.
- **`ty_uuid_cmp`** compares the bytes, as `memcmp` does.
- **`ty_uuid_hash`** is PostgreSQL's hash of the 16 bytes, 64
  bits with a seed; the low 32 bits with seed 0 are the standard
  hash (section 10).
- **`ty_uuid_increment` and `ty_uuid_decrement`** add or take 1
  as the 128-bit number of the bytes, for the skip support of an
  index; all ones, or all zeros, is `TY_ERR_RANGE`.
- **`ty_uuid_version`** gives the version, or -1 if the variant
  is not RFC 9562's.
- **`ty_uuid_unix_us`** gives the time of a version 1 or version
  7 uuid in microseconds since the Unix epoch. For any other
  uuid it returns 1, which is not an error.
- **`ty_uuid_v4` and `ty_uuid_v7`** make a uuid of random bytes
  from the system (`getrandom`), with the version and the
  variant set. Version 7 takes the time in milliseconds and the
  nanoseconds past it, kept in 12 bits, so uuids of one
  millisecond still order. The clock, and keeping it ascending,
  are the host's.

None of them allocates: a uuid is a value of 16 bytes. The tests
are in `test/test_uuid.cc` and 627 vectors.

## 9. bytea

`ty_bytea.h` holds the operations of a byte string. A bytea is a
varlena (section 3.4): ty reads its header, so the host gives the
value as it has it. A function that makes a bytea allocates it
with the thread's alloc function and gives it in an
out-parameter. The text forms, hex and escape, are C strings: a
pointer and a length.

- **Text.** `ty_bytea_parse` reads the hex form (`\x`, pairs of
  digits, any case, a space, tab, newline or return between
  pairs) and the escaped form (a byte, `\nnn` octal up to 377,
  `\\`). It makes the bytea. Its error is `TY_ERR_INVALID`, and
  its message is PostgreSQL's text for a hex fault.
  `ty_bytea_format_hex` and `ty_bytea_format_escape` write both
  forms to the caller's buffer, after `_hex_size` and
  `_escape_size` give its size; which one a session wants is
  the host's setting (`bytea_output`).
- **Compare and hash.** `ty_bytea_cmp` compares the bytes, and a
  prefix is less. `ty_bytea_hash` is PostgreSQL's hash of the
  bytes, 64 bits with a seed (section 10).
- **Making bytes.** `ty_bytea_cat`, `ty_bytea_reverse`, and
  `ty_bytea_overlay`. `ty_bytea_substr_range` gives the place and
  count that SUBSTRING names and needs no value, so a host that
  fetches part of a stored value fetches that part, and gives
  ty the part.
- **Looking inside.** `ty_bytea_position`, `ty_bytea_bit_count`,
  `ty_bytea_get_byte`, `ty_bytea_get_bit`; and
  `ty_bytea_set_byte` and `ty_bytea_set_bit`, which make a copy
  with the change. An index outside is `TY_ERR_RANGE`, with
  PostgreSQL's message.
- **Integers.** A bytea of at most N bytes is an unsigned number
  of N bytes, which then becomes the signed type, and a longer
  one is `TY_ERR_RANGE`. `ty_intN_to_bytea` is the other way, and
  makes a bytea.

`ty_bytea_eq` and the other predicates are in line with
`ty_bytea_cmp`. `ty_bytea_encoding`, `ty_bytea_encode` and
`ty_bytea_decode` are ENCODE and DECODE for hex, base64 (a line
feed after 76 characters) and escape. `encode` makes a text and
`decode` a bytea, and a fault is `TY_ERR_INVALID` with
PostgreSQL's message.

`ty_bytea_trim_range` is `btrim`, `ltrim` and `rtrim`: it gives
the place and the length of the part left, and the host copies
it, as for the other results of bytea. A set of one byte is
compared to, a set of up to 16 looked through, a longer one put
in a table of 256.

The state of `string_agg` over bytea is text's, in section 16.
The tests are in `test/test_bytea.cc` and
3,522 vectors; `to_bin`, `to_oct`, `to_hex` and the trims in
`test/test_trimbase.cc`, by hand and by 274 lines of
`test/trimbase_known.txt`, which `test/gen_trimbase.py` took from
a digres built before they moved.

## 10. The hash

Every type's hash in ty is PostgreSQL's, bit for bit, so a system
that moves to ty keeps its hash partitions and its hash indexes,
and no regression file changes. The algorithm is Bob Jenkins's
lookup3, as PostgreSQL's `hash_bytes` has it. It moved from
digres's `src/common/hashfn.c` to `src/hash.cc`, with
PostgreSQL's notice; digres's four functions (`hash_bytes`,
`hash_bytes_extended`, `hash_bytes_uint32` and
`hash_bytes_uint32_extended`) now call it.

- **`ty_hash.h`** has the hash of a byte string and of a 32-bit
  value, each standard (32 bits) and extended (64 bits, by a
  seed). With seed 0 the low 32 bits of the extended hash are
  the standard hash.
- **An integer** hashes as PostgreSQL's `hashint8`: the low 32
  bits, with the high 32 bits folded in (the high half itself if
  the value is not negative, its complement if it is), as a
  32-bit value. A value that fits 32 bits then hashes as an
  `int32` does, so an `int16`, an `int32` and an `int64` of one
  value hash alike, and a hash join across the widths works.
  `ty_int16_hash`, `ty_int32_hash` and `ty_int64_hash`.
- **A bool** hashes as the 32-bit 0 or 1.
- **A uuid** and **a bytea** hash as their bytes.
- **A decimal** hashes as digres's did: a key of 32 bytes, the
  16 of the value in its shortest form (no zeros at the end past
  the point; a zero has scale 0), 2 of the scale, and 14 zeros,
  in the machine's order. Equal values then hash alike at any
  scale, and a `decimal64` hashes as the `decimal128` of its
  value.

The tests are `test/test_hash.cc`, which holds values that
PostgreSQL printed for the integers, bool and uuid, from a
digres built before the move; and the vectors of a reference of
lookup3 in Python, `gen_vectors.py`, for every type. The key of
a decimal is the one thing the reference takes from ty's
description, not from PostgreSQL: PostgreSQL has no decimal of
this form.

## 11. The floats

`ty_float.h` holds `float32` and `float64` (SQL's `real` and
`double precision`), as `ty_decimal.h` holds the decimals. The
cases and the errors are PostgreSQL's, from `float.c` and
`utils/float.h`; the code is new.

- **Arithmetic** (`add`, `sub`, `mul`, `div`) fails with
  `TY_ERR_RANGE` for a result that is infinite when no operand
  was, `TY_ERR_UNDERFLOW` for a product or quotient that is zero
  when no operand was, and `TY_ERR_DIVZERO` for a zero divisor
  unless the dividend is NaN. Infinity and NaN that come in go
  through.
- **Order.** All NaNs are equal and above every number, and `0`
  equals `-0`: `cmp`, `eq`, `ne`, `lt`, `le`, `gt`, `ge`,
  `larger` and `smaller`, in line.
- **Conversions.** `float64` to `float32` overflows or underflows
  as above. A float to an integer rounds to the nearest, a tie to
  the even, and is `TY_ERR_RANGE` for NaN or a value that does
  not fit. An integer to a float is C's.
- **Rounding**: `round`, `ceil`, `floor`, `trunc` and `sign`.
- **Powers and logarithms**: `sqrt`, `cbrt`, `exp`, `ln`, `log10`
  and `pow`. `pow` decides the NaN and the infinity cases itself,
  as POSIX says, so that no platform changes them. A domain error
  (the root of a negative, the logarithm of zero or of a
  negative, 0 to a negative power, a negative to a fraction) is
  `TY_ERR_INVALID` with PostgreSQL's message. The host picks the
  SQLSTATE: digres uses the power-function code for `sqrt` and
  `pow` and the logarithm code for `ln` and `log`.
- **`width_bucket`** of a float, in either direction.
- **The hash** is PostgreSQL's: `0` and `-0` hash as the seed,
  every NaN as one NaN, and a `float32` as the `float64` of its
  value.
- **Text.** `ty_floatN_format` takes `extra_float_digits` as an
  argument, not from a session: above 0 it prints the shortest
  text with PostgreSQL's Ryu, else `%.Ng` with `N` the digits of
  the type plus the argument. `ty_floatN_parse` reads what
  `strtod` (`strtof` for `float32`) takes, and the names NaN and
  Infinity, and reports the length of a number that is out of
  range so the host can quote it. This is how ty takes a setting:
  the host passes it (the plan's question of hooks,
  `TYPEMOVE_PLAN` section 8).
- **Ryu.** `src/ryu/` is PostgreSQL's copy of the Ryu printers,
  with its notices, as sm2 has one. Its functions are renamed
  (`ty_rename.h`) so that a host with functions of the old names,
  as digres has, does not capture ty's calls. digres's own copy
  is gone: its `double_to_shortest_decimal_buf` and the others
  call ty.


- **Trigonometry and the like.** `acos`, `asin`, `atan`, `atan2`,
  `cos`, `sin`, `tan` and `cot` in radians; the same in degrees,
  `acosd` to `cotd`, exact at the angles that have exact values
  (`sind(30)` is `0.5`, `tand(45)` is `1`), by PostgreSQL's
  method, scaling by values the library computes once; `sinh`,
  `cosh`, `tanh`, `asinh`, `acosh`, `atanh`, `erf`, `erfc`,
  `gamma` and `lgamma`; and `degrees`, `radians` and `pi`. A NaN
  gives NaN. An argument outside the domain, or an infinite
  angle, is `TY_ERR_INVALID` with the message "input is out of
  range"; digres gives it the out-of-range SQLSTATE.
- **Statistical aggregates.** `ty_float_var_t` and
  `ty_float_regr_t` are the states of `avg`, the variances and
  deviations, and the regression family, kept by the
  Youngs-Cramer method. `accum` adds a value, `combine` adds
  another state, and the final functions return 1 where SQL gives
  NULL. digres keeps its array of doubles as the state, and these
  functions do the arithmetic on it.
- **In line, for speed.** The common path of `sqrt`, `cbrt`,
  `exp`, `ln`, `log10`, `pow`, `sin`, `cos`, `tan`, `asin`,
  `acos` and `atan`, and the accumulate step of the aggregates,
  are in `ty_float.h`, so a wrapper in a host costs no call.
  Anything unusual goes to an out-of-line `_slow` function, which
  decides it. Even so, a query of nothing but ten math functions
  reads 1.4% more instructions than before the move; see the
  plan.

The tests are `test/test_float.cc`, by hand; the vectors of a
Python reference (`gen_vectors.py`), which use the libm of the
machine that made them, so a subnormal result of `exp` or `pow`
is left out, as `glibc`'s `errno` for it is not a rule; and
`test/float_text.txt`, 1,230 texts that digres printed before the
move (`test/gen_float_text.py`). No libm rounds sinh, cosh, tanh,
their inverses, erf, erfc, gamma or lgamma correctly, so another
machine may give the next float: the test takes one float apart
as the same for those, and needs the same bits for the rest.

`ty_float64_in_range`, in line, is the test of a window frame of
RANGE offsets, for float8, and for float4 widened. digres has no
such frame yet, and reaches it by a call of `in_range` only.

## 12. HyperLogLog and abbreviated keys

`ty_hll.h` estimates the number of distinct values in a set from
a 32-bit hash of each, in 2^4 to 2^16 registers of a byte. A sort
uses it to decide when to stop its abbreviated keys, and a hash
aggregate to size its spill partitions. ty's code is new. Its
estimates are those of PostgreSQL's
`src/backend/lib/hyperloglog.c`, bit for bit, so a sort ends its
abbreviated keys at the row PostgreSQL's would:

- the register is the first `width` bits of the hash, and its
  value the most leading zeros of the rest, plus one;
- the estimate sums 2^-r over the registers in their order, and
  takes the paper's corrections for a small and a large number,
  with PostgreSQL's constants and in PostgreSQL's order of
  operations.

`ty_hll_add` is in line, as a sort calls it for each row. The
registers are the alloc function's. digres's `hyperloglog.c`
keeps PostgreSQL's names and errors, and calls ty.

`ty_abbrev.h` holds the abbreviated keys of a sort, which are
built on it. A key is the first 8 bytes of a value as an unsigned
integer in the machine's order, so a sort compares two integers
and reads the values only on a tie. There are two kinds:

- **uuid**: the first 8 of its 16 bytes. The state counts the
  values and estimates their distinct keys, until there are more
  than 100,000.
- **bytes in the C collation**: text, varchar, bpchar without its
  trailing spaces, and bytea. The key is the first 8 bytes, and 0
  after a shorter value. The state estimates both the distinct
  keys and the distinct values, the value by its first 128 bytes
  and its length. A host whose keys come from `strxfrm`, as
  digres's ICU collations do, makes the key and gives it to
  `ty_abbrev_bytes_count`.

Each kind has a rule, which the sort calls each time its rows
double: `ty_abbrev_uuid_abort` and `ty_abbrev_bytes_abort`. A
rule gives what it decided (wait, go on, end the count, abort)
and the numbers it judged by, so the host logs them as
PostgreSQL does (`trace_sort`). The keys, the hashes and the
rules are PostgreSQL's, so a sort aborts at the row PostgreSQL's
does. digres keeps the `SortSupport` wiring, the comparators of
the keys, and the `strxfrm` path of the collations that are not
C.

The tests: `test/test_hll.cc` compares 45 estimates, of 5 widths
and of 0 to 1,000,000 values, with those that PostgreSQL's file,
built alone, printed for the same hashes, and checks the
correction for a large number and the widths by error.
`test/test_abbrev.cc` checks that keys order as their bytes, and
each rule's decisions, with numbers PostgreSQL logged for the same
values. digres's `server.sh` checks that a sort logs the
estimates of the digres before the move (`trace_sort`), and
`dg/test/select_sort_abbrev.sql` that sorts by the keys, aborted
or not, in the C collation and in ICU's, give the same order.

## 16. text

`ty_text.h` holds the operations of text: UTF-8 strings ordered
by their bytes, the C collation. digres has one encoding, UTF8; a
collation that is not C, or not deterministic, stays in digres,
which calls ty only for the C collation. The input is valid
UTF-8: the host checks it when the text comes in. A function
never reads past its string, whatever the bytes.

A text, and each `character(n)` and `character varying(n)`, is a
varlena (section 3.4). A function that makes a text allocates it
with the thread's alloc function and gives it in an
out-parameter, with a long header; `substr`, `left`, `right`,
`split_part` and the trims copy the part they cut. A function
that may have nothing to change (`replace`, `translate`,
`character(n)`) may give its input as its result, and says so. A
position or a count is in characters.

Inside, each function reads the bytes and the length of its
inputs, and calls a function of `src/text.cc` that works on a
pointer and a length (`src/text_raw.h`). That layer is not part
of the interface. The comparisons and the hash, with `a || b`,
are in line in `ty_text.h`. The `_bytes` forms
(`ty_text_cmp_bytes`, `ty_text_eq_bytes`, `ty_text_length_bytes`,
`ty_bpchar_truelen_bytes`, `ty_text_like_bytes`) are for a host's
own strings that are not varlenas, such as a collation's sort
key; `ty_ascii_lower`, `_upper` and `_initcap` work on a C
string.

The functions are `cmp`, `eq`, `starts_with` and the hash;
`length`; `substr`, `left`, `right`, `reverse` and `overlay`;
`position`, `replace` and `split_part`, which use the search of
PostgreSQL (a skip table of a size that fits the length of the
text); `repeat`, `lpad`, `rpad`, the trims and `translate`; and
`ascii` and `chr`. `replace` counts the copies first, so that the
result is made in one allocation. The limits are PostgreSQL's: a
result may not pass `MaxAllocSize`, and `lpad` and `rpad` apply
the limit to four bytes a character, as PostgreSQL does.

`character(n)`, `character varying(n)` and `name` use the same
functions: `ty_bpchar_fit` cuts to n characters or pads to n with
spaces (a character cut off that is not a space is an error,
unless the cast is explicit), the order and the hash ignore
trailing spaces, and `ty_name_from_text` and `ty_name_parse` cut
a name to 63 bytes at a character boundary (a name is not a
varlena). The typmod text, the error text and the binary forms
stay in digres.

LIKE is `ty_text_like`, for text (a character is a whole UTF-8
character) and for bytea (a byte). It keeps PostgreSQL's three
values, a match, no match here and no match at any start, so that
the error for a backslash at the end of the pattern comes only if
the match gets to it, as in PostgreSQL. `ty_text_like_escape`
makes the pattern of an `ESCAPE` clause. The pattern with `%`
nests one call for each `%`, and a nesting of 5,000 is an error,
as the stack limit is in PostgreSQL. `ILIKE` still lowers both
strings in digres, then calls ty.

`lower`, `upper` and `initcap` for the C ctype are
`ty_ascii_lower`, `ty_ascii_upper` and `ty_ascii_initcap`: only
ASCII letters change. The other ctypes (the builtin Unicode
tables, ICU) depend on the collation and stay in digres.

`ty_text_cmp` is also the order of the pattern operators, `~<~`
and the rest, which compare the bytes whatever the collation is.
`ty_quote_literal` is `quote_literal`; `ty_quote_literal_bytes`
is its form for a C string. `quote_ident` needs the keywords and
a setting, and stays in digres.

`format` is a loop the host drives, as only the host has the
arguments and their text:

1. `ty_format_begin` takes the format string.
2. `ty_format_next` adds the text up to the next conversion to
   the result, and gives the conversion: its letter (`s`, `I` or
   `L`), the argument's place, the width or the width's place,
   and the `-` flag.
3. The host takes the arguments, makes their text and quotes it
   (`quote_identifier` needs the keywords and a setting), then
   calls `ty_format_put`, which pads the text to the width in
   characters.
4. `ty_format_finish` gives the result, a text.

So the errors come in PostgreSQL's order: an error of the format
string comes when the parse reaches it, after an error of an
argument before it. ty keeps the result in one buffer of the
thread's alloc function, made larger as it fills, and copies the
text of the format a byte at a time, as its pieces are short; an
argument with no width is added in line. It checks the
limit of PostgreSQL's `StringInfo` as PostgreSQL's appends would,
and gives the host the bytes for the detail, before it allocates
anything. A width near 1 GB fails at once, where PostgreSQL first
fills 1 GB with spaces.

`string_agg`, over text and over bytea, keeps its state in ty
(`ty_string_agg_t`): the bytes of the values, each after its
delimiter. The first value's delimiter stays in the state, as in
PostgreSQL, so that the parts of a parallel aggregate join up;
`ty_string_agg_finish` cuts it off and copies the rest, and
leaves the state as it was, for a window. The combine, serialize
and deserialize functions are ty's too, with PostgreSQL's bytes
for a serialized state. The state's bytes are the alloc
function's, so digres makes the aggregate's memory context
current around each call that adds. An add of a state that has
room is in line.

The regular expressions and the functions on arrays of text stay
in digres.

The tests: the vectors, from a reference of Python strings, whose
characters are code points as UTF-8's are (about 51,000); the
hand tests in `test/test_text.cc`, `test/test_format.cc` and
`test/test_string_agg.cc`; and `dg/test/select_text_ops.sql`,
`select_format_ops.sql` and `select_string_agg.sql`, which print
the same as the digres before the move.



## 18. interval

`ty_interval.h` holds the arithmetic of interval: a span of
months, days and microseconds, in PostgreSQL's layout (`int64`
time, `int32` day, `int32` month). The months, the days and the
microseconds are kept apart, as a month has no fixed number of
days and a day no fixed number of hours.

The functions are the order (the span in 128 bits, with a month
of 30 days and a day of 24 hours, so that it cannot overflow),
the sign, the hash, `-`, `+` and `-` of two values, `*` and `/`
by a float (the fraction of a month goes to days, and the
fraction of a day to microseconds, as PostgreSQL does), and
`justify_hours`, `justify_days` and `justify_interval`. Minus
infinity and plus infinity are the values with the smallest and
the largest value in all three fields; the cases of an operand
that is infinite are PostgreSQL's.

The text in is `ty_interval_parse`: the tokenizer, the decoder of
the postgres and sql forms (the words `ago`, `@`, the units and
their abbreviations, signs, fractions), and the decoder of the
ISO 8601 form, with the interval style passed in. The code is
PostgreSQL's (`datetime.c`), moved with its notice into
`src/dtparse.cc`, and is checked against what the old digres
did with about 1,070 texts; the codes of the errors are
PostgreSQL's, and the host words them. The text out is in section
19; `extract`, `date_trunc` and `age`, and and the text of a
typmod, are still in digres. The typmod itself is
`ty_interval_adjust_typmod`: the fields to the right of the range
are zeroed, and the microseconds are rounded to the precision.
`make_interval` is `ty_interval_make`.

The tests: the vectors, from Python integers for the order and
the sum, and from a port of the steps of PostgreSQL's code in
Python floats for the product, the quotient and the justify
functions (about 4,300); `test/test_interval.cc`; and
`dg/test/select_interval_ops.sql` and `select_interval_in.sql`,
which print the same as the digres before the move.



## 19. date, time and timestamp

`ty_datetime.h` holds the calendar and the arithmetic of date,
time and timestamp, without a time zone and without text. A date
is an `int32` count of days since 2000-01-01, a time an `int64`
count of microseconds, and a timestamp an `int64` count of
microseconds since 2000-01-01; the smallest and the largest value
of a date and of a timestamp are the infinities.

The functions are the Julian day of a date and back (`date2j`,
`j2date`, `j2day`); `make_date` and `make_time`, with their
checks and PostgreSQL's messages; `date + n`, `date - n` and
`date - date`; `time + interval`, `time - interval` and `time -
time`; a timestamp as year, month, day, hour, minute, second and
microseconds, and back; `timestamp + interval` and `- interval`
(the months first, with the day cut to the last day of the month,
then the days, then the microseconds); and `timestamp -
timestamp`, which justifies the hours into days.

The text out of date, time, timestamp, timestamptz and interval
is here too (`ty_date_format`, `ty_time_format`,
`ty_timestamp_format` and `ty_interval_format`), by the settings
the host passes in: the date style, the date order and the
interval style, with PostgreSQL's values. For a timestamptz the
host finds the broken-down time in its zone and the name of the
zone, and passes them. The special values, infinity, are written
by the host. The text in of timestamptz and timetz, `AT TIME
ZONE` and the casts that need a zone are ty's too, given the zone
(section 20); digres keeps the lookup of a zone by its name.

`ty_date_cmp`, `ty_time_cmp` and `ty_timestamp_cmp` order two
values as their integers, in line: the infinities of a date and a
timestamp are the least and the greatest integer, so they order
first and last. digres's compares, `larger` and `smaller` of the
three, and `timestamp_cmp_internal`, call them.

The text in of date, time and timestamp is `ty_date_parse`,
`ty_time_parse` and `ty_timestamp_parse` (`src/dtparse.cc`, with
the text in of interval). They are PostgreSQL's `DecodeDateTime`
and `DecodeTimeOnly`, moved with their notice. The date order is
an argument. The words that name a zone or read the clock are the
host's, through `ty_dtzone_t`, a table of four hooks that the
call passes: is a word a zone abbreviation, is it the name of a
zone, does a zone have one offset, and what is the time of the
transaction. A hook may be NULL, for a host without one: then no
zone word and no clock word is known. The hooks are the third
kind of setting of the plan, where the host reads it, as a zone
database is not a number that can be passed. ty does not compute
the offset of a zone: no caller of these three reads it. A bad
text gives -1 and `TY_ERR_INVALID`, with PostgreSQL's code in
`*dterr` and the zone name for the message in `*extra`; a value
out of range gives `TY_ERR_RANGE` and a code of 0. digres words
both. The typmod stays with `ty_time_adjust_typmod` and
`ty_timestamp_adjust_typmod`.

The text in of timestamptz and timetz is `ty_timestamptz_parse` and
`ty_timetz_parse`, from the same decoders. They need two more
hooks, for the offsets: `offset` gives the offset of a zone at a
local time (the session's zone if the zone is NULL; a time with no
date has `mday` 0, and means today's date in the session's zone),
and `abbrev_offset` the offset of a dynamic abbreviation. A host
that leaves them NULL gets the zone of the text, and UTC if there
is none. The three types with no zone leave them NULL, so they do
not pay for an offset nobody reads. The tests of these two: 5,748
known answers (the zones with a fixed offset), hand tests of the
hooks, and `dg/test/select_datetimetz_in.sql`, which runs on the
zones of the database.

The tests: 4,383 known answers from the old digres, for the three
types in the three date orders (`gen_known.py --dtin`), with a
host of a few abbreviations and zones; hand tests of the hooks
(the clock, a dynamic abbreviation, an unknown zone, a hook that
fails, no hooks); and `dg/test/select_datetime_in.sql`, which
prints the same as the digres before the move, with the message
of each error.

`ty_timestamp_adjust_typmod` and `ty_time_adjust_typmod` round a
timestamp and a time to the precision of `timestamp(p)` and
`time(p)`.

`ty_timestamp_trunc` is `date_trunc`, `ty_timestamp_bin` is
`date_bin`, and `ty_timestamp_age` is `age` of two timestamps.
The ISO week functions are `ty_date2isoweek`, `ty_date2isoyear`,
`ty_date2isoyearday`, `ty_isoweek2j` and `ty_isoweek2date`.
`ty_datetime_unit` tells if a word is a unit, and gives its code.

`ty_timetz_t` is time with time zone. `ty_timetz_cmp` orders by
the time in UTC, then by the zone. `ty_timetz_hash` xors the hash
of the time with the hash of the zone. `ty_time_in_range` and
`ty_timetz_in_range` are the RANGE offsets of a window frame; the
timetz one compares in 128 bits, because a sum may be far from a
time of day.

`ty_timestamp_part`, `ty_date_part`, `ty_time_part`,
`ty_timetz_part` and `ty_interval_part` are `extract` and
`date_part`. A `ty_part_t`
holds the exact value, as an integer, a scaled integer, or a sum,
and the float8 that `date_part` gives. digres makes the numeric
from it. `ty_datetime_part_unit` looks up a word: the words that
extract is nearly always asked with are found without the tables.
An epoch of a timestamp at the end of the range is exact now: the
old numeric division rounded it to three digits. The infinities
and the numeric are still in digres.

`ty_interval_trunc` is `date_trunc` of an interval: the fields
are cut at the unit, as for a timestamp. A week is not a unit of
an interval.

`ty_overlaps_i64` and `ty_overlaps_timetz` are the SQL OVERLAPS
operator. A null end is a null pointer. The answer is true, false
or null, as the SQL standard says, which gives a result that is
not null for some inputs with nulls.

`ty_date_to_timestamp`, `ty_timestamp_to_date`,
`ty_timestamp_to_time`, `ty_datetime_to_timestamp` and
`ty_interval_to_time` are the conversions between date, time,
timestamp and interval; section 20 has those that need a zone.
`ty_timestamptz_from_epoch` is `to_timestamp` of a number, in
line. `ty_timestamp_bin` serves `date_bin` of timestamptz too,
whose arithmetic is the same. `ty_zone_offset_parse` reads a zone
written as a number, such as `+05:30`.

The tests: the vectors, from Python integers and a second
formulation of the Julian day (the Wikipedia formula and
Richards' inverse, not PostgreSQL's), about 3,500;
`test/test_datetime.cc`; and `dg/test/select_datetime_ops.sql`,
which prints the same as the digres before the move.


## 20. Time zones

`ty_timezone.h` holds the rules of the IANA time zone database,
and the data of the zones. A zone is found by its name in any case,
by a POSIX spec (`EST5EDT`, `UTC+3`), or by an offset, as
PostgreSQL's `pg_tzset` and `pg_tzset_offset` do. The pointer it
gives stays good for the life of the process, and the calls are
safe for many threads: a lookup takes a lock, so a host that asks
often keeps the pointer.

The code is PostgreSQL's, with its notices, in `src/timezone.cc`:
the IANA library of `localtime.c` (the zone file reader, the POSIX
rule parser, UTC to local time, the next DST boundary, the
meaning of an abbreviation), `pg_tzset` of `pgtz.c`, and
`DetermineTimeZoneOffset` of `datetime.c`. Three things differ
from PostgreSQL's:

1. A zone file is not read from the disk. The data is in
   `src/tzdata.cc`, as C data, and `ty_tz_find` reads a zone from
   it. [`TZDATA.md`](TZDATA.md) says how the file is made, and how
   to make it again when PostgreSQL has newer data.
2. The zones found are kept for the process, in one table, not for
   a thread.
3. No setting is read: the session's zone is the host's, who
   passes the zone, or NULL where a call has one.

The functions: `ty_tz_localtime` (a UTC instant in a zone: the
fields, the offset, the abbreviation); `ty_tz_offset_for_local` (the
offset of a local time, with the rule for a time a transition
skips and for one it repeats); `ty_tz_abbrev_offset`,
`ty_tz_abbrev_at` and `ty_tz_abbrev_is_known` (what a zone means
by an abbreviation); `ty_tz_fixed_offset`; and the data's release
and names. A host with a file of zones may give ty a pack to use
in place of its own, by `ty_tz_use_data`; ty checks it first.

The operations of timestamptz that need a zone are in ty too, each
taking the zone: `ty_timestamptz_to_local` (the fields of an instant
in a zone, the offset and the abbreviation: `timestamp2tm` with a
zone), `ty_timestamp_to_timestamptz` and `ty_timestamptz_to_timestamp`
(the casts, and the full-zone branch of `AT TIME ZONE`),
`ty_date_to_timestamptz` and `ty_timestamptz_to_date`. The infinities
pass through, an overflow is an error or, if the caller asks, an
infinity and a flag, and the messages are PostgreSQL's.

The arithmetic and the parts of a timestamptz are there as well:
`ty_timestamptz_add_interval` and `_sub_interval` (the months, then the
days, each time finding the offset of the new local time, so that a day
is a day of the clock across a change of daylight time; then the
microseconds), `ty_timestamptz_trunc`, `ty_timestamptz_part` (the units
of the local time, the zone's own, and the epoch, which is the instant)
and `ty_timestamptz_age`. Each works on the fields once, as the code of
PostgreSQL does: `ty_tm_trunc`, `ty_tm_part` and `ty_age_of_fields`
are the cores of the timestamp versions, which take the fields, so that
no call turns the fields into a timestamp and back.

The text in of timestamptz and timetz asks the host for the offset
of a zone (section 19). The host may now answer from these calls.

digres uses these zones and no other: its own copy of the IANA
library (`src/timezone/localtime.c`) is gone. A `pg_tz` of digres is
a `ty_tz_t`, and `pg_tzset`, `pg_localtime`, `pg_tz_acceptable` and
the others of `pgtime.h` are calls into ty, with the names and the
shapes of PostgreSQL's: `ty_tz_localtime_pg` gives a `pg_tm` (the
month from 0, the year from 1900) in storage of the thread, and
`ty_tz_next_abbrev` lists the abbreviations of a zone.
`DetermineTimeZoneOffset` and its relatives in `datetime.c` call
`ty_tz_offset_for_local`, `ty_tz_abbrev_offset` and
`ty_tz_abbrev_at`. A lookup by name keeps the last answer of the
thread, so the zone of a repeated name takes no lock.

The tests: `test/tz_known.txt`, 16,618 lines that Python's
`zoneinfo` gave for the same zone files, for every distinct zone
(341), at fixed instants and local times and around the
transitions of the zone, with the rule of PostgreSQL for a skipped
and a repeated time; and hand tests of the lookup, the POSIX
specs, the abbreviations, the bad packs, and a process that is
given a pack of the host.

## 21. Vectors

`ty_vec.h` holds the operations on vectors: one call on the n
values of a column at once, for a columnar executor
([`../REPORT.md`](../REPORT.md) §9). The integers' add, sub,
mul, div and mod, and the flag and bool operations are built.

- **A vector is an array of n values and an array of n flags**,
  a byte a row. Bit 0, `TY_VEC_NOTVALID`, is 1 for a row that is
  not valid, such as a row a filter dropped; bit 1,
  `TY_VEC_NULL`, is 1 for a null. The other bits are 0 for now.
  One flag array serves both inputs: a row is null if one of its
  inputs is. A caller ORs the flags of two inputs into it before
  the call.
- **The call**: `tyvec_intN_add(n, flag, a, b, out)`, and the
  same for `_sub`, `_mul`, `_div` and `_mod`. `out` gets the
  results. `flag` is the flags of both inputs, and then of the
  results: an operation writes the results' flags over the
  inputs'. These leave them as they are, so a null input gives a
  null result.
- **A row not valid, or null, is never computed into an error.**
  Its result means nothing.
- **A failing row fails the call at once**: `TY_ERR_RANGE` if it
  overflows, or `TY_ERR_DIVZERO` if it divides by 0.
  `ty_erridx()` is the index of the lowest row computed that
  fails: the row PostgreSQL, going row by row, would fail on.
- **They run on vector instructions** (`src/vec.cc`): GCC's
  vector types of 16 bytes, which the compiler turns into NEON on
  aarch64 and SSE on x86-64. A loop computes with wrap-around and
  gathers the failures of the rows computed, with no branch
  a row. It tests them once a block of 256 rows, and only then
  looks for the row, one at a time.
- **An overflow's bits**: a sum's sign differs from both
  operands' signs. A difference's operands differ in sign, and
  its sign is not `a`'s. A product of 16 or 32 bits is made in
  twice the width, and its high half is not all the sign of its
  low half.
- **A division fails for a divisor of 0, and for MIN / -1**,
  found by two compares. Such a row is divided by 1, computed or
  not, as the machine may trap on it. NEON and SSE do not divide
  integers: 16 bits are divided as floats, and 32 bits as
  doubles, and the quotient is cut to zero. That is exact: one
  that is no integer is 1 / |b| or more from the next integer,
  and the rounding error is less. 64 bits are divided one row at
  a time.
- **A remainder fails for a divisor of 0.** It is the dividend
  less the quotient times the divisor. A divisor of 0 or -1 is
  taken as 1, as the machine may trap on it, and any `a % -1` is
  0.
- **Two multiplications are not plain vector types.** int32's
  takes NEON's intrinsics on aarch64, as GCC 13 makes scalar ones
  of the vector types'. int64's is one row at a time: neither
  NEON nor SSE multiplies 64 bits by 64 bits.
- **A flag is widened to its row's lane by interleaving** the
  flags' vector with itself. GCC makes a move a row of
  `__builtin_convertvector` from bytes.
- **An operation that cannot fail may compute every row**, valid
  or not, as that is cheaper.
- **`IS NULL` is `tyvec_flag_isnull(n, flag, out)`**: `out[i]`
  is 1 if row i is null, or else 0, for every row, so `out` is a
  vector of bool. Its result is never null: it sets each
  `TY_VEC_NULL` to 0, and leaves the other bits. A caller that
  needs the input's nulls afterwards, as in
  `WHERE x IS NULL OR x > 3`, copies the flags first.
- **`IS NOT NULL` is `tyvec_flag_isnotnull`**, with the same
  parameters and the same change to `flag`.
- **`IS TRUE` and `IS FALSE` are `tyvec_bool_istrue` and
  `tyvec_bool_isfalse`**, `(n, flag, vec, out)`, of the bools of
  `vec`. A null row is neither, and `flag` changes in the same
  way.
- **`NOT` is `tyvec_bool_not(n, flag, vec, out)`**. `NOT` of a
  null is null, so `flag` stays as it is.
- **`AND` and `OR` are `tyvec_bool_and` and `tyvec_bool_or`**,
  `(n, flag, a, b, out)`. `flag` stays as it is, so a row with a
  null input is null. That is not SQL's answer for
  `false AND null`, which is false, and for `true OR null`, which
  is true: one flag does not say which input is null.
- **`out` of `AND` and `OR` may be `a`, `b` or both.**
- **`out` of another bool operation may be `vec`.** `flag` may
  not overlap an input or `out`.
- **`out` of add, sub, mul, div and mod may be `a` or `b`.** When
  a block fails, its results are already over that input. Add and
  sub take the input back from the result and the other input.
  The others cannot, so they save each block of that input first,
  which costs a copy of 256 rows. Add does not allow `a`, `b` and
  `out` to be one array, as it has no other input then. Sub does,
  as `a - a` never overflows, and so do the others.

The tests are `test/test_vec.cc`: each row against the scalar
`ty_intN_add`, `_sub`, `_mul`, `_div` and `_mod`, at sizes around
the vector's lanes and the loop's blocks; the lowest failing row
among several, one past the last whole vector, and one in a row
not valid and one in a null row, which are no error; and the
results written over an input, with and without an overflow. The
flag and bool operations are tested on every value of a flag. Div
and mod are tested around multiples of the divisor, where a
float's rounding would show.

## 22. to_char and its reverse

`ty_tochar.h` writes `to_char` of a timestamp, a timestamptz
and an interval by a format: keywords (`YYYY`, `MM`, `HH24`,
`Month`, `TZ` and the others), a prefix `FM` or `TM` before one
and a suffix `TH`, `th` or `SP` after it, and text, quoted or
not. The code is new. The keywords, the output and the errors
are those of PostgreSQL's `formatting.c`, byte for byte.

1. `ty_dch_compile` reads the format once into nodes, as
   PostgreSQL's `parse_format` does: the first keyword that
   matches wins, so `HH24` is read before `HH`.
2. `ty_timestamp_to_char`, `ty_timestamptz_to_char` and
   `ty_interval_to_char` take a value's fields and write each
   node.

A timestamptz takes its zone, and ty finds the zone's
abbreviation and offset (`ty_tz_t`). `TM` takes the localized
names from the host, through a function in a
`ty_tochar_locale_t`: the host has the names of its `lc_time`
and cases them under its collation. With no host locale, `TM`
writes the C locale's English names. The result is a text of
room for 12 bytes a byte of format, as PostgreSQL allots, and
no field writes more.

digres compiles a call site's format once and keeps it in the
call's `fn_extra`, with the format's text, so a format that is
the same for each row is read once. It forgets the old format
before it compiles a new one, so a format that fails leaves no
pointer to freed memory.

The numbers' `to_char` is in the same header: `ty_num_compile`
reads a format of `9`, `0`, the point and `D`, the comma and
`G`, the signs (`S`, `MI`, `PL`, `SG`, `PR`), `L`, `B`, `FM`,
`V`, `TH`, `RN` and `EEEE`, and checks it with PostgreSQL's
errors. ty makes the digits of an int4, an int8, a float4 and a
float8, and writes them by the format, as PostgreSQL's
`NUM_processor` does. A float's digits are those of PostgreSQL's
`snprintf`, whose NaN is `NaN` where libc's is `nan`. D, G, L and
S take the host's `lconv` symbols, through a function in a
`ty_num_locale_t` that is called only for a format that uses one.
digres makes a decimal's digits, and gives them to
`ty_num_to_char_digits`.

The reverse parse of a date format is `ty_dch_parse`: the text
read field by field, as PostgreSQL's `DCH_from_char`, then the
fields made a date and time, as its `do_to_timestamp`. It serves
`to_timestamp`, `to_date` and jsonpath's `datetime()`, whose
standard mode `ty_dch_compile` and `ty_dch_parse` take as `std`.
`ty_dch_to_timestamp` and `ty_dch_to_date` finish the first two.
The host gives two functions, in a `ty_dch_parse_host_t`: the
localized names that TM matches, folded by its collation, and
the zone abbreviations of its session's table. An error comes
with PostgreSQL's code, detail and hint (`ty_dch_error_t`), or
as `DateTimeParseError`'s code, whose message the host writes
from the text. digres keeps its cache of date formats, which
now holds ty's compiled formats, as jsonpath calls the parse with
no FmgrInfo to keep one in.

`to_number` is `ty_num_from_char`: the text read by a number
format, as PostgreSQL's `NUM_processor` reads it, into the text
`numeric_in` reads, with the precision and the scale of the
result. digres makes the decimal of it, and divides by V's power
of 10. `formatting.c` now holds only digres's glue: the caches of
formats, the host's functions, and the decimal steps.

The tests: `test/tochar_known.txt`, 18,484 lines that the digres
before the move printed (`test/gen_tochar.py`), of timestamps,
timestamptz in 11 zones and intervals, each keyword with each
prefix and suffix, and mixed formats; `test/tonum_known.txt`,
7,973 lines of int4, int8, float4, float8 and numeric by 99
number formats (`test/gen_tonum.py`); `test/fromchar_known.txt`,
8,736 texts by formats through `to_timestamp` and `to_date`, of
which 1,433 are errors, each with its message, detail and hint
(`test/gen_fromchar.py`); `test/test_tochar.cc` for
the errors, the host's names and symbols, and the edges; and
`dg/test/select_tochar_ops.sql`, `select_tonum_ops.sql`,
`select_fromchar_ops.sql` and `select_tonumber_ops.sql`, with a
German locale, zone abbreviations, jsonpath's standard mode, and
PostgreSQL's own `to_number` cases.

## 23. Regular expressions

ty has no regular expression engine of its own: a host gives
one, for the whole process, through `ty_set_regex` in
`ty_regex.h`, and a caller of ty matches `~` and `~*` by
`ty_text_regex`. ty passes the bytes of the pattern and the
text, the flags and the host's collation id, and gives back
the answer, or the engine's message as `TY_ERR_INVALID`; with
no engine set, `~` is an error. The pointer is an atomic, so a
host may set it while other threads read it.

digres sets PostgreSQL's engine (`regexp.c`), so a match through
ty is digres's own, with its collations, its character classes
and its cache of compiled patterns. Its engine catches an error
and gives back its message: ty and its callers report errors by
their return codes, and digres's `ereport`, a C++ exception,
would cross them unseen, in the middle of their work. A cancel
during a match comes back as such an error. digres's own `~` calls its engine directly,
not through ty. The engine stays out of ty because it is Henry
Spencer's library, 13,335 lines, whose character classes are
the collation's: a rewrite could not match it, and a copy is
not wanted.

The tests: `test/test_regex.cc`, with an engine of its own, for
the arguments passed, the answers, an engine's error and no
engine. Nothing in digres calls `ty_text_regex` yet; when the
hook went in, a build whose `~` and `~*` went through it passed
all 231 pgregress files and printed digres's own errors.

## 24. The text of a decimal

`ty_decimal.h` holds a decimal's text, as digres's `numeric_in`
and `numeric_out` had it; the code is digres's own, moved.
`ty_dec128_parse` reads a number with a sign, spaces, a point, an
exponent and single underscores between digits, or an integer in
base 16, 8 or 2 after `0x`, `0o` or `0b`. A typmod rounds it half
away from zero and checks the digits before the point. A value of
more than 36 significant digits is refused, not rounded. The
reason of a refusal is `*why`, one of `TY_DEC_IN_*`, so the host
words PostgreSQL's message and detail: syntax, NaN or infinity,
range, 36 digits, or the typmod's field. `ty_dec128_format`
writes every place of the scale and no exponent.

The binary form stays digres's, but its digits go through ty:
`ty_dec_digits_t` holds up to 154 of them, and `_drop`, `_pad`,
`_typmod` and `_pack` make the value, as the text does. A
`decimal64` is read and written as the `decimal128` of its value.
`ty_dec128_min_scale` and `ty_dec128_trim_scale` are inline. The
float casts go by the text, as PostgreSQL's do:
`ty_dec128_to_float64` and `_to_float32` read a decimal's text by
`strtod` and `strtof`, and `ty_dec128_from_float` reads a float's
`"%.*g"` text, 15 digits for a float8 and 6 for a float4.

The tests: `test/test_decin.cc`, by hand, and by
`test/decin_known.txt`, 6,668 lines that `test/gen_decin.py`
took from a digres built before the move: each text under 10
typmods or none, with its error's kind, the scale functions and
the float casts both ways. In digres a bare `numeric` is a
`decimal64`, so the vectors with no typmod refuse what a
`decimal64` cannot hold.

## 25. Unicode

`ty_unicode.h` holds `normalize` (`ty_text_normalize`), the test
`IS NORMALIZED` (`ty_text_is_normalized`), `unicode_assigned`
(`ty_text_is_assigned`) and `unistr` (`ty_text_unistr`), and the
properties of a character (`ty_u_category`, `ty_u_isalpha` and
the rest). `ty_norm_chars` is the normal form of an array of code
points: digres's SASLprep calls it for a password. The regular
expressions' engine and the builtin collation provider of digres
call the properties, through wrappers in line
(`common/unicode_category.h`).

The normal forms, the properties and their tables are
PostgreSQL's, with its notices, in `src/unicode.cc`,
`src/unicode_category.cc` and `src/unicode/`. There is one copy:
digres has none. `dg/src/common/unicode/README` says how to make
the tables for a new version of Unicode. The tables of letter
case stay in digres: only its collation provider reads them.

An array of code points is the thread's alloc function's, and is
freed before the call returns, but for that of `ty_norm_chars`.
`ty_text_unistr` gives the reason of a refusal in `*why`
(`TY_UNISTR_*`), as the host's SQLSTATE depends on it.

The tests: `test/test_unicode.cc`, by hand, and by
`test/unicode_known.txt`, 3,592 lines that `test/gen_unicode.py`
took from a digres built before the move: the four forms of every
code point, by a hash of each 4,096, how many of them are
normalized and assigned, 508 texts of marks and starters, and 200
texts of escapes. A program of one use compared the properties of
every code point with the old functions.

## 26. The scale of a decimal's result

A quotient, a root, a logarithm, an exponential and a power are
not exact, so PostgreSQL's numeric gives each a scale by rules of
its own, in base 10,000: at least 16 significant digits, and not
below the arguments' scales. `src/decscale.cc` holds the rules.
`ty_dec128_div_scale`, `_sqrt_scale`, `_exp_scale`, `_ln_scale`,
`_log_scale` and `_var_scale` give the scale, and the caller
passes it to the function that computes; sm may pass another.
`ty_dec128_power` is the power whole, with its errors.
`ty_dec128_typmod` is the assignment to `numeric(p, s)`,
`ty_dec128_to_int64`, in line, the casts to the integers, and
`ty_dec128_digits` and `ty_dec128_scale_up` the parts a host
needs. The aggregates' states stay in digres.

The tests: `test/test_decscale.cc`, by hand, and by
`test/decscale_known.txt`, 5,333 lines that
`test/gen_decscale.py` took from a digres built before the move.
A variance is checked by its count of places: the sum of squares
is the host's. `test/test_rest.cc` and `test/rest_known.txt`
(`test/gen_rest.py`) do the same for `date_bin`, `in_range`,
`to_timestamp`, the pattern operators, `quote_literal` and
`extract` of a timetz.

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
([`../CLAUDE.md`](../CLAUDE.md)). The one file ty copies is
`src/hash.cc` and `include/ty_hash.h`, from PostgreSQL's
`src/common/hashfn.c` (section 10); they carry the notice. The
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
  them. - **The small functions are in line**: `add`, `sub`,
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

`ty_bytea.h` holds the operations of a byte string, on a pointer
and a length. It does not allocate. Where a result has bytes,
the caller gives a buffer, and the caller can tell its size
before the call: from `ty_bytea_hex_size`,
`ty_bytea_escape_size`, `ty_bytea_overlay_size`, or from the
length of the inputs. A host then writes a result where it
keeps its own: digres writes after the 4 bytes of a varlena's
header, with no copy. This differs from the plan's use of
`ty_set_alloc` for results (`../sm/TYPEMOVE_PLAN.md` section 3)
on purpose: that is for a result whose size only ty can know,
and none of bytea's is.

- **Text.** `ty_bytea_parse` reads the hex form (`\x`, pairs of
  digits, any case, a space, tab, newline or return between
  pairs) and the escaped form (a byte, `\nnn` octal up to 377,
  `\\`). Its error is `TY_ERR_INVALID`, and its message is
  PostgreSQL's text for a hex fault. `ty_bytea_format_hex` and
  `ty_bytea_format_escape` write both forms; which one a
  session wants is the host's setting (`bytea_output`).
- **Compare and hash.** `ty_bytea_cmp` compares the bytes, and a
  prefix is less. `ty_bytea_hash` is PostgreSQL's hash of the
  bytes, 64 bits with a seed (section 10).
- **Making bytes.** `ty_bytea_cat`, `ty_bytea_reverse`, and
  `ty_bytea_overlay`. `ty_bytea_substr_range` gives the place and
  count that SUBSTRING names and needs no length, so a host that
  fetches part of a stored value fetches that part.
- **Looking inside.** `ty_bytea_position`, `ty_bytea_bit_count`,
  `ty_bytea_get_byte`, `ty_bytea_get_bit`; and
  `ty_bytea_set_byte` and `ty_bytea_set_bit`, which change bytes
  the caller copied. An index outside is `TY_ERR_RANGE`, with
  PostgreSQL's message.
- **Integers.** A bytea of at most N bytes is an unsigned number
  of N bytes, which then becomes the signed type, and a longer
  one is `TY_ERR_RANGE`. `ty_intN_to_bytea` is the other way.

`ty_bytea_eq` and the other predicates are in line with
`ty_bytea_cmp`. `ty_bytea_encoding`, `ty_bytea_encode_size`,
`ty_bytea_encode`, `ty_bytea_decode_size` and `ty_bytea_decode`
are ENCODE and DECODE for hex, base64 (a line feed after 76
characters) and escape; the buffers are the caller's, as above,
and a fault is `TY_ERR_INVALID` with PostgreSQL's message.

Not here: the `string_agg` state, which is an aggregate's memory
and stays in digres. The tests are in `test/test_bytea.cc` and
3,522 vectors.

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

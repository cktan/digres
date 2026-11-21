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
Python reference (`gen_vectors.py`), which use the same libm as
ty, so a subnormal result of `exp` or `pow` is left out, as
`glibc`'s `errno` for it is not a rule; and
`test/float_text.txt`, 1,230 texts that digres printed before the
move (`test/gen_float_text.py`).

## 12. macaddr and macaddr8

`ty_mac.h` holds `ty_macaddr_t` (6 bytes) and `ty_macaddr8_t` (8
bytes), from PostgreSQL's `mac.c` and `mac8.c`; the code is new.
`parse` reads the forms PostgreSQL reads: for a macaddr seven
forms, each as `scanf`'s `%x` reads them (so a field may have
fewer than two digits, or a sign, and a field past 255 is
`TY_ERR_RANGE`); for a macaddr8 six or eight pairs of hex digits
with one spacer, which a six-byte form widens with `FF:FE`.
`format`, `cmp` (the order of the bytes), `not`, `and`, `or`,
`trunc`, `set7bit` and the two conversions are as PostgreSQL's.
The hash is that of the bytes.

The text forms have too many odd cases to rewrite as a reference,
so `test/known.txt` holds what a digres built before the move
printed for a few hundred texts, good and bad
(`test/gen_known.py` made it); `test/test_mac.cc` covers the
rest. digres's storage does not hold these types yet, so they
have no key in a table.

## 13. bit and bit varying

`ty_bits.h` holds the operations of bit strings, from
PostgreSQL's `varbit.c`; the code is new. A bit string is its
bytes and its length in bits; the first bit is the high bit of
the first byte, and the bits of the last byte past the length are
zero, in every result, so that `memcmp` orders strings. As with
bytea, a result goes to the caller's buffer; the sizes are known
before the call.

`parse` reads binary digits (with `b` or without) or hex digits
after `x`; `format` writes `0` and `1`; `cmp` is the order of the
bytes then the lengths; `cat`, `substr`, `overlay`, `position`,
`count`, `and`, `or`, `xor`, `not`, the shifts (a negative count
shifts the other way), `get` and `set` of a bit, `from_int32` and
`from_int64` (two's complement, sign-extended or cut at the
left), `to_int32` and `to_int64`, and `resize`, which pads or
cuts for `bit(n)` and `bit varying(n)`. The length rules of the
types, which of the two errors a cast gives, are the host's.

The vectors are a reference of Python strings of `0` and `1`,
which are the plainest way to say what each operation means; the
hand tests are in `test/test_bits.cc`.

## 14. inet and cidr

`ty_inet.h` holds `ty_inet_t`: the family (2 for IPv4, 3 for
IPv6), the mask length and 16 bytes of address. It has the layout
PostgreSQL stores, so a stored value is a `ty_inet_t` after its
varlena header.

The text parse and format are the ISC code PostgreSQL uses:
`src/inet_pton.cc`, `inet_ntop.cc` and `inet_cidr_ntop.cc`, moved
from digres with their copyright notices. They read the short
forms (`10/8`, `10.1.2/24`) and write the IPv6 forms with `::`.
The other operations are new code, from the cases of `network.c`:
order, the five containment tests, `host`, `network`,
`broadcast`, `netmask`, `hostmask`, `merge`, `~`, `&`, `|`, `+`
and `-` with an integer, `-` of two values, the cidr check for a
bit right of the mask, and the hash.

A cidr with a bit right of its mask is `TY_ERR_RANGE`; any other
bad text is `TY_ERR_INVALID`. The caller words the message, as
`inet_in` and `cidr_in` do.

The tests: the text is `test/known.txt`, what the old digres
printed for about 450 inputs, each as an inet and as a cidr; the
other operations are in `test/vectors.txt`, from a reference of
Python integers; the hand tests are in `test/test_inet.cc`.

## 15. tid, pg_lsn and "char"

`ty_small.h` holds three types of a few operations each.

A tid is `ty_tid_t`, PostgreSQL's `ItemPointerData`: the block
number as two 16-bit halves, high first, then the offset. The
functions are the text in and out, the order (the block, then the
offset), and the hash of the six bytes. `ty_tid_parse` keeps the
rule of `tidin` that the block may be a negative number that
wraps.

A pg_lsn is a `uint64_t`. The functions are the text in and out
(`X/Y`, one to eight hex digits each side; written in upper
case). The compares are the native ones of the integer. The sum
and the difference with a number go through `numeric`, so they
stay in digres.

A "char" is an `int8_t`. The functions are the text in and out (a
backslash and three octal digits is a byte; a byte of 0x80 or
more is written that way), the order, which is of the byte as
unsigned, and `ty_char_from_int32`. The cast to int4 is of the
byte as signed.

The text of all three is in `test/known.txt`, what the old digres
printed for about 100 inputs each. The hand tests are in
`test/test_small.cc`.

## 16. text

`ty_text.h` holds the operations of text: UTF-8 strings ordered
by their bytes, the C collation. digres has one encoding, UTF8; a
collation that is not C, or not deterministic, stays in digres,
which calls ty only for the C collation. The input is valid
UTF-8: the host checks it when the text comes in. A function
never reads past its string, whatever the bytes.

A string is a pointer and a length. A function that makes a
string writes it to the caller's buffer, and says in `ty_text.h`
how large the buffer must be; a function that cuts a string, such
as `substr`, gives the place of the part in the input, so that no
copy is needed. A position or a count is in characters.

The functions are `cmp`, `eq`, `starts_with` and the hash;
`length`; `substr`, `left`, `right`, `reverse` and `overlay`;
`position`, `replace` and `split_part`, which use the search of
PostgreSQL (a skip table of a size that fits the length of the
text); `repeat`, `lpad`, `rpad`, the trims and `translate`; and
`ascii` and `chr`. `replace` counts the copies first, so that the
result is made in one buffer. The limits are PostgreSQL's: a
result may not pass `MaxAllocSize`, and `lpad` and `rpad` apply
the limit to four bytes a character, as PostgreSQL does.

`character(n)`, `character varying(n)` and `name` use the same
functions: `ty_bpchar_fit` cuts to n characters or pads to n with
spaces (a character cut off that is not a space is an error,
unless the cast is explicit), the order and the hash ignore
trailing spaces, and `ty_name_parse` cuts a name to 63 bytes at a
character boundary. The typmod text, the error text and the
binary forms stay in digres.

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

The regular expressions, `format` and the functions on arrays of
text.

The tests: the vectors, from a reference of Python strings, whose
characters are code points as UTF-8's are (about 51,000); the
hand tests in `test/test_text.cc`; and
`dg/test/select_text_ops.sql`, which prints the same as the
digres before the move.



## 17. money

`ty_money.h` holds the operations of money: an `int64` count of
the smallest unit. The text depends on the locale
(`lc_monetary`). The host gives it as a `ty_money_fmt_t`, a copy
of the fields of C's `struct lconv` that PostgreSQL reads, so
that ty reads no setting itself; `ty_money_fmt_c` is the C
locale. The settings are the second kind of hook of
[`../sm/TYPEMOVE_PLAN.md`](../sm/TYPEMOVE_PLAN.md): a value
passed in, as `extra_float_digits` is for the floats.

The functions are `parse` and `format` (the signs and the symbols
in the places `sign_posn`, `cs_precedes` and `sep_by_space` say),
`+` and `-`, `*` and `/` with an integer or a float, `/` of two
values and `cash_words`. The float arithmetic is that of
`ty_float.h`. The casts with `numeric` stay in digres, as they
use it.

The tests: the arithmetic and the words are in the vectors, from
Python integers; the text in six made-up locales is a port in
Python of PostgreSQL's code (about 250 values each); the text in
the C locale is in `test/known.txt`, from the old digres; the
hand tests are in `test/test_money.cc`. The words of the smallest
money value were wrong in the old digres (a signed overflow,
which the compiler used): ty gives "Minus ninety two quadrillion
...".

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
`src/interval_in.cc`, and is checked against what the old digres
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
by the host. The text in is the host's, and reads `DateStyle` and
the time zone (`DecodeDateTime`), `extract`, `date_trunc`, `age`,
the casts, the typmods and everything with a time zone stay in
digres until those settings are passed in, as the locale of money
is.

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
time of day. The text of timetz and its typmod are still in
digres.

`ty_timestamp_part`, `ty_date_part`, `ty_time_part` and
`ty_interval_part` are `extract` and `date_part`. A `ty_part_t`
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

The tests: the vectors, from Python integers and a second
formulation of the Julian day (the Wikipedia formula and
Richards' inverse, not PostgreSQL's), about 3,500;
`test/test_datetime.cc`; and `dg/test/select_datetime_ops.sql`,
which prints the same as the digres before the move.



## 20. point and box

`ty_geo.h` holds the operations of the geometric types `point`
and `box`, and the fuzzy compares that all the geometric types
use. A float is the same as another if they are within `EPSILON`
(1.0e-6) of each other, and below another only if it is below by
more than `EPSILON`. The structs have PostgreSQL's layout.

The functions for a point are the compares (the same, left,
right, above, below, vertical, horizontal), the distance (a
hypotenuse that cannot overflow unless its result does), the
slopes, and the sum, difference, product and quotient, as complex
numbers. For a box they are the construction from two points, the
order of the corners, the same and overlap and contain, the
position operators, the width, the height, the area, the center,
the intersection, the distance of the centers, `+`, `-`, `*` and
`/` with a point, and the order by area. The float arithmetic is
that of `ty_float.h`, so an overflow, an underflow and a division
by zero come back as ty's errors.

The text, the binary forms, and the types `lseg`, `line`, `path`,
`polygon` and `circle`, with the functions between types, stay in
digres: they call these functions for the points and boxes they
hold.

The tests: the vectors (about 13,500), from the rules of the
fuzzy compares and of PostgreSQL's float arithmetic in Python
floats, with NaN, infinities, ties at `EPSILON` and values near
overflow; `dg/test/select_geo_ops.sql`, which prints the same as
the digres before the move.



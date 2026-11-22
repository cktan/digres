# ty

The operations of the SQL types: add, round, compare, hash,
`sqrt` and the rest. It is a library, `build/libty.a`, with its
interface in `include/ty.h`. sm calls it, and digres calls it.

```
make -j4         build build/libty.a and the test binary
make test        build, and run the tests in test/
make tsan        the tests under the thread sanitizer
make clean       remove build/
make format      clang-format src/ and test/
```

It needs a C++20 compiler and nothing else. Nothing ty is built
from is outside this directory.

[`DESIGN.md`](DESIGN.md) says how it is put together and why.
The plan to move the types into it is
[`../sm/TYPEMOVE_PLAN.md`](../sm/TYPEMOVE_PLAN.md).

## Using it

```cpp
#include "ty.h"

// On a thread that is to get results in its own memory:
// 1. Give ty the functions that allocate and free there.
ty_set_alloc(my_alloc, my_free);

// 2. Call the operations. On -1, read the thread's error.
if (/* a ty call */ 0 != 0) {
    fprintf(stderr, "%s: %s\n", ty_errname(ty_errnum()), ty_errmsg());
}
```

A thread that calls `ty_set_alloc` for nothing gets `malloc` and
`free`.

## Layout

| Path | What it holds |
| --- | --- |
| `include/ty.h` | the interface |
| `include/ty_decimal.h` | the decimal operations |
| `include/ty_int.h` | the integer operations |
| `include/ty_bool.h` | the boolean parse |
| `include/ty_uuid.h` | the uuid operations |
| `include/ty_varlena.h` | the varlena: text, bytea and character values, as PostgreSQL has them |
| `include/ty_bytea.h` | the bytea operations, on varlenas |
| `include/ty_float.h` | the float4 and float8 operations, with their text |
| `include/ty_hash.h` | the hash of every type, PostgreSQL's |
| `include/ty_text.h` | the text operations, UTF-8, on varlenas |
| `include/ty_interval.h` | the arithmetic of interval |
| `include/ty_datetime.h` | the calendar, date, time and timestamp arithmetic |
| `include/ty_error.h` | the error codes |
| `src/` | the library; `text_raw.h` and `bytea_raw.h` are the operations on a pointer and a length that `text_vl.cc` and `bytea_vl.cc` call |
| `test/` | the unit tests, in doctest, and the vectors |
| `test/gen_vectors.py` | a reference of plain Python integers that writes `test/vectors.txt` |
| `test/vectors.txt` | about 152,000 vectors of the operations of every type, with the result or the error kind |
| `test/known.txt` | about 10,200 texts, with what the old digres printed for them |

`ty/` follows [`../sm/CODE.md`](../sm/CODE.md).

## Tests

`make test` runs two kinds, in one binary (`build/test_ty`):

- **Unit tests** (`test/test_*.cc`), by hand: the allocator, the error
  state, and the cases of each operation that a reader needs to see.
- **The vectors** (`test/test_vectors.cc`): the operations of every
  type against `test/vectors.txt` and `test/known.txt`. A reference
  in plain Python integers, `test/gen_vectors.py`, follows the rules in
  [`DESIGN.md`](DESIGN.md) and writes them: edges at the ends of both
  ranges, scales that do and do not align in 128 bits, and random
  values; `exp`, `ln`, `log` and the powers are checked to 16 places
  against Python's `decimal`. ty is not compared with itself, nor with
  digres.

The vectors are reproducible: the seed is fixed. To add a case or an
operation, change the generator, then run
`test/gen_vectors.py > test/vectors.txt`, and `make test`.
It found a bug that no hand-written test had: a quotient that rounds
to nothing, rounded up when its divisor passed 128 bits.

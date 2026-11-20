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
| `include/ty_error.h` | the error codes |
| `src/` | the library |
| `test/` | the unit tests, in doctest |

`ty/` follows [`../sm/CODE.md`](../sm/CODE.md).

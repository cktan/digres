# Coding in ty

ty follows [`../sm/CODE.md`](../sm/CODE.md). This file adds what
is particular to ty. [`DESIGN.md`](DESIGN.md) says how ty is put
together, and why.

## 1. Simple vector operations

A vector operation is one call on the n values of a column
(`include/ty_vec.h`, [`DESIGN.md`](DESIGN.md#21-vectors) §21).
A *simple* one has a SIMD form: the same few instructions on
each row of a vector, such as add, a compare or `IS NULL`.

**These rules are for simple operations only.** They do not
apply to a complicated one, whose work differs from row to row,
such as a search for a string in each row (`strstr`). The rules
for those are not set.

A row is *computed* if its flag has neither `TY_VEC_NOTVALID`
nor `TY_VEC_NULL`.

### 1.1 The call

- **The name is `tyvec_<type>_<operation>`**, such as
  `tyvec_int32_add` and `tyvec_bool_not`. A float's type is
  `fp32` or `fp64`, as in the scalar `ty_fp32_add`. The type is
  `flag` if the operation reads only the flags:
  `tyvec_flag_isnull`. The prefix keeps it apart from the scalar
  `ty_int32_add`.
- **The return is `int`**: 0, or -1 with the thread's error. An
  operation that cannot fail also returns `int`, always 0, so
  that all operations have one shape.
- **`flag` is one in/out parameter, `uint8_t *flag`, with no
  `const`.** It holds the flags of the inputs, and after the
  call those of the results. Do not add a second flag array: a
  caller ORs the flags of two inputs before the call.
- **Keep the parameters few.** The order is `n`, `flag`, the
  inputs, `out`. What describes an error goes in the thread's
  error state, not in a parameter: the failing row is
  `ty_erridx()`.
- **A bool result is one byte a row**: 1 for true, 0 for false.
- **Say in `ty_vec.h` which arrays may be the same one.**

### 1.2 The result

- **Each computed row gets what the scalar operation gives.**
- **The result's flags are right for `out`.** An operation whose
  result is never null, such as `IS NULL`, sets each
  `TY_VEC_NULL` to 0. A caller that needs the inputs' flags
  afterwards copies them first.
- **A row not valid has an `out` that means nothing**, and it
  keeps its `TY_VEC_NOTVALID`. Do not spend instructions on it.
- **The other bits of a flag stay as they are.**
- **An operation that cannot fail computes every row**, valid or
  not, when that is cheaper. It usually is.
- **An operation that can fail raises no error for a row not
  computed.** It fails with the lowest computed row that does
  not fit, also when `out` is over an input.

### 1.3 The loop

- **Use explicit SIMD whenever possible**: GCC's vector types of
  16 bytes (`__attribute__((vector_size(16)))`), which become
  NEON on aarch64 and SSE on x86-64. Do not leave it to the
  compiler to vectorize a plain loop: GCC may do so at `-O3` and
  not at `-O2`.
- **Look at the code GCC makes** (`objdump -d`): it can make
  scalar code of a vector type's operation, such as a
  `__builtin_convertvector` from bytes, or a multiplication of
  64-bit lanes. Use a shuffle or the machine's intrinsics there,
  with the vector types as the fallback.
- **Load and store with `memcpy`**: an array may not be aligned
  to 16 bytes.
- **No branch a row.** Gather the errors of a block of rows, test
  them once a block, and only then look for the row.
- **The rows past the last whole vector go one at a time**, by
  the same rule.

### 1.4 The tests

In `test/test_vec.cc`:

- Compare each computed row with the scalar operation.
- Use sizes around the lanes of a vector and the block.
- Cover a row not valid and a null row, and each error at the
  first and the last row of a block and past the last whole
  vector.
- Test the results written over an input, with an error and
  without, if the operation allows it.

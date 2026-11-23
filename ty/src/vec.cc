/*
 * Copyright (c) 2026 CK Tan
 * SPDX-License-Identifier: MIT
 */

/*
 * vec.cc -- the vector operations of ty_vec.h. A loop works on 16 bytes
 * at a time, in GCC's vector types, which the compiler turns into the
 * machine's vector instructions: NEON on aarch64, SSE on x86-64.
 */
#include "ty_vec.h"

#include <string.h>

namespace {

/* Rows in a block: the loop tests for an overflow once a block. */
constexpr uint32_t block_rows = 256;

/*
 * The vector add of one width: T the type, U its unsigned type, which
 * adds with wrap-around, as C defines for it.
 * 1. Copy the flags: a row's result is valid if its inputs are, and null
 *    if one of them is.
 * 2. For each block of rows, 16 bytes at a time: add a and b with
 *    wrap-around, store the sum, and gather into ovf the overflow bits of
 *    the rows computed, neither not valid nor null. A sum overflowed if its
 * sign differs from both operands' signs: the top bit of (a ^ r) & (b ^ r).
 * 3. At the block's end, if a row computed overflowed, find the lowest
 *    one, row by row, and fail with its index.
 * 4. The rows past the last 16 bytes, one at a time, by the same rule.
 */
template <typename T, typename U>
int add_vec(uint32_t n, const uint8_t *inflag, const T *a, const T *b, T *out,
            uint8_t *outflag, uint32_t *bad) {
  // L lanes of U in 16 bytes, and L flags of a row each.
  constexpr uint32_t L = 16 / sizeof(T);
  typedef U vec_t __attribute__((vector_size(16)));
  typedef uint8_t flags_t __attribute__((vector_size(L)));
  constexpr U top = U(1) << (8 * sizeof(T) - 1);
  // The overflow of row i: its sign bit, and whether it is computed.
  auto overflowed = [&](uint32_t i) {
    U r = U(a[i]) + U(b[i]);
    return 0 == (inflag[i] & TY_VEC_SKIP) &&
           0 != ((U(a[i]) ^ r) & (U(b[i]) ^ r) & top);
  };
  // The lowest valid row of [from, to) that overflows: fail with it.
  auto fail_in = [&](uint32_t from, uint32_t to) {
    for (uint32_t i = from; i < to; i++) {
      if (overflowed(i)) {
        *bad = i;
        break;
      }
    }
    return ty_err_range();
  };
  // 1. The flags.
  if (outflag != inflag) {
    memmove(outflag, inflag, n);
  }
  // 2. and 3. Whole blocks of whole vectors.
  uint32_t i = 0;
  const uint32_t whole = n - n % L;
  while (i < whole) {
    const uint32_t from = i;
    const uint32_t to = whole - i < block_rows ? whole : i + block_rows;
    vec_t ovf = {};
    for (; i < to; i += L) {
      vec_t va, vb;
      flags_t f;
      memcpy(&va, a + i, 16);
      memcpy(&vb, b + i, 16);
      memcpy(&f, inflag + i, L);
      vec_t vr = va + vb;
      memcpy(out + i, &vr, 16);
      // All ones in the lane of a row computed, zero in another.
      vec_t computed =
          (vec_t)((__builtin_convertvector(f, vec_t) & TY_VEC_SKIP) == 0);
      ovf |= (va ^ vr) & (vb ^ vr) & computed;
    }
    U any = 0;
    for (uint32_t l = 0; l < L; l++) {
      any |= ovf[l];
    }
    if (TY_UNLIKELY(any & top)) {
      return fail_in(from, to);
    }
  }
  // 4. The rest, one row at a time.
  for (; i < n; i++) {
    if (TY_UNLIKELY(overflowed(i))) {
      *bad = i;
      return ty_err_range();
    }
    out[i] = T(U(a[i]) + U(b[i]));
  }
  return 0;
}

} // namespace

/* out = a + b of each row computed; see ty_vec.h. */
int ty_int16_add_vec(uint32_t n, const uint8_t *inflag, const int16_t *a,
                     const int16_t *b, int16_t *out, uint8_t *outflag,
                     uint32_t *bad) {
  return add_vec<int16_t, uint16_t>(n, inflag, a, b, out, outflag, bad);
}

/* out = a + b of each row computed; see ty_vec.h. */
int ty_int32_add_vec(uint32_t n, const uint8_t *inflag, const int32_t *a,
                     const int32_t *b, int32_t *out, uint8_t *outflag,
                     uint32_t *bad) {
  return add_vec<int32_t, uint32_t>(n, inflag, a, b, out, outflag, bad);
}

/* out = a + b of each row computed; see ty_vec.h. */
int ty_int64_add_vec(uint32_t n, const uint8_t *inflag, const int64_t *a,
                     const int64_t *b, int64_t *out, uint8_t *outflag,
                     uint32_t *bad) {
  return add_vec<int64_t, uint64_t>(n, inflag, a, b, out, outflag, bad);
}

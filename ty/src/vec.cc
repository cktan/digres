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

#include "error.h"

namespace {

/* Rows in a block: the loop tests for an overflow once a block. */
constexpr uint32_t block_rows = 256;

/*
 * The vector add of one width: T the type, U its unsigned type, which
 * adds with wrap-around, as C defines for it.
 * 1. For each block of rows, 16 bytes at a time: add a and b with
 *    wrap-around, store the sum, and gather into ovf the overflow bits of
 *    the rows computed, neither not valid nor null. A sum overflowed if its
 * sign differs from both operands' signs: the top bit of (a ^ r) & (b ^ r).
 * 2. At the block's end, if a row computed overflowed, find the lowest
 *    one, row by row, and fail with its index. If out is a or b, the
 *    block's sums are over that input: it is the sum less the other one,
 *    exactly, as the sum wrapped around.
 * 3. The rows past the last 16 bytes, one at a time, by the same rule.
 * The flags stay as they are: a row's result is valid if its inputs are,
 * and null if one of them is.
 */
template <typename T, typename U>
int add_vec(uint32_t n, uint8_t *flag, const T *a, const T *b, T *out) {
  // L lanes of U in 16 bytes, and L flags of a row each.
  constexpr uint32_t L = 16 / sizeof(T);
  typedef U vec_t __attribute__((vector_size(16)));
  typedef uint8_t flags_t __attribute__((vector_size(L)));
  constexpr U top = U(1) << (8 * sizeof(T) - 1);
  // Fail with row i.
  auto fail_at = [](uint32_t i) {
    return ty_internal::set_error(TY_ERR_RANGE, "result out of range", i);
  };
  // The overflow of row i, x + y: its sign bit, and whether it is computed.
  auto overflowed = [&](uint32_t i, U x, U y) {
    U r = x + y;
    return 0 == (flag[i] & TY_VEC_SKIP) && 0 != ((x ^ r) & (y ^ r) & top);
  };
  // Fail with the lowest row computed of [from, to), whose sums are in out,
  // that overflows; there is one.
  auto fail_in = [&](uint32_t from, uint32_t to) {
    uint32_t i = from;
    for (; i + 1 < to; i++) {
      U r = U(out[i]);
      U x = out == a ? U(r - U(b[i])) : U(a[i]);
      U y = out == b ? U(r - U(a[i])) : U(b[i]);
      if (overflowed(i, x, y)) {
        break;
      }
    }
    return fail_at(i);
  };
  // 1. and 2. Whole blocks of whole vectors.
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
      memcpy(&f, flag + i, L);
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
  // 3. The rest, one row at a time.
  for (; i < n; i++) {
    if (TY_UNLIKELY(overflowed(i, U(a[i]), U(b[i])))) {
      return fail_at(i);
    }
    out[i] = T(U(a[i]) + U(b[i]));
  }
  return 0;
}

/*
 * An operation on flags and bools, a byte each: out = op(flag, vec), by the
 * bits of a byte, in every row, valid or not. An operation that takes no
 * bools passes flag as vec. With CLEAR, the result is never null. op is
 * called with 16 rows in a vector, and with one row's two bytes.
 * 1. 16 rows at a time: read both inputs, and then write out and, with
 *    CLEAR, flag with its null bits 0.
 * 2. The rows past the last 16, one at a time, by the same rule.
 */
template <bool CLEAR, typename OP>
int flag_op(uint32_t n, uint8_t *flag, const uint8_t *vec, uint8_t *out,
            OP op) {
  typedef uint8_t vec_t __attribute__((vector_size(16)));
  static_assert(2 == TY_VEC_NULL, "an op takes the null bit by f >> 1");
  constexpr uint8_t keep = uint8_t(~TY_VEC_NULL);
  const uint32_t whole = n - n % 16;
  uint32_t i = 0;
  // 1. Whole vectors.
  for (; i < whole; i += 16) {
    vec_t f, v;
    memcpy(&f, flag + i, 16);
    memcpy(&v, vec + i, 16);
    vec_t r = op(f, v);
    memcpy(out + i, &r, 16);
    if constexpr (CLEAR) {
      f &= keep;
      memcpy(flag + i, &f, 16);
    }
  }
  // 2. The rest, one row at a time.
  for (; i < n; i++) {
    out[i] = uint8_t(op(flag[i], vec[i]));
    if constexpr (CLEAR) {
      flag[i] &= keep;
    }
  }
  return 0;
}

} // namespace

/* out = a + b of each row computed; see ty_vec.h. */
int ty_int16_add_vec(uint32_t n, uint8_t *flag, const int16_t *a,
                     const int16_t *b, int16_t *out) {
  return add_vec<int16_t, uint16_t>(n, flag, a, b, out);
}

/* out = a + b of each row computed; see ty_vec.h. */
int ty_int32_add_vec(uint32_t n, uint8_t *flag, const int32_t *a,
                     const int32_t *b, int32_t *out) {
  return add_vec<int32_t, uint32_t>(n, flag, a, b, out);
}

/* out = a + b of each row computed; see ty_vec.h. */
int ty_int64_add_vec(uint32_t n, uint8_t *flag, const int64_t *a,
                     const int64_t *b, int64_t *out) {
  return add_vec<int64_t, uint64_t>(n, flag, a, b, out);
}

/* out = 1 for each null row, and 0 for another; see ty_vec.h. */
int ty_flag_isnull(uint32_t n, uint8_t *flag, uint8_t *out) {
  return flag_op<true>(n, flag, flag, out,
                       [](auto f, auto) { return (f >> 1) & 1; });
}

/* out = 0 for each null row, and 1 for another; see ty_vec.h. */
int ty_flag_isnotnull(uint32_t n, uint8_t *flag, uint8_t *out) {
  return flag_op<true>(n, flag, flag, out,
                       [](auto f, auto) { return ~(f >> 1) & 1; });
}

/* out = 1 for each row true and not null, and 0 for another. */
int ty_flag_istrue(uint32_t n, uint8_t *flag, const uint8_t *vec,
                   uint8_t *out) {
  return flag_op<true>(n, flag, vec, out,
                       [](auto f, auto v) { return v & ~(f >> 1) & 1; });
}

/* out = 1 for each row false and not null, and 0 for another. */
int ty_flag_isfalse(uint32_t n, uint8_t *flag, const uint8_t *vec,
                    uint8_t *out) {
  return flag_op<true>(n, flag, vec, out,
                       [](auto f, auto v) { return ~(v | (f >> 1)) & 1; });
}

/* out = NOT vec; a null row stays null. See ty_vec.h. */
int ty_flag_not(uint32_t n, uint8_t *flag, const uint8_t *vec, uint8_t *out) {
  return flag_op<false>(n, flag, vec, out, [](auto, auto v) { return v ^ 1; });
}

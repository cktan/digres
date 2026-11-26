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

#include <assert.h>
#include <string.h>

#include <limits>

#ifdef __aarch64__
#include <arm_neon.h>
#endif

#include "error.h"
#include "ty_int.h"
#include "ty_print.h"

namespace {

/* Rows in a block: the loop tests for an overflow once a block. */
constexpr uint32_t block_rows = 256;

/*
 * The vector types of T, an integer or a float of U's width: L lanes in 16
 * bytes. An integer operation adds with wrap-around, so it computes in U.
 */
template <typename T, typename U> struct lanes_t {
  static constexpr uint32_t L = 16 / sizeof(T);
  typedef T elem_t;
  typedef U uelem_t;
  typedef U vec_t __attribute__((vector_size(16)));
  typedef T svec_t __attribute__((vector_size(16)));
  static constexpr U top = U(1) << (8 * sizeof(T) - 1);
};

/*
 * An operation of arith_vec() is a struct with:
 *   - vec(): the results of one vector, and in *bad, a lane a row, bits of
 *     which bad_bits are not all 0 if the row fails.
 *   - one(): the result of one row in *r, and TY_OK; or why it fails, with
 *     a *r that means nothing.
 *   - undoes: true if undo_a() gives a back of the result and b, and
 *     undo_b() gives b back of the result and a.
 */

/* a + b. It overflowed if its sign differs from both operands' signs. */
template <typename T, typename U> struct add_op_t : lanes_t<T, U> {
  typedef typename lanes_t<T, U>::vec_t vec_t;
  static constexpr U bad_bits = lanes_t<T, U>::top;
  static constexpr bool undoes = true;
  static vec_t vec(vec_t a, vec_t b, vec_t *bad) {
    vec_t r = a + b;
    *bad = (a ^ r) & (b ^ r);
    return r;
  }
  static ty_errnum_t one(T a, T b, T *r) {
    return __builtin_add_overflow(a, b, r) ? TY_ERR_RANGE : TY_OK;
  }
  static T undo_a(T r, T b) { return T(U(r) - U(b)); }
  static T undo_b(T r, T a) { return T(U(r) - U(a)); }
};

/*
 * a - b. It overflowed if the operands' signs differ, and the result's is
 * not a's.
 */
template <typename T, typename U> struct sub_op_t : lanes_t<T, U> {
  typedef typename lanes_t<T, U>::vec_t vec_t;
  static constexpr U bad_bits = lanes_t<T, U>::top;
  static constexpr bool undoes = true;
  static vec_t vec(vec_t a, vec_t b, vec_t *bad) {
    vec_t r = a - b;
    *bad = (a ^ b) & (a ^ r);
    return r;
  }
  static ty_errnum_t one(T a, T b, T *r) {
    return __builtin_sub_overflow(a, b, r) ? TY_ERR_RANGE : TY_OK;
  }
  static T undo_a(T r, T b) { return T(U(r) + U(b)); }
  static T undo_b(T r, T a) { return T(U(a) - U(r)); }
};

/*
 * a * b, of 16 or 32 bits: the product in W, of twice the width. Its low
 * half is the result, and it overflowed if its high half is not all the
 * result's sign.
 */
template <typename T, typename U, typename W> struct mul_op_t : lanes_t<T, U> {
  typedef typename lanes_t<T, U>::vec_t vec_t;
  typedef typename lanes_t<T, U>::svec_t svec_t;
  static constexpr int bits = 8 * sizeof(T);
  static constexpr U bad_bits = U(-1);
  static constexpr bool undoes = false;
  static vec_t vec(vec_t a, vec_t b, vec_t *bad) {
#ifdef __aarch64__
    // GCC 13 makes scalar multiplications of the loop below's int32.
    if constexpr (4 == sizeof(T)) {
      int32x4_t x = vreinterpretq_s32_u32((uint32x4_t)a);
      int32x4_t y = vreinterpretq_s32_u32((uint32x4_t)b);
      int32x4_t lo =
          vreinterpretq_s32_s64(vmull_s32(vget_low_s32(x), vget_low_s32(y)));
      int32x4_t hi = vreinterpretq_s32_s64(vmull_high_s32(x, y));
      int32x4_t r = vuzp1q_s32(lo, hi);
      *bad = (vec_t)veorq_s32(vuzp2q_s32(lo, hi), vshrq_n_s32(r, 31));
      return (vec_t)r;
    }
#endif
    typedef W wide_t __attribute__((vector_size(32)));
    wide_t p = __builtin_convertvector((svec_t)a, wide_t) *
               __builtin_convertvector((svec_t)b, wide_t);
    svec_t r = __builtin_convertvector(p, svec_t);
    svec_t high = __builtin_convertvector(p >> bits, svec_t);
    *bad = (vec_t)(high ^ (r >> (bits - 1)));
    return (vec_t)r;
  }
  static ty_errnum_t one(T a, T b, T *r) {
    return __builtin_mul_overflow(a, b, r) ? TY_ERR_RANGE : TY_OK;
  }
};

/*
 * a * b of 64 bits, one lane at a time: neither NEON nor SSE multiplies
 * 64 bits by 64 bits.
 */
struct mul64_op_t : lanes_t<int64_t, uint64_t> {
  static constexpr uint64_t bad_bits = 1;
  static constexpr bool undoes = false;
  static vec_t vec(vec_t a, vec_t b, vec_t *bad) {
    int64_t r0, r1;
    uint64_t o0 = __builtin_mul_overflow(int64_t(a[0]), int64_t(b[0]), &r0);
    uint64_t o1 = __builtin_mul_overflow(int64_t(a[1]), int64_t(b[1]), &r1);
    *bad = (vec_t){o0, o1};
    return (vec_t){uint64_t(r0), uint64_t(r1)};
  }
  static ty_errnum_t one(int64_t a, int64_t b, int64_t *r) {
    return __builtin_mul_overflow(a, b, r) ? TY_ERR_RANGE : TY_OK;
  }
};

/*
 * The quotients of x and y, cut to zero; no y is 0, and no row MIN / -1.
 * Neither NEON nor SSE divides integers.
 *   - 16 bits: in float, and 32 bits: in double. The quotient cut to an
 *     integer is exact: one that is no integer is 1 / |b| or more from the
 *     next, and the rounding error is less than that.
 *   - 64 bits: one lane at a time.
 */
template <typename T, typename svec_t> svec_t quotients(svec_t x, svec_t y) {
  if constexpr (2 == sizeof(T)) {
    typedef int32_t wide_t __attribute__((vector_size(32)));
    typedef float float_t __attribute__((vector_size(32)));
    float_t q =
        __builtin_convertvector(__builtin_convertvector(x, wide_t), float_t) /
        __builtin_convertvector(__builtin_convertvector(y, wide_t), float_t);
    return __builtin_convertvector(__builtin_convertvector(q, wide_t), svec_t);
  } else if constexpr (4 == sizeof(T)) {
#ifdef __aarch64__
    // GCC 13 makes scalar conversions of the vector types' below.
    int32x4_t a = (int32x4_t)x, b = (int32x4_t)y;
    float64x2_t lo = vdivq_f64(vcvtq_f64_s64(vmovl_s32(vget_low_s32(a))),
                               vcvtq_f64_s64(vmovl_s32(vget_low_s32(b))));
    float64x2_t hi = vdivq_f64(vcvtq_f64_s64(vmovl_high_s32(a)),
                               vcvtq_f64_s64(vmovl_high_s32(b)));
    return (svec_t)vuzp1q_s32(vreinterpretq_s32_s64(vcvtq_s64_f64(lo)),
                              vreinterpretq_s32_s64(vcvtq_s64_f64(hi)));
#else
    typedef double double_t __attribute__((vector_size(32)));
    return __builtin_convertvector(__builtin_convertvector(x, double_t) /
                                       __builtin_convertvector(y, double_t),
                                   svec_t);
#endif
  } else {
    return svec_t{x[0] / y[0], x[1] / y[1]};
  }
}

/*
 * a / b, to zero. It fails for a b of 0, and for MIN / -1, which does not
 * fit. Such a row, which may be one not computed, is divided by 1: the
 * machine may trap on it.
 */
template <typename T, typename U> struct div_op_t : lanes_t<T, U> {
  typedef typename lanes_t<T, U>::vec_t vec_t;
  typedef typename lanes_t<T, U>::svec_t svec_t;
  static constexpr U bad_bits = U(-1);
  static constexpr bool undoes = false;
  static constexpr T min = std::numeric_limits<T>::min();
  static vec_t vec(vec_t a, vec_t b, vec_t *bad) {
    svec_t x = (svec_t)a, y = (svec_t)b;
    svec_t fails = (y == 0) | ((x == min) & (y == T(-1)));
    *bad = (vec_t)fails;
    return (vec_t)quotients<T>(x, (y & ~fails) | (fails & 1));
  }
  static ty_errnum_t one(T a, T b, T *r) {
    *r = 0;
    if (0 == b) {
      return TY_ERR_DIVZERO;
    }
    if (min == a && -1 == b) {
      return TY_ERR_RANGE;
    }
    *r = T(a / b);
    return TY_OK;
  }
};

/*
 * a % b, with the sign of a. It fails for a b of 0. A row with a b of 0, or
 * of -1, is divided by 1: the machine may trap on both, and any a % -1 is 0.
 */
template <typename T, typename U> struct mod_op_t : lanes_t<T, U> {
  typedef typename lanes_t<T, U>::vec_t vec_t;
  typedef typename lanes_t<T, U>::svec_t svec_t;
  static constexpr U bad_bits = U(-1);
  static constexpr bool undoes = false;
  static vec_t vec(vec_t a, vec_t b, vec_t *bad) {
    svec_t x = (svec_t)a, y = (svec_t)b;
    svec_t fails = y == 0;
    svec_t by_one = fails | (y == T(-1));
    *bad = (vec_t)fails;
    y = (y & ~by_one) | (by_one & 1);
    if constexpr (8 == sizeof(T)) {
      return (vec_t)svec_t{x[0] % y[0], x[1] % y[1]};
    } else {
      return a - (vec_t)quotients<T>(x, y) * (vec_t)y;
    }
  }
  static ty_errnum_t one(T a, T b, T *r) {
    *r = 0;
    if (0 == b) {
      return TY_ERR_DIVZERO;
    }
    if (-1 != b) {
      *r = T(a % b);
    }
    return TY_OK;
  }
};

/*
 * The operations of floats, with PostgreSQL's errors (ty_float.h): a result
 * that is infinite when no input was overflows, RANGE; one that is 0 when no
 * input was is an underflow, UNDERFLOW. A NaN or an infinity that comes in
 * goes through.
 */
template <typename T, typename U> struct fp_op_t : lanes_t<T, U> {
  typedef typename lanes_t<T, U>::vec_t vec_t;
  typedef typename lanes_t<T, U>::svec_t fvec_t;
  static constexpr U bad_bits = U(-1);
  static constexpr bool undoes = false;
  static constexpr T inf = std::numeric_limits<T>::infinity();
  /* The lanes of x that are infinite. */
  static vec_t is_inf(fvec_t x) {
    return (vec_t)((fvec_t)((vec_t)x & U(~lanes_t<T, U>::top)) == inf);
  }
  static bool is_inf(T x) { return inf == x || -inf == x; }
};

/* a + b, or with SUB, a - b. */
template <typename T, typename U, bool SUB> struct fp_add_op_t : fp_op_t<T, U> {
  typedef typename lanes_t<T, U>::vec_t vec_t;
  typedef typename lanes_t<T, U>::svec_t fvec_t;
  using fp_op_t<T, U>::is_inf;
  static vec_t vec(vec_t a, vec_t b, vec_t *bad) {
    fvec_t x = (fvec_t)a, y = (fvec_t)b;
    fvec_t r = SUB ? x - y : x + y;
    *bad = is_inf(r) & ~(is_inf(x) | is_inf(y));
    return (vec_t)r;
  }
  static ty_errnum_t one(T a, T b, T *r) {
    *r = SUB ? a - b : a + b;
    return is_inf(*r) && !is_inf(a) && !is_inf(b) ? TY_ERR_RANGE : TY_OK;
  }
};

/* a * b. */
template <typename T, typename U> struct fp_mul_op_t : fp_op_t<T, U> {
  typedef typename lanes_t<T, U>::vec_t vec_t;
  typedef typename lanes_t<T, U>::svec_t fvec_t;
  using fp_op_t<T, U>::is_inf;
  static vec_t vec(vec_t a, vec_t b, vec_t *bad) {
    fvec_t x = (fvec_t)a, y = (fvec_t)b;
    fvec_t r = x * y;
    *bad = (is_inf(r) & ~(is_inf(x) | is_inf(y))) |
           (vec_t)((r == 0) & ~((x == 0) | (y == 0)));
    return (vec_t)r;
  }
  static ty_errnum_t one(T a, T b, T *r) {
    *r = a * b;
    if (is_inf(*r) && !is_inf(a) && !is_inf(b)) {
      return TY_ERR_RANGE;
    }
    return 0 == *r && 0 != a && 0 != b ? TY_ERR_UNDERFLOW : TY_OK;
  }
};

/*
 * a / b. A divisor of 0 is DIVZERO, unless a is NaN. A divisor that is
 * infinite gives 0 with no underflow.
 */
template <typename T, typename U> struct fp_div_op_t : fp_op_t<T, U> {
  typedef typename lanes_t<T, U>::vec_t vec_t;
  typedef typename lanes_t<T, U>::svec_t fvec_t;
  using fp_op_t<T, U>::is_inf;
  static vec_t vec(vec_t a, vec_t b, vec_t *bad) {
    fvec_t x = (fvec_t)a, y = (fvec_t)b;
    fvec_t r = x / y;
    // A NaN is not equal to itself.
    *bad = (vec_t)((y == 0) & (x == x)) | (is_inf(r) & ~is_inf(x)) |
           ((vec_t)((r == 0) & (x != 0)) & ~is_inf(y));
    return (vec_t)r;
  }
  static ty_errnum_t one(T a, T b, T *r) {
    *r = 0;
    if (0 == b && a == a) {
      return TY_ERR_DIVZERO;
    }
    *r = 0 == b ? a : a / b;
    if (is_inf(*r) && !is_inf(a)) {
      return TY_ERR_RANGE;
    }
    return 0 == *r && 0 != a && !is_inf(b) ? TY_ERR_UNDERFLOW : TY_OK;
  }
};

/* 16 rows' bytes in a vector. */
typedef uint8_t bytes_t __attribute__((vector_size(16)));

/*
 * Call fn(mask, row) for each vector of the 16 rows of m, a byte a row of
 * all ones or zeros: mask is the bytes of its rows, each W times, so a lane
 * of W bytes a row, and row is its first row.
 * 1. Interleave the low half of m with itself, for its first rows, and the
 *    high half for the others: each byte is there twice as often.
 * 2. Do it again with each, until a row has W bytes.
 * GCC makes a move a row of a __builtin_convertvector from bytes.
 */
template <uint32_t W, uint32_t ROWS = 16, typename FN>
inline void for_lanes(bytes_t m, size_t row, FN &&fn) {
  if constexpr (1 == W) {
    fn(m, row);
  } else {
    const bytes_t low = {0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7};
    const bytes_t high = low + 8;
    for_lanes<W / 2, ROWS / 2>(__builtin_shuffle(m, low), row, fn);
    for_lanes<W / 2, ROWS / 2>(__builtin_shuffle(m, high), row + ROWS / 2, fn);
  }
}

/* The message of a row's error, as the scalar operations word it. */
const char *row_error_msg(ty_errnum_t why) {
  return TY_ERR_DIVZERO == why     ? "division by zero"
         : TY_ERR_UNDERFLOW == why ? "result underflows"
                                   : "result out of range";
}

/*
 * An operation OP on two vectors of numbers, which may fail.
 * 1. For each block of rows, 16 rows at a time: merge flag_b into flag_a,
 *    if there is a flag_b. Then, 16 bytes at a time: compute, store the
 *    results, and gather into bad the failures of the rows computed: valid,
 *    not null, not failed. If out is a or b, and OP cannot undo a result,
 *    save the block's input first.
 * 2. At the block's end, if a row computed failed, find each that did, row
 *    by row, and mark it. If out is a or b, the block's results are over
 *    that input: undo them, which is exact, as they wrapped around, or take
 *    what was saved, to compute each row again. A failed row's result is
 *    left as it is: it means nothing.
 * 3. The rows past the last 16, one at a time, by the same rule.
 * The flags of a row not marked stay as they are: its result is valid if
 * its inputs are, and null if one of them is.
 */
template <typename OP>
int arith_vec(uint32_t n, uint8_t *flag, const typename OP::elem_t *a,
              const uint8_t *flag_b, const typename OP::elem_t *b,
              typename OP::elem_t *out, std::vector<ty_rowerr_t> &err) {
  typedef typename OP::elem_t T;
  typedef typename OP::vec_t vec_t;
  typedef uint64_t words_t __attribute__((vector_size(16)));
  const bool in_place = out == a || out == b;
  T saved[OP::undoes ? 1 : block_rows];
  int marked = 0;
  // Mark row i as failed, for why.
  auto mark = [&](size_t i, ty_errnum_t why) {
    flag[i] |= TY_VEC_ERROR;
    err.push_back({uint32_t(i), uint8_t(why)});
    marked = 1;
  };
  // Whether row i is computed.
  auto is_computed = [&](size_t i) { return 0 == (flag[i] & TY_VEC_SKIP); };
  // Mark each row computed of [from, to), whose results are in out, that
  // fails.
  auto mark_in = [&](size_t from, size_t to) {
    for (size_t i = from; i < to; i++) {
      T x = a[i], y = b[i], r;
      if constexpr (OP::undoes) {
        x = out == a ? OP::undo_a(out[i], b[i]) : x;
        y = out == b ? OP::undo_b(out[i], a[i]) : y;
      } else {
        x = out == a ? saved[i - from] : x;
        y = out == b ? saved[i - from] : y;
      }
      const ty_errnum_t why = OP::one(x, y, &r);
      if (is_computed(i) && TY_OK != why) {
        mark(i, why);
      }
    }
  };
  // 1. and 2. Whole blocks of 16 rows; a size_t, as a uint32_t wraps around,
  // which costs an instruction an address.
  size_t i = 0;
  const size_t whole = n - n % 16;
  while (i < whole) {
    const size_t from = i;
    const size_t to = whole - i < block_rows ? whole : i + block_rows;
    vec_t bad = {};
    if constexpr (!OP::undoes) {
      if (in_place) {
        memcpy(saved, out + from, (to - from) * sizeof(T));
      }
    }
    for (; i < to; i += 16) {
      bytes_t f;
      memcpy(&f, flag + i, 16);
      if (nullptr != flag_b) {
        bytes_t g;
        memcpy(&g, flag_b + i, 16);
        f |= g;
        memcpy(flag + i, &f, 16);
      }
      // All ones in the byte of a row computed, zero in another.
      const bytes_t computed = (bytes_t)((f & uint8_t(TY_VEC_SKIP)) == 0);
      for_lanes<sizeof(T)>(
          computed,
          i, [&](bytes_t mask, size_t row) __attribute__((always_inline)) {
            vec_t va, vb, o;
            memcpy(&va, a + row, 16);
            memcpy(&vb, b + row, 16);
            vec_t vr = OP::vec(va, vb, &o);
            memcpy(out + row, &vr, 16);
            bad |= o & (vec_t)mask;
          });
    }
    const words_t any = (words_t)(bad & OP::bad_bits);
    if (TY_UNLIKELY(any[0] | any[1])) {
      mark_in(from, to);
    }
  }
  // 3. The rest, one row at a time.
  for (; i < n; i++) {
    T r;
    if (nullptr != flag_b) {
      flag[i] |= flag_b[i];
    }
    const ty_errnum_t why = OP::one(a[i], b[i], &r);
    if (TY_UNLIKELY(TY_OK != why && is_computed(i))) {
      mark(i, why);
    }
    out[i] = r;
  }
  return marked;
}

/*
 * The masks of 16 rows from row on, a byte a row: fn(row) gives those of
 * one vector, a lane of W bytes a row, of all ones or zeros. It is the
 * reverse of for_lanes().
 * 1. Take the masks of the first half of the rows, and of the second.
 * 2. Keep every other byte of the two: a lane is half as wide.
 */
template <uint32_t W, uint32_t ROWS = 16, typename FN>
inline bytes_t narrow_masks(size_t row, FN &&fn) {
  if constexpr (1 == W) {
    return fn(row);
  } else {
    const bytes_t even = {0,  2,  4,  6,  8,  10, 12, 14,
                          16, 18, 20, 22, 24, 26, 28, 30};
    const bytes_t first = narrow_masks<W / 2, ROWS / 2>(row, fn);
    const bytes_t second = narrow_masks<W / 2, ROWS / 2>(row + ROWS / 2, fn);
    return __builtin_shuffle(first, second, even);
  }
}

/*
 * A compare of two vectors of T: out = op(a, b), a bool, in every row, valid
 * or not. op is called with a vector of each, for its masks, and with one
 * value of each.
 * 1. 16 rows at a time.
 * 2. The rows past the last 16, one at a time, by the same rule.
 */
template <typename T, typename OP>
int cmp_vec(uint32_t n, const T *a, const T *b, uint8_t *out, OP op) {
  typedef T vec_t __attribute__((vector_size(16)));
  size_t i = 0;
  const size_t whole = n - n % 16;
  // 1. 16 rows at a time.
  for (; i < whole; i += 16) {
    bytes_t r = narrow_masks<sizeof(T)>(
        i, [&](size_t row) __attribute__((always_inline)) {
          vec_t x, y;
          memcpy(&x, a + row, 16);
          memcpy(&y, b + row, 16);
          return (bytes_t)op(x, y);
        });
    r &= 1;
    memcpy(out + i, &r, 16);
  }
  // 2. The rest, one row at a time.
  for (; i < n; i++) {
    out[i] = uint8_t(op(a[i], b[i]));
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

/*
 * AND of two vectors of bool by SQL's three values, or OR if OR, in every
 * row, valid or not. A null row's byte may be no bool.
 *
 * AND is null if an input is, unless the other is known to be false: unless
 * it is not null and 0. So a row is null if an input is null, and a is null
 * or 1, and b is null or 1. OR is the same with "known to be true", so with
 * the bools turned around. The bits of a null are taken at the place of
 * TY_VEC_NULL: x + x has a bool there. A row failed if an input did, unless
 * a is known to be false: a left input is computed first, so only it drops
 * the other's error. So a's error bit stays, and b's is taken where a is
 * null or 1: (f | (x + x)) << 1 has those at TY_VEC_ERROR. A row is not valid
 * if an input is not. No flag_b is flags of all zeros.
 * 1. 16 rows at a time:
 *    a. The inputs, read before anything is written.
 *    b. The result. One of a null and a known input is right by its low bit.
 *    c. The result's three bits, over flag_a's; the other bits stay.
 * 2. The rows past the last 16, one at a time, by the same rule.
 */
template <bool OR>
int and_or_vec(uint32_t n, uint8_t *flag_a, const uint8_t *a,
               const uint8_t *flag_b, const uint8_t *b, uint8_t *out) {
  typedef uint8_t vec_t __attribute__((vector_size(16)));
  static_assert(2 == TY_VEC_NULL, "x + x puts a bool at the null bit");
  static_assert(4 == TY_VEC_ERROR, "f << 1 and x << 2 put bits at the error");
  constexpr uint8_t notvalid = TY_VEC_NOTVALID;
  constexpr uint8_t two = TY_VEC_NOTVALID | TY_VEC_NULL;
  constexpr uint8_t error = TY_VEC_ERROR;
  constexpr uint8_t keep = uint8_t(~TY_VEC_NULL);
  constexpr uint8_t turn = OR ? 1 : 0;
  // The result, and the flag, of 16 rows or of one.
  auto result = [](auto x, auto y) {
    if constexpr (OR) {
      return (x | y) & 1;
    } else {
      return x & y;
    }
  };
  auto flag_of = [](auto f, auto g, auto x, auto y) {
    x ^= turn;
    y ^= turn;
    // a's null or bool, at TY_VEC_NULL; one place up, at TY_VEC_ERROR.
    const auto fx = f | (x + x);
    return (f & keep) | ((f | g) & ((fx & (g | (y + y))) | notvalid) & two) |
           (g & (fx << 1) & error);
  };

  const uint32_t whole = n - n % 16;
  uint32_t i = 0;
  // 1. Whole vectors.
  for (; i < whole; i += 16) {
    // a. The inputs.
    vec_t f, g = {}, x, y;
    memcpy(&f, flag_a + i, 16);
    if (nullptr != flag_b) {
      memcpy(&g, flag_b + i, 16);
    }
    memcpy(&x, a + i, 16);
    memcpy(&y, b + i, 16);
    // b. The result.
    vec_t r = result(x, y);
    memcpy(out + i, &r, 16);
    // c. The result's two bits.
    f = flag_of(f, g, x, y);
    memcpy(flag_a + i, &f, 16);
  }
  // 2. The rest, one row at a time.
  for (; i < n; i++) {
    const uint8_t x = a[i], y = b[i];
    out[i] = uint8_t(result(x, y));
    const uint8_t g = nullptr != flag_b ? flag_b[i] : 0;
    flag_a[i] = uint8_t(flag_of(flag_a[i], g, x, y));
  }
  return 0;
}

} // namespace

/* out = a + b of each row computed; see ty_vec.h. */
int tyvec_int16_add(uint32_t n, uint8_t *flag_a, const int16_t *a,
                    const uint8_t *flag_b, const int16_t *b, int16_t *out,
                    std::vector<ty_rowerr_t> &err) {
  return arith_vec<add_op_t<int16_t, uint16_t>>(n, flag_a, a, flag_b, b, out,
                                                err);
}

/* out = a + b of each row computed; see ty_vec.h. */
int tyvec_int32_add(uint32_t n, uint8_t *flag_a, const int32_t *a,
                    const uint8_t *flag_b, const int32_t *b, int32_t *out,
                    std::vector<ty_rowerr_t> &err) {
  return arith_vec<add_op_t<int32_t, uint32_t>>(n, flag_a, a, flag_b, b, out,
                                                err);
}

/* out = a + b of each row computed; see ty_vec.h. */
int tyvec_int64_add(uint32_t n, uint8_t *flag_a, const int64_t *a,
                    const uint8_t *flag_b, const int64_t *b, int64_t *out,
                    std::vector<ty_rowerr_t> &err) {
  return arith_vec<add_op_t<int64_t, uint64_t>>(n, flag_a, a, flag_b, b, out,
                                                err);
}

/* out = a - b of each row computed; see ty_vec.h. */
int tyvec_int16_sub(uint32_t n, uint8_t *flag_a, const int16_t *a,
                    const uint8_t *flag_b, const int16_t *b, int16_t *out,
                    std::vector<ty_rowerr_t> &err) {
  return arith_vec<sub_op_t<int16_t, uint16_t>>(n, flag_a, a, flag_b, b, out,
                                                err);
}

/* out = a - b of each row computed; see ty_vec.h. */
int tyvec_int32_sub(uint32_t n, uint8_t *flag_a, const int32_t *a,
                    const uint8_t *flag_b, const int32_t *b, int32_t *out,
                    std::vector<ty_rowerr_t> &err) {
  return arith_vec<sub_op_t<int32_t, uint32_t>>(n, flag_a, a, flag_b, b, out,
                                                err);
}

/* out = a - b of each row computed; see ty_vec.h. */
int tyvec_int64_sub(uint32_t n, uint8_t *flag_a, const int64_t *a,
                    const uint8_t *flag_b, const int64_t *b, int64_t *out,
                    std::vector<ty_rowerr_t> &err) {
  return arith_vec<sub_op_t<int64_t, uint64_t>>(n, flag_a, a, flag_b, b, out,
                                                err);
}

/* out = a * b of each row computed; see ty_vec.h. */
int tyvec_int16_mul(uint32_t n, uint8_t *flag_a, const int16_t *a,
                    const uint8_t *flag_b, const int16_t *b, int16_t *out,
                    std::vector<ty_rowerr_t> &err) {
  return arith_vec<mul_op_t<int16_t, uint16_t, int32_t>>(n, flag_a, a, flag_b,
                                                         b, out, err);
}

/* out = a * b of each row computed; see ty_vec.h. */
int tyvec_int32_mul(uint32_t n, uint8_t *flag_a, const int32_t *a,
                    const uint8_t *flag_b, const int32_t *b, int32_t *out,
                    std::vector<ty_rowerr_t> &err) {
  return arith_vec<mul_op_t<int32_t, uint32_t, int64_t>>(n, flag_a, a, flag_b,
                                                         b, out, err);
}

/* out = a * b of each row computed; see ty_vec.h. */
int tyvec_int64_mul(uint32_t n, uint8_t *flag_a, const int64_t *a,
                    const uint8_t *flag_b, const int64_t *b, int64_t *out,
                    std::vector<ty_rowerr_t> &err) {
  return arith_vec<mul64_op_t>(n, flag_a, a, flag_b, b, out, err);
}

/* out = a / b of each row computed; see ty_vec.h. */
int tyvec_int16_div(uint32_t n, uint8_t *flag_a, const int16_t *a,
                    const uint8_t *flag_b, const int16_t *b, int16_t *out,
                    std::vector<ty_rowerr_t> &err) {
  return arith_vec<div_op_t<int16_t, uint16_t>>(n, flag_a, a, flag_b, b, out,
                                                err);
}

/* out = a / b of each row computed; see ty_vec.h. */
int tyvec_int32_div(uint32_t n, uint8_t *flag_a, const int32_t *a,
                    const uint8_t *flag_b, const int32_t *b, int32_t *out,
                    std::vector<ty_rowerr_t> &err) {
  return arith_vec<div_op_t<int32_t, uint32_t>>(n, flag_a, a, flag_b, b, out,
                                                err);
}

/* out = a / b of each row computed; see ty_vec.h. */
int tyvec_int64_div(uint32_t n, uint8_t *flag_a, const int64_t *a,
                    const uint8_t *flag_b, const int64_t *b, int64_t *out,
                    std::vector<ty_rowerr_t> &err) {
  return arith_vec<div_op_t<int64_t, uint64_t>>(n, flag_a, a, flag_b, b, out,
                                                err);
}

/* out = a % b of each row computed; see ty_vec.h. */
int tyvec_int16_mod(uint32_t n, uint8_t *flag_a, const int16_t *a,
                    const uint8_t *flag_b, const int16_t *b, int16_t *out,
                    std::vector<ty_rowerr_t> &err) {
  return arith_vec<mod_op_t<int16_t, uint16_t>>(n, flag_a, a, flag_b, b, out,
                                                err);
}

/* out = a % b of each row computed; see ty_vec.h. */
int tyvec_int32_mod(uint32_t n, uint8_t *flag_a, const int32_t *a,
                    const uint8_t *flag_b, const int32_t *b, int32_t *out,
                    std::vector<ty_rowerr_t> &err) {
  return arith_vec<mod_op_t<int32_t, uint32_t>>(n, flag_a, a, flag_b, b, out,
                                                err);
}

/* out = a % b of each row computed; see ty_vec.h. */
int tyvec_int64_mod(uint32_t n, uint8_t *flag_a, const int64_t *a,
                    const uint8_t *flag_b, const int64_t *b, int64_t *out,
                    std::vector<ty_rowerr_t> &err) {
  return arith_vec<mod_op_t<int64_t, uint64_t>>(n, flag_a, a, flag_b, b, out,
                                                err);
}

/* out = a + b of each row computed; see ty_vec.h. */
int tyvec_fp32_add(uint32_t n, uint8_t *flag_a, const float *a,
                   const uint8_t *flag_b, const float *b, float *out,
                   std::vector<ty_rowerr_t> &err) {
  return arith_vec<fp_add_op_t<float, uint32_t, false>>(n, flag_a, a, flag_b, b,
                                                        out, err);
}

/* out = a + b of each row computed; see ty_vec.h. */
int tyvec_fp64_add(uint32_t n, uint8_t *flag_a, const double *a,
                   const uint8_t *flag_b, const double *b, double *out,
                   std::vector<ty_rowerr_t> &err) {
  return arith_vec<fp_add_op_t<double, uint64_t, false>>(n, flag_a, a, flag_b,
                                                         b, out, err);
}

/* out = a - b of each row computed; see ty_vec.h. */
int tyvec_fp32_sub(uint32_t n, uint8_t *flag_a, const float *a,
                   const uint8_t *flag_b, const float *b, float *out,
                   std::vector<ty_rowerr_t> &err) {
  return arith_vec<fp_add_op_t<float, uint32_t, true>>(n, flag_a, a, flag_b, b,
                                                       out, err);
}

/* out = a - b of each row computed; see ty_vec.h. */
int tyvec_fp64_sub(uint32_t n, uint8_t *flag_a, const double *a,
                   const uint8_t *flag_b, const double *b, double *out,
                   std::vector<ty_rowerr_t> &err) {
  return arith_vec<fp_add_op_t<double, uint64_t, true>>(n, flag_a, a, flag_b, b,
                                                        out, err);
}

/* out = a * b of each row computed; see ty_vec.h. */
int tyvec_fp32_mul(uint32_t n, uint8_t *flag_a, const float *a,
                   const uint8_t *flag_b, const float *b, float *out,
                   std::vector<ty_rowerr_t> &err) {
  return arith_vec<fp_mul_op_t<float, uint32_t>>(n, flag_a, a, flag_b, b, out,
                                                 err);
}

/* out = a * b of each row computed; see ty_vec.h. */
int tyvec_fp64_mul(uint32_t n, uint8_t *flag_a, const double *a,
                   const uint8_t *flag_b, const double *b, double *out,
                   std::vector<ty_rowerr_t> &err) {
  return arith_vec<fp_mul_op_t<double, uint64_t>>(n, flag_a, a, flag_b, b, out,
                                                  err);
}

/* out = a / b of each row computed; see ty_vec.h. */
int tyvec_fp32_div(uint32_t n, uint8_t *flag_a, const float *a,
                   const uint8_t *flag_b, const float *b, float *out,
                   std::vector<ty_rowerr_t> &err) {
  return arith_vec<fp_div_op_t<float, uint32_t>>(n, flag_a, a, flag_b, b, out,
                                                 err);
}

/* out = a / b of each row computed; see ty_vec.h. */
int tyvec_fp64_div(uint32_t n, uint8_t *flag_a, const double *a,
                   const uint8_t *flag_b, const double *b, double *out,
                   std::vector<ty_rowerr_t> &err) {
  return arith_vec<fp_div_op_t<double, uint64_t>>(n, flag_a, a, flag_b, b, out,
                                                  err);
}

/* out = 1 for each null row, and 0 for another; see ty_vec.h. */
int tyvec_flag_isnull(uint32_t n, uint8_t *flag, uint8_t *out) {
  return flag_op<true>(n, flag, flag, out,
                       [](auto f, auto) { return (f >> 1) & 1; });
}

/* out = 0 for each null row, and 1 for another; see ty_vec.h. */
int tyvec_flag_isnotnull(uint32_t n, uint8_t *flag, uint8_t *out) {
  return flag_op<true>(n, flag, flag, out,
                       [](auto f, auto) { return ~(f >> 1) & 1; });
}

/* out = 1 for each row true and not null, and 0 for another. */
int tyvec_bool_istrue(uint32_t n, uint8_t *flag, const uint8_t *vec,
                      uint8_t *out) {
  return flag_op<true>(n, flag, vec, out,
                       [](auto f, auto v) { return v & ~(f >> 1) & 1; });
}

/* out = 1 for each row false and not null, and 0 for another. */
int tyvec_bool_isfalse(uint32_t n, uint8_t *flag, const uint8_t *vec,
                       uint8_t *out) {
  return flag_op<true>(n, flag, vec, out,
                       [](auto f, auto v) { return ~(v | (f >> 1)) & 1; });
}

/* out = NOT vec; a null row stays null. See ty_vec.h. */
int tyvec_bool_not(uint32_t n, uint8_t *flag, const uint8_t *vec,
                   uint8_t *out) {
  return flag_op<false>(n, flag, vec, out, [](auto, auto v) { return v ^ 1; });
}

/* out = a AND b, by SQL's three values; see ty_vec.h. */
int tyvec_bool_and(uint32_t n, uint8_t *flag_a, const uint8_t *a,
                   const uint8_t *flag_b, const uint8_t *b, uint8_t *out,
                   std::vector<ty_rowerr_t> & /* err */) {
  return and_or_vec<false>(n, flag_a, a, flag_b, b, out);
}

/* out = a OR b, by SQL's three values; see ty_vec.h. */
int tyvec_bool_or(uint32_t n, uint8_t *flag_a, const uint8_t *a,
                  const uint8_t *flag_b, const uint8_t *b, uint8_t *out,
                  std::vector<ty_rowerr_t> & /* err */) {
  return and_or_vec<true>(n, flag_a, a, flag_b, b, out);
}

/*
 * The lowest valid failed row's error; see ty_vec.h. Only the rows err
 * names are read: there are few, and none if no row failed.
 */
int tyvec_flag_error(uint32_t n, const uint8_t *flag,
                     const std::vector<ty_rowerr_t> &err) {
  const ty_rowerr_t *low = nullptr;
  for (const ty_rowerr_t &e : err) {
    if (e.row < n &&
        TY_VEC_ERROR == (flag[e.row] & (TY_VEC_ERROR | TY_VEC_NOTVALID)) &&
        (nullptr == low || e.row < low->row)) {
      low = &e;
    }
  }
  if (nullptr == low) {
    return 0;
  }
  const ty_errnum_t why = ty_errnum_t(low->code);
  return ty_internal::set_error(why, row_error_msg(why), low->row);
}

/*
 * a |= b | c of n bytes, or a |= b with no c; see ty_vec.h.
 * 1. 16 bytes at a time, each read before a is written.
 * 2. The bytes past the last 16, one at a time.
 */
void ty_bitor_vec(uint32_t n, uint8_t *a, const uint8_t *b, const uint8_t *c) {
  const uint32_t whole = n - n % 16;
  uint32_t i = 0;
  // 1. 16 bytes at a time; with no c, b is or'd twice, which is b.
  const uint8_t *c2 = nullptr != c ? c : b;
  for (; i < whole; i += 16) {
    bytes_t x, y, z;
    memcpy(&x, a + i, 16);
    memcpy(&y, b + i, 16);
    memcpy(&z, c2 + i, 16);
    x |= y | z;
    memcpy(a + i, &x, 16);
  }
  // 2. The rest.
  for (; i < n; i++) {
    a[i] |= uint8_t(b[i] | c2[i]);
  }
}

/*
 * b's errors that a result keeps; see ty_vec.h. Few rows fail, so the rows
 * of err are noted only when both lists have entries.
 */
int tyvec_err_merge(uint32_t n, const uint8_t *flag,
                    std::vector<ty_rowerr_t> &err,
                    const std::vector<ty_rowerr_t> &err_b) {
  if (err_b.empty() || &err == &err_b) {
    return 0;
  }
  std::vector<bool> in_err;
  if (!err.empty()) {
    in_err.assign(n, false);
    for (const ty_rowerr_t &e : err) {
      if (e.row < n) {
        in_err[e.row] = true;
      }
    }
  }
  for (const ty_rowerr_t &e : err_b) {
    if (e.row < n && (flag[e.row] & TY_VEC_ERROR) &&
        (in_err.empty() || !in_err[e.row])) {
      err.push_back(e);
    }
  }
  return 0;
}

/*
 * The six compares of a type, out = a op b of each row; see ty_vec.h. Each
 * OP is an expression of x and y, which are two vectors or two values.
 */
#define CMP_FN(TYPE, T, NAME, OP)                                              \
  int tyvec_##TYPE##_##NAME(uint32_t n, uint8_t * /* flag */, const T *a,      \
                            const T *b, uint8_t *out) {                        \
    return cmp_vec<T>(n, a, b, out, [](auto x, auto y) { return OP; });        \
  }
#define CMP_FNS(TYPE, T)                                                       \
  CMP_FN(TYPE, T, eq, x == y)                                                  \
  CMP_FN(TYPE, T, ne, x != y)                                                  \
  CMP_FN(TYPE, T, lt, x < y)                                                   \
  CMP_FN(TYPE, T, le, x <= y)                                                  \
  CMP_FN(TYPE, T, gt, x > y)                                                   \
  CMP_FN(TYPE, T, ge, x >= y)
/* All NaNs are equal, and above every number; a NaN is not equal to itself. */
#define CMP_FNS_FP(TYPE, T)                                                    \
  CMP_FN(TYPE, T, eq, (x == y) | ((x != x) & (y != y)))                        \
  CMP_FN(TYPE, T, ne, (x != y) & ((x == x) | (y == y)))                        \
  CMP_FN(TYPE, T, lt, (x == x) & !(x >= y))                                    \
  CMP_FN(TYPE, T, le, (y != y) | (x <= y))                                     \
  CMP_FN(TYPE, T, gt, (y == y) & !(x <= y))                                    \
  CMP_FN(TYPE, T, ge, (x != x) | (x >= y))

CMP_FNS(int16, int16_t)
CMP_FNS(int32, int32_t)
CMP_FNS(int64, int64_t)
CMP_FNS(bool, uint8_t)
CMP_FNS_FP(fp32, float)
CMP_FNS_FP(fp64, double)

namespace {

/*
 * The shuffles that pack a group of R rows of W bytes each, R * W bytes in
 * all, whose bytes are at Off on in a vector: for each mask of the rows
 * kept, the places of the kept rows' bytes in their order, then 0s; and the
 * count of the rows kept.
 */
template <uint32_t R, uint32_t W, uint32_t Off> struct pack_table_t {
  alignas(16) uint8_t idx[1u << R][R * W];
  uint8_t count[1u << R];
  constexpr pack_table_t() : idx(), count() {
    for (uint32_t m = 0; m < (1u << R); m++) {
      uint32_t at = 0;
      for (uint32_t r = 0; r < R; r++) {
        if (m & (1u << r)) {
          for (uint32_t b = 0; b < W; b++) {
            idx[m][at * W + b] = uint8_t(Off + r * W + b);
          }
          at++;
        }
      }
      count[m] = uint8_t(at);
    }
  }
};

/*
 * The tables: of the low and high 8 rows of a vector of bytes, a flag or a
 * bool a row; and of a vector of 8, 4 or 2 rows of 2, 4 or 8 bytes.
 */
constexpr pack_table_t<8, 1, 0> pack_lo = {};
constexpr pack_table_t<8, 1, 8> pack_hi = {};
constexpr pack_table_t<8, 2, 0> pack16 = {};
constexpr pack_table_t<4, 4, 0> pack32 = {};
constexpr pack_table_t<2, 8, 0> pack64 = {};

/* The table of a value of W bytes, 2, 4 or 8. */
template <uint32_t W> constexpr const auto &pack_of() {
  if constexpr (2 == W) {
    return pack16;
  } else if constexpr (4 == W) {
    return pack32;
  } else {
    return pack64;
  }
}

/* Store at to the B bytes, 16 or 8, of v that idx names, in its order. */
template <uint32_t B>
inline void shuffle_to(bytes_t v, const uint8_t *idx, uint8_t *to) {
#ifdef __aarch64__
  // GCC's __builtin_shuffle adds an AND of each place, which tbl needs not.
  if constexpr (16 == B) {
    vst1q_u8(to, vqtbl1q_u8((uint8x16_t)v, vld1q_u8(idx)));
  } else {
    vst1_u8(to, vqtbl1_u8((uint8x16_t)v, vld1_u8(idx)));
  }
#else
  bytes_t places = {};
  memcpy(&places, idx, B);
  const bytes_t out = __builtin_shuffle(v, places);
  memcpy(to, &out, B);
#endif
}

static_assert(1 == TY_VEC_NOTVALID, "kept_of takes the bit as 0 or 1");

/*
 * True if no row of the 16 flags at flag is dropped: a test of two words,
 * apart from the vector of the flags, so a block with none dropped does no
 * vector work.
 */
inline bool none_of(const uint8_t *flag) {
  uint64_t lo, hi;
  memcpy(&lo, flag, 8);
  memcpy(&hi, flag + 8, 8);
  return 0 == ((lo | hi) & 0x0101010101010101ull);
}

/* The bits of the bytes of d that are 0, of 16 bytes of 0 or 1: byte i at
   bit i. */
inline uint32_t kept_of(bytes_t d) {
#ifdef __aarch64__
  const uint8x16_t bit = {1, 2, 4, 8, 16, 32, 64, 128,
                          1, 2, 4, 8, 16, 32, 64, 128};
  const uint8x16_t k = vandq_u8(vceqzq_u8((uint8x16_t)d), bit);
  return vaddv_u8(vget_low_u8(k)) | uint32_t(vaddv_u8(vget_high_u8(k))) << 8;
#else
  // A multiply moves each byte's bit to its place in the top byte.
  const bytes_t one = d ^ uint8_t(1);
  uint64_t lo, hi;
  memcpy(&lo, &one, 8);
  memcpy(&hi, reinterpret_cast<const char *>(&one) + 8, 8);
  const uint64_t magic = 0x0102040810204080ull;
  return uint32_t((lo * magic) >> 56) | uint32_t((hi * magic) >> 56) << 8;
#endif
}

/*
 * Pack the kept rows of the 16 rows of W bytes at from to to, at or before
 * from, and give their count. Each store ends at or before the end of the
 * group it was loaded from, so no row not yet loaded is written over.
 * 1. W of 1: load the 16 bytes; shuffle the low 8 rows into 8 bytes, then
 *    the high 8 rows after them.
 * 2. W of 16: each row is a vector; store it at to, and step to past it if
 *    it is kept, with no branch.
 * 3. Else: each group of 16 bytes, R rows, is loaded and shuffled in turn.
 */
template <uint32_t W>
inline uint32_t pack_block(const uint8_t *from, uint8_t *to, uint32_t keep) {
  bytes_t v;
  if constexpr (1 == W) {
    // 1. Bytes.
    memcpy(&v, from, 16);
    const uint32_t lo = keep & 0xff, hi = keep >> 8;
    shuffle_to<8>(v, pack_lo.idx[lo], to);
    const uint32_t c = pack_lo.count[lo];
    shuffle_to<8>(v, pack_hi.idx[hi], to + c);
    return c + pack_hi.count[hi];
  } else if constexpr (16 == W) {
    // 2. Rows of 16 bytes.
    uint32_t at = 0;
    for (uint32_t r = 0; r < 16; r++) {
      memcpy(&v, from + r * 16, 16);
      memcpy(to + at * 16, &v, 16);
      at += (keep >> r) & 1;
    }
    return at;
  } else {
    // 3. Groups of 16 bytes.
    constexpr uint32_t R = 16 / W;
    const auto &t = pack_of<W>();
    uint32_t at = 0;
    for (uint32_t g = 0; g < 16; g += R) {
      const uint32_t m = (keep >> g) & ((1u << R) - 1);
      memcpy(&v, from + g * W, 16);
      shuffle_to<16>(v, t.idx[m], to + at * W);
      at += t.count[m];
    }
    return at;
  }
}

/*
 * Drop the rows not valid of vec, of rows of W bytes, and of flag; see
 * tyvec_squeeze in ty_vec.h. Only W matters, not the type. A row is written at
 * or before the place it is read from, so the rows move forward, over rows
 * already read.
 * 1. 16 rows at a time: which rows are dropped, from their flags.
 * 2. None dropped, which two words of the flags show: move the 16 rows, 16
 * bytes at a time forward, or leave them where they are if none was dropped
 * before.
 * 3. All dropped: pass them.
 * 4. Else pack the values and the flags, by shuffles from the tables.
 * 5. The rows past the last 16, one at a time.
 */
template <uint32_t W> int squeeze_vec(uint32_t n, uint8_t *flag, void *vec) {
  static_assert(1 == W || 2 == W || 4 == W || 8 == W || 16 == W);
#ifndef NDEBUG
  // A vector to squeeze has no error: its errors were raised before.
  for (uint32_t k = 0; k < n; k++) {
    assert(0 == (flag[k] & TY_VEC_ERROR));
  }
#endif
  uint8_t *const bytes = static_cast<uint8_t *>(vec);
  const uint32_t whole = n - n % 16;
  uint32_t w = 0, i = 0;
  for (; i < whole; i += 16) {
    // 1, 2. None dropped.
    if (none_of(flag + i)) {
      if (w != i) {
        for (uint32_t k = 0; k < 16 * W; k += 16) {
          bytes_t v;
          memcpy(&v, bytes + i * W + k, 16);
          memcpy(bytes + w * W + k, &v, 16);
        }
        memmove(flag + w, flag + i, 16);
      }
      w += 16;
      continue;
    }
    // 1. The rows dropped, as bytes of 1. 3. All dropped.
    bytes_t f;
    memcpy(&f, flag + i, 16);
    const uint32_t keep = kept_of(f & uint8_t(TY_VEC_NOTVALID));
    if (0 == keep) {
      continue;
    }
    // 4. Some dropped.
    pack_block<W>(bytes + i * W, bytes + w * W, keep);
    w += pack_block<1>(flag + i, flag + w, keep);
  }
  // 5. The rest.
  for (; i < n; i++) {
    if (0 == (flag[i] & TY_VEC_NOTVALID)) {
      memmove(bytes + w * W, bytes + i * W, W);
      flag[w++] = flag[i];
    }
  }
  return int(w);
}

/*
 * n lines of a vector's rows into out; see tyvec_print in ty_vec.h. A value
 * is written by ty_print, as V, which is T but for a bool.
 * 1. Each row's first entry in err, if any row failed.
 * 2. Each row: its index, then what it is: not valid, failed, null, or its
 *    value.
 */
template <typename V, typename T>
void print_vec(uint32_t n, const uint8_t *flag, const T *values,
               std::string &out, const std::vector<ty_rowerr_t> &err) {
  // 1. The entries; few rows fail.
  std::vector<const ty_rowerr_t *> first;
  if (!err.empty()) {
    first.assign(n, nullptr);
    for (const ty_rowerr_t &e : err) {
      if (e.row < n && nullptr == first[e.row]) {
        first[e.row] = &e;
      }
    }
  }
  // 2. The rows.
  char buf[TY_INT32_TEXT_SIZE];
  for (uint32_t i = 0; i < n; i++) {
    out.append(buf, ty_uint32_format_n(i, buf));
    out += ' ';
    const uint8_t f = nullptr != flag ? flag[i] : 0;
    if (f & TY_VEC_NOTVALID) {
      out += "notvalid";
    } else if (f & TY_VEC_ERROR) {
      out += "error ";
      const ty_rowerr_t *e = first.empty() ? nullptr : first[i];
      out += nullptr != e ? ty_errname(ty_errnum_t(e->code)) : "?";
    } else if (f & TY_VEC_NULL) {
      out += "null";
    } else {
      ty_print(V(values[i]), out);
    }
    out += '\n';
  }
}

} // namespace

/* Drop the rows not valid; see ty_vec.h. */
int tyvec_squeeze(uint32_t n, uint8_t *flag, int16_t *vec) {
  return squeeze_vec<sizeof *vec>(n, flag, vec);
}

/* Drop the rows not valid; see ty_vec.h. */
int tyvec_squeeze(uint32_t n, uint8_t *flag, int32_t *vec) {
  return squeeze_vec<sizeof *vec>(n, flag, vec);
}

/* Drop the rows not valid; see ty_vec.h. */
int tyvec_squeeze(uint32_t n, uint8_t *flag, int64_t *vec) {
  return squeeze_vec<sizeof *vec>(n, flag, vec);
}

/* Drop the rows not valid; see ty_vec.h. */
int tyvec_squeeze(uint32_t n, uint8_t *flag, float *vec) {
  return squeeze_vec<sizeof *vec>(n, flag, vec);
}

/* Drop the rows not valid; see ty_vec.h. */
int tyvec_squeeze(uint32_t n, uint8_t *flag, double *vec) {
  return squeeze_vec<sizeof *vec>(n, flag, vec);
}

/* Drop the rows not valid; see ty_vec.h. */
int tyvec_squeeze(uint32_t n, uint8_t *flag, uint8_t *vec) {
  return squeeze_vec<sizeof *vec>(n, flag, vec);
}

/* Drop the rows not valid; see ty_vec.h. */
int tyvec_squeeze(uint32_t n, uint8_t *flag, uint32_t *vec) {
  return squeeze_vec<sizeof *vec>(n, flag, vec);
}

/* Drop the rows not valid; see ty_vec.h. */
int tyvec_squeeze(uint32_t n, uint8_t *flag, const char **vec) {
  return squeeze_vec<sizeof *vec>(n, flag, vec);
}

/* Drop the rows not valid; see ty_vec.h. */
int tyvec_squeeze(uint32_t n, uint8_t *flag, ty_int128_t *vec) {
  return squeeze_vec<sizeof *vec>(n, flag, vec);
}

/* Drop the rows not valid; see ty_vec.h. */
int tyvec_squeeze(uint32_t n, uint8_t *flag, ty_uuid_t *vec) {
  return squeeze_vec<sizeof *vec>(n, flag, vec);
}

/* Drop the rows not valid; see ty_vec.h. */
int tyvec_squeeze(uint32_t n, uint8_t *flag, ty_interval_t *vec) {
  return squeeze_vec<sizeof *vec>(n, flag, vec);
}

/* Drop the rows not valid; see ty_vec.h. */
int tyvec_squeeze(uint32_t n, uint8_t *flag, ty_timetz_t *vec) {
  return squeeze_vec<sizeof *vec>(n, flag, vec);
}

/* A vector of int16, a line a row; see ty_vec.h. */
void tyvec_print(uint32_t n, const uint8_t *flag, const int16_t *values,
                 std::string &out, const std::vector<ty_rowerr_t> &err) {
  print_vec<int16_t>(n, flag, values, out, err);
}

/* A vector of int32, a line a row; see ty_vec.h. */
void tyvec_print(uint32_t n, const uint8_t *flag, const int32_t *values,
                 std::string &out, const std::vector<ty_rowerr_t> &err) {
  print_vec<int32_t>(n, flag, values, out, err);
}

/* A vector of int64, a line a row; see ty_vec.h. */
void tyvec_print(uint32_t n, const uint8_t *flag, const int64_t *values,
                 std::string &out, const std::vector<ty_rowerr_t> &err) {
  print_vec<int64_t>(n, flag, values, out, err);
}

/* A vector of float, a line a row; see ty_vec.h. */
void tyvec_print(uint32_t n, const uint8_t *flag, const float *values,
                 std::string &out, const std::vector<ty_rowerr_t> &err) {
  print_vec<float>(n, flag, values, out, err);
}

/* A vector of double, a line a row; see ty_vec.h. */
void tyvec_print(uint32_t n, const uint8_t *flag, const double *values,
                 std::string &out, const std::vector<ty_rowerr_t> &err) {
  print_vec<double>(n, flag, values, out, err);
}

/* A vector of bool, a byte a row, a line a row; see ty_vec.h. */
void tyvec_print(uint32_t n, const uint8_t *flag, const uint8_t *values,
                 std::string &out, const std::vector<ty_rowerr_t> &err) {
  print_vec<bool>(n, flag, values, out, err);
}

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

#include <limits>

#ifdef __aarch64__
#include <arm_neon.h>
#endif

#include "error.h"

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

/*
 * An operation OP on two vectors of numbers, which may fail.
 * 1. For each block of rows, 16 rows at a time, and 16 bytes of them at a
 *    time: compute, store the results, and gather into bad the failures
 *    of the rows computed, neither not valid nor null. If out is a or
 *    b, and OP cannot undo a result, save the block's input first.
 * 2. At the block's end, if a row computed failed, find the lowest one,
 *    row by row, and fail with its error and its index. If out is a or b, the
 *    block's results are over that input: undo them, which is exact, as
 *    they wrapped around, or take what was saved.
 * 3. The rows past the last 16, one at a time, by the same rule.
 * The flags stay as they are: a row's result is valid if its inputs are,
 * and null if one of them is.
 */
template <typename OP>
int arith_vec(uint32_t n, uint8_t *flag, const typename OP::elem_t *a,
              const typename OP::elem_t *b, typename OP::elem_t *out) {
  typedef typename OP::elem_t T;
  typedef typename OP::vec_t vec_t;
  typedef uint64_t words_t __attribute__((vector_size(16)));
  const bool in_place = out == a || out == b;
  T saved[OP::undoes ? 1 : block_rows];
  // Fail with row i, for why.
  auto fail_at = [](size_t i, ty_errnum_t why) {
    const char *msg = TY_ERR_DIVZERO == why     ? "division by zero"
                      : TY_ERR_UNDERFLOW == why ? "result underflows"
                                                : "result out of range";
    return ty_internal::set_error(why, msg, uint32_t(i));
  };
  // Whether row i is computed.
  auto is_computed = [&](size_t i) { return 0 == (flag[i] & TY_VEC_SKIP); };
  // Fail with the lowest row computed of [from, to), whose results are in
  // out, that fails; there is one.
  auto fail_in = [&](size_t from, size_t to) {
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
        return fail_at(i, why);
      }
    }
    return fail_at(to - 1, TY_ERR_RANGE);
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
      return fail_in(from, to);
    }
  }
  // 3. The rest, one row at a time.
  for (; i < n; i++) {
    T r;
    const ty_errnum_t why = OP::one(a[i], b[i], &r);
    if (TY_UNLIKELY(TY_OK != why && is_computed(i))) {
      return fail_at(i, why);
    }
    out[i] = r;
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
 * An operation on two vectors of bool: out = op(a, b), by the bits of a
 * byte, in every row, valid or not. op is called with 16 rows in a vector,
 * and with one row's two bytes.
 * 1. 16 rows at a time.
 * 2. The rows past the last 16, one at a time, by the same rule.
 */
template <typename OP>
int bool_op(uint32_t n, const uint8_t *a, const uint8_t *b, uint8_t *out,
            OP op) {
  typedef uint8_t vec_t __attribute__((vector_size(16)));
  const uint32_t whole = n - n % 16;
  uint32_t i = 0;
  // 1. Whole vectors.
  for (; i < whole; i += 16) {
    vec_t va, vb;
    memcpy(&va, a + i, 16);
    memcpy(&vb, b + i, 16);
    vec_t r = op(va, vb);
    memcpy(out + i, &r, 16);
  }
  // 2. The rest, one row at a time.
  for (; i < n; i++) {
    out[i] = uint8_t(op(a[i], b[i]));
  }
  return 0;
}

} // namespace

/* out = a + b of each row computed; see ty_vec.h. */
int tyvec_int16_add(uint32_t n, uint8_t *flag, const int16_t *a,
                    const int16_t *b, int16_t *out) {
  return arith_vec<add_op_t<int16_t, uint16_t>>(n, flag, a, b, out);
}

/* out = a + b of each row computed; see ty_vec.h. */
int tyvec_int32_add(uint32_t n, uint8_t *flag, const int32_t *a,
                    const int32_t *b, int32_t *out) {
  return arith_vec<add_op_t<int32_t, uint32_t>>(n, flag, a, b, out);
}

/* out = a + b of each row computed; see ty_vec.h. */
int tyvec_int64_add(uint32_t n, uint8_t *flag, const int64_t *a,
                    const int64_t *b, int64_t *out) {
  return arith_vec<add_op_t<int64_t, uint64_t>>(n, flag, a, b, out);
}

/* out = a - b of each row computed; see ty_vec.h. */
int tyvec_int16_sub(uint32_t n, uint8_t *flag, const int16_t *a,
                    const int16_t *b, int16_t *out) {
  return arith_vec<sub_op_t<int16_t, uint16_t>>(n, flag, a, b, out);
}

/* out = a - b of each row computed; see ty_vec.h. */
int tyvec_int32_sub(uint32_t n, uint8_t *flag, const int32_t *a,
                    const int32_t *b, int32_t *out) {
  return arith_vec<sub_op_t<int32_t, uint32_t>>(n, flag, a, b, out);
}

/* out = a - b of each row computed; see ty_vec.h. */
int tyvec_int64_sub(uint32_t n, uint8_t *flag, const int64_t *a,
                    const int64_t *b, int64_t *out) {
  return arith_vec<sub_op_t<int64_t, uint64_t>>(n, flag, a, b, out);
}

/* out = a * b of each row computed; see ty_vec.h. */
int tyvec_int16_mul(uint32_t n, uint8_t *flag, const int16_t *a,
                    const int16_t *b, int16_t *out) {
  return arith_vec<mul_op_t<int16_t, uint16_t, int32_t>>(n, flag, a, b, out);
}

/* out = a * b of each row computed; see ty_vec.h. */
int tyvec_int32_mul(uint32_t n, uint8_t *flag, const int32_t *a,
                    const int32_t *b, int32_t *out) {
  return arith_vec<mul_op_t<int32_t, uint32_t, int64_t>>(n, flag, a, b, out);
}

/* out = a * b of each row computed; see ty_vec.h. */
int tyvec_int64_mul(uint32_t n, uint8_t *flag, const int64_t *a,
                    const int64_t *b, int64_t *out) {
  return arith_vec<mul64_op_t>(n, flag, a, b, out);
}

/* out = a / b of each row computed; see ty_vec.h. */
int tyvec_int16_div(uint32_t n, uint8_t *flag, const int16_t *a,
                    const int16_t *b, int16_t *out) {
  return arith_vec<div_op_t<int16_t, uint16_t>>(n, flag, a, b, out);
}

/* out = a / b of each row computed; see ty_vec.h. */
int tyvec_int32_div(uint32_t n, uint8_t *flag, const int32_t *a,
                    const int32_t *b, int32_t *out) {
  return arith_vec<div_op_t<int32_t, uint32_t>>(n, flag, a, b, out);
}

/* out = a / b of each row computed; see ty_vec.h. */
int tyvec_int64_div(uint32_t n, uint8_t *flag, const int64_t *a,
                    const int64_t *b, int64_t *out) {
  return arith_vec<div_op_t<int64_t, uint64_t>>(n, flag, a, b, out);
}

/* out = a % b of each row computed; see ty_vec.h. */
int tyvec_int16_mod(uint32_t n, uint8_t *flag, const int16_t *a,
                    const int16_t *b, int16_t *out) {
  return arith_vec<mod_op_t<int16_t, uint16_t>>(n, flag, a, b, out);
}

/* out = a % b of each row computed; see ty_vec.h. */
int tyvec_int32_mod(uint32_t n, uint8_t *flag, const int32_t *a,
                    const int32_t *b, int32_t *out) {
  return arith_vec<mod_op_t<int32_t, uint32_t>>(n, flag, a, b, out);
}

/* out = a % b of each row computed; see ty_vec.h. */
int tyvec_int64_mod(uint32_t n, uint8_t *flag, const int64_t *a,
                    const int64_t *b, int64_t *out) {
  return arith_vec<mod_op_t<int64_t, uint64_t>>(n, flag, a, b, out);
}

/* out = a + b of each row computed; see ty_vec.h. */
int tyvec_fp32_add(uint32_t n, uint8_t *flag, const float *a, const float *b,
                   float *out) {
  return arith_vec<fp_add_op_t<float, uint32_t, false>>(n, flag, a, b, out);
}

/* out = a + b of each row computed; see ty_vec.h. */
int tyvec_fp64_add(uint32_t n, uint8_t *flag, const double *a, const double *b,
                   double *out) {
  return arith_vec<fp_add_op_t<double, uint64_t, false>>(n, flag, a, b, out);
}

/* out = a - b of each row computed; see ty_vec.h. */
int tyvec_fp32_sub(uint32_t n, uint8_t *flag, const float *a, const float *b,
                   float *out) {
  return arith_vec<fp_add_op_t<float, uint32_t, true>>(n, flag, a, b, out);
}

/* out = a - b of each row computed; see ty_vec.h. */
int tyvec_fp64_sub(uint32_t n, uint8_t *flag, const double *a, const double *b,
                   double *out) {
  return arith_vec<fp_add_op_t<double, uint64_t, true>>(n, flag, a, b, out);
}

/* out = a * b of each row computed; see ty_vec.h. */
int tyvec_fp32_mul(uint32_t n, uint8_t *flag, const float *a, const float *b,
                   float *out) {
  return arith_vec<fp_mul_op_t<float, uint32_t>>(n, flag, a, b, out);
}

/* out = a * b of each row computed; see ty_vec.h. */
int tyvec_fp64_mul(uint32_t n, uint8_t *flag, const double *a, const double *b,
                   double *out) {
  return arith_vec<fp_mul_op_t<double, uint64_t>>(n, flag, a, b, out);
}

/* out = a / b of each row computed; see ty_vec.h. */
int tyvec_fp32_div(uint32_t n, uint8_t *flag, const float *a, const float *b,
                   float *out) {
  return arith_vec<fp_div_op_t<float, uint32_t>>(n, flag, a, b, out);
}

/* out = a / b of each row computed; see ty_vec.h. */
int tyvec_fp64_div(uint32_t n, uint8_t *flag, const double *a, const double *b,
                   double *out) {
  return arith_vec<fp_div_op_t<double, uint64_t>>(n, flag, a, b, out);
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

/* out = a AND b; a row with a null input stays null. See ty_vec.h. */
int tyvec_bool_and(uint32_t n, uint8_t * /* flag */, const uint8_t *a,
                   const uint8_t *b, uint8_t *out) {
  return bool_op(n, a, b, out, [](auto x, auto y) { return x & y; });
}

/* out = a OR b; a row with a null input stays null. See ty_vec.h. */
int tyvec_bool_or(uint32_t n, uint8_t * /* flag */, const uint8_t *a,
                  const uint8_t *b, uint8_t *out) {
  return bool_op(n, a, b, out, [](auto x, auto y) { return x | y; });
}

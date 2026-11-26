/*
 * Copyright (c) 2026 CK Tan
 * SPDX-License-Identifier: MIT
 */

/*
 * test_vec.cc -- the vector operations of ty_vec.h, each row against the
 * scalar operation of ty_int.h or ty_float.h.
 */
#include "doctest.h"
#include "ty_bool.h"
#include "ty_error.h"
#include "ty_float.h"
#include "ty_int.h"
#include "ty_print.h"
#include "ty_vec.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <random>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

/* The scalar add of each width, for the reference. */
int scalar_add(int16_t a, int16_t b, int16_t *r) {
  return ty_int16_add(a, b, r);
}
int scalar_add(int32_t a, int32_t b, int32_t *r) {
  return ty_int32_add(a, b, r);
}
int scalar_add(int64_t a, int64_t b, int64_t *r) {
  return ty_int64_add(a, b, r);
}

/*
 * A vector operation that fails at once: it marks its failing rows, and
 * then fails with the lowest one's error, by tyvec_flag_error.
 */
template <typename FN> int at_once(uint32_t n, uint8_t *f, FN &&fn) {
  std::vector<ty_rowerr_t> err;
  const int rc = fn(err);
  REQUIRE((0 == rc) == err.empty());
  return 0 == rc ? 0 : tyvec_flag_error(n, f, err);
}

/* The vector add of each width, failing at once. */
int vec_add(uint32_t n, uint8_t *f, const int16_t *a, const int16_t *b,
            int16_t *out) {
  return at_once(n, f, [&](auto &err) {
    return tyvec_int16_add(n, f, a, nullptr, b, out, err);
  });
}
int vec_add(uint32_t n, uint8_t *f, const int32_t *a, const int32_t *b,
            int32_t *out) {
  return at_once(n, f, [&](auto &err) {
    return tyvec_int32_add(n, f, a, nullptr, b, out, err);
  });
}
int vec_add(uint32_t n, uint8_t *f, const int64_t *a, const int64_t *b,
            int64_t *out) {
  return at_once(n, f, [&](auto &err) {
    return tyvec_int64_add(n, f, a, nullptr, b, out, err);
  });
}

/* Columns of n rows of T: a, b and their flags. */
template <typename T> struct cols_t {
  std::vector<T> a, b;
  std::vector<uint8_t> flag;
};

/*
 * n rows of random values that never overflow when added (each within a
 * quarter of T's range), a row in about every invalid_in not valid, and
 * another in about every invalid_in null.
 */
template <typename T>
cols_t<T> random_cols(std::mt19937_64 &rng, uint32_t n, uint32_t invalid_in) {
  cols_t<T> c;
  const T quarter = std::numeric_limits<T>::max() / 4;
  std::uniform_int_distribution<int64_t> v(-quarter, quarter);
  for (uint32_t i = 0; i < n; i++) {
    c.a.push_back(T(v(rng)));
    c.b.push_back(T(v(rng)));
    uint8_t f = 0;
    if (rng() % invalid_in == 0) {
      f |= TY_VEC_NOTVALID;
    }
    if (rng() % invalid_in == 0) {
      f |= TY_VEC_NULL;
    }
    c.flag.push_back(f);
  }
  return c;
}

/*
 * The adds of every row agree with the scalar add, at sizes around the
 * vector's lanes and the loop's blocks of 256 rows.
 * 1. For each size, random rows and flags.
 * 2. The vector add succeeds; each computed row's sum is the scalar add's,
 *    and the flags are as they were.
 */
template <typename T> void check_agrees() {
  std::mt19937_64 rng(7);
  for (uint32_t n : {0u, 1u, 3u, 7u, 8u, 9u, 255u, 256u, 257u, 1000u, 10000u}) {
    cols_t<T> c = random_cols<T>(rng, n, 5);
    std::vector<T> out(n);
    std::vector<uint8_t> of = c.flag;
    REQUIRE(0 == vec_add(n, of.data(), c.a.data(), c.b.data(), out.data()));
    for (uint32_t i = 0; i < n; i++) {
      CHECK(of[i] == c.flag[i]);
      if (0 == (c.flag[i] & TY_VEC_SKIP)) {
        T r;
        REQUIRE(0 == scalar_add(c.a[i], c.b[i], &r));
        CHECK(out[i] == r);
      }
    }
  }
}

/*
 * An overflow fails at once with the lowest row computed that overflows,
 * and one in a row not valid, or null, is no error.
 * 1. Rows that overflow at 600 and 40, at 5 in a row not valid, and at 7
 *    in a null row: the
 *    add fails with RANGE, ty_erridx() = 40.
 * 2. Only the invalid row 5 and the null row 7 overflow: no error.
 * 3. Rows that overflow in the last rows past the whole vectors, and
 *    below by MIN + -1: the lowest is reported.
 * 4. MAX + 0 and MIN + MAX fit; MAX + 1 does not.
 */
template <typename T> void check_overflow() {
  const T max = std::numeric_limits<T>::max();
  const T min = std::numeric_limits<T>::min();
  std::mt19937_64 rng(11);
  const uint32_t n = 1003; // not a multiple of any lane count
  cols_t<T> c = random_cols<T>(rng, n, 1000000);
  std::vector<T> out(n);
  // 1. Three overflows, the lowest in a row not valid.
  cols_t<T> d = c;
  d.a[600] = max, d.b[600] = 1;
  d.a[40] = min, d.b[40] = -1;
  d.a[5] = max, d.b[5] = max, d.flag[5] = TY_VEC_NOTVALID;
  d.a[7] = max, d.b[7] = max, d.flag[7] = TY_VEC_NULL;
  CHECK(-1 == vec_add(n, d.flag.data(), d.a.data(), d.b.data(), out.data()));
  CHECK(TY_ERR_RANGE == ty_errnum());
  CHECK(40u == ty_erridx());
  // 2. Only the invalid row and the null row.
  cols_t<T> e = c;
  e.a[5] = max, e.b[5] = max, e.flag[5] = TY_VEC_NOTVALID;
  e.a[7] = max, e.b[7] = max, e.flag[7] = TY_VEC_NULL;
  CHECK(0 == vec_add(n, e.flag.data(), e.a.data(), e.b.data(), out.data()));
  // 3. The last row, and one before it.
  cols_t<T> g = c;
  g.a[n - 1] = max, g.b[n - 1] = max;
  CHECK(-1 == vec_add(n, g.flag.data(), g.a.data(), g.b.data(), out.data()));
  CHECK(n - 1 == ty_erridx());
  g.a[n - 2] = min, g.b[n - 2] = -1;
  CHECK(-1 == vec_add(n, g.flag.data(), g.a.data(), g.b.data(), out.data()));
  CHECK(n - 2 == ty_erridx());
  // 4. The edges.
  T a4[3] = {max, min, max}, b4[3] = {0, max, 1}, o4[3];
  uint8_t f4[3] = {0, 0, 0};
  CHECK(-1 == vec_add(3, f4, a4, b4, o4));
  CHECK(2u == ty_erridx());
  CHECK(0 == vec_add(2, f4, a4, b4, o4));
  CHECK(o4[0] == max);
  CHECK(o4[1] == T(-1));
}

/*
 * The results may be written over an input: out is a.
 * 1. Random rows; the add with out = a.
 * 2. a holds the sums, and the flags are as they were.
 */
template <typename T> void check_in_place() {
  std::mt19937_64 rng(13);
  const uint32_t n = 777;
  cols_t<T> c = random_cols<T>(rng, n, 3);
  cols_t<T> was = c;
  REQUIRE(0 == vec_add(n, c.flag.data(), c.a.data(), c.b.data(), c.a.data()));
  for (uint32_t i = 0; i < n; i++) {
    CHECK(c.flag[i] == was.flag[i]);
    if (0 == (was.flag[i] & TY_VEC_SKIP)) {
      CHECK(c.a[i] == T(was.a[i] + was.b[i]));
    }
  }
}

/*
 * An overflow is reported at its row when the results are written over
 * an input.
 * 1. Rows that overflow at 300 and 400, in the second block, and one in a
 *    row not valid at 290.
 * 2. With out = a, and then with out = b, the add fails at 300.
 * 3. The same at the last row of the block, and at its first.
 */
template <typename T> void check_overflow_in_place() {
  const T max = std::numeric_limits<T>::max();
  const T min = std::numeric_limits<T>::min();
  std::mt19937_64 rng(17);
  const uint32_t n = 777;
  // 1. Rows that overflow at 300 and 400, and at 290, not valid.
  cols_t<T> c = random_cols<T>(rng, n, 1000000);
  c.a[290] = max, c.b[290] = 1, c.flag[290] = TY_VEC_NOTVALID;
  c.a[300] = max, c.b[300] = 1;
  c.a[400] = min, c.b[400] = min;
  // 2. out = a, then out = b.
  cols_t<T> d = c;
  CHECK(-1 == vec_add(n, d.flag.data(), d.a.data(), d.b.data(), d.a.data()));
  CHECK(TY_ERR_RANGE == ty_errnum());
  CHECK(300u == ty_erridx());
  d = c;
  CHECK(-1 == vec_add(n, d.flag.data(), d.a.data(), d.b.data(), d.b.data()));
  CHECK(300u == ty_erridx());
  // 3. The last row of the block, and its first.
  for (uint32_t row : {511u, 256u}) {
    d = random_cols<T>(rng, n, 1000000);
    d.flag.assign(n, 0);
    d.a[row] = min, d.b[row] = -1;
    CHECK(-1 == vec_add(n, d.flag.data(), d.a.data(), d.b.data(), d.a.data()));
    CHECK(row == ty_erridx());
  }
}

/*
 * An operation on integers: the vector's of each width, which marks its
 * failing rows (mark) or fails at once (vec), and the scalar's.
 */
#define INT_OP_W(NAME, T, TYPE)                                                \
  static int mark(uint32_t n, uint8_t *f, const T *a, const uint8_t *g,        \
                  const T *b, T *out, std::vector<ty_rowerr_t> &err) {         \
    return tyvec_##TYPE##_##NAME(n, f, a, g, b, out, err);                     \
  }                                                                            \
  static int vec(uint32_t n, uint8_t *f, const T *a, const T *b, T *out) {     \
    return at_once(n, f, [&](auto &err) {                                      \
      return mark(n, f, a, nullptr, b, out, err);                              \
    });                                                                        \
  }                                                                            \
  static int one(T a, T b, T *r) { return ty_##TYPE##_##NAME(a, b, r); }
#define INT_OP(NAME)                                                           \
  struct NAME##_op {                                                           \
    INT_OP_W(NAME, int16_t, int16)                                             \
    INT_OP_W(NAME, int32_t, int32)                                             \
    INT_OP_W(NAME, int64_t, int64)                                             \
  };
INT_OP(add)
INT_OP(sub)
INT_OP(mul)
INT_OP(div)
INT_OP(mod)

/* The same of floats. */
#define FP_OP(NAME)                                                            \
  struct fp_##NAME##_op {                                                      \
    INT_OP_W(NAME, float, fp32)                                                \
    INT_OP_W(NAME, double, fp64)                                               \
  };
FP_OP(add)
FP_OP(sub)
FP_OP(mul)
FP_OP(div)

/* Whether a and b are the same value: of two floats, both NaN, or the same
   bits, so 0 is not -0. */
template <typename T> bool same(T a, T b) {
  return (a != a && b != b) || 0 == memcmp(&a, &b, sizeof a);
}

/* Where an operation's results go: an array of its own, a, b, or one array
   that is a, b and out. */
enum place_t { OWN, INTO_A, INTO_B, ONE };

/*
 * Run OP on the rows of a, b and flag, with the results at place, and check
 * it against the scalar operation, row by row. Returns the lowest row that
 * fails, or -1.
 * 1. The scalar operation of each computed row, and its error if it fails.
 * 2. The vector operation, on copies of the rows with one more at the end:
 *    with the flags all in flag_a and no flag_b, and then split between
 *    the two, some bits in both.
 * 3. Each computed row that fails by the scalar is marked, with its error,
 *    in row order, and no other; flag_a gets the flags ORed, and each other
 *    row gives the scalar's result. Nothing is written past the rows, and
 *    flag_b stays.
 * 4. tyvec_flag_error gives the lowest failing row, and its error.
 */
template <typename OP, typename T>
int64_t check_rows(const std::vector<T> &a, const std::vector<T> &b,
                   const std::vector<uint8_t> &flag, place_t place) {
  const uint32_t n = uint32_t(a.size());
  const std::vector<T> &second = ONE == place ? a : b;
  // 1. The scalar operation.
  std::vector<T> want(n);
  std::vector<ty_rowerr_t> want_err;
  for (uint32_t i = 0; i < n; i++) {
    if (0 == (flag[i] & TY_VEC_SKIP) &&
        0 != OP::one(a[i], second[i], &want[i])) {
      want_err.push_back({i, uint8_t(ty_errnum())});
    }
  }
  const int64_t fails = want_err.empty() ? -1 : int64_t(want_err[0].row);
  for (bool split : {false, true}) {
    CAPTURE(split);
    // 2. The vector operation.
    std::vector<T> x = a, y = b, own(n + 1, T(77));
    std::vector<uint8_t> f = flag, g(n + 1, 0);
    for (uint32_t i = 0; split && i < n; i++) {
      const uint8_t to_b = flag[i] & (0 == i % 3   ? 0x05
                                      : 1 == i % 3 ? 0x02
                                                   : 0);
      g[i] = to_b;
      f[i] = uint8_t(flag[i] & ~to_b) | (0 == i % 5 ? to_b : 0);
    }
    const std::vector<uint8_t> g_was = g;
    std::vector<ty_rowerr_t> err;
    x.push_back(T(77)), y.push_back(T(77)), f.push_back(0xff);
    T *out = OWN == place ? own.data() : INTO_B == place ? y.data() : x.data();
    int rc = OP::mark(n, f.data(), x.data(), split ? g.data() : nullptr,
                      ONE == place ? x.data() : y.data(), out, err);
    // 3. The marks and the results; one assertion each.
    CHECK(rc == (want_err.empty() ? 0 : 1));
    bool same_err = err.size() == want_err.size();
    for (size_t k = 0; same_err && k < err.size(); k++) {
      same_err =
          err[k].row == want_err[k].row && err[k].code == want_err[k].code;
    }
    CHECK(same_err);
    int64_t bad_flag = -1, bad_value = -1;
    size_t k = 0;
    for (uint32_t i = 0; i < n; i++) {
      const bool fails = k < want_err.size() && want_err[k].row == i;
      k += fails;
      if (f[i] != (fails ? flag[i] | TY_VEC_ERROR : flag[i]) && bad_flag < 0) {
        bad_flag = i;
      }
      if (!fails && 0 == (flag[i] & TY_VEC_SKIP) && !same(out[i], want[i]) &&
          bad_value < 0) {
        bad_value = i;
      }
    }
    CHECK(-1 == bad_flag);
    CHECK(-1 == bad_value);
    CHECK(T(77) == out[n]);
    CHECK(0xff == f[n]);
    CHECK(g == g_was);
    // 4. The lowest.
    CHECK(tyvec_flag_error(n, f.data(), err) == (fails < 0 ? 0 : -1));
    if (fails >= 0) {
      CHECK(fails == int64_t(ty_erridx()));
      CHECK(want_err[0].code == ty_errnum());
    }
  }
  return fails;
}

/*
 * OP agrees with the scalar operation on random rows, some of which fail.
 * 1. For sizes around 16 rows and the blocks of 256, rows of small values,
 *    and, at one in every wild, of any value; a row in five is not valid,
 *    and another in five null, with any value.
 * 2. Check the rows, with the results at each place; with one array for
 *    all, if OP is not add, as add does not allow it.
 * 3. Some runs failed, and some did not.
 */
template <typename OP, typename T> void check_random() {
  std::mt19937_64 rng(23);
  int failed = 0, passed = 0;
  for (uint32_t n : {0u, 1u, 15u, 16u, 17u, 33u, 255u, 256u, 257u, 1003u}) {
    for (uint32_t wild : {20u, 300u, 2000u, 1000000000u}) {
      // 1. The rows.
      std::vector<T> a(n), b(n);
      std::vector<uint8_t> flag(n);
      for (uint32_t i = 0; i < n; i++) {
        flag[i] = (0 == rng() % 5 ? TY_VEC_NOTVALID : 0) |
                  (0 == rng() % 5 ? TY_VEC_NULL : 0) |
                  (0 == rng() % 9 ? TY_VEC_ERROR : 0);
        const bool any = 0 != flag[i] || 0 == rng() % wild;
        a[i] = any ? T(rng()) : T(int64_t(rng() % 361) - 180);
        b[i] = any ? T(rng()) : T(int64_t(rng() % 361) - 180);
      }
      // 2. Check them at each place.
      for (place_t place : {OWN, INTO_A, INTO_B, ONE}) {
        if (ONE == place && std::is_same_v<OP, add_op>) {
          continue;
        }
        (check_rows<OP, T>(a, b, flag, place) < 0 ? passed : failed)++;
      }
    }
  }
  // 3. Both happened.
  CHECK(passed > 10);
  CHECK(failed > 10);
}

/*
 * One pair of values at each place of two 16 rows and their rest, or at
 * every step-th from first: it fails at its row, or fits, and is no error if
 * its row is not valid or null.
 */
template <typename OP, typename T>
void check_pair(T x, T y, bool fits, uint32_t first = 0, uint32_t step = 1) {
  const uint32_t n = 37;
  for (uint32_t at = first; at < n; at += step) {
    std::vector<T> a(n, T(3)), b(n, T(2));
    std::vector<uint8_t> flag(n, 0);
    a[at] = x, b[at] = y;
    for (place_t place : {OWN, INTO_A, INTO_B}) {
      CHECK((check_rows<OP, T>(a, b, flag, place)) ==
            (fits ? -1 : int64_t(at)));
      for (uint8_t skip :
           {TY_VEC_NOTVALID, TY_VEC_NULL, TY_VEC_ERROR, TY_VEC_SKIP}) {
        std::vector<uint8_t> f = flag;
        f[at] = skip;
        CHECK((check_rows<OP, T>(a, b, f, place)) == -1);
      }
    }
  }
}

/*
 * The edges of each operation.
 * 1. add: MAX + 1 and MIN + -1 overflow; MIN + MAX fits.
 * 2. sub: MIN - 1, MAX - -1 and 0 - MIN overflow; -1 - MIN and MIN - MIN
 *    fit.
 * 3. mul: MIN * -1, MIN * MIN and half of MAX + 1 times 2 overflow;
 *    MAX * -1, half of MIN times 2, MIN * 1 and MIN * 0 fit. So do the
 *    largest values whose product fits, and one more does not.
 * 4. div: anything / 0, and MIN / -1, fail; MAX / -1, MIN / 1, MIN / MIN
 *    and a quotient of each sign, cut to zero, are right.
 * 5. mod: anything % 0 fails; anything % -1 is 0, MIN too, and each sign is
 *    right.
 */
template <typename T> void check_edges() {
  const T max = std::numeric_limits<T>::max();
  const T min = std::numeric_limits<T>::min();
  // 1. add.
  check_pair<add_op, T>(max, 1, false);
  check_pair<add_op, T>(min, -1, false);
  check_pair<add_op, T>(min, max, true);
  // 2. sub.
  check_pair<sub_op, T>(min, 1, false);
  check_pair<sub_op, T>(max, -1, false);
  check_pair<sub_op, T>(0, min, false);
  check_pair<sub_op, T>(-1, min, true);
  check_pair<sub_op, T>(min, min, true);
  // 3. mul.
  check_pair<mul_op, T>(min, -1, false);
  check_pair<mul_op, T>(-1, min, false);
  check_pair<mul_op, T>(min, min, false);
  check_pair<mul_op, T>(T(max / 2 + 1), 2, false);
  check_pair<mul_op, T>(max, max, false);
  check_pair<mul_op, T>(max, -1, true);
  check_pair<mul_op, T>(T(min / 2), 2, true);
  check_pair<mul_op, T>(min, 1, true);
  check_pair<mul_op, T>(min, 0, true);
  const T half = T(T(1) << (4 * sizeof(T) - 1));
  check_pair<mul_op, T>(half, half, true);
  check_pair<mul_op, T>(half, T(2 * half), false);
  check_pair<mul_op, T>(half, T(-2 * half), true);
  check_pair<mul_op, T>(T(half + 1), T(-2 * half), false);
  // 4. div.
  check_pair<div_op, T>(1, 0, false);
  check_pair<div_op, T>(0, 0, false);
  check_pair<div_op, T>(min, 0, false);
  check_pair<div_op, T>(min, -1, false);
  check_pair<div_op, T>(max, -1, true);
  check_pair<div_op, T>(min, 1, true);
  check_pair<div_op, T>(min, min, true);
  check_pair<div_op, T>(max, min, true);
  check_pair<div_op, T>(T(max - 1), max, true);
  check_pair<div_op, T>(-7, 2, true);
  check_pair<div_op, T>(7, -2, true);
  check_pair<div_op, T>(-7, -2, true);
  // 5. mod.
  check_pair<mod_op, T>(1, 0, false);
  check_pair<mod_op, T>(0, 0, false);
  check_pair<mod_op, T>(min, 0, false);
  check_pair<mod_op, T>(min, -1, true);
  check_pair<mod_op, T>(max, -1, true);
  check_pair<mod_op, T>(min, 1, true);
  check_pair<mod_op, T>(min, min, true);
  check_pair<mod_op, T>(min, max, true);
  check_pair<mod_op, T>(max, min, true);
  check_pair<mod_op, T>(-7, 2, true);
  check_pair<mod_op, T>(7, -2, true);
  check_pair<mod_op, T>(-7, -2, true);
}

/*
 * A division fails with the error of its lowest failing row, and the error
 * of a row divided by 0 is DIVZERO.
 * 1. MIN / -1 at first, and 0 / 0 at second: RANGE at first.
 * 2. 0 / 0 at first, and MIN / -1 at second: DIVZERO at first.
 * 3. The same with the row at first not valid, and then null: the error of
 *    the row at second.
 * 4. A divisor of 0 in every row not computed is no error.
 */
template <typename T> void check_div_errors() {
  const T min = std::numeric_limits<T>::min();
  const uint32_t n = 600, first = 300, second = 590;
  std::vector<T> out(n);
  auto fails = [&](const std::vector<T> &a, const std::vector<T> &b,
                   std::vector<uint8_t> f, ty_errnum_t why, uint32_t row) {
    CHECK(-1 == div_op::vec(n, f.data(), a.data(), b.data(), out.data()));
    CHECK(why == ty_errnum());
    CHECK(row == ty_erridx());
  };
  std::vector<T> a(n, T(100)), b(n, T(7));
  std::vector<uint8_t> flag(n, 0);
  // 1. RANGE, and then DIVZERO.
  a[first] = min, b[first] = -1;
  a[second] = 0, b[second] = 0;
  fails(a, b, flag, TY_ERR_RANGE, first);
  // 2. DIVZERO, and then RANGE.
  std::swap(a[first], a[second]);
  std::swap(b[first], b[second]);
  fails(a, b, flag, TY_ERR_DIVZERO, first);
  CHECK(std::string("division by zero") == ty_errmsg());
  // 3. The first row not computed.
  flag[first] = TY_VEC_NOTVALID;
  fails(a, b, flag, TY_ERR_RANGE, second);
  flag[first] = TY_VEC_NULL;
  fails(a, b, flag, TY_ERR_RANGE, second);
  // 4. A divisor of 0 in every row not computed.
  for (uint32_t i = 0; i < n; i++) {
    flag[i] = uint8_t(i % 4);
    a[i] = T(i);
    b[i] = 0 == flag[i] ? T(3) : T(0);
  }
  CHECK((check_rows<div_op, T>(a, b, flag, OWN)) == -1);
}

/*
 * A quotient, or a remainder, is exact: the dividends around a multiple of the
 * divisor.
 * 1. Random divisors, and a multiple of each that fits, less 1, itself,
 *    and 1 more.
 * 2. Check the rows.
 */
template <typename OP, typename T> void check_div_exact() {
  std::mt19937_64 rng(29);
  const T max = std::numeric_limits<T>::max();
  std::vector<T> a, b;
  // 1. The rows.
  for (int i = 0; i < 5000; i++) {
    const T d = T(T(rng()) >> (rng() % (8 * sizeof(T) - 1)));
    const T y = 0 == d || -1 == d ? T(2) : d;
    const T q = T(T(rng()) % (max / 2) / y);
    for (int off = -1; off <= 1; off++) {
      a.push_back(T(q * y + off));
      b.push_back(y);
    }
  }
  // 2. Check the rows.
  CHECK((check_rows<OP, T>(a, b, std::vector<uint8_t>(a.size(), 0), OWN)) ==
        -1);
}

/*
 * OP agrees with the scalar operation on pairs of every two of a list of
 * floats: infinities, NaN, both zeros, the largest, the smallest and the
 * smallest normal ones, and others.
 * 1. Each pair alone, at some places of two 16 rows and their rest, which
 *    are others for the next pair.
 * 2. The pairs that fit, all in one vector, with some rows not computed that
 *    hold a pair that fails.
 * 3. The same with one pair that fails, at the end.
 */
template <typename OP, typename T> void check_fp() {
  typedef std::numeric_limits<T> lim;
  const T list[] = {0,
                    T(-0.0),
                    1,
                    -1,
                    T(0.5),
                    3,
                    T(-7.25),
                    lim::infinity(),
                    -lim::infinity(),
                    lim::quiet_NaN(),
                    lim::max(),
                    lim::lowest(),
                    lim::max() / 2,
                    lim::min(),
                    -lim::min(),
                    lim::denorm_min(),
                    -lim::denorm_min(),
                    lim::epsilon(),
                    T(1e-30),
                    T(1e30)};
  std::vector<T> good_a, good_b, bad_a, bad_b;
  // 1. Each pair alone.
  uint32_t pair = 0;
  for (T x : list) {
    for (T y : list) {
      T r;
      const bool fits = 0 == OP::one(x, y, &r);
      (fits ? good_a : bad_a).push_back(x);
      (fits ? good_b : bad_b).push_back(y);
      check_pair<OP, T>(x, y, fits, pair++ % 13, 13);
    }
  }
  REQUIRE(good_a.size() > 100);
  REQUIRE(bad_a.size() > 1);
  // 2. The pairs that fit, with others not computed.
  std::vector<T> a, b;
  std::vector<uint8_t> flag;
  for (size_t i = 0; i < good_a.size(); i++) {
    a.push_back(good_a[i]), b.push_back(good_b[i]), flag.push_back(0);
    if (0 == i % 3) {
      a.push_back(bad_a[i % bad_a.size()]);
      b.push_back(bad_b[i % bad_b.size()]);
      flag.push_back(i % 2 ? TY_VEC_NOTVALID : TY_VEC_NULL);
    }
  }
  for (place_t place : {OWN, INTO_A, INTO_B}) {
    CHECK((check_rows<OP, T>(a, b, flag, place)) == -1);
  }
  check_rows<OP, T>(a, b, flag, ONE);
  // 3. One that fails, at the end.
  for (size_t i = 0; i < bad_a.size(); i++) {
    a.push_back(bad_a[i]), b.push_back(bad_b[i]), flag.push_back(0);
    for (place_t place : {OWN, INTO_A, INTO_B}) {
      CHECK((check_rows<OP, T>(a, b, flag, place)) == int64_t(a.size() - 1));
    }
    a.pop_back(), b.pop_back(), flag.pop_back();
  }
}

/*
 * OP agrees with the scalar operation on random bits, which are floats of
 * every size.
 */
template <typename OP, typename T, typename U> void check_fp_random() {
  std::mt19937_64 rng(31);
  for (int k = 0; k < 2000; k++) {
    const uint32_t n = 1 + uint32_t(rng() % 40);
    std::vector<T> a(n), b(n);
    std::vector<uint8_t> flag(n);
    for (uint32_t i = 0; i < n; i++) {
      const U x = U(rng()), y = U(rng());
      memcpy(&a[i], &x, sizeof x);
      memcpy(&b[i], &y, sizeof y);
      flag[i] = uint8_t(rng() % 8 < 3 ? 1 + rng() % 7 : 0);
    }
    check_rows<OP, T>(a, b, flag, place_t(k % 4));
  }
}

/* A compare of a type: the vector's, the scalar's and its name. */
template <typename T> struct cmp_fns_t {
  const char *name;
  int (*vec)(uint32_t n, uint8_t *flag, const T *a, const T *b, uint8_t *out);
  int (*one)(T a, T b);
};
#define CMP_ROW(TYPE, T, NAME)                                                 \
  {#TYPE "_" #NAME, tyvec_##TYPE##_##NAME,                                     \
   [](T a, T b) { return (int)ty_##TYPE##_##NAME(a, b); }}
#define CMP_ROWS(TYPE, T)                                                      \
  {                                                                            \
    CMP_ROW(TYPE, T, eq), CMP_ROW(TYPE, T, ne), CMP_ROW(TYPE, T, lt),          \
        CMP_ROW(TYPE, T, le), CMP_ROW(TYPE, T, gt), CMP_ROW(TYPE, T, ge)       \
  }

/*
 * A compare of two vectors agrees with the scalar's in each computed row,
 * and changes no flag.
 * 1. For each compare, and sizes around 16 rows, every two of a list of
 *    values and of random ones; each lane gets each pair, as the pairs are
 *    shifted by a row each time. Flags are random.
 * 2. Each computed row is the scalar's answer, 1 or 0; a row not computed
 *    has any answer.
 * 3. The flags are as they were, and nothing is written past the rows.
 */
template <typename T>
void check_cmp(const std::vector<cmp_fns_t<T>> &fns, std::vector<T> list) {
  std::mt19937_64 rng(37);
  std::vector<T> a0, b0;
  for (T x : list) {
    for (T y : list) {
      a0.push_back(x), b0.push_back(y);
    }
  }
  for (const cmp_fns_t<T> &fn : fns) {
    INFO(fn.name);
    std::vector<T> a = a0, b = b0;
    for (uint32_t shift = 0; shift < 33; shift++) {
      // 1. The rows.
      const uint32_t n = uint32_t(a.size());
      std::vector<uint8_t> flag(n + 1, 0xff), out(n + 1, 0xaa);
      for (uint32_t i = 0; i < n; i++) {
        flag[i] = uint8_t(rng() % 8 < 3 ? 1 + rng() % 7 : 0);
      }
      const std::vector<uint8_t> was = flag;
      REQUIRE(0 == fn.vec(n, flag.data(), a.data(), b.data(), out.data()));
      // 2. The scalar's answers.
      uint32_t wrong = 0;
      for (uint32_t i = 0; i < n; i++) {
        wrong += 0 == (was[i] & TY_VEC_SKIP) && out[i] != fn.one(a[i], b[i]);
      }
      CHECK(0 == wrong);
      // 3. The flags, and past the rows.
      CHECK(flag == was);
      CHECK(0xaa == out[n]);
      a.insert(a.begin(), list[shift % list.size()]);
      b.insert(b.begin(), list[rng() % list.size()]);
    }
    // 1. to 3. again, for the small sizes, of the rows as shifted.
    for (uint32_t n : {0u, 1u, 15u, 16u, 17u, 31u}) {
      REQUIRE(n <= a.size());
      std::vector<uint8_t> flag(n + 1, 0), out(n + 1, 0xaa);
      REQUIRE(0 == fn.vec(n, flag.data(), a.data(), b.data(), out.data()));
      for (uint32_t i = 0; i < n; i++) {
        CHECK(out[i] == fn.one(a[i], b[i]));
      }
      CHECK(0xaa == out[n]);
    }
  }
}

/* The values an integer compare is tested on. */
template <typename T> std::vector<T> int_list() {
  typedef std::numeric_limits<T> lim;
  return {0,
          1,
          -1,
          2,
          -2,
          127,
          -128,
          255,
          256,
          -256,
          lim::max(),
          T(lim::max() - 1),
          lim::min(),
          T(lim::min() + 1),
          T(lim::max() / 2),
          T(lim::max() / 256),
          T(lim::min() / 256)};
}

/* The values a float compare is tested on; two NaNs of other bits. */
template <typename T> std::vector<T> fp_list() {
  typedef std::numeric_limits<T> lim;
  return {0,
          T(-0.0),
          1,
          -1,
          T(1) + lim::epsilon(),
          lim::infinity(),
          -lim::infinity(),
          lim::quiet_NaN(),
          std::copysign(lim::quiet_NaN(), T(-1)),
          lim::max(),
          lim::lowest(),
          lim::min(),
          lim::denorm_min(),
          -lim::denorm_min()};
}

} // namespace

// Verify that IS NULL of a vector is the null bit of each row, and that no
// result is null.
// 1. For each size around the 16 bytes of a vector instruction, and one
//    past all 256 values of a flag, rows whose flags are 0, 1, 2 and on.
// 2. out is 1 for a null row, valid or not, and 0 for another; the null
//    bit is 0 after, and each other bit is as it was.
// 3. Nothing is written past the n rows.
TEST_CASE("IS NULL of a vector is each row's null bit") {
  for (uint32_t n : {0u, 1u, 15u, 16u, 17u, 31u, 33u, 256u, 1003u}) {
    // 1. Rows of every flag; one more row, to see it is not written.
    std::vector<uint8_t> flag(n + 1), out(n + 1, 0xaa);
    for (uint32_t i = 0; i <= n; i++) {
      flag[i] = uint8_t(i);
    }
    flag[n] = 0xff;
    REQUIRE(0 == tyvec_flag_isnull(n, flag.data(), out.data()));
    // 2. The null bit of each row, and the flags after.
    for (uint32_t i = 0; i < n; i++) {
      CHECK(out[i] == (uint8_t(i) & TY_VEC_NULL ? 1 : 0));
      CHECK(flag[i] == (uint8_t(i) & 0xfd));
    }
    // 3. Nothing past the n rows.
    CHECK(0xaa == out[n]);
    CHECK(0xff == flag[n]);
  }
}

// Verify that IS NOT NULL, IS TRUE, IS FALSE and NOT of a vector give SQL's
// answer for each of true, false and null.
// 1. For each size around the 16 bytes of a vector instruction, and one
//    past all 256 values of a flag, rows of each flag with each bool. A row
//    not computed holds a byte that is no bool.
// 2. Each operation, into an array of its own, and into vec.
// 3. out is SQL's answer for each valid row that has not failed, and a
//    bool; NOT has none for a null row.
// 4. The null bit is 0 after, and as it was after NOT; each other bit is as
//    it was.
// 5. Nothing is written past the n rows.
TEST_CASE("IS NOT NULL, IS TRUE, IS FALSE and NOT of a vector") {
  enum { NOTNULL, ISTRUE, ISFALSE, NOT, NOPS };
  for (uint32_t n : {0u, 1u, 15u, 16u, 17u, 31u, 33u, 256u, 2003u}) {
    for (int op = NOTNULL; op < NOPS; op++) {
      for (bool in_place : {false, true}) {
        // 1. Rows of each flag with each bool; one more row at the end.
        std::vector<uint8_t> was(n + 1, 0xff), vec(n + 1, 0xaa);
        for (uint32_t i = 0; i < n; i++) {
          was[i] = uint8_t(i / 2);
          vec[i] = was[i] & TY_VEC_SKIP ? uint8_t(0x54 + i) : uint8_t(i % 2);
        }
        std::vector<uint8_t> flag = was, b = vec, own(n + 1, 0xaa);
        uint8_t *out = in_place && NOTNULL != op ? b.data() : own.data();
        // 2. The operation.
        int rc = -1;
        switch (op) {
        case NOTNULL:
          rc = tyvec_flag_isnotnull(n, flag.data(), out);
          break;
        case ISTRUE:
          rc = tyvec_bool_istrue(n, flag.data(), b.data(), out);
          break;
        case ISFALSE:
          rc = tyvec_bool_isfalse(n, flag.data(), b.data(), out);
          break;
        case NOT:
          rc = tyvec_bool_not(n, flag.data(), b.data(), out);
          break;
        }
        REQUIRE(0 == rc);
        for (uint32_t i = 0; i < n; i++) {
          const bool null = was[i] & TY_VEC_NULL;
          const bool valid = !(was[i] & TY_VEC_NOTVALID);
          // A failed row has no answer, and stays failed.
          const bool failed = was[i] & TY_VEC_ERROR;
          // 3. SQL's answer.
          if (NOTNULL == op) {
            CHECK(out[i] == !null);
          } else if (NOT == op) {
            CHECK((!valid || null || failed || out[i] == !vec[i]));
          } else if (valid && !failed) {
            CHECK(out[i] == (!null && vec[i] == (ISTRUE == op)));
          }
          CHECK((NOT == op || out[i] <= 1));
          // 4. The flags.
          CHECK(flag[i] == (NOT == op ? was[i] : was[i] & 0xfd));
        }
        // 5. Nothing past the n rows.
        CHECK(0xaa == out[n]);
        CHECK(0xff == flag[n]);
      }
    }
  }
}

// Verify that AND and OR of two vectors give SQL's answer for each pair of
// true, false, null and failed, whatever byte a null or failed row holds.
// 1. For each size around the 16 bytes of a vector instruction, rows of each
//    pair of flags, all 8 bits of a's and some of b's, with each pair of
//    bools. A null or failed row holds a byte that is no bool, with the low
//    bit of the bool. A failed row of a has an entry in err_a, of b in err_b.
// 2. Each operation, into an array of its own, into a, into b, with a, b
//    and out one array, and one flag array and one err, and with no flag_b.
// 3. The row's result, null, failed or a bool, by SQL's table: only a's
//    false, of AND, or true, of OR, drops b's error.
// 4. A row is not valid if it is not in a or not in b: it has no answer. The
//    other bits of a's flags stay, as b's flags do. Nothing is appended to
//    err; tyvec_err_merge then appends to a's entries those of the rows that
//    failed by b. Nothing is written past the n rows.
TEST_CASE("AND and OR of two vectors are SQL's") {
  enum { OWN, INTO_A, INTO_B, ONE, NO_FLAG_B, NPLACES };
  // SQL's answers, at [a][b], each 0, 1, null as 2, or failed as 3.
  static const uint8_t sql_and[4][4] = {
      {0, 0, 0, 0}, {0, 1, 2, 3}, {0, 2, 2, 3}, {3, 3, 3, 3}};
  static const uint8_t sql_or[4][4] = {
      {0, 1, 2, 3}, {1, 1, 1, 1}, {2, 1, 2, 3}, {3, 3, 3, 3}};
  for (uint32_t n : {0u, 1u, 15u, 16u, 17u, 31u, 33u, 256u, 8209u}) {
    for (bool is_and : {true, false}) {
      for (int place = OWN; place < NPLACES; place++) {
        // 1. Rows of each pair of flags with each pair of bools.
        std::vector<uint8_t> wfa(n + 1, 0xff), wfb = wfa;
        std::vector<uint8_t> wa(n + 1, 0xaa), wb = wa;
        std::vector<ty_rowerr_t> wea, web;
        for (uint32_t i = 0; i < n; i++) {
          wa[i] = uint8_t(i & 1);
          wb[i] = uint8_t((i >> 1) & 1);
          wfb[i] = uint8_t(((i >> 2) & TY_VEC_SKIP) | ((i * 7) & 0xf8));
          wfa[i] = uint8_t(i >> 5);
          if (wfa[i] & (TY_VEC_NULL | TY_VEC_ERROR)) {
            wa[i] |= uint8_t(0x54 + 2 * i);
          }
          if (wfb[i] & (TY_VEC_NULL | TY_VEC_ERROR)) {
            wb[i] |= uint8_t(0x72 + 2 * i);
          }
          if (wfa[i] & TY_VEC_ERROR) {
            wea.push_back({i, TY_ERR_RANGE});
          }
          if (wfb[i] & TY_VEC_ERROR) {
            web.push_back({i, TY_ERR_DIVZERO});
          }
        }
        if (ONE == place) {
          wb = wa;
          wfb = wfa;
          web = wea;
        }
        if (NO_FLAG_B == place) {
          // b has no flags: each of its bytes is a bool.
          std::fill(wfb.begin(), wfb.begin() + n, 0);
          for (uint32_t i = 0; i < n; i++) {
            wb[i] &= 1;
          }
          web.clear();
        }
        std::vector<uint8_t> fa = wfa, fb = wfb, a = wa, b = wb;
        std::vector<ty_rowerr_t> ea = wea, eb = web;
        std::vector<uint8_t> own(n + 1, 0xaa);
        const uint8_t *y = ONE == place ? a.data() : b.data();
        const uint8_t *g = ONE == place         ? fa.data()
                           : NO_FLAG_B == place ? nullptr
                                                : fb.data();
        const std::vector<ty_rowerr_t> &e2 = ONE == place ? ea : eb;
        uint8_t *out = OWN == place      ? own.data()
                       : INTO_B == place ? b.data()
                                         : a.data();
        // 2. The operation.
        REQUIRE(0 ==
                (is_and
                     ? tyvec_bool_and(n, fa.data(), a.data(), g, y, out, ea)
                     : tyvec_bool_or(n, fa.data(), a.data(), g, y, out, ea)));
        // They cannot fail: nothing is appended.
        CHECK(ea.size() == wea.size());
        // b's entries that the result keeps.
        REQUIRE(0 == tyvec_err_merge(n, fa.data(), ea, e2));
        // The first row whose answer, or flags, are not SQL's.
        int64_t bad_value = -1, bad_flag = -1;
        std::vector<ty_rowerr_t> want_err = wea;
        for (uint32_t i = 0; i < n; i++) {
          // 3. SQL's answer.
          const int p = wfa[i] & TY_VEC_ERROR  ? 3
                        : wfa[i] & TY_VEC_NULL ? 2
                                               : wa[i];
          const int q = wfb[i] & TY_VEC_ERROR  ? 3
                        : wfb[i] & TY_VEC_NULL ? 2
                                               : wb[i];
          const uint8_t want = is_and ? sql_and[p][q] : sql_or[p][q];
          const bool valid = !((wfa[i] | wfb[i]) & TY_VEC_NOTVALID);
          // An entry of b's whether the row is valid or not: one not valid
          // is never raised.
          if (3 == want && 3 != p) {
            want_err.push_back({i, TY_ERR_DIVZERO});
          }
          if (valid &&
              ((want < 2 && out[i] != want) ||
               (3 == want) != (0 != (fa[i] & TY_VEC_ERROR)) ||
               (3 > want && (2 == want) != (0 != (fa[i] & TY_VEC_NULL))))) {
            bad_value = bad_value < 0 ? i : bad_value;
          }
          // 4. The other bits of the flags.
          if ((fa[i] & TY_VEC_NOTVALID) != (valid ? 0 : TY_VEC_NOTVALID) ||
              (fa[i] & 0xf8) != (wfa[i] & 0xf8)) {
            bad_flag = bad_flag < 0 ? i : bad_flag;
          }
        }
        CAPTURE(n);
        CAPTURE(is_and);
        CAPTURE(place);
        CHECK(-1 == bad_value);
        CHECK(-1 == bad_flag);
        CHECK(fb == wfb);
        CHECK(0xff == fa[n]);
        CHECK(0xaa == out[n]);
        // err_a: its own entries, then b's of the rows b failed; in ONE,
        // the entries are a's alone, and stay.
        bool same_err = ea.size() == (ONE == place ? wea : want_err).size();
        const std::vector<ty_rowerr_t> &we = ONE == place ? wea : want_err;
        for (size_t k = 0; same_err && k < ea.size(); k++) {
          same_err = ea[k].row == we[k].row && ea[k].code == we[k].code;
        }
        CHECK(same_err);
        CHECK(eb.size() == web.size());
      }
    }
  }
}

// Verify that the errors of two inputs merge, a's first, and that the lowest
// valid failed row is raised with its own error.
// 1. a and b fail in some rows each, some of them the same; some rows are
//    not valid, and one of those failed.
// 2. The flags are ORed, as an operation does; tyvec_err_merge: err keeps
//    its entries and gains b's of the rows the result failed and err does
//    not name, not of a row whose mark was dropped; nothing with b's list
//    as its own.
// 3. tyvec_flag_error: the lowest failed row that is valid, with its code;
//    0 with no entry, with entries only of rows not valid, or past n.
TEST_CASE("the flags of two inputs merge, and the lowest error is raised") {
  const uint32_t n = 70;
  // 1. The failures.
  std::vector<uint8_t> fa(n, 0), fb(n, 0);
  std::vector<ty_rowerr_t> ea, eb;
  auto fail = [](std::vector<uint8_t> &f, std::vector<ty_rowerr_t> &e,
                 uint32_t row, ty_errnum_t code) {
    f[row] |= TY_VEC_ERROR;
    e.push_back({row, uint8_t(code)});
  };
  fail(fa, ea, 40, TY_ERR_RANGE);
  fail(fa, ea, 9, TY_ERR_RANGE);
  fail(fb, eb, 40, TY_ERR_DIVZERO);
  fail(fb, eb, 20, TY_ERR_DIVZERO);
  fail(fb, eb, 3, TY_ERR_UNDERFLOW);
  fail(fb, eb, 50, TY_ERR_DIVZERO);
  fb[3] |= TY_VEC_NOTVALID;
  fb[60] = TY_VEC_NULL;
  // In the rows past the whole vectors, a's bits must stay.
  fa[66] = TY_VEC_NULL;
  fa[67] = TY_VEC_NOTVALID, fb[67] = TY_VEC_NULL;
  // 2. Merge.
  std::vector<uint8_t> f = fa;
  std::vector<ty_rowerr_t> e = ea;
  ty_bitor_vec(n, f.data(), fb.data(), nullptr);
  for (uint32_t i = 0; i < n; i++) {
    CHECK(f[i] == (fa[i] | fb[i]));
  }
  // An AND whose a is false at row 50 dropped b's mark there.
  f[50] &= uint8_t(~TY_VEC_ERROR);
  REQUIRE(0 == tyvec_err_merge(n, f.data(), e, eb));
  std::vector<ty_rowerr_t> self = eb;
  REQUIRE(0 == tyvec_err_merge(n, f.data(), self, self));
  CHECK(self.size() == eb.size());
  // Row 40 failed in a, so b's entry for it is not taken.
  REQUIRE(4 == e.size());
  CHECK((40 == e[0].row && 9 == e[1].row && 20 == e[2].row && 3 == e[3].row));
  CHECK((TY_ERR_RANGE == e[0].code && TY_ERR_DIVZERO == e[2].code &&
         TY_ERR_UNDERFLOW == e[3].code));
  // 3. The lowest valid: row 9, of a; row 3 is not valid.
  CHECK(-1 == tyvec_flag_error(n, f.data(), e));
  CHECK(9u == ty_erridx());
  CHECK(TY_ERR_RANGE == ty_errnum());
  CHECK(std::string("result out of range") == ty_errmsg());
  f[9] |= TY_VEC_NOTVALID;
  CHECK(-1 == tyvec_flag_error(n, f.data(), e));
  CHECK(20u == ty_erridx());
  CHECK(TY_ERR_DIVZERO == ty_errnum());
  CHECK(0 == tyvec_flag_error(n, f.data(), {}));
  std::vector<ty_rowerr_t> only_3 = {{3, TY_ERR_UNDERFLOW}};
  CHECK(0 == tyvec_flag_error(n, f.data(), only_3));
  // An entry past the n rows is not read: rows 60 to 69 are past 60.
  std::vector<ty_rowerr_t> past = {{65, TY_ERR_RANGE}};
  f[65] = TY_VEC_ERROR;
  CHECK(0 == tyvec_flag_error(60, f.data(), past));
}

// Verify a filter that keeps a column's nulls, and one that drops them.
// 1. A column x with nulls; a is x IS NULL.
// 2. b is x > 3, whose flags are x's own, taken before.
// 3. x IS NULL OR x > 3 is true for a null and for 4, and false for 2.
// 4. x IS NOT NULL AND x > 3 is true for 4 alone, and null for no row.
TEST_CASE("x IS NULL OR x > 3, and x IS NOT NULL AND x > 3") {
  const uint32_t n = 40;
  // 1. A column x with nulls: 2, 4, null, 2, 4, null ...
  std::vector<int32_t> x(n), three(n, 3);
  std::vector<uint8_t> fx(n);
  for (uint32_t i = 0; i < n; i++) {
    x[i] = 0 == i % 3 ? 2 : 4;
    fx[i] = 2 == i % 3 ? TY_VEC_NULL : 0;
  }
  for (bool is_or : {true, false}) {
    std::vector<uint8_t> fa = fx, fb = fx, a(n), b(n);
    REQUIRE(0 == (is_or ? tyvec_flag_isnull(n, fa.data(), a.data())
                        : tyvec_flag_isnotnull(n, fa.data(), a.data())));
    // 2. b is x > 3.
    REQUIRE(0 ==
            tyvec_int32_gt(n, fb.data(), x.data(), three.data(), b.data()));
    // 3, 4. No row is null, and each is SQL's answer.
    std::vector<ty_rowerr_t> ea, eb;
    REQUIRE(0 == (is_or ? tyvec_bool_or(n, fa.data(), a.data(), fb.data(),
                                        b.data(), a.data(), ea)
                        : tyvec_bool_and(n, fa.data(), a.data(), fb.data(),
                                         b.data(), a.data(), ea)));
    for (uint32_t i = 0; i < n; i++) {
      CHECK(0 == fa[i]);
      CHECK(a[i] == (0 == i % 3 ? 0 : 1 == i % 3 ? 1 : is_or));
    }
  }
}

// Verify that ty_bitor_vec ORs b and c into a, with c or without, and with
// b or c the same array as a.
// 1. For sizes around 16 bytes, bytes of each bit pattern, and one more
//    byte past them.
// 2. a |= b | c, and a |= b with no c; b as a; c as a.
// 3. Nothing is written past the n bytes.
TEST_CASE("ty_bitor_vec ORs b, and c if there is one, into a") {
  for (uint32_t n : {0u, 1u, 15u, 16u, 17u, 31u, 33u, 300u}) {
    // 1. The bytes.
    std::vector<uint8_t> a(n + 1, 0x11), b(n + 1, 0xff), c(n + 1, 0xff);
    for (uint32_t i = 0; i < n; i++) {
      a[i] = uint8_t(i * 37), b[i] = uint8_t(i * 11 + 1), c[i] = uint8_t(i * 5);
    }
    // 2. Each form.
    struct form_t {
      bool with_c, b_is_a, c_is_a;
    };
    for (form_t fm : {form_t{true, false, false}, form_t{false, false, false},
                      form_t{false, true, false}, form_t{true, false, true}}) {
      std::vector<uint8_t> x = a;
      const uint8_t *y = fm.b_is_a ? x.data() : b.data();
      const uint8_t *z = fm.c_is_a ? x.data() : c.data();
      ty_bitor_vec(n, x.data(), y, fm.with_c ? z : nullptr);
      uint32_t bad = n;
      for (uint32_t i = 0; i < n; i++) {
        const uint8_t yb = fm.b_is_a ? a[i] : b[i];
        const uint8_t zb = !fm.with_c ? 0 : fm.c_is_a ? a[i] : c[i];
        if (x[i] != uint8_t(a[i] | yb | zb)) {
          bad = std::min(bad, i);
        }
      }
      CHECK(bad == n);
      // 3. Past the n bytes.
      CHECK(0x11 == x[n]);
    }
  }
}

// Verify that tyvec_print writes a line a row: its index, then its value as
// PostgreSQL writes it, or what the row is.
// 1. A vector of int32 with a row of each kind: a value, null, failed with
//    an entry, failed with none, not valid and failed, and the edges.
// 2. The same with no flags, and with no row.
// 3. A float, a double and a bool: the fewest digits, NaN, -0, t and f.
// 4. It appends: what out held stays.
// 5. A value is ty_print's: each type's own text.
TEST_CASE("tyvec_print writes a line a row") {
  // 1. Each kind of row.
  const int32_t v[] = {3, 9, 7, 8, 5, INT32_MIN, INT32_MAX};
  const uint8_t f[] = {0,
                       TY_VEC_NULL,
                       TY_VEC_ERROR,
                       TY_VEC_ERROR | TY_VEC_NULL,
                       TY_VEC_NOTVALID | TY_VEC_ERROR,
                       0,
                       0};
  const std::vector<ty_rowerr_t> err = {{2, TY_ERR_DIVZERO}, {2, TY_ERR_RANGE}};
  std::string out;
  tyvec_print(7, f, v, out, err);
  CHECK(out == "0 3\n1 null\n2 error TY_ERR_DIVZERO\n3 error ?\n4 notvalid\n"
               "5 -2147483648\n6 2147483647\n");
  // 2. No flags, and no row.
  out.clear();
  tyvec_print(2, nullptr, v, out, {});
  CHECK(out == "0 3\n1 9\n");
  out.clear();
  tyvec_print(0, f, v, out, err);
  CHECK(out.empty());
  // 3. The other types.
  const float x[] = {0.1f, -0.0f, std::numeric_limits<float>::quiet_NaN()};
  const double y[] = {0.1, 1e300, -std::numeric_limits<double>::infinity()};
  const int16_t h[] = {-32768};
  const int64_t w[] = {INT64_MIN};
  const uint8_t b[] = {1, 0};
  out.clear();
  tyvec_print(3, nullptr, x, out, {});
  tyvec_print(3, nullptr, y, out, {});
  tyvec_print(1, nullptr, h, out, {});
  tyvec_print(1, nullptr, w, out, {});
  tyvec_print(2, nullptr, b, out, {});
  CHECK(out == "0 0.1\n1 -0\n2 NaN\n0 0.1\n1 1e+300\n2 -Infinity\n"
               "0 -32768\n0 -9223372036854775808\n0 t\n1 f\n");
  // 4. It appends.
  out = "before\n";
  tyvec_print(1, nullptr, v, out, {});
  CHECK(out == "before\n0 3\n");
  // 5. ty_print alone.
  out.clear();
  ty_print(int16_t(-7), out), out += ' ';
  ty_print(int64_t(1) << 40, out), out += ' ';
  ty_print(1.5f, out), out += ' ';
  ty_print(std::numeric_limits<double>::quiet_NaN(), out), out += ' ';
  ty_print(false, out);
  CHECK(out == "-7 1099511627776 1.5 NaN f");
}

namespace {

/*
 * PostgreSQL's order of floats (float4_eq to float8_ge in its
 * src/include/utils/float.h): every NaN equals every other, and is above
 * every number; -0 equals 0; else the numbers' own order. -1, 0 or 1.
 */
template <typename T> int pg_float_cmp(T a, T b) {
  const bool na = a != a, nb = b != b;
  if (na || nb) {
    return int(na) - int(nb);
  }
  return (a > b) - (a < b);
}

/* The bits of u as a float of its width. */
template <typename T, typename U> T float_of(U u) {
  T x;
  memcpy(&x, &u, sizeof x);
  return x;
}

/*
 * Check the six compares of T, vector and scalar, against PostgreSQL's
 * order on a and b; returns the rows that differ.
 */
template <typename T>
uint64_t check_pg_cmp(const std::vector<cmp_fns_t<T>> &fns,
                      const std::vector<T> &a, const std::vector<T> &b) {
  const uint32_t n = uint32_t(a.size());
  std::vector<uint8_t> f(n, 0), out(n);
  uint64_t bad = 0;
  for (const cmp_fns_t<T> &fn : fns) {
    REQUIRE(0 == fn.vec(n, f.data(), a.data(), b.data(), out.data()));
    const std::string name = fn.name;
    for (uint32_t i = 0; i < n; i++) {
      const int c = pg_float_cmp(a[i], b[i]);
      const bool want = name.ends_with("eq")   ? 0 == c
                        : name.ends_with("ne") ? 0 != c
                        : name.ends_with("lt") ? c < 0
                        : name.ends_with("le") ? c <= 0
                        : name.ends_with("gt") ? c > 0
                                               : c >= 0;
      bad += out[i] != want;
      bad += (0 != fn.one(a[i], b[i])) != want;
    }
  }
  return bad;
}

/*
 * The compares of T are PostgreSQL's, on pairs of special values at each
 * place of two 16 rows and their rest, and on random bits.
 */
template <typename T, typename U>
void check_pg_order(const std::vector<cmp_fns_t<T>> &fns) {
  typedef std::numeric_limits<T> lim;
  const T list[] = {0,
                    T(-0.0),
                    1,
                    -1,
                    lim::infinity(),
                    -lim::infinity(),
                    lim::quiet_NaN(),
                    -lim::quiet_NaN(),
                    lim::signaling_NaN(),
                    float_of<T>(U(-1)),
                    lim::max(),
                    lim::lowest(),
                    lim::min(),
                    lim::denorm_min(),
                    -lim::denorm_min()};
  uint64_t bad = 0;
  // 1. Each pair at each place.
  for (T x : list) {
    for (T y : list) {
      for (uint32_t at = 0; at < 37; at += 5) {
        std::vector<T> a(37, T(2)), b(37, T(3));
        a[at] = x, b[at] = y;
        bad += check_pg_cmp(fns, a, b);
      }
    }
  }
  // 2. Random bits, a NaN's payloads and denormals among them; a row in
  //    seven equal.
  std::mt19937_64 rng(41);
  std::vector<T> a(1 << 16), b(1 << 16);
  for (size_t i = 0; i < a.size(); i++) {
    a[i] = float_of<T>(U(rng()));
    b[i] = 0 == i % 7 ? a[i] : float_of<T>(U(rng()));
  }
  bad += check_pg_cmp(fns, a, b);
  CHECK(0 == bad);
}

} // namespace

// Verify that the compares of floats, vector and scalar, are PostgreSQL's:
// a NaN equals a NaN, of any payload or sign, and is above every number; -0
// equals 0; infinities and denormals as numbers.
// 1. Each pair of special values, at places of two 16 rows and the rest.
// 2. Random bits.
TEST_CASE("a float compare is PostgreSQL's") {
  check_pg_order<float, uint32_t>(CMP_ROWS(fp32, float));
  check_pg_order<double, uint64_t>(CMP_ROWS(fp64, double));
}

namespace {

/*
 * Check tyvec_squeeze on vectors of T: each row's value its index, so a
 * kept row is known by its value.
 * 1. For each size around 16 rows, and each pattern: none dropped, all,
 *    every other, runs of 16 dropped and kept, each 16's first half or
 *    second half dropped, and random; each flag's other bits set in some
 *    rows.
 * 2. The count is that of the rows kept; the front of vec and flag is the
 *    kept rows' values and flags, in their order.
 */
template <typename T> void check_squeeze() {
  std::mt19937_64 rng(53);
  for (uint32_t n : {0u, 1u, 15u, 16u, 17u, 31u, 32u, 33u, 100u, 1000u}) {
    for (int pattern = 0; pattern < 8; pattern++) {
      // 1. The rows.
      std::vector<T> vec(n);
      std::vector<uint8_t> flag(n);
      for (uint32_t i = 0; i < n; i++) {
        vec[i] = T(i % 100);
        const bool drop = 0 == pattern   ? false
                          : 1 == pattern ? true
                          : 2 == pattern ? 1 == i % 2
                          : 3 == pattern ? 1 == (i / 16) % 2
                          : 4 == pattern ? 0 == (i / 16) % 3
                          : 5 == pattern ? i % 16 < 8
                          : 6 == pattern ? i % 16 >= 8
                                         : 0 == rng() % 3;
        flag[i] = uint8_t((drop ? TY_VEC_NOTVALID : 0) | (rng() % 7 & ~1));
      }
      std::vector<T> want_vec;
      std::vector<uint8_t> want_flag;
      for (uint32_t i = 0; i < n; i++) {
        if (0 == (flag[i] & TY_VEC_NOTVALID)) {
          want_vec.push_back(vec[i]);
          want_flag.push_back(flag[i]);
        }
      }
      // 2. The rows kept.
      CAPTURE(n);
      CAPTURE(pattern);
      const int m = tyvec_squeeze(n, flag.data(), vec.data());
      REQUIRE(m == int(want_vec.size()));
      vec.resize(m), flag.resize(m);
      CHECK(vec == want_vec);
      CHECK(flag == want_flag);
    }
  }
}

} // namespace

// Verify that tyvec_squeeze drops the rows not valid, and keeps the others,
// null and failed ones too, in their order, for each type.
TEST_CASE("tyvec_squeeze drops the rows not valid") {
  check_squeeze<int16_t>();
  check_squeeze<int32_t>();
  check_squeeze<int64_t>();
  check_squeeze<float>();
  check_squeeze<double>();
  check_squeeze<uint8_t>();
}

TEST_CASE("a vector add agrees with the scalar add, row by row") {
  check_agrees<int16_t>();
  check_agrees<int32_t>();
  check_agrees<int64_t>();
}

TEST_CASE("a vector add fails at the lowest row computed that overflows") {
  check_overflow<int16_t>();
  check_overflow<int32_t>();
  check_overflow<int64_t>();
}

TEST_CASE("a vector add writes its results over its inputs") {
  check_in_place<int16_t>();
  check_in_place<int32_t>();
  check_in_place<int64_t>();
}

TEST_CASE("a vector add over its input fails at the right row") {
  check_overflow_in_place<int16_t>();
  check_overflow_in_place<int32_t>();
  check_overflow_in_place<int64_t>();
}

TEST_CASE("a vector add, sub, mul, div and mod agree with the scalar's") {
  check_random<add_op, int16_t>();
  check_random<add_op, int32_t>();
  check_random<add_op, int64_t>();
  check_random<sub_op, int16_t>();
  check_random<sub_op, int32_t>();
  check_random<sub_op, int64_t>();
  check_random<mul_op, int16_t>();
  check_random<mul_op, int32_t>();
  check_random<mul_op, int64_t>();
  check_random<div_op, int16_t>();
  check_random<mod_op, int16_t>();
  check_random<div_op, int32_t>();
  check_random<mod_op, int32_t>();
  check_random<div_op, int64_t>();
  check_random<mod_op, int64_t>();
}

TEST_CASE("a vector add, sub, mul, div and mod at their edges") {
  check_edges<int16_t>();
  check_edges<int32_t>();
  check_edges<int64_t>();
}

TEST_CASE("a vector div fails with the error of its lowest failing row") {
  check_div_errors<int16_t>();
  check_div_errors<int32_t>();
  check_div_errors<int64_t>();
}

TEST_CASE("a vector div and mod are exact around a multiple of the divisor") {
  check_div_exact<div_op, int16_t>();
  check_div_exact<mod_op, int16_t>();
  check_div_exact<div_op, int32_t>();
  check_div_exact<mod_op, int32_t>();
  check_div_exact<div_op, int64_t>();
  check_div_exact<mod_op, int64_t>();
}

TEST_CASE("a vector add, sub, mul and div of floats agree with the scalar's") {
  check_fp<fp_add_op, float>();
  check_fp_random<fp_add_op, float, uint32_t>();
  check_fp<fp_add_op, double>();
  check_fp_random<fp_add_op, double, uint64_t>();
  check_fp<fp_sub_op, float>();
  check_fp_random<fp_sub_op, float, uint32_t>();
  check_fp<fp_sub_op, double>();
  check_fp_random<fp_sub_op, double, uint64_t>();
  check_fp<fp_mul_op, float>();
  check_fp_random<fp_mul_op, float, uint32_t>();
  check_fp<fp_mul_op, double>();
  check_fp_random<fp_mul_op, double, uint64_t>();
  check_fp<fp_div_op, float>();
  check_fp_random<fp_div_op, float, uint32_t>();
  check_fp<fp_div_op, double>();
  check_fp_random<fp_div_op, double, uint64_t>();
}

TEST_CASE("a vector compare agrees with the scalar's") {
  check_cmp<int16_t>(CMP_ROWS(int16, int16_t), int_list<int16_t>());
  check_cmp<int32_t>(CMP_ROWS(int32, int32_t), int_list<int32_t>());
  check_cmp<int64_t>(CMP_ROWS(int64, int64_t), int_list<int64_t>());
  check_cmp<float>(CMP_ROWS(fp32, float), fp_list<float>());
  check_cmp<double>(CMP_ROWS(fp64, double), fp_list<double>());
  check_cmp<uint8_t>(CMP_ROWS(bool, uint8_t), {0, 1});
}

// Verify that a compare of bools may write its answer over an input.
// 1. Rows of each pair of bools.
// 2. Each compare, over a, and over b.
// 3. The answers are the scalar's.
TEST_CASE("a vector compare of bools over its input") {
  const std::vector<cmp_fns_t<uint8_t>> fns = CMP_ROWS(bool, uint8_t);
  const uint32_t n = 77;
  for (const cmp_fns_t<uint8_t> &fn : fns) {
    for (bool over_a : {true, false}) {
      // 1. Rows of each pair.
      std::vector<uint8_t> a(n), b(n), flag(n, 0);
      for (uint32_t i = 0; i < n; i++) {
        a[i] = uint8_t(i & 1);
        b[i] = uint8_t((i >> 1) & 1);
      }
      const std::vector<uint8_t> x = a, y = b;
      // 2. The compare.
      std::vector<uint8_t> &out = over_a ? a : b;
      REQUIRE(0 == fn.vec(n, flag.data(), a.data(), b.data(), out.data()));
      // 3. The answers.
      for (uint32_t i = 0; i < n; i++) {
        CHECK(out[i] == fn.one(x[i], y[i]));
      }
    }
  }
}

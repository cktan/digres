/*
 * Copyright (c) 2026 CK Tan
 * SPDX-License-Identifier: MIT
 */

/*
 * test_vec.cc -- the vector operations of ty_vec.h, each row against the
 * scalar operation of ty_int.h.
 */
#include "doctest.h"
#include "ty_error.h"
#include "ty_int.h"
#include "ty_vec.h"

#include <cstdint>
#include <limits>
#include <random>
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

/* The vector add of each width. */
int vec_add(uint32_t n, const uint8_t *f, const int16_t *a, const int16_t *b,
            int16_t *out, uint8_t *of, uint32_t *bad) {
  return ty_int16_add_vec(n, f, a, b, out, of, bad);
}
int vec_add(uint32_t n, const uint8_t *f, const int32_t *a, const int32_t *b,
            int32_t *out, uint8_t *of, uint32_t *bad) {
  return ty_int32_add_vec(n, f, a, b, out, of, bad);
}
int vec_add(uint32_t n, const uint8_t *f, const int64_t *a, const int64_t *b,
            int64_t *out, uint8_t *of, uint32_t *bad) {
  return ty_int64_add_vec(n, f, a, b, out, of, bad);
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
 *    and outflag is inflag.
 */
template <typename T> void check_agrees() {
  std::mt19937_64 rng(7);
  for (uint32_t n : {0u, 1u, 3u, 7u, 8u, 9u, 255u, 256u, 257u, 1000u, 10000u}) {
    cols_t<T> c = random_cols<T>(rng, n, 5);
    std::vector<T> out(n);
    std::vector<uint8_t> of(n, 0xff);
    uint32_t bad = UINT32_MAX;
    REQUIRE(0 == vec_add(n, c.flag.data(), c.a.data(), c.b.data(), out.data(),
                         of.data(), &bad));
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
 *    add fails with RANGE, bad = 40.
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
  std::vector<uint8_t> of(n);
  uint32_t bad = UINT32_MAX;
  // 1. Three overflows, the lowest in a row not valid.
  cols_t<T> d = c;
  d.a[600] = max, d.b[600] = 1;
  d.a[40] = min, d.b[40] = -1;
  d.a[5] = max, d.b[5] = max, d.flag[5] = TY_VEC_NOTVALID;
  d.a[7] = max, d.b[7] = max, d.flag[7] = TY_VEC_NULL;
  CHECK(-1 == vec_add(n, d.flag.data(), d.a.data(), d.b.data(), out.data(),
                      of.data(), &bad));
  CHECK(TY_ERR_RANGE == ty_errnum());
  CHECK(40u == bad);
  // 2. Only the invalid row and the null row.
  cols_t<T> e = c;
  e.a[5] = max, e.b[5] = max, e.flag[5] = TY_VEC_NOTVALID;
  e.a[7] = max, e.b[7] = max, e.flag[7] = TY_VEC_NULL;
  bad = UINT32_MAX;
  CHECK(0 == vec_add(n, e.flag.data(), e.a.data(), e.b.data(), out.data(),
                     of.data(), &bad));
  CHECK(UINT32_MAX == bad);
  // 3. The last row, and one before it.
  cols_t<T> g = c;
  g.a[n - 1] = max, g.b[n - 1] = max;
  CHECK(-1 == vec_add(n, g.flag.data(), g.a.data(), g.b.data(), out.data(),
                      of.data(), &bad));
  CHECK(n - 1 == bad);
  g.a[n - 2] = min, g.b[n - 2] = -1;
  CHECK(-1 == vec_add(n, g.flag.data(), g.a.data(), g.b.data(), out.data(),
                      of.data(), &bad));
  CHECK(n - 2 == bad);
  // 4. The edges.
  T a4[3] = {max, min, max}, b4[3] = {0, max, 1}, o4[3];
  uint8_t f4[3] = {0, 0, 0}, of4[3];
  CHECK(-1 == vec_add(3, f4, a4, b4, o4, of4, &bad));
  CHECK(2u == bad);
  CHECK(0 == vec_add(2, f4, a4, b4, o4, of4, &bad));
  CHECK(o4[0] == max);
  CHECK(o4[1] == T(-1));
}

/*
 * The results may be written over an input: out is a, and outflag is
 * inflag.
 * 1. Random rows; the add with out = a and outflag = inflag.
 * 2. a holds the sums, and the flags are as they were.
 */
template <typename T> void check_in_place() {
  std::mt19937_64 rng(13);
  const uint32_t n = 777;
  cols_t<T> c = random_cols<T>(rng, n, 3);
  cols_t<T> was = c;
  uint32_t bad = UINT32_MAX;
  REQUIRE(0 == vec_add(n, c.flag.data(), c.a.data(), c.b.data(), c.a.data(),
                       c.flag.data(), &bad));
  for (uint32_t i = 0; i < n; i++) {
    CHECK(c.flag[i] == was.flag[i]);
    if (0 == (was.flag[i] & TY_VEC_SKIP)) {
      CHECK(c.a[i] == T(was.a[i] + was.b[i]));
    }
  }
}

} // namespace

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

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
int vec_add(uint32_t n, uint8_t *f, const int16_t *a, const int16_t *b,
            int16_t *out) {
  return tyvec_int16_add(n, f, a, b, out);
}
int vec_add(uint32_t n, uint8_t *f, const int32_t *a, const int32_t *b,
            int32_t *out) {
  return tyvec_int32_add(n, f, a, b, out);
}
int vec_add(uint32_t n, uint8_t *f, const int64_t *a, const int64_t *b,
            int64_t *out) {
  return tyvec_int64_add(n, f, a, b, out);
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
// 3. out is SQL's answer for each valid row, and a bool; NOT has none for a
//    null row.
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
          // 3. SQL's answer.
          if (NOTNULL == op) {
            CHECK(out[i] == !null);
          } else if (NOT == op) {
            CHECK((!valid || null || out[i] == !vec[i]));
          } else if (valid) {
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

// Verify that AND and OR of two vectors are right in each computed row, and
// change no flag.
// 1. For each size around the 16 bytes of a vector instruction, rows of
//    each flag with each pair of bools. A row not computed holds bytes that
//    are no bool.
// 2. Each operation, into an array of its own, into a, into b, and with a, b
//    and out one array.
// 3. out is a AND b, or a OR b, of each computed row.
// 4. The flags are as they were, and nothing is written past the n rows.
TEST_CASE("AND and OR of two vectors") {
  enum { OWN, INTO_A, INTO_B, ONE, NPLACES };
  for (uint32_t n : {0u, 1u, 15u, 16u, 17u, 31u, 33u, 256u, 2003u}) {
    for (bool is_and : {true, false}) {
      for (int place = OWN; place < NPLACES; place++) {
        // 1. Rows of each flag with each pair of bools.
        std::vector<uint8_t> was(n + 1, 0xff), wa(n + 1, 0xaa), wb = wa;
        for (uint32_t i = 0; i < n; i++) {
          const bool skip = (i / 4) & TY_VEC_SKIP;
          was[i] = uint8_t(i / 4);
          wa[i] = skip ? uint8_t(0x54 + i) : uint8_t(i & 1);
          wb[i] = skip ? uint8_t(0x73 + i) : uint8_t((i >> 1) & 1);
        }
        if (ONE == place) {
          wb = wa;
        }
        std::vector<uint8_t> flag = was, a = wa, b = wb, own(n + 1, 0xaa);
        const uint8_t *y = ONE == place ? a.data() : b.data();
        uint8_t *out = OWN == place      ? own.data()
                       : INTO_B == place ? b.data()
                                         : a.data();
        // 2. The operation.
        REQUIRE(0 == (is_and
                          ? tyvec_bool_and(n, flag.data(), a.data(), y, out)
                          : tyvec_bool_or(n, flag.data(), a.data(), y, out)));
        // 3. Each computed row.
        for (uint32_t i = 0; i < n; i++) {
          if (0 == (was[i] & TY_VEC_SKIP)) {
            CHECK(out[i] == (is_and ? wa[i] && wb[i] : wa[i] || wb[i]));
          }
        }
        // 4. The flags, and past the n rows.
        CHECK(flag == was);
        CHECK(0xaa == out[n]);
      }
    }
  }
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

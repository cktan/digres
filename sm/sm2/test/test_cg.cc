/*
 * Copyright (c) 2026 CK Tan
 * SPDX-License-Identifier: MIT
 */

/*
 * test_cg.cc -- the Column Store's column group (src/col/cg.cc,
 * ../COLSTORE.md §5, §6): encodings, the file, the zonemap, the
 * filter, and the pruning test.
 */

#include "doctest.h"

#include "sm2.h"

#include <random>
#include <string>
#include <vector>

namespace {

/* A column of type t, with no collation. */
sm2::cg_column_t column(const sm2::type_t &t) {
  return sm2::cg_column_t{&t, -1, nullptr};
}

/* The datum of t's canonical text s. */
std::string datum(const sm2::type_t &t, std::string_view s) {
  std::string d;
  REQUIRE(t.in(s, -1, d) == 0);
  return d;
}

/* The key of t's canonical text s. */
std::string key(const sm2::type_t &t, std::string_view s) {
  std::string k;
  REQUIRE(t.key_encode(datum(t, s), -1, nullptr, k) == 0);
  return k;
}

/* Values from datums, with nulls where null[i] is set. */
std::vector<sm2::cg_value_t> values(const std::vector<std::string> &d,
                                    const std::vector<bool> &null = {}) {
  std::vector<sm2::cg_value_t> v;
  for (size_t i = 0; i < d.size(); i++) {
    v.push_back({d[i], i < null.size() && null[i]});
  }
  return v;
}

/* Build a CG, open it, and check every row reads back. */
sm2::cg_built_t round_trip(const sm2::cg_column_t &col,
                           const std::vector<sm2::cg_value_t> &v) {
  sm2::cg_built_t b;
  REQUIRE(sm2::cg_build(col, v, b) == 0);
  sm2::cg_t *cg = nullptr;
  REQUIRE(sm2::cg_open(b.file, cg) == 0);
  REQUIRE(sm2::cg_rows(cg) == v.size());
  for (uint32_t i = 0; i < v.size(); i++) {
    std::string_view d;
    sm2::cg_long_t w;
    int r = sm2::cg_get(cg, i, d, w);
    if (v[i].isnull) {
      CHECK(r == 1);
    } else if (r == 2) {
      std::string_view bytes(b.long_file.data() + w.offset, w.length);
      CHECK(sm2::cg_long_check(bytes, w) == 0);
      CHECK(bytes == v[i].datum);
    } else {
      CHECK(r == 0);
      CHECK(d == v[i].datum);
    }
  }
  sm2::cg_close(cg);
  return b;
}

/* An equality bound on key k. */
sm2::cg_bound_t eq(const std::string &k) {
  sm2::cg_bound_t b;
  b.has_lo = b.has_hi = true;
  b.lo = b.hi = k;
  return b;
}

} // namespace

// Verify that each encoding reads back what was built.
// 1. Sorted int8 with no null: delta.
// 2. Repeated text: a dictionary.
// 3. Distinct text with nulls: plain.
// 4. A text longer than 1024 bytes: out of line, in the long file.
TEST_CASE("a CG reads back in each encoding") {
  // 1. Sorted integers.
  std::vector<std::string> ints;
  for (int i = 0; i < 1000; i++) {
    ints.push_back(datum(sm2::type_int8, std::to_string(1000 + i * 3)));
  }
  sm2::cg_built_t b = round_trip(column(sm2::type_int8), values(ints));
  CHECK(b.stats.sorted);
  CHECK(b.stats.distinct == 1000);
  // 2. Repeated text.
  std::vector<std::string> kinds;
  for (int i = 0; i < 1000; i++) {
    kinds.push_back(datum(sm2::type_text, i % 3 ? "click" : "view"));
  }
  b = round_trip(column(sm2::type_text), values(kinds));
  CHECK(b.stats.distinct == 2);
  // 3. Distinct text, every tenth null.
  std::vector<std::string> names;
  std::vector<bool> null;
  for (int i = 0; i < 500; i++) {
    names.push_back(datum(sm2::type_text, "name" + std::to_string(i)));
    null.push_back(i % 10 == 0);
  }
  b = round_trip(column(sm2::type_text), values(names, null));
  CHECK(b.stats.nulls == 50);
  // 4. A long value.
  std::vector<std::string> docs = {datum(sm2::type_text, "short"),
                                   std::string(5000, 'x')};
  b = round_trip(column(sm2::type_text), values(docs));
  CHECK(b.stats.has_long);
  CHECK(b.long_file.size() == 5000);
}

// Verify that a changed byte of a CG file is found by its CRC32C.
// 1. Build a CG.
// 2. Change one byte of its body; opening fails with SM_ERR_CORRUPT.
TEST_CASE("a CG file with a changed byte is corrupt") {
  // 1. Build.
  std::vector<std::string> d = {datum(sm2::type_int4, "1"),
                                datum(sm2::type_int4, "7")};
  sm2::cg_built_t b;
  REQUIRE(sm2::cg_build(column(sm2::type_int4), values(d), b) == 0);
  // 2. Change a byte.
  b.file[0] = static_cast<char>(b.file[0] ^ 0x40);
  sm2::cg_t *cg = nullptr;
  CHECK(sm2::cg_open(b.file, cg) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_CORRUPT);
}

// Verify the zonemap: min and max, NaN and infinity, cut min and max.
// 1. float8 with NaN and both infinities: min and max are finite.
// 2. Long keys are cut at 32 bytes, with the cut flags.
// 3. The statistics round trip through their encoding.
TEST_CASE("a CG's zonemap keeps NaN and infinity in flags") {
  // 1. Floats.
  std::vector<std::string> f = {
      datum(sm2::type_float8, "2.5"), datum(sm2::type_float8, "NaN"),
      datum(sm2::type_float8, "-Infinity"), datum(sm2::type_float8, "-1"),
      datum(sm2::type_float8, "Infinity")};
  sm2::cg_built_t b = round_trip(column(sm2::type_float8), values(f));
  CHECK(b.stats.has_nan);
  CHECK(b.stats.has_pos_inf);
  CHECK(b.stats.has_neg_inf);
  CHECK(b.stats.min == key(sm2::type_float8, "-1"));
  CHECK(b.stats.max == key(sm2::type_float8, "2.5"));
  // 2. Long keys.
  std::vector<std::string> t = {datum(sm2::type_text, std::string(100, 'a')),
                                datum(sm2::type_text, std::string(100, 'b'))};
  b = round_trip(column(sm2::type_text), values(t));
  CHECK(b.stats.min_cut);
  CHECK(b.stats.max_cut);
  CHECK(b.stats.min.size() == sm2::cg_minmax_max);
  // 3. The encoding.
  std::string bytes;
  sm2::cg_stats_encode(b.stats, bytes);
  std::string_view in = bytes;
  sm2::cg_stats_t st;
  REQUIRE(sm2::cg_stats_decode(in, st) == 0);
  CHECK(in.empty());
  CHECK(st.min == b.stats.min);
  CHECK(st.max_cut == b.stats.max_cut);
  CHECK(st.rows == 2);
}

// Verify the pruning test on ranges, equality, IN and nulls.
// 1. A CG of int4 10 to 20: ranges inside and outside it.
// 2. Equality: the exact set rules out a value in range.
// 3. IS NULL and IS NOT NULL.
// 4. A float CG with +Infinity is not skipped for x > 1e300.
TEST_CASE("pruning skips only CGs that cannot match") {
  auto k = [](int v) { return key(sm2::type_int4, std::to_string(v)); };
  // 1. Values 10, 12, ..., 20.
  std::vector<std::string> d;
  for (int v = 10; v <= 20; v += 2) {
    d.push_back(datum(sm2::type_int4, std::to_string(v)));
  }
  sm2::cg_column_t col = column(sm2::type_int4);
  sm2::cg_built_t b;
  REQUIRE(sm2::cg_build(col, values(d), b) == 0);
  sm2::cg_bound_t gt;
  gt.has_lo = true;
  gt.lo_incl = false;
  gt.lo = k(20);
  CHECK_FALSE(sm2::cg_may_match(col, b.stats, b.filter, gt));
  gt.lo = k(19);
  CHECK(sm2::cg_may_match(col, b.stats, b.filter, gt));
  sm2::cg_bound_t lt;
  lt.has_hi = true;
  lt.hi_incl = false;
  lt.hi = k(10);
  CHECK_FALSE(sm2::cg_may_match(col, b.stats, b.filter, lt));
  // 2. Equality: 13 is in range, but not in the set.
  CHECK_FALSE(sm2::cg_may_match(col, b.stats, b.filter, eq(k(13))));
  CHECK(sm2::cg_may_match(col, b.stats, b.filter, eq(k(14))));
  sm2::cg_bound_t in;
  in.has_in = true;
  in.in = {k(3), k(13)};
  CHECK_FALSE(sm2::cg_may_match(col, b.stats, b.filter, in));
  in.in.push_back(k(16));
  CHECK(sm2::cg_may_match(col, b.stats, b.filter, in));
  // 3. Nulls.
  sm2::cg_bound_t isnull;
  isnull.is_null = true;
  CHECK_FALSE(sm2::cg_may_match(col, b.stats, b.filter, isnull));
  sm2::cg_built_t allnull;
  std::vector<std::string> nd = {"", ""};
  REQUIRE(sm2::cg_build(col, values(nd, {true, true}), allnull) == 0);
  CHECK(sm2::cg_may_match(col, allnull.stats, allnull.filter, isnull));
  sm2::cg_bound_t notnull;
  notnull.not_null = true;
  CHECK_FALSE(sm2::cg_may_match(col, allnull.stats, allnull.filter, notnull));
  // 4. Infinity above every finite value.
  sm2::cg_column_t fcol = column(sm2::type_float8);
  std::vector<std::string> f = {datum(sm2::type_float8, "1"),
                                datum(sm2::type_float8, "Infinity")};
  sm2::cg_built_t fb;
  REQUIRE(sm2::cg_build(fcol, values(f), fb) == 0);
  sm2::cg_bound_t big;
  big.has_lo = true;
  big.lo_incl = false;
  big.lo = key(sm2::type_float8, "1e+300");
  CHECK(sm2::cg_may_match(fcol, fb.stats, fb.filter, big));
}

// Verify that pruning never skips a CG holding a match.
// 1. Build random int4 CGs, some with nulls.
// 2. For random ranges, equalities and IN lists, compare the
//    pruning test with a test of every value: a skip must mean no
//    value matches.
TEST_CASE("pruning never skips a match") {
  std::mt19937 rng(7);
  sm2::cg_column_t col = column(sm2::type_int4);
  auto k = [](int v) { return key(sm2::type_int4, std::to_string(v)); };
  for (int round = 0; round < 200; round++) {
    // 1. A random CG.
    int n = 1 + static_cast<int>(rng() % 50);
    int spread = 1 + static_cast<int>(rng() % 1000);
    std::vector<int> v(n);
    std::vector<std::string> d(n);
    std::vector<bool> null(n);
    for (int i = 0; i < n; i++) {
      v[i] = static_cast<int>(rng() % spread) - spread / 2;
      d[i] = datum(sm2::type_int4, std::to_string(v[i]));
      null[i] = rng() % 7 == 0;
    }
    sm2::cg_built_t b;
    REQUIRE(sm2::cg_build(col, values(d, null), b) == 0);
    // 2. Random bounds against every value.
    for (int q = 0; q < 20; q++) {
      int lo = static_cast<int>(rng() % spread) - spread / 2;
      int hi = lo + static_cast<int>(rng() % 50);
      sm2::cg_bound_t bd;
      bool match = false;
      if (q % 3 == 0) {
        bd = eq(k(lo));
        for (int i = 0; i < n; i++) {
          match = match || (!null[i] && v[i] == lo);
        }
      } else if (q % 3 == 1) {
        bd.has_lo = bd.has_hi = true;
        bd.lo = k(lo);
        bd.hi = k(hi);
        for (int i = 0; i < n; i++) {
          match = match || (!null[i] && v[i] >= lo && v[i] <= hi);
        }
      } else {
        bd.has_in = true;
        bd.in = {k(lo), k(hi)};
        for (int i = 0; i < n; i++) {
          match = match || (!null[i] && (v[i] == lo || v[i] == hi));
        }
      }
      if (match) {
        CHECK(sm2::cg_may_match(col, b.stats, b.filter, bd));
      }
    }
  }
}

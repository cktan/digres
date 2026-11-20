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

#include <cstdio>
#include <random>
#include <string>
#include <vector>

namespace {

/* A column of type t, with no collation. */
sm2::cg_column_t column(const sm2::type_t &t) {
  return sm2::cg_column_t{&t, -1, nullptr};
}

/* The bytes of t's canonical text s, as a type function takes them. */
std::string payload(const sm2::type_t &t, std::string_view s) {
  std::string d;
  REQUIRE(t.in(s, -1, d) == 0);
  return d;
}

/* The datum of t's canonical text s, as a CG takes it: a varlena whole. */
std::string datum(const sm2::type_t &t, std::string_view s) {
  return t.len < 0 ? sm2::make_varlena(payload(t, s)) : payload(t, s);
}

/* The key of t's canonical text s. */
std::string key(const sm2::type_t &t, std::string_view s) {
  std::string k;
  REQUIRE(t.key_encode(payload(t, s), -1, nullptr, k) == 0);
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

// Verify that a CG with a long value whose key is short is not skipped
// by its filter: a bpchar's key leaves out its trailing spaces.
// 1. Build a bpchar CG of "b" and of "a" padded to 2,000 bytes, a long
//    value, kept out of the filter.
// 2. An equality on "a" may match, as the long value is "a".
// 3. An equality on "c", outside min and max, may not.
TEST_CASE("a long value with a short key is not skipped by the filter") {
  // 1. The CG.
  sm2::cg_column_t col = column(sm2::type_bpchar);
  std::vector<std::string> d = {
      datum(sm2::type_bpchar, "b"),
      datum(sm2::type_bpchar, "a" + std::string(1999, ' '))};
  sm2::cg_built_t b;
  REQUIRE(sm2::cg_build(col, values(d), b) == 0);
  REQUIRE(b.stats.has_long);
  auto eq = [&](std::string_view s) {
    sm2::cg_bound_t x;
    x.not_null = x.has_in = true;
    x.in = {key(sm2::type_bpchar, s)};
    return x;
  };
  // 2. "a" may match.
  CHECK(sm2::cg_may_match(col, b.stats, b.filter, eq("a")));
  // 3. "c" may not.
  CHECK_FALSE(sm2::cg_may_match(col, b.stats, b.filter, eq("c")));
}

// Verify that a CG of another version of its column's collation is
// not pruned by its values: their keys may sort otherwise now.
// 1. Build a text CG of "b" and "c" under an ICU collation.
// 2. With the version it was built with, = 'a' and > 'x' skip it.
// 3. With another version recorded, neither skips it; IS NULL still
//    does, as the CG holds no null.
TEST_CASE("a CG of another collation version is not pruned by value") {
  // 1. The CG.
  sm2::collation_t *coll = nullptr;
  REQUIRE_MESSAGE(sm2::collation_open("en-US", true, "", coll) == 0,
                  sm1::errmsg());
  sm2::cg_column_t col{&sm2::type_text, -1, coll};
  sm2::cg_built_t b;
  REQUIRE(sm2::cg_build(
              col,
              values({datum(sm2::type_text, "b"), datum(sm2::type_text, "c")}),
              b) == 0);
  REQUIRE(b.stats.collation_version == sm2::collation_version(coll));
  auto k = [&](std::string_view s) {
    std::string out;
    REQUIRE(sm2::type_text.key_encode(payload(sm2::type_text, s), -1, coll,
                                      out) == 0);
    return out;
  };
  sm2::cg_bound_t eq, above, null;
  eq.not_null = eq.has_in = true;
  eq.in = {k("a")};
  above.not_null = above.has_lo = true;
  above.lo = k("x");
  null.is_null = true;
  // 2. The same version.
  CHECK_FALSE(sm2::cg_may_match(col, b.stats, b.filter, eq));
  CHECK_FALSE(sm2::cg_may_match(col, b.stats, b.filter, above));
  // 3. Another version.
  sm2::cg_stats_t old = b.stats;
  old.collation_version = "0.0-old";
  CHECK(sm2::cg_may_match(col, old, b.filter, eq));
  CHECK(sm2::cg_may_match(col, old, b.filter, above));
  CHECK_FALSE(sm2::cg_may_match(col, old, b.filter, null));
  sm2::collation_close(coll);
}

namespace {

/* A CG file's encoding, from its tail (../COLSTORE.md §5.3). */
int encoding_of(const std::string &file) {
  return static_cast<unsigned char>(file[file.size() - 38 + 8]);
}

/* A filter's kind, its first byte: 1 an exact set, 2 a bloom filter. */
int filter_kind(const std::string &filter) {
  return static_cast<unsigned char>(filter[0]);
}

/* An equality on key k. */
sm2::cg_bound_t equal_to(std::string k) {
  sm2::cg_bound_t b;
  b.not_null = b.has_in = true;
  b.in = {std::move(k)};
  return b;
}

} // namespace

// Verify that a CG takes a dictionary only where it is smaller than
// plain, and delta for sorted integers (../COLSTORE.md §5.1, §5.2).
// 1. 1,000 rows of 4 distinct text values: a dictionary.
// 2. 1,000 distinct text values: plain.
// 3. 1,000 ascending int4 values with no null: delta.
// 4. Each reads back.
TEST_CASE("a CG takes a dictionary only where it is smaller") {
  // 1. Few distinct values.
  std::vector<std::string> few, many, ints;
  for (int i = 0; i < 1000; i++) {
    few.push_back(datum(sm2::type_text, "value-" + std::to_string(i % 4)));
    many.push_back(datum(sm2::type_text, "value-" + std::to_string(i)));
    ints.push_back(datum(sm2::type_int4, std::to_string(i * 3)));
  }
  sm2::cg_built_t b = round_trip(column(sm2::type_text), values(few));
  CHECK(encoding_of(b.file) == 2);
  // 2. Many.
  b = round_trip(column(sm2::type_text), values(many));
  CHECK(encoding_of(b.file) == 1);
  // 3. Sorted integers.
  b = round_trip(column(sm2::type_int4), values(ints));
  CHECK(encoding_of(b.file) == 3);
}

// Verify that the filter is an exact set while its keys total 4 KB or
// less, and a bloom filter above, with no false negative and few false
// positives (../COLSTORE.md §6.3).
// 1. As many int8 values as 4 KB of keys holds: an exact set, which
//    answers each value and no other.
// 2. One more value: a bloom filter, which answers each value.
// 3. Of 1,000 values outside the CG's set, between its min and max,
//    under 5% may match.
TEST_CASE("the filter is an exact set to 4 KB of keys, a bloom filter above") {
  const size_t ksize = key(sm2::type_int8, "1").size();
  const size_t fit = 4096 / ksize;
  auto build = [&](size_t n) {
    std::vector<std::string> d;
    for (size_t i = 0; i < n; i++) {
      d.push_back(datum(sm2::type_int8, std::to_string(i * 2)));
    }
    sm2::cg_built_t b;
    REQUIRE(sm2::cg_build(column(sm2::type_int8), values(d), b) == 0);
    return b;
  };
  auto eq = [&](size_t v) {
    return equal_to(key(sm2::type_int8, std::to_string(v)));
  };
  // 1. The exact set.
  sm2::cg_built_t b = build(fit);
  REQUIRE(filter_kind(b.filter) == 1);
  for (size_t i = 0; i < fit; i++) {
    REQUIRE(sm2::cg_may_match(column(sm2::type_int8), b.stats, b.filter,
                              eq(i * 2)));
    REQUIRE_FALSE(sm2::cg_may_match(column(sm2::type_int8), b.stats, b.filter,
                                    eq(i * 2 + 1)));
  }
  // 2. The bloom filter.
  const size_t n = fit + 1;
  b = build(n);
  REQUIRE(filter_kind(b.filter) == 2);
  for (size_t i = 0; i < n; i++) {
    REQUIRE(sm2::cg_may_match(column(sm2::type_int8), b.stats, b.filter,
                              eq(i * 2)));
  }
  // 3. Few false positives.
  int fp = 0;
  for (size_t i = 0; i < 1000; i++) {
    fp += sm2::cg_may_match(column(sm2::type_int8), b.stats, b.filter,
                            eq((i % n) * 2 + 1));
  }
  CHECK(fp < 50);
}

// Verify that under a non-deterministic, case-insensitive collation an
// equality matches a value of another case, in an exact set and in a
// bloom filter (../COLSTORE.md §6.3).
// 1. A CG of "Apple" alone: an exact set; = 'apple' may match.
// 2. A CG of 600 values "Name0000" and on: a bloom filter; = 'name0300'
//    and = 'NAME0599' may match.
// 3. = 'name9999' is in neither CG's filter.
TEST_CASE("a non-deterministic collation's filter matches another case") {
  sm2::collation_t *ci = nullptr;
  REQUIRE_MESSAGE(sm2::collation_open("und-u-ks-level2", false, "", ci) == 0,
                  sm1::errmsg());
  sm2::cg_column_t col{&sm2::type_text, -1, ci};
  auto k = [&](std::string_view s) {
    std::string out;
    REQUIRE(sm2::type_text.key_encode(payload(sm2::type_text, s), -1, ci,
                                      out) == 0);
    return out;
  };
  // 1. The exact set.
  sm2::cg_built_t one;
  REQUIRE(sm2::cg_build(col, values({datum(sm2::type_text, "Apple")}), one) ==
          0);
  REQUIRE(filter_kind(one.filter) == 1);
  CHECK(sm2::cg_may_match(col, one.stats, one.filter, equal_to(k("apple"))));
  // 2. The bloom filter.
  std::vector<std::string> d;
  for (int i = 0; i < 600; i++) {
    char name[16];
    snprintf(name, sizeof(name), "Name%04d", i);
    d.push_back(datum(sm2::type_text, name));
  }
  sm2::cg_built_t many;
  REQUIRE(sm2::cg_build(col, values(d), many) == 0);
  REQUIRE(filter_kind(many.filter) == 2);
  CHECK(
      sm2::cg_may_match(col, many.stats, many.filter, equal_to(k("name0300"))));
  CHECK(
      sm2::cg_may_match(col, many.stats, many.filter, equal_to(k("NAME0599"))));
  // 3. Absent.
  CHECK_FALSE(
      sm2::cg_may_match(col, one.stats, one.filter, equal_to(k("name9999"))));
  sm2::collation_close(ci);
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

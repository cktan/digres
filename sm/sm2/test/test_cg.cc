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

#include "sm0_codec.h"
#include "sm0_crc.h"
#include "sm2.h"
#include "ty_vec.h"

#include <zstd.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <functional>
#include <limits>
#include <random>
#include <set>
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

/* The length of the varlena at p, by its header. */
size_t varlena_size(const char *p) {
  if (*p & 1) {
    return static_cast<unsigned char>(*p) >> 1;
  }
  uint32_t header;
  memcpy(&header, p, 4);
  return header >> 2;
}

/* Value k of a CG's arrays, of type t, through the bytes they hold. */
std::string_view vec_value(const sm2::cg_vec_t &vec, const sm2::type_t &t,
                           uint32_t k) {
  if (t.len < 0) {
    const char *p = static_cast<const char *const *>(vec.values)[k];
    return {p, varlena_size(p)};
  }
  return {static_cast<const char *>(vec.values) + size_t(k) * t.len,
          static_cast<size_t>(t.len)};
}

/*
 * Check a CG's arrays against the values it was built from.
 * 1. The flags: none if no row is null, else ty's null bit alone.
 * 2. The values, by the codes if there are any, each below dict_n.
 * 3. A null row's value can be read: zero bytes, or an empty varlena.
 * 4. The alignment: 8 of a fixed type's array, 4 of a long varlena.
 */
void check_vec(const sm2::cg_t *cg, const sm2::type_t &t,
               const std::vector<sm2::cg_value_t> &v) {
  sm2::cg_vec_t vec;
  sm2::cg_vec(cg, vec);
  REQUIRE(vec.n == v.size());
  // 1. The flags.
  bool any = false;
  for (const sm2::cg_value_t &x : v) {
    any = any || x.isnull;
  }
  REQUIRE((nullptr != vec.flags) == any);
  REQUIRE((nullptr != vec.codes) == (0 != vec.dict_n));
  // The first row of each fault, n if there is none: one assertion each.
  uint32_t flag = vec.n, code = vec.n, value = vec.n, null = vec.n,
           align = vec.n;
  for (uint32_t i = 0; i < vec.n; i++) {
    if (any && vec.flags[i] != (v[i].isnull ? TY_VEC_NULL : 0)) {
      flag = std::min(flag, i);
    }
    // 2. The values.
    if (vec.codes && vec.codes[i] >= vec.dict_n) {
      code = std::min(code, i);
      continue;
    }
    const std::string_view d = vec_value(vec, t, vec.codes ? vec.codes[i] : i);
    if (!v[i].isnull) {
      if (d != v[i].datum) {
        value = std::min(value, i);
      }
    } else if (!vec.codes && d != (t.len < 0 ? sm2::make_varlena("")
                                             : std::string(t.len, '\0'))) {
      // 3. A null row's.
      null = std::min(null, i);
    }
    // 4. The alignment.
    if (t.len < 0 && 0 == (d[0] & 1) &&
        reinterpret_cast<uintptr_t>(d.data()) % 4 != 0) {
      align = std::min(align, i);
    }
  }
  REQUIRE(flag == vec.n);
  REQUIRE(code == vec.n);
  REQUIRE(value == vec.n);
  REQUIRE(null == vec.n);
  REQUIRE(align == vec.n);
  if (t.len > 0 && vec.n > 0) {
    REQUIRE(reinterpret_cast<uintptr_t>(vec.values) % 8 == 0);
  }
}

/* Build a CG, open it, and check every row reads back. */
sm2::cg_built_t round_trip(const sm2::cg_column_t &col,
                           const std::vector<sm2::cg_value_t> &v) {
  sm2::cg_built_t b;
  REQUIRE(sm2::cg_build(col, v, b) == 0);
  sm2::cg_t *cg = nullptr;
  REQUIRE(sm2::cg_open(b.file, col.type, cg) == 0);
  check_vec(cg, *col.type, v);
  REQUIRE(sm2::cg_rows(cg) == v.size());
  // The first row that does not read back: one assertion a CG.
  uint32_t bad = cg_rows(cg);
  for (uint32_t i = 0; i < v.size(); i++) {
    std::string_view d;
    int r = sm2::cg_get(cg, i, d);
    if (v[i].isnull ? r != 1 : r != 0 || d != v[i].datum) {
      bad = std::min(bad, i);
    }
  }
  CHECK(bad == v.size());
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
// 4. A text of 2,048 bytes: in the CG, as every value is.
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
  // 4. A value of 2,048 bytes.
  std::vector<std::string> docs = {
      datum(sm2::type_text, "short"),
      datum(sm2::type_text, std::string(2048, 'x'))};
  b = round_trip(column(sm2::type_text), values(docs));
  CHECK(b.stats.distinct == 2);
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
  CHECK(sm2::cg_open(b.file, &sm2::type_int4, cg) == -1);
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

// Verify that a string CG takes 'NaN' and the infinities as text, not
// as special values that sort outside min and max.
// 1. A text CG of 'NaN' and 'zebra': no flag; its max is 'zebra',
//    so x > 'p' may match.
// 2. A text CG of '!' and '-Infinity': no flag; its min is '!', so
//    x < '#' may match.
// 3. Each string type's CG of 'Infinity': no flag; x = 'Infinity'
//    may match.
TEST_CASE("a string CG takes NaN and infinity as text") {
  // 1. 'NaN' sorts below 'zebra'.
  sm2::cg_column_t col = column(sm2::type_text);
  std::vector<std::string> d = {datum(sm2::type_text, "NaN"),
                                datum(sm2::type_text, "zebra")};
  sm2::cg_built_t b = round_trip(col, values(d));
  CHECK_FALSE(b.stats.has_nan);
  CHECK(b.stats.min == key(sm2::type_text, "NaN"));
  CHECK(b.stats.max == key(sm2::type_text, "zebra"));
  sm2::cg_bound_t gt;
  gt.has_lo = true;
  gt.lo_incl = false;
  gt.lo = key(sm2::type_text, "p");
  CHECK(sm2::cg_may_match(col, b.stats, b.filter, gt));
  // 2. '-Infinity' sorts above '!'.
  d = {datum(sm2::type_text, "!"), datum(sm2::type_text, "-Infinity")};
  b = round_trip(col, values(d));
  CHECK_FALSE(b.stats.has_neg_inf);
  CHECK(b.stats.min == key(sm2::type_text, "!"));
  sm2::cg_bound_t lt;
  lt.has_hi = true;
  lt.hi_incl = false;
  lt.hi = key(sm2::type_text, "#");
  CHECK(sm2::cg_may_match(col, b.stats, b.filter, lt));
  // 3. Every string type.
  for (const sm2::type_t *t : {&sm2::type_text, &sm2::type_varchar,
                               &sm2::type_bpchar, &sm2::type_name}) {
    sm2::cg_column_t c = column(*t);
    b = round_trip(c, values({datum(*t, "Infinity")}));
    CHECK_FALSE(b.stats.has_pos_inf);
    CHECK(sm2::cg_may_match(c, b.stats, b.filter, eq(key(*t, "Infinity"))));
  }
}

// Verify that a bpchar CG's filter matches a padded value by its key,
// which leaves out its trailing spaces.
// 1. Build a bpchar CG of "b" and of "a" padded to 2,000 bytes.
// 2. An equality on "a" may match, as the padded value is "a".
// 3. An equality on "c", outside min and max, may not.
TEST_CASE("a bpchar CG's filter matches a padded value") {
  // 1. The CG.
  sm2::cg_column_t col = column(sm2::type_bpchar);
  std::vector<std::string> d = {
      datum(sm2::type_bpchar, "b"),
      datum(sm2::type_bpchar, "a" + std::string(1999, ' '))};
  sm2::cg_built_t b;
  REQUIRE(sm2::cg_build(col, values(d), b) == 0);
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

namespace {

/* The types of a column, with a typmod each takes. */
struct col_type_t {
  const sm2::type_t *type;
  int32_t typmod;
};
constexpr int32_t dec_typmod = ((15 << 16) | 2) + 4;
const std::vector<col_type_t> all_types = {{&sm2::type_bool, -1},
                                           {&sm2::type_int2, -1},
                                           {&sm2::type_int4, -1},
                                           {&sm2::type_int8, -1},
                                           {&sm2::type_float4, -1},
                                           {&sm2::type_float8, -1},
                                           {&sm2::type_text, -1},
                                           {&sm2::type_varchar, -1},
                                           {&sm2::type_bytea, -1},
                                           {&sm2::type_date, -1},
                                           {&sm2::type_time, -1},
                                           {&sm2::type_timestamp, -1},
                                           {&sm2::type_timestamptz, -1},
                                           {&sm2::type_interval, -1},
                                           {&sm2::type_uuid, -1},
                                           {&sm2::type_decimal64, dec_typmod},
                                           {&sm2::type_decimal128, dec_typmod},
                                           {&sm2::type_bpchar, -1},
                                           {&sm2::type_name, -1}};

/* The datum of type t for the number v: equal numbers give equal datums. */
std::string datum_of(const sm2::type_t &t, uint32_t v) {
  if (t.len < 0) {
    // Lengths on both sides of the short header's 126 bytes.
    return sm2::make_varlena(std::string(v % 140, char('a' + v % 26)) +
                             std::to_string(v));
  }
  if (&t == &sm2::type_bool) {
    return std::string(1, char(v & 1));
  }
  if (&t == &sm2::type_decimal64) {
    const int64_t x = (int64_t(v) << 5) | 2;
    return {reinterpret_cast<const char *>(&x), 8};
  }
  if (&t == &sm2::type_decimal128) {
    const __int128 x = (__int128(v) << 6) | 2;
    return {reinterpret_cast<const char *>(&x), 16};
  }
  if (&t == &sm2::type_float4) {
    const float x = float(v);
    return {reinterpret_cast<const char *>(&x), 4};
  }
  if (&t == &sm2::type_float8) {
    const double x = v;
    return {reinterpret_cast<const char *>(&x), 8};
  }
  if (&t == &sm2::type_name) {
    std::string d = "n" + std::to_string(v);
    d.resize(64, '\0');
    return d;
  }
  // An integer's bytes, a date's and a time's are the number's; an
  // interval's and a uuid's are of the number twice.
  std::string d(t.len, '\0');
  for (size_t at = 0; at < d.size(); at += 8) {
    const uint64_t x = v;
    memcpy(&d[at], &x, std::min<size_t>(8, d.size() - at));
  }
  return d;
}

/* The body of a CG file, and a file of another body, its CRC32C right. */
std::string body_of(const std::string &file) {
  const char *t = file.data() + file.size() - 38;
  std::string body(sm0::get_be32(t + 18), '\0');
  REQUIRE(ZSTD_decompress(body.data(), body.size(), file.data(),
                          sm0::get_be32(t + 14)) == body.size());
  return body;
}
std::string with_body(const std::string &file, const std::string &body) {
  const size_t zlen = sm0::get_be32(file.data() + file.size() - 38 + 14);
  std::string z(ZSTD_compressBound(body.size()), '\0');
  z.resize(ZSTD_compress(z.data(), z.size(), body.data(), body.size(), 3));
  std::string f = z + file.substr(zlen);
  const size_t t = f.size() - 38;
  std::string n;
  sm0::put_be32(n, static_cast<uint32_t>(z.size()));
  sm0::put_be32(n, static_cast<uint32_t>(body.size()));
  f.replace(t + 14, 8, n);
  n.clear();
  sm0::put_be32(n, sm0::crc32c(0, f.data(), f.size() - 8));
  f.replace(t + 30, 4, n);
  return f;
}

/* The message of opening file as a CG of type t, or "" if it opens. */
std::string open_error(const std::string &file, const sm2::type_t &t) {
  sm2::cg_t *cg = nullptr;
  if (0 == sm2::cg_open(file, &t, cg)) {
    sm2::cg_close(cg);
    return "";
  }
  CHECK(nullptr == cg);
  CHECK(sm1::errnum() == sm1::SM_ERR_CORRUPT);
  return std::string(sm1::errmsg());
}

} // namespace

// Verify that every type reads back in every encoding, as arrays and row by
// row, with no null, some and all.
// 1. Each type, at sizes around a byte and a vector, and a large one.
// 2. Values of few kinds, so a dictionary where it is smaller; of many, so
//    plain; and rising, so delta of the integers.
// 3. No null, every seventh row, all but one, and all; a null row first.
TEST_CASE("a CG of each type reads back as arrays") {
  std::set<int> encodings;
  for (const col_type_t &ct : all_types) {
    sm2::cg_column_t col{ct.type, ct.typmod, nullptr};
    // 1. The sizes.
    for (uint32_t n : {0u, 1u, 2u, 7u, 8u, 9u, 17u, 1000u}) {
      for (int kind = 0; kind < 3; kind++) {
        // 2. The values.
        std::vector<std::string> d;
        for (uint32_t i = 0; i < n; i++) {
          d.push_back(datum_of(*ct.type, 0 == kind   ? i * 7 % 3
                                         : 1 == kind ? i * 7919 % 10007
                                                     : i));
        }
        // 3. The nulls.
        for (int nulls = 0; nulls < 4; nulls++) {
          std::vector<bool> null;
          for (uint32_t i = 0; i < n; i++) {
            null.push_back(1 == nulls   ? i % 7 == 0
                           : 2 == nulls ? i != n / 2
                                        : 3 == nulls);
          }
          CAPTURE(ct.type->name);
          CAPTURE(n);
          CAPTURE(kind);
          CAPTURE(nulls);
          sm2::cg_built_t b = round_trip(col, values(d, null));
          encodings.insert(encoding_of(b.file));
        }
      }
    }
  }
  CHECK(encodings == std::set<int>{1, 2, 3});
}

// Verify that the packed codes and deltas read back at each width.
// 1. Deltas of 0 to 63 bits, of int8, and the largest, of 64.
// 2. Dictionaries of 1 value to 1,025: codes of 0 to 11 bits.
TEST_CASE("a CG's packed fields read back at each width") {
  // 1. Deltas: a small one, then the widest, then small ones.
  for (int bits = 0; bits <= 64; bits++) {
    std::vector<std::string> d;
    int64_t v = 64 == bits ? std::numeric_limits<int64_t>::min() : -5;
    const uint64_t big = 0 == bits ? 0 : uint64_t(1) << (bits - 1);
    for (int i = 0; i < 20; i++) {
      d.emplace_back(reinterpret_cast<const char *>(&v), 8);
      const uint64_t step = i == 3 ? big : bits > 1 ? uint64_t(i % 2) : 0;
      v = int64_t(uint64_t(v) + step);
    }
    CAPTURE(bits);
    sm2::cg_built_t b = round_trip(column(sm2::type_int8), values(d));
    CHECK(encoding_of(b.file) == 3);
  }
  // 2. Codes.
  for (uint32_t kinds : {1u, 2u, 3u, 4u, 5u, 16u, 17u, 256u, 257u, 1025u}) {
    std::vector<std::string> d;
    for (uint32_t i = 0; i < 10000; i++) {
      d.push_back(datum_of(sm2::type_decimal128, i * 7919 % kinds));
    }
    CAPTURE(kinds);
    sm2::cg_built_t b =
        round_trip({&sm2::type_decimal128, dec_typmod, nullptr}, values(d));
    CHECK(encoding_of(b.file) == 2);
  }
}

// Verify that a varlena with a 4-byte header is aligned, wherever the body
// has it.
// 1. Long varlenas after short ones of 0 to 7 bytes, in plain encoding.
// 2. The same as a dictionary's values.
TEST_CASE("a CG's long varlenas are aligned to 4") {
  for (int first = 0; first < 8; first++) {
    std::vector<std::string> d;
    // 1. Plain. check_vec sees the alignment.
    for (int i = 0; i < 40; i++) {
      d.push_back(sm2::make_varlena(std::string(first + i % 3, 'a' + i % 26)));
      d.push_back(sm2::make_varlena(std::string(127 + i, 'A' + i % 26)));
    }
    sm2::cg_built_t b = round_trip(column(sm2::type_text), values(d));
    // 2. A dictionary.
    std::vector<std::string> many;
    for (int i = 0; i < 20; i++) {
      many.insert(many.end(), d.begin(), d.end());
    }
    b = round_trip(column(sm2::type_text), values(many));
    CHECK(encoding_of(b.file) == 2);
  }
}

// Verify that a badly made CG, whose CRC32C is right, fails to open.
// 1. Plain int4: a length that is not 4; a flag byte that is none; a null
//    row with a length; another type's width.
// 2. Plain text: a header that is not the length; a compressed varlena; a
//    TOAST pointer; an empty value.
// 3. Delta: of a type with no delta; of another width; a width of 65 bits.
// 4. A dictionary: more values than rows; a code past it; 33-bit codes.
// 5. An unknown encoding, and a row count that is not the tail's.
TEST_CASE("a badly made CG fails to open") {
  const size_t hdr = 6; // the encoding, the rows, and the flags' byte
  auto changed = [](const std::string &file,
                    const std::function<void(std::string &)> &change) {
    std::string body = body_of(file);
    change(body);
    return with_body(file, body);
  };
  // 1. Plain int4, unsorted, so not delta.
  sm2::cg_built_t b = round_trip(
      column(sm2::type_int4),
      values({datum(sm2::type_int4, "7"), datum(sm2::type_int4, "3")},
             {false, true}));
  REQUIRE(encoding_of(b.file) == 1);
  CHECK("" ==
        open_error(changed(b.file, [](std::string &) {}), sm2::type_int4));
  CHECK("" !=
        open_error(changed(b.file, [&](std::string &s) { s[hdr + 2 + 3] = 3; }),
                   sm2::type_int4));
  CHECK("" != open_error(changed(b.file, [&](std::string &s) { s[hdr] = 1; }),
                         sm2::type_int4));
  CHECK("" !=
        open_error(changed(b.file, [&](std::string &s) { s[hdr + 1] = 3; }),
                   sm2::type_int4));
  CHECK("" != open_error(changed(b.file, [&](std::string &s) { s[hdr] = 2; }),
                         sm2::type_int4));
  CHECK("" !=
        open_error(changed(b.file, [&](std::string &s) { s[hdr + 1] = 0; }),
                   sm2::type_int4));
  CHECK("" != open_error(b.file, sm2::type_int8));
  CHECK("" != open_error(b.file, sm2::type_text));

  // 2. Plain text: the length is at hdr, the varlena's header after it.
  b = round_trip(column(sm2::type_text), values({datum(sm2::type_text, "abc"),
                                                 datum(sm2::type_text, "de")}));
  REQUIRE(encoding_of(b.file) == 1);
  for (int first : {(5 << 1) | 1, (3 << 1) | 1, 0x01, 0x12, 0x10}) {
    CAPTURE(first);
    CHECK("" !=
          open_error(changed(b.file,
                             [&](std::string &s) { s[hdr + 4] = char(first); }),
                     sm2::type_text));
  }
  CHECK("" != open_error(changed(b.file,
                                 [&](std::string &s) {
                                   s.replace(hdr, 8, std::string(4, '\0'));
                                 }),
                         sm2::type_text));

  // A null's length is 0: the last row's, here given 1 and a byte.
  b = round_trip(column(sm2::type_text), values({datum(sm2::type_text, "a"),
                                                 datum(sm2::type_text, "b")},
                                                {false, true}));
  REQUIRE(encoding_of(b.file) == 1);
  CHECK("" != open_error(changed(b.file,
                                 [](std::string &s) {
                                   s.back() = 1;
                                   s.push_back('x');
                                 }),
                         sm2::type_text));
  // A 4-byte header of the right length, but compressed.
  b = round_trip(column(sm2::type_text),
                 values({datum(sm2::type_text, std::string(200, 'a'))}));
  CHECK("" !=
        open_error(changed(b.file, [&](std::string &s) { s[hdr + 4] |= 2; }),
                   sm2::type_text));

  // 3. Delta: the base is at hdr, the width at hdr + 8, the bits after.
  b = round_trip(column(sm2::type_int4),
                 values({datum(sm2::type_int4, "1"), datum(sm2::type_int4, "2"),
                         datum(sm2::type_int4, "3")}));
  REQUIRE(encoding_of(b.file) == 3);
  CHECK("" != open_error(b.file, sm2::type_date));
  CHECK("" != open_error(b.file, sm2::type_int8));
  CHECK("" !=
        open_error(changed(b.file, [&](std::string &s) { s[hdr + 8] = 8; }),
                   sm2::type_int4));
  CHECK("" != open_error(changed(b.file,
                                 [&](std::string &s) {
                                   s[hdr + 9] = 65;
                                   s.append(100, 'x');
                                 }),
                         sm2::type_int4));

  // 4. A dictionary of 2 decimals: its size is at hdr, its values take 20
  //    bytes each, then the bits, and the codes.
  std::vector<std::string> d;
  for (uint32_t i = 0; i < 40; i++) {
    d.push_back(datum_of(sm2::type_decimal128, i % 2));
  }
  b = round_trip({&sm2::type_decimal128, dec_typmod, nullptr}, values(d));
  REQUIRE(encoding_of(b.file) == 2);
  CHECK(std::string::npos !=
        open_error(changed(b.file, [&](std::string &s) { s[hdr + 3] = 41; }),
                   sm2::type_decimal128)
            .find("dictionary larger"));
  CHECK("" != open_error(changed(b.file,
                                 [&](std::string &s) {
                                   // 2-bit codes of 0xff: 3, past it.
                                   s[hdr + 4 + 40] = 2;
                                   s.append(10, '\xff');
                                 }),
                         sm2::type_decimal128));
  CHECK("" != open_error(changed(b.file,
                                 [&](std::string &s) {
                                   s[hdr + 4 + 40] = 33;
                                   s.append(400, '\0');
                                 }),
                         sm2::type_decimal128));

  // 5. The encoding, and the rows.
  CHECK("" != open_error(changed(b.file, [](std::string &s) { s[0] = 4; }),
                         sm2::type_decimal128));
  CHECK(std::string::npos !=
        open_error(changed(b.file, [](std::string &s) { s[4] ^= 1; }),
                   sm2::type_decimal128)
            .find("row counts"));
}

// Verify that a body cut short anywhere fails to open, with its CRC32C
// right: no read goes past it.
// 1. A CG of each encoding, of a fixed type and of a varlena, with nulls
//    and without.
// 2. Each prefix of its body fails; the whole opens.
TEST_CASE("a CG's body cut short fails to open") {
  struct one_t {
    const sm2::type_t *type;
    bool nulls;
    uint32_t kinds;
  };
  // 1. The CGs: delta, plain, and a dictionary.
  for (const one_t &o :
       {one_t{&sm2::type_int8, false, 0}, one_t{&sm2::type_int4, false, 97},
        one_t{&sm2::type_int4, true, 97}, one_t{&sm2::type_text, false, 97},
        one_t{&sm2::type_text, true, 97}, one_t{&sm2::type_text, false, 3},
        one_t{&sm2::type_text, true, 3},
        one_t{&sm2::type_decimal128, true, 3}}) {
    std::vector<std::string> d;
    std::vector<bool> null;
    for (uint32_t i = 0; i < 90; i++) {
      d.push_back(datum_of(*o.type, o.kinds ? i * 41 % o.kinds : i * 5));
      null.push_back(o.nulls && i % 9 == 4);
    }
    sm2::cg_column_t col{o.type, o.type->len == 16 ? dec_typmod : -1, nullptr};
    sm2::cg_built_t b = round_trip(col, values(d, null));
    const std::string body = body_of(b.file);
    // 2. Each prefix.
    for (size_t len = 0; len < body.size(); len++) {
      CAPTURE(len);
      CHECK("" != open_error(with_body(b.file, body.substr(0, len)), *o.type));
    }
    CHECK("" == open_error(with_body(b.file, body), *o.type));
  }
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

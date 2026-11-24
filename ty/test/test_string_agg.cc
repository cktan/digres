/*
 * Copyright (c) 2026 CK Tan
 * SPDX-License-Identifier: MIT
 */

/*
 * Tests of string_agg's state: ty_string_agg_add, _combine, _serialize,
 * _deserialize and _finish, with the cases of PostgreSQL's aggregates test.
 */
#include <stdlib.h>
#include <string.h>

#include <string>
#include <vector>

#include "doctest.h"
#include "ty_text.h"
#include "vl.h"

namespace {

/* The allocations a test's thread made and has not freed. */
thread_local int t_live = 0;

/* An alloc that counts what is live, from malloc. */
void *live_alloc(size_t size) {
  t_live++;
  return malloc(size);
}

/* A free that counts what is live. */
void live_free(void *ptr) {
  t_live--;
  free(ptr);
}

/* Set the counting functions for a test; the default again at its end. */
struct live_t {
  live_t() {
    t_live = 0;
    ty_set_alloc(live_alloc, live_free);
  }
  ~live_t() { ty_set_alloc(nullptr, nullptr); }
};

/* A row: a value and a delimiter, either of them NULL. */
struct row_t {
  const char *v;
  const char *d;
};

/* Add the rows that have a value, as the transition function does. */
void add_rows(ty_string_agg_t *st, const std::vector<row_t> &rows) {
  for (const row_t &r : rows) {
    if (nullptr == r.v) {
      continue;
    }
    REQUIRE(0 == ty_string_agg_add(st, r.d, r.d ? strlen(r.d) : 0, r.v,
                                   strlen(r.v)));
  }
}

/* The result of a state, as the final function gives it. */
std::string finish(const ty_string_agg_t *st) {
  vl_out_t r;

  REQUIRE(0 == ty_string_agg_finish(st, &r));
  return r.str();
}

/* Free a state's bytes. */
void drop(ty_string_agg_t *st) {
  if (nullptr != st->buf) {
    ty_free(st->buf);
  }
}

/* string_agg over the rows; "NULL" if no row has a value. */
std::string agg(const std::vector<row_t> &rows) {
  ty_string_agg_t st = {};

  add_rows(&st, rows);
  if (!st.started) {
    return "NULL";
  }
  std::string s = finish(&st);

  drop(&st);
  return s;
}

} // namespace

// Verify the values and delimiters of a string_agg, as PostgreSQL joins them.
// 1. The cases of PostgreSQL's aggregates test: NULL values are skipped.
// 2. The first value's delimiter is cut off, whatever it is, NULL included.
// 3. A later NULL delimiter adds nothing; empty values and bytes of 0 stay.
// 4. The state does not change at the finish.
TEST_CASE("string_agg joins the values") {
  live_t live;

  // 1. The cases of PostgreSQL's aggregates test: NULL values are skipped.
  CHECK(agg({{"aaaa", ","}, {"bbbb", ","}, {"cccc", ","}}) == "aaaa,bbbb,cccc");
  CHECK(agg({{"aaaa", ","}, {nullptr, ","}, {"bbbb", ","}, {"cccc", ","}}) ==
        "aaaa,bbbb,cccc");
  CHECK(
      agg({{nullptr, "AB"}, {nullptr, "AB"}, {"bbbb", "AB"}, {"cccc", "AB"}}) ==
      "bbbbABcccc");
  CHECK(agg({{nullptr, ","}, {nullptr, ","}}) == "NULL");

  // 2. The first value's delimiter is cut off, whatever it is, NULL included.
  CHECK(agg({{"a", "--"}, {"b", ","}}) == "a,b");
  CHECK(agg({{"a", nullptr}, {"b", ","}}) == "a,b");
  CHECK(agg({{"", nullptr}, {"b", ","}}) == ",b");

  // 3. A later NULL delimiter adds nothing; empty values and bytes of 0 stay.
  CHECK(agg({{"a", ","}, {"b", nullptr}, {"c", ","}}) == "ab,c");
  CHECK(agg({{"", ","}, {"", ","}}) == ",");
  CHECK(agg({{"", nullptr}}) == "");
  ty_string_agg_t st = {};

  REQUIRE(0 == ty_string_agg_add(&st, "\0", 1, "x\0y", 3));
  REQUIRE(0 == ty_string_agg_add(&st, "\0", 1, "z", 1));
  CHECK(finish(&st) == std::string("x\0y\0z", 5));

  // 4. The state does not change at the finish.
  CHECK(finish(&st) == std::string("x\0y\0z", 5));
  drop(&st);
  CHECK(0 == t_live);
}

// Verify the parts of a parallel aggregate, joined and sent between workers.
// 1. Two parts combined give what one state over all the rows gives.
// 2. A zeroed state takes its part's first delimiter; an empty part adds none.
// 3. A part written to bytes and read back is the same state.
// 4. Fewer than 4 bytes is an error.
TEST_CASE("string_agg's parts join up") {
  live_t live;
  std::vector<row_t> one = {{"a", "-"}, {"b", ","}};
  std::vector<row_t> two = {{"c", ";"}, {"d", ","}};
  ty_string_agg_t p1 = {}, p2 = {};

  // 1. Two parts combined give what one state over all the rows gives.
  add_rows(&p1, one);
  add_rows(&p2, two);
  REQUIRE(0 == ty_string_agg_combine(&p1, &p2));
  CHECK(finish(&p1) == "a,b;c,d");

  // 2. A zeroed state takes its part's first delimiter; an empty part adds
  //    none.
  ty_string_agg_t z = {}, empty = {};

  REQUIRE(0 == ty_string_agg_combine(&z, &p2));
  CHECK(finish(&z) == "c,d");
  REQUIRE(0 == ty_string_agg_add(&empty, nullptr, 0, "", 0));
  REQUIRE(0 == ty_string_agg_combine(&z, &empty));
  CHECK(finish(&z) == "c,d");
  CHECK(1 == z.first);

  // 3. A part written to bytes and read back is the same state.
  ty_string_agg_t back = {};
  {
    vl_out_t bytes;

    REQUIRE(0 == ty_string_agg_serialize(&p2, &bytes));
    const std::string b = bytes.str();

    CHECK(b == std::string("\0\0\0\1;c,d", 8));
    REQUIRE(0 == ty_string_agg_deserialize(&back, b.data(), b.size()));
  }
  CHECK(1 == back.first);
  CHECK(finish(&back) == "c,d");

  // 4. Fewer than 4 bytes is an error.
  ty_string_agg_t bad = {};

  CHECK(-1 == ty_string_agg_deserialize(&bad, "\0\0\0", 3));
  CHECK(TY_ERR_INVALID == ty_errnum());
  CHECK(std::string(ty_errmsg()) == "insufficient data left in message");
  CHECK(!bad.started);
  drop(&p1);
  drop(&p2);
  drop(&z);
  drop(&empty);
  drop(&back);
  CHECK(0 == t_live);
}

// Verify the limit of a state, without allocating it, and its growth.
// 1. A value that makes the state 1073741823 bytes or more is the error of
//    PostgreSQL's StringInfo, with the bytes there were and the bytes asked.
// 2. The delimiter is checked first, then the value; the state is unchanged.
// 3. A state grows past its first buffer many times.
TEST_CASE("string_agg's limit and growth") {
  live_t live;
  ty_string_agg_t st = {};
  static const char big[1] = {'x'};

  // 1. A value that makes the state 1073741823 bytes or more is the error of
  //    PostgreSQL's StringInfo, with the bytes there were and the bytes asked.
  REQUIRE(0 == ty_string_agg_add(&st, ",", 1, "ab", 2));
  CHECK(-1 == ty_string_agg_add(&st, ",", 1, big, 1073741819));
  CHECK(TY_ERR_RANGE == ty_errnum());
  CHECK(std::string(ty_errmsg()) ==
        "string buffer exceeds maximum allowed length (1073741823 bytes)");
  CHECK(4 == st.had);
  CHECK(1073741819 == st.need);

  // 2. The delimiter is checked first, then the value; the state is unchanged.
  CHECK(-1 == ty_string_agg_add(&st, big, 1073741820, "x", 1));
  CHECK(3 == st.had);
  CHECK(1073741820 == st.need);
  CHECK(3 == st.len);
  CHECK(finish(&st) == "ab");

  // 3. A state grows past its first buffer many times.
  std::string want = "ab";

  for (int i = 0; i < 2000; i++) {
    std::string v = std::to_string(i);

    REQUIRE(0 == ty_string_agg_add(&st, ",", 1, v.data(), v.size()));
    want += "," + v;
  }
  CHECK(finish(&st) == want);
  drop(&st);
  CHECK(0 == t_live);
}

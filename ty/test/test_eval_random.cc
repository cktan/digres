/*
 * Copyright (c) 2026 CK Tan
 * SPDX-License-Identifier: MIT
 */

/*
 * test_eval_random.cc -- random programs of the evaluation engine
 * (../EVAL_PLAN.md §8, step 5): each written twice, of vectors and of
 * scalars, from one tree. The vector program on a batch must give, in each
 * valid row, what the scalar one gives on that row alone: its value, its
 * null, and its error.
 */
#include "doctest.h"
#include "ty_eval.h"
#include "ty_vec.h"

#include <string.h>

#include <algorithm>
#include <random>
#include <string>
#include <vector>

namespace {

/* The types, by their names in the language. */
enum ty_t { I16, I32, I64, F32, F64, BOOL, NTYPES };
const char *const tname[] = {"i16", "i32", "i64", "f32", "f64", "bool"};
const size_t tsize[] = {2, 4, 8, 4, 8, 1};

/* A program's two texts: of vectors, and its scalar twin. */
struct twin_t {
  std::string vec, sc;
};

/*
 * A random program over the parameters a16, a32, b32, a64, af32, af64, ab,
 * one of each type and two of i32, all vectors in the vector program, and k,
 * a scalar i32 in both.
 */
struct gen_t {
  std::mt19937_64 rng;
  std::vector<std::pair<std::string, ty_t>> names; /* lets in scope */
  int lets = 0;

  explicit gen_t(uint64_t seed) : rng(seed) {}

  uint32_t pick(uint32_t n) { return uint32_t(rng() % n); }

  /* Two forms of op over the parts, a vector's and a scalar's. */
  static twin_t form(const std::string &op, ty_t t,
                     std::initializer_list<twin_t> parts,
                     const std::string &suffix = "") {
    twin_t r{"(" + op + "." + tname[t] + suffix + ".v",
             "(" + op + "." + tname[t] + suffix};
    for (const twin_t &p : parts) {
      r.vec += " " + p.vec;
      r.sc += " " + p.sc;
    }
    r.vec += ")";
    r.sc += ")";
    return r;
  }

  /* A constant of t: small, an edge, or nil. */
  std::string constant(ty_t t) {
    if (0 == pick(12)) {
      return "nil";
    }
    switch (t) {
    case BOOL:
      return pick(2) ? "true" : "false";
    case F32:
    case F64: {
      static const char *const f[] = {"0",    "1",    "-1", "2.5",
                                      "-0.5", "1e30", "3",  "1e-30"};
      return f[pick(8)];
    }
    default: {
      static const char *const edge[] = {"-32768", "32767", "0", "-1"};
      if (0 == pick(6)) {
        return edge[pick(4)];
      }
      return std::to_string(int(pick(21)) - 10);
    }
    }
  }

  /*
   * A leaf of t: a parameter, a let's name, a constant, or of k. typed is
   * whether an operator gives the leaf its type: a constant, which the
   * scalar twin writes bare, needs one.
   */
  twin_t leaf(ty_t t, bool typed) {
    std::vector<std::string> of_t;
    static const char *const param[] = {"a16",  "a32",  "a64",
                                        "af32", "af64", "ab"};
    of_t.push_back(param[t]);
    if (I32 == t) {
      of_t.push_back("b32");
    }
    for (const auto &[n, nt] : names) {
      if (nt == t) {
        of_t.push_back(n);
      }
    }
    const uint32_t c = pick(4);
    if (0 == c && typed) {
      const std::string k = constant(t);
      return {"(splat." + std::string(tname[t]) + " " + k + ")", k};
    }
    if (1 == c && I32 == t) {
      const std::string s = "(add.i32 k " + constant(I32) + ")";
      return {"(splat.i32 " + s + ")", s};
    }
    const std::string n = of_t[pick(uint32_t(of_t.size()))];
    return {n, n};
  }

  /*
   * A random expression of t, of at most depth more levels; typed as leaf
   * takes it.
   */
  twin_t expr(ty_t t, int depth, bool typed = true) {
    if (depth <= 0 || 0 == pick(4)) {
      return leaf(t, typed);
    }
    const uint32_t k = pick(10);
    // if, cond and let, of any type.
    if (0 == k) {
      return form(
          "if", t,
          {expr(BOOL, depth - 1), expr(t, depth - 1), expr(t, depth - 1)});
    }
    if (1 == k) {
      return form("cond", t,
                  {expr(BOOL, depth - 1), expr(t, depth - 1),
                   expr(BOOL, depth - 1), expr(t, depth - 1),
                   expr(t, depth - 1)});
    }
    if (2 == k) {
      const ty_t u = ty_t(pick(NTYPES));
      const std::string n = "v" + std::to_string(lets++);
      const twin_t value = expr(u, depth - 1, false);
      names.push_back({n, u});
      const twin_t body = expr(t, depth - 1, typed);
      names.pop_back();
      return {"(let [" + n + " " + value.vec + "] " + body.vec + ")",
              "(let [" + n + " " + value.sc + "] " + body.sc + ")"};
    }
    if (3 == k) {
      // A cast from another type that may be cast to t.
      ty_t f = ty_t(pick(NTYPES));
      if (f == t || ((BOOL == f || BOOL == t) && I32 != f && I32 != t)) {
        return leaf(t, typed);
      }
      return form("cast", f, {expr(f, depth - 1)}, std::string(".") + tname[t]);
    }
    if (BOOL == t) {
      const uint32_t b = pick(6);
      if (0 == b) {
        static const char *const rel[] = {"eq", "ne", "lt", "le", "gt", "ge"};
        const ty_t f = ty_t(pick(NTYPES));
        return form(std::string("cmp.") + rel[pick(6)], f,
                    {expr(f, depth - 1), expr(f, depth - 1)});
      }
      if (1 == b || 2 == b) {
        return form(1 == b ? "and" : "or", BOOL,
                    {expr(BOOL, depth - 1), expr(BOOL, depth - 1)});
      }
      if (3 == b) {
        return form("not", BOOL, {expr(BOOL, depth - 1)});
      }
      if (4 == b) {
        const ty_t f = ty_t(pick(NTYPES));
        return form(pick(2) ? "isnull" : "isnotnull", f, {expr(f, depth - 1)});
      }
      return form(pick(2) ? "istrue" : "isfalse", BOOL,
                  {expr(BOOL, depth - 1)});
    }
    static const char *const arith[] = {"add", "sub", "mul", "div", "mod"};
    const bool is_int = I16 == t || I32 == t || I64 == t;
    return form(arith[pick(is_int ? 5 : 4)], t,
                {expr(t, depth - 1), expr(t, depth - 1)});
  }
};

/* One row of each parameter, the vector arrays' n rows. */
struct rows_t {
  uint32_t n;
  std::vector<int16_t> a16;
  std::vector<int32_t> a32, b32;
  std::vector<int64_t> a64;
  std::vector<float> af32;
  std::vector<double> af64;
  std::vector<uint8_t> ab;
  std::vector<uint8_t> flag[7]; /* a16, a32, b32, a64, af32, af64, ab */
  bool no_flags[7];             /* given as NULL: no row is null */
  int32_t k;
  uint8_t kflag;
};

/*
 * n rows of small values, edges and nulls. No row is not valid: a row's
 * validity comes from the inputs a program reads, and one of constants
 * alone reads none, so it would compute a row its twin skips. The tests of
 * test_eval_run.cc take rows not valid.
 */
rows_t random_rows(std::mt19937_64 &rng, uint32_t n) {
  rows_t r;
  r.n = n;
  auto small = [&]() { return int64_t(rng() % 21) - 10; };
  for (uint32_t i = 0; i < n; i++) {
    const bool edge = 0 == rng() % 9;
    r.a16.push_back(edge ? (rng() % 2 ? INT16_MIN : INT16_MAX) : small());
    r.a32.push_back(edge ? (rng() % 2 ? INT32_MIN : 0) : small());
    r.b32.push_back(small());
    r.a64.push_back(edge ? INT64_MIN : small());
    r.af32.push_back(edge ? 3e38f : float(small()) / 2);
    r.af64.push_back(edge ? 1e308 : double(small()) / 4);
    r.ab.push_back(rng() % 2);
    for (auto &f : r.flag) {
      f.push_back(0 == rng() % 10 ? TY_VEC_NULL : 0);
    }
  }
  // A third of the parameters have no null, and are given no flags.
  for (int p = 0; p < 7; p++) {
    r.no_flags[p] = 0 == rng() % 3;
    if (r.no_flags[p]) {
      std::fill(r.flag[p].begin(), r.flag[p].end(), 0);
    }
  }
  r.k = int32_t(small());
  r.kflag = 0 == rng() % 8 ? TY_VEC_NULL : 0;
  return r;
}

/* The args of the vector program, of all n rows; or of one row, i. */
std::vector<ty_arg_t> args_of(const rows_t &r, int64_t i) {
  const void *v[] = {r.a16.data(),  r.a32.data(),  r.b32.data(), r.a64.data(),
                     r.af32.data(), r.af64.data(), r.ab.data()};
  const size_t size[] = {2, 4, 4, 8, 4, 8, 1};
  std::vector<ty_arg_t> a;
  for (int p = 0; p < 7; p++) {
    const char *base = static_cast<const char *>(v[p]);
    const uint8_t *f = r.no_flags[p] ? nullptr : r.flag[p].data();
    a.push_back(i < 0 ? ty_arg_t{base, f}
                      : ty_arg_t{base + i * size[p], f ? f + i : nullptr});
  }
  a.push_back({&r.k, &r.kflag});
  return a;
}

/* The parameters of each program: vectors, or scalars. */
const char *const params_vec =
    "[a16:i16.v a32:i32.v b32:i32.v a64:i64.v af32:f32.v af64:f64.v "
    "ab:bool.v k:i32]";
const char *const params_sc =
    "[a16:i16 a32:i32 b32:i32 a64:i64 af32:f32 af64:f64 ab:bool k:i32]";

/* Whether two values of size bytes are the same: two NaNs are. */
bool same_value(ty_t t, const void *a, const void *b) {
  if (F32 == t) {
    float x, y;
    memcpy(&x, a, 4), memcpy(&y, b, 4);
    return (x != x && y != y) || 0 == memcmp(&x, &y, 4);
  }
  if (F64 == t) {
    double x, y;
    memcpy(&x, a, 8), memcpy(&y, b, 8);
    return (x != x && y != y) || 0 == memcmp(&x, &y, 8);
  }
  return 0 == memcmp(a, b, tsize[t]);
}

/*
 * Run one random program and its twin on a batch, and compare. Returns
 * false, with what differs, if they do.
 * 1. Compile both: each must compile.
 * 2. The vector program on n rows.
 * 3. The scalar one on each row alone, its first failing valid row too.
 * 4. Both fail at the same row with the same error, or both succeed and
 *    each valid row is the same: null, or the same value.
 */
bool check_twin(uint64_t seed, std::string &why, bool &failed) {
  gen_t g(seed);
  const ty_t t = ty_t(g.pick(NTYPES));
  const twin_t e = g.expr(t, 4, false);
  const std::string vtext =
      std::string("(fn ") + params_vec + " " + e.vec + ")";
  const std::string stext = std::string("(fn ") + params_sc + " " + e.sc + ")";
  // 1. Compile.
  ty_prog_t *pv = nullptr, *ps = nullptr;
  if (0 != ty_prog_compile(vtext.data(), vtext.size(), &pv) ||
      0 != ty_prog_compile(stext.data(), stext.size(), &ps)) {
    why = std::string("compile: ") + ty_errmsg() + "\n" + vtext;
    ty_prog_free(pv);
    return false;
  }
  std::mt19937_64 rng(seed * 31 + 7);
  const uint32_t n = 1 + uint32_t(rng() % 40);
  const rows_t rows = random_rows(rng, n);
  ty_run_t *rv = nullptr, *rs = nullptr;
  REQUIRE(0 == ty_run_new(pv, 64, &rv));
  REQUIRE(0 == ty_run_new(ps, 1, &rs));
  // 2. The vector program.
  ty_result_t res;
  const std::vector<ty_arg_t> all = args_of(rows, -1);
  const int vrc = ty_run(rv, n, all.data(), &res);
  const ty_errnum_t vwhy = ty_errnum();
  const uint32_t vrow = ty_erridx();
  // 3. The scalar one, row by row.
  bool ok = true;
  int64_t first = -1;
  ty_errnum_t swhy = TY_OK;
  for (uint32_t i = 0; ok && i < n; i++) {
    const std::vector<ty_arg_t> one = args_of(rows, i);
    ty_result_t s;
    if (0 != ty_run(rs, 1, one.data(), &s)) {
      if (first < 0) {
        first = i, swhy = ty_errnum();
      }
      continue;
    }
    // 4. The same row, if the vector program did not fail.
    if (0 == vrc && first < 0) {
      const bool vnull = res.flags[i] & TY_VEC_NULL;
      const bool snull = s.flags[0] & TY_VEC_NULL;
      const char *vv = static_cast<const char *>(res.values) + i * tsize[t];
      if (vnull != snull || (!vnull && !same_value(t, vv, s.values))) {
        why = "row " + std::to_string(i) + " differs";
        ok = false;
      }
    }
  }
  // 4. The same failure.
  if (ok && (0 != vrc) != (first >= 0)) {
    why =
        0 != vrc
            ? "only the vector program failed, at row " + std::to_string(vrow)
            : "only the scalar program failed, at row " + std::to_string(first);
    ok = false;
  }
  if (ok && 0 != vrc && (int64_t(vrow) != first || vwhy != swhy)) {
    why = "they fail at rows " + std::to_string(vrow) + " and " +
          std::to_string(first) + ", with " + ty_errname(vwhy) + " and " +
          ty_errname(swhy);
    ok = false;
  }
  if (!ok) {
    why += "\n" + vtext + "\n" + stext;
  }
  failed = 0 != vrc;
  ty_run_free(rv);
  ty_run_free(rs);
  ty_prog_free(pv);
  ty_prog_free(ps);
  return ok;
}

} // namespace

// Verify that a random program of vectors gives, in each valid row, what its
// scalar twin gives on that row alone.
// 1. For each of 3,000 seeds, a program of every operator, of depth 4 at
//    most, and its twin.
// 2. Rows of small values and edges, and a null in each tenth; a third of
//    the parameters have no null, and are given no flags.
// 3. Some programs fail, and some do not; no program differs.
TEST_CASE("random programs of vectors are their scalar twins, row by row") {
  int failed = 0, differ = 0;
  for (uint64_t seed = 1; seed <= 3000; seed++) {
    std::string why;
    bool f = false;
    const bool same = check_twin(seed, why, f);
    failed += f;
    if (!same) {
      if (differ++ < 5) {
        CAPTURE(seed);
        FAIL_CHECK(why);
      }
    }
  }
  CHECK(0 == differ);
  // Both outcomes are tested.
  CAPTURE(failed);
  CHECK((failed > 300 && failed < 2700));
}

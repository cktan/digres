/*
 * Copyright (c) 2026 CK Tan
 * SPDX-License-Identifier: MIT
 */

/*
 * test_eval_run.cc -- the machine of the evaluation engine (ty_eval.h): a
 * program run on vectors and scalars, its results as tyvec_print writes
 * them, and its errors, which only rows that need them raise.
 */
#include "doctest.h"
#include "ty_eval.h"
#include "ty_print.h"
#include "ty_vec.h"

#include <string.h>

#include <limits>
#include <string>
#include <type_traits>
#include <vector>

namespace {

/* A compiled program and its run, for batches of up to 64 rows. */
struct prog_run_t {
  ty_prog_t *prog = nullptr;
  ty_run_t *run = nullptr;
  explicit prog_run_t(const std::string &text) {
    REQUIRE_MESSAGE(0 == ty_prog_compile(text.data(), text.size(), &prog),
                    ty_errmsg());
    REQUIRE(0 == ty_run_new(prog, 64, &run));
  }
  ~prog_run_t() {
    ty_run_free(run);
    ty_prog_free(prog);
  }
};

/*
 * Run p on n rows of args, and give its result as text: tyvec_print's lines
 * for a vector of T, or "scalar " and ty_print's text, "null", or "ERR" with
 * the error's name, row and message.
 */
template <typename T>
std::string run(prog_run_t &p, uint32_t n, const std::vector<ty_arg_t> &args,
                bool vec = true) {
  ty_result_t r;
  if (0 != ty_run(p.run, n, args.data(), &r)) {
    return std::string("ERR ") + ty_errname(ty_errnum()) + " row " +
           std::to_string(ty_erridx()) + ": " + ty_errmsg();
  }
  std::string s;
  if (vec) {
    tyvec_print(n, r.flags, static_cast<const T *>(r.values), s, {});
  } else if (r.flags[0] & TY_VEC_NULL) {
    s = "scalar null";
  } else {
    s = "scalar ";
    T v;
    memcpy(&v, r.values, sizeof v);
    if constexpr (std::is_same_v<T, uint8_t>) {
      ty_print(0 != v, s);
    } else {
      ty_print(v, s);
    }
  }
  return s;
}

} // namespace

// Verify EVAL_PLAN.md §1's example on a batch: a scalar computed once,
// splat, vector compares, and SQL's AND of nulls.
// 1. Six rows of x and y, with nulls in each, and lim a scalar.
// 2. x > lim + 1 AND y < 10, row by row: false AND null is false.
// 3. With a null lim, every row null but those whose y settles it.
TEST_CASE("a run gives the plan's example row by row") {
  prog_run_t p("(fn [x:i32.v y:i32.v lim:i32]\n"
               "  (and.bool.v (cmp.gt.i32.v x (splat.i32 (add.i32 lim 1)))\n"
               "              (cmp.lt.i32.v y (splat.i32 10))))");
  // 1. The rows.
  const int32_t x[] = {9, 2, 9, 0, 9, 2}, y[] = {1, 1, 20, 0, 0, 0};
  const uint8_t fx[] = {0, 0, 0, TY_VEC_NULL, 0, 0};
  const uint8_t fy[] = {0, 0, 0, 0, TY_VEC_NULL, TY_VEC_NULL};
  int32_t lim = 5;
  // 2. Row by row.
  CHECK(run<uint8_t>(p, 6, {{x, fx}, {y, fy}, {&lim, nullptr}}) ==
        "0 t\n1 f\n2 f\n3 null\n4 null\n5 f\n");
  // 3. A null lim.
  const uint8_t null = TY_VEC_NULL;
  CHECK(run<uint8_t>(p, 6, {{x, fx}, {y, fy}, {&lim, &null}}) ==
        "0 null\n1 null\n2 f\n3 null\n4 null\n5 null\n");
}

// Verify that a row's error is raised only where its row needs it, as
// EVAL_PLAN.md §7 says.
// 1. (b = 0) OR (100 / b > 1): the division fails where b is 0, and the OR,
//    whose left side is true there, drops it.
// 2. (100 / b > 1) AND (b = 0): the left side's error is raised, at the
//    lowest row it fails in.
// 3. A vector if whose other arm divides by 0 there: no error.
// 4. A row not valid, whose division fails: never raised.
// 5. Every row fails: the lowest valid one is raised.
TEST_CASE("a run raises a row's error only where the row needs it") {
  const int32_t b[] = {4, 0, 200, 0, 1};
  // 1. OR drops it.
  prog_run_t p1("(fn [b:i32.v] (or.bool.v (cmp.eq.i32.v b (splat.i32 0))\n"
                "  (cmp.gt.i32.v (div.i32.v (splat.i32 100) b) (splat.i32 "
                "1))))");
  CHECK(run<uint8_t>(p1, 5, {{b, nullptr}}) == "0 t\n1 t\n2 f\n3 t\n4 t\n");
  // 2. AND's left side.
  prog_run_t p2("(fn [b:i32.v] (and.bool.v\n"
                "  (cmp.gt.i32.v (div.i32.v (splat.i32 100) b) (splat.i32 1))"
                "  (cmp.eq.i32.v b (splat.i32 0))))");
  CHECK(run<uint8_t>(p2, 5, {{b, nullptr}}) ==
        "ERR TY_ERR_DIVZERO row 1: division by zero");
  // 3. A vector if.
  prog_run_t p3("(fn [b:i32.v] (if.i32.v (cmp.eq.i32.v b (splat.i32 0))\n"
                "  (splat.i32 -1) (div.i32.v (splat.i32 100) b)))");
  CHECK(run<int32_t>(p3, 5, {{b, nullptr}}) ==
        "0 25\n1 -1\n2 0\n3 -1\n4 100\n");
  // 4. A row not valid.
  prog_run_t p4("(fn [b:i32.v] (div.i32.v (splat.i32 100) b))");
  const uint8_t fb[] = {0, TY_VEC_NOTVALID, 0, TY_VEC_NOTVALID, 0};
  CHECK(run<int32_t>(p4, 5, {{b, fb}}) ==
        "0 25\n1 notvalid\n2 0\n3 notvalid\n4 100\n");
  // 5. Every row.
  const int32_t zero[] = {0, 0, 0};
  const uint8_t fz[] = {TY_VEC_NOTVALID, 0, 0};
  CHECK(run<int32_t>(p4, 3, {{zero, fz}}) ==
        "ERR TY_ERR_DIVZERO row 1: division by zero");
}

// Verify scalar programs: their arms jump, and their errors, nulls and
// three values are SQL's.
// 1. if: 100 / b where b is not 0, and the arm not taken is not computed.
// 2. AND and OR of each pair of true, false and null; with an error on the
//    right, dropped by false AND, raised by true AND.
// 3. An error beats a null; IS NULL of an error raises; IS NULL of a null
//    is true.
// 4. An overflow, raised as row 0.
TEST_CASE("a scalar run jumps, and gives SQL's answers") {
  // 1. if.
  prog_run_t p1("(fn [b:i32] (if.i32 (cmp.eq.i32 b 0) -1 (div.i32 100 b)))");
  int32_t b = 0;
  CHECK(run<int32_t>(p1, 1, {{&b, nullptr}}, false) == "scalar -1");
  b = 7;
  CHECK(run<int32_t>(p1, 1, {{&b, nullptr}}, false) == "scalar 14");
  // 2. AND and OR.
  prog_run_t pa("(fn [p:bool q:bool] (and.bool p q))");
  prog_run_t po("(fn [p:bool q:bool] (or.bool p q))");
  const char *name[] = {"f", "t", "null"};
  const char *sql_and[3][3] = {
      {"f", "f", "f"}, {"f", "t", "null"}, {"f", "null", "null"}};
  const char *sql_or[3][3] = {
      {"f", "t", "null"}, {"t", "t", "t"}, {"null", "t", "null"}};
  for (int i = 0; i < 3; i++) {
    for (int j = 0; j < 3; j++) {
      uint8_t x = i == 1, y = j == 1;
      uint8_t fx = 2 == i ? TY_VEC_NULL : 0, fy = 2 == j ? TY_VEC_NULL : 0;
      CAPTURE(name[i]);
      CAPTURE(name[j]);
      CHECK(run<uint8_t>(pa, 1, {{&x, &fx}, {&y, &fy}}, false) ==
            std::string("scalar ") + sql_and[i][j]);
      CHECK(run<uint8_t>(po, 1, {{&x, &fx}, {&y, &fy}}, false) ==
            std::string("scalar ") + sql_or[i][j]);
    }
  }
  prog_run_t pe("(fn [p:bool b:i32] (and.bool p (cmp.gt.i32 (div.i32 1 b) "
                "0)))");
  uint8_t t = 1, f = 0;
  int32_t z = 0;
  CHECK(run<uint8_t>(pe, 1, {{&f, nullptr}, {&z, nullptr}}, false) ==
        "scalar f");
  CHECK(run<uint8_t>(pe, 1, {{&t, nullptr}, {&z, nullptr}}, false) ==
        "ERR TY_ERR_DIVZERO row 0: division by zero");
  // 3. Errors, nulls, IS NULL.
  prog_run_t pn("(fn [a:i32] (isnull.i32 (add.i32 a (div.i32 1 0))))");
  const uint8_t null = TY_VEC_NULL;
  int32_t a = 1;
  CHECK(run<uint8_t>(pn, 1, {{&a, &null}}, false) ==
        "ERR TY_ERR_DIVZERO row 0: division by zero");
  prog_run_t pi("(fn [a:i32] (isnull.i32 a))");
  CHECK(run<uint8_t>(pi, 1, {{&a, &null}}, false) == "scalar t");
  CHECK(run<uint8_t>(pi, 1, {{&a, nullptr}}, false) == "scalar f");
  // 4. An overflow.
  prog_run_t po2("(fn [a:i16] (mul.i16 a a))");
  int16_t big = 200;
  CHECK(run<int16_t>(po2, 1, {{&big, nullptr}}, false) ==
        "ERR TY_ERR_RANGE row 0: result out of range");
}

// Verify let, casts, cond and the IS tests of vectors, and that a run is
// used again with nothing left of the last one.
// 1. let: a value computed once and used twice.
// 2. Casts of vectors: i32 to i16 fails on a row that does not fit; f64 to
//    i32 rounds; bool to i32.
// 3. A vector cond of three arms, and IS TRUE of a vector with a null.
// 4. A run that fails, then one that does not, of the same run.
// 5. More rows than the run was made for: TY_ERR_INVALID.
TEST_CASE("a run takes let, casts and cond, and is used again") {
  // 1. let.
  prog_run_t p1("(fn [x:i64.v] (let [y (mul.i64.v x x)] (add.i64.v y y)))");
  const int64_t x[] = {3, -4};
  CHECK(run<int64_t>(p1, 2, {{x, nullptr}}) == "0 18\n1 32\n");
  // 2. Casts.
  prog_run_t p2("(fn [x:i32.v] (cast.i32.i16.v x))");
  const int32_t wide[] = {7, 40000, -40000};
  CHECK(run<int16_t>(p2, 3, {{wide, nullptr}}) ==
        "ERR TY_ERR_RANGE row 1: result out of range");
  prog_run_t p3("(fn [d:f64.v] (cast.f64.i32.v d))");
  const double d[] = {2.5, 3.5, -0.4};
  CHECK(run<int32_t>(p3, 3, {{d, nullptr}}) == "0 2\n1 4\n2 0\n");
  prog_run_t p4("(fn [b:bool.v] (cast.bool.i32.v b))");
  const uint8_t bv[] = {1, 0};
  CHECK(run<int32_t>(p4, 2, {{bv, nullptr}}) == "0 1\n1 0\n");
  // 3. cond, IS TRUE.
  prog_run_t p5("(fn [x:i32.v] (cond.i32.v (cmp.lt.i32.v x (splat.i32 0)) "
                "(splat.i32 -1) (cmp.gt.i32.v x (splat.i32 0)) (splat.i32 1) "
                "(splat.i32 0)))");
  const int32_t s[] = {-5, 0, 9, 3};
  const uint8_t fs[] = {0, 0, 0, TY_VEC_NULL};
  CHECK(run<int32_t>(p5, 4, {{s, fs}}) == "0 -1\n1 0\n2 1\n3 0\n");
  prog_run_t p6("(fn [b:bool.v] (istrue.bool.v b))");
  const uint8_t fb[] = {0, TY_VEC_NULL};
  CHECK(run<uint8_t>(p6, 2, {{bv, fb}}) == "0 t\n1 f\n");
  // 4. A run again.
  prog_run_t p7("(fn [b:i32.v] (div.i32.v (splat.i32 10) b))");
  const int32_t z[] = {0, 5}, five[] = {5, 2};
  CHECK(run<int32_t>(p7, 2, {{z, nullptr}}) ==
        "ERR TY_ERR_DIVZERO row 0: division by zero");
  CHECK(run<int32_t>(p7, 2, {{five, nullptr}}) == "0 2\n1 5\n");
  // 5. Too many rows.
  std::vector<int32_t> many(65, 1);
  CHECK(run<int32_t>(p7, 65, {{many.data(), nullptr}})
            .starts_with("ERR TY_ERR_INVALID"));
}

// Verify the paths of errors that the cases above do not take.
// 1. A vector operation whose right side failed: its error is raised.
// 2. A vector if whose test failed in a row: that row's error is raised,
//    though its arms do not fail.
// 3. Both sides of a scalar operation fail: the left side's error, as
//    PostgreSQL computes it first.
// 4. A scalar if whose test failed: its error is raised, not an arm's
//    value.
// 5. splat of a failed scalar: every row has its error.
// 6. A vector cast of a null row, which holds a value out of range: no
//    error.
TEST_CASE("a run raises the error of each path") {
  // 1. The right side.
  prog_run_t p1("(fn [x:i32.v y:i32.v] (add.i32.v x (div.i32.v x y)))");
  const int32_t x[] = {1, 2, 3}, y[] = {1, 0, 1};
  CHECK(run<int32_t>(p1, 3, {{x, nullptr}, {y, nullptr}}) ==
        "ERR TY_ERR_DIVZERO row 1: division by zero");
  // 2. A vector if's test.
  prog_run_t p2("(fn [x:i32.v y:i32.v] (if.i32.v (cmp.gt.i32.v (div.i32.v x "
                "y) (splat.i32 0)) x y))");
  CHECK(run<int32_t>(p2, 3, {{x, nullptr}, {y, nullptr}}) ==
        "ERR TY_ERR_DIVZERO row 1: division by zero");
  // 3. Both sides of a scalar.
  prog_run_t p3("(fn [a:i16] (add.i16 (div.i16 a 0) (mul.i16 300 300)))");
  int16_t a = 1;
  CHECK(run<int16_t>(p3, 1, {{&a, nullptr}}, false) ==
        "ERR TY_ERR_DIVZERO row 0: division by zero");
  prog_run_t p3b("(fn [a:i16] (add.i16 (mul.i16 300 300) (div.i16 a 0)))");
  CHECK(run<int16_t>(p3b, 1, {{&a, nullptr}}, false) ==
        "ERR TY_ERR_RANGE row 0: result out of range");
  // 4. A scalar if's test.
  prog_run_t p4("(fn [b:i32] (if.i32 (cmp.gt.i32 (div.i32 1 b) 0) 1 2))");
  int32_t zero = 0;
  CHECK(run<int32_t>(p4, 1, {{&zero, nullptr}}, false) ==
        "ERR TY_ERR_DIVZERO row 0: division by zero");
  // 5. splat of a failed scalar.
  prog_run_t p5("(fn [b:i32 x:i32.v] (add.i32.v x (splat.i32 (div.i32 1 "
                "b))))");
  CHECK(run<int32_t>(p5, 3, {{&zero, nullptr}, {x, nullptr}}) ==
        "ERR TY_ERR_DIVZERO row 0: division by zero");
  // 6. A cast of a null row.
  prog_run_t p6("(fn [x:i64.v] (cast.i64.i16.v x))");
  const int64_t big[] = {1, 1000000};
  const uint8_t fb[] = {0, TY_VEC_NULL};
  CHECK(run<int16_t>(p6, 2, {{big, fb}}) == "0 1\n1 null\n");
}

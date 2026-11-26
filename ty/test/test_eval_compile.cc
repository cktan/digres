/*
 * Copyright (c) 2026 CK Tan
 * SPDX-License-Identifier: MIT
 */

/*
 * test_eval_compile.cc -- the compiler of the evaluation engine
 * (ty_eval.h): the bytecode of each form, and each error with its line and
 * column.
 */
#include "doctest.h"
#include "eval_prog.h"
#include "ty_eval.h"

#include <string>

namespace {

/* The dump of text's program, or "ERR message". */
std::string dump(const std::string &text) {
  ty_prog_t *p = nullptr;
  if (0 != ty_prog_compile(text.data(), text.size(), &p)) {
    CHECK(nullptr == p);
    return std::string("ERR ") + ty_errmsg();
  }
  std::string s;
  ty_prog_dump(p, s);
  ty_prog_free(p);
  return s;
}

/* The program of text, which compiles. */
ty_prog_t *compile(const std::string &text) {
  ty_prog_t *p = nullptr;
  REQUIRE_MESSAGE(0 == ty_prog_compile(text.data(), text.size(), &p),
                  ty_errmsg());
  return p;
}

} // namespace

// Verify the bytecode of EVAL_PLAN.md §1's example: scalar parts once, splat
// to a vector, the vector compares, and.
// 1. Compile it.
// 2. Its dump, instruction by instruction.
// 3. Its parameters, its result and the stack's depth.
TEST_CASE("the compiler makes the bytecode of the plan's example") {
  // 1. The program.
  const std::string text = "(fn [x:i32.v y:i32.v lim:i32]\n"
                           "  (and.bool.v\n"
                           "    (cmp.gt.i32.v x (splat.i32 (add.i32 lim 1)))\n"
                           "    (cmp.lt.i32.v y (splat.i32 10))))";
  // 2. Its dump.
  CHECK(dump(text) == "  0  load x\n"
                      "  1  load lim\n"
                      "  2  const.i32 1\n"
                      "  3  add.i32\n"
                      "  4  splat.i32\n"
                      "  5  cmp.gt.i32.v\n"
                      "  6  load y\n"
                      "  7  const.i32 10\n"
                      "  8  splat.i32\n"
                      "  9  cmp.lt.i32.v\n"
                      " 10  and.bool.v\n"
                      " 11  ret\n");
  // 3. Its parameters, result and depth.
  ty_prog_t *p = compile(text);
  REQUIRE(3 == p->params.size());
  CHECK((p->params[0].type.vec && !p->params[2].type.vec));
  CHECK(ty_internal::vtype_t::boolean == p->result.type);
  CHECK(p->result.vec);
  CHECK(3 == p->max_depth);
  ty_prog_free(p);
}

// Verify the forms of control: a scalar if, cond, and and or jump; a vector
// one computes each arm and selects.
// 1. A scalar if: the test, br.ifnot to the else arm, or past it with the
//    test's error; the then arm; br past the else arm.
// 2. A vector if: the three arms, then select.
// 3. A scalar cond of two tests: nested ifs. A vector one: each test and
//    arm, then a select a pair, from the last back.
// 4. A scalar and, and or: the left side, br.and or br.or past the right
//    side, the right side, and the combine.
TEST_CASE("the compiler jumps in scalar control, and selects in vector") {
  // 1. A scalar if.
  CHECK(dump("(fn [b:i32] (if.i32 (cmp.eq.i32 b 0) 0 (div.i32 100 b)))") ==
        "  0  load b\n"
        "  1  const.i32 0\n"
        "  2  cmp.eq.i32\n"
        "  3  br.ifnot 6 9\n"
        "  4  const.i32 0\n"
        "  5  br 9\n"
        "  6  const.i32 100\n"
        "  7  load b\n"
        "  8  div.i32\n"
        "  9  ret\n");
  // 2. A vector if.
  CHECK(dump("(fn [c:bool.v x:f64.v y:f64.v] (if.f64.v c x y))") ==
        "  0  load c\n"
        "  1  load x\n"
        "  2  load y\n"
        "  3  select.f64.v\n"
        "  4  ret\n");
  // 3. cond.
  CHECK(dump("(fn [x:i16] (cond.i16 (cmp.lt.i16 x 0) -1 (cmp.gt.i16 x 0) 1 "
             "0))") == "  0  load x\n"
                       "  1  const.i16 0\n"
                       "  2  cmp.lt.i16\n"
                       "  3  br.ifnot 6 13\n"
                       "  4  const.i16 -1\n"
                       "  5  br 13\n"
                       "  6  load x\n"
                       "  7  const.i16 0\n"
                       "  8  cmp.gt.i16\n"
                       "  9  br.ifnot 12 13\n"
                       " 10  const.i16 1\n"
                       " 11  br 13\n"
                       " 12  const.i16 0\n"
                       " 13  ret\n");
  CHECK(dump("(fn [a:bool.v b:bool.v x:i64.v y:i64.v z:i64.v] (cond.i64.v a "
             "x b y z))") == "  0  load a\n"
                             "  1  load x\n"
                             "  2  load b\n"
                             "  3  load y\n"
                             "  4  load z\n"
                             "  5  select.i64.v\n"
                             "  6  select.i64.v\n"
                             "  7  ret\n");
  // A vector cond's selects leave one slot: the stack's depth after is
  // that of a value.
  {
    ty_prog_t *p = compile(
        "(fn [a:bool.v x:i64.v z:i64.v w:i64.v] (add.i64.v (cond.i64.v a x z) "
        "(add.i64.v w (add.i64.v w w))))");
    CHECK(4 == p->max_depth);
    ty_prog_free(p);
  }
  // 4. A scalar and, and or.
  CHECK(dump("(fn [p:bool q:bool] (or.bool (and.bool p q) (not.bool p)))") ==
        "  0  load p\n"
        "  1  br.and 4\n"
        "  2  load q\n"
        "  3  and.bool\n"
        "  4  br.or 8\n"
        "  5  load p\n"
        "  6  not.bool\n"
        "  7  or.bool\n"
        "  8  ret\n");
}

// Verify constants, let, casts and the IS tests.
// 1. Constants of each type: an integer as a float, a real, true, nil, and
//    the edges of an integer type.
// 2. let: a value computed once into a local, then got by its name; a let
//    in a let sees the outer names, and a name shadows a parameter.
// 3. A cast, splat, and isnull of each kind of operand.
TEST_CASE("the compiler takes constants, let, casts and IS tests") {
  // 1. Constants.
  CHECK(dump("(fn [] (add.f32 1 2.5))") == "  0  const.f32 1\n"
                                           "  1  const.f32 2.5\n"
                                           "  2  add.f32\n"
                                           "  3  ret\n");
  CHECK(dump("(fn [] (and.bool true nil))") == "  0  const.bool t\n"
                                               "  1  br.and 4\n"
                                               "  2  const.bool nil\n"
                                               "  3  and.bool\n"
                                               "  4  ret\n");
  CHECK(dump("(fn [] (sub.i16 -32768 32767))") == "  0  const.i16 -32768\n"
                                                  "  1  const.i16 32767\n"
                                                  "  2  sub.i16\n"
                                                  "  3  ret\n");
  CHECK(dump("(fn [] (mul.i64 -9223372036854775808 1e0))")
            .starts_with("ERR 1:38: 1e0 is not i64"));
  // 2. let.
  CHECK(dump("(fn [x:i32.v] (let [y (add.i32.v x x) x (splat.i32 2)]\n"
             "  (mul.i32.v y x)))") == "  0  load x\n"
                                       "  1  load x\n"
                                       "  2  add.i32.v\n"
                                       "  3  local.set y\n"
                                       "  4  const.i32 2\n"
                                       "  5  splat.i32\n"
                                       "  6  local.set x\n"
                                       "  7  local.get y\n"
                                       "  8  local.get x\n"
                                       "  9  mul.i32.v\n"
                                       " 10  ret\n");
  CHECK(dump("(fn [a:f64] (let [b (mul.f64 a a)] (let [c (add.f64 b 1)] "
             "(sub.f64 c b))))") == "  0  load a\n"
                                    "  1  load a\n"
                                    "  2  mul.f64\n"
                                    "  3  local.set b\n"
                                    "  4  local.get b\n"
                                    "  5  const.f64 1\n"
                                    "  6  add.f64\n"
                                    "  7  local.set c\n"
                                    "  8  local.get c\n"
                                    "  9  local.get b\n"
                                    " 10  sub.f64\n"
                                    " 11  ret\n");
  // An inner let's name hides an outer one, after its value, which still
  // sees the outer; it is gone after its let.
  CHECK(dump("(fn [x:i32] (let [a x] (let [a (add.i32 a 1)] a)))") ==
        "  0  load x\n"
        "  1  local.set a\n"
        "  2  local.get a\n"
        "  3  const.i32 1\n"
        "  4  add.i32\n"
        "  5  local.set a\n"
        "  6  local.get a\n"
        "  7  ret\n");
  {
    ty_prog_t *p =
        compile("(fn [x:i32] (let [a x] (let [a (add.i32 a 1)] a)))");
    // The body's a is the second local, not the first.
    CHECK(1 == p->code[6].a);
    ty_prog_free(p);
  }
  CHECK(dump("(fn [x:i32] (add.i32 (let [a x] a) a))") ==
        "ERR 1:36: a is not a name");
  // 3. Casts, splat, IS tests.
  CHECK(dump("(fn [x:i32.v b:bool] (isnull.f64.v (cast.i32.f64.v x)))") ==
        "  0  load x\n"
        "  1  cast.i32.f64.v\n"
        "  2  isnull.f64.v\n"
        "  3  ret\n");
  CHECK(dump("(fn [b:bool] (istrue.bool.v (splat.bool b)))") ==
        "  0  load b\n"
        "  1  splat.bool\n"
        "  2  istrue.bool.v\n"
        "  3  ret\n");
}

// Verify that each program that is wrong fails with the line and column of
// the form, and why.
// 1. The program's shape: no form, two, not a fn, a bad parameter, one twice.
// 2. Names: one not in scope, an operator that is none, an operator of a type
//    it does not take, a cast that is none, splat of a vector.
// 3. Operands: the wrong count, the wrong type, a vector for a scalar, a
//    constant where a vector is wanted, a constant whose type is not known.
// 4. Constants that do not fit their type, or are not of it.
// 5. A bracket list where a value is wanted, a list with no operator, fn
//    inside the program, and a reader's error.
TEST_CASE("the compiler names each error's line and column") {
  // 1. The shape.
  CHECK(dump("") == "ERR 1:1: no program");
  CHECK(dump("(fn [] 1) (fn [] 2)").starts_with("ERR 1:11: a program is one"));
  CHECK(dump("(add.i32 1 2)").starts_with("ERR 1:1: a program is (fn"));
  CHECK(dump("(fn [x] x)").starts_with("ERR 1:6: a parameter is name:type"));
  CHECK(dump("(fn [x:i8] x)").starts_with("ERR 1:6: a parameter is"));
  CHECK(dump("(fn [1x:i32] x)").starts_with("ERR 1:6: a bad number"));
  CHECK(dump("(fn [x:i32 x:f64] x)") == "ERR 1:12: x is a parameter twice");
  CHECK(dump("(fn [x:i32] x y)").starts_with("ERR 1:1: a program is (fn"));
  // 2. Names.
  CHECK(dump("(fn [x:i32] y)") == "ERR 1:13: y is not a name");
  CHECK(dump("(fn [x:i32] (pow.i32 x x))") ==
        "ERR 1:14: pow.i32 is not an operator");
  CHECK(dump("(fn [x:f64] (mod.f64 x x))") ==
        "ERR 1:14: mod.f64 is not an operator");
  CHECK(dump("(fn [x:bool] (add.bool x x))") ==
        "ERR 1:15: add.bool is not an operator");
  CHECK(dump("(fn [x:i32] (not.i32 x))") ==
        "ERR 1:14: not.i32 is not an operator");
  CHECK(dump("(fn [x:i32] (istrue.i32 x))") ==
        "ERR 1:14: istrue.i32 is not an operator");
  CHECK(dump("(fn [x:bool] (cast.bool.f64 x))") ==
        "ERR 1:15: cast.bool.f64 is not a cast");
  CHECK(dump("(fn [x:i32] (cast.i32.i32 x))") ==
        "ERR 1:14: cast.i32.i32 is not a cast");
  CHECK(dump("(fn [x:i32] (splat.i32.v x))")
            .starts_with("ERR 1:14: splat makes a vector of a scalar"));
  // 3. Operands.
  CHECK(dump("(fn [x:i32] (add.i32 x))") ==
        "ERR 1:13: add.i32 takes 2 operands");
  CHECK(dump("(fn [x:i32] (not.bool x x))") ==
        "ERR 1:13: not.bool takes 1 operand");
  CHECK(dump("(fn [x:i32 y:i64] (add.i32 x y))") ==
        "ERR 1:30: this is i64, where i32 is wanted");
  CHECK(dump("(fn [x:i32.v y:i32] (add.i32.v x y))") ==
        "ERR 1:34: this is i32, where i32.v is wanted");
  CHECK(dump("(fn [x:i32.v] (add.i32.v x 1))")
            .starts_with("ERR 1:28: a constant is a scalar: (splat.i32 ...)"));
  CHECK(dump("(fn [] (let [a 1] a))") ==
        "ERR 1:16: the type of 1 is not known here");
  CHECK(dump("(fn [] nil)") == "ERR 1:8: the type of nil is not known here");
  CHECK(dump("(fn [x:i32] (cond.i32 (cmp.eq.i32 x 0) 1))")
            .starts_with("ERR 1:13: cond.i32 takes test value ... else"));
  CHECK(dump("(fn [x:i32] (let [y] y))") ==
        "ERR 1:18: let takes [name value ...]");
  CHECK(dump("(fn [x:i32] (let [1 x] x))") ==
        "ERR 1:19: a let's name is a word");
  // 4. Constants.
  CHECK(dump("(fn [] (add.i16 32768 0))") ==
        "ERR 1:17: 32768 does not fit i16");
  CHECK(dump("(fn [] (add.f32 1e39 0))") == "ERR 1:17: 1e39 does not fit f32");
  CHECK(dump("(fn [] (add.i32 1.5 0))") == "ERR 1:17: 1.5 is not i32");
  CHECK(dump("(fn [] (and.bool 1 true))") == "ERR 1:18: 1 is not bool");
  CHECK(dump("(fn [] (add.i32 true 1))") == "ERR 1:17: true is not i32");
  // 5. The rest.
  CHECK(dump("(fn [] [1])") ==
        "ERR 1:8: a bracket list is only a fn's or a let's");
  CHECK(dump("(fn [] (1 2))") ==
        "ERR 1:8: a list begins with an operator's name");
  CHECK(dump("(fn [] (fn [] 1))") == "ERR 1:8: fn is the program's form alone");
  CHECK(dump("(fn [] (add.i32 1 2)") == "ERR 1:1: ( is not closed");
  CHECK(TY_ERR_INVALID == ty_errnum());
}

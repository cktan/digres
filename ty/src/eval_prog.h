/*
 * Copyright (c) 2026 CK Tan
 * SPDX-License-Identifier: MIT
 */

/*
 * eval_prog.h -- a compiled program of the evaluation engine
 * (../include/ty_eval.h, ../EVAL_PLAN.md §5): its instructions, constants,
 * parameters and locals, for the compiler and the machine.
 */
#ifndef TY_SRC_EVAL_PROG_H
#define TY_SRC_EVAL_PROG_H

#include <stdint.h>

#include <string>
#include <vector>

namespace ty_internal {

/* The types of the language. */
enum class vtype_t : uint8_t { i16, i32, i64, f32, f64, boolean };

/* The name of a type: i16, i32, i64, f32, f64, bool. */
const char *vtype_name(vtype_t t);

/* The bytes of a type's value in a vector. */
inline uint32_t vtype_size(vtype_t t) {
  static const uint8_t size[] = {2, 4, 8, 4, 8, 1};
  return size[int(t)];
}

/* What a slot of the stack holds: a type, and a vector or a scalar. */
struct slot_type_t {
  vtype_t type = vtype_t::i32;
  bool vec = false;
};

/*
 * An instruction's operation. Each takes its operands from the top of the
 * stack, the first deepest, and pushes its result, unless it says not.
 */
enum class op_t : uint8_t {
  load,      /* push parameter a */
  local_set, /* pop into local a */
  local_get, /* push local a */
  konst,     /* push constant a */
  splat,     /* a scalar into a vector of n rows */
  add,
  sub,
  mul,
  div,
  mod,
  eq,
  ne,
  lt,
  le,
  gt,
  ge,
  bnot,
  band, /* of vectors; of scalars, both computed, after br_and */
  bor,  /* of vectors; of scalars, both computed, after br_or */
  isnull,
  isnotnull,
  istrue,
  isfalse,
  cast,   /* to the type in to */
  select, /* of vectors c, x, y: x where c is true, else y */
  br,     /* jump to a */
  /*
   * Pop a scalar condition: if it failed, push a failed value of type, with
   * its error, and jump to b; if it is not true, jump to a; else go on.
   */
  br_ifnot,
  br_and, /* if the scalar on top is false, not null, jump to a; it stays */
  br_or,  /* if the scalar on top is true, jump to a; it stays */
  ret,    /* the value on top is the result */
};

/* One instruction. */
struct instr_t {
  op_t op = op_t::ret;
  vtype_t type = vtype_t::i32; /* of its operands, or of what it pushes */
  vtype_t to = vtype_t::i32;   /* a cast's */
  bool vec = false;            /* on vectors */
  uint16_t a = 0, b = 0;       /* an index, or targets */
};

/* A constant: a value of its type, or nil, a null. */
struct konst_t {
  vtype_t type = vtype_t::i32;
  bool null = false;
  int64_t i = 0; /* an integer's, or a bool's 0 or 1 */
  double d = 0;  /* an f64's */
  float f = 0;   /* an f32's */
};

/* A parameter, or a local, by its name. */
struct name_t {
  std::string name;
  slot_type_t type;
};

} // namespace ty_internal

/* A compiled program (ty_eval.h). */
struct ty_prog_t {
  std::vector<ty_internal::name_t> params;
  std::vector<ty_internal::name_t> locals;
  std::vector<ty_internal::konst_t> consts;
  std::vector<ty_internal::instr_t> code;
  ty_internal::slot_type_t result;
  uint32_t max_depth = 0; /* the most slots the stack holds */
};

#endif /* TY_SRC_EVAL_PROG_H */

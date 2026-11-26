/*
 * Copyright (c) 2026 CK Tan
 * SPDX-License-Identifier: MIT
 */

/*
 * eval_compile.cc -- the compiler of the evaluation engine (ty_eval.h,
 * ../EVAL_PLAN.md §4 and §5): forms into the bytecode of a stack machine,
 * every operand's type checked against its operator's name. Written for ty.
 */
#include "ty_eval.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include <new>
#include <string_view>

#include "error.h"
#include "eval_form.h"
#include "eval_prog.h"
#include "ty_bool.h"
#include "ty_float.h"
#include "ty_int.h"
#include "ty_print.h"

namespace ty_internal {

/* The name of a type; see eval_prog.h. */
const char *vtype_name(vtype_t t) {
  static const char *const name[] = {"i16", "i32", "i64", "f32", "f64", "bool"};
  return name[int(t)];
}

namespace {

/* The most instructions, constants, locals or parameters: 16-bit indexes. */
constexpr size_t max_index = 65535;

/* The most slots the stack may hold. */
constexpr uint32_t max_depth = 4096;

/* Thrown when compiling fails, once the thread's error is set. */
struct compile_error_t {};

/* A type by its name, into t: false if it names none. */
bool type_of(std::string_view name, vtype_t &t) {
  for (int i = 0; i <= int(vtype_t::boolean); i++) {
    if (name == vtype_name(vtype_t(i))) {
      t = vtype_t(i);
      return true;
    }
  }
  return false;
}

/* Whether t is an integer type. */
bool is_int(vtype_t t) {
  return vtype_t::i16 == t || vtype_t::i32 == t || vtype_t::i64 == t;
}

/* Whether t is a float type. */
bool is_float(vtype_t t) { return vtype_t::f32 == t || vtype_t::f64 == t; }

/* A slot type as text: i32, or i32.v. */
std::string slot_text(slot_type_t s) {
  return std::string(vtype_name(s.type)) + (s.vec ? ".v" : "");
}

/*
 * An operator's name, read: op.type[.v], cmp.rel.type[.v],
 * cast.from.to[.v]. base is op, or cmp.rel.
 */
struct op_name_t {
  std::string base;
  vtype_t type = vtype_t::i32;
  vtype_t to = vtype_t::i32;
  bool vec = false;
};

/* Read an operator's name; false if it is none. */
bool read_op_name(const std::string &text, op_name_t &n) {
  std::vector<std::string> part;
  size_t from = 0;
  for (;;) {
    const size_t dot = text.find('.', from);
    part.push_back(text.substr(from, dot - from));
    if (std::string::npos == dot) {
      break;
    }
    from = dot + 1;
  }
  if (part.size() > 1 && "v" == part.back()) {
    n.vec = true;
    part.pop_back();
  }
  if (3 == part.size() && "cmp" == part[0]) {
    n.base = "cmp." + part[1];
    return type_of(part[2], n.type);
  }
  if (3 == part.size() && "cast" == part[0]) {
    n.base = "cast";
    return type_of(part[1], n.type) && type_of(part[2], n.to);
  }
  if (2 == part.size()) {
    n.base = part[0];
    return type_of(part[1], n.type);
  }
  return false;
}

/* The operation of a binary operator's base, or false. */
bool binary_op(const std::string &base, op_t &op) {
  static const struct {
    const char *name;
    op_t op;
  } ops[] = {{"add", op_t::add},   {"sub", op_t::sub},   {"mul", op_t::mul},
             {"div", op_t::div},   {"mod", op_t::mod},   {"cmp.eq", op_t::eq},
             {"cmp.ne", op_t::ne}, {"cmp.lt", op_t::lt}, {"cmp.le", op_t::le},
             {"cmp.gt", op_t::gt}, {"cmp.ge", op_t::ge}};
  for (const auto &o : ops) {
    if (base == o.name) {
      op = o.op;
      return true;
    }
  }
  return false;
}

/* Whether a cast from f to t is one the language has. */
bool cast_ok(vtype_t f, vtype_t t) {
  if (f == t) {
    return false;
  }
  if (vtype_t::boolean == f || vtype_t::boolean == t) {
    return vtype_t::i32 == f || vtype_t::i32 == t;
  }
  return true;
}

/* The compiler's state: the program, the stack's depth, the names in
   scope. */
struct compiler_t {
  ty_prog_t *prog;
  uint32_t depth = 0;
  std::vector<std::pair<std::string, uint16_t>> scope; /* local by name */

  /* Fail at form f, for why. */
  [[noreturn]] void fail(const form_t &f, const std::string &why) {
    char head[32];
    snprintf(head, sizeof head, "%u:%u: ", f.line, f.col);
    set_error(TY_ERR_INVALID, (head + why).c_str());
    throw compile_error_t{};
  }

  /* Emit an instruction, at form f for its error. */
  void emit(const form_t &f, instr_t in) {
    if (prog->code.size() >= max_index) {
      fail(f, "the program is too long");
    }
    prog->code.push_back(in);
  }

  /* The stack gets one more slot, at form f; or n fewer. */
  void push(const form_t &f) {
    if (++depth > max_depth) {
      fail(f, "the program is too deep");
    }
    prog->max_depth = depth > prog->max_depth ? depth : prog->max_depth;
  }
  void pop(uint32_t n = 1) { depth -= n; }

  /* Set jump target a, or b, of instruction at, to the next instruction. */
  void patch_a(size_t at) { prog->code[at].a = uint16_t(prog->code.size()); }
  void patch_b(size_t at) { prog->code[at].b = uint16_t(prog->code.size()); }

  /* Fail if f is not a list of n items, its operator first. */
  void arity(const form_t &f, size_t n) {
    if (f.items.size() != n + 1) {
      fail(f, f.items[0].text + " takes " + std::to_string(n) +
                  (1 == n ? " operand" : " operands"));
    }
  }

  /*
   * Compile a constant of the type want: its text read by that type.
   * 1. nil is a null of any type.
   * 2. true and false, of bool.
   * 3. An integer, of an integer type or a float; a real, of a float.
   */
  slot_type_t constant(const form_t &f, const slot_type_t *want) {
    if (nullptr == want) {
      fail(f, "the type of " + (f.text.empty() ? std::string("nil") : f.text) +
                  " is not known here");
    }
    if (want->vec) {
      fail(f, "a constant is a scalar: (splat." +
                  std::string(vtype_name(want->type)) + " ...) makes a vector");
    }
    konst_t k;
    k.type = want->type;
    const char *t = f.text.c_str();
    // 1. nil.
    if (form_kind_t::nil == f.kind) {
      k.null = true;
    } else if (form_kind_t::boolean == f.kind) {
      // 2. A bool.
      if (vtype_t::boolean != k.type) {
        fail(f, f.text + " is not " + vtype_name(k.type));
      }
      k.i = f.value ? 1 : 0;
    } else if (vtype_t::boolean == k.type ||
               (form_kind_t::real == f.kind && !is_float(k.type))) {
      fail(f, f.text + " is not " + vtype_name(k.type));
    } else {
      // 3. A number.
      int rc = -1;
      switch (k.type) {
      case vtype_t::i16: {
        int16_t v;
        rc = ty_int16_parse(t, f.text.size(), &v);
        k.i = v;
        break;
      }
      case vtype_t::i32: {
        int32_t v;
        rc = ty_int32_parse(t, f.text.size(), &v);
        k.i = v;
        break;
      }
      case vtype_t::i64:
        rc = ty_int64_parse(t, f.text.size(), &k.i);
        break;
      case vtype_t::f32:
        rc = ty_fp32_parse(t, &k.f, nullptr, nullptr);
        break;
      case vtype_t::f64:
        rc = ty_fp64_parse(t, &k.d, nullptr, nullptr);
        break;
      case vtype_t::boolean:
        break;
      }
      if (0 != rc) {
        fail(f, f.text + " does not fit " + vtype_name(k.type));
      }
    }
    if (prog->consts.size() >= max_index) {
      fail(f, "the program has too many constants");
    }
    prog->consts.push_back(k);
    instr_t in;
    in.op = op_t::konst;
    in.type = k.type;
    in.a = uint16_t(prog->consts.size() - 1);
    emit(f, in);
    push(f);
    return *want;
  }

  /* Compile a name: a local in scope, innermost first, or a parameter. */
  slot_type_t name(const form_t &f) {
    instr_t in;
    for (size_t i = scope.size(); i-- > 0;) {
      if (scope[i].first == f.text) {
        in.op = op_t::local_get;
        in.a = scope[i].second;
        const slot_type_t s = prog->locals[in.a].type;
        in.type = s.type;
        in.vec = s.vec;
        emit(f, in);
        push(f);
        return s;
      }
    }
    for (size_t i = 0; i < prog->params.size(); i++) {
      if (prog->params[i].name == f.text) {
        in.op = op_t::load;
        in.a = uint16_t(i);
        in.type = prog->params[i].type.type;
        in.vec = prog->params[i].type.vec;
        emit(f, in);
        push(f);
        return prog->params[i].type;
      }
    }
    fail(f, f.text + " is not a name");
  }

  /*
   * Compile (let [n e ...] body): each e into a local, named n for what
   * follows it, then body; the names leave scope after it.
   */
  slot_type_t let(const form_t &f, const slot_type_t *want) {
    arity(f, 2);
    const form_t &binds = f.items[1];
    if (form_kind_t::bracket != binds.kind || binds.items.size() % 2) {
      fail(binds, "let takes [name value ...]");
    }
    const size_t was = scope.size();
    for (size_t i = 0; i < binds.items.size(); i += 2) {
      const form_t &n = binds.items[i];
      if (form_kind_t::symbol != n.kind || !is_identifier(n.text)) {
        fail(n, "a let's name is a word");
      }
      const slot_type_t s = expr(binds.items[i + 1], nullptr);
      if (prog->locals.size() >= max_index) {
        fail(n, "the program has too many locals");
      }
      prog->locals.push_back({n.text, s});
      instr_t in;
      in.op = op_t::local_set;
      in.a = uint16_t(prog->locals.size() - 1);
      in.type = s.type;
      in.vec = s.vec;
      emit(n, in);
      pop();
      scope.push_back({n.text, in.a});
    }
    const slot_type_t r = expr(f.items[2], want);
    scope.resize(was);
    return r;
  }

  /*
   * Compile a scalar (if.t c x y): c; br_ifnot to y, or past y with c's
   * error; x; br past y; y.
   */
  slot_type_t if_scalar(const form_t &f, vtype_t t) {
    const slot_type_t b = {vtype_t::boolean, false}, s = {t, false};
    expr(f.items[1], &b);
    instr_t in;
    in.op = op_t::br_ifnot;
    in.type = t;
    const size_t test = prog->code.size();
    emit(f, in);
    pop();
    expr(f.items[2], &s);
    pop();
    instr_t jump;
    jump.op = op_t::br;
    const size_t over = prog->code.size();
    emit(f, jump);
    patch_a(test);
    expr(f.items[3], &s);
    patch_a(over);
    patch_b(test);
    return s;
  }

  /*
   * Compile a cond of items from at: (cond.t c1 x1 c2 x2 ... y), as if of
   * nested ifs. A vector cond computes every arm, and selects from the last
   * pair back; a scalar one jumps.
   */
  slot_type_t cond(const form_t &f, size_t at, slot_type_t s) {
    if (at + 1 == f.items.size()) {
      return expr(f.items[at], &s);
    }
    const slot_type_t b = {vtype_t::boolean, s.vec};
    if (s.vec) {
      expr(f.items[at], &b);
      expr(f.items[at + 1], &s);
      cond(f, at + 2, s);
      instr_t in;
      in.op = op_t::select;
      in.type = s.type;
      in.vec = true;
      emit(f, in);
      pop(2);
      return s;
    }
    expr(f.items[at], &b);
    instr_t in;
    in.op = op_t::br_ifnot;
    in.type = s.type;
    const size_t test = prog->code.size();
    emit(f, in);
    pop();
    expr(f.items[at + 1], &s);
    pop();
    instr_t jump;
    jump.op = op_t::br;
    const size_t over = prog->code.size();
    emit(f, jump);
    patch_a(test);
    cond(f, at + 2, s);
    patch_a(over);
    patch_b(test);
    return s;
  }

  /*
   * Compile an operator's list, (name operand ...), its name read and its
   * operands each compiled with the type it takes.
   */
  slot_type_t list(const form_t &f, const slot_type_t *want) {
    if (f.items.empty() || form_kind_t::symbol != f.items[0].kind) {
      fail(f, "a list begins with an operator's name");
    }
    const std::string &head = f.items[0].text;
    if ("let" == head) {
      return let(f, want);
    }
    if ("fn" == head) {
      fail(f, "fn is the program's form alone");
    }
    op_name_t n;
    if (!read_op_name(head, n)) {
      fail(f.items[0], head + " is not an operator");
    }
    const vtype_t t = n.type;
    const slot_type_t s = {t, n.vec}, b = {vtype_t::boolean, n.vec};
    instr_t in;
    in.type = t;
    in.vec = n.vec;
    op_t op;
    // Add to compare: two operands of t.
    if (binary_op(n.base, op)) {
      const bool arith = !n.base.starts_with("cmp.");
      if ((arith && vtype_t::boolean == t) || (op_t::mod == op && !is_int(t))) {
        fail(f.items[0], head + " is not an operator");
      }
      arity(f, 2);
      expr(f.items[1], &s);
      expr(f.items[2], &s);
      in.op = op;
      emit(f, in);
      pop();
      return arith ? s : b;
    }
    if ("not" == n.base || "and" == n.base || "or" == n.base) {
      if (vtype_t::boolean != t) {
        fail(f.items[0], head + " is not an operator");
      }
      if ("not" == n.base) {
        arity(f, 1);
        expr(f.items[1], &b);
        in.op = op_t::bnot;
        emit(f, in);
        return b;
      }
      arity(f, 2);
      in.op = "and" == n.base ? op_t::band : op_t::bor;
      expr(f.items[1], &b);
      if (n.vec) {
        expr(f.items[2], &b);
        emit(f, in);
        pop();
        return b;
      }
      // A scalar's left side may settle it: jump past the right side.
      instr_t skip;
      skip.op = op_t::band == in.op ? op_t::br_and : op_t::br_or;
      skip.type = t;
      const size_t at = prog->code.size();
      emit(f, skip);
      expr(f.items[2], &b);
      emit(f, in);
      pop();
      patch_a(at);
      return b;
    }
    if ("isnull" == n.base || "isnotnull" == n.base || "istrue" == n.base ||
        "isfalse" == n.base) {
      const bool of_bool = "istrue" == n.base || "isfalse" == n.base;
      if (of_bool && vtype_t::boolean != t) {
        fail(f.items[0], head + " is not an operator");
      }
      arity(f, 1);
      expr(f.items[1], &s);
      in.op = "isnull" == n.base      ? op_t::isnull
              : "isnotnull" == n.base ? op_t::isnotnull
              : "istrue" == n.base    ? op_t::istrue
                                      : op_t::isfalse;
      emit(f, in);
      return b;
    }
    if ("cast" == n.base) {
      if (!cast_ok(t, n.to)) {
        fail(f.items[0], head + " is not a cast");
      }
      arity(f, 1);
      expr(f.items[1], &s);
      in.op = op_t::cast;
      in.to = n.to;
      emit(f, in);
      return {n.to, n.vec};
    }
    if ("splat" == n.base) {
      if (n.vec) {
        fail(f.items[0], "splat makes a vector of a scalar: splat." +
                             std::string(vtype_name(t)));
      }
      arity(f, 1);
      const slot_type_t one = {t, false};
      expr(f.items[1], &one);
      in.op = op_t::splat;
      in.vec = true;
      emit(f, in);
      return {t, true};
    }
    if ("if" == n.base) {
      arity(f, 3);
      if (!n.vec) {
        return if_scalar(f, t);
      }
      expr(f.items[1], &b);
      expr(f.items[2], &s);
      expr(f.items[3], &s);
      in.op = op_t::select;
      emit(f, in);
      pop(2);
      return s;
    }
    if ("cond" == n.base) {
      if (f.items.size() < 4 || 0 != f.items.size() % 2) {
        fail(f, head + " takes test value ... else");
      }
      return cond(f, 1, s);
    }
    fail(f.items[0], head + " is not an operator");
  }

  /*
   * Compile one form, which pushes one slot: of the type want, if want is
   * not nullptr.
   */
  slot_type_t expr(const form_t &f, const slot_type_t *want) {
    slot_type_t got;
    switch (f.kind) {
    case form_kind_t::symbol:
      got = name(f);
      break;
    case form_kind_t::integer:
    case form_kind_t::real:
    case form_kind_t::boolean:
    case form_kind_t::nil:
      return constant(f, want);
    case form_kind_t::list:
      got = list(f, want);
      break;
    case form_kind_t::bracket:
    default:
      fail(f, "a bracket list is only a fn's or a let's");
    }
    if (nullptr != want && (got.type != want->type || got.vec != want->vec)) {
      fail(f, "this is " + slot_text(got) + ", where " + slot_text(*want) +
                  " is wanted");
    }
    return got;
  }

  /* Whether s is a name a parameter or a local may have. */
  static bool is_identifier(const std::string &s) {
    if (s.empty() || !(isalpha((unsigned char)s[0]) || '_' == s[0])) {
      return false;
    }
    for (char c : s) {
      if (!(isalnum((unsigned char)c) || '_' == c)) {
        return false;
      }
    }
    return true;
  }

  /*
   * Compile the program, (fn [p:t ...] body).
   * 1. One fn form, of a bracket list and a body.
   * 2. Each parameter: a name, a colon, a type and .v for a vector; no two
   *    of one name.
   * 3. The body, then ret.
   */
  void program(const std::vector<form_t> &forms, std::string_view text) {
    // 1. One fn form.
    if (1 != forms.size()) {
      form_t at;
      at.line = 1, at.col = 1;
      if (forms.size() > 1) {
        at = forms[1];
      }
      fail(at, text.empty() || forms.empty() ? "no program"
                                             : "a program is one fn form");
    }
    const form_t &fn = forms[0];
    if (form_kind_t::list != fn.kind || fn.items.empty() ||
        form_kind_t::symbol != fn.items[0].kind || "fn" != fn.items[0].text) {
      fail(fn, "a program is (fn [parameter ...] body)");
    }
    if (3 != fn.items.size() || form_kind_t::bracket != fn.items[1].kind) {
      fail(fn, "a program is (fn [parameter ...] body)");
    }
    // 2. The parameters.
    for (const form_t &p : fn.items[1].items) {
      const size_t colon = p.text.find(':');
      slot_type_t s;
      std::string type =
          std::string::npos == colon ? "" : p.text.substr(colon + 1);
      if (type.size() > 2 && type.ends_with(".v")) {
        s.vec = true;
        type.resize(type.size() - 2);
      }
      if (form_kind_t::symbol != p.kind || std::string::npos == colon ||
          !is_identifier(p.text.substr(0, colon)) || !type_of(type, s.type)) {
        fail(p, "a parameter is name:type, or name:type.v for a vector");
      }
      const std::string name = p.text.substr(0, colon);
      for (const name_t &q : prog->params) {
        if (q.name == name) {
          fail(p, name + " is a parameter twice");
        }
      }
      if (prog->params.size() >= max_index) {
        fail(p, "the program has too many parameters");
      }
      prog->params.push_back({name, s});
    }
    // 3. The body.
    prog->result = expr(fn.items[2], nullptr);
    instr_t ret;
    ret.op = op_t::ret;
    ret.type = prog->result.type;
    ret.vec = prog->result.vec;
    emit(fn, ret);
  }
};

/* The name of an instruction's operation and types, as the text has it. */
std::string instr_name(const ty_prog_t *p, const instr_t &in) {
  static const char *const base[] = {
      "load",   "local.set", "local.get", "const",  "splat",   "add",
      "sub",    "mul",       "div",       "mod",    "cmp.eq",  "cmp.ne",
      "cmp.lt", "cmp.le",    "cmp.gt",    "cmp.ge", "not",     "and",
      "or",     "isnull",    "isnotnull", "istrue", "isfalse", "cast",
      "select", "br",        "br.ifnot",  "br.and", "br.or",   "ret"};
  std::string s = base[int(in.op)];
  switch (in.op) {
  case op_t::load:
    return s + " " + p->params[in.a].name;
  case op_t::local_set:
  case op_t::local_get:
    return s + " " + p->locals[in.a].name;
  case op_t::br:
  case op_t::br_and:
  case op_t::br_or:
    return s + " " + std::to_string(in.a);
  case op_t::br_ifnot:
    return s + " " + std::to_string(in.a) + " " + std::to_string(in.b);
  case op_t::ret:
    return s;
  case op_t::konst: {
    const konst_t &k = p->consts[in.a];
    s = s + "." + vtype_name(k.type) + " ";
    if (k.null) {
      return s + "nil";
    }
    switch (k.type) {
    case vtype_t::i16:
      ty_print(int16_t(k.i), s);
      break;
    case vtype_t::i32:
      ty_print(int32_t(k.i), s);
      break;
    case vtype_t::i64:
      ty_print(int64_t(k.i), s);
      break;
    case vtype_t::f32:
      ty_print(k.f, s);
      break;
    case vtype_t::f64:
      ty_print(k.d, s);
      break;
    case vtype_t::boolean:
      ty_print(0 != k.i, s);
      break;
    }
    return s;
  }
  case op_t::cast:
    return s + "." + vtype_name(in.type) + "." + vtype_name(in.to) +
           (in.vec ? ".v" : "");
  case op_t::splat:
    return s + "." + vtype_name(in.type);
  default:
    return s + "." + vtype_name(in.type) + (in.vec ? ".v" : "");
  }
}

} // namespace
} // namespace ty_internal

using namespace ty_internal;

/*
 * Compile a program; see ty_eval.h.
 * 1. Read the text into forms.
 * 2. Compile them; a failure has set the thread's error.
 * 3. Running out of memory is TY_ERR_NOMEM: no exception leaves ty.
 */
int ty_prog_compile(const char *text, size_t len, ty_prog_t **out) {
  *out = nullptr;
  ty_prog_t *prog = nullptr;
  try {
    // 1. The forms.
    std::vector<form_t> forms;
    if (0 != read_forms(std::string_view(text, len), forms)) {
      return -1;
    }
    // 2. The program.
    prog = new ty_prog_t;
    compiler_t c;
    c.prog = prog;
    c.program(forms, std::string_view(text, len));
    *out = prog;
    return 0;
  } catch (const compile_error_t &) {
    delete prog;
    return -1;
  } catch (const std::bad_alloc &) {
    // 3. No memory.
    delete prog;
    return set_error(TY_ERR_NOMEM, "out of memory");
  }
}

/* Free a program; see ty_eval.h. */
void ty_prog_free(ty_prog_t *prog) { delete prog; }

/* The bytecode, an instruction a line; see ty_eval.h. */
void ty_prog_dump(const ty_prog_t *prog, std::string &out) {
  for (size_t i = 0; i < prog->code.size(); i++) {
    char at[32];
    snprintf(at, sizeof at, "%3zu  ", i);
    out += at;
    out += instr_name(prog, prog->code[i]);
    out += '\n';
  }
}
